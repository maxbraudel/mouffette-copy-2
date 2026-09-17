#include "backend/domain/project/ProjectManager.h"

#include "backend/domain/project/ProjectStore.h"

#include <QDateTime>
#include <QScopedValueRollback>
#include <QUuid>
#include <algorithm>
#include <utility>

namespace {
bool targetContentEquals(const ProjectTargetReference& left,
                         const ProjectTargetReference& right)
{
    return left.toJson() == right.toJson();
}

ProjectTargetReference mergeTargetPresentation(
    const ProjectTargetReference& previous,
    const ProjectTargetReference& incoming)
{
    ProjectTargetReference merged = incoming;
    if (merged.machineName.trimmed().isEmpty()) merged.machineName = previous.machineName;
    if (merged.platform.trimmed().isEmpty()) merged.platform = previous.platform;
    return merged;
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
    m_mediaReleaseExpiredTargets.clear();
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
        m_targetByProjectId.insert(project.projectId, project.targetEndpointId);
        m_projectsByTarget.insert(project.targetEndpointId, project);
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

bool ProjectManager::hasProjectForTarget(const QString& targetEndpointId) const
{
    return m_projectsByTarget.contains(targetEndpointId);
}

ProjectRecord* ProjectManager::mutableProjectForTarget(const QString& targetEndpointId)
{
    auto it = m_projectsByTarget.find(targetEndpointId);
    return it == m_projectsByTarget.end() ? nullptr : &it.value();
}

const ProjectRecord* ProjectManager::projectForTarget(const QString& targetEndpointId) const
{
    auto it = m_projectsByTarget.constFind(targetEndpointId);
    return it == m_projectsByTarget.constEnd() ? nullptr : &it.value();
}

const ProjectRecord* ProjectManager::projectById(const QString& projectId) const
{
    return projectForTarget(m_targetByProjectId.value(projectId));
}

QString ProjectManager::createProjectFromSnapshot(
    const ProjectTargetReference& target,
    const QList<ScreenInfo>& screens,
    int volumePercent,
    quint64 snapshotRevision,
    qint64 snapshotCapturedAtMs,
    qint64 atMs)
{
    if (!target.isValid() || hasProjectForTarget(target.endpointId)
        || volumePercent < -1 || volumePercent > 100
        || snapshotRevision == 0 || snapshotCapturedAtMs < 1) {
        return {};
    }
    const qint64 current = atMs >= 0 ? atMs : nowMs();
    ProjectRecord project;
    project.projectId = QUuid::createUuid().toString(QUuid::WithoutBraces);
    project.targetEndpointId = target.endpointId;
    project.target = target;
    project.savedScreens = screens;
    project.savedVolumePercent = volumePercent;
    project.snapshotRevision = snapshotRevision;
    project.snapshotCapturedAtMs = snapshotCapturedAtMs;
    project.state = ProjectLifecycleState::Visible;
    project.createdAtMs = current;
    project.updatedAtMs = current;
    project.lastCheckpointAtMs = current;
    project.hiddenAtMs = -1;

    const bool hadPendingChanges = m_dirty;
    m_projectsByTarget.insert(project.targetEndpointId, project);
    m_targetByProjectId.insert(project.projectId, project.targetEndpointId);
    m_dirty = true;
    if (!flush()) {
        m_projectsByTarget.remove(project.targetEndpointId);
        m_targetByProjectId.remove(project.projectId);
        m_dirty = hadPendingChanges;
        if (hadPendingChanges) scheduleSave();
        return {};
    }
    emit projectCreated(project.projectId, project.targetEndpointId);
    emit projectsChanged();
    return project.projectId;
}

bool ProjectManager::setVisible(const QString& targetEndpointId, qint64 atMs)
{
    const qint64 current = atMs >= 0 ? atMs : nowMs();
    ProjectRecord* project = mutableProjectForTarget(targetEndpointId);
    if (!project) {
        return false;
    }
    const QString projectId = project->projectId;
    releaseProjectMediaIfDue(targetEndpointId, current);
    // A release observer can delete, replace or reopen this project.
    project = mutableProjectForTarget(targetEndpointId);
    if (!project || project->projectId != projectId) {
        return false;
    }
    if (project->state == ProjectLifecycleState::Hidden
        && project->hiddenAtMs >= 0
        && current >= project->hiddenAtMs + m_timing.projectHiddenRetentionMs) {
        removeProjectInternal(targetEndpointId);
        return false;
    }
    if (project->state == ProjectLifecycleState::Visible) {
        return true;
    }

    project->state = ProjectLifecycleState::Visible;
    project->hiddenAtMs = -1;
    project->lastCheckpointAtMs = current;
    project->updatedAtMs = current;
    m_mediaReleaseExpiredTargets.remove(targetEndpointId);
    scheduleSave();
    emit projectVisibilityChanged(projectId, targetEndpointId, ProjectLifecycleState::Visible);
    emit projectUpdated(projectId, targetEndpointId);
    emit projectsChanged();
    return true;
}

bool ProjectManager::setHidden(const QString& targetEndpointId, qint64 atMs)
{
    ProjectRecord* project = mutableProjectForTarget(targetEndpointId);
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
    m_mediaReleaseExpiredTargets.remove(targetEndpointId);
    const QString projectId = project->projectId;
    scheduleSave();
    emit projectVisibilityChanged(projectId, targetEndpointId, ProjectLifecycleState::Hidden);
    emit projectUpdated(projectId, targetEndpointId);
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

bool ProjectManager::removeProjectInternal(const QString& targetEndpointId)
{
    const auto it = m_projectsByTarget.constFind(targetEndpointId);
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
                                     [&targetEndpointId](const ProjectRecord& project) {
        return project.targetEndpointId == targetEndpointId;
    }), prospective.end());
    if (!persistProjects(prospective)) {
        return false;
    }

    m_projectsByTarget.remove(targetEndpointId);
    m_targetByProjectId.remove(snapshot.projectId);
    m_mediaReleaseExpiredTargets.remove(targetEndpointId);
    emit projectDeleted(snapshot.projectId, targetEndpointId);
    emit projectsChanged();
    return true;
}

bool ProjectManager::deleteProject(const QString& targetEndpointId)
{
    return removeProjectInternal(targetEndpointId);
}

bool ProjectManager::updateCanvasState(const QString& targetEndpointId,
                                       const QJsonObject& canvasState,
                                       const QList<ProjectMediaReference>& references,
                                       const QList<ScreenInfo>& savedScreens,
                                       qint64 atMs)
{
    ProjectRecord* project = mutableProjectForTarget(targetEndpointId);
    if (!project) {
        return false;
    }
    project->canvasState = canvasState;
    project->mediaReferences = references;
    project->savedScreens = savedScreens;
    project->updatedAtMs = atMs >= 0 ? atMs : nowMs();
    scheduleSave();
    emit projectUpdated(project->projectId, targetEndpointId);
    emit projectsChanged();
    return true;
}

bool ProjectManager::updateTargetReference(const ProjectTargetReference& target,
                                           qint64 atMs)
{
    ProjectRecord* project = mutableProjectForTarget(target.endpointId);
    if (!project || !target.isValid()) {
        return false;
    }
    const ProjectTargetReference merged =
        mergeTargetPresentation(project->target, target);
    if (targetContentEquals(project->target, merged)) {
        return true;
    }
    project->target = merged;
    project->updatedAtMs = atMs >= 0 ? atMs : nowMs();
    scheduleSave();
    emit projectUpdated(project->projectId, project->targetEndpointId);
    emit projectsChanged();
    return true;
}

bool ProjectManager::updateSavedScreens(const QString& targetEndpointId,
                                        const QList<ScreenInfo>& screens,
                                        qint64 atMs)
{
    ProjectRecord* project = mutableProjectForTarget(targetEndpointId);
    if (!project) return false;
    QJsonArray before;
    QJsonArray after;
    for (const ScreenInfo& screen : project->savedScreens) before.append(screen.toJson());
    for (const ScreenInfo& screen : screens) after.append(screen.toJson());
    if (before == after) return true;
    project->savedScreens = screens;
    project->updatedAtMs = atMs >= 0 ? atMs : nowMs();
    scheduleSave();
    emit projectUpdated(project->projectId, targetEndpointId);
    emit projectsChanged();
    return true;
}

bool ProjectManager::updateRemoteSnapshot(
    const QString& targetEndpointId,
    const QList<ScreenInfo>& screens,
    int volumePercent,
    quint64 snapshotRevision,
    qint64 snapshotCapturedAtMs,
    qint64 atMs)
{
    ProjectRecord* project = mutableProjectForTarget(targetEndpointId);
    if (!project || volumePercent < -1 || volumePercent > 100
        || snapshotRevision == 0 || snapshotCapturedAtMs < 0) {
        return false;
    }
    project->savedScreens = screens;
    project->savedVolumePercent = volumePercent;
    project->snapshotRevision = snapshotRevision;
    project->snapshotCapturedAtMs = snapshotCapturedAtMs;
    project->updatedAtMs = atMs >= 0 ? atMs : nowMs();
    scheduleSave();
    emit projectUpdated(project->projectId, targetEndpointId);
    emit projectsChanged();
    return true;
}

qint64 ProjectManager::projectDeleteAtMs(const QString& targetEndpointId) const
{
    const ProjectRecord* project = projectForTarget(targetEndpointId);
    return project && project->state == ProjectLifecycleState::Hidden && project->hiddenAtMs >= 0
        ? project->hiddenAtMs + m_timing.projectHiddenRetentionMs
        : -1;
}

qint64 ProjectManager::projectMediaReleaseAtMs(const QString& targetEndpointId) const
{
    const ProjectRecord* project = projectForTarget(targetEndpointId);
    return project && project->state == ProjectLifecycleState::Hidden && project->hiddenAtMs >= 0
            && !projectMediaReleaseExpired(targetEndpointId)
        ? project->hiddenAtMs + m_timing.projectMediaHiddenTimeoutMs
        : -1;
}

bool ProjectManager::projectMediaReleaseExpired(const QString& targetEndpointId) const
{
    return m_mediaReleaseExpiredTargets.contains(targetEndpointId);
}

void ProjectManager::releaseProjectMediaIfDue(const QString& targetEndpointId, qint64 atMs)
{
    const qint64 deadline = projectMediaReleaseAtMs(targetEndpointId);
    if (deadline < 0 || atMs < deadline || projectMediaReleaseExpired(targetEndpointId)) {
        return;
    }
    const QString projectId = projectForTarget(targetEndpointId)->projectId;
    // Mark first: observers may process deadlines again or update residency.
    m_mediaReleaseExpiredTargets.insert(targetEndpointId);
    emit projectMediaReleaseDue(projectId, targetEndpointId);
    emit projectsChanged();
}

void ProjectManager::processDeadlines(qint64 atMs)
{
    if (m_processingDeadlines) {
        return;
    }
    const QScopedValueRollback<bool> processing(m_processingDeadlines, true);
    const qint64 current = atMs >= 0 ? atMs : nowMs();
    const QStringList targets = m_projectsByTarget.keys();
    for (const QString& target : targets) {
        releaseProjectMediaIfDue(target, current);
    }
    // RAM release must not be delayed by an unavailable project store.
    for (const QString& target : targets) {
        // Release callbacks may change visibility or remove a project.
        const qint64 deleteAt = projectDeleteAtMs(target);
        if (deleteAt < 0 || current < deleteAt) {
            continue;
        }
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

    QList<ProjectClientEntry> result;
    QSet<QString> seen;
    bool targetChanged = false;
    for (ClientInfo client : discovered) {
        const QString endpointId = client.endpointId().trimmed();
        if (endpointId.isEmpty() || seen.contains(endpointId)) {
            continue;
        }
        seen.insert(endpointId);
        client.setOnline(true);

        ProjectClientEntry entry;
        entry.endpointId = endpointId;
        entry.online = true;
        if (ProjectRecord* project = mutableProjectForTarget(endpointId)) {
            const ProjectTargetReference fresh = mergeTargetPresentation(
                project->target,
                ProjectTargetReference::fromClientInfo(client));
            if (!targetContentEquals(project->target, fresh)) {
                project->target = fresh;
                project->updatedAtMs = current;
                targetChanged = true;
                emit projectUpdated(project->projectId, endpointId);
            }
            if (client.getMachineName().trimmed().isEmpty()) {
                client.setMachineName(fresh.machineName);
            }
            if (client.getPlatform().trimmed().isEmpty()) {
                client.setPlatform(fresh.platform);
            }
            client.setScreens(project->savedScreens);
            client.setVolumePercent(project->savedVolumePercent);
            entry.hasProject = true;
            entry.projectId = project->projectId;
            entry.projectState = project->state;
            entry.projectDeleteAtMs = projectDeleteAtMs(endpointId);
        }
        entry.client = client;
        result.append(entry);
    }

    for (const ProjectRecord& project : sortedProjects()) {
        if (seen.contains(project.targetEndpointId)) {
            continue;
        }
        ProjectClientEntry entry;
        entry.client = project.target.toClientInfo(false);
        entry.client.setScreens(project.savedScreens);
        entry.client.setVolumePercent(project.savedVolumePercent);
        entry.endpointId = project.targetEndpointId;
        entry.projectId = project.projectId;
        entry.hasProject = true;
        entry.online = false;
        entry.projectState = project.state;
        entry.projectDeleteAtMs = projectDeleteAtMs(project.targetEndpointId);
        result.append(entry);
    }

    if (targetChanged) {
        scheduleSave();
        emit projectsChanged();
    }
    return result;
}
