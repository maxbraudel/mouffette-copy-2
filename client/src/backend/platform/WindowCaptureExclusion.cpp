#include "backend/platform/WindowCaptureExclusion.h"

#include <QEvent>
#include <QGuiApplication>
#include <QOperatingSystemVersion>
#include <QPlatformSurfaceEvent>
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
    qApp->installNativeEventFilter(this);
}

WindowCaptureExclusion::~WindowCaptureExclusion()
{
    if (qApp) {
        qApp->removeEventFilter(this);
        qApp->removeNativeEventFilter(this);
    }
}

QList<QWindow*> WindowCaptureExclusion::sceneWindows() const
{
    QList<QWindow*> windows;
    for (const auto& window : m_sceneWindows)
        if (window) windows.append(window.data());
    return windows;
}

void WindowCaptureExclusion::setSceneWindow(QWindow* window, bool scene)
{
    if (!window) return;
    const bool wasScene = m_sceneWindows.contains(window);
    if (wasScene == scene) {
        applyWindow(window);
        return;
    }
    if (scene) {
        m_sceneWindows.append(window);
        connect(window, &QObject::destroyed, this, [this] {
            const auto count = m_sceneWindows.size();
            m_sceneWindows.removeIf([](const auto& candidate) { return candidate.isNull(); });
            if (count != m_sceneWindows.size()) emit sceneWindowsChanged();
        });
    } else {
        m_sceneWindows.removeAll(window);
    }
    applyWindow(window);
    emit sceneWindowsChanged();
}

void WindowCaptureExclusion::failCapture(const QString& reason)
{
    if (m_captureError == reason) return;
    m_captureError = reason;
    emit captureSafetyChanged(false, m_captureError);
}

#ifdef Q_OS_WIN
bool WindowCaptureExclusion::applyNativeWindow(quintptr handle)
{
    const auto window = reinterpret_cast<HWND>(handle);
    if (!IsWindow(window) || (GetWindowLongPtr(window, GWL_STYLE) & WS_CHILD)) return true;
    DWORD processId = 0;
    GetWindowThreadProcessId(window, &processId);
    if (processId != GetCurrentProcessId()) return true;
    const auto version = QOperatingSystemVersion::current();
    if (version.majorVersion() < 10 || (version.majorVersion() == 10 && version.microVersion() < 19041)) {
        failCapture(QStringLiteral("Excluding Mouffette controls from screen sharing requires Windows 10 version 2004 or later."));
        return false;
    }
    bool scene = false;
    for (const auto& candidate : m_sceneWindows) {
        if (candidate && candidate->handle() && candidate->winId() == handle) {
            scene = true;
            break;
        }
    }
    // The numeric value also builds with older Windows SDK headers. Its use
    // remains gated above: older Windows silently falls back to a black box.
    constexpr DWORD excludeFromCapture = 0x00000011;
    const DWORD affinity = scene ? WDA_NONE : excludeFromCapture;
    DWORD actual = WDA_NONE;
    if (GetWindowDisplayAffinity(window, &actual) && actual == affinity) return true;
    // GetWindowDisplayAffinity is documented to require a layered window.
    // Ordinary Qt control windows need not be layered, so a successful setter
    // is authoritative even when reading the affinity back is unsupported.
    if (SetWindowDisplayAffinity(window, affinity)) return true;
    failCapture(QStringLiteral("Windows could not exclude Mouffette controls from screen sharing (error %1).")
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
        QScopedValueRollback<bool> guard(m_applying, true);
        struct Inventory {
            WindowCaptureExclusion* coordinator;
            QSet<quintptr> qtWindows;
        } inventory{this, {}};
        for (QWindow* window : QGuiApplication::topLevelWindows())
            if (window->handle()) inventory.qtWindows.insert(window->winId());
        // Include visible native windows and all Qt surfaces, even hidden
        // controls that can reopen. Hidden OS/Qt infrastructure HWNDs (tray,
        // clipboard, message dispatch) have no content to exclude and can
        // reject affinity. A native SWP_SHOWWINDOW is protected separately.
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
            failCapture(QStringLiteral("Excluding Mouffette controls from screen sharing requires Windows 10 version 2004 or later."));
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
    bool surfaceChanged = false;
    switch (event->type()) {
    case QEvent::Show:
    case QEvent::WinIdChange:
        applyWindow(window);
        surfaceChanged = true;
        break;
    case QEvent::PlatformSurface:
        if (static_cast<QPlatformSurfaceEvent*>(event)->surfaceEventType()
            == QPlatformSurfaceEvent::SurfaceCreated) applyWindow(window);
        surfaceChanged = true;
        break;
    default:
        break;
    }
    if (surfaceChanged && m_sceneWindows.contains(window)) emit sceneWindowsChanged();
    return false;
}

bool WindowCaptureExclusion::nativeEventFilter(const QByteArray&, void* message, qintptr*)
{
#ifdef Q_OS_WIN
    if (m_applying || QGuiApplication::platformName() != QLatin1String("windows")) return false;
    const auto* event = static_cast<MSG*>(message);
    if (event->message == WM_WINDOWPOSCHANGING) {
        const auto* position = reinterpret_cast<WINDOWPOS*>(event->lParam);
        if (!position || !(position->flags & SWP_SHOWWINDOW)) return false;
        QScopedValueRollback<bool> guard(m_applying, true);
        applyNativeWindow(reinterpret_cast<quintptr>(event->hwnd));
    }
#else
    Q_UNUSED(message);
#endif
    return false;
}
