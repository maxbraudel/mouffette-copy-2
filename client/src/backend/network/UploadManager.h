#ifndef UPLOADMANAGER_H
#define UPLOADMANAGER_H

#include "backend/network/RetryScheduler.h"

#include "backend/network/RemoteCacheStore.h"
#include "backend/network/UploadScheduler.h"
#include "backend/runtime/SuspendInclusiveClock.h"

#include <QObject>
#include <QJsonArray>
#include <QPointer>
#include <QHash>
#include <QFile>
#include <QElapsedTimer>
#include <QSet>
#include <QStringList>
#include <QVector>
#include <QTimer>
#include <QThreadPool>
#include <QUuid>
#include <functional>
#include <atomic>
#include <memory>

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
    QString remoteSessionId;
    quint64 generation = 0;                 // RemoteSession generation
    quint64 sourceConnectionGeneration = 0; // authenticated sender transport
    QString uploadId;
    QString cacheDirPath;
    QHash<QString, QFile*> openFiles;          // assetId -> QFile*
    QHash<QString, qint64> expectedSizes;      // assetId -> total bytes
    QHash<QString, qint64> receivedByFile;     // assetId -> durable contiguous bytes
    QHash<QString, QString> filePaths;         // assetId -> upload-owned staging path
    QHash<QString, QString> assetIdToFileId;   // assetId -> SHA-256/fileId
    QHash<QString, QString> assetIdToSha256;   // assetId -> immutable digest
    QHash<QString, QString> assetIdToName;     // assetId -> display name from manifest
    QHash<QString, QStringList> assetIdToMediaIds;
    QHash<QString, QString> assetIdToExtension;
    qint64 totalSize = 0;
    qint64 received = 0;
    QHash<QString, qint64> queuedByFile; // accepted bytes, not yet durably acknowledged
    std::shared_ptr<std::atomic_bool> writeCancelled;
    quint64 writeEpoch = 0;
    QJsonObject deferredResume;
    qint64 lastProgressBytesReported = 0;
    int totalFiles = 0;
    bool suspendedForResume = false;
    std::shared_ptr<std::atomic_bool> validationCancelled;
    quint64 completionValidationEpoch = 0;
    bool completionValidationPending = false;
    bool completionValidationDone = false;
    QHash<QString, QString> verifiedDigestsByPath;
    QTimer* stallTimer = nullptr;
};

// Dedicated component that encapsulates upload/unload logic previously in ApplicationRuntime.
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
        Queued,
        AwaitingTargetReady,
        Streaming,
        AwaitingValidation,
        Suspended,
        Cancelling
    };
    Q_ENUM(OutgoingState)

    struct BulkTeardownResult {
        int discoveredScopes = 0;
        int committedScopes = 0;
        int cleanupErrorScopes = 0;
        int pendingScopes = 0;
        int removedFileMappings = 0;
        qint64 quarantinedBytes = 0;
        QString errorCode;

        bool allLogicallyCommitted() const
        {
            return cleanupErrorScopes == 0 && pendingScopes == 0 && errorCode.isEmpty();
        }
    };

    explicit UploadManager(
        FileManager* fileManager,
        QObject* parent = nullptr,
        const QString& remoteCacheRoot = RemoteCacheStore::defaultRootPath());
    ~UploadManager() override;
    void setWebSocketClient(WebSocketClient* client);
    void setTargetClientId(const QString& id);
    QString targetClientId() const { return m_targetClientId; }
    bool remoteMediaReady(const QString& targetEndpointId, const QString& sha256) const;
    struct SourceUploadStatus {
        enum State { NotUploaded, Uploading, Uploaded } state = NotUploaded;
        int progress = 0;
    };
    SourceUploadStatus sourceUploadStatus(const QString& targetEndpointId,
                                          const QString& fileId) const;
    static QString residencyOwnerId(const QString& sessionId, quint64 generation,
                                   const QString& sha256);

    QString activeUploadTargetClientId() const;
    void setActiveWorkspaceEndpointId(const QString& identity) { m_activeWorkspaceEndpointId = identity; }
    QString activeWorkspaceEndpointId() const { return m_activeWorkspaceEndpointId; }
    void forceResetForClient(const QString& clientId = QString());
    
    // Set local client ID for generating directional session IDs
    void setMyClientId(const QString& myClientId) { m_myClientId = myClientId; }

    // Outbound (sender side)
    bool hasActiveUpload() const;
    bool isUploading() const;
    bool isCancelling() const;
    bool isFinalizing() const;
    bool isRemoving() const {
        return !m_pendingAssetRemovals.isEmpty();
    }
    bool isBusy() const;
    bool canRequestCancel() const;
    OutgoingState outgoingState() const;
    QString currentUploadId() const;
    QString currentRemoteSessionId() const;
    quint64 currentRemoteSessionGeneration() const;
    int activeOutgoingTransferCount() const;
    UploadScheduler* uploadScheduler() const { return m_uploadScheduler; }

    // Starts a new/incremental upload. Cancellation is an explicit guarded action.
    bool toggleUpload(const QVector<UploadFileInfo>& files);
    void requestCancel();
    // Explicit user action: remove every validated asset owned by this
    // RemoteSession while keeping the session itself alive. The caller's
    // known IDs are merged with the manager's authoritative inventory so an
    // out-of-date UI projection cannot leave remote files behind.
    bool requestUnload(const QString& targetEndpointId,
                       const QSet<QString>& knownLocalFileIds = {});
    // Removes one immutable validated asset while keeping the RemoteSession
    // and every other uploaded asset alive. This is the canonical path used
    // when the last local reference to a source disappears.
    bool requestAssetRemoval(const QString& targetEndpointId,
                             const QString& localFileId,
                             const QString& reason = QStringLiteral("source_removed"));

    // Incoming (target side) handling entry point
    void handleIncomingMessage(const QJsonObject& message);

    // Called after the render/audio side has reached teardownSettled.  The
    // first call blocks all subsequent upload commands for this scope, closes
    // an in-flight receiver, drops FileManager handles/mappings, then commits
    // the atomic quarantine.  Replays return AlreadyCommitted.
    RemoteCacheStore::CommitResult teardownRemoteSession(
        const QString& senderEndpointId,
        const QString& remoteSessionId,
        quint64 generation,
        const QString& teardownId);
    // Terminal transport events must stop writers immediately but may not
    // rename/delete their cache while a renderer still owns those files.
    // ApplicationRuntime calls completeTerminalIncomingCleanup() only after every
    // correlated RemoteSceneController::teardownSettled barrier has fired.
    void beginTerminalIncomingCleanup(
        const QString& reasonCode,
        const QSet<QString>& remoteSessionIds = {});
    // Cancels background readers before a renderer-settled cache transaction.
    void beginIncomingFileReaderTeardown(const QSet<QString>& remoteSessionIds = {});
    bool incomingFileReadersSettled(const QSet<QString>& remoteSessionIds = {}) const;
    BulkTeardownResult completeTerminalIncomingCleanup(
        const QString& reasonCode,
        const QSet<QString>& remoteSessionIds = {});
    // Terminal local events (lease expiry/server restart) have no server
    // teardownId. Every live on-disk receiver scope is therefore committed
    // under a provisional tombstone which can later adopt the authenticated
    // server teardown identity without reopening the cache.
    BulkTeardownResult teardownAllIncomingRemoteSessions(
        const QString& reasonCode,
        const QSet<QString>& remoteSessionIds = {});
    bool receiverReadyForAdvertisement() const
    {
        return m_remoteCacheReady && m_receiverAdvertisementReady;
    }
    QString receiverCleanupError() const { return m_receiverCleanupError; }
    // Retry only after a previous startup/server-boot cleanup failure. A
    // healthy reconnect never sweeps live RemoteSessions.
    bool retryReceiverAdvertisementCleanup();
    RemoteCacheStore* remoteCacheStore() const { return m_remoteCacheStore; }
    int lastTeardownRemovedFileCount() const { return m_lastTeardownRemovedFileCount; }

signals:
    void incomingFileReadersChanged();
    void uiStateChanged(); // generic signal to refresh button text/state
    void uploadProgress(int percent, int filesCompleted, int totalFiles); // forwarded from server
    // Carries the immutable transfer identity so terminal UI/history events
    // remain correlated even after the runtime transfer state is cleared.
    void uploadFinished(const QString& uploadId);
    void uploadCancelled(const QString& uploadId);
    void uploadRejected(const QString& uploadId, const QString& reason);
    void assetRemovalCommitted(const QString& targetEndpointId,
                               const QStringList& localFileIds);
    void assetRemovalFailed(const QString& targetEndpointId,
                            const QString& remoteSessionId,
                            const QStringList& localFileIds,
                            const QString& reason);
    // New: subset of files confirmed complete by target so far
    void uploadCompletedFileIds(const QStringList& fileIds);
    // New: fine-grained per-file upload lifecycle (sender-side only)
    void fileUploadStarted(const QString& fileId);
    void fileUploadProgress(const QString& fileId, int percent);
    void fileUploadFinished(const QString& fileId);
    // Complete target replies. The transport layer adds its standard
    // protocolVersion/serverBootId/messageId/connectionGeneration envelope.
    void uploadProtocolResponseReady(const QJsonObject& response);
    void remoteSessionCacheCommitted(const QString& senderEndpointId,
                                     const QString& remoteSessionId,
                                     quint64 generation,
                                     const QString& teardownId,
                                     int removedFileMappings,
                                     qint64 quarantinedBytes);
    void remoteSessionCacheCleanupError(const QString& senderEndpointId,
                                        const QString& remoteSessionId,
                                        quint64 generation,
                                        const QString& teardownId,
                                        const QString& errorCode);
    void receiverAdvertisementReadinessChanged(bool ready,
                                               const QString& errorCode);
    // Requests the application-level renderer barrier. This is emitted
    // synchronously from leaseExpired/serverRestarted while RemoteSession
    // bindings can still be enumerated, before WebSocketClient clears them.
    void terminalIncomingCleanupRequired(const QString& reasonCode);

public slots:
    // Canonical protocol entry point. Both owner responses and target
    // requests arrive here with their immutable RemoteSession correlation.
    void handleUploadProtocolMessage(const QJsonObject& message);
    // Handle network connection loss while uploading/finalizing
    void onConnectionLost();

private:
    struct OutgoingAsset {
        QString assetId;       // content SHA-256 used by protocol v6
        QString sha256;
        QString path;
        QString name;
        QString extension;
        QStringList mediaIds;
        QStringList localFileIds;
        qint64 size = 0;
    };

    struct CommittedRemoteAsset {
        QString targetEndpointId;
        QString remoteSessionId;
        quint64 generation = 0;
        QString uploadId;
        QString assetId;
        QString sha256;
        QString extension;
        QStringList localFileIds;
        qint64 size = 0;
    };

    struct PendingAssetRemoval {
        QString removalId;
        CommittedRemoteAsset asset;
        QString reason;
        quint64 lastSentGeneration = 0;
    };

    struct IncomingUploadCompletionTombstone {
        QString senderEndpointId;
        QString remoteSessionId;
        quint64 generation = 0;
        quint64 sourceConnectionGeneration = 0;
        QString uploadId;
        QJsonArray assets;
        qint64 expiresAtMonotonicMs = 0;
    };

    // Every queued or active transfer owns a fully independent context. All
    // protocol routing is correlated by uploadId and RemoteSession identity.
    struct ParallelOutgoingTransfer {
        QString targetEndpointId;
        QString remoteSessionId;
        QString uploadId;
        quint64 generation = 0;
        quint64 schedulerGeneration = 0;
        OutgoingState state = OutgoingState::Queued;
        OutgoingState stateBeforeSuspend = OutgoingState::Idle;
        bool remoteInventoryBeforeStart = false;
        bool waitingForResume = false;
        bool payloadCompleteSent = false;
        bool transportRegistered = false;
        bool pumpRunning = false;
        QVector<OutgoingAsset> assets;
        QJsonArray manifest;
        QHash<QString, qint64> durableOffsets;
        QFile fileHandle;
        int fileIndex = 0;
        qint64 sentForFile = 0;
        qint64 totalBytes = 0;
        qint64 sentBytes = 0;
        qint64 remoteAcknowledgedBytes = 0;
        QTimer* pumpTimer = nullptr;
        QTimer* stallTimer = nullptr;
        QTimer* startAckTimer = nullptr;
        QTimer* ackTimer = nullptr;
        QTimer* cancelTimer = nullptr;
        MouffetteClock::ElapsedTimer stateAge;
        int totalFiles = 0;
        int localPercent = 0;
        int remotePercent = 0;
        int remoteFilesCompleted = 0;
        QHash<QString, int> localFilePercents;
        QHash<QString, int> remoteFilePercents;
    };

    void startUpload(const QVector<UploadFileInfo>& files);
    void startVerifiedUpload(const QVector<UploadFileInfo>& files, const QString& uploadId);
    QHash<QString, quint64> m_pendingUploadVerification;
    QHash<QString, std::shared_ptr<std::atomic_bool>> m_uploadVerificationCancellation;
    QHash<QString, QString> m_verifyingUploadIds;
    QHash<QString, QSet<QString>> m_verifyingFileIdsByTarget;
    quint64 m_uploadVerificationGeneration = 0;
    ParallelOutgoingTransfer* parallelForUpload(const QString& uploadId) const;
    ParallelOutgoingTransfer* parallelForSession(const QString& remoteSessionId) const;
    ParallelOutgoingTransfer* parallelForTarget(const QString& targetEndpointId) const;
    void initializeParallelTimers(ParallelOutgoingTransfer* transfer);
    void setParallelState(ParallelOutgoingTransfer* transfer, OutgoingState state);
    void startParallelScheduled(ParallelOutgoingTransfer* transfer);
    bool resumeParallel(ParallelOutgoingTransfer* transfer);
    void suspendParallel(ParallelOutgoingTransfer* transfer);
    void scheduleParallelPump(ParallelOutgoingTransfer* transfer);
    void stopParallel(ParallelOutgoingTransfer* transfer);
    void pumpParallel(ParallelOutgoingTransfer* transfer);
    QJsonArray parallelAssetStates(const ParallelOutgoingTransfer* transfer,
                                   bool complete) const;
    bool applyParallelOffsets(ParallelOutgoingTransfer* transfer,
                              const QJsonArray& assets,
                              bool resetSendCursor,
                              QString* errorMessage = nullptr);
    void failParallel(ParallelOutgoingTransfer* transfer, const QString& reason);
    void finishParallel(ParallelOutgoingTransfer* transfer);
    void cancelParallel(ParallelOutgoingTransfer* transfer);
    void removeParallel(ParallelOutgoingTransfer* transfer,
                        bool preserveRemoteInventory,
                        bool releaseScheduler,
                        bool schedulerSuccess = false);
    void handleParallelMessage(ParallelOutgoingTransfer* transfer,
                               const QJsonObject& message);
    void emitParallelProgress(const ParallelOutgoingTransfer* transfer);
    void startScheduledUpload(const UploadScheduler::UploadRequest& request);
    bool resumeOutgoingUpload();
    void suspendOutgoingForResume(const QString& reason = QString());
    void terminateRemoteSessionUpload(const QString& remoteSessionId,
                                      const QString& reason);
    bool applyAuthoritativeOffsets(const QJsonArray& assets,
                                   bool resetSendCursor,
                                   QString* errorMessage = nullptr);
    QJsonArray outgoingAssetStates(bool complete) const;
    void releaseSchedulerSlot(bool success);
    void clearOutgoingTransfer(bool preserveRemoteInventory);
    void applyRemoteSessionEnvelope(const QJsonObject& envelope);
    void setOutgoingState(OutgoingState state);
    void resetToInitial();
    void cleanupOrphanedIncomingCache();
    void cleanupIncomingCacheForConnectionLoss();
    bool discardIncomingUpload(const QString& uploadId,
                               bool rememberRejectedUpload);
    bool removeResidualIncomingStaging(const QString& senderId,
                                       const QString& uploadId);
    void rejectIncomingUpload(const QString& senderId,
                              const QString& uploadId,
                              const QString& reason,
                              bool discardMatchingSession,
                              const QString& remoteSessionId = QString(),
                              quint64 generation = 0);
    void clearIncomingChunkTracking(const QString& uploadId);
    void resetProgressTracking();
    void scheduleOutgoingPump();
    void pumpOutgoingUpload();
    void stopOutgoingPump();
    void failOutgoingUpload(const QString& reason);
    void finishLocalCancellation();
    void onUploadFinished(const QString& uploadId);
    void onUploadRejected(const QString& uploadId, const QString& reason);
    void updateLocalProgress(int percent, int filesCompleted);
    void updateRemoteProgress(int percent, int filesCompleted);
    void emitEffectiveProgressIfChanged();
    void updatePerFileLocalProgress(const QString& fileId, int percent);
    void updatePerFileRemoteProgress(const QString& fileId, int percent);
    void emitEffectivePerFileProgress(const QString& fileId);
    bool canAcceptNewAction() const;
    void recordAcceptedAction();
    void restartIncomingStallTimer(IncomingUploadSession& incoming);
    void closeIncomingFiles(IncomingUploadSession& incoming, bool flush);
    void suspendIncomingForResume(IncomingUploadSession& incoming);
    QJsonArray incomingAssetOffsets(const IncomingUploadSession& incoming) const;
    void rememberIncomingUploadCompletion(
        const QString& senderEndpointId,
        const QString& remoteSessionId,
        quint64 generation,
        quint64 sourceConnectionGeneration,
        const QString& uploadId,
        const QJsonArray& assets);
    bool replayIncomingUploadCompletion(
        const QJsonObject& message,
        const QString& senderEndpointId,
        const QString& remoteSessionId,
        quint64 generation,
        quint64 sourceConnectionGeneration);
    void pruneIncomingUploadCompletions(qint64 nowMonotonicMs);
    void forgetIncomingUploadCompletions(const QString& remoteSessionId);
    void emitIncomingResponse(const QString& type,
                              const QString& senderEndpointId,
                              const QString& remoteSessionId,
                              quint64 generation,
                              const QString& uploadId,
                              const QJsonObject& extra = QJsonObject());
    int detachReceivedMappingsForScope(const RemoteCacheStore::Scope& scope);
    void rememberCommittedAssets(const QString& targetEndpointId,
                                 const QString& remoteSessionId,
                                 quint64 generation,
                                 const QString& uploadId,
                                 const QVector<OutgoingAsset>& assets);
    bool findCommittedAsset(const QString& targetEndpointId,
                            const QString& localFileId,
                            CommittedRemoteAsset* asset) const;
    void forgetCommittedAsset(const CommittedRemoteAsset& asset);
    void forgetRemoteSessionInventory(const QString& remoteSessionId);
    void failAssetRemoval(const QString& removalId, const QString& reason);
    bool sendPendingAssetRemoval(PendingAssetRemoval& pending);
    bool assetRemovalMessageMatches(const PendingAssetRemoval& pending,
                                    const QJsonObject& message,
                                    bool requireDerivedFields = true) const;
    void handleIncomingAssetRemoval(const QJsonObject& message);
    void handleAssetRemovalResult(const QJsonObject& message);

    struct ResidentIncomingAsset {
        QString sessionId;
        quint64 generation = 0;
        QString assetId;
        QString sha256;
        QString path;
    };
    QHash<QString, ResidentIncomingAsset> m_residentIncoming;
    QHash<QString, quint64> m_residencySequences;
    QHash<QString, QJsonObject> m_remoteResidency;
    void publishResidency(const QString& sessionId);
    void receiveResidency(const QJsonObject& envelope);
    void releaseResidency(const QString& sessionId, const QString& sha256 = {});
    void settleIncomingFileReaders();
    QHash<QString, QSet<QString>> m_retiringResidencyPaths;
    QHash<QString, int> m_validationReadersBySession;
    QHash<QString, int> m_validationReadersByUpload;
    QHash<QString, bool> m_deferredIncomingDiscards;
    QHash<QString, QJsonObject> m_deferredIncomingAborts;
    QHash<QString, QJsonObject> m_deferredIncomingRemovals;

    QPointer<WebSocketClient> m_ws;
    QString m_targetClientId;
    // Captured at startUpload to remain stable across the whole transfer
    QString m_uploadTargetClientId;
    QString m_activeWorkspaceEndpointId;

    // Sender side state
    bool m_uploadActive = false;      // true while this target has validated remote inventory
    OutgoingState m_outgoingState = OutgoingState::Idle;
    OutgoingState m_stateBeforeSuspend = OutgoingState::Idle;
    bool m_uploadWasActiveBeforeStart = false; // preserve earlier synchronized files on incremental failure
    QString m_currentUploadId;        // uuid
    int m_lastPercent = 0;
    int m_filesCompleted = 0;
    int m_totalFiles = 0;
    QTimer* m_cancelFallbackTimer = nullptr; // fires if remote never responds to abort
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
    QVector<OutgoingAsset> m_outgoingAssets;
    QJsonArray m_outgoingManifest;
    QHash<QString, qint64> m_outgoingDurableOffsets;
    QString m_outgoingRemoteSessionId;
    quint64 m_outgoingGeneration = 0;
    quint64 m_schedulerGeneration = 0;
    bool m_waitingForResume = false;
    bool m_outgoingTransportRegistered = false;
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
    QVector<QMetaObject::Connection> m_webSocketConnections;
    QHash<QString, int> m_localFilePercents;
    QHash<QString, int> m_remoteFilePercents;
    QHash<QString, int> m_effectiveFilePercents;
    QHash<QString, ParallelOutgoingTransfer*> m_parallelOutgoingByUpload;
    QHash<QString, QString> m_parallelUploadBySession;
    QSet<QString> m_remoteInventoryTargets;
    QHash<QString, QHash<QString, CommittedRemoteAsset>>
        m_committedAssetsByTarget; // targetEndpointId -> assetId -> metadata
    QHash<QString, PendingAssetRemoval> m_pendingAssetRemovals; // removalId -> request
    QThreadPool m_incomingWritePool;
    qint64 m_queuedIncomingWriteBytes = 0;
    QHash<QString, int> m_teardownRemovedMappings;
    struct PendingCacheTeardown {
        RemoteCacheStore::Scope scope;
        QString teardownId;
        QString reason;
    };
    QHash<QString, PendingCacheTeardown> m_terminalCacheTeardowns;
    RetryScheduler m_receiverCleanupRetries;
    int m_receiverCleanupAttempt = 0;
    void scheduleReceiverCleanup();
    RetryScheduler m_stagingCleanupRetries;
    QHash<QString, int> m_stagingCleanupAttempts;
    void scheduleStagingCleanup(const QString& senderId, const QString& uploadId);
    QHash<QString, QJsonObject> m_pendingDiskRemovals;

    FileManager* m_fileManager = nullptr;
    RemoteCacheStore* m_remoteCacheStore = nullptr;
    UploadScheduler* m_uploadScheduler = nullptr;
    bool m_remoteCacheReady = false;
    bool m_receiverAdvertisementReady = false;
    QString m_receiverCleanupError;
    QString m_receiverCleanupReason = QStringLiteral("startup_recovery");
    bool m_terminalIncomingCleanupAwaitingRenderer = false;
    int m_lastTeardownRemovedFileCount = 0;

    // Incoming sessions are independent per upload and may belong to different
    // owners/RemoteSessions. This prevents one sender from blocking another.
    QHash<QString, IncomingUploadSession*> m_incomingUploads;
    QSet<QString> m_canceledIncoming; // uploadIds canceled by sender
    QHash<QString, IncomingUploadCompletionTombstone>
        m_incomingUploadCompletionTombstones;
    // Track the next durable byte offset per (uploadId:assetId).
    QHash<QString, qint64> m_expectedChunkIndex;
    
    // Local client ID for directional session generation
    QString m_myClientId; 

    // Anti-spam protection. State, rather than a short-lived boolean lock, is
    // authoritative; timing only filters accidental double-clicks.
    MouffetteClock::ElapsedTimer m_lastAcceptedAction;
    MouffetteClock::ElapsedTimer m_outgoingStateAge;
};

#endif // UPLOADMANAGER_H
