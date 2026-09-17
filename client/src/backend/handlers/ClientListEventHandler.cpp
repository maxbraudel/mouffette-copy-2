#include "ClientListEventHandler.h"
#include "backend/runtime/ApplicationRuntime.h"
#include "frontend/managers/ui/RemoteClientState.h"
#include "backend/network/WebSocketClient.h"
#include "backend/network/RemoteSessionCoordinator.h"
#include "backend/domain/project/ProjectManager.h"
#include "backend/domain/models/ClientInfo.h"
#include "shared/rendering/ICanvasHost.h"
#include "frontend/rendering/navigation/ScreenNavigationManager.h"
#include "backend/network/UploadManager.h"
#include <QDebug>

ClientListEventHandler::ClientListEventHandler(ApplicationRuntime* mainWindow, WebSocketClient* webSocketClient, QObject* parent)
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
    
    ICanvasHost* activeCanvas = m_mainWindow->getActiveCanvas();
    if (activeCanvas) {
        activeCanvas->updateRemoteSceneTargetFromClientList(clients);
    }
    
    QList<ClientInfo> displayList = m_mainWindow->buildDisplayClientList(clients);

    int previousConnectedCount = m_mainWindow->getLastConnectedClientCount();
    m_mainWindow->setLastConnectedClientCount(clients.size());

    const QString activeWorkspaceEndpointId =
        m_mainWindow->activeWorkspaceEndpointId();
    if (!activeWorkspaceEndpointId.isEmpty()) {
        ApplicationRuntime::ClientWorkspace* activeWorkspace =
            m_mainWindow->findWorkspace(activeWorkspaceEndpointId);
        if (activeWorkspace) {
            m_mainWindow->setSelectedClient(activeWorkspace->lastClientInfo);
            if (activeWorkspace->canvas) {
                activeWorkspace->canvas->setRemoteSceneTarget(
                    activeWorkspace->targetEndpointId,
                    activeWorkspace->lastClientInfo.getMachineName());
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
    if (navigationManager && navigationManager->isOnScreenView()
        && !activeWorkspaceEndpointId.isEmpty()) {
        ApplicationRuntime::ClientWorkspace* activeWorkspace =
            m_mainWindow->findWorkspace(activeWorkspaceEndpointId);
        if (activeWorkspace) {
            const ClientInfo* matchingDevice = nullptr;
            // Use the project-enriched list here. Discovery snapshots can be
            // identity-only for one refresh; the durable project supplies the
            // authenticated presentation fields without fabricating presence.
            for (const ClientInfo& candidate : displayList) {
                if (candidate.endpointId() == activeWorkspaceEndpointId
                    && candidate.isOnline()) {
                    matchingDevice = &candidate;
                    break;
                }
            }

            if (matchingDevice) {
                activeWorkspace->lastClientInfo = *matchingDevice;
                activeWorkspace->lastClientInfo.setEndpointId(activeWorkspaceEndpointId);
                activeWorkspace->lastClientInfo.setOnline(true);
                activeWorkspace->remoteContentClearedOnDisconnect = false;
                m_mainWindow->setSelectedClient(activeWorkspace->lastClientInfo);
                if (activeWorkspace->canvas) {
                    activeWorkspace->canvas->setRemoteSceneTarget(
                        activeWorkspaceEndpointId,
                        activeWorkspace->lastClientInfo.getMachineName());
                }
                navigationManager->refreshActiveClientPreservingCanvas(
                    activeWorkspace->lastClientInfo);
                m_mainWindow->updateClientNameDisplay(
                    activeWorkspace->lastClientInfo);
            } else {
                // Discovery loss does not delete the local project/canvas and
                // does not itself purge uploads. The RemoteSession lease owns
                // that terminal decision after its negotiated recovery deadline.
                RemoteSessionCoordinator* coordinator = m_webSocketClient
                    ? m_webSocketClient->remoteSessionCoordinator() : nullptr;
                const RemoteSessionCoordinator::Binding binding = coordinator
                    ? coordinator->outgoingForPeer(activeWorkspaceEndpointId)
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
                activeWorkspace->lastClientInfo.setOnline(false);
                activeWorkspace->lastClientInfo.setFromMemory(true);
                m_mainWindow->setSelectedClient(activeWorkspace->lastClientInfo);
                m_mainWindow->setPreserveViewportOnReconnect(true);
                m_mainWindow->refreshOverlayActionsState(false, /*propagateLoss*/ false);
                if (m_mainWindow->getUploadManager()) {
                    m_mainWindow->getUploadManager()->setTargetClientId(QString());
                }
                RemoteClientState state = RemoteClientState::disconnected();
                state.clientInfo = activeWorkspace->lastClientInfo;
                state.volumeVisible = false;
                state.volumePercent = -1;
                m_mainWindow->setRemoteClientState(state, /*propagateLoss*/ false);
            }
        }
    }
}
