#ifndef CONNECTIONMANAGER_H
#define CONNECTIONMANAGER_H

#include <QObject>
#include <QTimer>
#include <QString>
#include "backend/network/RetryScheduler.h"
#include "backend/network/RetryPolicy.h"
#include "backend/runtime/SuspendInclusiveClock.h"

class WebSocketClient;
class ClientInfo;

/**
 * @brief Manages WebSocket connection lifecycle and reconnection logic
 * 
 * ConnectionManager encapsulates all connection-related logic including:
 * - Initial connection to server
 * - Disconnection handling
 * - Lease-aware fast reconnection followed by indefinite capped backoff
 * - Connection status tracking
 * 
 * This removes ~200 lines of connection logic from ApplicationRuntime.
 */
class ConnectionManager : public QObject {
    Q_OBJECT

public:
    enum class State { Disconnected, Disconnecting, Connecting, Authenticating, Synchronizing,
                       Connected, Degraded, CleanupPending, Failed };
    Q_ENUM(State)
    State state() const { return m_state; }
    bool connectionEnabled() const { return m_desiredEnabled; }
    bool disconnectInProgress() const { return m_draining; }
    quint64 transitionId() const { return m_transitionId; }
    void setConnectionEnabled(bool enabled);
    void completeDisconnect(quint64 transitionId);
    void reconfigureServer(const QString& serverUrl);
    void setReceiverReady(bool ready);
    /**
     * @brief Construct a ConnectionManager
     * @param wsClient The WebSocketClient instance to manage
     * @param parent Parent QObject for memory management
     */
    explicit ConnectionManager(WebSocketClient* wsClient, QObject* parent = nullptr,
        RetryScheduler::Clock clock = [] { return MouffetteClock::nowMs(); });
    ~ConnectionManager() override = default;

    /**
     * @brief Connect to the specified WebSocket server
     * @param serverUrl The WebSocket URL (e.g., "ws://192.168.0.188:8080")
     */
    void connectToServer(const QString& serverUrl);
    
    /**
     * @brief Disconnect from the server
     */
    void disconnect();
    
    /**
     * @brief Check if currently connected to the server
     * @return true if connected, false otherwise
     */
    bool isConnected() const; // compatibility: authenticated transport
    bool isTransportAuthenticated() const;
    bool isReady() const { return m_state == State::Connected; }
    RetryAction retryAction() const { return m_retryAction; }
    int retryAttemptCount() const { return m_wasWithinLease ? m_fastRetryAttempt : m_backgroundRetryAttempt; }
    qint64 nextAttemptAtMs() const { return m_retries.dueAt(QStringLiteral("connect")); }
    QString connectionDetail() const;
    
    /**
     * @brief Get the current server URL
     * @return The configured server URL
     */
    QString getServerUrl() const { return m_serverUrl; }
    
    /**
     * @brief Set the server URL (does not trigger connection)
     * @param url The new server URL
     */
    void setServerUrl(const QString& url);
    
    /**
     * @brief Get the current connection status string
     * @return Status string (e.g., "Connected", "Disconnected", "Connecting")
     */
    QString getConnectionStatus() const;

    /** Pure retry policy helper, exposed to make timing boundaries testable. */
    static int retryDelayForAttempt(int attempt, bool withinLease);

signals:
    void retryStateChanged();
    void stateChanged(ConnectionManager::State state);
    void connectionEnabledChanged(bool enabled);
    void disconnectRequested(quint64 transitionId);
    /**
     * @brief Emitted when successfully connected to the server
     */
    void connected();
    
    /**
     * @brief Emitted when disconnected from the server
     */
    void disconnected();
    
    /**
     * @brief Emitted when a connection error occurs
     * @param error Error message describing what went wrong
     */
    void connectionError(const QString& error);
    
    /**
     * @brief Emitted when connection status changes
     * @param status New status string
     */
    void statusChanged(const QString& status);
    void leaseExpired(const QString& serverBootId, quint64 connectionGeneration);
    
    /**
     * @brief Emitted when client registration is confirmed by server
     * @param clientInfo Information about this client from the server
     */
    void registrationConfirmed(const ClientInfo& clientInfo);

private slots:
    void onConnected();
    void onDisconnected();
    void onConnectionError(const QString& error);
    void onFatalError(const QString& error);
    void onLeaseExpired(const QString& serverBootId, quint64 connectionGeneration);
    void onTransportHealthChanged(bool degraded);
    void attemptReconnect();
    void onAttemptTimedOut();

private:
    void scheduleReconnect();
    void beginAttempt();
    void setState(State state);
    void suspendAttempts();
    void refreshAuthenticatedState();
    void observeStability(bool healthy);
    
    WebSocketClient* m_wsClient;
    RetryScheduler::Clock m_clock;
    RetryScheduler m_retries;
    QTimer m_syncTimeoutTimer;
    RetryAction m_retryAction = RetryAction::Idle;
    QString m_lastError;
    void setRetryAction(RetryAction action);
    QTimer* m_attemptTimeoutTimer;
    QString m_serverUrl;
    State m_state = State::Disconnected;
    bool m_desiredEnabled = true;
    bool m_draining = false;
    bool m_receiverReady = true;
    bool m_registrationReady = false;
    bool m_reconciliationReady = false;
    bool m_degraded = false;
    quint64 m_transitionId = 0;
    StableConnectionWindow m_stableConnection;
    int m_fastRetryAttempt = 0;
    int m_backgroundRetryAttempt = 0;
    bool m_wasWithinLease = false;
    bool m_attemptInProgress = false;
    bool m_fatalFailure = false;
};

#endif // CONNECTIONMANAGER_H
