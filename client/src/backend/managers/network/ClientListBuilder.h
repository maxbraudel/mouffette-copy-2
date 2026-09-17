#ifndef CLIENTLISTBUILDER_H
#define CLIENTLISTBUILDER_H

#include <QList>
#include <QSet>
#include <QString>
#include "backend/domain/models/ClientInfo.h"

class ApplicationRuntime;
class WorkspaceManager;

/**
 * @brief Builds the visible union of available endpoints and durable project targets.
 */
class ClientListBuilder
{
public:
    /**
     * @brief Build a display list combining available clients and retained projects
     * @param mainWindow The ApplicationRuntime instance (for session access)
     * @param connectedClients Full presence list, including retained offline entries
     * @param localDiscoveryUsable Whether the local connection can use discovery
     * @return Unified list of clients to display in the UI
     * 
     * This method:
     * 1. Marks all sessions as offline initially
     * 2. Updates sessions for connected clients (marks online)
     * 3. Keeps unavailable clients only when their workspace has a project
     */
    static QList<ClientInfo> buildDisplayClientList(
        ApplicationRuntime* mainWindow,
        const QList<ClientInfo>& connectedClients,
        bool localDiscoveryUsable
    );
};

#endif // CLIENTLISTBUILDER_H
