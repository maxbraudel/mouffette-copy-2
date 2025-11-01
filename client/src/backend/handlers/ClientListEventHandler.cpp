#include "ClientListEventHandler.h"
#include "MainWindow.h"
#include "frontend/managers/ui/RemoteClientState.h"
#include "backend/network/WebSocketClient.h"
#include "backend/domain/models/ClientInfo.h"
#include "frontend/rendering/canvas/ScreenCanvas.h"
#include "frontend/rendering/navigation/ScreenNavigationManager.h"
#include "backend/network/UploadManager.h"
#include "backend/network/WatchManager.h"
#include "frontend/ui/pages/ClientListPage.h"
#include <QDebug>
#include <optional>

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
    ScreenCanvas* screenCanvas = m_mainWindow->getScreenCanvas();
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

    // Update remote status and canvas behavior if we're on screen view:
    // Handle remote reconnect where the server assigns a NEW clientId.
    // Strategy:
    // 1) If selected client's id is present -> mark CONNECTED. If we're on loader, request screens + watch.
    // 2) Else, try to match by machineName (and platform). If found -> treat as same device, switch selection to new id,
    //    reset reveal flag, show screen view for it, request screens + watch.
    // 3) Else -> mark DISCONNECTED and keep loader.
    ScreenNavigationManager* navigationManager = m_mainWindow->getNavigationManager();
    if (navigationManager && navigationManager->isOnScreenView() && !activeSessionIdentity.isEmpty()) {
        MainWindow::CanvasSession* activeSession = m_mainWindow->findCanvasSession(activeSessionIdentity);
        if (activeSession) {
            const QString selId = activeSession->serverAssignedId;
            const QString selName = activeSession->lastClientInfo.getMachineName();
            const QString selPlatform = activeSession->lastClientInfo.getPlatform();

            const auto findById = [&clients](const QString& id) -> std::optional<ClientInfo> {
                for (const auto& c : clients) { if (c.getId() == id) return c; }
                return std::nullopt;
            };
            const auto findByNamePlatform = [&clients](const QString& name, const QString& platform) -> std::optional<ClientInfo> {
                for (const auto& c : clients) {
                    if (c.getMachineName().compare(name, Qt::CaseInsensitive) == 0 && c.getPlatform() == platform) return c;
                }
                return std::nullopt;
            };

            std::optional<ClientInfo> byId;
            if (!selId.isEmpty()) {
                byId = findById(selId);
            }

            if (byId.has_value()) {
                activeSession->serverAssignedId = byId->getId();
                activeSession->lastClientInfo = byId.value();
                activeSession->lastClientInfo.setClientId(activeSessionIdentity);
                activeSession->remoteContentClearedOnDisconnect = false;
                m_mainWindow->setSelectedClient(activeSession->lastClientInfo);
                if (activeSession->canvas && !activeSession->serverAssignedId.isEmpty()) {
                    activeSession->canvas->setRemoteSceneTarget(activeSession->serverAssignedId, selName);
                }
                UploadManager* uploadManager = m_mainWindow->getUploadManager();
                if (uploadManager) uploadManager->setTargetClientId(activeSession->serverAssignedId);
                
                const bool isActiveSelection = (activeSession->persistentClientId == activeSessionIdentity);
                if (isActiveSelection) {
                    QString activeRemoteClientId = m_mainWindow->getActiveRemoteClientId();
                    if (activeRemoteClientId != activeSession->serverAssignedId) {
                        m_mainWindow->setActiveRemoteClientId(activeSession->serverAssignedId);
                        m_mainWindow->setRemoteClientConnected(false);
                    }
                    if (!m_mainWindow->isRemoteClientConnected()) {
                        m_mainWindow->addRemoteStatusToLayout();
                        m_mainWindow->setRemoteConnectionStatus("CONNECTING...", /*propagateLoss*/ false);
                        if (m_webSocketClient && m_webSocketClient->isConnected()) {
                            m_mainWindow->setCanvasRevealedForCurrentClient(false);
                            m_webSocketClient->requestScreens(activeSession->serverAssignedId);
                            WatchManager* watchManager = m_mainWindow->getWatchManager();
                            if (watchManager) {
                                if (watchManager->watchedClientId() != activeSession->serverAssignedId) {
                                    watchManager->unwatchIfAny();
                                    watchManager->toggleWatch(activeSession->serverAssignedId);
                                }
                            }
                        }
                    }
                }
            } else {
                auto byName = findByNamePlatform(selName, selPlatform);
                if (byName.has_value()) {
                    activeSession->serverAssignedId = byName->getId();
                    activeSession->lastClientInfo = byName.value();
                    activeSession->lastClientInfo.setClientId(activeSessionIdentity);
                    activeSession->remoteContentClearedOnDisconnect = false;
                    m_mainWindow->setSelectedClient(activeSession->lastClientInfo);
                    if (activeSession->canvas && !activeSession->serverAssignedId.isEmpty()) {
                        activeSession->canvas->setRemoteSceneTarget(activeSession->serverAssignedId, selName);
                    }
                    UploadManager* uploadManager = m_mainWindow->getUploadManager();
                    if (uploadManager) uploadManager->setTargetClientId(activeSession->serverAssignedId);
                    
                    if (navigationManager) {
                        navigationManager->refreshActiveClientPreservingCanvas(activeSession->lastClientInfo);
                    }
                    m_mainWindow->updateClientNameDisplay(activeSession->lastClientInfo);
                    const bool isActiveSelection = (activeSession->persistentClientId == activeSessionIdentity);
                    if (isActiveSelection) {
                        QString activeRemoteClientId = m_mainWindow->getActiveRemoteClientId();
                        if (activeRemoteClientId != activeSession->serverAssignedId) {
                            m_mainWindow->setActiveRemoteClientId(activeSession->serverAssignedId);
                            m_mainWindow->setRemoteClientConnected(false);
                        }
                        if (!m_mainWindow->isRemoteClientConnected()) {
                            m_mainWindow->setRemoteConnectionStatus("CONNECTING...");
                            m_mainWindow->addRemoteStatusToLayout();
                            if (m_webSocketClient && m_webSocketClient->isConnected()) {
                                m_mainWindow->setCanvasRevealedForCurrentClient(false);
                                m_webSocketClient->requestScreens(activeSession->serverAssignedId);
                                WatchManager* watchManager = m_mainWindow->getWatchManager();
                                if (watchManager) {
                                    if (watchManager->watchedClientId() != activeSession->serverAssignedId) {
                                        watchManager->unwatchIfAny();
                                        watchManager->toggleWatch(activeSession->serverAssignedId);
                                    }
                                }
                            }
                        }
                    }
                } else {
                    if (!activeSession->remoteContentClearedOnDisconnect) {
                        m_mainWindow->unloadUploadsForSession(*activeSession, true);
                    }
                    
                    m_mainWindow->setPreserveViewportOnReconnect(true);
                    UploadManager* uploadManager = m_mainWindow->getUploadManager();
                    if (uploadManager) {
                        uploadManager->setTargetClientId(QString());
                    }
                    WatchManager* watchManager = m_mainWindow->getWatchManager();
                    if (watchManager) {
                        watchManager->unwatchIfAny();
                    }
                    
                    // Apply DISCONNECTED state
                    RemoteClientState state = RemoteClientState::disconnected();
                    const int volumePercent = activeSession->lastClientInfo.getVolumePercent();
                    if (volumePercent >= 0) {
                        m_mainWindow->setSelectedClient(activeSession->lastClientInfo);
                        state.clientInfo = activeSession->lastClientInfo;
                        state.volumeVisible = true;
                        state.volumePercent = volumePercent;
                    }
                    
                    m_mainWindow->setRemoteClientState(state);
                }
            }
        }
    }
}
