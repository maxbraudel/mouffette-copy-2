#ifndef PROJECTMANAGER_H
#define PROJECTMANAGER_H

#include "backend/domain/project/ProjectModel.h"

#include <QHash>
#include <QObject>
#include <QSet>
#include <QTimer>
#include <functional>
#include <memory>

class ProjectStore;

/**
 * Owns durable local projects. Remote sessions and upload state intentionally
 * stay outside this class so disconnecting a target cannot erase its canvas.
 */
class ProjectManager final : public QObject {
    Q_OBJECT

public:
    struct TimingPolicy {
        qint64 remoteSessionHiddenTimeoutMs = 60'000;
        qint64 projectHiddenRetentionMs = 300'000;
        int autosaveDelayMs = 300;
        int checkpointIntervalMs = 15'000;
        int deadlinePollIntervalMs = 250;
    };

    explicit ProjectManager(QObject* parent = nullptr);
    explicit ProjectManager(const TimingPolicy& timing, QObject* parent = nullptr);
    explicit ProjectManager(ProjectStore* store, QObject* parent = nullptr);
    ProjectManager(ProjectStore* store, const TimingPolicy& timing, QObject* parent = nullptr);
    ~ProjectManager() override;

    bool load();
    bool flush();
    QString lastError() const { return m_lastError; }

    TimingPolicy timingPolicy() const { return m_timing; }
    QList<ProjectRecord> projects() const;
    int projectCount() const { return m_projectsByTarget.size(); }
    bool hasProjectForTarget(const QString& targetEndpointId) const;
    const ProjectRecord* projectForTarget(const QString& targetEndpointId) const;
    const ProjectRecord* projectById(const QString& projectId) const;

    QString ensureProject(const ClientSnapshot& snapshot,
                          ProjectLifecycleState initialState = ProjectLifecycleState::Visible,
                          qint64 nowMs = -1);
    bool setVisible(const QString& targetEndpointId, qint64 nowMs = -1);
    bool setHidden(const QString& targetEndpointId, qint64 nowMs = -1);
    void markAllHidden(qint64 nowMs = -1);
    bool deleteProject(const QString& targetEndpointId);

    bool updateCanvasState(const QString& targetEndpointId,
                           const QJsonObject& canvasState,
                           const QList<ProjectMediaReference>& references = {},
                           qint64 nowMs = -1);
    bool updateClientSnapshot(const ClientSnapshot& snapshot, qint64 nowMs = -1);

    qint64 remoteSessionCloseAtMs(const QString& targetEndpointId) const;
    qint64 projectDeleteAtMs(const QString& targetEndpointId) const;

    // Deterministic entry point for tests and wake/resume handling. Deadlines
    // use >= comparisons, so an event at exactly 3:00/5:00 is terminal.
    void processDeadlines(qint64 nowMs = -1);
    void checkpointVisibleProjects(qint64 nowMs = -1);

    // Merges live discovery with durable projects strictly by endpointId. Online
    // clients are emitted in discovery order, followed by offline projects.
    QList<ProjectClientEntry> mergeDiscoveredClients(const QList<ClientInfo>& discovered,
                                                      qint64 nowMs = -1);

    void setNowProviderForTesting(std::function<qint64()> provider);
    void stopAutomaticTimersForTesting();

signals:
    void projectCreated(const QString& projectId, const QString& targetEndpointId);
    void projectUpdated(const QString& projectId, const QString& targetEndpointId);
    void projectVisibilityChanged(const QString& projectId,
                                  const QString& targetEndpointId,
                                  ProjectLifecycleState state);
    void projectCheckpointDue(const QString& targetEndpointId);
    void remoteSessionCloseDue(const QString& projectId, const QString& targetEndpointId);
    void projectRestoredAfterSessionDeadline(const QString& projectId,
                                             const QString& targetEndpointId);
    void projectAboutToDelete(const ProjectRecord& project);
    void projectDeleted(const QString& projectId, const QString& targetEndpointId);
    void projectsChanged();
    void persistenceError(const QString& message);

private:
    qint64 nowMs() const;
    void initializeTimers();
    void scheduleSave();
    bool persistProjects(const QList<ProjectRecord>& projects);
    QList<ProjectRecord> sortedProjects() const;
    ProjectRecord* mutableProjectForTarget(const QString& targetEndpointId);
    bool removeProjectInternal(const QString& targetEndpointId);

    std::unique_ptr<ProjectStore> m_ownedStore;
    ProjectStore* m_store = nullptr;
    TimingPolicy m_timing;
    QHash<QString, ProjectRecord> m_projectsByTarget;
    QHash<QString, QString> m_targetByProjectId;
    QSet<QString> m_sessionDeadlineNotified;
    QTimer m_autosaveTimer;
    QTimer m_checkpointTimer;
    QTimer m_deadlineTimer;
    std::function<qint64()> m_nowProvider;
    QString m_lastError;
    bool m_dirty = false;
    bool m_automaticTimersEnabled = true;
};

#endif // PROJECTMANAGER_H
