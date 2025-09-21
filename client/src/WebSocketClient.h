#ifndef WEBSOCKETCLIENT_H
#define WEBSOCKETCLIENT_H

#include <QObject>
#include <QWebSocket>
#include <QSet>
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
    // Upload channel (secondary socket) management
    bool ensureUploadChannel(); // opens m_uploadSocket if needed (async); returns true if already connected or opening
    void closeUploadChannel();  // closes m_uploadSocket if open
    bool isUploadChannelConnected() const;
    
    // Client registration
    void registerClient(const QString& machineName, const QString& platform, const QList<ScreenInfo>& screens, int volumePercent);
    void requestClientList();
    void requestScreens(const QString& targetClientId);
    void watchScreens(const QString& targetClientId);
    void unwatchScreens(const QString& targetClientId);
    void sendStateSnapshot(const QList<ScreenInfo>& screens, int volumePercent);
        // Send current cursor position (global desktop coordinates) when this client is watched
        void sendCursorUpdate(int globalX, int globalY);

    // Upload/unload protocol (JSON relayed by server)
    void sendUploadStart(const QString& targetClientId, const QJsonArray& filesManifest, const QString& uploadId);
    void sendUploadChunk(const QString& targetClientId, const QString& uploadId, const QString& fileId, int chunkIndex, const QByteArray& dataBase64);
    void sendUploadComplete(const QString& targetClientId, const QString& uploadId);
    void sendUploadAbort(const QString& targetClientId, const QString& uploadId, const QString& reason = QString());
    void sendUnloadMedia(const QString& targetClientId);
    void sendRemoveFile(const QString& targetClientId, const QString& fileId);
    // Target -> Sender notifications
    void notifyUploadProgressToSender(const QString& senderClientId, const QString& uploadId, int percent, int filesCompleted, int totalFiles);
    void notifyUploadFinishedToSender(const QString& senderClientId, const QString& uploadId);
    void notifyUnloadedToSender(const QString& senderClientId);

    // Client-side cancel safeguard: mark an uploadId as cancelled to ignore any further chunk sends
    void cancelUploadId(const QString& uploadId) { m_canceledUploads.insert(uploadId); }
    
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

    // Upload progress signals (from target via server)
    void uploadProgressReceived(const QString& uploadId, int percent, int filesCompleted, int totalFiles);
    void uploadFinishedReceived(const QString& uploadId);
    void unloadedReceived();

private slots:
    void onConnected();
    void onDisconnected();
    void onTextMessageReceived(const QString& message);
    void onUploadTextMessageReceived(const QString& message);
    void onError(QAbstractSocket::SocketError error);
    void attemptReconnect();
    // Upload socket handlers
    void onUploadConnected();
    void onUploadDisconnected();
    void onUploadError(QAbstractSocket::SocketError error);

private:
    void handleMessage(const QJsonObject& message);
    void sendMessage(const QJsonObject& message);
    void sendMessageUpload(const QJsonObject& message);
    void setConnectionStatus(const QString& status);
    QSet<QString> m_canceledUploads; // uploadIds that should drop further chunk sends
    
    QWebSocket* m_webSocket;
    QWebSocket* m_uploadSocket = nullptr;
    QString m_serverUrl;
    QString m_clientId;
    QString m_uploadClientId;
    QString m_connectionStatus;
    QTimer* m_reconnectTimer;
    int m_reconnectAttempts;
    bool m_userInitiatedDisconnect = false;
    static const int MAX_RECONNECT_ATTEMPTS = 5;
    static const int RECONNECT_INTERVAL = 3000; // 3 seconds
};

#endif // WEBSOCKETCLIENT_H
