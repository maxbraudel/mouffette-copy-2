#include "backend/managers/network/ClientListBuilder.h"
#include "backend/runtime/ApplicationRuntime.h"
#include "backend/domain/workspace/WorkspaceManager.h"
#include "shared/rendering/ICanvasHost.h"
#include "backend/network/WebSocketClient.h"
#include "backend/network/RemoteSessionCoordinator.h"
#include <QDebug>

QList<ClientInfo> ClientListBuilder::buildDisplayClientList(
    ApplicationRuntime* mainWindow,
    const QList<ClientInfo>& connectedClients,
    bool localDiscoveryUsable)
{
    if (!mainWindow) return QList<ClientInfo>();
    
    QList<ClientInfo> result;
    
    // Discovery is authoritative for presence. Projects remain visible offline.
    mainWindow->markAllWorkspacesDisconnected();
    
    QSet<QString> identitiesSeen;

    // Process connected clients and update their sessions
    for (ClientInfo client : connectedClients) {
        const QString targetEndpointId = client.endpointId();
        if (targetEndpointId.isEmpty()) {
            qWarning() << "ClientListBuilder: ignored a client without endpointId";
            continue;
        }
        if (identitiesSeen.contains(targetEndpointId)) continue;
        client.setEndpointId(targetEndpointId);
        bool hasProject = false;

        if (ApplicationRuntime::ClientWorkspace* workspace =
                mainWindow->findWorkspace(targetEndpointId)) {
            hasProject = !workspace->projectId.isEmpty();
            client.setProjectId(workspace->projectId);
            workspace->lastClientInfo = client;
            workspace->lastClientInfo.setEndpointId(targetEndpointId);
            workspace->lastClientInfo.setFromMemory(true);
            workspace->lastClientInfo.setOnline(client.isOnline());
            workspace->remoteContentClearedOnDisconnect = false;
            
            if (workspace->canvas) {
                workspace->canvas->setRemoteSceneTarget(
                    workspace->targetEndpointId,
                    workspace->lastClientInfo.getInstanceDisplayName()
                );
            }
            
            client.setFromMemory(true);
        } else {
            client.setFromMemory(false);
        }

        identitiesSeen.insert(targetEndpointId);
        if (!hasProject && (!localDiscoveryUsable || !client.canAcceptSession())) continue;
        client.setHasProject(hasProject);
        result.append(client);
    }

    // A workspace without a durable project is ephemeral and disappears as
    // soon as its endpoint leaves discovery.
    WorkspaceManager* workspaceManager = mainWindow->getWorkspaceManager();
    if (workspaceManager) {
        for (ApplicationRuntime::ClientWorkspace* workspace
             : workspaceManager->allWorkspaces()) {
            if (identitiesSeen.contains(workspace->targetEndpointId)
                || workspace->projectId.isEmpty()) continue;
            
            ClientInfo info = workspace->lastClientInfo;
            info.setEndpointId(workspace->targetEndpointId);
            info.setOnline(false);
            info.setFromMemory(true);
            info.setHasProject(true);
            info.setProjectId(workspace->projectId);
            result.append(info);
        }
    }

    // Use the same network/session projection as the main merger and header.
    for (ClientInfo& info : result) {
        const ClientInfo* presence = nullptr;
        for (const ClientInfo& discovered : connectedClients) {
            if (discovered.endpointId() == info.endpointId()) {
                presence = &discovered;
                break;
            }
        }
        const QString status = mainWindow->remoteConnectionStatus(
            info.endpointId(), presence, localDiscoveryUsable);
        info.setStatus(status);
        info.setAvailabilityStatus(status);
    }

    return result;
}
