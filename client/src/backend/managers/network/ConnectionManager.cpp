#include "backend/managers/network/ConnectionManager.h"
#include "backend/config/AppConfig.h"
#include "backend/network/WebSocketClient.h"
#include "backend/network/RemoteSessionCoordinator.h"
#include "backend/network/RetryPolicy.h"
#include "backend/network/NetworkDiagnostics.h"
#include <QMetaEnum>
#include "backend/domain/models/ClientInfo.h"
#include <QDebug>
#include <algorithm>

ConnectionManager::ConnectionManager(WebSocketClient* wsClient, QObject* parent, RetryScheduler::Clock clock)
    : QObject(parent),
      m_wsClient(wsClient),
      m_clock(std::move(clock)),
      m_retries(this, m_clock),
      m_attemptTimeoutTimer(new QTimer(this)),
      m_serverUrl()
{
    Q_ASSERT(m_wsClient);

    m_recoveryDisplayTimer.setSingleShot(true);
    m_recoveryDisplayTimer.setTimerType(Qt::PreciseTimer);
    connect(&m_recoveryDisplayTimer, &QTimer::timeout, this, [this] {
        const qint64 remaining = m_recoveryDisplayDeadlineMs - m_clock();
        if (remaining > 0) {
            m_recoveryDisplayTimer.start(static_cast<int>(remaining));
            return;
        }
        emit statusChanged(getConnectionStatus());
        emit retryStateChanged();
    });
    
    m_syncTimeoutTimer.setSingleShot(true);
    connect(&m_syncTimeoutTimer, &QTimer::timeout, this, [this] {
        if (m_state != State::Synchronizing || !m_desiredEnabled || m_draining) return;
        m_lastError = QStringLiteral("Initial synchronization timed out");
        qWarning() << "connection_sync_timeout" << "transition" << m_transitionId;
        m_wsClient->abortConnectionAttempt();
        scheduleReconnect();
    });
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
    connect(m_wsClient, &WebSocketClient::remoteSessionRecoveryExpired, this,
            [this](const QString&, quint64) {
        if (m_recoveryDisplayDeadlineMs < 0) return;
        m_recoveryDisplayDeadlineMs = m_clock();
        m_recoveryDisplayTimer.stop();
        emit statusChanged(getConnectionStatus());
        emit retryStateChanged();
    });
    connect(m_wsClient, &WebSocketClient::heartbeatSampleReceived, this, [this] {
        observeStability(m_state == State::Connected);
    });
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
    m_recoveryDisplayTimer.stop();
    m_recoveryDisplayDeadlineMs = -1;
    m_retries.cancel(QStringLiteral("connect"));
    m_attemptTimeoutTimer->stop();
    m_attemptInProgress = false;
    m_syncTimeoutTimer.stop();
    m_stableConnection.reset();
    setRetryAction(RetryAction::Idle);
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
        || m_retries.contains(QStringLiteral("connect"))) return;
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
    return isTransportAuthenticated();
}

bool ConnectionManager::isTransportAuthenticated() const
{
    return m_wsClient && m_wsClient->isTransportAuthenticated();
}

QString ConnectionManager::connectionDetail() const
{
    QStringList details;
    if (recoveryDisplayActive()) details << QStringLiteral("Network interrupted; reconnecting automatically");
    if (m_state == State::Authenticating) details << QStringLiteral("Authenticating with server");
    else if (m_state == State::Synchronizing) details << QStringLiteral("Synchronizing registration and sessions");
    else if (m_state == State::Connecting) details << QStringLiteral("Establishing server connection");
    else if (m_state == State::CleanupPending) details << QStringLiteral("Waiting for local resource cleanup");
    else if (m_draining) details << (m_desiredEnabled
        ? QStringLiteral("Finishing disconnect; Enable is queued") : QStringLiteral("Finishing disconnect"));
    else if (!m_desiredEnabled) details << QStringLiteral("Network disabled");
    if (!m_lastError.isEmpty()) details << m_lastError;
    if (nextAttemptAtMs() >= 0) details << QStringLiteral("Next attempt in %1 s")
        .arg(qMax<qint64>(0, nextAttemptAtMs() - m_clock()) / 1000.0, 0, 'f', 1);
    if (m_retryAction == RetryAction::Blocked) details << QStringLiteral("Automatic attempts blocked; corrective action required");
    return details.join(QStringLiteral("\n"));
}

void ConnectionManager::setRetryAction(RetryAction action)
{
    m_retryAction = action;
    emit retryStateChanged();
}

void ConnectionManager::setServerUrl(const QString& url)
{
    m_serverUrl = url;
}

QString ConnectionManager::getConnectionStatus() const
{
    // Keep transport/authentication phases internal during the fixed recovery
    // window. This changes presentation only; commands remain fenced while the
    // underlying connection is unauthenticated or not fully synchronized.
    if (recoveryDisplayActive() && m_state != State::CleanupPending)
        return QStringLiteral("Degraded");
    if (m_recoveryDisplayDeadlineMs >= 0 && !recoveryDisplayActive()
        && m_state != State::Connected && m_state != State::CleanupPending
        && m_state != State::Disconnecting && m_state != State::Failed)
        return QStringLiteral("Disconnected");
    switch (m_state) {
    case State::Disconnected: return QStringLiteral("Disconnected");
    case State::Disconnecting: return QStringLiteral("Disconnecting");
    case State::Connecting: return QStringLiteral("Connecting");
    case State::Authenticating: return QStringLiteral("Connecting");
    case State::Synchronizing: return QStringLiteral("Connecting");
    case State::Connected: return QStringLiteral("Connected");
    case State::Degraded: return m_recoveryDisplayDeadlineMs >= 0
        && !recoveryDisplayActive() ? QStringLiteral("Disconnected") : QStringLiteral("Degraded");
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
    m_retries.cancel(QStringLiteral("connect"));
    
    refreshAuthenticatedState();
    if (m_desiredEnabled && !m_draining) emit connected();
}

void ConnectionManager::onDisconnected()
{
    qDebug() << "ConnectionManager: Disconnected";
    m_attemptInProgress = false;
    m_attemptTimeoutTimer->stop();
    
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
    
    m_lastError = error;
    emit retryStateChanged();
    emit connectionError(error);
    // Socket errors normally lead to disconnected(); cover errors raised after
    // the socket has already reached UnconnectedState without owning a second
    // retry path.
    if (m_desiredEnabled && !m_draining && !m_fatalFailure
        && !m_wsClient->isTransportConnected()) {
        // Some platforms deliver the socket error before disconnected/health.
        // Enter recovery before publishing the retry's Disconnected phase.
        onTransportHealthChanged(true);
        m_attemptInProgress = false;
        m_attemptTimeoutTimer->stop();
        scheduleReconnect();
    }
}

void ConnectionManager::onFatalError(const QString& error)
{
    if (!m_desiredEnabled || m_draining) return;
    m_fatalFailure = true;
    m_lastError = error;
    setRetryAction(RetryAction::Blocked);
    m_syncTimeoutTimer.stop();
    m_retries.cancel(QStringLiteral("connect"));
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
        m_retries.cancel(QStringLiteral("connect"));
        scheduleReconnect();
    }
}

void ConnectionManager::onTransportHealthChanged(bool degraded)
{
    if (degraded && m_desiredEnabled && !m_draining && !m_fatalFailure
        && m_recoveryDisplayDeadlineMs < 0
        && (m_wsClient->isConnected() || (m_registrationReady && m_reconciliationReady))) {
        // The socket is already disconnected when a hard-loss health signal
        // arrives. Completed synchronization still proves this is recovery,
        // rather than a failed initial connection attempt.
        qint64 duration = m_wsClient->transportRecoveryRemainingMs();
        for (const auto& binding : m_wsClient->remoteSessionCoordinator()->all()) {
            if (binding.phase == QLatin1String("Active") || binding.phase == QLatin1String("Grace"))
                duration = qMin(duration, m_wsClient->sessionRecoveryRemainingMs(binding.remoteSessionId));
        }
        // A late callback after sleep/event-loop starvation cannot grant a
        // fresh grace period beyond the last authenticated contact's budget.
        m_recoveryDisplayDeadlineMs = m_clock() + duration;
        m_recoveryDisplayTimer.start(static_cast<int>(qMax<qint64>(0, duration)));
    }
    m_degraded = degraded;
    refreshAuthenticatedState();
}

bool ConnectionManager::recoveryDisplayActive() const
{
    return m_desiredEnabled && !m_draining && !m_fatalFailure
        && m_recoveryDisplayDeadlineMs > m_clock()
        && m_wsClient->transportRecoveryRemainingMs() > 0;
}

void ConnectionManager::scheduleReconnect()
{
    if (!m_desiredEnabled || m_draining || m_fatalFailure
        || m_retries.contains(QStringLiteral("connect")) || m_attemptInProgress) {
        return; // Already scheduled
    }
    
    bool withinLease = recoveryDisplayActive();
    // Recover promptly even without an open session, and use each retained
    // session's own deadline when deciding whether a quick RESUME is viable.
    for (const auto& binding : m_wsClient->remoteSessionCoordinator()->all()) {
        if ((binding.phase == QLatin1String("Active") || binding.phase == QLatin1String("Grace"))
            && m_wsClient->sessionRecoveryRemainingMs(binding.remoteSessionId) > 0) {
            withinLease = true;
            break;
        }
    }
    if (withinLease != m_wasWithinLease) {
        if (withinLease) m_fastRetryAttempt = 0;
        m_wasWithinLease = withinLease;
    }
    int& counter = withinLease ? m_fastRetryAttempt : m_backgroundRetryAttempt;
    const int attempt = counter;
    counter = RetryPolicy::increment(counter);
    const int delay = retryDelayForAttempt(attempt, withinLease);
    
    qDebug() << "ConnectionManager: Scheduling reconnect attempt" << (attempt + 1)
             << "in" << delay << "ms";
    NetworkDiagnostics::record(QStringLiteral("transport_retry"), {
        {"channel", "control"}, {"delayMs", delay}, {"count", attempt + 1},
        {"reason", withinLease ? "session_recovery" : "background_reconnect"},
        {"remainingMs", m_wsClient->transportRecoveryRemainingMs()}});
    
    setState(State::Disconnected);
    const quint64 cycle = m_transitionId;
    m_retries.schedule(QStringLiteral("connect"), delay, [this, cycle] {
        if (cycle == m_transitionId) attemptReconnect();
    });
    setRetryAction(RetryAction::Scheduled);
}

int ConnectionManager::retryDelayForAttempt(int attempt, bool withinLease)
{
    const auto& config = AppConfig::instance();
    const RetryPolicy policy = withinLease
        ? RetryPolicy{config.reconnectFastStepMs(), config.reconnectFastMaxMs(),
                      config.reconnectJitterPercent(), RetryPolicy::Growth::Linear, true}
        : RetryPolicy{config.reconnectBaseMs(), config.reconnectMaxMs(), config.reconnectJitterPercent()};
    return policy.delay(attempt);
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
    m_retries.cancel(QStringLiteral("connect"));
    m_registrationReady = false;
    m_reconciliationReady = false;
    m_degraded = false;
    setRetryAction(RetryAction::Running);
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

void ConnectionManager::observeStability(bool healthy)
{
    const qint64 observationGap = m_wsClient->serverPolicy()
        .value(QStringLiteral("transportSuspectAfterMs")).toInteger(1500);
    // A long sleep or blocked event loop is not evidence of a healthy interval.
    if (m_stableConnection.transition(healthy, m_clock(),
            AppConfig::instance().reconnectStableResetMs(), observationGap)) {
        m_fastRetryAttempt = 0;
        m_backgroundRetryAttempt = 0;
        emit retryStateChanged();
    }
}

void ConnectionManager::setState(State state)
{
    if (m_state == state) return;
    observeStability(state == State::Connected);
    if (state == State::Connected) {
        m_recoveryDisplayTimer.stop();
        m_recoveryDisplayDeadlineMs = -1;
        m_lastError.clear();
        setRetryAction(RetryAction::Idle);
    }
    if (state == State::Synchronizing) {
        if (!m_syncTimeoutTimer.isActive()) m_syncTimeoutTimer.start(AppConfig::instance().connectionSyncTimeoutMs());
    } else m_syncTimeoutTimer.stop();
    if (state == State::CleanupPending) setRetryAction(RetryAction::Suspended);
    if (state == State::Connecting || state == State::Authenticating || state == State::Synchronizing)
        setRetryAction(RetryAction::Running);
    NetworkDiagnostics::record(QStringLiteral("connection_transition"), {
        {"before", QString::fromLatin1(QMetaEnum::fromType<State>().valueToKey(static_cast<int>(m_state)))},
        {"after", QString::fromLatin1(QMetaEnum::fromType<State>().valueToKey(static_cast<int>(state)))},
        {"remainingMs", m_wsClient->transportRecoveryRemainingMs()}});
    m_state = state;
    emit retryStateChanged();
    emit stateChanged(state);
    emit statusChanged(getConnectionStatus());
}
