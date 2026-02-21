#include "backend/domain/session/SessionManager.h"
#include <QUuid>
#include <QPushButton>
#include <QDebug>

SessionManager::SessionManager(QObject *parent)
    : QObject(parent)
{
}

SessionManager::~SessionManager()
{
    clearAllSessions();
}

// Session lookup by persistentClientId
SessionManager::CanvasSession* SessionManager::findSession(const QString& persistentClientId) {
    if (persistentClientId.isEmpty()) {
        return nullptr;
    }
    auto it = m_sessions.find(persistentClientId);
    return (it != m_sessions.end()) ? &it.value() : nullptr;
}

const SessionManager::CanvasSession* SessionManager::findSession(const QString& persistentClientId) const {
    if (persistentClientId.isEmpty()) {
        return nullptr;
    }
    auto it = m_sessions.constFind(persistentClientId);
    return (it != m_sessions.constEnd()) ? &it.value() : nullptr;
}

// Session lookup by canvasSessionId - PHASE 1 OPTIMIZED: O(1) with secondary index
SessionManager::CanvasSession* SessionManager::findSessionByIdeaId(const QString& canvasSessionId) {
    if (canvasSessionId.isEmpty()) {
        return nullptr;
    }
    QString persistentClientId = m_canvasSessionIdToClientId.value(canvasSessionId);
    if (persistentClientId.isEmpty()) {
        return nullptr;
    }
    return findSession(persistentClientId);
}

const SessionManager::CanvasSession* SessionManager::findSessionByIdeaId(const QString& canvasSessionId) const {
    if (canvasSessionId.isEmpty()) {
        return nullptr;
    }
    QString persistentClientId = m_canvasSessionIdToClientId.value(canvasSessionId);
    if (persistentClientId.isEmpty()) {
        return nullptr;
    }
    return findSession(persistentClientId);
}

// Session lookup by server-assigned session ID (for incoming messages) - PHASE 1 OPTIMIZED: O(1) with secondary index
SessionManager::CanvasSession* SessionManager::findSessionByServerClientId(const QString& serverClientId) {
    if (serverClientId.isEmpty()) {
        return nullptr;
    }
    QString persistentClientId = m_serverIdToClientId.value(serverClientId);
    if (persistentClientId.isEmpty()) {
        return nullptr;
    }
    return findSession(persistentClientId);
}

const SessionManager::CanvasSession* SessionManager::findSessionByServerClientId(const QString& serverClientId) const {
    if (serverClientId.isEmpty()) {
        return nullptr;
    }
    QString persistentClientId = m_serverIdToClientId.value(serverClientId);
    if (persistentClientId.isEmpty()) {
        return nullptr;
    }
    return findSession(persistentClientId);
}

// Generate directional session ID
QString SessionManager::generateDirectionalSessionId(const QString& sourceClientId, const QString& targetClientId) const {
    // Format: "sourceClient_TO_targetClient_canvas_uuid"
    // This ensures A→B and B→A have completely different session IDs
    QString uuid = QUuid::createUuid().toString(QUuid::WithoutBraces);
    return QString("%1_TO_%2_canvas_%3").arg(sourceClientId, targetClientId, uuid);
}

// Get or create session
SessionManager::CanvasSession& SessionManager::getOrCreateSession(const QString& persistentClientId, const ClientInfo& clientInfo) {
    if (persistentClientId.isEmpty()) {
        qWarning() << "SessionManager::getOrCreateSession: persistentClientId is empty";
        static CanvasSession dummy;
        return dummy;
    }

    auto it = m_sessions.find(persistentClientId);
    if (it != m_sessions.end()) {
        // Update existing session info
        it.value().lastClientInfo = clientInfo;
        emit sessionModified(persistentClientId);
        return it.value();
    }

    // Create new session with DIRECTIONAL session ID
    CanvasSession newSession;
    newSession.persistentClientId = persistentClientId;
    newSession.lastClientInfo = clientInfo;
    newSession.serverAssignedId = clientInfo.getId();
    newSession.remoteContentClearedOnDisconnect = false;
    newSession.connectionsInitialized = false;
    
    // CRITICAL FIX: Use directional session ID
    // This creates separate sessions for A→B and B→A
    if (m_myClientId.isEmpty()) {
        qWarning() << "SessionManager: myClientId not set! Using default session ID";
        newSession.canvasSessionId = DEFAULT_IDEA_ID;
    } else {
        newSession.canvasSessionId = generateDirectionalSessionId(m_myClientId, persistentClientId);
    }

    m_sessions.insert(persistentClientId, newSession);
    
    // PHASE 1: Update secondary indexes
    m_canvasSessionIdToClientId[newSession.canvasSessionId] = persistentClientId;
    m_serverIdToClientId[newSession.serverAssignedId] = persistentClientId;
    
    qDebug() << "SessionManager: Created new session for client" << persistentClientId 
             << "with directional canvasSessionId" << newSession.canvasSessionId;
    
    emit sessionCreated(persistentClientId);
    return m_sessions[persistentClientId];
}

// Session management
bool SessionManager::hasSession(const QString& persistentClientId) const {
    return m_sessions.contains(persistentClientId);
}

void SessionManager::deleteSession(const QString& persistentClientId) {
    if (persistentClientId.isEmpty()) {
        return;
    }

    auto it = m_sessions.find(persistentClientId);
    if (it == m_sessions.end()) {
        return;
    }

    qDebug() << "SessionManager: Deleting session for client" << persistentClientId;
    
    // PHASE 1: Remove from secondary indexes
    removeFromIndexes(persistentClientId);
    
    // Note: Canvas and uploadButton cleanup is handled by MainWindow
    // (ownership may be with layouts/parent widgets)
    
    m_sessions.erase(it);
    emit sessionDeleted(persistentClientId);
}

void SessionManager::clearAllSessions() {
    qDebug() << "SessionManager: Clearing all sessions";
    m_sessions.clear();
    
    // PHASE 1: Clear secondary indexes
    m_canvasSessionIdToClientId.clear();
    m_serverIdToClientId.clear();
}

// Session enumeration
QList<QString> SessionManager::getAllPersistentClientIds() const {
    return m_sessions.keys();
}

QList<SessionManager::CanvasSession*> SessionManager::getAllSessions() {
    QList<CanvasSession*> result;
    for (auto it = m_sessions.begin(); it != m_sessions.end(); ++it) {
        result.append(&it.value());
    }
    return result;
}

QList<const SessionManager::CanvasSession*> SessionManager::getAllSessions() const {
    QList<const CanvasSession*> result;
    for (auto it = m_sessions.constBegin(); it != m_sessions.constEnd(); ++it) {
        result.append(&it.value());
    }
    return result;
}

// Bulk operations
void SessionManager::markAllSessionsOffline() {
    qDebug() << "SessionManager: Marking all sessions as offline";
    for (auto it = m_sessions.begin(); it != m_sessions.end(); ++it) {
        it.value().lastClientInfo.setOnline(false);
        emit sessionModified(it.key());
    }
}

void SessionManager::clearRemoteContentForOfflineSessions() {
    qDebug() << "SessionManager: Clearing remote content for offline sessions";
    for (auto it = m_sessions.begin(); it != m_sessions.end(); ++it) {
        if (!it.value().lastClientInfo.isOnline()) {
            it.value().knownRemoteFileIds.clear();
            it.value().remoteContentClearedOnDisconnect = true;
            emit sessionModified(it.key());
        }
    }
}

// PHASE 1: Public methods to update IDs and maintain indexes
void SessionManager::updateSessionIdeaId(const QString& persistentClientId, const QString& newIdeaId) {
    auto* session = findSession(persistentClientId);
    if (!session) {
        qWarning() << "SessionManager::updateSessionIdeaId: Session not found for" << persistentClientId;
        return;
    }
    
    QString oldIdeaId = session->canvasSessionId;
    if (oldIdeaId == newIdeaId) {
        return; // No change
    }
    
    session->canvasSessionId = newIdeaId;
    updateIdeaIdIndex(persistentClientId, oldIdeaId, newIdeaId);
    emit sessionModified(persistentClientId);
}

void SessionManager::updateSessionServerId(const QString& persistentClientId, const QString& newServerId) {
    auto* session = findSession(persistentClientId);
    if (!session) {
        qWarning() << "SessionManager::updateSessionServerId: Session not found for" << persistentClientId;
        return;
    }
    
    QString oldServerId = session->serverAssignedId;
    if (oldServerId == newServerId) {
        return; // No change
    }
    
    session->serverAssignedId = newServerId;
    updateServerIdIndex(persistentClientId, oldServerId, newServerId);
    emit sessionModified(persistentClientId);
}

// PHASE 1: Index maintenance helpers
void SessionManager::updateIdeaIdIndex(const QString& persistentClientId, const QString& oldIdeaId, const QString& newIdeaId) {
    if (!oldIdeaId.isEmpty()) {
        m_canvasSessionIdToClientId.remove(oldIdeaId);
    }
    if (!newIdeaId.isEmpty()) {
        m_canvasSessionIdToClientId[newIdeaId] = persistentClientId;
    }
    qDebug() << "SessionManager: Updated canvasSessionId index:" << persistentClientId << "from" << oldIdeaId << "to" << newIdeaId;
}

void SessionManager::updateServerIdIndex(const QString& persistentClientId, const QString& oldServerId, const QString& newServerId) {
    if (!oldServerId.isEmpty()) {
        m_serverIdToClientId.remove(oldServerId);
    }
    if (!newServerId.isEmpty()) {
        m_serverIdToClientId[newServerId] = persistentClientId;
    }
    qDebug() << "SessionManager: Updated serverSessionId index:" << persistentClientId << "from" << oldServerId << "to" << newServerId;
}

void SessionManager::removeFromIndexes(const QString& persistentClientId) {
    auto it = m_sessions.find(persistentClientId);
    if (it != m_sessions.end()) {
        const auto& session = it.value();
        m_canvasSessionIdToClientId.remove(session.canvasSessionId);
        m_serverIdToClientId.remove(session.serverAssignedId);
        qDebug() << "SessionManager: Removed from indexes:" << persistentClientId;
    }
}
