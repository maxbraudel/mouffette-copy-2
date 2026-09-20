#ifndef FILEMANAGER_H
#define FILEMANAGER_H

#include <QString>
#include <QStringList>
#include <QHash>
#include <QList>
#include <QFileInfo>
#include <QCryptographicHash>
#include <QSet>
#include <QSharedPointer>
#include <QByteArray>

#include "backend/network/RemoteCacheStore.h"

class LocalFileRepository;
class RemoteFileTracker;

/**
 * FileManager - Façade orchestrating file operations
 * 
 * Delegates storage concerns to three specialized services:
 * - LocalFileRepository: fileId ↔ filePath mapping
 * - RemoteFileTracker: remote client & projectId tracking
 * 
 */
class FileManager
{
public:
    explicit FileManager();
    ~FileManager() = default;
    
    // Get or create file ID for a given file path
    QString getOrCreateFileId(const QString& filePath);
    void registerVerifiedLocalFile(const QString& fileId, const QString& filePath);
    
    // Associate a media ID with a file ID
    void associateMediaWithFile(const QString& mediaId, const QString& fileId);
    
    // Defer last-reference cleanup until a complete document edit is committed.
    void beginMediaAssociationTransaction();
    void endMediaAssociationTransaction();

    // Remove media association (when media is deleted)
    void removeMediaAssociation(const QString& mediaId);
    
    // Get file ID for a media ID
    QString getFileIdForMedia(const QString& mediaId) const;
    
    // Get all media IDs for a file ID
    QList<QString> getMediaIdsForFile(const QString& fileId) const;
    
    // Get file path for a file ID
    QString getFilePathForId(const QString& fileId) const;
    // Recorded locations for cache transactions only; may be absent or invalid.
    QStringList getRecordedFilePathsForId(const QString& fileId) const;
    
    // Get all unique file IDs (for upload)
    QList<QString> getAllFileIds() const;

    // Register a file path for a fileId when receiving from remote (target side). If already exists, leave unchanged.
    void registerReceivedFilePath(const QString& fileId, const QString& absolutePath);
    // Remove a previously registered received file mapping on the target side (called when sender asks to delete a file)
    void removeReceivedFileMapping(const QString& fileId);

    // Protocol-v4 received files live in a RemoteSession scope and must never
    // share the local-source fileId namespace or another session's mapping.
    QString getReceivedFilePath(const RemoteCacheStore::Scope& scope,
                                const QString& fileId) const;
    bool registerReceivedFilePath(const RemoteCacheStore::Scope& scope,
                                  const QString& fileId,
                                  const QString& absolutePath);
    bool removeReceivedFileMapping(const RemoteCacheStore::Scope& scope,
                                   const QString& fileId);
    int removeReceivedFileMappingsForScope(const RemoteCacheStore::Scope& scope);
    bool rebindReceivedFileScope(const RemoteCacheStore::Scope& oldScope,
                                 const RemoteCacheStore::Scope& newScope);
    QList<QString> getReceivedFileIds(const RemoteCacheStore::Scope& scope) const;
    // Check if a file ID exists
    bool hasFileId(const QString& fileId) const;
    
    // Remove file completely (when no more media references it)
    void removeFileIfUnused(const QString& fileId);
    
    // Track which clients have received which files
    void markFileUploadedToClient(const QString& fileId, const QString& clientId);
    QList<QString> getClientsWithFile(const QString& fileId) const;
    // Check if a specific file was already uploaded to a given client
    bool isFileUploadedToClient(const QString& fileId, const QString& clientId) const;
    // Remove the association indicating a file is uploaded to a client
    void unmarkFileUploadedToClient(const QString& fileId, const QString& clientId);


    // Associate/de-associate files with logical project IDs (scenes/projects)
    void associateFileWithProject(const QString& fileId, const QString& projectId);
    void dissociateFileFromProject(const QString& fileId, const QString& projectId);
    QSet<QString> getProjectIdsForFile(const QString& fileId) const;
    QSet<QString> getFileIdsForProject(const QString& projectId) const;
    void replaceProjectFileSet(const QString& projectId, const QSet<QString>& fileIds);
    void removeProjectAssociations(const QString& projectId);

    // Clear all uploaded markers for a given client across all files
    void unmarkAllForClient(const QString& clientId);

    // Remove any received-file bookkeeping for paths under the given prefix (used when cleaning cache folders)
    void removeReceivedFileMappingsUnderPathPrefix(const QString& pathPrefix);
    
    // Set callback for when file should be deleted from remote clients
    static void setFileRemovalNotifier(std::function<void(const QString& fileId, const QList<QString>& clientIds, const QList<QString>& projectIds)> cb);

private:
    struct ReceivedScopeFiles {
        RemoteCacheStore::Scope scope;
        QHash<QString, QString> pathsByFileId;
    };

    static QString receivedScopeKey(const RemoteCacheStore::Scope& scope);


    // Non-owning service references initialized in the constructor.
    LocalFileRepository* m_repository;
    RemoteFileTracker* m_tracker;
    
    int m_mediaAssociationTransactionDepth = 0;
    QSet<QString> m_deferredUnusedFiles;

    // Media associations (not moved to services - app-specific logic)
    QHash<QString, QList<QString>> m_fileIdToMediaIds; // fileId -> [mediaId1, mediaId2, ...]
    QHash<QString, QString> m_mediaIdToFileId;     // mediaId -> fileId
    QHash<QString, ReceivedScopeFiles> m_receivedFilesByScope;
    
};

#endif // FILEMANAGER_H
