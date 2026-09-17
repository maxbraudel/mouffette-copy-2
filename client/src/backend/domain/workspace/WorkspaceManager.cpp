#include "backend/runtime/SuspendInclusiveClock.h"
#include "backend/domain/workspace/WorkspaceManager.h"

#include "backend/config/AppConfig.h"

#include <QDateTime>
#include <QDebug>
#include <utility>

namespace {
ClientInfo mergeClientPresentation(const ClientInfo& previous,
                                   const ClientInfo& incoming)
{
    ClientInfo merged = incoming;
    if (merged.getMachineName().trimmed().isEmpty()) {
        merged.setMachineName(previous.getMachineName());
    }
    if (merged.endpointId().trimmed().isEmpty()) {
        merged.setEndpointId(previous.endpointId());
    }
    if (merged.getPlatform().trimmed().isEmpty()) {
        merged.setPlatform(previous.getPlatform());
    }
    if (merged.installationId().trimmed().isEmpty()) {
        merged.setInstallationId(previous.installationId());
    }
    if (merged.instanceId().trimmed().isEmpty()) {
        merged.setInstanceId(previous.instanceId());
        merged.setInstanceOrdinal(previous.instanceOrdinal());
    }
    if (merged.runtimeId().trimmed().isEmpty()) {
        merged.setRuntimeId(previous.runtimeId());
    }
    return merged;
}

qint64 currentEpochMs(const std::function<qint64()>& provider)
{
    return provider ? provider() : MouffetteClock::anchoredEpochMs();
}
}

WorkspaceManager::WorkspaceManager(QObject* parent)
    : QObject(parent)
{
    m_deadlineTimer.setInterval(
        AppConfig::instance().sessionDeadlinePollIntervalMs());
    connect(&m_deadlineTimer, &QTimer::timeout,
            this, [this]() { processDeadlines(); });
    m_deadlineTimer.start();
}

WorkspaceManager::~WorkspaceManager()
{
    clearAllWorkspaces();
}

WorkspaceManager::ClientWorkspace* WorkspaceManager::findWorkspace(
    const QString& targetEndpointId)
{
    if (targetEndpointId.isEmpty()) return nullptr;
    auto it = m_workspaces.find(targetEndpointId);
    return it == m_workspaces.end() ? nullptr : &it.value();
}

const WorkspaceManager::ClientWorkspace* WorkspaceManager::findWorkspace(
    const QString& targetEndpointId) const
{
    if (targetEndpointId.isEmpty()) return nullptr;
    auto it = m_workspaces.constFind(targetEndpointId);
    return it == m_workspaces.cend() ? nullptr : &it.value();
}

WorkspaceManager::ClientWorkspace* WorkspaceManager::getOrCreateWorkspace(
    const QString& targetEndpointId,
    const ClientInfo& clientInfo)
{
    if (targetEndpointId.isEmpty()) {
        qCritical() << "WorkspaceManager: refused an empty target endpoint";
        return nullptr;
    }

    auto existing = m_workspaces.find(targetEndpointId);
    if (existing != m_workspaces.end()) {
        existing->lastClientInfo =
            mergeClientPresentation(existing->lastClientInfo, clientInfo);
        emit workspaceModified(targetEndpointId);
        return &existing.value();
    }

    ClientWorkspace workspace;
    workspace.targetEndpointId = targetEndpointId;
    workspace.projectId = clientInfo.projectId();
    workspace.lastClientInfo = clientInfo;
    m_workspaces.insert(targetEndpointId, workspace);
    emit workspaceCreated(targetEndpointId);
    return &m_workspaces[targetEndpointId];
}

bool WorkspaceManager::hasWorkspace(const QString& targetEndpointId) const
{
    return m_workspaces.contains(targetEndpointId);
}

void WorkspaceManager::deleteWorkspace(const QString& targetEndpointId)
{
    if (targetEndpointId.isEmpty() || !m_workspaces.remove(targetEndpointId)) {
        return;
    }
    emit workspaceDeleted(targetEndpointId);
}

void WorkspaceManager::clearAllWorkspaces()
{
    m_workspaces.clear();
}

QList<QString> WorkspaceManager::allTargetEndpointIds() const
{
    return m_workspaces.keys();
}

QList<WorkspaceManager::ClientWorkspace*> WorkspaceManager::allWorkspaces()
{
    QList<ClientWorkspace*> workspaces;
    workspaces.reserve(m_workspaces.size());
    for (auto it = m_workspaces.begin(); it != m_workspaces.end(); ++it) {
        workspaces.append(&it.value());
    }
    return workspaces;
}

QList<const WorkspaceManager::ClientWorkspace*> WorkspaceManager::allWorkspaces() const
{
    QList<const ClientWorkspace*> workspaces;
    workspaces.reserve(m_workspaces.size());
    for (auto it = m_workspaces.cbegin(); it != m_workspaces.cend(); ++it) {
        workspaces.append(&it.value());
    }
    return workspaces;
}

void WorkspaceManager::setRemoteSessionHiddenTimeoutMs(qint64 timeoutMs)
{
    m_remoteSessionHiddenTimeoutMs = qMax<qint64>(1, timeoutMs);
}

WorkspaceManager::RemoteSessionState WorkspaceManager::remoteSessionState(
    const QString& targetEndpointId) const
{
    const ClientWorkspace* workspace = findWorkspace(targetEndpointId);
    return workspace ? workspace->remoteSessionState : RemoteSessionState::Absent;
}

bool WorkspaceManager::setRemoteSessionState(const QString& targetEndpointId,
                                             RemoteSessionState state)
{
    ClientWorkspace* workspace = findWorkspace(targetEndpointId);
    if (!workspace || workspace->remoteSessionState == state) return workspace;
    workspace->remoteSessionState = state;
    if (state == RemoteSessionState::Absent
        || state == RemoteSessionState::Closing) {
        workspace->sessionHiddenAtMs = -1;
    } else if (!workspace->workspaceVisible && workspace->sessionHiddenAtMs < 0) {
        workspace->sessionHiddenAtMs = currentEpochMs(m_nowProvider);
    }
    emit remoteSessionStateChanged(targetEndpointId, state);
    emit workspaceModified(targetEndpointId);
    return true;
}

bool WorkspaceManager::setWorkspaceVisible(const QString& targetEndpointId,
                                           qint64 nowMs)
{
    Q_UNUSED(nowMs);
    ClientWorkspace* workspace = findWorkspace(targetEndpointId);
    if (!workspace) return false;
    workspace->workspaceVisible = true;
    workspace->sessionHiddenAtMs = -1;
    emit workspaceModified(targetEndpointId);
    return true;
}

bool WorkspaceManager::setWorkspaceHidden(const QString& targetEndpointId,
                                          qint64 nowMs)
{
    ClientWorkspace* workspace = findWorkspace(targetEndpointId);
    if (!workspace) return false;
    workspace->workspaceVisible = false;
    const bool hasLease = workspace->remoteSessionState == RemoteSessionState::Opening
        || workspace->remoteSessionState == RemoteSessionState::Active
        || workspace->remoteSessionState == RemoteSessionState::Grace;
    if (hasLease && workspace->sessionHiddenAtMs < 0) {
        workspace->sessionHiddenAtMs = nowMs >= 0
            ? nowMs : currentEpochMs(m_nowProvider);
    }
    emit workspaceModified(targetEndpointId);
    return true;
}

void WorkspaceManager::markAllWorkspacesHidden(qint64 nowMs)
{
    const qint64 timestamp = nowMs >= 0 ? nowMs : currentEpochMs(m_nowProvider);
    for (const QString& targetEndpointId : m_workspaces.keys()) {
        setWorkspaceHidden(targetEndpointId, timestamp);
    }
}

qint64 WorkspaceManager::remoteSessionCloseAtMs(
    const QString& targetEndpointId) const
{
    const ClientWorkspace* workspace = findWorkspace(targetEndpointId);
    if (!workspace || workspace->sessionHiddenAtMs < 0) return -1;
    const bool hasLease = workspace->remoteSessionState == RemoteSessionState::Opening
        || workspace->remoteSessionState == RemoteSessionState::Active
        || workspace->remoteSessionState == RemoteSessionState::Grace;
    return hasLease
        ? workspace->sessionHiddenAtMs + m_remoteSessionHiddenTimeoutMs : -1;
}

void WorkspaceManager::processDeadlines(qint64 nowMs)
{
    const qint64 timestamp = nowMs >= 0 ? nowMs : currentEpochMs(m_nowProvider);
    for (const QString& targetEndpointId : m_workspaces.keys()) {
        ClientWorkspace* workspace = findWorkspace(targetEndpointId);
        const qint64 deadline = remoteSessionCloseAtMs(targetEndpointId);
        if (!workspace || deadline < 0 || timestamp < deadline) continue;
        workspace->remoteSessionState = RemoteSessionState::Closing;
        workspace->sessionHiddenAtMs = -1;
        emit remoteSessionStateChanged(targetEndpointId,
                                       RemoteSessionState::Closing);
        emit workspaceModified(targetEndpointId);
        emit remoteSessionCloseDue(targetEndpointId);
    }
}

void WorkspaceManager::setNowProviderForTesting(std::function<qint64()> provider)
{
    m_nowProvider = std::move(provider);
}

void WorkspaceManager::stopAutomaticTimersForTesting()
{
    m_deadlineTimer.stop();
}

void WorkspaceManager::updateWorkspaceProjectId(
    const QString& targetEndpointId,
    const QString& projectId)
{
    ClientWorkspace* workspace = findWorkspace(targetEndpointId);
    if (!workspace || workspace->projectId == projectId) return;
    workspace->projectId = projectId;
    emit workspaceModified(targetEndpointId);
}
