#include "backend/managers/system/SystemLifecycleMonitorBackend.h"

#import <AppKit/AppKit.h>

#include <vector>

namespace {
class MacSystemLifecycleMonitorBackend final : public SystemLifecycleMonitorBackend
{
public:
    using SystemLifecycleMonitorBackend::SystemLifecycleMonitorBackend;

    ~MacSystemLifecycleMonitorBackend() override
    {
        stop();
    }

    bool start() override
    {
        if (m_started) {
            return true;
        }

        @autoreleasepool {
            NSNotificationCenter* workspaceCenter =
                [[NSWorkspace sharedWorkspace] notificationCenter];
            observe(workspaceCenter, NSWorkspaceWillSleepNotification,
                    SystemLifecycleMonitor::NativeEvent::SystemWillSleep);
            observe(workspaceCenter, NSWorkspaceDidWakeNotification,
                    SystemLifecycleMonitor::NativeEvent::SystemDidWake);

            // These cover login-session resignation/activation (including fast
            // user switching). The distributed notifications additionally
            // cover the ordinary screen-lock path on current macOS releases.
            observe(workspaceCenter, NSWorkspaceSessionDidResignActiveNotification,
                    SystemLifecycleMonitor::NativeEvent::SessionLocked);
            observe(workspaceCenter, NSWorkspaceSessionDidBecomeActiveNotification,
                    SystemLifecycleMonitor::NativeEvent::SessionUnlocked);

            NSDistributedNotificationCenter* distributedCenter =
                [NSDistributedNotificationCenter defaultCenter];
            observe(distributedCenter, @"com.apple.screenIsLocked",
                    SystemLifecycleMonitor::NativeEvent::SessionLocked);
            observe(distributedCenter, @"com.apple.screenIsUnlocked",
                    SystemLifecycleMonitor::NativeEvent::SessionUnlocked);
        }

        m_started = !m_observers.empty();
        return m_started;
    }

    void stop() override
    {
        if (m_observers.empty()) {
            m_started = false;
            return;
        }

        @autoreleasepool {
            for (const Observer& observer : m_observers) {
                [observer.center removeObserver:observer.token];
                [observer.token release];
            }
        }
        m_observers.clear();
        m_started = false;
    }

private:
    struct Observer {
        NSNotificationCenter* center = nil;
        id token = nil;
    };

    void observe(NSNotificationCenter* center,
                 NSString* name,
                 SystemLifecycleMonitor::NativeEvent event)
    {
        if (!center || !name) {
            return;
        }
        id token = [center addObserverForName:name
                                      object:nil
                                       queue:nil
                                  usingBlock:^(NSNotification*) {
                                      publish(event);
                                  }];
        if (token) {
            m_observers.push_back({center, [token retain]});
        }
    }

    std::vector<Observer> m_observers;
    bool m_started = false;
};
}

std::unique_ptr<SystemLifecycleMonitorBackend>
createSystemLifecycleMonitorBackend(SystemLifecycleMonitor* monitor)
{
    return std::make_unique<MacSystemLifecycleMonitorBackend>(monitor);
}
