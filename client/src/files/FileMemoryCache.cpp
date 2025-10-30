#include "files/FileMemoryCache.h"
#include <QFile>
#include <QDebug>

FileMemoryCache& FileMemoryCache::instance() {
    static FileMemoryCache instance;
    return instance;
}

QSharedPointer<QByteArray> FileMemoryCache::loadFileFromDisk(const QString& filePath) {
    if (filePath.isEmpty()) {
        qWarning() << "FileMemoryCache::loadFileFromDisk: empty filePath";
        return QSharedPointer<QByteArray>::create();
    }
    
    QFile file(filePath);
    if (!file.open(QIODevice::ReadOnly)) {
        qWarning() << "FileMemoryCache::loadFileFromDisk: Failed to open file" << filePath << file.errorString();
        return QSharedPointer<QByteArray>::create();
    }
    
    QByteArray data = file.readAll();
    file.close();
    
    qDebug() << "FileMemoryCache: Loaded" << data.size() << "bytes from" << filePath;
    return QSharedPointer<QByteArray>::create(data);
}

void FileMemoryCache::preloadFileIntoMemory(const QString& fileId, const QString& filePath) {
    if (fileId.isEmpty() || filePath.isEmpty()) {
        return;
    }
    
    if (m_cachedFiles.contains(fileId)) {
        qDebug() << "FileMemoryCache: File" << fileId << "already cached";
        return;
    }
    
    QSharedPointer<QByteArray> data = loadFileFromDisk(filePath);
    if (data && !data->isEmpty()) {
        m_cachedFiles.insert(fileId, data);
        qDebug() << "FileMemoryCache: Preloaded file" << fileId << "into memory (" << data->size() << "bytes)";
    }
}

QSharedPointer<QByteArray> FileMemoryCache::getFileBytes(const QString& fileId, const QString& filePath, bool forceReload) {
    if (fileId.isEmpty()) {
        qWarning() << "FileMemoryCache::getFileBytes: empty fileId";
        return QSharedPointer<QByteArray>::create();
    }
    
    // Check cache first (unless force reload)
    if (!forceReload && m_cachedFiles.contains(fileId)) {
        qDebug() << "FileMemoryCache: Cache hit for file" << fileId;
        return m_cachedFiles.value(fileId);
    }
    
    // Load from disk
    if (filePath.isEmpty()) {
        qWarning() << "FileMemoryCache::getFileBytes: filePath required for fileId" << fileId;
        return QSharedPointer<QByteArray>::create();
    }
    
    QSharedPointer<QByteArray> data = loadFileFromDisk(filePath);
    if (data && !data->isEmpty()) {
        m_cachedFiles.insert(fileId, data);
        qDebug() << "FileMemoryCache: Cached file" << fileId << "on demand (" << data->size() << "bytes)";
    }
    
    return data;
}

void FileMemoryCache::releaseFileMemory(const QString& fileId) {
    if (m_cachedFiles.remove(fileId)) {
        qDebug() << "FileMemoryCache: Released memory for file" << fileId;
    }
}

bool FileMemoryCache::isFileCached(const QString& fileId) const {
    return m_cachedFiles.contains(fileId);
}

qint64 FileMemoryCache::getTotalCachedBytes() const {
    qint64 total = 0;
    for (const auto& data : m_cachedFiles) {
        if (data) {
            total += data->size();
        }
    }
    return total;
}

void FileMemoryCache::clearCache() {
    qDebug() << "FileMemoryCache: Clearing cache (" << m_cachedFiles.size() << "files)";
    m_cachedFiles.clear();
}
