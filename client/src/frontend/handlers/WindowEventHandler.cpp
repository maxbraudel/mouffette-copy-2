#include "WindowEventHandler.h"
#include "MainWindow.h"
#include "frontend/rendering/navigation/ScreenNavigationManager.h"
#include "frontend/ui/layout/ResponsiveLayoutManager.h"
#include "backend/domain/models/ClientInfo.h"
#include "shared/rendering/ICanvasHost.h"
#include "frontend/rendering/canvas/ScreenCanvas.h"
#include "frontend/ui/pages/CanvasViewPage.h"
#include <QDebug>

WindowEventHandler::WindowEventHandler(MainWindow* mainWindow, QObject* parent)
    : QObject(parent)
    , m_mainWindow(mainWindow)
{
}

void WindowEventHandler::handleCloseEvent(QCloseEvent* event)
{
    // Closing the window hides the active local Project. Incoming receiver
    // availability is intentionally unaffected.
    m_mainWindow->hide();
    event->ignore(); // do not quit app
}

void WindowEventHandler::handleShowEvent(QShowEvent* event)
{
    Q_UNUSED(event);

    m_windowSuspended = (m_mainWindow->windowState() & Qt::WindowMinimized)
        || !m_mainWindow->isVisible();
    applyCombinedSuspendedState();
}

void WindowEventHandler::handleHideEvent(QHideEvent* event)
{
    Q_UNUSED(event);
    m_windowSuspended = true;
    applyCombinedSuspendedState();
}

void WindowEventHandler::handleResizeEvent(QResizeEvent* event)
{
    Q_UNUSED(event);
    
    // Update responsive layout based on window width
    if (m_mainWindow->getResponsiveLayoutManager()) {
        m_mainWindow->getResponsiveLayoutManager()->updateResponsiveLayout();
    }
    
    // If we're currently showing the screen view and have a canvas with content,
    // recenter the view to maintain good visibility only before first reveal or when no screens are present
    QStackedWidget* stackedWidget = m_mainWindow->getStackedWidget();
    CanvasViewPage* canvasViewPage = m_mainWindow->getCanvasViewPage();
    ICanvasHost* screenCanvas = m_mainWindow->getScreenCanvas();
    
    if (stackedWidget && stackedWidget->currentWidget() == canvasViewPage && screenCanvas) {
        const bool hasScreens = !m_mainWindow->getSelectedClient().getScreens().isEmpty();
        if (!m_mainWindow->isCanvasRevealedForCurrentClient() && hasScreens) {
            screenCanvas->recenterWithMargin(53);
        }
    }
}

void WindowEventHandler::handleChangeEvent(QEvent* event)
{
    if (event->type() == QEvent::WindowStateChange) {
        const bool minimized = (m_mainWindow->windowState() & Qt::WindowMinimized);
        m_windowSuspended = minimized || m_mainWindow->isHidden();
        applyCombinedSuspendedState();
    }
}

void WindowEventHandler::handleApplicationStateChanged(Qt::ApplicationState state)
{
    m_qtApplicationSuspended =
        state == Qt::ApplicationHidden || state == Qt::ApplicationSuspended;
    applyCombinedSuspendedState();
}

void WindowEventHandler::updateApplicationSuspendedState(bool suspended)
{
    m_windowSuspended = suspended;
    applyCombinedSuspendedState();
}

void WindowEventHandler::updateNativeSystemSuspendedState(bool suspended)
{
    m_nativeSystemSuspended = suspended;
    applyCombinedSuspendedState();
}

void WindowEventHandler::applyCombinedSuspendedState()
{
    const bool suspended = m_windowSuspended || m_qtApplicationSuspended
        || m_nativeSystemSuspended;
    if (m_mainWindow->isApplicationSuspended() == suspended) {
        return;
    }
    m_mainWindow->setApplicationSuspended(suspended);
    ScreenCanvas::setAllCanvasesSuspended(suspended);
}

void WindowEventHandler::onTrayIconActivated(QSystemTrayIcon::ActivationReason reason)
{
    // Show/hide window on any click (left, right, or double-click)
    switch (reason) {
    case QSystemTrayIcon::Trigger:        // Single left-click
    case QSystemTrayIcon::DoubleClick:    // Double left-click  
    case QSystemTrayIcon::Context:        // Right-click
        {
            const bool minimized = (m_mainWindow->windowState() & Qt::WindowMinimized);
            const bool hidden = m_mainWindow->isHidden() || !m_mainWindow->isVisible();
            if (minimized || hidden) {
                // Reveal and focus the window if minimized or hidden
                if (minimized) {
                    m_mainWindow->setWindowState(m_mainWindow->windowState() & ~Qt::WindowMinimized);
                    m_mainWindow->showNormal();
                }
                m_mainWindow->show();
                m_mainWindow->raise();
                m_mainWindow->activateWindow();
            } else {
                // Fully visible: toggle to hide to tray
                m_mainWindow->hide();
            }
        }
        break;
    default:
        break;
    }
}
