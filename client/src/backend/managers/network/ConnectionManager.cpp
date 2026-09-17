#include "backend/managers/network/ConnectionManager.h"
#include "backend/config/AppConfig.h"
#include "backend/network/WebSocketClient.h"
#include <QRandomGenerator>
#include "backend/domain/models/ClientInfo.h"
#include <QDebug>
#include <algorithm>

ConnectionManager::ConnectionManager(WebSocketClient* wsClient, QObject* parent)
    : QObject(parent),
      m_wsClient(wsClient),
      m_reconnectTimer(new QTimer(this)),
      m_attemptTimeoutTimer(new QTimer(this)),
      m_serverUrl()
{
    Q_ASSERT(m_wsClient);
    
    m_reconnectTimer->setSingleShot(true);
    m_attemptTimeoutTimer->setSingleShot(true);
    m_attemptTimeoutTimer->setInterval(
        AppConfig::instance().connectionAttemptTimeoutMs());
    
    // Connect WebSocketClient signals to local slots
    connect(m_wsClient, &WebSocketClient::connected, this, &ConnectionManager::onConnected);
    connect(m_wsClient, &WebSocketClient::transportConnected, this, [this]() {
        if (m_desiredEnabled && !m_draining && !m_fatalFailure)
            setState(State::Authenticating);
    });
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
    connect(m_wsClient, &WebSocketClient::registrationConfirmed, this,
            [this](const ClientInfo& info) {
        if (!m_desiredEnabled || m_draining) return;
        m_registrationReady = true;
        refreshAuthenticatedState();
        emit registrationConfirmed(info);
    });
    connect(m_wsClient, &WebSocketClient::reconciliationCompleted, this, [this]() {
        if (!m_desiredEnabled || m_draining) return;
        m_reconciliationReady = true;
        refreshAuthenticatedState();
    });
}

void ConnectionManager::connectToServer(const QString& serverUrl)
{
    if (serverUrl.trimmed().isEmpty()) return;
    if (m_serverUrl != serverUrl && (!m_serverUrl.isEmpty()
        || m_wsClient->isTransportConnected())) {
        reconfigureServer(serverUrl);
        setConnectionEnabled(true);
        return;
    }
    m_serverUrl = serverUrl;
    setConnectionEnabled(true);
}

void ConnectionManager::suspendAttempts()
{
    m_reconnectTimer->stop();
    m_attemptTimeoutTimer->stop();
    m_attemptInProgress = false;
    m_stableConnection.invalidate();
}

void ConnectionManager::setConnectionEnabled(bool enabled)
{
    const bool changed = m_desiredEnabled != enabled;
    m_desiredEnabled = enabled;
    if (changed) emit connectionEnabledChanged(enabled);
    if (!enabled) {
        if (!changed && m_state == State::Disconnected && !m_draining) return;
        suspendAttempts();
        if (m_draining) return;
        m_draining = true;
        ++m_transitionId;
        setState(State::Disconnecting);
        emit disconnectRequested(m_transitionId);
        return;
    }
    if (m_draining) return; // The latest intent is replayed after the old drain.
    if (m_wsClient->isConnected() || m_attemptInProgress
        || m_reconnectTimer->isActive()) return;
    m_fatalFailure = false;
    beginAttempt();
}

void ConnectionManager::completeDisconnect(quint64 transitionId)
{
    if (!m_draining || transitionId != m_transitionId) return;
    suspendAttempts();
    m_wsClient->disconnect();
    m_draining = false;
    setState(State::Disconnected);
    if (m_desiredEnabled) {
        QTimer::singleShot(0, this, [this, transitionId]() {
            if (transitionId == m_transitionId && m_desiredEnabled && !m_draining) {
                m_fatalFailure = false;
                beginAttempt();
            }
        });
    }
}

void ConnectionManager::reconfigureServer(const QString& serverUrl)
{
    if (serverUrl.trimmed().isEmpty() || m_serverUrl == serverUrl) return;
    m_serverUrl = serverUrl;
    if (!m_desiredEnabled || m_draining) return;
    if (!m_wsClient->isTransportConnected() && !m_attemptInProgress
        && m_state == State::Disconnected) { beginAttempt(); return; }
    suspendAttempts();
    m_draining = true;
    ++m_transitionId;
    setState(State::Disconnecting);
    emit disconnectRequested(m_transitionId);
}

void ConnectionManager::disconnect()
{
    if (m_desiredEnabled) {
        m_desiredEnabled = false;
        emit connectionEnabledChanged(false);
    }
    suspendAttempts();
    ++m_transitionId;
    m_draining = false;
    m_wsClient->disconnect();
    setState(State::Disconnected);
}

void ConnectionManager::setReceiverReady(bool ready)
{
    m_receiverReady = ready;
    // Quarantine invalidates advertisement; a fresh endpoint snapshot must be
    // acknowledged before the next Connected state, even on the same socket.
    if (!ready) m_registrationReady = false;
    refreshAuthenticatedState();
}

void ConnectionManager::refreshAuthenticatedState()
{
    if (!m_desiredEnabled || m_draining || m_fatalFailure || !m_wsClient->isConnected()) return;
    if (!m_receiverReady) setState(State::CleanupPending);
    else if (!m_registrationReady || !m_reconciliationReady) setState(State::Synchronizing);
    else setState(m_degraded ? State::Degraded : State::Connected);
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
    switch (m_state) {
    case State::Disconnected: return QStringLiteral("Disconnected");
    case State::Disconnecting: return QStringLiteral("Disconnecting");
    case State::Connecting: return QStringLiteral("Connecting");
    case State::Authenticating: return QStringLiteral("Authenticating");
    case State::Synchronizing: return QStringLiteral("Synchronizing");
    case State::Connected: return QStringLiteral("Connected");
    case State::Degraded: return QStringLiteral("Degraded");
    case State::Reconnecting: return QStringLiteral("Reconnecting");
    case State::CleanupPending: return QStringLiteral("Cleanup pending");
    case State::Failed: return QStringLiteral("Failed");
    }
    return QStringLiteral("Disconnected");
}

void ConnectionManager::onConnected()
{
    if (!m_desiredEnabled || m_draining || m_fatalFailure) return;
    qDebug() << "ConnectionManager: Connected successfully";
    m_attemptInProgress = false;
    m_attemptTimeoutTimer->stop();
    m_stableConnection.start();
    m_reconnectTimer->stop();
    
    refreshAuthenticatedState();
    if (m_desiredEnabled && !m_draining) emit connected();
}

void ConnectionManager::onDisconnected()
{
    qDebug() << "ConnectionManager: Disconnected";
    m_attemptInProgress = false;
    m_attemptTimeoutTimer->stop();
    
    if (m_stableConnection.isValid()
        && m_stableConnection.elapsed() >= AppConfig::instance().reconnectStableResetMs()) {
        m_fastRetryAttempt = 0;
        m_backgroundRetryAttempt = 0;
    }
    m_stableConnection.invalidate();
    // A disconnected observer may synchronously finish the drain and queue
    // the latest Enable. That transition already owns the next attempt.
    const bool wasDraining = m_draining;
    emit disconnected();
    if (wasDraining || m_draining) return;
    if (m_fatalFailure) { setState(State::Failed); return; }
    setState(State::Disconnected);
    
    // Schedule reconnect if not manually disconnected
    if (m_desiredEnabled && !m_draining && !m_fatalFailure) {
        scheduleReconnect();
    }
}

void ConnectionManager::onConnectionError(const QString& error)
{
    if (m_fatalFailure || !m_desiredEnabled || m_draining) return;
    qWarning() << "ConnectionManager: Connection error:" << error;
    
    emit connectionError(error);
    // Socket errors normally lead to disconnected(); cover errors raised after
    // the socket has already reached UnconnectedState without owning a second
    // retry path.
    if (m_desiredEnabled && !m_draining && !m_fatalFailure
        && !m_wsClient->isTransportConnected()) {
        m_attemptInProgress = false;
        m_attemptTimeoutTimer->stop();
        scheduleReconnect();
    }
}

void ConnectionManager::onFatalError(const QString& error)
{
    if (!m_desiredEnabled || m_draining) return;
    m_fatalFailure = true;
    m_reconnectTimer->stop();
    m_attemptTimeoutTimer->stop();
    m_attemptInProgress = false;
    qCritical() << "ConnectionManager: Fatal transport error:" << error;
    emit connectionError(error);
    setState(State::Failed);
}

void ConnectionManager::onLeaseExpired(const QString& serverBootId,
                                       quint64 connectionGeneration)
{
    m_fastRetryAttempt = 0;
    m_wasWithinLease = false;
    emit leaseExpired(serverBootId, connectionGeneration);
    if (m_desiredEnabled && !m_draining && !m_fatalFailure && !m_wsClient->isConnected()) {
        m_reconnectTimer->stop();
        scheduleReconnect();
    }
}

void ConnectionManager::onTransportHealthChanged(bool degraded)
{
    m_degraded = degraded;
    refreshAuthenticatedState();
}

void ConnectionManager::scheduleReconnect()
{
    if (!m_desiredEnabled || m_draining || m_fatalFailure
        || m_reconnectTimer->isActive() || m_attemptInProgress) {
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
    
    setState(State::Reconnecting);
    m_reconnectTimer->start(delay);
}

int ConnectionManager::retryDelayForAttempt(int attempt, bool withinLease)
{
    attempt = std::max(0, attempt);
    const AppConfig& config = AppConfig::instance();
    if (withinLease) {
        const qint64 base = qMin<qint64>(
            static_cast<qint64>(attempt) * config.reconnectFastStepMs(),
            config.reconnectFastMaxMs());
        return static_cast<int>(base);
    }
    const int exponent = std::min(attempt, 20);
    const qint64 capped = qMin<qint64>(
        static_cast<qint64>(config.reconnectBaseMs()) << exponent,
        config.reconnectMaxMs());
    const qint64 spread = capped * config.reconnectJitterPercent() / 100;
    if (spread <= 0) return static_cast<int>(capped);
    const qint64 minimum = qMax<qint64>(1, capped - spread);
    const quint64 width = static_cast<quint64>(capped + spread - minimum + 1);
    return static_cast<int>(minimum
        + static_cast<qint64>(QRandomGenerator::global()->generate64() % width));
}

void ConnectionManager::attemptReconnect()
{
    if (!m_desiredEnabled || m_draining || m_fatalFailure) {
        qDebug() << "ConnectionManager: Skipping reconnect (manual disconnect)";
        return;
    }
    
    beginAttempt();
}

void ConnectionManager::beginAttempt()
{
    if (!m_desiredEnabled || m_draining || m_fatalFailure
        || m_serverUrl.isEmpty() || m_attemptInProgress
        || m_wsClient->isConnected()) return;
    m_reconnectTimer->stop();
    m_registrationReady = false;
    m_reconciliationReady = false;
    m_degraded = false;
    setState(State::Connecting);
    m_attemptInProgress = true;
    qDebug() << "ConnectionManager: Attempting connection to" << m_serverUrl;
    m_wsClient->connectToServer(m_serverUrl);
    if (m_attemptInProgress && !m_wsClient->isConnected()) {
        // Session recovery expiry is independent of a new TCP/authentication
        // attempt: never abort a viable handshake merely to fit a grace window.
        const int timeoutMs = AppConfig::instance().connectionAttemptTimeoutMs();
        m_attemptTimeoutTimer->start(timeoutMs);
    }
}

void ConnectionManager::onAttemptTimedOut()
{
    if (!m_desiredEnabled || m_draining || m_fatalFailure
        || !m_attemptInProgress || m_wsClient->isConnected()) return;
    m_attemptInProgress = false;
    qWarning() << "ConnectionManager: Connection/authentication attempt timed out after"
               << m_attemptTimeoutTimer->interval() << "ms";
    emit connectionError(QStringLiteral("Connection timeout"));
    m_wsClient->abortConnectionAttempt();
    scheduleReconnect();
}

void ConnectionManager::setState(State state)
{
    if (m_state == state) return;
    m_state = state;
    emit stateChanged(state);
    emit statusChanged(getConnectionStatus());
}
