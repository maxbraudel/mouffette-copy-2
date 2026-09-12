#include "ClientListEventHandler.h"
#include "MainWindow.h"
#include "frontend/managers/ui/RemoteClientState.h"
#include "backend/network/WebSocketClient.h"
#include "backend/network/RemoteSessionCoordinator.h"
#include "backend/domain/models/ClientInfo.h"
#include "shared/rendering/ICanvasHost.h"
#include "frontend/rendering/navigation/ScreenNavigationManager.h"
#include "backend/network/UploadManager.h"
#include "frontend/ui/pages/ClientListPage.h"
#include <QDebug>

ClientListEventHandler::ClientListEventHandler(MainWindow* mainWindow, WebSocketClient* webSocketClient, QObject* parent)
    : QObject(parent)
    , m_mainWindow(mainWindow)
    , m_webSocketClient(webSocketClient)
{
}

void ClientListEventHandler::setupConnections(WebSocketClient* client)
{
    if (!client) return;
    
    connect(client, &WebSocketClient::clientListReceived, this, &ClientListEventHandler::onClientListReceived);
}

void ClientListEventHandler::onClientListReceived(const QList<ClientInfo>& clients)
{
    qDebug() << "Received client list with" << clients.size() << "clients";
    
    // Update remote scene target ID if the target machine reconnected with a new ID
    ICanvasHost* screenCanvas = m_mainWindow->getScreenCanvas();
    if (screenCanvas) {
        screenCanvas->updateRemoteSceneTargetFromClientList(clients);
    }
    
    QList<ClientInfo> displayList = m_mainWindow->buildDisplayClientList(clients);

    int previousConnectedCount = m_mainWindow->getLastConnectedClientCount();
    m_mainWindow->setLastConnectedClientCount(clients.size());

    // Phase 1.1: Update ClientListPage
    ClientListPage* clientListPage = m_mainWindow->getClientListPage();
    if (clientListPage) {
        clientListPage->updateClientList(displayList);
    }

    QString activeSessionIdentity = m_mainWindow->getActiveSessionIdentity();
    if (!activeSessionIdentity.isEmpty()) {
        MainWindow::CanvasSession* activeSession = m_mainWindow->findCanvasSession(activeSessionIdentity);
        if (activeSession) {
            m_mainWindow->setSelectedClient(activeSession->lastClientInfo);
            if (activeSession->canvas && !activeSession->serverAssignedId.isEmpty()) {
                activeSession->canvas->setRemoteSceneTarget(activeSession->serverAssignedId, activeSession->lastClientInfo.getMachineName());
            }
        }
    }

    // Show notification if new connected clients appeared
    if (clients.size() > previousConnectedCount && previousConnectedCount >= 0) {
        int newClients = clients.size() - previousConnectedCount;
        if (newClients > 0) {
            QString message = QString("%1 new client%2 available for sharing")
                .arg(newClients)
                .arg(newClients == 1 ? "" : "s");
            qDebug() << "New clients available:" << message;
        }
    }

    // Reconcile exclusively by the authenticated installation identity. A
    // machine name is presentation data and must never reconnect one physical
    // device's project to another device which happens to share that name.
    ScreenNavigationManager* navigationManager = m_mainWindow->getNavigationManager();
    if (navigationManager && navigationManager->isOnScreenView() && !activeSessionIdentity.isEmpty()) {
        MainWindow::CanvasSession* activeSession = m_mainWindow->findCanvasSession(activeSessionIdentity);
        if (activeSession) {
            const ClientInfo* matchingDevice = nullptr;
            // Use the project-enriched list here. Discovery snapshots can be
            // identity-only for one refresh; the durable project supplies the
            // authenticated presentation fields without fabricating presence.
            for (const ClientInfo& candidate : displayList) {
                if (candidate.endpointId() == activeSessionIdentity
                    && candidate.isOnline()) {
                    matchingDevice = &candidate;
                    break;
                }
            }

            if (matchingDevice) {
                m_mainWindow->getSessionManager()->updateSessionServerId(
                    activeSessionIdentity, matchingDevice->getId());
                activeSession = m_mainWindow->findCanvasSession(activeSessionIdentity);
                if (!activeSession) {
                    return;
                }
                activeSession->lastClientInfo = *matchingDevice;
                activeSession->lastClientInfo.setEndpointId(activeSessionIdentity);
                activeSession->lastClientInfo.setOnline(true);
                activeSession->remoteContentClearedOnDisconnect = false;
                m_mainWindow->setSelectedClient(activeSession->lastClientInfo);
                if (activeSession->canvas && !activeSession->serverAssignedId.isEmpty()) {
                    activeSession->canvas->setRemoteSceneTarget(
                        activeSessionIdentity,
                        activeSession->lastClientInfo.getMachineName());
                    // Replace only the remote topology. ScreenCanvas rebuilds
                    // its screen backdrops without remapping media, whose
                    // project coordinates remain absolute.
                    activeSession->canvas->setScreens(
                        activeSession->lastClientInfo.getScreens());
                }
                navigationManager->refreshActiveClientPreservingCanvas(activeSession->lastClientInfo);
                m_mainWindow->updateClientNameDisplay(activeSession->lastClientInfo);
                const bool isActiveSelection = (activeSession->persistentClientId == activeSessionIdentity);
                if (isActiveSelection) {
                    QString activeRemoteClientId = m_mainWindow->getActiveRemoteClientId();
                    if (activeRemoteClientId != activeSessionIdentity) {
                        m_mainWindow->setActiveRemoteClientId(activeSessionIdentity);
                    }
                }
            } else {
                // Discovery loss does not delete the local project/canvas and
                // does not itself purge uploads. The RemoteSession lease owns
                // that terminal decision after its exact three-second limit.
                RemoteSessionCoordinator* coordinator = m_webSocketClient
                    ? m_webSocketClient->remoteSessionCoordinator() : nullptr;
                const RemoteSessionCoordinator::Binding binding = coordinator
                    ? coordinator->outgoingForPeer(activeSessionIdentity)
                    : RemoteSessionCoordinator::Binding();
                const bool retainedActive = !binding.remoteSessionId.isEmpty()
                    && binding.phase == QLatin1String("Active");
                const bool retainedGrace = !binding.remoteSessionId.isEmpty()
                    && binding.phase == QLatin1String("Grace");
                if (retainedActive || retainedGrace) {
                    // Presence lists are advisory while a server-authenticated
                    // RemoteSession binding exists. A missing discovery entry
                    // must therefore be a no-op: lease events alone own the
                    // Connected/Reconnecting state and command gating.
                    return;
                }
                activeSession->lastClientInfo.setOnline(false);
                activeSession->lastClientInfo.setFromMemory(true);
                m_mainWindow->setSelectedClient(activeSession->lastClientInfo);
                m_mainWindow->setPreserveViewportOnReconnect(true);
                m_mainWindow->refreshOverlayActionsState(false, /*propagateLoss*/ false);
                if (m_mainWindow->getUploadManager()) {
                    m_mainWindow->getUploadManager()->setTargetClientId(QString());
                }
                RemoteClientState state = RemoteClientState::disconnected();
                state.clientInfo = activeSession->lastClientInfo;
                state.volumeVisible = activeSession->lastClientInfo.getVolumePercent() >= 0;
                state.volumePercent = activeSession->lastClientInfo.getVolumePercent();
                m_mainWindow->setRemoteClientState(state, /*propagateLoss*/ false);
            }
        }
    }
}
