#include "backend/files/LocalFileRepository.h"
#include <QFile>
#include <QDebug>
#include <QDateTime>
#include <QSet>
#include <algorithm>

LocalFileRepository& LocalFileRepository::instance() {
    static LocalFileRepository instance;
    return instance;
}

QString LocalFileRepository::fileSignature(const QString& path) {
    const QFileInfo info(path);
    if (!info.exists() || !info.isFile() || !info.isReadable()
        || info.canonicalFilePath() != path) return {};
    return QStringLiteral("%1:%2:%3:%4").arg(info.size())
        .arg(info.lastModified().toMSecsSinceEpoch())
        .arg(info.birthTime().toMSecsSinceEpoch())
        .arg(info.metadataChangeTime().toMSecsSinceEpoch());
}

bool LocalFileRepository::isUnchangedPath(const QString& path, const QString& fileId) const {
    if (path.isEmpty() || m_pathToFileId.value(path) != fileId) return false;
    const QString verified = m_pathSignatures.value(path);
    return !verified.isEmpty() && fileSignature(path) == verified;
}

QString LocalFileRepository::generateFileId(const QString& filePath) const {
    QFile file(filePath);
    if (!file.open(QIODevice::ReadOnly)) return {};
    QCryptographicHash hash(QCryptographicHash::Sha256);
    while (!file.atEnd()) {
        const QByteArray block = file.read(1024 * 1024);
        if (block.isEmpty() && file.error() != QFileDevice::NoError) return {};
        hash.addData(block);
    }
    return file.error() == QFileDevice::NoError
        ? QString::fromLatin1(hash.result().toHex()) : QString();
}

void LocalFileRepository::refreshPreferredPath(const QString& fileId) {
    if (fileId.isEmpty()) return;
    const QString preferred = m_fileIdToPath.value(fileId);
    if (!preferred.isEmpty() && m_pathToFileId.value(preferred) == fileId) return;
    auto paths = m_pathToFileId.keys(fileId);
    if (paths.isEmpty()) m_fileIdToPath.remove(fileId);
    else {
        std::sort(paths.begin(), paths.end());
        m_fileIdToPath.insert(fileId, paths.first());
    }
}

void LocalFileRepository::registerPath(const QString& fileId, const QString& path,
                                       const QString& verifiedSignature) {
    const QString previous = m_pathToFileId.value(path);
    m_pathToFileId.insert(path, fileId);
    m_pathSignatures.insert(path, verifiedSignature);
    if (!previous.isEmpty() && previous != fileId) refreshPreferredPath(previous);
    if (!m_fileIdToPath.contains(fileId)) m_fileIdToPath.insert(fileId, path);
}

QString LocalFileRepository::getOrCreateFileId(const QString& filePath) {
    if (filePath.isEmpty()) return {};
    const QString canonical = QFileInfo(filePath).canonicalFilePath();
    const QString before = fileSignature(canonical);
    if (before.isEmpty()) return {};
    // Legacy callers still request a fresh full identity at every boundary.
    // Once verified, lookups below only stat the paths; they never hash on UI.
    const QString fileId = generateFileId(canonical);
    if (fileId.isEmpty() || fileSignature(canonical) != before) return {};
    registerPath(fileId, canonical, before);
    return fileId;
}

void LocalFileRepository::registerVerifiedLocalFile(const QString& fileId, const QString& filePath) {
    if (fileId.size() != 64 || filePath.isEmpty()) return;
    const QString canonical = QFileInfo(filePath).canonicalFilePath();
    const QString stamp = fileSignature(canonical);
    if (!stamp.isEmpty()) registerPath(fileId, canonical, stamp);
}

QString LocalFileRepository::getFilePathForId(const QString& fileId) const {
    const QString preferred = m_fileIdToPath.value(fileId);
    if (isUnchangedPath(preferred, fileId)) return preferred;
    // Every candidate was independently verified when registered. Never
    // substitute modified bytes merely because another occurrence shares a
    // former SHA-256, and never rehash a large video during this UI lookup.
    auto alternatives = m_pathToFileId.keys(fileId);
    std::sort(alternatives.begin(), alternatives.end());
    for (const auto& path : alternatives)
        if (path != preferred && isUnchangedPath(path, fileId)) return path;
    return {};
}

QStringList LocalFileRepository::recordedFilePaths(const QString& fileId) const {
    auto paths = m_pathToFileId.keys(fileId);
    std::sort(paths.begin(), paths.end());
    return paths;
}

bool LocalFileRepository::hasFileId(const QString& fileId) const {
    return m_fileIdToPath.contains(fileId);
}

QList<QString> LocalFileRepository::getAllFileIds() const {
    return m_fileIdToPath.keys();
}

void LocalFileRepository::registerReceivedFilePath(const QString& fileId, const QString& absolutePath) {
    if (fileId.isEmpty() || absolutePath.isEmpty()) return;
    const QString canonical = QFileInfo(absolutePath).canonicalFilePath();
    const QString stamp = fileSignature(canonical);
    if (!stamp.isEmpty()) registerPath(fileId, canonical, stamp);
}

void LocalFileRepository::removeReceivedFileMapping(const QString& fileId) {
    removeFileMapping(fileId);
}

void LocalFileRepository::removeFileMapping(const QString& fileId) {
    const auto paths = m_pathToFileId.keys(fileId);
    for (const auto& path : paths) {
        m_pathToFileId.remove(path);
        m_pathSignatures.remove(path);
    }
    m_fileIdToPath.remove(fileId);
}

QList<QString> LocalFileRepository::getFileIdsUnderPathPrefix(const QString& pathPrefix) const {
    QSet<QString> ids;
    // Include every registered location, even if a directory has just been
    // renamed or removed: cleanup must retire all mappings for affected IDs.
    for (auto it = m_pathToFileId.cbegin(); it != m_pathToFileId.cend(); ++it)
        if (it.key().startsWith(pathPrefix)) ids.insert(it.value());
    auto result = ids.values();
    std::sort(result.begin(), result.end());
    return result;
}

void LocalFileRepository::clear() {
    m_fileIdToPath.clear();
    m_pathToFileId.clear();
    m_pathSignatures.clear();
}
