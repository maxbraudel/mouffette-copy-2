#include "TimerController.h"
#include "../MainWindow.h"
#include "../WebSocketClient.h"
#include <QRandomGenerator>
#include <QtMath>
#include <QCursor>
#include <QDebug>

#ifdef Q_OS_WIN
#include <windows.h>
#endif

TimerController::TimerController(MainWindow* mainWindow, QObject* parent)
    : QObject(parent)
    , m_mainWindow(mainWindow)
{
}

void TimerController::setupTimers() {
    QTimer* statusUpdateTimer = m_mainWindow->getStatusUpdateTimer();
    QTimer* displaySyncTimer = m_mainWindow->getDisplaySyncTimer();
    QTimer* reconnectTimer = m_mainWindow->getReconnectTimer();
    
    // Periodic connection status refresh no longer needed (now event-driven); keep timer disabled
    statusUpdateTimer->stop();

    // Periodic display sync only when watched
    displaySyncTimer->setInterval(3000);
    connect(displaySyncTimer, &QTimer::timeout, this, &TimerController::onDisplaySyncTimeout);
    // Don't start automatically - will be started when watched

    // Smart reconnect timer
    reconnectTimer->setSingleShot(true);
    connect(reconnectTimer, &QTimer::timeout, this, &TimerController::attemptReconnect);
}

void TimerController::scheduleReconnect() {
    if (m_mainWindow->isUserDisconnected()) {
        return; // Don't reconnect if user disabled the client
    }
    
    int reconnectAttempts = m_mainWindow->getReconnectAttempts();
    int maxReconnectDelay = m_mainWindow->getMaxReconnectDelay();
    
    // Exponential backoff: 2^attempts seconds, capped at maxReconnectDelay
    int delay = qMin(static_cast<int>(qPow(2, reconnectAttempts)) * 1000, maxReconnectDelay);
    
    // Add some jitter to avoid thundering herd (±25%)
    int jitter = QRandomGenerator::global()->bounded(-delay/4, delay/4);
    delay += jitter;
    
    qDebug() << "Scheduling reconnect attempt" << (reconnectAttempts + 1) << "in" << delay << "ms";
    
    QTimer* reconnectTimer = m_mainWindow->getReconnectTimer();
    reconnectTimer->start(delay);
    m_mainWindow->incrementReconnectAttempts();
}

void TimerController::attemptReconnect() {
    if (m_mainWindow->isUserDisconnected()) {
        return; // Don't reconnect if user disabled the client
    }
    
    qDebug() << "Attempting reconnection...";
    m_mainWindow->connectToServer();
}

void TimerController::resetReconnectState() {
    m_mainWindow->resetReconnectAttempts();
    QTimer* reconnectTimer = m_mainWindow->getReconnectTimer();
    reconnectTimer->stop();
}

void TimerController::setWatchedState(bool watched) {
    m_mainWindow->setIsWatched(watched);
    
    QTimer* displaySyncTimer = m_mainWindow->getDisplaySyncTimer();
    
    // Start/stop display sync timer based on watch status to prevent unnecessary canvas reloads
    if (watched) {
        // Immediately push a fresh snapshot so watchers don't wait for the first 3s tick
        WebSocketClient* client = m_mainWindow->getWebSocketClient();
        if (client && client->isConnected()) {
            m_mainWindow->syncRegistration();
        }
        if (!displaySyncTimer->isActive()) {
            displaySyncTimer->start();
        }
    } else {
        if (displaySyncTimer->isActive()) {
            displaySyncTimer->stop();
        }
    }
    
    qDebug() << "Watch status changed:" << (watched ? "watched" : "not watched");

    // Begin/stop sending our cursor position to watchers (target side)
    QTimer* cursorTimer = m_mainWindow->getCursorTimer();
    if (watched) {
        if (!cursorTimer) {
            cursorTimer = new QTimer(m_mainWindow);
            m_mainWindow->setCursorTimer(cursorTimer);
            int cursorUpdateInterval = m_mainWindow->getCursorUpdateIntervalMs();
            cursorTimer->setInterval(cursorUpdateInterval);
            connect(cursorTimer, &QTimer::timeout, this, &TimerController::onCursorTimeout);
        }
        // Apply any updated interval before starting
        int cursorUpdateInterval = m_mainWindow->getCursorUpdateIntervalMs();
        cursorTimer->setInterval(cursorUpdateInterval);
        if (!cursorTimer->isActive()) {
            cursorTimer->start();
        }
    } else {
        if (cursorTimer) {
            cursorTimer->stop();
        }
    }
}

void TimerController::setCursorUpdateInterval(int intervalMs) {
    m_mainWindow->setCursorUpdateIntervalMs(intervalMs);
    QTimer* cursorTimer = m_mainWindow->getCursorTimer();
    if (cursorTimer) {
        cursorTimer->setInterval(intervalMs);
    }
}

void TimerController::onDisplaySyncTimeout() {
    if (m_mainWindow->isWatched()) {
        WebSocketClient* client = m_mainWindow->getWebSocketClient();
        if (client && client->isConnected()) {
            m_mainWindow->syncRegistration();
        }
    }
}

void TimerController::onCursorTimeout() {
    static int lastX = INT_MIN, lastY = INT_MIN;
#ifdef Q_OS_WIN
    POINT pt{0,0};
    const BOOL ok = GetCursorPos(&pt);
    const int gx = ok ? static_cast<int>(pt.x) : QCursor::pos().x();
    const int gy = ok ? static_cast<int>(pt.y) : QCursor::pos().y();
    if (gx != lastX || gy != lastY) {
        lastX = gx; lastY = gy;
        WebSocketClient* client = m_mainWindow->getWebSocketClient();
        if (client && client->isConnected() && m_mainWindow->isWatched()) {
            client->sendCursorUpdate(gx, gy);
        }
    }
#else
    const QPoint p = QCursor::pos();
    if (p.x() != lastX || p.y() != lastY) {
        lastX = p.x();
        lastY = p.y();
        WebSocketClient* client = m_mainWindow->getWebSocketClient();
        if (client && client->isConnected() && m_mainWindow->isWatched()) {
            client->sendCursorUpdate(p.x(), p.y());
        }
    }
#endif
}
