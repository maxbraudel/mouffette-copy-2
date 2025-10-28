#ifndef WEBSOCKETCLIENT_H
#define WEBSOCKETCLIENT_H

#include <QObject>
#include <QWebSocket>
#include <QSet>
#include <QHash>
#include <QJsonObject>
#include <QJsonDocument>
#include <QTimer>
#include <QJsonArray>
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
    bool prepareUploadChannel(int timeoutMs = 1500);
    void beginUploadSession(bool preferUploadChannel);
    void endUploadSession();
    
    // Client registration
    void registerClient(const QString& machineName, const QString& platform, const QList<ScreenInfo>& screens, int volumePercent);
    void requestScreens(const QString& targetClientId);
    void watchScreens(const QString& targetClientId);
    void unwatchScreens(const QString& targetClientId);
    void sendStateSnapshot(const QList<ScreenInfo>& screens, int volumePercent);
        // Send current cursor position (global desktop coordinates) when this client is watched
        void sendCursorUpdate(int globalX, int globalY);

    // Upload/unload protocol (JSON relayed by server)
    void sendUploadStart(const QString& targetClientId, const QJsonArray& filesManifest, const QString& uploadId, const QString& ideaId);
    void sendUploadChunk(const QString& targetClientId, const QString& uploadId, const QString& fileId, int chunkIndex, const QByteArray& dataBase64, const QString& ideaId);
    void sendUploadComplete(const QString& targetClientId, const QString& uploadId, const QString& ideaId);
    void sendUploadAbort(const QString& targetClientId, const QString& uploadId, const QString& reason, const QString& ideaId);
    void sendRemoveAllFiles(const QString& targetClientId, const QString& ideaId);
    void sendRemoveFile(const QString& targetClientId, const QString& ideaId, const QString& fileId);
    // Target -> Sender notifications
    void notifyUploadProgressToSender(const QString& senderClientId, const QString& uploadId, int percent, int filesCompleted, int totalFiles, const QStringList& completedFileIds = QStringList(), const QJsonArray& perFileProgress = QJsonArray());
    void notifyUploadFinishedToSender(const QString& senderClientId, const QString& uploadId);
    void notifyAllFilesRemovedToSender(const QString& senderClientId);

    // Remote scene control
    void sendRemoteSceneStart(const QString& targetClientId, const QJsonObject& scenePayload);
    void sendRemoteSceneStop(const QString& targetClientId);
    void sendRemoteSceneStopResult(const QString& senderClientId, bool success, const QString& errorMessage = QString());
    // Remote scene validation feedback
    void sendRemoteSceneValidationResult(const QString& senderClientId, bool success, const QString& errorMessage = QString());
    void sendRemoteSceneLaunched(const QString& senderClientId);

    // Client-side cancel safeguard: mark an uploadId as cancelled to ignore any further chunk sends
    void cancelUploadId(const QString& uploadId) { m_canceledUploads.insert(uploadId); }
    
    // Getters
    QString getClientId() const { return m_clientId; }
    QString getSessionId() const { return m_sessionId; }
    QString getPersistentClientId() const { return m_persistentClientId; }
    QString getConnectionStatus() const { return m_connectionStatus; }
    
    // Set persistent client ID (called once at startup after loading from settings)
    void setPersistentClientId(const QString& id) { m_persistentClientId = id; }

signals:
    void connected();
    void disconnected();
    void connectionError(const QString& error);
    void connectionStatusChanged(const QString& status); // emitted whenever textual connection status updates
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
    // New: carries fileIds that the target reports as fully received so far
    void uploadCompletedFileIdsReceived(const QString& uploadId, const QStringList& fileIds);
    // New: fine-grained per-file percent from target
    void uploadPerFileProgressReceived(const QString& uploadId, const QHash<QString,int>& filePercents);
    void uploadFinishedReceived(const QString& uploadId);
    void allFilesRemovedReceived();
    // Remote scene inbound events
    void remoteSceneStartReceived(const QString& senderClientId, const QJsonObject& scenePayload);
    void remoteSceneStopReceived(const QString& senderClientId);
    void remoteSceneStoppedReceived(const QString& targetClientId, bool success, const QString& errorMessage);
    // Remote scene validation feedback events
    void remoteSceneValidationReceived(const QString& targetClientId, bool success, const QString& errorMessage);
    void remoteSceneLaunchedReceived(const QString& targetClientId);

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
    QString m_clientId;           // Current session ID assigned/confirmed by server
    QString m_persistentClientId; // Stable ID generated by client, persisted across sessions
    QString m_uploadClientId;
    QString m_sessionId;          // Locally generated session identifier (sent during registration)
    QString m_socketClientId;     // Raw socket ID provided by welcome message (diagnostics)
    QString m_connectionStatus;
    QTimer* m_reconnectTimer;
    int m_reconnectAttempts;
    bool m_userInitiatedDisconnect = false;
    bool m_uploadSessionActive = false;
    bool m_useUploadSocketForSession = false;
    static const int MAX_RECONNECT_ATTEMPTS = 5;
    static const int RECONNECT_INTERVAL = 3000; // 3 seconds
};

#endif // WEBSOCKETCLIENT_H
