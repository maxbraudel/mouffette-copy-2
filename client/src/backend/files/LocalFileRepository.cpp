#include "backend/files/LocalFileRepository.h"
#include <QFile>
#include <QDebug>
#include <algorithm>
#include <array>

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

    QCryptographicHash hash(QCryptographicHash::Sha256);
    QByteArray metadata = canonicalPath.toUtf8();
    metadata.append('\0');
    metadata.append(QByteArray::number(fileInfo.size()));
    metadata.append('\0');
    metadata.append(QByteArray::number(fileInfo.lastModified().toMSecsSinceEpoch()));
    metadata.append('\0');
    metadata.append(QByteArray::number(fileInfo.metadataChangeTime().toMSecsSinceEpoch()));
    hash.addData(metadata);

    QFile file(canonicalPath);
    if (file.open(QIODevice::ReadOnly)) {
        constexpr qint64 fullHashLimit = 64LL * 1024 * 1024;
        constexpr qint64 sampleSize = 64LL * 1024;
        const qint64 size = file.size();
        if (size <= fullHashLimit) {
            while (!file.atEnd()) {
                const QByteArray block = file.read(1024 * 1024);
                if (block.isEmpty() && file.error() != QFileDevice::NoError) break;
                hash.addData(block);
            }
        } else {
            // Avoid synchronously reading multi-gigabyte media on the UI
            // thread. Metadata plus evenly distributed content samples still
            // invalidates normal replacements while keeping import bounded.
            const std::array<qint64, 5> offsets = {
                0,
                size / 4,
                size / 2,
                (size * 3) / 4,
                std::max<qint64>(0, size - sampleSize)
            };
            for (const qint64 offset : offsets) {
                if (!file.seek(std::clamp<qint64>(offset, 0, std::max<qint64>(0, size - 1)))) break;
                hash.addData(file.read(sampleSize));
            }
        }
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
    auto it = m_pathToFileId.constFind(canonicalPath);
    if (it != m_pathToFileId.constEnd()) {
        if (it.value() == fileId) return fileId;
        const QString staleFileId = it.value();
        m_pathToFileId.remove(canonicalPath);
        if (m_fileIdToPath.value(staleFileId) == canonicalPath) {
            m_fileIdToPath.remove(staleFileId);
        }
        qDebug() << "LocalFileRepository: File content changed; rotating fileId for"
                 << canonicalPath;
    }

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
