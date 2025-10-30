#include "WebSocketMessageHandler.h"
#include "MainWindow.h"
#include "backend/network/WebSocketClient.h"
#include "backend/network/WatchManager.h"
#include "backend/network/UploadManager.h"
#include "frontend/rendering/navigation/ScreenNavigationManager.h"
#include "frontend/ui/pages/ClientListPage.h"
#include "frontend/ui/notifications/ToastNotificationSystem.h"
#include <QDebug>

WebSocketMessageHandler::WebSocketMessageHandler(MainWindow* mainWindow, QObject* parent)
    : QObject(parent)
    , m_mainWindow(mainWindow)
{
}

void WebSocketMessageHandler::setupConnections(WebSocketClient* client)
{
    if (!client) {
        qWarning() << "WebSocketMessageHandler::setupConnections - No WebSocket client provided";
        return;
    }

    // Connect to connection lifecycle signals
    connect(client, &WebSocketClient::connected, this, &WebSocketMessageHandler::onConnected);
    connect(client, &WebSocketClient::disconnected, this, &WebSocketMessageHandler::onDisconnected);

    // Connect to message received signal with routing logic
    connect(client, &WebSocketClient::messageReceived, this, [this](const QJsonObject& message) {
        if (message.value("type").toString() == "state_sync") {
            handleStateSyncMessage(message);
        }
    });

    qDebug() << "WebSocketMessageHandler: Connections established";
}

void WebSocketMessageHandler::onConnected()
{
    if (!m_mainWindow) return;

    m_mainWindow->setUIEnabled(true);
    m_mainWindow->setLocalNetworkStatus("Connected");
    
    // Reset reconnection state on successful connection
    m_mainWindow->resetReconnectState();
    
    // Sync this client's info with the server
    m_mainWindow->syncRegistration();
    
    // If we were on a client's canvas page when the connection dropped,
    // re-request that client's screens and re-establish watch
    if (m_mainWindow->getNavigationManager() && m_mainWindow->getNavigationManager()->isOnScreenView()) {
        // Ensure the canvas will reveal again on fresh screens
        m_mainWindow->setCanvasRevealedForCurrentClient(false);
        
        // Remove volume indicator until remote is ready again
        m_mainWindow->removeVolumeIndicatorFromLayout();
        
        const ClientInfo& selectedClient = m_mainWindow->getSelectedClient();
        const QString selId = selectedClient.getId();
        
        if (!selId.isEmpty() && m_mainWindow->getWebSocketClient() && 
            m_mainWindow->getWebSocketClient()->isConnected()) {
            // Indicate we're attempting to reach the remote again
            m_mainWindow->setRemoteConnectionStatus("CONNECTING...");
            m_mainWindow->addRemoteStatusToLayout();
            m_mainWindow->getWebSocketClient()->requestScreens(selId);
            
            if (m_mainWindow->getWatchManager()) {
                // Ensure a clean state after reconnect, then start watching again
                m_mainWindow->getWatchManager()->unwatchIfAny();
                m_mainWindow->getWatchManager()->toggleWatch(selId);
            }
        }
    }
    
    // Show tray notification
    TOAST_SUCCESS("Connected to server", 2000);
    
    emit connectionStateChanged(true);
}

void WebSocketMessageHandler::onDisconnected()
{
    if (!m_mainWindow) return;

    m_mainWindow->setUIEnabled(false);
    m_mainWindow->setLocalNetworkStatus("Disconnected");
    
    // If user is currently on a client's canvas page, immediately switch to loading state
    if (m_mainWindow->getNavigationManager() && m_mainWindow->getNavigationManager()->isOnScreenView()) {
        // Preserve viewport but show loading state
        m_mainWindow->setPreserveViewportOnReconnect(true);
        
        // Remove volume indicator FIRST (before showing spinner) to avoid layout shift
        m_mainWindow->removeVolumeIndicatorFromLayout();
        
        // Show error state explicitly
        m_mainWindow->addRemoteStatusToLayout();
        m_mainWindow->setRemoteConnectionStatus("ERROR");
        
        // Now show the inline spinner after volume is removed
        m_mainWindow->getNavigationManager()->enterLoadingStateImmediate();
    }
    
    // Inform upload manager of connection loss
    bool hadUploadInProgress = false;
    if (m_mainWindow->getUploadManager()) {
        hadUploadInProgress = m_mainWindow->getUploadManager()->isUploading() || 
                             m_mainWindow->getUploadManager()->isFinalizing();
        m_mainWindow->getUploadManager()->onConnectionLost();
    }
    
    if (hadUploadInProgress) {
        TOAST_ERROR("Upload interrupted - connection lost", 3000);
    } else {
        TOAST_ERROR("Disconnected from server", 3000);
    }
    
    // Start smart reconnection if client is enabled and not manually disconnected
    if (!m_mainWindow->isUserDisconnected()) {
        m_mainWindow->scheduleReconnect();
    }
    
    // Reset upload state for all sessions
    m_mainWindow->resetAllSessionUploadStates();
    
    // Stop watching if any
    if (m_mainWindow->getWatchManager()) {
        m_mainWindow->getWatchManager()->unwatchIfAny();
    }
    
    // Clear client list
    if (m_mainWindow->getClientListPage()) {
        m_mainWindow->getClientListPage()->updateClientList(QList<ClientInfo>());
    }
    
    emit connectionStateChanged(false);
}

void WebSocketMessageHandler::handleStateSyncMessage(const QJsonObject& message)
{
    if (!m_mainWindow) return;

    // Process state_sync message from server after reconnection
    const QJsonArray ideas = message.value("ideas").toArray();
    
    if (ideas.isEmpty()) {
        qDebug() << "WebSocketMessageHandler: Received empty state_sync";
        return;
    }
    
    qDebug() << "WebSocketMessageHandler: Processing state_sync with" << ideas.size() << "idea(s)";
    
    for (const QJsonValue& ideaValue : ideas) {
        const QJsonObject ideaObj = ideaValue.toObject();
        const QString canvasSessionId = ideaObj.value("canvasSessionId").toString();
        const QJsonArray fileIdsArray = ideaObj.value("fileIds").toArray();
        
        if (fileIdsArray.isEmpty()) continue;
        
        QSet<QString> fileIds;
        for (const QJsonValue& fidValue : fileIdsArray) {
            const QString fid = fidValue.toString();
            if (!fid.isEmpty()) {
                fileIds.insert(fid);
            }
        }
        
        qDebug() << "WebSocketMessageHandler: Syncing idea" << canvasSessionId 
                 << "with" << fileIds.size() << "file(s)";
        
        // Delegate to MainWindow to update session state
        m_mainWindow->syncCanvasSessionFromServer(canvasSessionId, fileIds);
    }
}
