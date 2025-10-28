#include "FileManager.h"
#include "LocalFileRepository.h"
#include "RemoteFileTracker.h"
#include "FileMemoryCache.h"
#include <QFile>
#include <QFileInfo>
#include <QCryptographicHash>
#include <QDebug>
#include <QIODevice>
#include <QSet>
#include <QByteArrayView>

// Phase 4.2: FileManager delegates to specialized services
// - LocalFileRepository for fileId ↔ filePath
// - RemoteFileTracker for client & ideaId tracking
// - FileMemoryCache for memory caching

// Static member definition
std::function<void(const QString& fileId, const QList<QString>& clientIds, const QList<QString>& ideaIds)> FileManager::s_fileRemovalNotifier;

FileManager::FileManager()
    : m_repository(&LocalFileRepository::instance())
    , m_tracker(&RemoteFileTracker::instance())
    , m_cache(&FileMemoryCache::instance())
{
    // Initialize removal callback for tracker
    m_tracker->setFileRemovalNotifier([this](const QString& fileId, const QList<QString>& clientIds, const QList<QString>& ideaIds) {
        if (s_fileRemovalNotifier) {
            s_fileRemovalNotifier(fileId, clientIds, ideaIds);
        }
    });
}

FileManager& FileManager::instance()
{
    static FileManager instance;
    return instance;
}

QString FileManager::getOrCreateFileId(const QString& filePath)
{
    // Delegate to LocalFileRepository
    return m_repository->getOrCreateFileId(filePath);
}

void FileManager::associateMediaWithFile(const QString& mediaId, const QString& fileId)
{
    if (!m_repository->hasFileId(fileId)) {
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

    const QString fileId = m_mediaIdToFileId.value(mediaId);
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
    // Delegate to LocalFileRepository
    return m_repository->getFilePathForId(fileId);
}

QList<QString> FileManager::getAllFileIds() const
{
    // Delegate to LocalFileRepository
    return m_repository->getAllFileIds();
}

bool FileManager::hasFileId(const QString& fileId) const
{
    // Delegate to LocalFileRepository
    return m_repository->hasFileId(fileId);
}

void FileManager::removeFileIfUnused(const QString& fileId)
{
    if (!m_fileIdToMediaIds.contains(fileId)) {
        return;
    }

    if (!m_fileIdToMediaIds.value(fileId).isEmpty()) {
        return;
    }

    // Use RemoteFileTracker callback to notify before removal
    m_tracker->checkAndNotifyIfUnused(fileId);

    // Clean up local media associations
    m_fileIdToMediaIds.remove(fileId);
    
    // Clean up service-managed data
    m_repository->removeFileMapping(fileId);
    m_tracker->removeAllTrackingForFile(fileId);
    m_cache->releaseFileMemory(fileId);
}

void FileManager::registerReceivedFilePath(const QString& fileId, const QString& absolutePath) {
    // Delegate to LocalFileRepository
    m_repository->registerReceivedFilePath(fileId, absolutePath);
}

void FileManager::removeReceivedFileMapping(const QString& fileId) {
    if (fileId.isEmpty()) return;
    
    // Clean up local cache and repository
    m_cache->releaseFileMemory(fileId);
    m_repository->removeFileMapping(fileId);
    
    // Clean up tracker data
    m_tracker->removeAllTrackingForFile(fileId);
}

void FileManager::preloadFileIntoMemory(const QString& fileId) {
    // Delegate to FileMemoryCache
    QString filePath = m_repository->getFilePathForId(fileId);
    if (!filePath.isEmpty()) {
        m_cache->preloadFileIntoMemory(fileId, filePath);
    }
}

QSharedPointer<QByteArray> FileManager::getFileBytes(const QString& fileId, bool forceReload) {
    // Delegate to FileMemoryCache
    QString filePath = m_repository->getFilePathForId(fileId);
    if (filePath.isEmpty()) {
        return {};
    }
    return m_cache->getFileBytes(fileId, filePath, forceReload);
}

void FileManager::releaseFileMemory(const QString& fileId) {
    // Delegate to FileMemoryCache
    m_cache->releaseFileMemory(fileId);
}

QString FileManager::generateFileId(const QString& filePath)
{
    QCryptographicHash hash(QCryptographicHash::Sha256);
    QFile file(filePath);
    if (file.open(QIODevice::ReadOnly)) {
        constexpr qint64 kChunkSize = 1 << 16; // 64 KiB
        QByteArray buffer;
        buffer.resize(static_cast<int>(kChunkSize));
        while (true) {
            const qint64 bytesRead = file.read(buffer.data(), kChunkSize);
            if (bytesRead <= 0) break;
            hash.addData(QByteArrayView(buffer.constData(), static_cast<qsizetype>(bytesRead)));
        }
        file.close();
    } else {
        // Fallback to metadata-based hash if file cannot be opened (should be rare)
        hash.addData(filePath.toUtf8());
        QFileInfo info(filePath);
        if (info.exists()) {
            hash.addData(QString::number(info.size()).toUtf8());
            hash.addData(QString::number(info.lastModified().toSecsSinceEpoch()).toUtf8());
        }
    }

    const QByteArray digest = hash.result();
    if (digest.isEmpty()) {
        // As a final fallback, hash the path string itself
        return QString::fromUtf8(QCryptographicHash::hash(filePath.toUtf8(), QCryptographicHash::Sha256).toHex().left(32));
    }

    return QString::fromUtf8(digest.toHex().left(32));
}

void FileManager::markFileUploadedToClient(const QString& fileId, const QString& clientId)
{
    // Delegate to RemoteFileTracker
    m_tracker->markFileUploadedToClient(fileId, clientId);
}

QList<QString> FileManager::getClientsWithFile(const QString& fileId) const
{
    // Delegate to RemoteFileTracker
    return m_tracker->getClientsWithFile(fileId);
}

bool FileManager::isFileUploadedToClient(const QString& fileId, const QString& clientId) const
{
    // Delegate to RemoteFileTracker
    return m_tracker->isFileUploadedToClient(fileId, clientId);
}

void FileManager::unmarkFileUploadedToClient(const QString& fileId, const QString& clientId)
{
    // Delegate to RemoteFileTracker
    m_tracker->unmarkFileUploadedToClient(fileId, clientId);
}

void FileManager::markMediaUploadedToClient(const QString& mediaId, const QString& clientId)
{
    // Use fileId tracking instead of mediaId tracking
    QString fileId = m_mediaIdToFileId.value(mediaId);
    if (!fileId.isEmpty()) {
        markFileUploadedToClient(fileId, clientId);
    }
}

bool FileManager::isMediaUploadedToClient(const QString& mediaId, const QString& clientId) const
{
    // Use fileId tracking instead of mediaId tracking
    QString fileId = m_mediaIdToFileId.value(mediaId);
    if (fileId.isEmpty()) return false;
    return isFileUploadedToClient(fileId, clientId);
}

void FileManager::unmarkMediaUploadedToClient(const QString& mediaId, const QString& clientId)
{
    // Use fileId tracking instead of mediaId tracking
    QString fileId = m_mediaIdToFileId.value(mediaId);
    if (!fileId.isEmpty()) {
        unmarkFileUploadedToClient(fileId, clientId);
    }
}

void FileManager::setFileRemovalNotifier(std::function<void(const QString& fileId, const QList<QString>& clientIds, const QList<QString>& ideaIds)> cb)
{
    s_fileRemovalNotifier = std::move(cb);
}

void FileManager::unmarkAllFilesForClient(const QString& clientId)
{
    // Delegate to RemoteFileTracker
    m_tracker->unmarkAllFilesForClient(clientId);
}

void FileManager::unmarkAllMediaForClient(const QString& clientId)
{
    // Redirect to file-based unmark (media tracking is file-based)
    unmarkAllFilesForClient(clientId);
}

void FileManager::unmarkAllForClient(const QString& clientId)
{
    unmarkAllFilesForClient(clientId);
}

void FileManager::removeReceivedFileMappingsUnderPathPrefix(const QString& pathPrefix)
{
    if (pathPrefix.isEmpty()) return;

    // Get affected fileIds from repository
    QList<QString> fileIdsToRemove = m_repository->getFileIdsUnderPathPrefix(pathPrefix);
    if (fileIdsToRemove.isEmpty()) return;

    QSet<QString> removalSet = QSet<QString>(fileIdsToRemove.cbegin(), fileIdsToRemove.cend());

    // Clean up repository, tracker, and cache
    for (const QString& fileId : fileIdsToRemove) {
        m_repository->removeFileMapping(fileId);
        m_tracker->removeAllTrackingForFile(fileId);
        m_cache->releaseFileMemory(fileId);
        m_fileIdToMediaIds.remove(fileId);
    }

    // Clean up media associations
    for (auto it = m_mediaIdToFileId.begin(); it != m_mediaIdToFileId.end();) {
        if (removalSet.contains(it.value())) {
            it = m_mediaIdToFileId.erase(it);
        } else {
            ++it;
        }
    }
}

void FileManager::associateFileWithIdea(const QString& fileId, const QString& ideaId)
{
    // Delegate to RemoteFileTracker
    m_tracker->associateFileWithIdea(fileId, ideaId);
}

void FileManager::dissociateFileFromIdea(const QString& fileId, const QString& ideaId)
{
    // Delegate to RemoteFileTracker
    m_tracker->dissociateFileFromIdea(fileId, ideaId);
}

QSet<QString> FileManager::getIdeaIdsForFile(const QString& fileId) const
{
    // Delegate to RemoteFileTracker
    return m_tracker->getIdeaIdsForFile(fileId);
}

QSet<QString> FileManager::getFileIdsForIdea(const QString& ideaId) const
{
    // Delegate to RemoteFileTracker
    return m_tracker->getFileIdsForIdea(ideaId);
}

void FileManager::replaceIdeaFileSet(const QString& ideaId, const QSet<QString>& fileIds)
{
    // Delegate to RemoteFileTracker
    m_tracker->replaceIdeaFileSet(ideaId, fileIds);
}

void FileManager::removeIdeaAssociations(const QString& ideaId)
{
    // Delegate to RemoteFileTracker
    m_tracker->removeIdeaAssociations(ideaId);
}