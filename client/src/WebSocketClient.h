#ifndef WEBSOCKETCLIENT_H
#define WEBSOCKETCLIENT_H

#include <QObject>
#include <QWebSocket>
#include <QJsonObject>
#include <QJsonDocument>
#include <QTimer>
#include "ClientInfo.h"

class WebSocketClient : public QObject {
    Q_OBJECT

public:
    explicit WebSocketClient(QObject *parent = nullptr);
    ~WebSocketClient();
    
    // Connection management
    void connectToServer(const QString& serverUrl);
    void disconnect();
    bool isConnected() const;
    
    // Client registration
    void registerClient(const QString& machineName, const QString& platform, const QList<ScreenInfo>& screens, int volumePercent);
    void requestClientList();
    void requestScreens(const QString& targetClientId);
    void watchScreens(const QString& targetClientId);
    void unwatchScreens(const QString& targetClientId);
    void sendStateSnapshot(const QList<ScreenInfo>& screens, int volumePercent);
        // Send current cursor position (global desktop coordinates) when this client is watched
        void sendCursorUpdate(int globalX, int globalY);
    
    // Getters
    QString getClientId() const { return m_clientId; }
    QString getConnectionStatus() const { return m_connectionStatus; }

signals:
    void connected();
    void disconnected();
    void connectionError(const QString& error);
    void clientListReceived(const QList<ClientInfo>& clients);
    void registrationConfirmed(const ClientInfo& clientInfo);
    void screensInfoReceived(const ClientInfo& clientInfo);
    void messageReceived(const QJsonObject& message);
    void watchStatusChanged(bool watched);
    void dataRequestReceived();
        // Emitted to watchers with remote cursor position of the watched target
        void cursorPositionReceived(const QString& targetClientId, int x, int y);

private slots:
    void onConnected();
    void onDisconnected();
    void onTextMessageReceived(const QString& message);
    void onError(QAbstractSocket::SocketError error);
    void attemptReconnect();

private:
    void handleMessage(const QJsonObject& message);
    void sendMessage(const QJsonObject& message);
    void setConnectionStatus(const QString& status);
    
    QWebSocket* m_webSocket;
    QString m_serverUrl;
    QString m_clientId;
    QString m_connectionStatus;
    QTimer* m_reconnectTimer;
    int m_reconnectAttempts;
    bool m_userInitiatedDisconnect = false;
    static const int MAX_RECONNECT_ATTEMPTS = 5;
    static const int RECONNECT_INTERVAL = 3000; // 3 seconds
};

#endif // WEBSOCKETCLIENT_H
