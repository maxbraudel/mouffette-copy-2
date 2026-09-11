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
#include "backend/domain/models/ClientInfo.h"

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
    // Pins one already-connected transport for the whole transfer. This never
    // spins a nested event loop: if the dedicated channel is not ready yet, the
    // authenticated control channel is selected immediately and the dedicated
    // channel continues warming up for a later transfer.
    bool beginUploadSession(bool preferUploadChannel);
    void endUploadSession();
    qint64 uploadTransportBytesToWrite() const;
    bool isUploadSessionTransportAvailable() const;
    bool isUploadSessionUsingDedicatedChannel() const {
        return m_uploadSessionActive && m_useUploadSocketForSession;
    }
    
    // Client registration
    void registerClient(const QString& machineName, const QString& platform, const QList<ScreenInfo>& screens, int volumePercent);
    void requestScreens(const QString& targetClientId);
    void watchScreens(const QString& targetClientId);
    void unwatchScreens(const QString& targetClientId);
    void sendStateSnapshot(const QList<ScreenInfo>& screens, int volumePercent);
        // Send current cursor position when this client is watched.
        // Optional screenId/normalized values provide DPI-agnostic mapping fidelity.
        void sendCursorUpdate(int globalX, int globalY, int screenId = -1, qreal normalizedX = -1.0, qreal normalizedY = -1.0);

    // Upload/unload protocol (JSON relayed by server)
    bool sendUploadStart(const QString& targetClientId, const QJsonArray& filesManifest, const QString& uploadId, const QString& canvasSessionId);
    bool sendUploadChunk(const QString& targetClientId, const QString& uploadId, const QString& fileId, int chunkIndex, const QByteArray& dataBase64, const QString& canvasSessionId);
    bool sendUploadComplete(const QString& targetClientId, const QString& uploadId, const QString& canvasSessionId);
    bool sendUploadAbort(const QString& targetClientId, const QString& uploadId, const QString& reason, const QString& canvasSessionId);
    bool sendRemoveAllFiles(const QString& targetClientId,
                            const QString& canvasSessionId,
                            const QString& removalId);
    void sendRemoveFile(const QString& targetClientId, const QString& canvasSessionId, const QString& fileId);
    
    // PHASE 2: Canvas lifecycle notifications (CRITICAL for canvasSessionId validation)
    void sendCanvasCreated(const QString& persistentClientId, const QString& canvasSessionId);
    void sendCanvasDeleted(const QString& persistentClientId, const QString& canvasSessionId);
    
    // Target -> Sender notifications
    void notifyUploadReadyToSender(const QString& senderClientId,
                                   const QString& uploadId,
                                   const QString& canvasSessionId);
    void notifyUploadProgressToSender(const QString& senderClientId,
                                      const QString& uploadId,
                                      int percent,
                                      int filesCompleted,
                                      int totalFiles,
                                      qint64 receivedBytes,
                                      const QStringList& completedFileIds = QStringList(),
                                      const QJsonArray& perFileProgress = QJsonArray());
    void notifyUploadFinishedToSender(const QString& senderClientId,
                                      const QString& uploadId,
                                      const QString& canvasSessionId,
                                      const QStringList& validatedFileIds);
    void notifyUploadRejectedToSender(const QString& senderClientId,
                                      const QString& uploadId,
                                      const QString& reason,
                                      const QString& canvasSessionId = QString());
    void notifyUploadAbortAcknowledgedToSender(const QString& senderClientId,
                                               const QString& uploadId,
                                               const QString& canvasSessionId);
    void notifyAllFilesRemovedToSender(const QString& senderClientId,
                                       const QString& removalId,
                                       const QString& canvasSessionId);
    void notifyAllFilesRemovalFailedToSender(const QString& senderClientId,
                                             const QString& removalId,
                                             const QString& canvasSessionId,
                                             const QString& reason);

    // Remote scene control
    void sendRemoteSceneStart(const QString& targetClientId, const QJsonObject& scenePayload);
    void sendRemoteSceneActivate(const QString& targetClientId,
                                 const QString& sceneInstanceId,
                                 qint64 activationEpochMs,
                                 int activationDelayMs);
    void sendRemoteSceneVideoSync(const QString& targetClientId,
                                  const QString& sceneInstanceId,
                                  qint64 sequence,
                                  qint64 sampledEpochMs,
                                  const QJsonArray& videos);
    // An empty sceneInstanceId is retained only for legacy/global cleanup callers.
    // Normal scene lifecycle commands must always provide the run identifier.
    void sendRemoteSceneStop(const QString& targetClientId,
                             const QString& sceneInstanceId = QString());
    void sendRemoteSceneStopResult(const QString& senderClientId,
                                   const QString& sceneInstanceId,
                                   bool success,
                                   const QString& errorMessage = QString());
    // Remote scene validation feedback
    void sendRemoteSceneValidationResult(const QString& senderClientId,
                                         const QString& sceneInstanceId,
                                         bool success,
                                         const QString& errorMessage = QString());
    void sendRemoteSceneLaunched(const QString& senderClientId, const QString& sceneInstanceId);

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
    void fatalError(const QString& error); // PHASE 1: Non-recoverable errors (e.g., SSL handshake failure)
    void connectionStatusChanged(const QString& status); // emitted whenever textual connection status updates
    void clientListReceived(const QList<ClientInfo>& clients);
    void registrationConfirmed(const ClientInfo& clientInfo);
    void screensInfoReceived(const ClientInfo& clientInfo);
    void messageReceived(const QJsonObject& message);
    void watchStatusChanged(bool watched);
    void dataRequestReceived();
        // Emitted to watchers with remote cursor position of the watched target.
        // screenId/normalized values may be absent (-1).
        void cursorPositionReceived(const QString& targetClientId, int x, int y, int screenId, qreal normalizedX, qreal normalizedY);

    // Upload progress signals (from target via server)
    void uploadReadyReceived(const QString& uploadId, const QString& canvasSessionId);
    void uploadProgressReceived(const QString& uploadId, int percent, int filesCompleted, int totalFiles);
    void uploadBytesAcknowledgedReceived(const QString& uploadId, qint64 receivedBytes);
    // New: carries fileIds that the target reports as fully received so far
    void uploadCompletedFileIdsReceived(const QString& uploadId, const QStringList& fileIds);
    // New: fine-grained per-file percent from target
    void uploadPerFileProgressReceived(const QString& uploadId, const QHash<QString,int>& filePercents);
    void uploadFinishedReceived(const QString& uploadId);
    void uploadRejectedReceived(const QString& uploadId, const QString& reason);
    void uploadAbortedReceived(const QString& uploadId, const QString& canvasSessionId);
    void uploadTransportBytesWritten(qint64 bytes);
    void uploadTransportLost(const QString& reason);
    void allFilesRemovedReceived(const QString& removalId,
                                 const QString& targetClientId,
                                 const QString& canvasSessionId);
    void removalRejectedReceived(const QString& removalId, const QString& reason);
    // Remote scene inbound events
    void remoteSceneStartReceived(const QString& senderClientId, const QJsonObject& scenePayload);
    void remoteSceneActivateReceived(const QString& senderClientId,
                                     const QString& sceneInstanceId,
                                     qint64 activationEpochMs,
                                     int activationDelayMs);
    void remoteSceneVideoSyncReceived(const QString& senderClientId,
                                      const QString& sceneInstanceId,
                                      qint64 sequence,
                                      qint64 sampledEpochMs,
                                      const QJsonArray& videos);
    void remoteSceneStopReceived(const QString& senderClientId,
                                 const QString& sceneInstanceId);
    void remoteSceneStoppedReceived(const QString& targetClientId,
                                    const QString& sceneInstanceId,
                                    bool success,
                                    const QString& errorMessage);
    // Remote scene validation feedback events
    void remoteSceneValidationReceived(const QString& targetClientId,
                                       const QString& sceneInstanceId,
                                       bool success,
                                       const QString& errorMessage);
    void remoteSceneLaunchedReceived(const QString& targetClientId, const QString& sceneInstanceId);

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
    bool sendControlMessage(const QJsonObject& message);
    bool sendMessageUpload(const QJsonObject& message);
    void reportSelectedUploadTransportLost(const QString& reason);
    void setConnectionStatus(const QString& status);
    QSet<QString> m_canceledUploads; // uploadIds that should drop further chunk sends
    
    QWebSocket* m_webSocket;
    QWebSocket* m_uploadSocket = nullptr;
    QString m_serverUrl;
    QString m_clientId;           // Current session ID assigned/confirmed by server
    QString m_persistentClientId; // Stable ID generated by client, persisted across sessions
    QString m_uploadClientId;
    QString m_uploadChannelToken;
    QString m_sessionId;          // Locally generated session identifier (sent during registration)
    QString m_socketClientId;     // Raw socket ID provided by welcome message (diagnostics)
    QString m_connectionStatus;
    QTimer* m_reconnectTimer;
    int m_reconnectAttempts;
    bool m_userInitiatedDisconnect = false;
    bool m_uploadSessionActive = false;
    bool m_useUploadSocketForSession = false;
    bool m_uploadTransportLossReported = false;
    bool m_uploadChannelTokenRequested = false;
    bool m_uploadChannelAuthenticated = false;
    static const int MAX_RECONNECT_ATTEMPTS = 5;
    static const int RECONNECT_INTERVAL = 3000; // 3 seconds
};

#endif // WEBSOCKETCLIENT_H
