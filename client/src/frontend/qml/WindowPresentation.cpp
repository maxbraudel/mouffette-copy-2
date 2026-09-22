#include "frontend/qml/WindowPresentation.h"
#include "backend/platform/WindowStackingCoordinator.h"

#include <QCursor>
#include <QGuiApplication>
#include <QScreen>
#include <QtQml/qqml.h>

#if defined(Q_OS_WIN)
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
#ifdef Q_OS_MACOS
        // A standard Qt window lets AppKit supply the native title bar and
        // close/minimize/fullscreen controls without panel customizations.
        auto flags = Qt::WindowFlags(Qt::Window);
#else
        // Explicit decorations are needed when Qt's Windows backend receives
        // additional flags (StaysOnTop otherwise suppresses the title bar).
        auto flags = window->flags() | Qt::WindowTitleHint
                         | Qt::WindowSystemMenuHint | Qt::WindowMinMaxButtonsHint
                         | Qt::WindowCloseButtonHint;
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
    QPointer<QScreen> screen = QGuiApplication::screenAt(QCursor::pos());
    if (!screen) screen = QGuiApplication::primaryScreen();
    if (!screen) screen = m_window->screen();
    if (reopening) {
        m_window->setWindowState(Qt::WindowNoState);
        m_window->setScreen(screen);
        m_window->create();
        fitToScreen(screen);
    }
#ifdef Q_OS_WIN
    else if (screen && m_window->screen() != screen) {
        // An already open window follows the monitor containing the tray click.
        const auto state = m_window->windowState();
        if (state == Qt::WindowMaximized || state == Qt::WindowFullScreen)
            m_window->setWindowState(Qt::WindowNoState);
        const QSize previousSize = m_window->size();
        m_window->setScreen(screen);
        const QRect available = screen->availableGeometry();
        const QMargins margins = m_window->frameMargins();
        const QSize frameSize = (previousSize + QSize(margins.left() + margins.right(),
                                                     margins.top() + margins.bottom()))
                                    .boundedTo(available.size());
        const QPoint framePosition(available.x() + (available.width() - frameSize.width()) / 2,
                                   available.y() + (available.height() - frameSize.height()) / 2);
        m_window->setGeometry(QRect(framePosition + QPoint(margins.left(), margins.top()),
                                    QSize(qMax(1, frameSize.width() - margins.left() - margins.right()),
                                          qMax(1, frameSize.height() - margins.top() - margins.bottom()))));
        if (state == Qt::WindowFullScreen) m_window->setWindowState(Qt::WindowFullScreen);
        else if (state == Qt::WindowMaximized) m_window->setWindowState(Qt::WindowMaximized);
    }
#endif
    m_window->create();
    auto& stacking = WindowStackingCoordinator::instance();
    stacking.configureControlWindow(m_window);
#ifdef Q_OS_WIN
    if (m_window->isVisible()) WindowsWindowManager::moveToCurrentDesktop(m_window);
#endif
    m_window->show();
    stacking.configureControlWindow(m_window);
#ifdef Q_OS_WIN
    // The shell may assign a newly shown HWND its desktop only after show().
    WindowsWindowManager::moveToCurrentDesktop(m_window);
#endif
    // Native decoration sizes may only be known once the window is shown.
    if (reopening) fitToScreen(screen);
    m_window->raise();
    m_window->requestActivate();
    WindowStackingCoordinator::instance().enforce();
}
