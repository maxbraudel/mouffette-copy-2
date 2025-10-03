#include "FileManager.h"
#include <QFile>
#include <QFileInfo>
#include <QCryptographicHash>
#include <QDebug>

// Static member definition
std::function<void(const QString& fileId, const QList<QString>& clientIds)> FileManager::s_fileRemovalNotifier;

FileManager& FileManager::instance()
{
    static FileManager instance;
    return instance;
}

QString FileManager::getOrCreateFileId(const QString& filePath)
{
    // Normalize the path
    QString normalizedPath = QFileInfo(filePath).absoluteFilePath();
    
    // Check if we already have this path and whether the file has changed since the id was created
    if (m_pathToFileId.contains(normalizedPath)) {
        const QString existingId = m_pathToFileId[normalizedPath];
        QFileInfo info(normalizedPath);
        const qint64 curSize = info.exists() ? info.size() : 0;
        const qint64 curMtime = info.exists() ? info.lastModified().toSecsSinceEpoch() : 0;
        const FileMeta meta = m_fileIdMeta.value(existingId);
        if (meta.size == curSize && meta.mtimeSecs == curMtime) {
            return existingId;
        }
        // File content changed at same path: generate a new id and re-point mappings
        QString newId = generateFileId(normalizedPath);
        m_pathToFileId[normalizedPath] = newId;
        m_fileIdToPath.remove(existingId);
        m_fileIdToPath[newId] = normalizedPath;
        m_fileIdToMediaIds[newId] = m_fileIdToMediaIds.take(existingId); // move media associations
        // Update reverse media -> file map
        for (const QString& mediaId : m_fileIdToMediaIds[newId]) {
            m_mediaIdToFileId[mediaId] = newId;
        }
        // Clients association does not carry over: treat as not uploaded anywhere yet
        m_fileIdToClients.remove(existingId);
        // Record new meta
        m_fileIdMeta[newId] = { curSize, curMtime };
        
        return newId;
    }
    
    // Generate new file ID
    QString fileId = generateFileId(normalizedPath);
    
    // Store mappings
    m_pathToFileId[normalizedPath] = fileId;
    m_fileIdToPath[fileId] = normalizedPath;
    m_fileIdToMediaIds[fileId] = QList<QString>(); // Initialize empty list
    // Capture file metadata for change detection
    QFileInfo info(normalizedPath);
    m_fileIdMeta[fileId] = { info.exists() ? info.size() : 0, info.exists() ? info.lastModified().toSecsSinceEpoch() : 0 };
    
    
    return fileId;
}

void FileManager::associateMediaWithFile(const QString& mediaId, const QString& fileId)
{
    if (!m_fileIdToPath.contains(fileId)) {
        qWarning() << "Cannot associate media" << mediaId << "with unknown file ID" << fileId;
        return;
    }
    
    // Remove any existing association for this media
    if (m_mediaIdToFileId.contains(mediaId)) {
        QString oldFileId = m_mediaIdToFileId[mediaId];
        m_fileIdToMediaIds[oldFileId].removeAll(mediaId);
    }
    
    // Add new association
    m_mediaIdToFileId[mediaId] = fileId;
    if (!m_fileIdToMediaIds[fileId].contains(mediaId)) {
        m_fileIdToMediaIds[fileId].append(mediaId);
    }
    
    
}

void FileManager::removeMediaAssociation(const QString& mediaId)
{
    
    
    if (!m_mediaIdToFileId.contains(mediaId)) {
        
        return;
    }
    
    QString fileId = m_mediaIdToFileId[mediaId];
    
    m_fileIdToMediaIds[fileId].removeAll(mediaId);
    m_mediaIdToFileId.remove(mediaId);
    
    
    
    // Clean up file if no more media references it  
    removeFileIfUnused(fileId);
    
    
}

QString FileManager::getFileIdForMedia(const QString& mediaId) const
{
    return m_mediaIdToFileId.value(mediaId);
}

QList<QString> FileManager::getMediaIdsForFile(const QString& fileId) const
{
    return m_fileIdToMediaIds.value(fileId);
}

QString FileManager::getFilePathForId(const QString& fileId) const
{
    return m_fileIdToPath.value(fileId);
}

QList<QString> FileManager::getAllFileIds() const
{
    return m_fileIdToPath.keys();
}

bool FileManager::hasFileId(const QString& fileId) const
{
    return m_fileIdToPath.contains(fileId);
}

void FileManager::removeFileIfUnused(const QString& fileId)
{
    
    
    if (!m_fileIdToMediaIds.contains(fileId)) {
        
        return;
    }
    
    if (m_fileIdToMediaIds[fileId].isEmpty()) {
        QString filePath = m_fileIdToPath.value(fileId);
        QList<QString> clientsWithFile = m_fileIdToClients.value(fileId);
        
        
        
        // Notify that file should be removed from remote clients
        if (!clientsWithFile.isEmpty() && s_fileRemovalNotifier) {
            
            s_fileRemovalNotifier(fileId, clientsWithFile);
        } else {
            
        }
        
        // Clean up local tracking data
        m_fileIdToPath.remove(fileId);
        m_pathToFileId.remove(filePath);
        m_fileIdToMediaIds.remove(fileId);
        m_fileIdToClients.remove(fileId);
        m_fileIdMeta.remove(fileId);
        
        
    } else {
        
    }
}

void FileManager::registerReceivedFilePath(const QString& fileId, const QString& absolutePath) {
    if (fileId.isEmpty() || absolutePath.isEmpty()) return;
    if (m_fileIdToPath.contains(fileId)) return; // already known (sender side)
    m_fileIdToPath.insert(fileId, absolutePath);
    m_pathToFileId.insert(absolutePath, fileId);
    if (!m_fileIdToMediaIds.contains(fileId)) m_fileIdToMediaIds[fileId] = QList<QString>();
    QFileInfo info(absolutePath);
    m_fileIdMeta[fileId] = { info.exists() ? info.size() : 0, info.exists() ? info.lastModified().toSecsSinceEpoch() : 0 };
}

QString FileManager::generateFileId(const QString& filePath)
{
    // Use file path hash + some uniqueness to ensure no collisions
    QCryptographicHash hash(QCryptographicHash::Sha256);
    hash.addData(filePath.toUtf8());
    
    // Add file size and modification time for better uniqueness
    QFileInfo info(filePath);
    if (info.exists()) {
        hash.addData(QString::number(info.size()).toUtf8());
        hash.addData(QString::number(info.lastModified().toSecsSinceEpoch()).toUtf8());
    }
    
    QString result = hash.result().toHex().left(16); // Use first 16 chars of hash
    return result; // Remove "file_" prefix
}

void FileManager::markFileUploadedToClient(const QString& fileId, const QString& clientId)
{
    if (!m_fileIdToClients.contains(fileId)) {
        m_fileIdToClients[fileId] = QList<QString>();
    }
    if (!m_fileIdToClients[fileId].contains(clientId)) {
        m_fileIdToClients[fileId].append(clientId);
        
    }
}

QList<QString> FileManager::getClientsWithFile(const QString& fileId) const
{
    return m_fileIdToClients.value(fileId);
}

bool FileManager::isFileUploadedToClient(const QString& fileId, const QString& clientId) const
{
    const QList<QString> clients = m_fileIdToClients.value(fileId);
    return clients.contains(clientId);
}

void FileManager::unmarkFileUploadedToClient(const QString& fileId, const QString& clientId)
{
    if (!m_fileIdToClients.contains(fileId)) return;
    QList<QString>& clients = m_fileIdToClients[fileId];
    clients.removeAll(clientId);
}

void FileManager::markMediaUploadedToClient(const QString& mediaId, const QString& clientId)
{
    if (!m_mediaIdToClients.contains(mediaId)) {
        m_mediaIdToClients[mediaId] = QList<QString>();
    }
    if (!m_mediaIdToClients[mediaId].contains(clientId)) {
        m_mediaIdToClients[mediaId].append(clientId);
        qDebug() << "FileManager: Marked media" << mediaId << "as uploaded to client" << clientId;
    }
}

bool FileManager::isMediaUploadedToClient(const QString& mediaId, const QString& clientId) const
{
    const QList<QString> clients = m_mediaIdToClients.value(mediaId);
    return clients.contains(clientId);
}

void FileManager::unmarkMediaUploadedToClient(const QString& mediaId, const QString& clientId)
{
    if (!m_mediaIdToClients.contains(mediaId)) return;
    QList<QString>& clients = m_mediaIdToClients[mediaId];
    clients.removeAll(clientId);
    qDebug() << "FileManager: Unmarked media" << mediaId << "from client" << clientId;
}

void FileManager::setFileRemovalNotifier(std::function<void(const QString& fileId, const QList<QString>& clientIds)> cb)
{
    s_fileRemovalNotifier = std::move(cb);
}

void FileManager::unmarkAllFilesForClient(const QString& clientId)
{
    if (clientId.isEmpty()) return;
    // Iterate copy of keys to allow modification during iteration
    const QList<QString> fileIds = m_fileIdToClients.keys();
    for (const QString& fid : fileIds) {
        QList<QString>& clients = m_fileIdToClients[fid];
        clients.removeAll(clientId);
        if (clients.isEmpty()) {
            // keep entry; removal not strictly necessary here
        }
    }
}

void FileManager::unmarkAllMediaForClient(const QString& clientId)
{
    if (clientId.isEmpty()) return;
    const QList<QString> mediaIds = m_mediaIdToClients.keys();
    for (const QString& mid : mediaIds) {
        QList<QString>& clients = m_mediaIdToClients[mid];
        clients.removeAll(clientId);
        if (clients.isEmpty()) {
            // keep entry; optional cleanup
        }
    }
}

void FileManager::unmarkAllForClient(const QString& clientId)
{
    unmarkAllFilesForClient(clientId);
    unmarkAllMediaForClient(clientId);
}