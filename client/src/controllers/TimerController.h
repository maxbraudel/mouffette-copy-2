#ifndef TIMERCONTROLLER_H
#define TIMERCONTROLLER_H

#include <QObject>
#include <QTimer>

class MainWindow;

/**
 * [PHASE 10] TimerController
 * Manages all timer setup, configuration, and callbacks for MainWindow.
 * Handles: status update timer, display sync timer, reconnect timer, cursor update timer.
 */
class TimerController : public QObject {
    Q_OBJECT

public:
    explicit TimerController(MainWindow* mainWindow, QObject* parent = nullptr);
    ~TimerController() = default;

    // Timer setup and initialization
    void setupTimers();
    
    // Reconnection timer management
    void scheduleReconnect();
    void attemptReconnect();
    void resetReconnectState();
    
    // Watch state management (controls display sync and cursor timers)
    void setWatchedState(bool watched);
    
    // Cursor update interval configuration
    void setCursorUpdateInterval(int intervalMs);

private:
    MainWindow* m_mainWindow;
    
    // Timer callbacks
    void onDisplaySyncTimeout();
    void onCursorTimeout();
};

#endif // TIMERCONTROLLER_H
