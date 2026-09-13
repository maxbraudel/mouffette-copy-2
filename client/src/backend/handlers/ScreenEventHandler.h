#ifndef SCREENEVENTHANDLER_H
#define SCREENEVENTHANDLER_H

#include <QObject>
#include "backend/domain/models/ClientInfo.h"

class ApplicationRuntime;
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
 * This handler extracts screen event processing logic from ApplicationRuntime
 */
class ScreenEventHandler : public QObject
{
    Q_OBJECT

public:
    explicit ScreenEventHandler(ApplicationRuntime* mainWindow, QObject* parent = nullptr);
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

    // Internal compatibility entry points. Protocol v3 does not expose
    // request/watch messages; both are intentionally disconnected.
    void onScreensInfoReceived(const ClientInfo& clientInfo);
    void onDataRequestReceived();

private:
    ApplicationRuntime* m_mainWindow = nullptr;
    WebSocketClient* m_webSocketClient = nullptr;
};

#endif // SCREENEVENTHANDLER_H
