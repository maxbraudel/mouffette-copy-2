#ifndef FILEMANAGER_H
#define FILEMANAGER_H

#include <QString>
#include <QHash>
#include <QList>
#include <QFileInfo>
#include <QCryptographicHash>
#include <QSet>
#include <QSharedPointer>
#include <QByteArray>

// Phase 4.2: Forward declarations of specialized services
class LocalFileRepository;
class RemoteFileTracker;
class FileMemoryCache;

/**
 * FileManager - Façade orchestrating file operations
 * 
 * Phase 4.2 Refactor: Delegates to 3 specialized services:
 * - LocalFileRepository: fileId ↔ filePath mapping
 * - RemoteFileTracker: remote client & ideaId tracking
 * - FileMemoryCache: memory caching for performance
 * 
 * Maintains backward-compatible API while internally using services.
 */
class FileManager
{
public:
    static FileManager& instance();
    
    // Get or create file ID for a given file path
    QString getOrCreateFileId(const QString& filePath);
    
    // Associate a media ID with a file ID
    void associateMediaWithFile(const QString& mediaId, const QString& fileId);
    
    // Remove media association (when media is deleted)
    void removeMediaAssociation(const QString& mediaId);
    
    // Get file ID for a media ID
    QString getFileIdForMedia(const QString& mediaId) const;
    
    // Get all media IDs for a file ID
    QList<QString> getMediaIdsForFile(const QString& fileId) const;
    
    // Get file path for a file ID
    QString getFilePathForId(const QString& fileId) const;
    
    // Get all unique file IDs (for upload)
    QList<QString> getAllFileIds() const;

    // Register a file path for a fileId when receiving from remote (target side). If already exists, leave unchanged.
    void registerReceivedFilePath(const QString& fileId, const QString& absolutePath);
    // Remove a previously registered received file mapping on the target side (called when sender asks to delete a file)
    void removeReceivedFileMapping(const QString& fileId);

    // Ensure file bytes are resident in memory for low-latency playback.
    void preloadFileIntoMemory(const QString& fileId);
    // Retrieve a shared QByteArray for a file. Loads from disk on first access unless already cached.
    QSharedPointer<QByteArray> getFileBytes(const QString& fileId, bool forceReload = false);
    // Release any resident memory for the given fileId (used when file is deleted remotely).
    void releaseFileMemory(const QString& fileId);
    
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

    // Track which clients have received which media instances (per-item, independent of file dedupe)
    void markMediaUploadedToClient(const QString& mediaId, const QString& clientId);
    bool isMediaUploadedToClient(const QString& mediaId, const QString& clientId) const;
    void unmarkMediaUploadedToClient(const QString& mediaId, const QString& clientId);
    // Associate/de-associate files with logical idea IDs (scenes/projects)
    void associateFileWithIdea(const QString& fileId, const QString& ideaId);
    void dissociateFileFromIdea(const QString& fileId, const QString& ideaId);
    QSet<QString> getIdeaIdsForFile(const QString& fileId) const;
    QSet<QString> getFileIdsForIdea(const QString& ideaId) const;
    void replaceIdeaFileSet(const QString& ideaId, const QSet<QString>& fileIds);
    void removeIdeaAssociations(const QString& ideaId);

    // Clear all uploaded markers for a given client across all files and media
    void unmarkAllFilesForClient(const QString& clientId);
    void unmarkAllMediaForClient(const QString& clientId);
    void unmarkAllForClient(const QString& clientId);

    // Remove any received-file bookkeeping for paths under the given prefix (used when cleaning cache folders)
    void removeReceivedFileMappingsUnderPathPrefix(const QString& pathPrefix);
    
    // Set callback for when file should be deleted from remote clients
    static void setFileRemovalNotifier(std::function<void(const QString& fileId, const QList<QString>& clientIds, const QList<QString>& ideaIds)> cb);

private:
    FileManager();
    
    // Phase 4.2: Service references (initialized in constructor)
    LocalFileRepository* m_repository;
    RemoteFileTracker* m_tracker;
    FileMemoryCache* m_cache;
    
    // Media associations (not moved to services - app-specific logic)
    QHash<QString, QList<QString>> m_fileIdToMediaIds; // fileId -> [mediaId1, mediaId2, ...]
    QHash<QString, QString> m_mediaIdToFileId;     // mediaId -> fileId
    
    static std::function<void(const QString& fileId, const QList<QString>& clientIds, const QList<QString>& ideaIds)> s_fileRemovalNotifier;
};

#endif // FILEMANAGER_H