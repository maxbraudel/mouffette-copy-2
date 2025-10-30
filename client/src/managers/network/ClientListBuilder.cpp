#include "managers/network/ClientListBuilder.h"
#include "MainWindow.h"
#include "domain/session/SessionManager.h"
#include "rendering/canvas/ScreenCanvas.h"
#include <QDebug>

QList<ClientInfo> ClientListBuilder::buildDisplayClientList(
    MainWindow* mainWindow,
    const QList<ClientInfo>& connectedClients)
{
    if (!mainWindow) return QList<ClientInfo>();
    
    QList<ClientInfo> result;
    
    // Mark all sessions as offline initially
    mainWindow->markAllSessionsOffline();
    
    QSet<QString> identitiesSeen;

    // Process connected clients and update their sessions
    for (ClientInfo client : connectedClients) {
        QString persistentId = client.clientId();
        if (persistentId.isEmpty()) {
            qWarning() << "ClientListBuilder::buildDisplayClientList: client has no persistentClientId";
            continue;
        }
        client.setClientId(persistentId);
        client.setOnline(true);

        // Find existing session for this client
        if (MainWindow::CanvasSession* session = mainWindow->findCanvasSession(persistentId)) {
            session->serverAssignedId = client.getId(); // Keep for local lookup
            session->lastClientInfo = client;
            session->lastClientInfo.setClientId(persistentId);
            session->lastClientInfo.setFromMemory(true);
            session->lastClientInfo.setOnline(true);
            session->remoteContentClearedOnDisconnect = false;
            
            // Update remote scene target if canvas exists
            if (session->canvas && !session->persistentClientId.isEmpty()) {
                session->canvas->setRemoteSceneTarget(
                    session->persistentClientId,
                    session->lastClientInfo.getMachineName()
                );
            }
            
            client.setFromMemory(true);
            client.setId(session->serverAssignedId);
        } else {
            client.setFromMemory(false);
        }

        identitiesSeen.insert(persistentId);
        result.append(client);
    }

    // Add offline clients from session history
    SessionManager* sessionManager = mainWindow->getSessionManager();
    if (sessionManager) {
        for (MainWindow::CanvasSession* session : sessionManager->getAllSessions()) {
            // Skip if already seen as online
            if (identitiesSeen.contains(session->persistentClientId)) continue;
            
            ClientInfo info = session->lastClientInfo;
            info.setClientId(session->persistentClientId);
            if (!session->serverAssignedId.isEmpty()) {
                info.setId(session->serverAssignedId);
            }
            info.setOnline(false);
            info.setFromMemory(true);
            result.append(info);
        }
    }

    return result;
}
