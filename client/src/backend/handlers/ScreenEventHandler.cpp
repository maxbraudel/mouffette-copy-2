#include "backend/handlers/ScreenEventHandler.h"
#include "backend/runtime/ApplicationRuntime.h"
#include "backend/network/WebSocketClient.h"
#include "backend/managers/app/SettingsManager.h"

ScreenEventHandler::ScreenEventHandler(ApplicationRuntime* runtime, QObject* parent)
    : QObject(parent), m_mainWindow(runtime)
{
}

void ScreenEventHandler::setupConnections(WebSocketClient* client)
{
    m_webSocketClient = client;
}

void ScreenEventHandler::syncRegistration()
{
    if (!m_mainWindow || !m_webSocketClient) return;
    QList<ScreenInfo> screens;
    if (!m_mainWindow->captureLocalScreenInfo(&screens)) return;
    m_webSocketClient->registerClient(m_mainWindow->getMachineName(),
        m_mainWindow->getPlatformName(), screens, m_mainWindow->getSystemVolumePercent(),
        m_mainWindow->getSettingsManager()->username(),
        m_mainWindow->getSettingsManager()->profilePictureJpeg());
}
