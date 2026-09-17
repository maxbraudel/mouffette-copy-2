#include "frontend/qml/WindowPresentation.h"
#include "backend/platform/WindowStackingCoordinator.h"

#include <QCursor>
#include <QGuiApplication>
#include <QScreen>
#include <QtQml/qqml.h>

#if defined(Q_OS_MACOS)
#include "backend/platform/macos/MacWindowManager.h"
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
        window->setFlags(window->flags() | Qt::WindowTitleHint
                         | Qt::WindowSystemMenuHint | Qt::WindowMinMaxButtonsHint
                         | Qt::WindowCloseButtonHint | Qt::WindowStaysOnTopHint);
        stacking.registerControlWindow(window);
        stacking.enforce();
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
    m_window->raise();
    m_window->requestActivate();
#ifdef Q_OS_MACOS
    MacWindowManager::activateApplicationWindow(m_window);
#endif
    WindowStackingCoordinator::instance().enforce();
}
