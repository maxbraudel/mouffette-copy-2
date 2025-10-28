#ifndef LOCALFILEREPOSITORY_H
#define LOCALFILEREPOSITORY_H

#include <QString>
#include <QHash>
#include <QFileInfo>
#include <QCryptographicHash>

/**
 * Phase 4.2: LocalFileRepository
 * 
 * Manages local file system operations and fileId ↔ filePath mappings.
 * Extracted from FileManager to separate concerns.
 * 
 * Responsibilities:
 * - Generate stable fileIds from file paths (SHA-256 hash)
 * - Maintain bidirectional fileId ↔ filePath mapping
 * - Check file existence and provide file info
 * - Register received remote files (target-side)
 */
class LocalFileRepository {
public:
    static LocalFileRepository& instance();
    
    // Get or create fileId for a given file path
    QString getOrCreateFileId(const QString& filePath);
    
    // Get file path for a fileId
    QString getFilePathForId(const QString& fileId) const;
    
    // Check if fileId exists in repository
    bool hasFileId(const QString& fileId) const;
    
    // Get all registered fileIds
    QList<QString> getAllFileIds() const;
    
    // Register a received file path for a fileId (target-side)
    void registerReceivedFilePath(const QString& fileId, const QString& absolutePath);
    
    // Remove a received file mapping (when remote sender deletes)
    void removeReceivedFileMapping(const QString& fileId);
    
    // Clear all mappings (for cleanup/reset)
    void clear();

private:
    LocalFileRepository() = default;
    ~LocalFileRepository() = default;
    LocalFileRepository(const LocalFileRepository&) = delete;
    LocalFileRepository& operator=(const LocalFileRepository&) = delete;
    
    // Generate stable fileId from file path
    QString generateFileId(const QString& filePath) const;
    
    QHash<QString, QString> m_fileIdToPath;  // fileId → absolute file path
    QHash<QString, QString> m_pathToFileId;  // absolute file path → fileId
};

#endif // LOCALFILEREPOSITORY_H
