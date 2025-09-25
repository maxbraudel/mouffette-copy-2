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
    
    // Check if we already have this file
    if (m_pathToFileId.contains(normalizedPath)) {
        return m_pathToFileId[normalizedPath];
    }
    
    // Generate new file ID
    QString fileId = generateFileId(normalizedPath);
    
    // Store mappings
    m_pathToFileId[normalizedPath] = fileId;
    m_fileIdToPath[fileId] = normalizedPath;
    m_fileIdToMediaIds[fileId] = QList<QString>(); // Initialize empty list
    
    qDebug() << "Created new file ID:" << fileId << "for path:" << normalizedPath;
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
    
    qDebug() << "Associated media" << mediaId << "with file" << fileId;
}

void FileManager::removeMediaAssociation(const QString& mediaId)
{
    qDebug() << "FileManager: Removing media association for" << mediaId;
    
    if (!m_mediaIdToFileId.contains(mediaId)) {
        qDebug() << "FileManager: Media ID" << mediaId << "not found in associations";
        return;
    }
    
    QString fileId = m_mediaIdToFileId[mediaId];
    qDebug() << "FileManager: Media" << mediaId << "was associated with file" << fileId;
    m_fileIdToMediaIds[fileId].removeAll(mediaId);
    m_mediaIdToFileId.remove(mediaId);
    
    qDebug() << "FileManager: File" << fileId << "now has" << m_fileIdToMediaIds[fileId].size() << "media associations";
    
    // Clean up file if no more media references it  
    removeFileIfUnused(fileId);
    
    qDebug() << "FileManager: Removed media association for" << mediaId;
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
    qDebug() << "FileManager: Checking if file" << fileId << "is unused";
    
    if (!m_fileIdToMediaIds.contains(fileId)) {
        qDebug() << "FileManager: File" << fileId << "not found in media associations";
        return;
    }
    
    if (m_fileIdToMediaIds[fileId].isEmpty()) {
        QString filePath = m_fileIdToPath.value(fileId);
        QList<QString> clientsWithFile = m_fileIdToClients.value(fileId);
        
        qDebug() << "FileManager: File" << fileId << "is unused, removing from" << clientsWithFile.size() << "clients";
        
        // Notify that file should be removed from remote clients
        if (!clientsWithFile.isEmpty() && s_fileRemovalNotifier) {
            qDebug() << "FileManager: Calling removal notifier for clients:" << clientsWithFile;
            s_fileRemovalNotifier(fileId, clientsWithFile);
        } else {
            qDebug() << "FileManager: No clients to notify or no notifier set";
        }
        
        // Clean up local tracking data
        m_fileIdToPath.remove(fileId);
        m_pathToFileId.remove(filePath);
        m_fileIdToMediaIds.remove(fileId);
        m_fileIdToClients.remove(fileId);
        
        qDebug() << "FileManager: Removed unused file ID:" << fileId << "from" << clientsWithFile.size() << "clients";
    } else {
        qDebug() << "FileManager: File" << fileId << "still has" << m_fileIdToMediaIds[fileId].size() << "media associations, not removing";
    }
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
        qDebug() << "FileManager: Marked file" << fileId << "as uploaded to client" << clientId;
    }
}

QList<QString> FileManager::getClientsWithFile(const QString& fileId) const
{
    return m_fileIdToClients.value(fileId);
}

void FileManager::setFileRemovalNotifier(std::function<void(const QString& fileId, const QList<QString>& clientIds)> cb)
{
    s_fileRemovalNotifier = std::move(cb);
}