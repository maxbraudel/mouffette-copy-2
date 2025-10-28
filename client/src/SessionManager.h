#ifndef SESSIONMANAGER_H
#define SESSIONMANAGER_H

#include <QObject>
#include <QHash>
#include <QSet>
#include <QString>
#include "ClientInfo.h"

class ScreenCanvas;
class QPushButton;

/**
 * Phase 4.1: SessionManager
 * 
 * Manages canvas sessions lifecycle, including:
 * - Session storage and lookup (by persistentClientId, ideaId, serverSessionId)
 * - Session creation and deletion
 * - Session state tracking (online status, remote content, file tracking)
 * 
 * Extracted from MainWindow to improve testability and separation of concerns.
 * 
 * MIGRATION STATUS (Phase 4.1 - 10%):
 * - [✓] Class created with core data structure
 * - [✓] Lookup functions implemented (findSession, findByIdeaId, findByServerClientId)
 * - [✓] CRUD operations (getOrCreate, delete, clear)
 * - [✓] Bulk operations (markOffline, clearRemoteContent)
 * - [ ] MainWindow integration (m_canvasSessions migration pending)
 * - [ ] UI extension handling (uploadButton, upload tracking)
 * 
 * NEXT STEPS:
 * 1. Create MainWindowSession : public SessionManager::CanvasSession { UI fields }
 * 2. Migrate m_canvasSessions Map<QString, MainWindowSession> 
 * 3. Replace findCanvas* functions to use SessionManager
 * 4. Move bulk operations (onDisconnected) to SessionManager
 * 
 * NOTE: SessionManager is currently initialized but not yet actively used.
 *       Full migration will occur after FileManager split (Phase 4.2).
 */
class SessionManager : public QObject {
    Q_OBJECT

public:
    struct CanvasSession {
        QString persistentClientId; // stable client ID persisted across sessions
        QString serverAssignedId;   // temporary server session ID (for local lookup only, send persistentClientId to server)
        QString ideaId;
        ScreenCanvas* canvas = nullptr;
        QPushButton* uploadButton = nullptr;
        ClientInfo lastClientInfo;
        QSet<QString> knownRemoteFileIds;        // Files we've successfully sent to this target
        QSet<QString> expectedIdeaFileIds;       // Files that should exist for this ideaId
        bool remoteContentClearedOnDisconnect = false;
        bool connectionsInitialized = false;
    };

    explicit SessionManager(QObject *parent = nullptr);
    ~SessionManager();

    // Session lookup
    CanvasSession* findSession(const QString& persistentClientId);
    const CanvasSession* findSession(const QString& persistentClientId) const;
    
    CanvasSession* findSessionByIdeaId(const QString& ideaId);
    const CanvasSession* findSessionByIdeaId(const QString& ideaId) const;
    
    CanvasSession* findSessionByServerClientId(const QString& serverClientId);
    const CanvasSession* findSessionByServerClientId(const QString& serverClientId) const;

    // Session creation/retrieval
    CanvasSession& getOrCreateSession(const QString& persistentClientId, const ClientInfo& clientInfo);
    
    // Session management
    bool hasSession(const QString& persistentClientId) const;
    void deleteSession(const QString& persistentClientId);
    void clearAllSessions();
    
    // Session enumeration
    QList<QString> getAllPersistentClientIds() const;
    QList<CanvasSession*> getAllSessions();
    QList<const CanvasSession*> getAllSessions() const;
    int sessionCount() const { return m_sessions.size(); }

    // Bulk operations
    void markAllSessionsOffline();
    void clearRemoteContentForOfflineSessions();

signals:
    void sessionCreated(const QString& persistentClientId);
    void sessionDeleted(const QString& persistentClientId);
    void sessionModified(const QString& persistentClientId);

private:
    QHash<QString, CanvasSession> m_sessions; // persistentClientId → CanvasSession
};

#endif // SESSIONMANAGER_H
