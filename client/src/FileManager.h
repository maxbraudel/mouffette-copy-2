#ifndef FILEMANAGER_H
#define FILEMANAGER_H

#include <QString>
#include <QHash>
#include <QList>
#include <QFileInfo>
#include <QCryptographicHash>

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
    // Clear all uploaded markers for a given client across all files and media
    void unmarkAllFilesForClient(const QString& clientId);
    void unmarkAllMediaForClient(const QString& clientId);
    void unmarkAllForClient(const QString& clientId);
    
    // Set callback for when file should be deleted from remote clients
    static void setFileRemovalNotifier(std::function<void(const QString& fileId, const QList<QString>& clientIds)> cb);

private:
    FileManager() = default;
    
    // Generate unique file ID based on file path and content hash
    QString generateFileId(const QString& filePath);
    
    struct FileMeta {
        qint64 size = 0;
        qint64 mtimeSecs = 0;
    };
    
    QHash<QString, QString> m_fileIdToPath;        // fileId -> filePath
    QHash<QString, QString> m_pathToFileId;        // filePath -> fileId
    QHash<QString, QList<QString>> m_fileIdToMediaIds; // fileId -> [mediaId1, mediaId2, ...]
    QHash<QString, QString> m_mediaIdToFileId;     // mediaId -> fileId
    QHash<QString, QList<QString>> m_fileIdToClients; // fileId -> [clientId1, clientId2, ...]
    QHash<QString, QList<QString>> m_mediaIdToClients; // mediaId -> [clientId1, clientId2, ...]
    QHash<QString, FileMeta> m_fileIdMeta;         // fileId -> size/mtime captured at id creation
    
    static std::function<void(const QString& fileId, const QList<QString>& clientIds)> s_fileRemovalNotifier;
};

#endif // FILEMANAGER_H