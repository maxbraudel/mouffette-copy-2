#ifndef UPLOADMANAGER_H
#define UPLOADMANAGER_H

#include <QObject>
#include <QJsonArray>
#include <QPointer>
#include <QHash>
#include <QFile>
#include <QSet>
#include <QVector>
#include <QTimer>
#include <QUuid>
#include <functional>

class WebSocketClient;
// (graphics scene/item no longer needed here)

struct UploadFileInfo {
    QString fileId;
    QString mediaId; // persistent id of the canvas item
    QString path;
    QString name;
    QString extension; // file extension (e.g., "jpg", "png", "mp4")
    qint64 size = 0;
};

struct IncomingUploadSession {
    QString senderId;
    QString uploadId;
    QString cacheDirPath;
    QHash<QString, QFile*> openFiles;          // fileId -> QFile*
    QHash<QString, qint64> expectedSizes;      // fileId -> total bytes
    QHash<QString, qint64> receivedByFile;     // fileId -> received bytes
    QHash<QString, QString> fileIdToMediaId;   // fileId -> mediaId for target-side naming
    qint64 totalSize = 0;
    qint64 received = 0;
    int totalFiles = 0;
};

// Dedicated component that encapsulates upload/unload logic previously in MainWindow.
// Responsibilities:
//  - Build manifest from scene media items
//  - Stream chunks (sequential for now) and report progress
//  - Handle cancel/abort, unload, and incoming upload assembly
//  - Expose high level signals UI can bind to
//  - Keep WebSocket protocol usage isolated
class UploadManager : public QObject {
    Q_OBJECT
public:
    explicit UploadManager(QObject* parent = nullptr);
    void setWebSocketClient(WebSocketClient* client);
    void setTargetClientId(const QString& id);
    QString targetClientId() const { return m_targetClientId; }
    QString activeUploadTargetClientId() const { return m_uploadTargetClientId; }
    QString lastRemovalClientId() const { return m_lastRemovalClientId; }
    void setActiveSessionIdentity(const QString& identity) { m_activeSessionIdentity = identity; }
    QString activeSessionIdentity() const { return m_activeSessionIdentity; }
    void clearLastRemovalClientId() { m_lastRemovalClientId.clear(); }
    void forceResetForClient(const QString& clientId = QString());

    // Outbound (sender side)
    bool hasActiveUpload() const { return m_uploadActive; }
    bool isUploading() const { return m_uploadInProgress; }
    bool isCancelling() const { return m_cancelRequested; }
    bool isFinalizing() const { return m_finalizing; }
    QString currentUploadId() const { return m_currentUploadId; }

    // Toggle behavior (call from UI):
    //  - if active (already uploaded): unload
    //  - else if uploading: cancel
    //  - else start new upload with provided files
    void toggleUpload(const QVector<UploadFileInfo>& files);
    void requestUnload();
    void requestCancel();
    void requestRemoval(const QString& clientId);

    // Incoming (target side) handling entry point
    void handleIncomingMessage(const QJsonObject& message);

signals:
    void uiStateChanged(); // generic signal to refresh button text/state
    void uploadProgress(int percent, int filesCompleted, int totalFiles); // forwarded from server
    void uploadFinished();
    // New: subset of files confirmed complete by target so far
    void uploadCompletedFileIds(const QStringList& fileIds);
    void allFilesRemoved();
    // New: fine-grained per-file upload lifecycle (sender-side only)
    void fileUploadStarted(const QString& fileId);
    void fileUploadProgress(const QString& fileId, int percent);
    void fileUploadFinished(const QString& fileId);

public slots:
    // Forwarded from WebSocket layer
    void onUploadProgress(const QString& uploadId, int percent, int filesCompleted, int totalFiles);
    void onUploadCompletedFileIds(const QString& uploadId, const QStringList& fileIds);
    void onUploadFinished(const QString& uploadId);
    void onAllFilesRemovedRemote();
    // Handle network connection loss while uploading/finalizing
    void onConnectionLost();

private:
    void startUpload(const QVector<UploadFileInfo>& files);
    void resetToInitial();
    void cleanupIncomingCacheForConnectionLoss();
    void finalizeLocalCancelState();
    void cleanupIncomingSession(bool deleteDiskContents, bool notifySender, const QString& senderOverride = QString(), const QString& cacheDirOverride = QString(), const QString& uploadIdOverride = QString());
    void resetProgressTracking();
    void updateLocalProgress(int percent, int filesCompleted);
    void updateRemoteProgress(int percent, int filesCompleted);
    void emitEffectiveProgressIfChanged();
    void updatePerFileLocalProgress(const QString& fileId, int percent);
    void updatePerFileRemoteProgress(const QString& fileId, int percent);
    void emitEffectivePerFileProgress(const QString& fileId);
    bool canAcceptNewAction() const;
    void scheduleActionDebounce();

    QPointer<WebSocketClient> m_ws;
    QString m_targetClientId;
    // Captured at startUpload to remain stable across the whole transfer
    QString m_uploadTargetClientId;
    QString m_activeSessionIdentity;

    // Sender side state
    bool m_uploadActive = false;      // true after remote finished (acts as toggle to unload)
    bool m_uploadInProgress = false;  // true while streaming chunks
    bool m_cancelRequested = false;   // user pressed cancel mid-stream
    bool m_finalizing = false;        // true after all bytes sent, awaiting server ack
    bool m_cancelFinalizePending = false; // true while local cancellation cleanup is outstanding
    QString m_currentUploadId;        // uuid
    int m_lastPercent = 0;
    int m_filesCompleted = 0;
    int m_totalFiles = 0;
    QTimer* m_cancelFallbackTimer = nullptr; // fires if remote never responds to abort/unload
    // Sender-side byte tracking for accurate weighted progress
    qint64 m_totalBytes = 0;
    qint64 m_sentBytes = 0;
    // Prefer remote (target-reported) progress when available to avoid early 100%
    bool m_remoteProgressReceived = false;
    int m_lastLocalPercent = 0;
    int m_lastLocalFilesCompleted = 0;
    int m_lastRemotePercent = 0;
    int m_lastRemoteFilesCompleted = 0;
    int m_effectivePercent = -1;
    int m_effectiveFilesCompleted = -1;

    // Sender-side per-file tracking
    QVector<UploadFileInfo> m_outgoingFiles;
    QHash<QString, int> m_localFilePercents;
    QHash<QString, int> m_remoteFilePercents;
    QHash<QString, int> m_effectiveFilePercents;

    QString m_lastRemovalClientId;

    // Incoming session (target side)
    IncomingUploadSession m_incoming;
    QSet<QString> m_canceledIncoming; // uploadIds canceled by sender
    // Track next expected chunk index per (uploadId:fileId) on the target side
    QHash<QString, int> m_expectedChunkIndex; 

    // Anti-spam protection
    QTimer* m_actionDebounceTimer = nullptr;
    QElapsedTimer m_lastActionTime;
    bool m_actionInProgress = false;
    static constexpr int ACTION_DEBOUNCE_MS = 500;
    static constexpr int MIN_ACTION_INTERVAL_MS = 300;
};

#endif // UPLOADMANAGER_H
