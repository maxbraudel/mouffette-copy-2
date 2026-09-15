#ifndef WORKSPACEMANAGER_H
#define WORKSPACEMANAGER_H

#include <QHash>
#include <QList>
#include <QObject>
#include <QSet>
#include <QString>
#include <QTimer>
#include <functional>

#include "backend/domain/models/ClientInfo.h"

class CanvasMedia;
class ICanvasHost;

/**
 * Owns one local workspace per remote endpoint. The project ID remains empty
 * until the correlated endpoint snapshot has been persisted atomically.
 */
class WorkspaceManager final : public QObject
{
    Q_OBJECT

public:
    enum class RemoteSessionState { Absent, Opening, Active, Grace, Closing };
    Q_ENUM(RemoteSessionState)

    struct ClientWorkspace {
        QString targetEndpointId;
        QString projectId;
        ICanvasHost* canvas = nullptr;
        ClientInfo lastClientInfo;
        bool connectionsInitialized = false;
        bool remoteContentClearedOnDisconnect = false;
        RemoteSessionState remoteSessionState = RemoteSessionState::Absent;
        qint64 sessionHiddenAtMs = -1;
        bool workspaceVisible = false;
        QSet<QString> expectedProjectFileIds;
        QSet<QString> knownRemoteFileIds;

        struct UploadTracking {
            QHash<QString, QList<CanvasMedia*>> itemsByFileId;
            QStringList currentUploadFileOrder;
            QSet<QString> serverCompletedFileIds;
            QHash<QString, int> perFileProgress;
            bool receivingFilesToastShown = false;
            QString activeUploadId;
            bool remoteFilesPresent = false;
        } upload;
    };

    explicit WorkspaceManager(QObject* parent = nullptr);
    ~WorkspaceManager() override;

    ClientWorkspace* findWorkspace(const QString& targetEndpointId);
    const ClientWorkspace* findWorkspace(const QString& targetEndpointId) const;
    ClientWorkspace* getOrCreateWorkspace(const QString& targetEndpointId,
                                          const ClientInfo& clientInfo);
    bool hasWorkspace(const QString& targetEndpointId) const;
    void deleteWorkspace(const QString& targetEndpointId);
    void clearAllWorkspaces();

    QList<QString> allTargetEndpointIds() const;
    QList<ClientWorkspace*> allWorkspaces();
    QList<const ClientWorkspace*> allWorkspaces() const;
    int workspaceCount() const { return m_workspaces.size(); }

    void setRemoteSessionHiddenTimeoutMs(qint64 timeoutMs);
    qint64 remoteSessionHiddenTimeoutMs() const {
        return m_remoteSessionHiddenTimeoutMs;
    }
    RemoteSessionState remoteSessionState(const QString& targetEndpointId) const;
    bool setRemoteSessionState(const QString& targetEndpointId,
                               RemoteSessionState state);
    bool setWorkspaceVisible(const QString& targetEndpointId,
                             qint64 nowMs = -1);
    bool setWorkspaceHidden(const QString& targetEndpointId,
                            qint64 nowMs = -1);
    void markAllWorkspacesHidden(qint64 nowMs = -1);
    qint64 remoteSessionCloseAtMs(const QString& targetEndpointId) const;
    void processDeadlines(qint64 nowMs = -1);
    void setNowProviderForTesting(std::function<qint64()> provider);
    void stopAutomaticTimersForTesting();

    void updateWorkspaceProjectId(const QString& targetEndpointId,
                                  const QString& projectId);

signals:
    void workspaceCreated(const QString& targetEndpointId);
    void workspaceDeleted(const QString& targetEndpointId);
    void workspaceModified(const QString& targetEndpointId);
    void remoteSessionStateChanged(const QString& targetEndpointId,
                                   RemoteSessionState state);
    void remoteSessionCloseDue(const QString& targetEndpointId);

private:
    QHash<QString, ClientWorkspace> m_workspaces;
    qint64 m_remoteSessionHiddenTimeoutMs = 60'000;
    QTimer m_deadlineTimer;
    std::function<qint64()> m_nowProvider;
};

#endif // WORKSPACEMANAGER_H
