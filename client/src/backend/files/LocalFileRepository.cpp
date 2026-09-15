#include "backend/files/LocalFileRepository.h"
#include <QFile>
#include <QDebug>

LocalFileRepository& LocalFileRepository::instance() {
    static LocalFileRepository instance;
    return instance;
}

QString LocalFileRepository::generateFileId(const QString& filePath) const {
    QFileInfo fileInfo(filePath);
    QString canonicalPath = fileInfo.canonicalFilePath();
    if (canonicalPath.isEmpty()) {
        canonicalPath = fileInfo.absoluteFilePath();
    }

    QFile file(canonicalPath);
    if (!file.open(QIODevice::ReadOnly)) {
        return QString();
    }

    // Protocol v4 uses one content-addressed identity all the way from the
    // local canvas through upload validation and the immutable scene manifest.
    // Metadata and paths must never participate in this value.
    QCryptographicHash hash(QCryptographicHash::Sha256);
    while (!file.atEnd()) {
        const QByteArray block = file.read(1024 * 1024);
        if (block.isEmpty() && file.error() != QFileDevice::NoError) {
            return QString();
        }
        hash.addData(block);
    }
    return QString::fromLatin1(hash.result().toHex());
}

QString LocalFileRepository::getOrCreateFileId(const QString& filePath) {
    if (filePath.isEmpty()) {
        qWarning() << "LocalFileRepository::getOrCreateFileId: empty filePath";
        return QString();
    }
    
    QFileInfo fileInfo(filePath);
    QString canonicalPath = fileInfo.canonicalFilePath();
    if (canonicalPath.isEmpty()) {
        canonicalPath = fileInfo.absoluteFilePath();
    }
    
    // Re-evaluate identity on every boundary call. The canonical path alone is
    // not a file identity: editors commonly replace/re-encode a movie in place.
    const QString fileId = generateFileId(canonicalPath);
    if (fileId.isEmpty()) {
        qWarning() << "LocalFileRepository: source could not be hashed";
        return QString();
    }
    auto it = m_pathToFileId.constFind(canonicalPath);
    if (it != m_pathToFileId.constEnd()) {
        if (it.value() == fileId) return fileId;
        const QString staleFileId = it.value();
        m_pathToFileId.remove(canonicalPath);
        if (m_fileIdToPath.value(staleFileId) == canonicalPath) {
            m_fileIdToPath.remove(staleFileId);
        }
        qDebug() << "LocalFileRepository: source content changed; rotating fileId";
    }

    m_fileIdToPath.insert(fileId, canonicalPath);
    m_pathToFileId.insert(canonicalPath, fileId);
    
    qDebug() << "LocalFileRepository: Created fileId" << fileId;
    return fileId;
}

QString LocalFileRepository::getFilePathForId(const QString& fileId) const {
    return m_fileIdToPath.value(fileId);
}

bool LocalFileRepository::hasFileId(const QString& fileId) const {
    return m_fileIdToPath.contains(fileId);
}

QList<QString> LocalFileRepository::getAllFileIds() const {
    return m_fileIdToPath.keys();
}

void LocalFileRepository::registerReceivedFilePath(const QString& fileId, const QString& absolutePath) {
    if (fileId.isEmpty() || absolutePath.isEmpty()) {
        return;
    }
    
    if (m_fileIdToPath.contains(fileId)) {
        // Already registered, don't override
        return;
    }
    
    QFileInfo fileInfo(absolutePath);
    QString canonicalPath = fileInfo.canonicalFilePath();
    if (canonicalPath.isEmpty()) {
        canonicalPath = fileInfo.absoluteFilePath();
    }
    
    m_fileIdToPath.insert(fileId, canonicalPath);
    m_pathToFileId.insert(canonicalPath, fileId);
    
    qDebug() << "LocalFileRepository: Registered received file" << fileId;
}

void LocalFileRepository::removeReceivedFileMapping(const QString& fileId) {
    QString path = m_fileIdToPath.value(fileId);
    if (!path.isEmpty()) {
        m_pathToFileId.remove(path);
    }
    m_fileIdToPath.remove(fileId);
    qDebug() << "LocalFileRepository: Removed mapping for fileId" << fileId;
}

void LocalFileRepository::removeFileMapping(const QString& fileId) {
    QString path = m_fileIdToPath.value(fileId);
    if (!path.isEmpty()) {
        m_pathToFileId.remove(path);
    }
    m_fileIdToPath.remove(fileId);
    qDebug() << "LocalFileRepository: Removed mapping for fileId" << fileId;
}

QList<QString> LocalFileRepository::getFileIdsUnderPathPrefix(const QString& pathPrefix) const {
    QList<QString> result;
    for (auto it = m_fileIdToPath.constBegin(); it != m_fileIdToPath.constEnd(); ++it) {
        if (it.value().startsWith(pathPrefix)) {
            result.append(it.key());
        }
    }
    return result;
}

void LocalFileRepository::clear() {
    qDebug() << "LocalFileRepository: Clearing all mappings";
    m_fileIdToPath.clear();
    m_pathToFileId.clear();
}
