#include "backend/managers/system/SystemLifecycleMonitor.h"

#include "backend/managers/system/SystemLifecycleMonitorBackend.h"

#include <QMetaObject>
#include <QThread>

SystemLifecycleMonitor::SystemLifecycleMonitor(QObject* parent)
    : QObject(parent)
    , m_backend(createSystemLifecycleMonitorBackend(this))
{
}

SystemLifecycleMonitor::~SystemLifecycleMonitor()
{
    stopNativeMonitoring();
}

bool SystemLifecycleMonitor::startNativeMonitoring()
{
    if (m_nativeMonitoringActive) {
        return true;
    }
    if (!m_backend) {
        return false;
    }
    m_nativeMonitoringActive = m_backend->start();
    return m_nativeMonitoringActive;
}

void SystemLifecycleMonitor::stopNativeMonitoring()
{
    if (m_backend) {
        // Backends are idempotent and may have installed a partial hook before
        // reporting failure, so always give them an opportunity to clean up.
        m_backend->stop();
    }
    m_nativeMonitoringActive = false;
}

void SystemLifecycleMonitor::simulateNativeEventForTesting(NativeEvent event)
{
    applyNativeEvent(event);
}

void SystemLifecycleMonitor::applyNativeEvent(NativeEvent event)
{
    if (QThread::currentThread() != thread()) {
        QMetaObject::invokeMethod(this, [this, event]() { applyNativeEvent(event); },
                                  Qt::QueuedConnection);
        return;
    }

    const bool wasSuspended = isSystemSuspended();
    switch (event) {
    case NativeEvent::SystemWillSleep:
        m_systemSleeping = true;
        break;
    case NativeEvent::SystemDidWake:
        m_systemSleeping = false;
        break;
    case NativeEvent::SessionLocked:
        m_sessionLocked = true;
        break;
    case NativeEvent::SessionUnlocked:
        m_sessionLocked = false;
        break;
    }

    const bool suspended = isSystemSuspended();
    if (suspended != wasSuspended) {
        emit systemSuspendedChanged(suspended);
    }
}
