#ifndef UPLOADSCHEDULER_H
#define UPLOADSCHEDULER_H

#include <QObject>
#include <QHash>
#include <QQueue>
#include <QSet>
#include <QString>
#include <QVector>

/**
 * Deterministic upload admission scheduler.
 *
 * The scheduler intentionally stores protocol identifiers only. File paths,
 * resume tokens and credentials remain owned by the upload/transport layers.
 * A request emitted through uploadStartRequested() owns one global slot until
 * completeUpload(), failUpload(), cancelUpload(), or a terminal session state
 * releases it.
 */
class UploadScheduler final : public QObject
{
    Q_OBJECT

public:
    enum class SessionState : quint8 {
        Opening,
        Active,
        Grace,
        Terminating,
        CleanupPending,
        Closed
    };
    Q_ENUM(SessionState)

    struct UploadRequest {
        QString remoteSessionId;
        quint64 connectionGeneration = 0;
        QString uploadId;
        QString assetId;

        bool operator==(const UploadRequest &other) const noexcept
        {
            return remoteSessionId == other.remoteSessionId
                && connectionGeneration == other.connectionGeneration
                && uploadId == other.uploadId
                && assetId == other.assetId;
        }
    };

    enum class EnqueueResult : quint8 {
        Enqueued,
        InvalidRequest,
        Duplicate,
        SessionTerminal
    };
    Q_ENUM(EnqueueResult)

    explicit UploadScheduler(int maximumConcurrentUploads = 2,
                             QObject *parent = nullptr);

    int maximumConcurrentUploads() const noexcept;
    bool setMaximumConcurrentUploads(int maximum);

    /**
     * Records a session state. Only Active sessions are admitted to a slot.
     * Once a terminal state is observed, the session cannot become active
     * again within this scheduler instance.
     */
    bool setSessionState(const QString &remoteSessionId, SessionState state);
    SessionState sessionState(const QString &remoteSessionId) const;
    bool isSessionTerminal(const QString &remoteSessionId) const;

    EnqueueResult enqueue(const UploadRequest &request);

    /** Release an active slot after an exact-generation result. */
    bool completeUpload(const QString &remoteSessionId,
                        quint64 connectionGeneration,
                        const QString &uploadId);
    bool failUpload(const QString &remoteSessionId,
                    quint64 connectionGeneration,
                    const QString &uploadId);

    /** Cancel one queued or active upload. Replayed IDs remain rejected. */
    bool cancelUpload(const QString &remoteSessionId,
                      quint64 connectionGeneration,
                      const QString &uploadId);

    /**
     * Enter the irreversible Terminating state, cancel the active upload (if
     * any), and discard every queued upload for this session.
     */
    bool cancelSession(const QString &remoteSessionId);

    int activeCount() const noexcept;
    int queuedCount() const noexcept;
    int queuedCount(const QString &remoteSessionId) const;
    bool isUploadActive(const QString &remoteSessionId,
                        quint64 connectionGeneration,
                        const QString &uploadId) const;
    QVector<UploadRequest> queuedUploads(const QString &remoteSessionId) const;

signals:
    /** Emitted synchronously, in deterministic admission order. */
    void uploadStartRequested(const UploadScheduler::UploadRequest &request);

    /**
     * Emitted for terminal/session and explicit cancellation. wasActive tells
     * an integration whether transport I/O also needs to be interrupted.
     */
    void uploadCancelled(const UploadScheduler::UploadRequest &request,
                         bool wasActive);
    void sessionPurged(const QString &remoteSessionId,
                       int queuedUploadCount,
                       bool hadActiveUpload);

private:
    static bool isTerminalState(SessionState state) noexcept;
    static QString uploadKey(const QString &remoteSessionId,
                             const QString &uploadId);
    static bool isValid(const UploadRequest &request) noexcept;

    void dispatch();
    void ensureRoundRobinEntry(const QString &remoteSessionId);
    void removeRoundRobinEntry(const QString &remoteSessionId);
    bool hasOtherEligibleSession(const QString &excludedSessionId) const;
    bool releaseActive(const QString &remoteSessionId,
                       quint64 connectionGeneration,
                       const QString &uploadId);
    void purgeTerminalSession(const QString &remoteSessionId);

    int m_maximumConcurrentUploads = 2;
    QHash<QString, SessionState> m_sessionStates;
    QSet<QString> m_terminalSessions;
    QHash<QString, QQueue<UploadRequest>> m_pendingBySession;
    QHash<QString, UploadRequest> m_activeBySession;
    QQueue<QString> m_roundRobinSessions;
    QSet<QString> m_roundRobinMembership;
    QSet<QString> m_seenUploadKeys;
    QString m_lastStartedSession;
    bool m_dispatching = false;
    bool m_dispatchAgain = false;
};

Q_DECLARE_METATYPE(UploadScheduler::UploadRequest)

#endif // UPLOADSCHEDULER_H
