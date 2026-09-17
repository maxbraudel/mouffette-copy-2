#include "backend/runtime/SuspendInclusiveClock.h"
#include "backend/domain/session/IncomingSessionOrphanWatchdog.h"

#include <QDateTime>
#include <limits>
#include <utility>

IncomingSessionOrphanWatchdog::IncomingSessionOrphanWatchdog(QObject* parent)
    : QObject(parent)
{
    m_timer.setSingleShot(true);
    connect(&m_timer, &QTimer::timeout,
            this, [this] { processDeadline(); });
}

void IncomingSessionOrphanWatchdog::setConfiguredTimeoutMs(qint64 timeoutMs)
{
    m_configuredTimeoutMs = qBound<qint64>(qint64(1'000), timeoutMs,
                                           qint64(86'400'000));
}

qint64 IncomingSessionOrphanWatchdog::nowMs() const
{
    return m_nowProvider ? m_nowProvider()
                         : MouffetteClock::anchoredEpochMs();
}

bool IncomingSessionOrphanWatchdog::arm(
    const QSet<QString>& remoteSessionIds,
    qint64 advertisedLeaseTimeoutMs,
    qint64 atMs)
{
    if (active() || remoteSessionIds.isEmpty()) return false;
    m_sessionIds.clear();
    for (const QString& id : remoteSessionIds) {
        if (!id.trimmed().isEmpty()) m_sessionIds.insert(id.trimmed());
    }
    if (m_sessionIds.isEmpty()) return false;

    const qint64 current = atMs >= 0 ? atMs : nowMs();
    const qint64 delay = qMax(m_configuredTimeoutMs,
                              advertisedLeaseTimeoutMs);
    m_deadlineMs = current + delay;
    m_timer.start(static_cast<int>(qMin<qint64>(
        delay, std::numeric_limits<int>::max())));
    return true;
}

bool IncomingSessionOrphanWatchdog::resolveBeforeDeadline(
    const QString& remoteSessionId, qint64 current)
{
    if (!active() || current >= m_deadlineMs) {
        processDeadline(current);
        return false;
    }
    if (!m_sessionIds.remove(remoteSessionId)) return false;
    if (m_sessionIds.isEmpty()) cancelAll();
    return true;
}

bool IncomingSessionOrphanWatchdog::authenticatedSessionResumed(
    const QString& remoteSessionId, qint64 atMs)
{
    return resolveBeforeDeadline(remoteSessionId,
                                 atMs >= 0 ? atMs : nowMs());
}

bool IncomingSessionOrphanWatchdog::officiallyClosed(
    const QString& remoteSessionId, qint64 atMs)
{
    return resolveBeforeDeadline(remoteSessionId,
                                 atMs >= 0 ? atMs : nowMs());
}

void IncomingSessionOrphanWatchdog::processDeadline(qint64 atMs)
{
    if (!active()) return;
    const qint64 current = atMs >= 0 ? atMs : nowMs();
    if (current < m_deadlineMs) {
        m_timer.start(static_cast<int>(qMin<qint64>(
            m_deadlineMs - current, std::numeric_limits<int>::max())));
        return;
    }
    const QSet<QString> due = m_sessionIds;
    cancelAll();
    if (!due.isEmpty()) emit orphanedSessionsDue(due);
}

void IncomingSessionOrphanWatchdog::setNowProviderForTesting(
    std::function<qint64()> provider)
{
    m_nowProvider = std::move(provider);
}

void IncomingSessionOrphanWatchdog::stopAutomaticTimerForTesting()
{
    m_timer.stop();
}

void IncomingSessionOrphanWatchdog::cancelAll()
{
    m_timer.stop();
    m_sessionIds.clear();
    m_deadlineMs = -1;
}
