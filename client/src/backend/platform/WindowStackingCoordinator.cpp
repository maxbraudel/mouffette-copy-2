#include "backend/platform/WindowStackingCoordinator.h"

#include <QEvent>
#include <QGuiApplication>
#include <QPlatformSurfaceEvent>
#include <QScopedValueRollback>

#if defined(Q_OS_MACOS)
#include "backend/platform/macos/MacWindowManager.h"
#elif defined(Q_OS_WIN)
#include "backend/platform/windows/WindowsWindowManager.h"
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace {
#ifdef Q_OS_WIN
QPointer<WindowStackingCoordinator> nativeCoordinator;
#endif
bool visible(const QWindow* window)
{
    return window && window->isVisible() && window->windowState() != Qt::WindowMinimized;
}
}

WindowStackingCoordinator& WindowStackingCoordinator::instance()
{
    static QPointer<WindowStackingCoordinator> coordinator;
    if (!coordinator) coordinator = new WindowStackingCoordinator(qGuiApp);
    return *coordinator;
}

WindowStackingCoordinator::WindowStackingCoordinator(QObject* parent) : QObject(parent)
{
    m_timer.setInterval(500);
    m_deferred.setSingleShot(true);
    m_deferred.setInterval(0);
    connect(&m_timer, &QTimer::timeout, this, &WindowStackingCoordinator::enforce);
    connect(&m_deferred, &QTimer::timeout, this, &WindowStackingCoordinator::enforce);
    qGuiApp->installEventFilter(this);
    qGuiApp->installNativeEventFilter(this);
    connect(qGuiApp, &QGuiApplication::applicationStateChanged, this,
            [this] { scheduleEnforcement(); });
    connect(qGuiApp, &QGuiApplication::focusWindowChanged, this,
            [this] { scheduleEnforcement(); });
#ifdef Q_OS_WIN
    if (QGuiApplication::platformName() == QLatin1String("windows")) {
        nativeCoordinator = this;
        // Native common dialogs/menus can run their own message loops and
        // bypass Qt's nativeEventFilter. WinEvents also catch other topmost
        // applications and Explorer changes; the timer remains a fallback.
        const WINEVENTPROC callback = [](HWINEVENTHOOK, DWORD event, HWND, LONG object,
                                         LONG, DWORD, DWORD) {
            if (nativeCoordinator && (event == EVENT_SYSTEM_FOREGROUND || object == OBJID_WINDOW))
                nativeCoordinator->scheduleEnforcement();
        };
        m_windowEventHook = SetWinEventHook(EVENT_OBJECT_DESTROY, EVENT_OBJECT_REORDER,
            nullptr, callback, 0, 0, WINEVENT_OUTOFCONTEXT);
        m_foregroundEventHook = SetWinEventHook(EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_FOREGROUND,
            nullptr, callback, 0, 0, WINEVENT_OUTOFCONTEXT);
    }
#endif
}

WindowStackingCoordinator::~WindowStackingCoordinator()
{
#ifdef Q_OS_WIN
    if (m_windowEventHook) UnhookWinEvent(static_cast<HWINEVENTHOOK>(m_windowEventHook));
    if (m_foregroundEventHook) UnhookWinEvent(static_cast<HWINEVENTHOOK>(m_foregroundEventHook));
    nativeCoordinator = nullptr;
#endif
    if (qGuiApp) {
        qGuiApp->removeEventFilter(this);
        qGuiApp->removeNativeEventFilter(this);
    }
}

void WindowStackingCoordinator::registerControlWindow(QWindow* window)
{
    registerWindow(window, false);
}

void WindowStackingCoordinator::registerSceneWindow(QWindow* window)
{
    registerWindow(window, true);
}

void WindowStackingCoordinator::registerWindow(QWindow* window, bool scene)
{
    if (!window) return;
    unregisterWindow(window);
    Entry entry{window, scene, !scene, {}};
    entry.destroyed = connect(window, &QObject::destroyed, this, [this] {
        m_windows.removeIf([](const Entry& entry) { return entry.window.isNull(); });
        scheduleEnforcement();
    });
    m_windows.append(entry);
#ifdef Q_OS_MACOS
    // Configuration is deliberately safe during the hidden PREPARE phase.
    if (scene) MacWindowManager::configureGlobalOverlay(window);
#endif
    scheduleEnforcement();
}

void WindowStackingCoordinator::setSceneWindowActive(QWindow* window, bool active)
{
    for (Entry& entry : m_windows) {
        if (entry.window == window && entry.scene) entry.active = active;
    }
    enforce();
}

void WindowStackingCoordinator::unregisterWindow(QWindow* window)
{
    m_windows.removeIf([window](const Entry& entry) {
        if (entry.window && entry.window != window) return false;
        QObject::disconnect(entry.destroyed);
        return true;
    });
    scheduleEnforcement();
}

void WindowStackingCoordinator::scheduleEnforcement()
{
    if (!m_enforcing && !m_deferred.isActive()) m_deferred.start();
}

void WindowStackingCoordinator::enforce()
{
    if (m_enforcing) return;
    QScopedValueRollback<bool> guard(m_enforcing, true);
    m_deferred.stop();
    const auto windows = m_windows;
    bool anyVisible = false;
    QWindow* lastScene = nullptr;
    // Stable front-to-back order prevents multiple overlays continually
    // raising one another. Never activate or show from an enforcement tick.
    for (const Entry& entry : windows) {
        if (!entry.scene || !entry.active || !visible(entry.window)) continue;
        anyVisible = true;
#if defined(Q_OS_MACOS)
        MacWindowManager::setWindowAsGlobalOverlay(entry.window);
#elif defined(Q_OS_WIN)
        WindowsWindowManager::keepAboveAndOnAllDesktops(entry.window, lastScene);
#endif
        lastScene = entry.window;
    }
    for (const Entry& entry : windows) {
        if (entry.scene || !visible(entry.window)) continue;
        anyVisible = true;
#if defined(Q_OS_MACOS)
        MacWindowManager::setWindowAlwaysOnTop(entry.window);
#elif defined(Q_OS_WIN)
        WindowsWindowManager::keepAboveAndOnAllDesktops(entry.window, lastScene, true);
#endif
    }
    if (anyVisible) {
        if (!m_timer.isActive()) m_timer.start();
    } else {
        m_timer.stop();
    }
}

bool WindowStackingCoordinator::eventFilter(QObject* watched, QEvent* event)
{
    if (m_windows.isEmpty() || m_enforcing || !qobject_cast<QWindow*>(watched)) return false;
    switch (event->type()) {
    case QEvent::Show:
    case QEvent::Hide:
    case QEvent::WindowActivate:
    case QEvent::ZOrderChange:
    case QEvent::WindowStateChange:
    case QEvent::WinIdChange:
        scheduleEnforcement();
        break;
    case QEvent::PlatformSurface:
        if (static_cast<QPlatformSurfaceEvent*>(event)->surfaceEventType()
            == QPlatformSurfaceEvent::SurfaceCreated) scheduleEnforcement();
        break;
    default:
        break;
    }
    return false;
}

bool WindowStackingCoordinator::nativeEventFilter(const QByteArray&, void* message, qintptr*)
{
#ifdef Q_OS_WIN
    // Constrain native menus/pickers too, before Windows performs their raise.
    // A delayed timer alone lets the control window flash above the scene.
    if (QGuiApplication::platformName() != QLatin1String("windows")) return false;
    const auto* msg = static_cast<MSG*>(message);
    if (msg->message != WM_WINDOWPOSCHANGING) return false;
    auto* position = reinterpret_cast<WINDOWPOS*>(msg->lParam);
    if (!position || (position->flags & (SWP_NOZORDER | SWP_HIDEWINDOW))) return false;
    if (GetWindowLongPtr(msg->hwnd, GWL_STYLE) & WS_CHILD) return false;
    if (!(GetWindowLongPtr(msg->hwnd, GWL_EXSTYLE) & WS_EX_TOPMOST)
        && position->hwndInsertAfter != HWND_TOPMOST) return false;
    if (position->hwndInsertAfter == HWND_BOTTOM || position->hwndInsertAfter == HWND_NOTOPMOST)
        return false;

    QList<HWND> overlays;
    for (const Entry& entry : m_windows) {
        if (!entry.scene || !entry.active || !visible(entry.window)) continue;
        const HWND handle = reinterpret_cast<HWND>(entry.window->winId());
        if (handle == msg->hwnd) return false;
        if (IsWindowVisible(handle)) overlays.append(handle);
    }
    HWND lastOverlay = nullptr;
    bool requestedBeforeOverlay = position->hwndInsertAfter == HWND_TOP
        || position->hwndInsertAfter == HWND_TOPMOST;
    bool passedRequested = requestedBeforeOverlay;
    for (HWND handle = GetTopWindow(nullptr); handle; handle = GetWindow(handle, GW_HWNDNEXT)) {
        if (handle == position->hwndInsertAfter) passedRequested = true;
        if (overlays.contains(handle)) {
            lastOverlay = handle;
            if (passedRequested && handle != position->hwndInsertAfter) requestedBeforeOverlay = true;
        }
    }
    if (lastOverlay && requestedBeforeOverlay) position->hwndInsertAfter = lastOverlay;
#else
    Q_UNUSED(message);
#endif
    return false;
}
