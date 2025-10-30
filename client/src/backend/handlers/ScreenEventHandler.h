#ifndef SCREENEVENTHANDLER_H
#define SCREENEVENTHANDLER_H

#include <QObject>
#include "backend/domain/models/ClientInfo.h"

class MainWindow;
class WebSocketClient;
class SystemMonitor;

/**
 * @brief ScreenEventHandler manages screen-related events and registration
 * 
 * Responsibilities:
 * - Handle client registration with server
 * - Process incoming screen information from remote clients
 * - Handle data request events from server
 * - Coordinate screen info collection with SystemMonitor
 * 
 * This handler extracts screen event processing logic from MainWindow
 */
class ScreenEventHandler : public QObject
{
    Q_OBJECT

public:
    explicit ScreenEventHandler(MainWindow* mainWindow, QObject* parent = nullptr);
    ~ScreenEventHandler() override = default;

    /**
     * @brief Setup connections to WebSocket client signals
     * @param client The WebSocket client to connect to
     */
    void setupConnections(WebSocketClient* client);

public slots:
    /**
     * @brief Synchronize client registration with server
     * Collects machine name, platform, screens, and volume info
     */
    void syncRegistration();

    /**
     * @brief Handle screen information received from remote client
     * @param clientInfo Client information including screen details
     */
    void onScreensInfoReceived(const ClientInfo& clientInfo);

    /**
     * @brief Handle data request from server
     * Send current screen and volume information
     */
    void onDataRequestReceived();

private:
    MainWindow* m_mainWindow = nullptr;
    WebSocketClient* m_webSocketClient = nullptr;
};

#endif // SCREENEVENTHANDLER_H
