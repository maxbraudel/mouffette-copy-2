#ifndef WEBSOCKETMESSAGEHANDLER_H
#define WEBSOCKETMESSAGEHANDLER_H

#include <QObject>
#include <QJsonObject>

class MainWindow;
class WebSocketClient;

/**
 * @brief WebSocketMessageHandler manages WebSocket connection lifecycle and message routing
 * 
 * Responsibilities:
 * - Handle connection/disconnection events
 * - Route incoming messages to appropriate handlers
 * - Manage state synchronization with server
 * - Coordinate reconnection logic
 * 
 * This handler extracts WebSocket event processing logic from MainWindow
 */
class WebSocketMessageHandler : public QObject
{
    Q_OBJECT

public:
    explicit WebSocketMessageHandler(MainWindow* mainWindow, QObject* parent = nullptr);
    ~WebSocketMessageHandler() override = default;

    /**
     * @brief Setup connections to WebSocket client
     * @param client The WebSocket client to connect to
     */
    void setupConnections(WebSocketClient* client);

public slots:
    /**
     * @brief Handle successful connection to server
     */
    void onConnected();

    /**
     * @brief Handle disconnection from server
     */
    void onDisconnected();

    /**
     * @brief Handle state synchronization message from server
     * @param message JSON message containing state sync data
     */
    void handleStateSyncMessage(const QJsonObject& message);

signals:
    /**
     * @brief Signal emitted when connection state changes
     * @param connected true if connected, false if disconnected
     */
    void connectionStateChanged(bool connected);

private:
    MainWindow* m_mainWindow = nullptr;
};

#endif // WEBSOCKETMESSAGEHANDLER_H
