#pragma once

#include <QObject>
#include <QSet>
#include <QTimer>
#include <functional>

/**
 * Non-extensible receiver-side deadline for incoming RemoteSessions whose
 * authenticated server authority was lost. Transport reconnection alone has
 * no effect; callers may cancel one captured id only after authenticated
 * resumption of that exact session.
 */
class IncomingSessionOrphanWatchdog final : public QObject
{
    Q_OBJECT

public:
    explicit IncomingSessionOrphanWatchdog(QObject* parent = nullptr);

    void setConfiguredTimeoutMs(qint64 timeoutMs);
    qint64 configuredTimeoutMs() const { return m_configuredTimeoutMs; }
    bool arm(const QSet<QString>& remoteSessionIds,
             qint64 advertisedLeaseTimeoutMs = 0,
             qint64 nowMs = -1);
    bool authenticatedSessionResumed(const QString& remoteSessionId,
                                     qint64 nowMs = -1);
    bool officiallyClosed(const QString& remoteSessionId,
                          qint64 nowMs = -1);
    // Intentionally a no-op: socket state is not authenticated authority.
    void transportConnected() const {}
    void processDeadline(qint64 nowMs = -1);

    bool active() const { return m_deadlineMs >= 0; }
    qint64 deadlineMs() const { return m_deadlineMs; }
    QSet<QString> capturedSessionIds() const { return m_sessionIds; }

    void setNowProviderForTesting(std::function<qint64()> provider);
    void stopAutomaticTimerForTesting();
    void cancelAll();

signals:
    void orphanedSessionsDue(const QSet<QString>& remoteSessionIds);

private:
    qint64 nowMs() const;
    bool resolveBeforeDeadline(const QString& remoteSessionId,
                               qint64 currentMs);

    QTimer m_timer;
    QSet<QString> m_sessionIds;
    qint64 m_configuredTimeoutMs = 3'000;
    qint64 m_deadlineMs = -1;
    std::function<qint64()> m_nowProvider;
};
