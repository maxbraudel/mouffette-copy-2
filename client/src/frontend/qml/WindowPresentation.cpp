#include "frontend/qml/WindowPresentation.h"

#include <QCursor>
#include <QEvent>
#include <QGuiApplication>
#include <QPlatformSurfaceEvent>
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
    // Reassert without activating: another topmost app, Qt flag changes or an
    // Explorer restart must not permanently demote an open control window.
    m_priorityTimer.setInterval(500);
    connect(&m_priorityTimer, &QTimer::timeout, this, &WindowPresentation::enforcePriority);
}

void WindowPresentation::setWindow(QWindow* window)
{
    if (m_window == window) return;
    m_priorityTimer.stop();
    if (m_window) {
        m_window->removeEventFilter(this);
        disconnect(m_window, nullptr, this, nullptr);
    }
    m_window = window;
    if (window) {
        window->setFlag(Qt::WindowStaysOnTopHint, true);
        window->installEventFilter(this);
        connect(window, &QWindow::visibilityChanged, this,
                &WindowPresentation::updateEnforcement);
        connect(window, &QWindow::screenChanged, this,
                &WindowPresentation::updateEnforcement);
        connect(window, &QObject::destroyed, this, [this] { m_priorityTimer.stop(); });
        updateEnforcement();
    }
    emit windowChanged();
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
    m_window->show();
    // Native decoration sizes may only be known once the window is shown.
    if (reopening) fitToScreen(screen);
    enforcePriority();
    m_window->raise();
    m_window->requestActivate();
#ifdef Q_OS_MACOS
    MacWindowManager::activateApplicationWindow(m_window);
#endif
}

void WindowPresentation::updateEnforcement()
{
    if (!m_window || !m_window->isVisible() || m_window->windowState() == Qt::WindowMinimized) {
        m_priorityTimer.stop();
        return;
    }
    enforcePriority();
    m_priorityTimer.start();
}

void WindowPresentation::enforcePriority()
{
    if (!m_window || !m_window->isVisible() || m_window->windowState() == Qt::WindowMinimized) return;
#if defined(Q_OS_MACOS)
    MacWindowManager::setWindowAlwaysOnTop(m_window);
#elif defined(Q_OS_WIN)
    WindowsWindowManager::keepAboveAndOnAllDesktops(m_window);
#endif
}

bool WindowPresentation::eventFilter(QObject* watched, QEvent* event)
{
    if (watched == m_window) {
        bool reapply = event->type() == QEvent::Show
            || event->type() == QEvent::WinIdChange
            || event->type() == QEvent::WindowStateChange;
        if (event->type() == QEvent::PlatformSurface) {
            reapply = static_cast<QPlatformSurfaceEvent*>(event)->surfaceEventType()
                == QPlatformSurfaceEvent::SurfaceCreated;
        }
        if (reapply) {
            // Qt's platform plugin must finish creating/updating the native
            // window before its default level/collection behavior is overridden.
            QTimer::singleShot(0, this, &WindowPresentation::updateEnforcement);
        }
    }
    return QObject::eventFilter(watched, event);
}
