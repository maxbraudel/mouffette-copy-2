#include "files/LocalFileRepository.h"
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
    
    QByteArray pathBytes = canonicalPath.toUtf8();
    QByteArray hash = QCryptographicHash::hash(pathBytes, QCryptographicHash::Sha256);
    return QString::fromLatin1(hash.toHex());
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
    
    // Check if already mapped
    auto it = m_pathToFileId.constFind(canonicalPath);
    if (it != m_pathToFileId.constEnd()) {
        return it.value();
    }
    
    // Generate new fileId
    QString fileId = generateFileId(canonicalPath);
    m_fileIdToPath.insert(fileId, canonicalPath);
    m_pathToFileId.insert(canonicalPath, fileId);
    
    qDebug() << "LocalFileRepository: Created fileId" << fileId << "for path" << canonicalPath;
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
    
    qDebug() << "LocalFileRepository: Registered received file" << fileId << "at" << canonicalPath;
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
