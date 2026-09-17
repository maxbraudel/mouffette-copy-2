#include "frontend/qml/WindowPresentation.h"
#include "backend/platform/WindowStackingCoordinator.h"

#include <QCursor>
#include <QGuiApplication>
#include <QScreen>
#include <QtQml/qqml.h>

#if defined(Q_OS_MACOS)
#include "backend/platform/macos/MacWindowManager.h"
#elif defined(Q_OS_WIN)
#include "backend/platform/windows/WindowsWindowManager.h"
#endif

namespace {
void registerWindowPresentation()
{
    qmlRegisterType<WindowPresentation>("Mouffette.App", 1, 0, "WindowPresentation");
}
}
Q_COREAPP_STARTUP_FUNCTION(registerWindowPresentation)

WindowPresentation::WindowPresentation(QObject* parent) : QObject(parent)
{
}

WindowPresentation::~WindowPresentation()
{
    if (m_window) WindowStackingCoordinator::instance().unregisterWindow(m_window);
}

void WindowPresentation::setWindow(QWindow* window)
{
    if (m_window == window) return;
    auto& stacking = WindowStackingCoordinator::instance();
    if (m_window) stacking.unregisterWindow(m_window);
    m_window = window;
    if (window) {
        // Explicit decorations are needed when Qt's Windows backend receives
        // additional flags (StaysOnTop otherwise suppresses the title bar).
        auto flags = window->flags() | Qt::WindowTitleHint
                         | Qt::WindowSystemMenuHint | Qt::WindowMinMaxButtonsHint
                         | Qt::WindowCloseButtonHint;
#ifdef Q_OS_MACOS
        // Keep Qt from replacing FullScreenAuxiliary with FullScreenPrimary
        // when it creates/updates the native window.
        // Cocoa only admits auxiliary panels into another application's
        // fullscreen Space. Keep one Qt-owned QNSPanel in both priority modes;
        // changing its level does not replace the Quick window or its scene.
        // Dialog gives that panel standard macOS title-bar buttons. Tool adds
        // NSWindowStyleMaskUtilityWindow, shrinking the title bar and buttons.
        // Window type alone does not make the window modal.
        flags = (flags & ~Qt::WindowType_Mask) | Qt::Dialog | Qt::CustomizeWindowHint;
        flags &= ~Qt::WindowFullscreenButtonHint;
#endif
        flags.setFlag(Qt::WindowStaysOnTopHint, m_alwaysOnTop);
        window->setFlags(flags);
        stacking.registerControlWindow(window, m_alwaysOnTop);
        stacking.enforce();
    }
    emit windowChanged();
}

void WindowPresentation::setAlwaysOnTop(bool enabled)
{
    if (m_alwaysOnTop == enabled) return;
    m_alwaysOnTop = enabled;
    if (m_window) {
        auto& stacking = WindowStackingCoordinator::instance();
        stacking.setControlAlwaysOnTop(m_window, enabled);
        m_window->setFlag(Qt::WindowStaysOnTopHint, enabled);
        stacking.configureControlWindow(m_window);
        stacking.enforce();
    }
    emit alwaysOnTopChanged();
}

QRect WindowPresentation::openingGeometry(const QRect& available, const QMargins& margins)
{
    // Qt screen coordinates are logical pixels, including on mixed-DPI setups.
    // Include the title bar/borders in the 90%, not just the content area.
    const QSize frameSize(qMax(1, qRound(available.width() * 0.9)),
                          qMax(1, qRound(available.height() * 0.9)));
    const QPoint framePosition(available.x() + (available.width() - frameSize.width()) / 2,
                               available.y() + (available.height() - frameSize.height()) / 2);
    return QRect(framePosition + QPoint(margins.left(), margins.top()),
                 QSize(qMax(1, frameSize.width() - margins.left() - margins.right()),
                       qMax(1, frameSize.height() - margins.top() - margins.bottom())));
}

void WindowPresentation::fitToScreen(QScreen* screen)
{
    if (!m_window || !screen) return;
    m_window->setScreen(screen);
    m_window->setGeometry(openingGeometry(screen->availableGeometry(), m_window->frameMargins()));
}

void WindowPresentation::open()
{
    if (!m_window) return;
    const bool reopening = !m_window->isVisible() || m_window->windowState() == Qt::WindowMinimized;
    QPointer<QScreen> screen;
    if (reopening) {
        screen = QGuiApplication::screenAt(QCursor::pos());
        if (!screen) screen = m_window->screen();
        if (!screen) screen = QGuiApplication::primaryScreen();
        m_window->setWindowState(Qt::WindowNoState);
        m_window->setScreen(screen);
        m_window->create();
        fitToScreen(screen);
    }
    m_window->create();
    auto& stacking = WindowStackingCoordinator::instance();
    stacking.configureControlWindow(m_window);
#ifdef Q_OS_WIN
    WindowsWindowManager::moveToCurrentDesktop(m_window);
#endif
    m_window->show();
    stacking.configureControlWindow(m_window);
    // Native decoration sizes may only be known once the window is shown.
    if (reopening) fitToScreen(screen);
#ifdef Q_OS_MACOS
    // QCocoaWindow::raise() activates the whole process and can switch Spaces.
    MacWindowManager::activateApplicationWindow(m_window);
#else
    m_window->raise();
    m_window->requestActivate();
#endif
    WindowStackingCoordinator::instance().enforce();
}
