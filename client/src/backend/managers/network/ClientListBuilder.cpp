#include "backend/managers/network/ClientListBuilder.h"
#include "backend/runtime/ApplicationRuntime.h"
#include "backend/domain/workspace/WorkspaceManager.h"
#include "shared/rendering/ICanvasHost.h"
#include <QDebug>

QList<ClientInfo> ClientListBuilder::buildDisplayClientList(
    ApplicationRuntime* mainWindow,
    const QList<ClientInfo>& connectedClients)
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
        client.setEndpointId(targetEndpointId);
        client.setOnline(true);

        if (ApplicationRuntime::ClientWorkspace* workspace =
                mainWindow->findWorkspace(targetEndpointId)) {
            workspace->lastClientInfo = client;
            workspace->lastClientInfo.setEndpointId(targetEndpointId);
            workspace->lastClientInfo.setFromMemory(true);
            workspace->lastClientInfo.setOnline(true);
            workspace->remoteContentClearedOnDisconnect = false;
            
            if (workspace->canvas) {
                workspace->canvas->setRemoteSceneTarget(
                    workspace->targetEndpointId,
                    workspace->lastClientInfo.getMachineName()
                );
            }
            
            client.setFromMemory(true);
        } else {
            client.setFromMemory(false);
        }

        identitiesSeen.insert(targetEndpointId);
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
            result.append(info);
        }
    }

    return result;
}
