#ifndef SESSIONMANAGER_H
#define SESSIONMANAGER_H

#include <QObject>
#include <QHash>
#include <QSet>
#include <QString>
#include <QFont>
#include <QList>
#include "domain/models/ClientInfo.h"

class ScreenCanvas;
class QPushButton;
class ResizableMediaBase;

// Phase 3: canvasSessionId is MANDATORY - use default value instead of empty string
inline const QString DEFAULT_IDEA_ID = QStringLiteral("default");

/**
 * Phase 4.1: SessionManager
 * 
 * Manages canvas sessions lifecycle, including:
 * - Session storage and lookup (by persistentClientId, canvasSessionId, serverSessionId)
 * - Session creation and deletion
 * - Session state tracking (online status, remote content, file tracking)
 * 
 * Extracted from MainWindow to improve testability and separation of concerns.
 * 
 * MIGRATION STATUS (Phase 4.1 - ✅ COMPLETE):
 * - [✅] Class created with core data structure
 * - [✅] Lookup functions implemented (findSession, findByIdeaId, findByServerClientId)
 * - [✅] CRUD operations (getOrCreate, delete, clear)
 * - [✅] Bulk operations (markOffline, clearRemoteContent)
 * - [✅] MainWindow integration (m_canvasSessions fully migrated)
 * - [✅] All iteration loops migrated to getAllSessions()
 * - [✅] IdeaId generation integrated into SessionManager
 * 
 * RESULTS:
 * - Removed QHash<QString, CanvasSession> m_canvasSessions from MainWindow
 * - All session access now goes through SessionManager
 * - ~150 lines of duplicate session management code eliminated
 * - Single source of truth for session data
 * 
 * NEXT PHASE: Phase 4.2 - FileManager split into 3 services
 */
class SessionManager : public QObject {
    Q_OBJECT

public:
    struct CanvasSession {
        QString persistentClientId; // stable client ID persisted across sessions
        QString serverAssignedId;   // temporary server session ID (for local lookup only, send persistentClientId to server)
        QString canvasSessionId;
        ScreenCanvas* canvas = nullptr;
        QPushButton* uploadButton = nullptr;
        bool uploadButtonInOverlay = false;
        QFont uploadButtonDefaultFont;
        ClientInfo lastClientInfo;
        bool connectionsInitialized = false;
        bool remoteContentClearedOnDisconnect = false;
        QSet<QString> expectedIdeaFileIds; // latest scene files present on canvas
        QSet<QString> knownRemoteFileIds;   // files we believe reside on the remote for current idea
        struct UploadTracking {
            // Removed mediaIdsBeingUploaded - redundant with fileIds tracking
            // Removed mediaIdByFileId - can be derived from FileManager
            QHash<QString, QList<ResizableMediaBase*>> itemsByFileId;
            QStringList currentUploadFileOrder;
            QSet<QString> serverCompletedFileIds;
            QHash<QString, int> perFileProgress;
            bool receivingFilesToastShown = false;
            QString activeUploadId;
            bool remoteFilesPresent = false;
        } upload;
    };

    explicit SessionManager(QObject *parent = nullptr);
    ~SessionManager();

    // Session lookup
    CanvasSession* findSession(const QString& persistentClientId);
    const CanvasSession* findSession(const QString& persistentClientId) const;
    
    CanvasSession* findSessionByIdeaId(const QString& canvasSessionId);
    const CanvasSession* findSessionByIdeaId(const QString& canvasSessionId) const;
    
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
    
    // PHASE 1: Index maintenance (call when updating canvasSessionId or serverAssignedId)
    void updateSessionIdeaId(const QString& persistentClientId, const QString& newIdeaId);
    void updateSessionServerId(const QString& persistentClientId, const QString& newServerId);

signals:
    void sessionCreated(const QString& persistentClientId);
    void sessionDeleted(const QString& persistentClientId);
    void sessionModified(const QString& persistentClientId);

private:
    // PHASE 1 OPTIMIZATION: Secondary indexes for O(1) lookups
    QHash<QString, CanvasSession> m_sessions; // persistentClientId → CanvasSession (primary storage)
    QHash<QString, QString> m_canvasSessionIdToClientId;       // canvasSessionId → persistentClientId (secondary index)
    QHash<QString, QString> m_serverIdToClientId;     // serverSessionId → persistentClientId (secondary index)
    
    // Index maintenance helpers
    void updateIdeaIdIndex(const QString& persistentClientId, const QString& oldIdeaId, const QString& newIdeaId);
    void updateServerIdIndex(const QString& persistentClientId, const QString& oldServerId, const QString& newServerId);
    void removeFromIndexes(const QString& persistentClientId);
};

#endif // SESSIONMANAGER_H
