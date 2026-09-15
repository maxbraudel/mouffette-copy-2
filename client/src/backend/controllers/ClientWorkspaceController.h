#ifndef CLIENTWORKSPACECONTROLLER_H
#define CLIENTWORKSPACECONTROLLER_H

#include "backend/domain/workspace/WorkspaceManager.h"

#include <QObject>
#include <QString>

class ApplicationRuntime;
class ClientInfo;
class QuickCanvasHost;

/** Creates, configures and selects the canvas owned by each client workspace. */
class ClientWorkspaceController final : public QObject
{
    Q_OBJECT

public:
    using ClientWorkspace = WorkspaceManager::ClientWorkspace;

    explicit ClientWorkspaceController(ApplicationRuntime* runtime,
                                       QObject* parent = nullptr);
    ~ClientWorkspaceController() override = default;

    ClientWorkspace* findWorkspace(const QString& targetEndpointId);
    const ClientWorkspace* findWorkspace(const QString& targetEndpointId) const;
    ClientWorkspace* ensureWorkspace(const ClientInfo& client);
    void prewarmQuickCanvasHost();
    void configureWorkspace(ClientWorkspace* workspace);
    void switchToWorkspace(const QString& targetEndpointId);

    void updateUploadButtonForWorkspace(ClientWorkspace* workspace);
    void clearUploadTracking(ClientWorkspace* workspace);
    ClientWorkspace* workspaceForActiveUpload();
    ClientWorkspace* workspaceForUploadId(const QString& uploadId);

private:
    ApplicationRuntime* m_runtime = nullptr;
    QuickCanvasHost* m_prewarmedQuickCanvasHost = nullptr;
    QString m_lastQuickInitError;
};

#endif // CLIENTWORKSPACECONTROLLER_H
