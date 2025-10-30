#ifndef CLIENTLISTBUILDER_H
#define CLIENTLISTBUILDER_H

#include <QList>
#include <QSet>
#include <QString>
#include "ClientInfo.h"

class MainWindow;
class SessionManager;

/**
 * @brief Builds the display client list by merging connected clients with offline session history
 * 
 * [Phase 16] Extracted from MainWindow::buildDisplayClientList() to reduce MainWindow size
 * This class handles the logic of building a unified client list that includes:
 * - Currently connected clients (online)
 * - Previously connected clients from session history (offline)
 */
class ClientListBuilder
{
public:
    /**
     * @brief Build a display list combining connected clients and offline sessions
     * @param mainWindow The MainWindow instance (for session access)
     * @param connectedClients List of currently connected clients from server
     * @return Unified list of clients to display in the UI
     * 
     * This method:
     * 1. Marks all sessions as offline initially
     * 2. Updates sessions for connected clients (marks online)
     * 3. Appends offline clients from session history
     */
    static QList<ClientInfo> buildDisplayClientList(
        MainWindow* mainWindow,
        const QList<ClientInfo>& connectedClients
    );
};

#endif // CLIENTLISTBUILDER_H
