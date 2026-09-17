#ifndef REMOTECACHESTORE_H
#define REMOTECACHESTORE_H

#include <QObject>
#include <QJsonObject>
#include <QList>
#include <QSet>
#include <QString>
#include <QHash>
#include <QThreadPool>
#include <QTimer>

#include <optional>
#include <functional>
#include <memory>

class RemoteCacheHistory;
#include "backend/runtime/storage/StorageVersions.h"

/**
 * Durable, session-scoped storage for media received from another device.
 *
 * A cache scope is always Uploads/<senderEndpointId>/<remoteSessionId>.  The
 * caller never supplies a filesystem path: every component is validated and
 * resolved below the configured root.  Closing a scope is a two-phase,
 * idempotent operation:
 *
 *   beginTeardown()  -> durable intent; further asset access is refused
 *   commitTeardown() -> atomic rename to the private quarantine + tombstone
 *
 * Once commitTeardown() reports an acknowledgement-safe result, the live
 * namespace is gone.  Physical deletion is deliberately asynchronous and is
 * retried by initialize() after a crash.
 */
class RemoteCacheStore final : public QObject {
    Q_OBJECT

public:
    static constexpr int MetadataSchemaVersion = StorageVersions::ReceivedMedia;

    struct Scope {
        QString senderEndpointId;
        QString remoteSessionId;
        // RemoteSession generation, not either peer's transport generation.
        quint64 generation = 0;

        bool operator==(const Scope& other) const
        {
            return senderEndpointId == other.senderEndpointId
                && remoteSessionId == other.remoteSessionId
                && generation == other.generation;
        }
    };

    enum class AssetArea {
        Staging,
        Validated
    };

    enum class SessionState {
        Missing,
        Open,
        Terminating,
        CleanupPending,
        Closed,
        CleanupError
    };

    enum class CommitOutcome {
        Pending,
        Committed,
        AlreadyCommitted,
        InvalidRequest,
        Conflict,
        CleanupError
    };

    struct CommitResult {
        CommitOutcome outcome = CommitOutcome::InvalidRequest;
        QString teardownId;
        QString errorCode;
        qint64 quarantinedBytes = 0;
        bool cleanupPending = false;

        bool acknowledgementSafe() const
        {
            return outcome == CommitOutcome::Committed
                || outcome == CommitOutcome::AlreadyCommitted;
        }
    };

    struct AssetRemovalResult {
        CommitOutcome outcome = CommitOutcome::InvalidRequest;
        QString removalId;
        QString errorCode;
        qint64 quarantinedBytes = 0;

        bool acknowledgementSafe() const
        {
            return outcome == CommitOutcome::Committed
                || outcome == CommitOutcome::AlreadyCommitted;
        }
    };

    struct AssetRemovalDescriptor {
        QString removalId;
        QString uploadId;
        QString assetId;
        QString fileId;
        QString sha256;
        qint64 offset = 0;
        qint64 size = 0;
        QString extension;
    };

    struct Tombstone {
        Scope scope;
        QString teardownId;
        QString reasonCode;
        SessionState state = SessionState::Closed;
        QString errorCode;
        qint64 quarantinedBytes = 0;
        // Local lease/crash cleanup does not know the server teardownId yet.
        // Such a tombstone may adopt exactly one authenticated official id.
        bool provisional = false;
    };

    explicit RemoteCacheStore(QString rootPath = defaultRootPath(),
                              QObject* parent = nullptr,
                              std::shared_ptr<RemoteCacheHistory> history = {});
    ~RemoteCacheStore() override;

    static QString defaultRootPath();

    // Creates the private metadata/quarantine directories and resumes every
    // interrupted commit/deletion.  Any live cache found during startup is
    // terminally quarantined because a process restart invalidates its lease.
    bool initialize(QString* errorCode = nullptr);
    // Runtime recovery runs on the same serialized disk queue as quarantine.
    // The caller must have stopped every renderer and incoming file reader.
    bool requestRecovery();
    bool recoveryPending() const { return m_recoveryPending; }

    QString rootPath() const { return m_rootPath; }
    QString lastErrorCode() const { return m_lastErrorCode; }

    static bool isValidEndpointId(const QString& value);
    static bool isValidSessionId(const QString& value);
    static bool isValidTeardownId(const QString& value);
    static bool isValidAssetId(const QString& value);

    // Opens (or verifies) a scope. A strictly newer authenticated
    // RemoteSession generation advances the descriptor atomically; an older
    // generation, a teardown intent, or a tombstone can never reopen it.
    bool ensureSession(const Scope& scope, QString* errorCode = nullptr);

    // A lease resume keeps the same cache but advances the server-controlled
    // session generation.  The caller must already have authenticated the
    // resume envelope; rewinding or rebinding a terminating scope is refused.
    bool rebindSessionGeneration(const Scope& currentScope,
                                 quint64 newGeneration,
                                 QString* errorCode = nullptr);

    // Returns a safe path for an upload-owned file, creating only the fixed
    // staging/validated subdirectory. Validated files use compact physical
    // names. extension is lower-cased and accepts ASCII alphanumerics only.
    QString assetPath(const Scope& scope,
                      const QString& assetId,
                      AssetArea area,
                      const QString& extension = QString(),
                      QString* errorCode = nullptr);

    // Staging is transfer-scoped, whereas validated assets are content-scoped.
    // Compact deterministic path tokens prevent both stale-transfer collisions
    // and Windows MAX_PATH failures; protocol identities remain full-length.
    QString stagingAssetPath(const Scope& scope,
                             const QString& uploadId,
                             const QString& assetId,
                             const QString& extension = QString(),
                             QString* errorCode = nullptr);

    // Atomically removes one validated asset from the live namespace. The
    // complete immutable command tuple is tombstoned before an ACK may be
    // emitted, so only an exact duplicated server command can replay the same
    // committed result. A signed in-process resume may advance scope.generation
    // monotonically; every other field remains fixed. The RemoteSession stays
    // open.
    AssetRemovalResult removeValidatedAsset(
        const Scope& scope,
        const AssetRemovalDescriptor& descriptor);
    bool requestAssetRemoval(const Scope& scope, const AssetRemovalDescriptor& descriptor,
                             std::function<void(AssetRemovalResult)> completion);

    // Persists the first terminal reason and blocks all subsequent asset calls.
    // Repeating the same tuple is successful; another teardown/generation is a
    // conflict.  The teardown id must be a canonical lower-case UUID.
    bool beginTeardown(const Scope& scope,
                       const QString& teardownId,
                       QString* errorCode = nullptr);

    // Used only when the local lease/process makes a scope terminal before the
    // server's teardown envelope arrives. The resulting tombstone remains
    // terminal but can later adopt the official teardownId exactly once.
    bool beginProvisionalTeardown(const Scope& scope,
                                  const QString& localTeardownId,
                                  const QString& reasonCode,
                                  QString* errorCode = nullptr);

    // Atomically removes the scope from the live namespace and persists the
    // replay result before scheduling physical deletion.
    CommitResult commitTeardown(const Scope& scope, const QString& teardownId);

    // Runtime variant: a bounded, serialized disk transaction. The scope is
    // fenced synchronously; retries poll the exact result without repeating it.
    CommitResult requestTeardown(const Scope& scope, const QString& teardownId,
                                 const QString& provisionalReason = {});
    bool teardownPending(const QString& sessionId = {}) const;
    CommitResult teardownResult(const Scope& scope, const QString& teardownId) const;

    SessionState state(const Scope& scope) const;
    std::optional<Tombstone> tombstone(const Scope& scope) const;
    bool acceptsCommands(const Scope& scope) const;
    bool ownsPath(const Scope& scope, const QString& candidatePath) const;
    QList<Scope> liveScopes(QString* errorCode = nullptr) const;
    QList<Tombstone> allTombstones() const;
    int cleanupPendingCount() const;
    qint64 quarantinedBytesAwaitingDeletion() const;
    bool hasCleanupErrors() const;
    // True only when no live receiver namespace or uncommitted teardown intent
    // remains. A valid tombstone makes the logical commit authoritative even
    // when deletion of its inaccessible quarantine is still pending/failed.
    bool receiverAdvertisementSafe(QString* errorCode = nullptr) const;

signals:
    void recoveryFinished(bool ready, const QString& errorCode);
    void teardownFinished(const QString& senderEndpointId,
                          const QString& remoteSessionId,
                          quint64 generation,
                          const QString& teardownId);
    void logicalCommitCompleted(const QString& senderEndpointId,
                                const QString& remoteSessionId,
                                quint64 generation,
                                const QString& teardownId,
                                qint64 quarantinedBytes);
    void physicalCleanupCompleted(const QString& senderEndpointId,
                                  const QString& remoteSessionId,
                                  quint64 generation,
                                  const QString& teardownId,
                                  qint64 bytesRemoved);
    void physicalCleanupFailed(const QString& senderEndpointId,
                               const QString& remoteSessionId,
                               quint64 generation,
                               const QString& teardownId,
                               const QString& errorCode);

private:
    struct DeleteResult;

    bool validateScope(const Scope& scope, QString* errorCode) const;
    QString scopeKey(const Scope& scope) const;
    QString scopeDirectory(const Scope& scope) const;
    QString intentPath(const Scope& scope) const;
    QString tombstonePath(const Scope& scope) const;
    QString quarantineName(const Scope& scope, const QString& teardownId) const;
    QString assetRemovalIntentPath(const QString& removalId) const;
    QString assetRemovalTombstonePath(const QString& removalId) const;
    QString assetQuarantineName(const Scope& scope,
                                const QString& assetId,
                                const QString& removalId) const;
    QString quarantinePath(const QString& entryName) const;

    bool ensurePrivateDirectory(const QString& path, QString* errorCode) const;
    bool writeJsonAtomically(const QString& path,
                             const QJsonObject& object,
                             QString* errorCode) const;
    bool loadJsonObject(const QString& path,
                        QJsonObject* object,
                        QString* errorCode) const;
    bool persistCleanupError(const Scope& scope,
                             const QString& teardownId,
                             const QString& errorCode,
                             const QString& quarantineEntry = QString()) const;
    bool beginTeardownInternal(const Scope& scope,
                               const QString& teardownId,
                               bool provisional,
                               const QString& reasonCode,
                               QString* errorCode);
    bool adoptProvisionalTombstone(const Scope& scope,
                                   const QString& officialTeardownId,
                                   QString* errorCode);
    bool recoverAssetRemovalIntents(QString* errorCode);
    bool recoverIntents(QString* errorCode);
    bool sweepQuarantine(QString* errorCode);
    bool quarantineAbandonedSessions(QString* errorCode);
    void schedulePhysicalCleanup(const Tombstone& tombstone,
                                 const QString& quarantineEntry);
    void scheduleOrphanCleanup(const QString& quarantineEntry);
    void finishPhysicalCleanup(const DeleteResult& result);
    DeleteResult commitPhysicalCleanup(DeleteResult result);
    void requestCleanupSweep();
    void collectExpiredHistory();
    qsizetype retainedScopeCount() const;
    void setError(const QString& code, QString* output = nullptr) const;

    QString m_rootPath;
    QString m_statePath;
    QString m_intentsPath;
    QString m_tombstonesPath;
    QString m_cleanupErrorsPath;
    QString m_assetRemovalIntentsPath;
    QString m_assetRemovalTombstonesPath;
    QString m_quarantinePath;
    mutable QString m_lastErrorCode;
    bool m_initialized = false;
    QSet<QString> m_scheduledEntries;
    QThreadPool m_transactionPool;
    QHash<QString, Scope> m_pendingTransactions;
    QHash<QString, CommitResult> m_completedTransactions;
    QHash<QString, qint64> m_transactionRetryAt;
    QHash<QString, int> m_transactionFailures;
    QHash<QString, qint64> m_transactionStartedAt;
    QSet<QString> m_transactionFences;
    bool m_backgroundTransaction = false;
    bool m_recoveryPending = false;
    bool m_cleanupSweepPending = false;
    int m_pendingAssetRemovalCount = 0;
    QList<QPair<Tombstone, QString>> m_collectedPhysicalDeletes;
    QStringList m_collectedOrphanDeletes;
    QTimer m_cleanupSweepTimer;
    std::shared_ptr<RemoteCacheHistory> m_history;
};

#endif // REMOTECACHESTORE_H
