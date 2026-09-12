#include "backend/domain/scene/SceneActivityModel.h"

#include <QDateTime>

#include <algorithm>

SceneActivityModel::SceneActivityModel(QObject* parent)
    : QObject(parent)
{
}

void SceneActivityModel::setLocalEndpointId(const QString& endpointId)
{
    if (m_localEndpointId == endpointId) return;
    const int previousCount = m_activities.size();
    m_localEndpointId = endpointId;

    // Activities cannot be safely re-attributed across installation identities.
    // This normally only runs once, immediately after signed authentication.
    if (!m_activities.isEmpty()) m_activities.clear();
    publishChange(previousCount);
}

bool SceneActivityModel::upsertLive(const QString& sceneRunId,
                                    const QString& remoteSessionId,
                                    const QString& ownerEndpointId,
                                    const QString& targetEndpointId,
                                    qint64 startedAtEpochMs,
                                    bool degraded)
{
    if (sceneRunId.isEmpty() || remoteSessionId.isEmpty()
        || ownerEndpointId.isEmpty() || targetEndpointId.isEmpty()
        || ownerEndpointId == targetEndpointId || m_localEndpointId.isEmpty()
        || (m_localEndpointId != ownerEndpointId && m_localEndpointId != targetEndpointId)) {
        return false;
    }

    const auto existing = m_activities.find(sceneRunId);
    if (existing != m_activities.end()) {
        // A SceneRun identity is immutable. A duplicated STARTED envelope may
        // refresh health only; it can never rebind the card to another session
        // or peer, nor rewrite its original start time.
        if (existing->remoteSessionId != remoteSessionId
            || existing->ownerEndpointId != ownerEndpointId
            || existing->targetEndpointId != targetEndpointId
            || (startedAtEpochMs > 0
                && existing->startedAtEpochMs != startedAtEpochMs)) {
            return false;
        }
        if (existing->degraded != degraded) {
            existing->degraded = degraded;
            emit activitiesChanged();
        }
        return true;
    }

    Activity next;
    next.sceneRunId = sceneRunId;
    next.remoteSessionId = remoteSessionId;
    next.ownerEndpointId = ownerEndpointId;
    next.targetEndpointId = targetEndpointId;
    next.direction = m_localEndpointId == ownerEndpointId
        ? Direction::Outgoing : Direction::Incoming;
    next.peerEndpointId = next.direction == Direction::Outgoing
        ? targetEndpointId : ownerEndpointId;
    next.startedAtEpochMs = startedAtEpochMs > 0
        ? startedAtEpochMs : QDateTime::currentMSecsSinceEpoch();
    next.degraded = degraded;

    const int previousCount = m_activities.size();
    m_activities.insert(sceneRunId, next);
    publishChange(previousCount);
    return true;
}

bool SceneActivityModel::remove(const QString& sceneRunId)
{
    const int previousCount = m_activities.size();
    if (m_activities.remove(sceneRunId) == 0) return false;
    publishChange(previousCount);
    return true;
}

int SceneActivityModel::removeForSession(const QString& remoteSessionId)
{
    const int previousCount = m_activities.size();
    int removed = 0;
    for (auto iterator = m_activities.begin(); iterator != m_activities.end();) {
        if (iterator->remoteSessionId == remoteSessionId) {
            iterator = m_activities.erase(iterator);
            ++removed;
        } else {
            ++iterator;
        }
    }
    if (removed > 0) publishChange(previousCount);
    return removed;
}

void SceneActivityModel::setSessionDegraded(const QString& remoteSessionId,
                                            bool degraded)
{
    bool changed = false;
    for (Activity& activity : m_activities) {
        if (activity.remoteSessionId == remoteSessionId
            && activity.degraded != degraded) {
            activity.degraded = degraded;
            changed = true;
        }
    }
    if (changed) emit activitiesChanged();
}

void SceneActivityModel::setAllDegraded(bool degraded)
{
    bool changed = false;
    for (Activity& activity : m_activities) {
        if (activity.degraded != degraded) {
            activity.degraded = degraded;
            changed = true;
        }
    }
    if (changed) emit activitiesChanged();
}

void SceneActivityModel::clear()
{
    if (m_activities.isEmpty()) return;
    const int previousCount = m_activities.size();
    m_activities.clear();
    publishChange(previousCount);
}

QList<SceneActivityModel::Activity> SceneActivityModel::liveActivities() const
{
    QList<Activity> result = m_activities.values();
    std::sort(result.begin(), result.end(), [](const Activity& left,
                                               const Activity& right) {
        if (left.startedAtEpochMs != right.startedAtEpochMs) {
            return left.startedAtEpochMs > right.startedAtEpochMs;
        }
        return left.sceneRunId < right.sceneRunId;
    });
    return result;
}

SceneActivityModel::Activity SceneActivityModel::activity(
    const QString& sceneRunId) const
{
    return m_activities.value(sceneRunId);
}

void SceneActivityModel::publishChange(int previousCount)
{
    emit activitiesChanged();
    if (previousCount != m_activities.size()) {
        emit liveCountChanged(m_activities.size());
    }
}
