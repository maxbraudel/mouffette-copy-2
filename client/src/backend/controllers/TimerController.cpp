#include "backend/controllers/TimerController.h"
#include "backend/runtime/ApplicationRuntime.h"
#include "backend/network/WebSocketClient.h"

TimerController::TimerController(ApplicationRuntime* mainWindow, QObject* parent)
    : QObject(parent)
    , m_mainWindow(mainWindow)
{
}

void TimerController::setupTimers() {
    QTimer* statusUpdateTimer = m_mainWindow->getStatusUpdateTimer();
    QTimer* displaySyncTimer = m_mainWindow->getDisplaySyncTimer();
    
    // Periodic connection status refresh no longer needed (now event-driven); keep timer disabled
    statusUpdateTimer->stop();

    // Device discovery in protocol v3 is snapshot based and has no watch
    // subscription. Keep the server's view of our topology/volume fresh even
    // while this window is hidden (incoming receiver availability is separate
    // from local Project visibility).
    displaySyncTimer->setInterval(3000);
    connect(displaySyncTimer, &QTimer::timeout, this, &TimerController::onDisplaySyncTimeout);
    displaySyncTimer->start();

}

void TimerController::onDisplaySyncTimeout() {
    WebSocketClient* client = m_mainWindow->getWebSocketClient();
    if (client && client->isConnected()) {
        m_mainWindow->syncRegistration();
    }
}
