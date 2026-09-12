#ifndef TIMERCONTROLLER_H
#define TIMERCONTROLLER_H

#include <QObject>
#include <QTimer>

class MainWindow;

/**
 * [PHASE 10] TimerController
 * Manages all timer setup, configuration, and callbacks for MainWindow.
 * Handles the periodic authoritative device-snapshot publication.
 */
class TimerController : public QObject {
    Q_OBJECT

public:
    explicit TimerController(MainWindow* mainWindow, QObject* parent = nullptr);
    ~TimerController() = default;

    // Timer setup and initialization
    void setupTimers();
    
private:
    MainWindow* m_mainWindow;
    
    // Timer callbacks
    void onDisplaySyncTimeout();
};

#endif // TIMERCONTROLLER_H
