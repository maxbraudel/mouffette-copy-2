#ifndef REMOTEFILETRACKER_H
#define REMOTEFILETRACKER_H

#include <QString>
#include <QHash>
#include <QSet>
#include <QList>
#include <functional>

/**
 * Tracks project-scoped remote file availability.
 * 
 * Tracks remote file distribution and projectId associations.
 * Extracted from FileManager to separate concerns.
 * 
 * Responsibilities:
 * - Track which files are uploaded to which remote clients
 * - Track which files belong to which projectIds
 * - Notify when files are no longer referenced (for cleanup)
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
    
    // ProjectId association
    void associateFileWithProject(const QString& fileId, const QString& projectId);
    void dissociateFileFromProject(const QString& fileId, const QString& projectId);
    QSet<QString> getFileIdsForProject(const QString& projectId) const;
    QSet<QString> getProjectIdsForFile(const QString& fileId) const;
    void replaceProjectFileSet(const QString& projectId, const QSet<QString>& fileIds);
    void removeProjectAssociations(const QString& projectId);
    
    // Remove all tracking data for a specific file
    void removeAllTrackingForFile(const QString& fileId);
    
    // File removal notification callback
    using FileRemovalNotifier = std::function<void(const QString& fileId, const QList<QString>& clientIds, const QList<QString>& projectIds)>;
    void setFileRemovalNotifier(FileRemovalNotifier callback);
    
    // Check and notify if file is unused (no clients, no projects)
    void checkAndNotifyIfUnused(const QString& fileId);
    
    // Clear all tracking data
    void clear();

private:
    RemoteFileTracker() = default;
    ~RemoteFileTracker() = default;
    RemoteFileTracker(const RemoteFileTracker&) = delete;
    RemoteFileTracker& operator=(const RemoteFileTracker&) = delete;
    
    QHash<QString, QSet<QString>> m_fileIdToClients;  // fileId → Set<clientId>
    QHash<QString, QSet<QString>> m_fileIdToProjectIds;  // fileId → Set<projectId>
    QHash<QString, QSet<QString>> m_projectIdToFileIds;  // projectId → Set<fileId>
    
    FileRemovalNotifier m_fileRemovalNotifier;
};

#endif // REMOTEFILETRACKER_H
