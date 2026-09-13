#include "WebSocketMessageHandler.h"
#include "backend/runtime/ApplicationRuntime.h"
#include "frontend/managers/ui/RemoteClientState.h"
#include "backend/network/WebSocketClient.h"
#include "backend/network/UploadManager.h"
#include "frontend/rendering/navigation/ScreenNavigationManager.h"
#include "frontend/ui/notifications/ToastNotificationSystem.h"
#include <QDebug>

WebSocketMessageHandler::WebSocketMessageHandler(ApplicationRuntime* mainWindow, QObject* parent)
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

    qDebug() << "WebSocketMessageHandler: Connections established";
}

void WebSocketMessageHandler::onConnected()
{
    if (!m_mainWindow) return;

    UploadManager* uploadManager = m_mainWindow->getUploadManager();
    if (uploadManager && !uploadManager->receiverReadyForAdvertisement()
        && !uploadManager->retryReceiverAdvertisementCleanup()) {
        // Authentication alone must not advertise a receiver whose previous
        // server-boot cache could not be quarantined. In particular, do not
        // send endpoint_snapshot: the server will keep this installation out of
        // discovery and therefore unavailable as a target.
        m_mainWindow->setLocalNetworkStatus("Cleanup error");
        TOAST_ERROR(
            QStringLiteral("Remote cache cleanup failed (%1). This device remains unavailable.")
                .arg(uploadManager->receiverCleanupError()),
            6000);
        emit connectionStateChanged(false);
        return;
    }
    m_mainWindow->setLocalNetworkStatus("Connected");
    
    // CRITICAL FIX: Set SessionManager's local client ID for directional sessions
    if (m_mainWindow->getSessionManager() && m_mainWindow->getWebSocketClient()) {
        QString myClientId = m_mainWindow->getWebSocketClient()->endpointId();
        m_mainWindow->getSessionManager()->setMyClientId(myClientId);
        qDebug() << "SessionManager: Set local client ID to" << myClientId;
    }
    
    // CRITICAL FIX: Set UploadManager's local client ID for directional incoming sessions
    if (uploadManager && m_mainWindow->getWebSocketClient()) {
        QString myClientId = m_mainWindow->getWebSocketClient()->endpointId();
        uploadManager->setMyClientId(myClientId);
        qDebug() << "UploadManager: Set local client ID to" << myClientId;
    }
    
    // Sync this client's info with the server
    m_mainWindow->syncRegistration();
    
    // The next client_list carries a complete authoritative snapshot. Keep the
    // durable canvas visible while discovery and RemoteSession state reconcile.
    if (m_mainWindow->getNavigationManager() && m_mainWindow->getNavigationManager()->isOnScreenView()) {
        const ClientInfo& selectedClient = m_mainWindow->getSelectedClient();
        const QString selId = selectedClient.endpointId();
        if (!selId.isEmpty()) {
            // Indicate we're attempting to reach the remote again
            m_mainWindow->setRemoteConnectionStatus("CONNECTING...");
        }
    }
    
    // Show tray notification
    TOAST_SUCCESS("Connected to server", 2000);
    
    emit connectionStateChanged(true);
}

void WebSocketMessageHandler::onDisconnected()
{
    if (!m_mainWindow) return;

    m_mainWindow->setLocalNetworkStatus("Reconnecting");
    
    // If user is currently on a client's canvas page, immediately switch to loading state
    if (m_mainWindow->getNavigationManager() && m_mainWindow->getNavigationManager()->isOnScreenView()) {
        // Preserve viewport but show loading state
        m_mainWindow->setPreserveViewportOnReconnect(true);
        
        // Grace is not a terminal session transition. Keep the authoritative
        // canvas/scene and upload offset intact while disabling only new remote
        // commands.
        RemoteClientState state =
            RemoteClientState::connecting(m_mainWindow->getSelectedClient());
        state.connectionStatus = RemoteClientState::Reconnecting;
        state.volumePercent =
            m_mainWindow->getSelectedClient().getVolumePercent();
        state.volumeVisible = state.volumePercent >= 0;
        m_mainWindow->setRemoteClientState(state, false);
    }

    TOAST_WARNING("Connection interrupted — attempting session resume", 2500);
    
    emit connectionStateChanged(false);
}
