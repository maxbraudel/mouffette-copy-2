#ifndef WINDOWEVENTHANDLER_H
#define WINDOWEVENTHANDLER_H

#include <QObject>
#include <QCloseEvent>
#include <QShowEvent>
#include <QHideEvent>
#include <QResizeEvent>
#include <QSystemTrayIcon>

class MainWindow;

/**
 * @brief Handler for window lifecycle events
 * 
 * Manages:
 * - Window show/hide/close events
 * - Window resize and state changes
 * - Application suspension state
 * - System tray icon interactions
 * 
 * Phase 9: Extracted from MainWindow to reduce complexity
 */
class WindowEventHandler : public QObject
{
    Q_OBJECT

public:
    explicit WindowEventHandler(MainWindow* mainWindow, QObject* parent = nullptr);
    ~WindowEventHandler() override = default;

    // Window event handlers
    void handleCloseEvent(QCloseEvent* event);
    void handleShowEvent(QShowEvent* event);
    void handleHideEvent(QHideEvent* event);
    void handleResizeEvent(QResizeEvent* event);
    void handleChangeEvent(QEvent* event);
    
    // Application state management
    void handleApplicationStateChanged(Qt::ApplicationState state);
    void updateApplicationSuspendedState(bool suspended);
    
    // System tray
    void onTrayIconActivated(QSystemTrayIcon::ActivationReason reason);

private:
    MainWindow* m_mainWindow;
};

#endif // WINDOWEVENTHANDLER_H
