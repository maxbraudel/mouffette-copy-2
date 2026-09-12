#include "backend/network/UploadScheduler.h"

#include <QtGlobal>

UploadScheduler::UploadScheduler(int maximumConcurrentUploads, QObject *parent)
    : QObject(parent)
    , m_maximumConcurrentUploads(qMax(1, maximumConcurrentUploads))
{
    qRegisterMetaType<UploadRequest>("UploadScheduler::UploadRequest");
}

int UploadScheduler::maximumConcurrentUploads() const noexcept
{
    return m_maximumConcurrentUploads;
}

bool UploadScheduler::setMaximumConcurrentUploads(int maximum)
{
    if (maximum < 1) {
        return false;
    }
    if (maximum == m_maximumConcurrentUploads) {
        return true;
    }
    m_maximumConcurrentUploads = maximum;
    dispatch();
    return true;
}

bool UploadScheduler::setSessionState(const QString &remoteSessionId,
                                      SessionState state)
{
    if (remoteSessionId.isEmpty()) {
        return false;
    }

    const bool wasTerminal = m_terminalSessions.contains(remoteSessionId);
    if (wasTerminal && !isTerminalState(state)) {
        return false;
    }

    m_sessionStates.insert(remoteSessionId, state);
    if (isTerminalState(state)) {
        if (!wasTerminal) {
            m_terminalSessions.insert(remoteSessionId);
            purgeTerminalSession(remoteSessionId);
        }
        return true;
    }

    dispatch();
    return true;
}

UploadScheduler::SessionState
UploadScheduler::sessionState(const QString &remoteSessionId) const
{
    return m_sessionStates.value(remoteSessionId, SessionState::Opening);
}

bool UploadScheduler::isSessionTerminal(const QString &remoteSessionId) const
{
    return m_terminalSessions.contains(remoteSessionId);
}

UploadScheduler::EnqueueResult
UploadScheduler::enqueue(const UploadRequest &request)
{
    if (!isValid(request)) {
        return EnqueueResult::InvalidRequest;
    }
    if (m_terminalSessions.contains(request.remoteSessionId)) {
        return EnqueueResult::SessionTerminal;
    }

    const QString key = uploadKey(request.remoteSessionId, request.uploadId);
    if (m_seenUploadKeys.contains(key)) {
        return EnqueueResult::Duplicate;
    }

    m_seenUploadKeys.insert(key);
    m_pendingBySession[request.remoteSessionId].enqueue(request);
    ensureRoundRobinEntry(request.remoteSessionId);
    dispatch();
    return EnqueueResult::Enqueued;
}

bool UploadScheduler::completeUpload(const QString &remoteSessionId,
                                     quint64 connectionGeneration,
                                     const QString &uploadId)
{
    return releaseActive(remoteSessionId, connectionGeneration, uploadId);
}

bool UploadScheduler::failUpload(const QString &remoteSessionId,
                                 quint64 connectionGeneration,
                                 const QString &uploadId)
{
    return releaseActive(remoteSessionId, connectionGeneration, uploadId);
}

bool UploadScheduler::cancelUpload(const QString &remoteSessionId,
                                   quint64 connectionGeneration,
                                   const QString &uploadId)
{
    const auto activeIt = m_activeBySession.constFind(remoteSessionId);
    if (activeIt != m_activeBySession.cend()
        && activeIt->connectionGeneration == connectionGeneration
        && activeIt->uploadId == uploadId) {
        const UploadRequest request = *activeIt;
        m_activeBySession.remove(remoteSessionId);
        emit uploadCancelled(request, true);
        dispatch();
        return true;
    }

    auto queueIt = m_pendingBySession.find(remoteSessionId);
    if (queueIt == m_pendingBySession.end()) {
        return false;
    }

    QQueue<UploadRequest> retained;
    bool cancelled = false;
    UploadRequest cancelledRequest;
    while (!queueIt->isEmpty()) {
        UploadRequest request = queueIt->dequeue();
        if (!cancelled
            && request.connectionGeneration == connectionGeneration
            && request.uploadId == uploadId) {
            cancelled = true;
            cancelledRequest = request;
        } else {
            retained.enqueue(request);
        }
    }

    if (!cancelled) {
        *queueIt = retained;
        return false;
    }

    if (retained.isEmpty()) {
        m_pendingBySession.erase(queueIt);
        removeRoundRobinEntry(remoteSessionId);
    } else {
        *queueIt = retained;
    }
    emit uploadCancelled(cancelledRequest, false);
    dispatch();
    return true;
}

bool UploadScheduler::cancelSession(const QString &remoteSessionId)
{
    return setSessionState(remoteSessionId, SessionState::Terminating);
}

int UploadScheduler::activeCount() const noexcept
{
    return m_activeBySession.size();
}

int UploadScheduler::queuedCount() const noexcept
{
    int count = 0;
    for (auto it = m_pendingBySession.cbegin(); it != m_pendingBySession.cend(); ++it) {
        count += it->size();
    }
    return count;
}

int UploadScheduler::queuedCount(const QString &remoteSessionId) const
{
    const auto it = m_pendingBySession.constFind(remoteSessionId);
    return it == m_pendingBySession.cend() ? 0 : it->size();
}

bool UploadScheduler::isUploadActive(const QString &remoteSessionId,
                                     quint64 connectionGeneration,
                                     const QString &uploadId) const
{
    const auto it = m_activeBySession.constFind(remoteSessionId);
    return it != m_activeBySession.cend()
        && it->connectionGeneration == connectionGeneration
        && it->uploadId == uploadId;
}

QVector<UploadScheduler::UploadRequest>
UploadScheduler::queuedUploads(const QString &remoteSessionId) const
{
    QVector<UploadRequest> result;
    const auto it = m_pendingBySession.constFind(remoteSessionId);
    if (it == m_pendingBySession.cend()) {
        return result;
    }
    result.reserve(it->size());
    for (const UploadRequest &request : *it) {
        result.append(request);
    }
    return result;
}

bool UploadScheduler::isTerminalState(SessionState state) noexcept
{
    return state == SessionState::Terminating
        || state == SessionState::CleanupPending
        || state == SessionState::Closed;
}

QString UploadScheduler::uploadKey(const QString &remoteSessionId,
                                   const QString &uploadId)
{
    // Length-prefixing makes the key unambiguous without restricting protocol IDs.
    return QString::number(remoteSessionId.size()) + QLatin1Char(':')
        + remoteSessionId + QLatin1Char(':')
        + uploadId;
}

bool UploadScheduler::isValid(const UploadRequest &request) noexcept
{
    return !request.remoteSessionId.isEmpty()
        && request.connectionGeneration > 0
        && !request.uploadId.isEmpty()
        && !request.assetId.isEmpty();
}

void UploadScheduler::dispatch()
{
    if (m_dispatching) {
        m_dispatchAgain = true;
        return;
    }

    m_dispatching = true;
    do {
        m_dispatchAgain = false;
        while (m_activeBySession.size() < m_maximumConcurrentUploads
               && !m_roundRobinSessions.isEmpty()) {
            const int candidates = m_roundRobinSessions.size();
            bool started = false;

            for (int examined = 0; examined < candidates; ++examined) {
                const QString sessionId = m_roundRobinSessions.dequeue();
                m_roundRobinMembership.remove(sessionId);

                auto queueIt = m_pendingBySession.find(sessionId);
                if (queueIt == m_pendingBySession.end() || queueIt->isEmpty()) {
                    if (queueIt != m_pendingBySession.end()) {
                        m_pendingBySession.erase(queueIt);
                    }
                    continue;
                }

                const bool eligible = sessionState(sessionId) == SessionState::Active
                    && !m_activeBySession.contains(sessionId)
                    && !m_terminalSessions.contains(sessionId);
                const bool deferLastSession = eligible
                    && sessionId == m_lastStartedSession
                    && hasOtherEligibleSession(sessionId);

                if (!eligible || deferLastSession) {
                    ensureRoundRobinEntry(sessionId);
                    continue;
                }

                const UploadRequest request = queueIt->dequeue();
                if (queueIt->isEmpty()) {
                    m_pendingBySession.erase(queueIt);
                } else {
                    ensureRoundRobinEntry(sessionId);
                }

                m_activeBySession.insert(sessionId, request);
                m_lastStartedSession = sessionId;
                started = true;
                emit uploadStartRequested(request);
                break;
            }

            if (!started) {
                break;
            }
        }
    } while (m_dispatchAgain);
    m_dispatching = false;
}

void UploadScheduler::ensureRoundRobinEntry(const QString &remoteSessionId)
{
    const auto pendingIt = m_pendingBySession.constFind(remoteSessionId);
    if (pendingIt == m_pendingBySession.cend() || pendingIt->isEmpty()
        || m_roundRobinMembership.contains(remoteSessionId)) {
        return;
    }
    m_roundRobinSessions.enqueue(remoteSessionId);
    m_roundRobinMembership.insert(remoteSessionId);
}

void UploadScheduler::removeRoundRobinEntry(const QString &remoteSessionId)
{
    if (!m_roundRobinMembership.remove(remoteSessionId)) {
        return;
    }
    QQueue<QString> retained;
    while (!m_roundRobinSessions.isEmpty()) {
        const QString candidate = m_roundRobinSessions.dequeue();
        if (candidate != remoteSessionId) {
            retained.enqueue(candidate);
        }
    }
    m_roundRobinSessions = retained;
}

bool UploadScheduler::hasOtherEligibleSession(const QString &excludedSessionId) const
{
    for (const QString &sessionId : m_roundRobinSessions) {
        if (sessionId == excludedSessionId) {
            continue;
        }
        const auto queueIt = m_pendingBySession.constFind(sessionId);
        if (queueIt != m_pendingBySession.cend() && !queueIt->isEmpty()
            && sessionState(sessionId) == SessionState::Active
            && !m_activeBySession.contains(sessionId)
            && !m_terminalSessions.contains(sessionId)) {
            return true;
        }
    }
    return false;
}

bool UploadScheduler::releaseActive(const QString &remoteSessionId,
                                    quint64 connectionGeneration,
                                    const QString &uploadId)
{
    const auto it = m_activeBySession.constFind(remoteSessionId);
    if (it == m_activeBySession.cend()
        || it->connectionGeneration != connectionGeneration
        || it->uploadId != uploadId) {
        return false;
    }
    m_activeBySession.remove(remoteSessionId);
    dispatch();
    return true;
}

void UploadScheduler::purgeTerminalSession(const QString &remoteSessionId)
{
    removeRoundRobinEntry(remoteSessionId);

    QQueue<UploadRequest> queued = m_pendingBySession.take(remoteSessionId);
    const int queuedUploadCount = queued.size();

    const auto activeIt = m_activeBySession.constFind(remoteSessionId);
    const bool hadActiveUpload = activeIt != m_activeBySession.cend();
    UploadRequest activeRequest;
    if (hadActiveUpload) {
        activeRequest = *activeIt;
        m_activeBySession.remove(remoteSessionId);
    }

    // Update all internal state before emitting, so synchronous handlers can
    // safely query or enqueue work for unrelated sessions.
    if (hadActiveUpload) {
        emit uploadCancelled(activeRequest, true);
    }
    while (!queued.isEmpty()) {
        emit uploadCancelled(queued.dequeue(), false);
    }
    emit sessionPurged(remoteSessionId, queuedUploadCount, hadActiveUpload);
    dispatch();
}
