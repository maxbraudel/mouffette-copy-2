#ifndef UPLOADMANAGER_H
#define UPLOADMANAGER_H

#include <QObject>
#include <QJsonArray>
#include <QPointer>
#include <QHash>
#include <QFile>
#include <QElapsedTimer>
#include <QSet>
#include <QVector>
#include <QTimer>
#include <QUuid>
#include <functional>

class WebSocketClient;
class FileManager;
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
    QString canvasSessionId;
    QString cacheDirPath;
    QHash<QString, QFile*> openFiles;          // fileId -> QFile*
    QHash<QString, qint64> expectedSizes;      // fileId -> total bytes
    QHash<QString, qint64> receivedByFile;     // fileId -> received bytes
    QHash<QString, QString> filePaths;         // fileId -> upload-owned staging path
    QHash<QString, QString> fileIdToName;      // fileId -> display name from manifest
    QHash<QString, QString> fileIdToMediaId;   // fileId -> mediaId for target-side naming
    QHash<QString, QString> fileIdToExtension; // fileId -> original file extension
    qint64 totalSize = 0;
    qint64 received = 0;
    qint64 lastProgressBytesReported = 0;
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
    enum class OutgoingState {
        Idle,
        AwaitingTargetReady,
        Streaming,
        AwaitingValidation,
        Cancelling
    };
    Q_ENUM(OutgoingState)

    explicit UploadManager(FileManager* fileManager, QObject* parent = nullptr);
    void setWebSocketClient(WebSocketClient* client);
    void setTargetClientId(const QString& id);
    QString targetClientId() const { return m_targetClientId; }
    QString activeUploadTargetClientId() const { return m_uploadTargetClientId; }
    QString lastRemovalClientId() const { return m_lastRemovalClientId; }
    void setActiveIdeaId(const QString& canvasSessionId) { m_activeIdeaId = canvasSessionId; }
    void setActiveSessionIdentity(const QString& identity) { m_activeSessionIdentity = identity; }
    QString activeSessionIdentity() const { return m_activeSessionIdentity; }
    void clearLastRemovalClientId() { m_lastRemovalClientId.clear(); }
    void forceResetForClient(const QString& clientId = QString());
    
    // Set local client ID for generating directional session IDs
    void setMyClientId(const QString& myClientId) { m_myClientId = myClientId; }

    // Outbound (sender side)
    bool hasActiveUpload() const { return m_uploadActive; }
    bool isUploading() const {
        return m_outgoingState == OutgoingState::AwaitingTargetReady
            || m_outgoingState == OutgoingState::Streaming;
    }
    bool isCancelling() const { return m_outgoingState == OutgoingState::Cancelling; }
    bool isFinalizing() const { return m_outgoingState == OutgoingState::AwaitingValidation; }
    bool isRemoving() const { return !m_pendingRemovalId.isEmpty(); }
    bool isBusy() const { return m_outgoingState != OutgoingState::Idle || isRemoving(); }
    bool canRequestCancel() const;
    OutgoingState outgoingState() const { return m_outgoingState; }
    QString currentUploadId() const { return m_currentUploadId; }

    // Starts a new/incremental upload, or unloads when already synchronized and
    // no new files are supplied. Cancellation is an explicit guarded action.
    bool toggleUpload(const QVector<UploadFileInfo>& files);
    bool requestUnload();
    void requestCancel();
    bool requestRemoval(const QString& clientId);

    // Incoming (target side) handling entry point
    void handleIncomingMessage(const QJsonObject& message);

signals:
    void uiStateChanged(); // generic signal to refresh button text/state
    void uploadProgress(int percent, int filesCompleted, int totalFiles); // forwarded from server
    void uploadFinished();
    void uploadCancelled(const QString& uploadId);
    void uploadRejected(const QString& uploadId, const QString& reason);
    void removalFailed(const QString& reason);
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
    void onUploadReady(const QString& uploadId, const QString& canvasSessionId);
    void onUploadBytesAcknowledged(const QString& uploadId, qint64 receivedBytes);
    void onUploadCompletedFileIds(const QString& uploadId, const QStringList& fileIds);
    void onUploadFinished(const QString& uploadId);
    void onUploadRejected(const QString& uploadId, const QString& reason);
    void onUploadAborted(const QString& uploadId, const QString& canvasSessionId);
    void onAllFilesRemovedRemote(const QString& removalId,
                                 const QString& targetClientId,
                                 const QString& canvasSessionId);
    void onRemovalRejected(const QString& removalId, const QString& reason);
    // Handle network connection loss while uploading/finalizing
    void onConnectionLost();

private:
    void startUpload(const QVector<UploadFileInfo>& files);
    void setOutgoingState(OutgoingState state);
    void resetToInitial();
    void cleanupOrphanedIncomingCache();
    void cleanupIncomingCacheForConnectionLoss();
    bool discardActiveIncomingSession(bool rememberRejectedUpload);
    bool removeResidualIncomingStaging(const QString& senderId,
                                       const QString& uploadId);
    void rejectIncomingUpload(const QString& senderId,
                              const QString& uploadId,
                              const QString& reason,
                              bool discardMatchingSession);
    void clearIncomingChunkTracking(const QString& uploadId);
    bool cleanupIncomingSession(bool deleteDiskContents,
                                bool notifySender,
                                const QString& senderOverride = QString(),
                                const QString& cacheDirOverride = QString(),
                                const QString& uploadIdOverride = QString(),
                                const QString& ideaOverride = QString());
    void resetProgressTracking();
    void scheduleOutgoingPump();
    void pumpOutgoingUpload();
    void stopOutgoingPump();
    void failOutgoingUpload(const QString& reason);
    void finishLocalCancellation();
    void updateLocalProgress(int percent, int filesCompleted);
    void updateRemoteProgress(int percent, int filesCompleted);
    void emitEffectiveProgressIfChanged();
    void updatePerFileLocalProgress(const QString& fileId, int percent);
    void updatePerFileRemoteProgress(const QString& fileId, int percent);
    void emitEffectivePerFileProgress(const QString& fileId);
    bool canAcceptNewAction() const;
    void recordAcceptedAction();
    void restartIncomingStallTimer();

    QPointer<WebSocketClient> m_ws;
    QString m_targetClientId;
    // Captured at startUpload to remain stable across the whole transfer
    QString m_uploadTargetClientId;
    QString m_activeSessionIdentity;
    QString m_activeIdeaId;

    // Sender side state
    bool m_uploadActive = false;      // true after remote finished (acts as toggle to unload)
    OutgoingState m_outgoingState = OutgoingState::Idle;
    bool m_uploadWasActiveBeforeStart = false; // preserve earlier synchronized files on incremental failure
    QString m_currentUploadId;        // uuid
    int m_lastPercent = 0;
    int m_filesCompleted = 0;
    int m_totalFiles = 0;
    QTimer* m_cancelFallbackTimer = nullptr; // fires if remote never responds to abort/unload
    QTimer* m_removalAckTimer = nullptr;
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
    QFile m_outgoingFileHandle;
    int m_outgoingFileIndex = 0;
    int m_outgoingChunkIndex = 0;
    qint64 m_outgoingSentForFile = 0;
    QTimer* m_outgoingPumpTimer = nullptr;
    QTimer* m_outgoingStallTimer = nullptr;
    QTimer* m_outgoingStartAckTimer = nullptr;
    QTimer* m_outgoingAckTimer = nullptr;
    bool m_outgoingPumpRunning = false;
    bool m_outgoingPayloadCompleteSent = false;
    qint64 m_remoteAcknowledgedBytes = 0;
    QMetaObject::Connection m_uploadBytesWrittenConnection;
    QMetaObject::Connection m_uploadTransportLostConnection;
    QHash<QString, int> m_localFilePercents;
    QHash<QString, int> m_remoteFilePercents;
    QHash<QString, int> m_effectiveFilePercents;

    QString m_lastRemovalClientId;
    QString m_pendingRemovalId;
    QString m_pendingRemovalTargetId;
    QString m_pendingRemovalCanvasSessionId;

    // Phase 4.3: FileManager injected (not singleton)
    FileManager* m_fileManager = nullptr;

    // Incoming session (target side)
    IncomingUploadSession m_incoming;
    QTimer* m_incomingStallTimer = nullptr;
    QSet<QString> m_canceledIncoming; // uploadIds canceled by sender
    // Track next expected chunk index per (uploadId:fileId) on the target side
    QHash<QString, int> m_expectedChunkIndex;
    
    // Local client ID for directional session generation
    QString m_myClientId; 

    // Anti-spam protection. State, rather than a short-lived boolean lock, is
    // authoritative; timing only filters accidental double-clicks.
    QElapsedTimer m_lastAcceptedAction;
    QElapsedTimer m_outgoingStateAge;
    static constexpr int MIN_ACTION_INTERVAL_MS = 300;
    static constexpr int CANCEL_GUARD_MS = 1000;
};

#endif // UPLOADMANAGER_H
