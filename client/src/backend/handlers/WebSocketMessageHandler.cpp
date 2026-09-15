#include "backend/handlers/WebSocketMessageHandler.h"

#include "backend/runtime/ApplicationRuntime.h"
#include "backend/domain/workspace/WorkspaceManager.h"
#include "backend/network/UploadManager.h"
#include "backend/network/WebSocketClient.h"

#include <QDebug>

WebSocketMessageHandler::WebSocketMessageHandler(
    ApplicationRuntime* mainWindow, QObject* parent)
    : QObject(parent)
    , m_mainWindow(mainWindow)
{
}

void WebSocketMessageHandler::setupConnections(WebSocketClient* client)
{
    if (!client) {
        qWarning() << "WebSocketMessageHandler: no WebSocket client";
        return;
    }
    connect(client, &WebSocketClient::connected,
            this, &WebSocketMessageHandler::onConnected);
    connect(client, &WebSocketClient::disconnected,
            this, &WebSocketMessageHandler::onDisconnected);
}

void WebSocketMessageHandler::onConnected()
{
    if (!m_mainWindow) return;

    UploadManager* uploads = m_mainWindow->getUploadManager();
    if (uploads && !uploads->receiverReadyForAdvertisement()
        && !uploads->retryReceiverAdvertisementCleanup()) {
        m_mainWindow->setLocalNetworkStatus(QStringLiteral("Cleanup error"));
        emit connectionStateChanged(false);
        return;
    }

    m_mainWindow->setLocalNetworkStatus(QStringLiteral("Connected"));
    if (m_mainWindow->getWorkspaceManager()
        && m_mainWindow->getWebSocketClient()) {
        const QString endpointId =
            m_mainWindow->getWebSocketClient()->endpointId();
        if (uploads) uploads->setMyClientId(endpointId);
    }
    m_mainWindow->syncRegistration();
    emit connectionStateChanged(true);
}

void WebSocketMessageHandler::onDisconnected()
{
    if (!m_mainWindow) return;
    m_mainWindow->setLocalNetworkStatus(QStringLiteral("Reconnecting"));
    emit connectionStateChanged(false);
}
