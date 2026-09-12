#include "backend/managers/system/SystemLifecycleMonitorBackend.h"

#include <QAbstractNativeEventFilter>
#include <QCoreApplication>
#include <QDebug>
#include <QGuiApplication>
#include <QWindow>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <wtsapi32.h>

namespace {
class WindowsSystemLifecycleMonitorBackend final
    : public SystemLifecycleMonitorBackend
    , public QAbstractNativeEventFilter
{
public:
    using SystemLifecycleMonitorBackend::SystemLifecycleMonitorBackend;

    ~WindowsSystemLifecycleMonitorBackend() override
    {
        stop();
    }

    bool start() override
    {
        if (m_filterInstalled) {
            return true;
        }
        if (!qobject_cast<QGuiApplication*>(QCoreApplication::instance())) {
            qWarning() << "System lifecycle monitoring requires QGuiApplication on Windows";
            return false;
        }

        m_window = std::make_unique<QWindow>();
        m_window->setFlags(Qt::Tool | Qt::FramelessWindowHint);
        m_window->setGeometry(-32'000, -32'000, 1, 1);
        m_window->create();
        m_hwnd = reinterpret_cast<HWND>(m_window->winId());
        if (!m_hwnd) {
            m_window.reset();
            return false;
        }

        QCoreApplication::instance()->installNativeEventFilter(this);
        m_filterInstalled = true;
        m_sessionNotificationsRegistered =
            WTSRegisterSessionNotification(m_hwnd, NOTIFY_FOR_THIS_SESSION) != FALSE;
        if (!m_sessionNotificationsRegistered) {
            qWarning() << "WTS session lock notifications are unavailable; power events remain monitored";
        }
        return true;
    }

    void stop() override
    {
        if (m_sessionNotificationsRegistered && m_hwnd) {
            WTSUnRegisterSessionNotification(m_hwnd);
        }
        m_sessionNotificationsRegistered = false;

        if (m_filterInstalled && QCoreApplication::instance()) {
            QCoreApplication::instance()->removeNativeEventFilter(this);
        }
        m_filterInstalled = false;
        m_hwnd = nullptr;
        m_window.reset();
    }

    bool nativeEventFilter(const QByteArray& eventType,
                           void* message,
                           qintptr* result) override
    {
        Q_UNUSED(eventType);
        Q_UNUSED(result);
        const MSG* nativeMessage = static_cast<const MSG*>(message);
        if (!nativeMessage || nativeMessage->hwnd != m_hwnd) {
            return false;
        }

        if (nativeMessage->message == WM_POWERBROADCAST) {
            switch (nativeMessage->wParam) {
            case PBT_APMSUSPEND:
                publish(SystemLifecycleMonitor::NativeEvent::SystemWillSleep);
                break;
            case PBT_APMRESUMEAUTOMATIC:
            case PBT_APMRESUMESUSPEND:
            case PBT_APMRESUMECRITICAL:
                publish(SystemLifecycleMonitor::NativeEvent::SystemDidWake);
                break;
            default:
                break;
            }
        } else if (nativeMessage->message == WM_WTSSESSION_CHANGE) {
            if (nativeMessage->wParam == WTS_SESSION_LOCK) {
                publish(SystemLifecycleMonitor::NativeEvent::SessionLocked);
            } else if (nativeMessage->wParam == WTS_SESSION_UNLOCK) {
                publish(SystemLifecycleMonitor::NativeEvent::SessionUnlocked);
            }
        }
        return false;
    }

private:
    std::unique_ptr<QWindow> m_window;
    HWND m_hwnd = nullptr;
    bool m_filterInstalled = false;
    bool m_sessionNotificationsRegistered = false;
};
}

std::unique_ptr<SystemLifecycleMonitorBackend>
createSystemLifecycleMonitorBackend(SystemLifecycleMonitor* monitor)
{
    return std::make_unique<WindowsSystemLifecycleMonitorBackend>(monitor);
}
