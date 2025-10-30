#include "backend/managers/network/ConnectionManager.h"
#include "backend/network/WebSocketClient.h"
#include "backend/domain/models/ClientInfo.h"
#include <QDebug>
#include <algorithm>

ConnectionManager::ConnectionManager(WebSocketClient* wsClient, QObject* parent)
    : QObject(parent),
      m_wsClient(wsClient),
      m_reconnectTimer(new QTimer(this)),
      m_serverUrl(),
      m_reconnectAttempts(0),
      m_maxReconnectDelay(15000),
      m_isManualDisconnect(false)
{
    Q_ASSERT(m_wsClient);
    
    m_reconnectTimer->setSingleShot(true);
    
    // Connect WebSocketClient signals to local slots
    connect(m_wsClient, &WebSocketClient::connected, this, &ConnectionManager::onConnected);
    connect(m_wsClient, &WebSocketClient::disconnected, this, &ConnectionManager::onDisconnected);
    connect(m_wsClient, &WebSocketClient::connectionError, this, &ConnectionManager::onConnectionError);
    connect(m_reconnectTimer, &QTimer::timeout, this, &ConnectionManager::attemptReconnect);
    
    // Forward registration confirmation
    connect(m_wsClient, &WebSocketClient::registrationConfirmed, 
            this, &ConnectionManager::registrationConfirmed);
}

void ConnectionManager::connectToServer(const QString& serverUrl)
{
    if (serverUrl.isEmpty()) {
        qWarning() << "ConnectionManager: Cannot connect with empty server URL";
        return;
    }
    
    m_serverUrl = serverUrl;
    m_isManualDisconnect = false;
    m_reconnectAttempts = 0;
    
    qDebug() << "ConnectionManager: Connecting to server:" << m_serverUrl;
    m_wsClient->connectToServer(m_serverUrl);
}

void ConnectionManager::disconnect()
{
    m_isManualDisconnect = true;
    m_reconnectTimer->stop();
    m_reconnectAttempts = 0;
    
    if (m_wsClient && m_wsClient->isConnected()) {
        m_wsClient->disconnect();
    }
}

bool ConnectionManager::isConnected() const
{
    return m_wsClient && m_wsClient->isConnected();
}

void ConnectionManager::setServerUrl(const QString& url)
{
    m_serverUrl = url;
}

QString ConnectionManager::getConnectionStatus() const
{
    if (!m_wsClient) {
        return "Error";
    }
    return m_wsClient->getConnectionStatus();
}

void ConnectionManager::onConnected()
{
    qDebug() << "ConnectionManager: Connected successfully";
    m_reconnectAttempts = 0;
    m_reconnectTimer->stop();
    
    emit connected();
    emit statusChanged("Connected");
}

void ConnectionManager::onDisconnected()
{
    qDebug() << "ConnectionManager: Disconnected";
    
    emit disconnected();
    emit statusChanged("Disconnected");
    
    // Schedule reconnect if not manually disconnected
    if (!m_isManualDisconnect) {
        scheduleReconnect();
    }
}

void ConnectionManager::onConnectionError(const QString& error)
{
    qWarning() << "ConnectionManager: Connection error:" << error;
    
    emit connectionError(error);
    emit statusChanged("Error");
    
    // Schedule reconnect on error
    if (!m_isManualDisconnect) {
        scheduleReconnect();
    }
}

void ConnectionManager::scheduleReconnect()
{
    if (m_reconnectTimer->isActive()) {
        return; // Already scheduled
    }
    
    const int delay = calculateReconnectDelay();
    m_reconnectAttempts++;
    
    qDebug() << "ConnectionManager: Scheduling reconnect attempt" << m_reconnectAttempts 
             << "in" << delay << "ms";
    
    emit statusChanged(QString("Reconnecting (%1)...").arg(m_reconnectAttempts));
    m_reconnectTimer->start(delay);
}

int ConnectionManager::calculateReconnectDelay() const
{
    // Exponential backoff: 1s, 2s, 4s, 8s, 15s (max)
    const int baseDelay = 1000; // 1 second
    const int exponentialDelay = baseDelay * (1 << std::min(m_reconnectAttempts - 1, 4));
    return std::min(exponentialDelay, m_maxReconnectDelay);
}

void ConnectionManager::attemptReconnect()
{
    if (m_isManualDisconnect) {
        qDebug() << "ConnectionManager: Skipping reconnect (manual disconnect)";
        return;
    }
    
    qDebug() << "ConnectionManager: Attempting reconnect to" << m_serverUrl;
    m_wsClient->connectToServer(m_serverUrl);
}
