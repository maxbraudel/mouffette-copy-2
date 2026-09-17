#include "backend/runtime/SuspendInclusiveClock.h"
#include "backend/runtime/ApplicationActivityMonitor.h"

#include <QDateTime>

#include <utility>

ApplicationActivityMonitor::ApplicationActivityMonitor(QObject* parent)
    : QObject(parent)
{
}

void ApplicationActivityMonitor::setPointerInside(bool inside)
{
    if (m_pointerInside == inside) return;
    m_pointerInside = inside;
    reconcile();
}

void ApplicationActivityMonitor::setControlWindowVisible(bool visible)
{
    if (m_controlWindowVisible == visible) return;
    m_controlWindowVisible = visible;
    reconcile();
}

void ApplicationActivityMonitor::setSystemSuspended(bool suspended)
{
    if (m_systemSuspended == suspended) return;
    m_systemSuspended = suspended;
    reconcile();
}

void ApplicationActivityMonitor::setNowProviderForTesting(
    std::function<qint64()> provider)
{
    m_nowProvider = std::move(provider);
}

qint64 ApplicationActivityMonitor::nowMs() const
{
    return m_nowProvider ? m_nowProvider()
                         : MouffetteClock::anchoredEpochMs();
}

void ApplicationActivityMonitor::reconcile()
{
    const bool active = m_pointerInside && m_controlWindowVisible
        && !m_systemSuspended;
    if (active == m_effectiveActive) return;
    m_effectiveActive = active;
    const qint64 timestamp = nowMs();
    if (active) {
        emit activityResumed(timestamp);
        m_inactiveSinceMs = -1;
        return;
    }
    m_inactiveSinceMs = timestamp;
    emit inactivityStarted(timestamp);
}
