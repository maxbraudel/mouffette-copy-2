#ifndef REMOTEFILETRACKER_H
#define REMOTEFILETRACKER_H

#include <QString>
#include <QHash>
#include <QSet>
#include <QList>
#include <functional>

/**
 * Phase 4.2: RemoteFileTracker
 * 
 * Tracks remote file distribution and canvasSessionId associations.
 * Extracted from FileManager to separate concerns.
 * 
 * Responsibilities:
 * - Track which files are uploaded to which remote clients
 * - Track which files belong to which canvasSessionIds
 * - Notify when files are no longer in use (for cleanup)
 * - Manage file removal notifications to remote clients
 */
class RemoteFileTracker {
public:
    static RemoteFileTracker& instance();
    
    // Remote client tracking
    void markFileUploadedToClient(const QString& fileId, const QString& clientId);
    void unmarkFileUploadedToClient(const QString& fileId, const QString& clientId);
    void unmarkAllFilesForClient(const QString& clientId);
    QList<QString> getClientsWithFile(const QString& fileId) const;
    bool isFileUploadedToClient(const QString& fileId, const QString& clientId) const;
    bool isFileUploadedToAnyClient(const QString& fileId) const;
    
    // IdeaId association
    void associateFileWithIdea(const QString& fileId, const QString& canvasSessionId);
    void dissociateFileFromIdea(const QString& fileId, const QString& canvasSessionId);
    QSet<QString> getFileIdsForIdea(const QString& canvasSessionId) const;
    QSet<QString> getIdeaIdsForFile(const QString& fileId) const;
    void replaceIdeaFileSet(const QString& canvasSessionId, const QSet<QString>& fileIds);
    void removeIdeaAssociations(const QString& canvasSessionId);
    
    // Remove all tracking data for a specific file
    void removeAllTrackingForFile(const QString& fileId);
    
    // File removal notification callback
    using FileRemovalNotifier = std::function<void(const QString& fileId, const QList<QString>& clientIds, const QList<QString>& canvasSessionIds)>;
    void setFileRemovalNotifier(FileRemovalNotifier callback);
    
    // Check and notify if file is unused (no clients, no ideas)
    void checkAndNotifyIfUnused(const QString& fileId);
    
    // Clear all tracking data
    void clear();

private:
    RemoteFileTracker() = default;
    ~RemoteFileTracker() = default;
    RemoteFileTracker(const RemoteFileTracker&) = delete;
    RemoteFileTracker& operator=(const RemoteFileTracker&) = delete;
    
    QHash<QString, QSet<QString>> m_fileIdToClients;  // fileId → Set<clientId>
    QHash<QString, QSet<QString>> m_fileIdToIdeaIds;  // fileId → Set<canvasSessionId>
    QHash<QString, QSet<QString>> m_canvasSessionIdToFileIds;  // canvasSessionId → Set<fileId>
    
    FileRemovalNotifier m_fileRemovalNotifier;
};

#endif // REMOTEFILETRACKER_H
