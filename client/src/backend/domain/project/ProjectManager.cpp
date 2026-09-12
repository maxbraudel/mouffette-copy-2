#include "backend/domain/project/ProjectManager.h"

#include "backend/domain/project/ProjectStore.h"

#include <QDateTime>
#include <QUuid>
#include <algorithm>
#include <utility>

namespace {
bool snapshotContentEquals(const ClientSnapshot& left, const ClientSnapshot& right)
{
    // JSON comparison is useful here because it also covers nested screen/UI
    // zone fields. lastSeenAtMs is intentionally included and checkpointed.
    return left.toJson() == right.toJson();
}
}

ProjectManager::ProjectManager(QObject* parent)
    : ProjectManager(TimingPolicy{}, parent)
{
}

ProjectManager::ProjectManager(const TimingPolicy& timing, QObject* parent)
    : QObject(parent)
    , m_ownedStore(std::make_unique<ProjectStore>())
    , m_store(m_ownedStore.get())
    , m_timing(timing)
{
    initializeTimers();
}

ProjectManager::ProjectManager(ProjectStore* store, QObject* parent)
    : ProjectManager(store, TimingPolicy{}, parent)
{
}

ProjectManager::ProjectManager(ProjectStore* store,
                               const TimingPolicy& timing,
                               QObject* parent)
    : QObject(parent)
    , m_store(store)
    , m_timing(timing)
{
    initializeTimers();
}

ProjectManager::~ProjectManager()
{
    if (m_dirty) {
        flush();
    }
}

void ProjectManager::initializeTimers()
{
    m_autosaveTimer.setSingleShot(true);
    m_autosaveTimer.setInterval(qMax(0, m_timing.autosaveDelayMs));
    connect(&m_autosaveTimer, &QTimer::timeout, this, [this]() { flush(); });

    m_checkpointTimer.setInterval(qMax(1, m_timing.checkpointIntervalMs));
    connect(&m_checkpointTimer, &QTimer::timeout, this, [this]() { checkpointVisibleProjects(); });
    m_checkpointTimer.start();

    m_deadlineTimer.setInterval(qMax(1, m_timing.deadlinePollIntervalMs));
    connect(&m_deadlineTimer, &QTimer::timeout, this, [this]() { processDeadlines(); });
    m_deadlineTimer.start();
}

qint64 ProjectManager::nowMs() const
{
    return m_nowProvider ? m_nowProvider() : QDateTime::currentMSecsSinceEpoch();
}

void ProjectManager::setNowProviderForTesting(std::function<qint64()> provider)
{
    m_nowProvider = std::move(provider);
}

void ProjectManager::stopAutomaticTimersForTesting()
{
    m_automaticTimersEnabled = false;
    m_autosaveTimer.stop();
    m_checkpointTimer.stop();
    m_deadlineTimer.stop();
}

bool ProjectManager::load()
{
    m_lastError.clear();
    m_projectsByTarget.clear();
    m_targetByProjectId.clear();
    m_sessionDeadlineNotified.clear();
    m_dirty = false;
    m_autosaveTimer.stop();

    if (!m_store) {
        m_lastError = QStringLiteral("Project store is not configured");
        emit persistenceError(m_lastError);
        return false;
    }

    QList<ProjectRecord> loaded;
    if (!m_store->load(&loaded)) {
        m_lastError = m_store->lastError();
        emit persistenceError(m_lastError);
        return false;
    }

    const qint64 current = nowMs();
    bool normalized = false;
    for (ProjectRecord project : loaded) {
        // A durable Visible state means the previous process did not perform a
        // clean hide. Its last liveness checkpoint becomes hiddenAt.
        if (project.state == ProjectLifecycleState::Visible) {
            project.state = ProjectLifecycleState::Hidden;
            project.hiddenAtMs = project.lastCheckpointAtMs >= 0
                ? project.lastCheckpointAtMs
                : qMax(project.updatedAtMs, project.createdAtMs);
            normalized = true;
        }
        if (project.state != ProjectLifecycleState::Hidden) {
            continue;
        }
        if (project.hiddenAtMs < 0) {
            project.hiddenAtMs = qMax(project.updatedAtMs, project.createdAtMs);
            normalized = true;
        }
        if (current >= project.hiddenAtMs + m_timing.projectHiddenRetentionMs) {
            normalized = true;
            continue;
        }
        if (current >= project.hiddenAtMs + m_timing.remoteSessionHiddenTimeoutMs) {
            // Sessions are never restored, so no teardown signal is necessary
            // for an already elapsed pre-restart deadline.
            m_sessionDeadlineNotified.insert(project.targetDeviceId);
        }
        m_targetByProjectId.insert(project.projectId, project.targetDeviceId);
        m_projectsByTarget.insert(project.targetDeviceId, project);
    }

    if (normalized && !flush()) {
        return false;
    }
    emit projectsChanged();
    return true;
}

QList<ProjectRecord> ProjectManager::sortedProjects() const
{
    QList<ProjectRecord> result = m_projectsByTarget.values();
    std::sort(result.begin(), result.end(), [](const ProjectRecord& left, const ProjectRecord& right) {
        if (left.updatedAtMs != right.updatedAtMs) {
            return left.updatedAtMs > right.updatedAtMs;
        }
        return left.projectId < right.projectId;
    });
    return result;
}

QList<ProjectRecord> ProjectManager::projects() const
{
    return sortedProjects();
}

bool ProjectManager::flush()
{
    return persistProjects(sortedProjects());
}

bool ProjectManager::persistProjects(const QList<ProjectRecord>& projects)
{
    m_autosaveTimer.stop();
    if (!m_store) {
        m_lastError = QStringLiteral("Project store is not configured");
        scheduleSave();
        emit persistenceError(m_lastError);
        return false;
    }
    if (!m_store->save(projects)) {
        m_lastError = m_store->lastError();
        // Keep all pending in-memory changes dirty and retry through the
        // normal delayed autosave path. This also avoids a tight retry loop.
        scheduleSave();
        emit persistenceError(m_lastError);
        return false;
    }
    m_lastError.clear();
    m_dirty = false;
    return true;
}

void ProjectManager::scheduleSave()
{
    m_dirty = true;
    if (m_automaticTimersEnabled && !m_autosaveTimer.isActive()) {
        m_autosaveTimer.start();
    }
}

bool ProjectManager::hasProjectForTarget(const QString& targetDeviceId) const
{
    return m_projectsByTarget.contains(targetDeviceId);
}

ProjectRecord* ProjectManager::mutableProjectForTarget(const QString& targetDeviceId)
{
    auto it = m_projectsByTarget.find(targetDeviceId);
    return it == m_projectsByTarget.end() ? nullptr : &it.value();
}

const ProjectRecord* ProjectManager::projectForTarget(const QString& targetDeviceId) const
{
    auto it = m_projectsByTarget.constFind(targetDeviceId);
    return it == m_projectsByTarget.constEnd() ? nullptr : &it.value();
}

const ProjectRecord* ProjectManager::projectById(const QString& projectId) const
{
    return projectForTarget(m_targetByProjectId.value(projectId));
}

QString ProjectManager::ensureProject(const ClientSnapshot& snapshot,
                                      ProjectLifecycleState initialState,
                                      qint64 atMs)
{
    if (!snapshot.isValid() || initialState == ProjectLifecycleState::Deleted) {
        return {};
    }
    const qint64 current = atMs >= 0 ? atMs : nowMs();
    if (ProjectRecord* existing = mutableProjectForTarget(snapshot.deviceId)) {
        if (existing->state == ProjectLifecycleState::Hidden
            && existing->hiddenAtMs >= 0
            && current >= existing->hiddenAtMs + m_timing.projectHiddenRetentionMs) {
            if (!removeProjectInternal(snapshot.deviceId)) {
                return {};
            }
            existing = nullptr;
        }
        if (!existing) {
            // The expired project was removed; create a new stable project
            // below instead of reviving an already terminal record.
        } else {
            const bool becomingVisible = initialState == ProjectLifecycleState::Visible
                && existing->state != ProjectLifecycleState::Visible;
            const bool restoredAfterSessionDeadline = becomingVisible
                && existing->hiddenAtMs >= 0
                && current >= existing->hiddenAtMs
                                  + m_timing.remoteSessionHiddenTimeoutMs;
            if (becomingVisible) {
                if (restoredAfterSessionDeadline
                    && !m_sessionDeadlineNotified.contains(snapshot.deviceId)) {
                    m_sessionDeadlineNotified.insert(snapshot.deviceId);
                    emit remoteSessionCloseDue(
                        existing->projectId, existing->targetDeviceId);
                }
                existing->state = ProjectLifecycleState::Visible;
                existing->hiddenAtMs = -1;
                existing->lastCheckpointAtMs = current;
                m_sessionDeadlineNotified.remove(snapshot.deviceId);
            }
            existing->clientSnapshot = snapshot;
            existing->updatedAtMs = current;
            scheduleSave();
            if (becomingVisible) {
                emit projectVisibilityChanged(existing->projectId,
                                              existing->targetDeviceId,
                                              existing->state);
                if (restoredAfterSessionDeadline) {
                    emit projectRestoredAfterSessionDeadline(
                        existing->projectId, existing->targetDeviceId);
                }
            }
            emit projectUpdated(existing->projectId, existing->targetDeviceId);
            emit projectsChanged();
            return existing->projectId;
        }
    }

    ProjectRecord project;
    project.projectId = QUuid::createUuid().toString(QUuid::WithoutBraces);
    project.targetDeviceId = snapshot.deviceId;
    project.clientSnapshot = snapshot;
    project.state = initialState;
    project.createdAtMs = current;
    project.updatedAtMs = current;
    project.lastCheckpointAtMs = current;
    project.hiddenAtMs = initialState == ProjectLifecycleState::Hidden ? current : -1;

    m_targetByProjectId.insert(project.projectId, project.targetDeviceId);
    m_projectsByTarget.insert(project.targetDeviceId, project);
    scheduleSave();
    emit projectCreated(project.projectId, project.targetDeviceId);
    emit projectsChanged();
    return project.projectId;
}

bool ProjectManager::setVisible(const QString& targetDeviceId, qint64 atMs)
{
    const qint64 current = atMs >= 0 ? atMs : nowMs();
    ProjectRecord* project = mutableProjectForTarget(targetDeviceId);
    if (!project) {
        return false;
    }
    if (project->state == ProjectLifecycleState::Hidden
        && project->hiddenAtMs >= 0
        && current >= project->hiddenAtMs + m_timing.projectHiddenRetentionMs) {
        removeProjectInternal(targetDeviceId);
        return false;
    }
    if (project->state == ProjectLifecycleState::Visible) {
        return true;
    }

    const bool restoredAfterSessionDeadline = project->hiddenAtMs >= 0
        && current >= project->hiddenAtMs
                          + m_timing.remoteSessionHiddenTimeoutMs;
    if (restoredAfterSessionDeadline
        && !m_sessionDeadlineNotified.contains(targetDeviceId)) {
        m_sessionDeadlineNotified.insert(targetDeviceId);
        emit remoteSessionCloseDue(project->projectId, targetDeviceId);
    }
    project->state = ProjectLifecycleState::Visible;
    project->hiddenAtMs = -1;
    project->lastCheckpointAtMs = current;
    project->updatedAtMs = current;
    m_sessionDeadlineNotified.remove(targetDeviceId);
    scheduleSave();
    emit projectVisibilityChanged(project->projectId, targetDeviceId, project->state);
    if (restoredAfterSessionDeadline) {
        emit projectRestoredAfterSessionDeadline(project->projectId,
                                                  targetDeviceId);
    }
    emit projectUpdated(project->projectId, targetDeviceId);
    emit projectsChanged();
    return true;
}

bool ProjectManager::setHidden(const QString& targetDeviceId, qint64 atMs)
{
    ProjectRecord* project = mutableProjectForTarget(targetDeviceId);
    if (!project) {
        return false;
    }
    if (project->state == ProjectLifecycleState::Hidden) {
        return true; // Never extend an existing hidden deadline.
    }
    if (project->state != ProjectLifecycleState::Visible) {
        return false;
    }

    const qint64 current = atMs >= 0 ? atMs : nowMs();
    project->state = ProjectLifecycleState::Hidden;
    project->hiddenAtMs = current;
    project->updatedAtMs = current;
    m_sessionDeadlineNotified.remove(targetDeviceId);
    scheduleSave();
    emit projectVisibilityChanged(project->projectId, targetDeviceId, project->state);
    emit projectUpdated(project->projectId, targetDeviceId);
    emit projectsChanged();
    return true;
}

void ProjectManager::markAllHidden(qint64 atMs)
{
    const QStringList targets = m_projectsByTarget.keys();
    const qint64 current = atMs >= 0 ? atMs : nowMs();
    for (const QString& target : targets) {
        setHidden(target, current);
    }
}

bool ProjectManager::removeProjectInternal(const QString& targetDeviceId)
{
    const auto it = m_projectsByTarget.constFind(targetDeviceId);
    if (it == m_projectsByTarget.constEnd()) {
        return false;
    }

    const ProjectRecord snapshot = it.value();
    emit projectAboutToDelete(snapshot);

    // Persist a prospective snapshot first. Until QSaveFile commits it, the
    // current record and both indexes remain fully observable and unchanged.
    // This is equivalent to an exact rollback without exposing a transiently
    // deleted project to persistenceError observers.
    QList<ProjectRecord> prospective = sortedProjects();
    prospective.erase(std::remove_if(prospective.begin(), prospective.end(),
                                     [&targetDeviceId](const ProjectRecord& project) {
        return project.targetDeviceId == targetDeviceId;
    }), prospective.end());
    if (!persistProjects(prospective)) {
        return false;
    }

    m_projectsByTarget.remove(targetDeviceId);
    m_targetByProjectId.remove(snapshot.projectId);
    m_sessionDeadlineNotified.remove(targetDeviceId);
    emit projectDeleted(snapshot.projectId, targetDeviceId);
    emit projectsChanged();
    return true;
}

bool ProjectManager::deleteProject(const QString& targetDeviceId)
{
    return removeProjectInternal(targetDeviceId);
}

bool ProjectManager::updateCanvasState(const QString& targetDeviceId,
                                       const QJsonObject& canvasState,
                                       const QList<ProjectMediaReference>& references,
                                       qint64 atMs)
{
    ProjectRecord* project = mutableProjectForTarget(targetDeviceId);
    if (!project) {
        return false;
    }
    project->canvasState = canvasState;
    project->mediaReferences = references;
    project->updatedAtMs = atMs >= 0 ? atMs : nowMs();
    scheduleSave();
    emit projectUpdated(project->projectId, targetDeviceId);
    emit projectsChanged();
    return true;
}

bool ProjectManager::updateClientSnapshot(const ClientSnapshot& snapshot, qint64 atMs)
{
    ProjectRecord* project = mutableProjectForTarget(snapshot.deviceId);
    if (!project || !snapshot.isValid()) {
        return false;
    }
    if (snapshotContentEquals(project->clientSnapshot, snapshot)) {
        return true;
    }
    project->clientSnapshot = snapshot;
    project->updatedAtMs = atMs >= 0 ? atMs : nowMs();
    scheduleSave();
    emit projectUpdated(project->projectId, project->targetDeviceId);
    emit projectsChanged();
    return true;
}

qint64 ProjectManager::remoteSessionCloseAtMs(const QString& targetDeviceId) const
{
    const ProjectRecord* project = projectForTarget(targetDeviceId);
    return project && project->state == ProjectLifecycleState::Hidden && project->hiddenAtMs >= 0
        ? project->hiddenAtMs + m_timing.remoteSessionHiddenTimeoutMs
        : -1;
}

qint64 ProjectManager::projectDeleteAtMs(const QString& targetDeviceId) const
{
    const ProjectRecord* project = projectForTarget(targetDeviceId);
    return project && project->state == ProjectLifecycleState::Hidden && project->hiddenAtMs >= 0
        ? project->hiddenAtMs + m_timing.projectHiddenRetentionMs
        : -1;
}

void ProjectManager::processDeadlines(qint64 atMs)
{
    const qint64 current = atMs >= 0 ? atMs : nowMs();
    QStringList expiredProjects;
    const QStringList targets = m_projectsByTarget.keys();
    for (const QString& target : targets) {
        const ProjectRecord* project = projectForTarget(target);
        if (!project || project->state != ProjectLifecycleState::Hidden || project->hiddenAtMs < 0) {
            continue;
        }
        if (!m_sessionDeadlineNotified.contains(target)
            && current >= project->hiddenAtMs + m_timing.remoteSessionHiddenTimeoutMs) {
            m_sessionDeadlineNotified.insert(target);
            emit remoteSessionCloseDue(project->projectId, target);
        }
        if (current >= project->hiddenAtMs + m_timing.projectHiddenRetentionMs) {
            expiredProjects.append(target);
        }
    }

    if (expiredProjects.isEmpty()) {
        return;
    }
    for (const QString& target : expiredProjects) {
        // Commit before exposing each deletion. If storage is temporarily
        // unavailable, retain this and the remaining projects for the next
        // deadline poll instead of losing them from memory.
        if (!removeProjectInternal(target)) {
            break;
        }
    }
}

void ProjectManager::checkpointVisibleProjects(qint64 atMs)
{
    const qint64 current = atMs >= 0 ? atMs : nowMs();
    bool changed = false;
    for (auto it = m_projectsByTarget.begin(); it != m_projectsByTarget.end(); ++it) {
        if (it.value().state == ProjectLifecycleState::Visible) {
            emit projectCheckpointDue(it.key());
            it.value().lastCheckpointAtMs = current;
            changed = true;
        }
    }
    if (changed) {
        m_dirty = true;
        flush();
    }
}

QList<ProjectClientEntry> ProjectManager::mergeDiscoveredClients(const QList<ClientInfo>& discovered,
                                                                  qint64 atMs)
{
    const qint64 current = atMs >= 0 ? atMs : nowMs();
    processDeadlines(current);

    QList<ProjectClientEntry> result;
    QSet<QString> seen;
    bool snapshotChanged = false;
    for (ClientInfo client : discovered) {
        const QString deviceId = client.clientId().trimmed();
        if (deviceId.isEmpty() || seen.contains(deviceId)) {
            continue;
        }
        seen.insert(deviceId);
        client.setOnline(true);

        ProjectClientEntry entry;
        entry.client = client;
        entry.deviceId = deviceId;
        entry.online = true;
        if (ProjectRecord* project = mutableProjectForTarget(deviceId)) {
            const ClientSnapshot fresh = ClientSnapshot::fromClientInfo(client, current);
            if (!snapshotContentEquals(project->clientSnapshot, fresh)) {
                project->clientSnapshot = fresh;
                project->updatedAtMs = current;
                snapshotChanged = true;
                emit projectUpdated(project->projectId, deviceId);
            }
            entry.hasProject = true;
            entry.projectId = project->projectId;
            entry.projectState = project->state;
            entry.remoteSessionCloseAtMs = remoteSessionCloseAtMs(deviceId);
            entry.projectDeleteAtMs = projectDeleteAtMs(deviceId);
        }
        result.append(entry);
    }

    for (const ProjectRecord& project : sortedProjects()) {
        if (seen.contains(project.targetDeviceId)) {
            continue;
        }
        ProjectClientEntry entry;
        entry.client = project.clientSnapshot.toClientInfo(false);
        entry.deviceId = project.targetDeviceId;
        entry.projectId = project.projectId;
        entry.hasProject = true;
        entry.online = false;
        entry.projectState = project.state;
        entry.remoteSessionCloseAtMs = remoteSessionCloseAtMs(project.targetDeviceId);
        entry.projectDeleteAtMs = projectDeleteAtMs(project.targetDeviceId);
        result.append(entry);
    }

    if (snapshotChanged) {
        scheduleSave();
        emit projectsChanged();
    }
    return result;
}
