#ifndef CONNECTIONMANAGER_H
#define CONNECTIONMANAGER_H

#include <QObject>
#include <QTimer>
#include <QString>

class WebSocketClient;
class ClientInfo;

/**
 * @brief Manages WebSocket connection lifecycle and reconnection logic
 * 
 * ConnectionManager encapsulates all connection-related logic including:
 * - Initial connection to server
 * - Disconnection handling
 * - Smart reconnection with exponential backoff
 * - Connection status tracking
 * 
 * This removes ~200 lines of connection logic from MainWindow.
 */
class ConnectionManager : public QObject {
    Q_OBJECT

public:
    /**
     * @brief Construct a ConnectionManager
     * @param wsClient The WebSocketClient instance to manage
     * @param parent Parent QObject for memory management
     */
    explicit ConnectionManager(WebSocketClient* wsClient, QObject* parent = nullptr);
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
    bool isConnected() const;
    
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
     * @return Status string (e.g., "Connected", "Disconnected", "Reconnecting...")
     */
    QString getConnectionStatus() const;

signals:
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
    
    /**
     * @brief Emitted when client registration is confirmed by server
     * @param clientInfo Information about this client from the server
     */
    void registrationConfirmed(const ClientInfo& clientInfo);

private slots:
    void onConnected();
    void onDisconnected();
    void onConnectionError(const QString& error);
    void attemptReconnect();

private:
    void scheduleReconnect();
    int calculateReconnectDelay() const;
    
    WebSocketClient* m_wsClient;
    QTimer* m_reconnectTimer;
    QString m_serverUrl;
    int m_reconnectAttempts;
    int m_maxReconnectDelay;
    bool m_isManualDisconnect;
};

#endif // CONNECTIONMANAGER_H
