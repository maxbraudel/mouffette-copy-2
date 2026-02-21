#include "backend/controllers/TimerController.h"
#include "MainWindow.h"
#include "backend/network/WebSocketClient.h"
#include <QRandomGenerator>
#include <QtMath>
#include <QCursor>
#include <QDebug>
#include <QGuiApplication>
#include <QScreen>
#include <algorithm>
#include <cmath>

#ifdef Q_OS_WIN
#include <windows.h>
#endif

namespace {
bool cursorDebugEnabled() {
    static const bool enabled = qEnvironmentVariableIsSet("MOUFFETTE_CURSOR_DEBUG");
    return enabled;
}
}

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
    int screenId = -1;
    qreal normalizedX = -1.0;
    qreal normalizedY = -1.0;
    QScreen* logicalScreen = nullptr;

    auto computeNormalizedCursorFromLogical = [&](const QPoint& logicalPos) {
        QScreen* screen = QGuiApplication::screenAt(logicalPos);
        if (!screen) {
            return;
        }

        const QList<QScreen*> allScreens = QGuiApplication::screens();
        const int idx = allScreens.indexOf(screen);
        if (idx < 0) {
            return;
        }

        const QRect geometry = screen->geometry();
        if (geometry.width() <= 0 || geometry.height() <= 0) {
            return;
        }

        screenId = idx;
        logicalScreen = screen;

        const int maxLogicalX = std::max(1, geometry.width() - 1);
        const int maxLogicalY = std::max(1, geometry.height() - 1);
        const int localLogicalX = std::clamp(logicalPos.x() - geometry.x(), 0, maxLogicalX);
        const int localLogicalY = std::clamp(logicalPos.y() - geometry.y(), 0, maxLogicalY);
        normalizedX = static_cast<qreal>(localLogicalX) / static_cast<qreal>(maxLogicalX);
        normalizedY = static_cast<qreal>(localLogicalY) / static_cast<qreal>(maxLogicalY);
        normalizedX = std::clamp(normalizedX, 0.0, 1.0);
        normalizedY = std::clamp(normalizedY, 0.0, 1.0);
        if (cursorDebugEnabled()) {
            qDebug() << "[CursorDebug][Sender][LogicalBasis]"
                     << "logicalPos=" << logicalPos
                     << "screenId=" << screenId
                     << "screenGeom=" << geometry
                     << "norm=" << normalizedX << normalizedY;
        }
    };

    auto computeNormalizedCursorFromPhysical = [&](int physicalX, int physicalY) {
        const QList<ScreenInfo> screens = m_mainWindow->getLocalScreenInfo();
        for (const ScreenInfo& screen : screens) {
            if (screen.width <= 0 || screen.height <= 0) {
                continue;
            }

            const QRect screenRect(screen.x, screen.y, screen.width, screen.height);
            if (!screenRect.contains(physicalX, physicalY)) {
                continue;
            }

            screenId = screen.id;
            const int maxPhysicalX = std::max(1, screen.width - 1);
            const int maxPhysicalY = std::max(1, screen.height - 1);
            const int localPhysicalX = std::clamp(physicalX - screen.x, 0, maxPhysicalX);
            const int localPhysicalY = std::clamp(physicalY - screen.y, 0, maxPhysicalY);
            normalizedX = static_cast<qreal>(localPhysicalX) / static_cast<qreal>(maxPhysicalX);
            normalizedY = static_cast<qreal>(localPhysicalY) / static_cast<qreal>(maxPhysicalY);
            normalizedX = std::clamp(normalizedX, 0.0, 1.0);
            normalizedY = std::clamp(normalizedY, 0.0, 1.0);
            if (cursorDebugEnabled()) {
                qDebug() << "[CursorDebug][Sender][PhysicalBasis]"
                         << "physical=" << QPoint(physicalX, physicalY)
                         << "screenId=" << screenId
                         << "screenRect=" << screenRect
                         << "norm=" << normalizedX << normalizedY;
            }
            return;
        }
    };

#ifdef Q_OS_WIN
    POINT pt{0,0};
    const BOOL ok = GetCursorPos(&pt);
    const int gx = ok ? static_cast<int>(pt.x) : QCursor::pos().x();
    const int gy = ok ? static_cast<int>(pt.y) : QCursor::pos().y();
    computeNormalizedCursorFromLogical(QCursor::pos());
    if (screenId < 0) {
        computeNormalizedCursorFromPhysical(gx, gy);
    }
    if (gx != lastX || gy != lastY) {
        lastX = gx; lastY = gy;
        WebSocketClient* client = m_mainWindow->getWebSocketClient();
        if (client && client->isConnected() && m_mainWindow->isWatched()) {
            if (cursorDebugEnabled()) {
                qDebug() << "[CursorDebug][Sender][Emit]"
                         << "global=" << QPoint(gx, gy)
                         << "screenId=" << screenId
                         << "norm=" << normalizedX << normalizedY;
            }
            client->sendCursorUpdate(gx, gy, screenId, normalizedX, normalizedY);
        }
    }
#else
    const QPoint logicalPos = QCursor::pos();
    computeNormalizedCursorFromLogical(logicalPos);
    QPoint p = logicalPos;
#ifdef Q_OS_MACOS
    {
        // Resolve the screen even when the cursor sits exactly on a border pixel where
        // QGuiApplication::screenAt() returns null (one logical pixel past the edge).
        QScreen* targetScreen = logicalScreen;
        if (!targetScreen) {
            // Find the nearest screen by clamping the logical position into each geometry.
            qint64 bestDist = std::numeric_limits<qint64>::max();
            for (QScreen* s : QGuiApplication::screens()) {
                const QRect g = s->geometry();
                const qint64 dx = std::clamp(logicalPos.x(), g.left(), g.right()) - logicalPos.x();
                const qint64 dy = std::clamp(logicalPos.y(), g.top(), g.bottom()) - logicalPos.y();
                const qint64 dist = dx * dx + dy * dy;
                if (dist < bestDist) {
                    bestDist = dist;
                    targetScreen = s;
                }
            }
        }
        if (targetScreen) {
            const qreal dpr = std::max<qreal>(1.0, targetScreen->devicePixelRatio());
            const QRect geometry = targetScreen->geometry();
            const int maxLogicalX = std::max(1, geometry.width() - 1);
            const int maxLogicalY = std::max(1, geometry.height() - 1);
            const int localLogicalX = std::clamp(logicalPos.x() - geometry.x(), 0, maxLogicalX);
            const int localLogicalY = std::clamp(logicalPos.y() - geometry.y(), 0, maxLogicalY);
            const int physicalX = static_cast<int>(std::lround((static_cast<qreal>(geometry.x()) + static_cast<qreal>(localLogicalX)) * dpr));
            const int physicalY = static_cast<int>(std::lround((static_cast<qreal>(geometry.y()) + static_cast<qreal>(localLogicalY)) * dpr));
            p = QPoint(physicalX, physicalY);
        }
    }
#endif
    // Always recompute normalization from the physical coordinate so the denominator
    // matches how the viewer reconstructs position (physicalWidth - 1).  This also
    // handles the case where logical-basis normalisation had screenId < 0.
    computeNormalizedCursorFromPhysical(p.x(), p.y());
    if (p.x() != lastX || p.y() != lastY) {
        lastX = p.x();
        lastY = p.y();
        WebSocketClient* client = m_mainWindow->getWebSocketClient();
        if (client && client->isConnected() && m_mainWindow->isWatched()) {
            if (cursorDebugEnabled()) {
                qDebug() << "[CursorDebug][Sender][Emit]"
                         << "logical=" << logicalPos
                         << "globalPhysical=" << p
                         << "screenId=" << screenId
                         << "norm=" << normalizedX << normalizedY;
            }
            client->sendCursorUpdate(p.x(), p.y(), screenId, normalizedX, normalizedY);
        }
    }
#endif
}
