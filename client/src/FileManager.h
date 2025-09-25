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
    
    // Check if a file ID exists
    bool hasFileId(const QString& fileId) const;
    
    // Remove file completely (when no more media references it)
    void removeFileIfUnused(const QString& fileId);
    
    // Track which clients have received which files
    void markFileUploadedToClient(const QString& fileId, const QString& clientId);
    QList<QString> getClientsWithFile(const QString& fileId) const;
    
    // Set callback for when file should be deleted from remote clients
    static void setFileRemovalNotifier(std::function<void(const QString& fileId, const QList<QString>& clientIds)> cb);

private:
    FileManager() = default;
    
    // Generate unique file ID based on file path and content hash
    QString generateFileId(const QString& filePath);
    
    QHash<QString, QString> m_fileIdToPath;        // fileId -> filePath
    QHash<QString, QString> m_pathToFileId;        // filePath -> fileId
    QHash<QString, QList<QString>> m_fileIdToMediaIds; // fileId -> [mediaId1, mediaId2, ...]
    QHash<QString, QString> m_mediaIdToFileId;     // mediaId -> fileId
    QHash<QString, QList<QString>> m_fileIdToClients; // fileId -> [clientId1, clientId2, ...]
    
    static std::function<void(const QString& fileId, const QList<QString>& clientIds)> s_fileRemovalNotifier;
};

#endif // FILEMANAGER_H