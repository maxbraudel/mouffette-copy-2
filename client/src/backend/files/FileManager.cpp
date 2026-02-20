#include "backend/files/FileManager.h"
#include "backend/files/LocalFileRepository.h"
#include "backend/network/RemoteFileTracker.h"
#include "backend/files/FileMemoryCache.h"
#include <QFile>
#include <QDebug>
#include <QDir>

// Phase 4.3: Constructor with dependency injection
FileManager::FileManager()
{
    // Phase 4.2: Initialize service references
    m_repository = &LocalFileRepository::instance();
    m_tracker = &RemoteFileTracker::instance();
    m_cache = &FileMemoryCache::instance();
}

FileManager::~FileManager()
{
    // Services are singletons, no need to delete
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

void FileManager::setFileRemovalNotifier(std::function<void(const QString& fileId, const QList<QString>& clientIds, const QList<QString>& canvasSessionIds)> cb)
{
    RemoteFileTracker::instance().setFileRemovalNotifier(std::move(cb));
}

void FileManager::unmarkAllForClient(const QString& clientId)
{
    m_tracker->unmarkAllFilesForClient(clientId);
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

void FileManager::associateFileWithIdea(const QString& fileId, const QString& canvasSessionId)
{
    // Delegate to RemoteFileTracker
    m_tracker->associateFileWithIdea(fileId, canvasSessionId);
}

void FileManager::dissociateFileFromIdea(const QString& fileId, const QString& canvasSessionId)
{
    // Delegate to RemoteFileTracker
    m_tracker->dissociateFileFromIdea(fileId, canvasSessionId);
}

QSet<QString> FileManager::getIdeaIdsForFile(const QString& fileId) const
{
    // Delegate to RemoteFileTracker
    return m_tracker->getIdeaIdsForFile(fileId);
}

QSet<QString> FileManager::getFileIdsForIdea(const QString& canvasSessionId) const
{
    // Delegate to RemoteFileTracker
    return m_tracker->getFileIdsForIdea(canvasSessionId);
}

void FileManager::replaceIdeaFileSet(const QString& canvasSessionId, const QSet<QString>& fileIds)
{
    // Delegate to RemoteFileTracker
    m_tracker->replaceIdeaFileSet(canvasSessionId, fileIds);
}

void FileManager::removeIdeaAssociations(const QString& canvasSessionId)
{
    // Delegate to RemoteFileTracker
    m_tracker->removeIdeaAssociations(canvasSessionId);
}