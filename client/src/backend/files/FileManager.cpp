#include "backend/files/FileManager.h"
#include "backend/files/LocalFileRepository.h"
#include "backend/network/RemoteFileTracker.h"
#include "backend/files/FileMemoryCache.h"
#include <QFile>
#include <QDebug>
#include <QDir>
#include <utility>

QString FileManager::receivedScopeKey(const RemoteCacheStore::Scope& scope)
{
    return scope.senderEndpointId + QChar(0x1f) + scope.remoteSessionId;
}

QString FileManager::receivedMemoryKey(const RemoteCacheStore::Scope& scope,
                                       const QString& fileId)
{
    return QStringLiteral("received:") + receivedScopeKey(scope)
        + QChar(0x1f) + QString::number(scope.generation)
        + QChar(0x1f) + fileId;
}

FileManager::FileManager()
{
    m_repository = &LocalFileRepository::instance();
    m_tracker = &RemoteFileTracker::instance();
    m_cache = &FileMemoryCache::instance();
}

void FileManager::registerVerifiedLocalFile(const QString& fileId, const QString& filePath) {
    m_repository->registerVerifiedLocalFile(fileId, filePath);
}

QString FileManager::getOrCreateFileId(const QString& filePath)
{
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

void FileManager::beginMediaAssociationTransaction()
{
    ++m_mediaAssociationTransactionDepth;
}

void FileManager::endMediaAssociationTransaction()
{
    Q_ASSERT(m_mediaAssociationTransactionDepth > 0);
    if (m_mediaAssociationTransactionDepth <= 0 || --m_mediaAssociationTransactionDepth) return;
    const QSet<QString> deferred = std::exchange(m_deferredUnusedFiles, {});
    for (const QString& fileId : deferred) removeFileIfUnused(fileId);
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
    return m_repository->getFilePathForId(fileId);
}

QStringList FileManager::getRecordedFilePathsForId(const QString& fileId) const
{
    return m_repository->recordedFilePaths(fileId);
}

QList<QString> FileManager::getAllFileIds() const
{
    return m_repository->getAllFileIds();
}

bool FileManager::hasFileId(const QString& fileId) const
{
    return m_repository->hasFileId(fileId);
}

void FileManager::removeFileIfUnused(const QString& fileId)
{
    if (m_mediaAssociationTransactionDepth > 0) {
        m_deferredUnusedFiles.insert(fileId);
        return;
    }
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

QString FileManager::getReceivedFilePath(const RemoteCacheStore::Scope& scope,
                                         const QString& fileId) const
{
    const auto scopeIt = m_receivedFilesByScope.constFind(receivedScopeKey(scope));
    if (scopeIt == m_receivedFilesByScope.constEnd()
        || !(scopeIt->scope == scope)) {
        return {};
    }
    return scopeIt->pathsByFileId.value(fileId);
}

bool FileManager::registerReceivedFilePath(const RemoteCacheStore::Scope& scope,
                                           const QString& fileId,
                                           const QString& absolutePath)
{
    if (fileId.isEmpty() || absolutePath.isEmpty()
        || !RemoteCacheStore::isValidEndpointId(scope.senderEndpointId)
        || !RemoteCacheStore::isValidSessionId(scope.remoteSessionId)
        || scope.generation == 0) {
        return false;
    }
    const QFileInfo info(absolutePath);
    const QString canonicalPath = info.canonicalFilePath();
    if (canonicalPath.isEmpty() || !info.isFile() || info.isSymLink()) return false;

    const QString key = receivedScopeKey(scope);
    auto scopeIt = m_receivedFilesByScope.find(key);
    if (scopeIt == m_receivedFilesByScope.end()) {
        ReceivedScopeFiles files;
        files.scope = scope;
        scopeIt = m_receivedFilesByScope.insert(key, files);
    } else if (!(scopeIt->scope == scope)) {
        return false;
    }
    const QString existing = scopeIt->pathsByFileId.value(fileId);
    if (!existing.isEmpty()) {
        return QDir::cleanPath(existing) == QDir::cleanPath(canonicalPath);
    }
    scopeIt->pathsByFileId.insert(fileId, canonicalPath);
    return true;
}

bool FileManager::removeReceivedFileMapping(const RemoteCacheStore::Scope& scope,
                                            const QString& fileId)
{
    const QString key = receivedScopeKey(scope);
    auto scopeIt = m_receivedFilesByScope.find(key);
    if (scopeIt == m_receivedFilesByScope.end() || !(scopeIt->scope == scope)) {
        return false;
    }
    releaseReceivedFileMemory(scope, fileId);
    const bool removed = scopeIt->pathsByFileId.remove(fileId) > 0;
    if (scopeIt->pathsByFileId.isEmpty()) m_receivedFilesByScope.erase(scopeIt);
    return removed;
}

int FileManager::removeReceivedFileMappingsForScope(
    const RemoteCacheStore::Scope& scope)
{
    const QString key = receivedScopeKey(scope);
    auto scopeIt = m_receivedFilesByScope.find(key);
    if (scopeIt == m_receivedFilesByScope.end() || !(scopeIt->scope == scope)) return 0;
    const QList<QString> ids = scopeIt->pathsByFileId.keys();
    for (const QString& fileId : ids) {
        releaseReceivedFileMemory(scope, fileId);
        m_tracker->dissociateFileFromProject(fileId, scope.remoteSessionId);
    }
    const int count = ids.size();
    m_receivedFilesByScope.erase(scopeIt);
    return count;
}

bool FileManager::rebindReceivedFileScope(const RemoteCacheStore::Scope& oldScope,
                                          const RemoteCacheStore::Scope& newScope)
{
    if (oldScope.senderEndpointId != newScope.senderEndpointId
        || oldScope.remoteSessionId != newScope.remoteSessionId
        || newScope.generation <= oldScope.generation) {
        return false;
    }
    auto scopeIt = m_receivedFilesByScope.find(receivedScopeKey(oldScope));
    if (scopeIt == m_receivedFilesByScope.end()) return true;
    if (scopeIt->scope == newScope) return true;
    if (!(scopeIt->scope == oldScope)) return false;
    for (const QString& fileId : scopeIt->pathsByFileId.keys()) {
        releaseReceivedFileMemory(oldScope, fileId);
    }
    scopeIt->scope = newScope;
    return true;
}

QList<QString> FileManager::getReceivedFileIds(
    const RemoteCacheStore::Scope& scope) const
{
    const auto scopeIt = m_receivedFilesByScope.constFind(receivedScopeKey(scope));
    if (scopeIt == m_receivedFilesByScope.constEnd() || !(scopeIt->scope == scope)) {
        return {};
    }
    return scopeIt->pathsByFileId.keys();
}

void FileManager::releaseReceivedFileMemory(const RemoteCacheStore::Scope& scope,
                                            const QString& fileId)
{
    m_cache->releaseFileMemory(receivedMemoryKey(scope, fileId));
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

void FileManager::setFileRemovalNotifier(std::function<void(const QString& fileId, const QList<QString>& clientIds, const QList<QString>& projectIds)> cb)
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

void FileManager::associateFileWithProject(const QString& fileId, const QString& projectId)
{
    // Delegate to RemoteFileTracker
    m_tracker->associateFileWithProject(fileId, projectId);
}

void FileManager::dissociateFileFromProject(const QString& fileId, const QString& projectId)
{
    // Delegate to RemoteFileTracker
    m_tracker->dissociateFileFromProject(fileId, projectId);
}

QSet<QString> FileManager::getProjectIdsForFile(const QString& fileId) const
{
    // Delegate to RemoteFileTracker
    return m_tracker->getProjectIdsForFile(fileId);
}

QSet<QString> FileManager::getFileIdsForProject(const QString& projectId) const
{
    // Delegate to RemoteFileTracker
    return m_tracker->getFileIdsForProject(projectId);
}

void FileManager::replaceProjectFileSet(const QString& projectId, const QSet<QString>& fileIds)
{
    // Delegate to RemoteFileTracker
    m_tracker->replaceProjectFileSet(projectId, fileIds);
}

void FileManager::removeProjectAssociations(const QString& projectId)
{
    // Delegate to RemoteFileTracker
    m_tracker->removeProjectAssociations(projectId);
}
