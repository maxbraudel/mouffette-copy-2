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

// Phase 4.2: FileManager transitioning to use specialized services
// TODO: Progressively migrate internal storage to services
// - LocalFileRepository for fileId ↔ filePath
// - RemoteFileTracker for client & ideaId tracking
// - FileMemoryCache for memory caching
// Current implementation still uses internal maps for compatibility

// Static member definition
std::function<void(const QString& fileId, const QList<QString>& clientIds, const QList<QString>& ideaIds)> FileManager::s_fileRemovalNotifier;

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
        releaseFileMemory(existingId);
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

    if (!m_fileIdToMediaIds.value(fileId).isEmpty()) {
        return;
    }

    const QString filePath = m_fileIdToPath.value(fileId);
    const QList<QString> clientsWithFile = m_fileIdToClients.value(fileId);
    const QList<QString> ideaList = m_fileIdToIdeaIds.value(fileId).values();

    if (s_fileRemovalNotifier && (!clientsWithFile.isEmpty() || !ideaList.isEmpty())) {
        s_fileRemovalNotifier(fileId, clientsWithFile, ideaList);
    }

    // Clean up local tracking data
    releaseFileMemory(fileId);
    m_fileIdToPath.remove(fileId);
    m_pathToFileId.remove(filePath);
    m_fileIdToMediaIds.remove(fileId);
    m_fileIdToClients.remove(fileId);
    m_fileIdMeta.remove(fileId);
    if (m_fileIdToIdeaIds.contains(fileId)) {
        const QSet<QString> ideas = m_fileIdToIdeaIds.take(fileId);
        for (const QString& ideaId : ideas) {
            QSet<QString>& files = m_ideaIdToFileIds[ideaId];
            files.remove(fileId);
            if (files.isEmpty()) {
                m_ideaIdToFileIds.remove(ideaId);
            }
        }
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
    releaseFileMemory(fileId);
}

void FileManager::removeReceivedFileMapping(const QString& fileId) {
    if (fileId.isEmpty()) return;
    QString path = m_fileIdToPath.value(fileId);
    if (!path.isEmpty()) {
        m_pathToFileId.remove(path);
    }
    m_fileIdToPath.remove(fileId);
    m_fileIdMeta.remove(fileId);
    releaseFileMemory(fileId);
    if (m_fileIdToIdeaIds.contains(fileId)) {
        const QSet<QString> ideas = m_fileIdToIdeaIds.take(fileId);
        for (const QString& ideaId : ideas) {
            QSet<QString>& files = m_ideaIdToFileIds[ideaId];
            files.remove(fileId);
            if (files.isEmpty()) {
                m_ideaIdToFileIds.remove(ideaId);
            }
        }
    }
    // Don't touch media associations here; they are sender-side concepts. Remote just needs path lookup cleared.
}

void FileManager::preloadFileIntoMemory(const QString& fileId) {
    if (fileId.isEmpty()) return;
    getFileBytes(fileId, true);
}

QSharedPointer<QByteArray> FileManager::getFileBytes(const QString& fileId, bool forceReload) {
    if (fileId.isEmpty()) return {};
    if (!forceReload) {
        const auto existing = m_fileMemoryCache.value(fileId);
        if (!existing.isNull() && !existing->isEmpty()) {
            return existing;
        }
    }

    const QString path = m_fileIdToPath.value(fileId);
    if (path.isEmpty()) {
        return {};
    }

    QFile file(path);
    if (!file.exists()) {
        return {};
    }
    if (!file.open(QIODevice::ReadOnly)) {
        qWarning() << "FileManager: Failed to open" << path << "for preloading";
        return {};
    }
    QByteArray bytes = file.readAll();
    file.close();

    auto shared = QSharedPointer<QByteArray>::create(std::move(bytes));
    m_fileMemoryCache.insert(fileId, shared);
    return shared;
}

void FileManager::releaseFileMemory(const QString& fileId) {
    if (fileId.isEmpty()) return;
    m_fileMemoryCache.remove(fileId);
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
    // Redirect to file-based unmark (mediaId tracking removed)
    unmarkAllFilesForClient(clientId);
}

void FileManager::unmarkAllForClient(const QString& clientId)
{
    unmarkAllFilesForClient(clientId);
    unmarkAllMediaForClient(clientId);
}

void FileManager::removeReceivedFileMappingsUnderPathPrefix(const QString& pathPrefix)
{
    if (pathPrefix.isEmpty()) return;

    QList<QString> fileIdsToRemove;
    fileIdsToRemove.reserve(m_fileIdToPath.size());
    for (auto it = m_fileIdToPath.cbegin(); it != m_fileIdToPath.cend(); ++it) {
        if (it.value().startsWith(pathPrefix)) {
            fileIdsToRemove.append(it.key());
        }
    }

    if (fileIdsToRemove.isEmpty()) return;

    QSet<QString> removalSet = QSet<QString>(fileIdsToRemove.cbegin(), fileIdsToRemove.cend());

    for (const QString& fileId : fileIdsToRemove) {
        const QString path = m_fileIdToPath.take(fileId);
        if (!path.isEmpty()) {
            m_pathToFileId.remove(path);
        }
        m_fileIdToMediaIds.remove(fileId);
        m_fileIdToClients.remove(fileId);
        m_fileIdMeta.remove(fileId);
        releaseFileMemory(fileId);
        if (m_fileIdToIdeaIds.contains(fileId)) {
            const QSet<QString> ideas = m_fileIdToIdeaIds.take(fileId);
            for (const QString& ideaId : ideas) {
                QSet<QString>& files = m_ideaIdToFileIds[ideaId];
                files.remove(fileId);
                if (files.isEmpty()) {
                    m_ideaIdToFileIds.remove(ideaId);
                }
            }
        }
    }

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
    if (fileId.isEmpty() || ideaId.isEmpty()) return;
    m_fileIdToIdeaIds[fileId].insert(ideaId);
    m_ideaIdToFileIds[ideaId].insert(fileId);
}

void FileManager::dissociateFileFromIdea(const QString& fileId, const QString& ideaId)
{
    if (fileId.isEmpty() || ideaId.isEmpty()) return;
    auto fileIt = m_fileIdToIdeaIds.find(fileId);
    if (fileIt != m_fileIdToIdeaIds.end()) {
        fileIt->remove(ideaId);
        if (fileIt->isEmpty()) {
            m_fileIdToIdeaIds.erase(fileIt);
        }
    }
    auto ideaIt = m_ideaIdToFileIds.find(ideaId);
    if (ideaIt != m_ideaIdToFileIds.end()) {
        ideaIt->remove(fileId);
        if (ideaIt->isEmpty()) {
            m_ideaIdToFileIds.erase(ideaIt);
        }
    }
}

QSet<QString> FileManager::getIdeaIdsForFile(const QString& fileId) const
{
    return m_fileIdToIdeaIds.value(fileId);
}

QSet<QString> FileManager::getFileIdsForIdea(const QString& ideaId) const
{
    return m_ideaIdToFileIds.value(ideaId);
}

void FileManager::replaceIdeaFileSet(const QString& ideaId, const QSet<QString>& fileIds)
{
    if (ideaId.isEmpty()) return;

    const QSet<QString> previous = m_ideaIdToFileIds.value(ideaId);
    const QSet<QString> toRemove = previous - fileIds;

    for (const QString& fid : toRemove) {
        QSet<QString>& ideas = m_fileIdToIdeaIds[fid];
        ideas.remove(ideaId);
        if (ideas.isEmpty()) {
            m_fileIdToIdeaIds.remove(fid);
        }
    }

    QSet<QString>& dest = m_ideaIdToFileIds[ideaId];
    dest = fileIds;
    if (dest.isEmpty()) {
        m_ideaIdToFileIds.remove(ideaId);
    }

    for (const QString& fid : fileIds) {
        if (fid.isEmpty()) continue;
        m_fileIdToIdeaIds[fid].insert(ideaId);
    }
}

void FileManager::removeIdeaAssociations(const QString& ideaId)
{
    if (ideaId.isEmpty()) return;
    const QSet<QString> files = m_ideaIdToFileIds.take(ideaId);
    for (const QString& fid : files) {
        QSet<QString>& ideas = m_fileIdToIdeaIds[fid];
        ideas.remove(ideaId);
        if (ideas.isEmpty()) {
            m_fileIdToIdeaIds.remove(fid);
        }
    }
}