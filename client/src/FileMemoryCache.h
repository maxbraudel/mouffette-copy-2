#ifndef FILEMORYCACHE_H
#define FILEMORYCACHE_H

#include <QString>
#include <QHash>
#include <QSharedPointer>
#include <QByteArray>

/**
 * Phase 4.2: FileMemoryCache
 * 
 * Manages in-memory caching of file contents for fast access.
 * Extracted from FileManager to separate concerns.
 * 
 * Responsibilities:
 * - Preload file contents into memory for low-latency playback
 * - Provide shared access to cached file bytes
 * - Manage memory lifecycle (load, cache, release)
 * - Avoid redundant disk reads
 */
class FileMemoryCache {
public:
    static FileMemoryCache& instance();
    
    // Preload file into memory (async-friendly)
    void preloadFileIntoMemory(const QString& fileId, const QString& filePath);
    
    // Get cached file bytes (loads from disk if not cached)
    QSharedPointer<QByteArray> getFileBytes(const QString& fileId, const QString& filePath, bool forceReload = false);
    
    // Release cached bytes for a file (free memory)
    void releaseFileMemory(const QString& fileId);
    
    // Check if file is cached in memory
    bool isFileCached(const QString& fileId) const;
    
    // Get cache statistics
    int getCachedFileCount() const { return m_cachedFiles.size(); }
    qint64 getTotalCachedBytes() const;
    
    // Clear entire cache
    void clearCache();

private:
    FileMemoryCache() = default;
    ~FileMemoryCache() = default;
    FileMemoryCache(const FileMemoryCache&) = delete;
    FileMemoryCache& operator=(const FileMemoryCache&) = delete;
    
    // Load file from disk
    QSharedPointer<QByteArray> loadFileFromDisk(const QString& filePath);
    
    QHash<QString, QSharedPointer<QByteArray>> m_cachedFiles;  // fileId → cached bytes
};

#endif // FILEMORYCACHE_H
