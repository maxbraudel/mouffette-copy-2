#include "backend/managers/network/ConnectionManager.h"
#include "backend/network/WebSocketClient.h"
#include "backend/domain/models/ClientInfo.h"
#include <QDebug>
#include <algorithm>

ConnectionManager::ConnectionManager(WebSocketClient* wsClient, QObject* parent)
    : QObject(parent),
      m_wsClient(wsClient),
      m_reconnectTimer(new QTimer(this)),
      m_attemptTimeoutTimer(new QTimer(this)),
      m_serverUrl(),
      m_isManualDisconnect(false)
{
    Q_ASSERT(m_wsClient);
    
    m_reconnectTimer->setSingleShot(true);
    m_attemptTimeoutTimer->setSingleShot(true);
    m_attemptTimeoutTimer->setInterval(3000);
    
    // Connect WebSocketClient signals to local slots
    connect(m_wsClient, &WebSocketClient::connected, this, &ConnectionManager::onConnected);
    connect(m_wsClient, &WebSocketClient::disconnected, this, &ConnectionManager::onDisconnected);
    connect(m_wsClient, &WebSocketClient::connectionError, this, &ConnectionManager::onConnectionError);
    connect(m_wsClient, &WebSocketClient::fatalError, this, &ConnectionManager::onFatalError);
    connect(m_wsClient, &WebSocketClient::leaseExpired,
            this, &ConnectionManager::onLeaseExpired);
    connect(m_wsClient, &WebSocketClient::transportHealthChanged,
            this, &ConnectionManager::onTransportHealthChanged);
    connect(m_reconnectTimer, &QTimer::timeout, this, &ConnectionManager::attemptReconnect);
    connect(m_attemptTimeoutTimer, &QTimer::timeout,
            this, &ConnectionManager::onAttemptTimedOut);
    
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
    m_reconnectTimer->stop();
    m_attemptTimeoutTimer->stop();
    m_fastRetryAttempt = 0;
    m_backgroundRetryAttempt = 0;
    m_wasWithinLease = false;
    m_attemptInProgress = false;
    m_fatalFailure = false;
    
    qDebug() << "ConnectionManager: Connecting to server:" << m_serverUrl;
    beginAttempt();
}

void ConnectionManager::disconnect()
{
    m_isManualDisconnect = true;
    m_reconnectTimer->stop();
    m_attemptTimeoutTimer->stop();
    m_attemptInProgress = false;
    m_fastRetryAttempt = 0;
    m_backgroundRetryAttempt = 0;
    
    if (m_wsClient) {
        m_wsClient->disconnect();
    }
    setStatus(QStringLiteral("Disconnected"));
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
    return m_status;
}

void ConnectionManager::onConnected()
{
    qDebug() << "ConnectionManager: Connected successfully";
    m_attemptInProgress = false;
    m_attemptTimeoutTimer->stop();
    m_fastRetryAttempt = 0;
    m_backgroundRetryAttempt = 0;
    m_wasWithinLease = false;
    m_reconnectTimer->stop();
    
    emit connected();
    setStatus(QStringLiteral("Connected"));
}

void ConnectionManager::onDisconnected()
{
    qDebug() << "ConnectionManager: Disconnected";
    m_attemptInProgress = false;
    m_attemptTimeoutTimer->stop();
    
    emit disconnected();
    setStatus(QStringLiteral("Disconnected"));
    
    // Schedule reconnect if not manually disconnected
    if (!m_isManualDisconnect && !m_fatalFailure) {
        scheduleReconnect();
    }
}

void ConnectionManager::onConnectionError(const QString& error)
{
    if (m_fatalFailure) return;
    qWarning() << "ConnectionManager: Connection error:" << error;
    
    emit connectionError(error);
    if (!m_wsClient->isConnected()) {
        setStatus(QStringLiteral("Connection error"));
    }
    // Socket errors normally lead to disconnected(); cover errors raised after
    // the socket has already reached UnconnectedState without owning a second
    // retry path.
    if (!m_isManualDisconnect && !m_fatalFailure
        && !m_wsClient->isTransportConnected()) {
        m_attemptInProgress = false;
        m_attemptTimeoutTimer->stop();
        scheduleReconnect();
    }
}

void ConnectionManager::onFatalError(const QString& error)
{
    m_fatalFailure = true;
    m_reconnectTimer->stop();
    m_attemptTimeoutTimer->stop();
    m_attemptInProgress = false;
    qCritical() << "ConnectionManager: Fatal transport error:" << error;
    emit connectionError(error);
    setStatus(QStringLiteral("Unavailable"));
}

void ConnectionManager::onLeaseExpired(const QString& serverBootId,
                                       quint64 connectionGeneration)
{
    m_fastRetryAttempt = 0;
    m_wasWithinLease = false;
    emit leaseExpired(serverBootId, connectionGeneration);
    if (!m_isManualDisconnect && !m_fatalFailure && !m_wsClient->isConnected()) {
        m_reconnectTimer->stop();
        scheduleReconnect();
    }
}

void ConnectionManager::onTransportHealthChanged(bool degraded)
{
    if (m_wsClient->isConnected()) {
        setStatus(degraded ? QStringLiteral("Degraded") : QStringLiteral("Connected"));
    }
}

void ConnectionManager::scheduleReconnect()
{
    if (m_fatalFailure || m_reconnectTimer->isActive()) {
        return; // Already scheduled
    }
    
    const bool withinLease = m_wsClient->hasUnexpiredLease();
    if (withinLease != m_wasWithinLease) {
        if (withinLease) m_fastRetryAttempt = 0;
        else m_backgroundRetryAttempt = 0;
        m_wasWithinLease = withinLease;
    }
    const int attempt = withinLease ? m_fastRetryAttempt++ : m_backgroundRetryAttempt++;
    const int delay = retryDelayForAttempt(attempt, withinLease);
    
    qDebug() << "ConnectionManager: Scheduling reconnect attempt" << (attempt + 1)
             << "in" << delay << "ms";
    
    setStatus(QStringLiteral("Reconnecting"));
    m_reconnectTimer->start(delay);
}

int ConnectionManager::retryDelayForAttempt(int attempt, bool withinLease)
{
    attempt = std::max(0, attempt);
    if (withinLease) {
        static constexpr int fastDelays[] = {0, 250, 500, 750};
        return fastDelays[std::min(attempt, 3)];
    }
    const int exponent = std::min(attempt, 5);
    return std::min(1000 * (1 << exponent), 30000);
}

void ConnectionManager::attemptReconnect()
{
    if (m_isManualDisconnect) {
        qDebug() << "ConnectionManager: Skipping reconnect (manual disconnect)";
        return;
    }
    
    beginAttempt();
}

void ConnectionManager::beginAttempt()
{
    if (m_isManualDisconnect || m_serverUrl.isEmpty() || m_attemptInProgress) return;
    m_attemptInProgress = true;
    qDebug() << "ConnectionManager: Attempting connection to" << m_serverUrl;
    m_wsClient->connectToServer(m_serverUrl);
    if (m_attemptInProgress && !m_wsClient->isConnected()) {
        const qint64 remainingLease = m_wsClient->leaseRemainingMs();
        const int timeoutMs = remainingLease > 0
            ? static_cast<int>(std::clamp<qint64>(remainingLease, 100, 750))
            : 3000;
        m_attemptTimeoutTimer->start(timeoutMs);
    }
}

void ConnectionManager::onAttemptTimedOut()
{
    if (m_isManualDisconnect || !m_attemptInProgress || m_wsClient->isConnected()) return;
    m_attemptInProgress = false;
    qWarning() << "ConnectionManager: Connection/authentication attempt timed out after"
               << m_attemptTimeoutTimer->interval() << "ms";
    emit connectionError(QStringLiteral("Connection timeout"));
    m_wsClient->abortConnectionAttempt();
    scheduleReconnect();
}

void ConnectionManager::setStatus(const QString& status)
{
    if (m_status == status) return;
    m_status = status;
    emit statusChanged(status);
}
