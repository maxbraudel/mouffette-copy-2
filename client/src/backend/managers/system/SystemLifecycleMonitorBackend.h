#ifndef SYSTEMLIFECYCLEMONITORBACKEND_H
#define SYSTEMLIFECYCLEMONITORBACKEND_H

#include "backend/managers/system/SystemLifecycleMonitor.h"

#include <memory>

class SystemLifecycleMonitorBackend
{
public:
    explicit SystemLifecycleMonitorBackend(SystemLifecycleMonitor* monitor)
        : m_monitor(monitor)
    {
    }
    virtual ~SystemLifecycleMonitorBackend() = default;

    virtual bool start() = 0;
    virtual void stop() = 0;

protected:
    void publish(SystemLifecycleMonitor::NativeEvent event)
    {
        if (m_monitor) {
            m_monitor->applyNativeEvent(event);
        }
    }

private:
    SystemLifecycleMonitor* m_monitor = nullptr;
};

std::unique_ptr<SystemLifecycleMonitorBackend>
createSystemLifecycleMonitorBackend(SystemLifecycleMonitor* monitor);

#endif // SYSTEMLIFECYCLEMONITORBACKEND_H
