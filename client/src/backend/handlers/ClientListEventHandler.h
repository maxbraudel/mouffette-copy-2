#ifndef CLIENTLISTEVENTHANDLER_H
#define CLIENTLISTEVENTHANDLER_H

#include <QObject>
#include <QList>
#include <QString>

class MainWindow;
class WebSocketClient;
class ClientInfo;

/**
 * @brief Handler for client list events and connection state management
 * 
 * Responsibilities:
 * - Process incoming client lists from server
 * - Manage client reconnection scenarios (new ID assignment)
 * - Handle client online/offline state transitions
 * - Coordinate canvas session updates based on client state
 * - Show notifications for new connected clients
 */
class ClientListEventHandler : public QObject
{
    Q_OBJECT

public:
    explicit ClientListEventHandler(MainWindow* mainWindow, WebSocketClient* webSocketClient, QObject* parent = nullptr);
    
    void setupConnections(WebSocketClient* client);

public slots:
    /**
     * @brief Handles the received client list from the server
     * 
     * Updates display list, manages reconnections with new IDs,
     * shows notifications, and updates canvas sessions
     */
    void onClientListReceived(const QList<ClientInfo>& clients);

private:
    MainWindow* m_mainWindow;
    WebSocketClient* m_webSocketClient;
};

#endif // CLIENTLISTEVENTHANDLER_H
