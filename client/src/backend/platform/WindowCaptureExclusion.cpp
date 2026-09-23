#include "backend/platform/WindowCaptureExclusion.h"

#include <QEvent>
#include <QGuiApplication>
#include <QOperatingSystemVersion>
#include <QPlatformSurfaceEvent>
#include <QPointer>
#include <QScopedValueRollback>
#include <QSet>
#include <QWindow>

#ifdef Q_OS_WIN
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

namespace {
QPointer<WindowCaptureExclusion> nativeCoordinator;
}
#endif

WindowCaptureExclusion& WindowCaptureExclusion::instance()
{
    static QPointer<WindowCaptureExclusion> coordinator;
    if (!coordinator) coordinator = new WindowCaptureExclusion(qApp);
    return *coordinator;
}

WindowCaptureExclusion::WindowCaptureExclusion(QObject* parent) : QObject(parent)
{
    qApp->installEventFilter(this);
#ifdef Q_OS_WIN
    if (QGuiApplication::platformName() == QLatin1String("windows")) installNativeWindowHook();
#endif
}

WindowCaptureExclusion::~WindowCaptureExclusion()
{
#ifdef Q_OS_WIN
    if (m_nativeWindowHook) UnhookWindowsHookEx(static_cast<HHOOK>(m_nativeWindowHook));
    nativeCoordinator = nullptr;
#endif
    if (qApp) qApp->removeEventFilter(this);
}

void WindowCaptureExclusion::failCapture(const QString& reason)
{
    if (m_captureError == reason) return;
    m_captureError = reason;
    emit captureSafetyChanged(false, m_captureError);
}

#ifdef Q_OS_WIN
bool WindowCaptureExclusion::installNativeWindowHook()
{
    if (m_nativeWindowHook) return true;
    nativeCoordinator = this;
    // Qt's native event filter can be bypassed by native modal dialogs and
    // menus. This GUI-thread hook observes their pre-show messages too, before
    // the window procedure can make any content visible to capture.
    m_nativeWindowHook = SetWindowsHookExW(WH_CALLWNDPROC,
        [](int code, WPARAM parameter, LPARAM message) -> LRESULT {
            auto* coordinator = nativeCoordinator.data();
            if (code == HC_ACTION && coordinator && !coordinator->m_applying) {
                const auto* event = reinterpret_cast<const CWPSTRUCT*>(message);
                if (event->message == WM_WINDOWPOSCHANGING) {
                    const auto* position = reinterpret_cast<const WINDOWPOS*>(event->lParam);
                    if (position && (position->flags & SWP_SHOWWINDOW)) {
                        QScopedValueRollback<bool> guard(coordinator->m_applying, true);
                        coordinator->applyNativeWindow(reinterpret_cast<quintptr>(event->hwnd));
                    }
                }
            }
            return CallNextHookEx(nullptr, code, parameter, message);
        }, nullptr, GetCurrentThreadId());
    if (m_nativeWindowHook) return true;
    failCapture(QStringLiteral("Windows could not protect new Mouffette windows from screen sharing (error %1).")
        .arg(GetLastError()));
    return false;
}

bool WindowCaptureExclusion::applyNativeWindow(quintptr handle)
{
    const auto window = reinterpret_cast<HWND>(handle);
    if (!IsWindow(window) || (GetWindowLongPtr(window, GWL_STYLE) & WS_CHILD)) return true;
    DWORD processId = 0;
    GetWindowThreadProcessId(window, &processId);
    if (processId != GetCurrentProcessId()) return true;
    const auto version = QOperatingSystemVersion::current();
    if (version.majorVersion() < 10 || (version.majorVersion() == 10 && version.microVersion() < 19041)) {
        failCapture(QStringLiteral("Excluding Mouffette from screen sharing requires Windows 10 version 2004 or later."));
        return false;
    }
    // The numeric value also builds with older Windows SDK headers. Its use
    // remains gated above: older Windows silently falls back to a black box.
    constexpr DWORD excludeFromCapture = 0x00000011;
    DWORD actual = WDA_NONE;
    if (GetWindowDisplayAffinity(window, &actual) && actual == excludeFromCapture) return true;
    // GetWindowDisplayAffinity is documented to require a layered window.
    // Ordinary Qt windows need not be layered, so a successful setter
    // is authoritative even when reading the affinity back is unsupported.
    if (SetWindowDisplayAffinity(window, excludeFromCapture)) return true;
    failCapture(QStringLiteral("Windows could not exclude Mouffette from screen sharing (error %1).")
        .arg(GetLastError()));
    return false;
}
#endif

void WindowCaptureExclusion::applyWindow(QWindow* window)
{
    if (!window || !window->handle() || m_applying) return;
#ifdef Q_OS_WIN
    if (QGuiApplication::platformName() != QLatin1String("windows")) return;
    QScopedValueRollback<bool> guard(m_applying, true);
    applyNativeWindow(window->winId());
#endif
}

bool WindowCaptureExclusion::prepareForCapture(QString* error)
{
    const bool previouslyAllowed = captureAllowed();
    m_captureError.clear();
#ifdef Q_OS_WIN
    if (QGuiApplication::platformName() == QLatin1String("windows")) {
        installNativeWindowHook();
        QScopedValueRollback<bool> guard(m_applying, true);
        struct Inventory {
            WindowCaptureExclusion* coordinator;
            QSet<quintptr> qtWindows;
        } inventory{this, {}};
        for (QWindow* window : QGuiApplication::topLevelWindows())
            if (window->handle()) inventory.qtWindows.insert(window->winId());
        // Include visible native windows and all Qt surfaces, even hidden
        // windows that can reopen. Hidden OS/Qt infrastructure HWNDs (tray,
        // clipboard, message dispatch) have no content to exclude and can
        // reject affinity. The native hook protects a later SWP_SHOWWINDOW.
        EnumWindows([](HWND window, LPARAM context) -> BOOL {
            auto* inventory = reinterpret_cast<Inventory*>(context);
            const auto handle = reinterpret_cast<quintptr>(window);
            if (IsWindowVisible(window) || inventory->qtWindows.contains(handle))
                inventory->coordinator->applyNativeWindow(handle);
            return TRUE;
        }, reinterpret_cast<LPARAM>(&inventory));
        // Check the OS even if the process currently has no native windows.
        const auto version = QOperatingSystemVersion::current();
        if (version.majorVersion() < 10 || (version.majorVersion() == 10 && version.microVersion() < 19041))
            failCapture(QStringLiteral("Excluding Mouffette from screen sharing requires Windows 10 version 2004 or later."));
    }
#endif
    if (error) *error = m_captureError;
    if (!previouslyAllowed && captureAllowed()) emit captureSafetyChanged(true, {});
    return captureAllowed();
}

bool WindowCaptureExclusion::eventFilter(QObject* watched, QEvent* event)
{
    auto* window = qobject_cast<QWindow*>(watched);
    if (!window || m_applying) return false;
    switch (event->type()) {
    case QEvent::Show:
    case QEvent::WinIdChange:
        applyWindow(window);
        break;
    case QEvent::PlatformSurface:
        if (static_cast<QPlatformSurfaceEvent*>(event)->surfaceEventType()
            == QPlatformSurfaceEvent::SurfaceCreated) applyWindow(window);
        break;
    default:
        break;
    }
    return false;
}
