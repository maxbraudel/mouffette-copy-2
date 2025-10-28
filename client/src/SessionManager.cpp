#include "SessionManager.h"
#include "ScreenCanvas.h"
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

// Session lookup by ideaId
SessionManager::CanvasSession* SessionManager::findSessionByIdeaId(const QString& ideaId) {
    if (ideaId.isEmpty()) {
        return nullptr;
    }
    for (auto it = m_sessions.begin(); it != m_sessions.end(); ++it) {
        if (it.value().ideaId == ideaId) {
            return &it.value();
        }
    }
    return nullptr;
}

const SessionManager::CanvasSession* SessionManager::findSessionByIdeaId(const QString& ideaId) const {
    if (ideaId.isEmpty()) {
        return nullptr;
    }
    for (auto it = m_sessions.constBegin(); it != m_sessions.constEnd(); ++it) {
        if (it.value().ideaId == ideaId) {
            return &it.value();
        }
    }
    return nullptr;
}

// Session lookup by server-assigned session ID (for incoming messages)
SessionManager::CanvasSession* SessionManager::findSessionByServerClientId(const QString& serverClientId) {
    if (serverClientId.isEmpty()) {
        return nullptr;
    }
    for (auto it = m_sessions.begin(); it != m_sessions.end(); ++it) {
        if (it.value().serverAssignedId == serverClientId) {
            return &it.value();
        }
    }
    return nullptr;
}

const SessionManager::CanvasSession* SessionManager::findSessionByServerClientId(const QString& serverClientId) const {
    if (serverClientId.isEmpty()) {
        return nullptr;
    }
    for (auto it = m_sessions.constBegin(); it != m_sessions.constEnd(); ++it) {
        if (it.value().serverAssignedId == serverClientId) {
            return &it.value();
        }
    }
    return nullptr;
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

    // Create new session
    CanvasSession newSession;
    newSession.persistentClientId = persistentClientId;
    newSession.lastClientInfo = clientInfo;
    newSession.serverAssignedId = clientInfo.getId();
    newSession.remoteContentClearedOnDisconnect = false;
    newSession.connectionsInitialized = false;
    newSession.ideaId = QUuid::createUuid().toString(QUuid::WithoutBraces); // Generate ideaId

    m_sessions.insert(persistentClientId, newSession);
    qDebug() << "SessionManager: Created new session for client" << persistentClientId << "with ideaId" << newSession.ideaId;
    
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
    
    // Note: Canvas and uploadButton cleanup is handled by MainWindow
    // (ownership may be with layouts/parent widgets)
    
    m_sessions.erase(it);
    emit sessionDeleted(persistentClientId);
}

void SessionManager::clearAllSessions() {
    qDebug() << "SessionManager: Clearing all sessions";
    m_sessions.clear();
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
