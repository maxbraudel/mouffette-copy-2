#ifndef CLIENTLISTBUILDER_H
#define CLIENTLISTBUILDER_H

#include <QList>
#include <QSet>
#include <QString>
#include "backend/domain/models/ClientInfo.h"

class ApplicationRuntime;
class WorkspaceManager;

/**
 * @brief Builds the display client list by merging connected clients with offline session history
 * 
 * Builds the visible union of available endpoints and durable project targets.
 * This class handles the logic of building a unified client list that includes:
 * - Currently connected clients (online)
 * - Previously connected clients from session history (offline)
 */
class ClientListBuilder
{
public:
    /**
     * @brief Build a display list combining connected clients and offline sessions
     * @param mainWindow The ApplicationRuntime instance (for session access)
     * @param connectedClients List of currently connected clients from server
     * @return Unified list of clients to display in the UI
     * 
     * This method:
     * 1. Marks all sessions as offline initially
     * 2. Updates sessions for connected clients (marks online)
     * 3. Appends offline clients from session history
     */
    static QList<ClientInfo> buildDisplayClientList(
        ApplicationRuntime* mainWindow,
        const QList<ClientInfo>& connectedClients
    );
};

#endif // CLIENTLISTBUILDER_H
