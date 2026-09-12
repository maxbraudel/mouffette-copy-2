#include "backend/network/UploadManager.h"
#include "backend/network/WebSocketClient.h"
#include "backend/network/RemoteSessionCoordinator.h"
#include "backend/config/AppConfig.h"
#include "backend/runtime/RuntimeProfile.h"
#include "backend/files/FileManager.h"
#include "backend/domain/media/MediaFilePolicy.h"
#include "backend/domain/session/SessionManager.h"  // Phase 3: For DEFAULT_IDEA_ID constant
#include <QGraphicsScene>
#include <QGraphicsItem>
#include <QFileInfo>
#include <QCryptographicHash>
#include <QJsonObject>
#include <QJsonArray>
#include <QMap>
#include <QDir>
#include <QTimer>
#include <QElapsedTimer>
#include <QDebug>
#include <QDateTime>
#include <QRegularExpression>
#include <QScopedValueRollback>
#include <QSet>
#include <algorithm>
#include <cmath>
#include <limits>

#ifndef Q_OS_WIN
#include <unistd.h>
#else
#include <io.h>
#endif

// Removed dependency on ResizableMediaBase / scene scanning.

namespace {

constexpr int kMaxIncomingFiles = 256;
constexpr qint64 kMaxIncomingFileBytes = 16LL * 1024 * 1024 * 1024;
constexpr qint64 kMaxIncomingUploadBytes = 64LL * 1024 * 1024 * 1024;
constexpr qsizetype kMaxIncomingChunkBytes = 128 * 1024;
constexpr qsizetype kMaxEncodedChunkCharacters = ((kMaxIncomingChunkBytes + 2) / 3) * 4;
constexpr qint64 kMaxQueuedUploadBytes = 2LL * 1024 * 1024;
constexpr qint64 kMaxUnacknowledgedRemoteBytes = 2LL * 1024 * 1024;
constexpr qint64 kIncomingProgressAckIntervalBytes = 512LL * 1024;
constexpr int kMaxChunksPerPump = 8;
constexpr int kMaxIncomingCompletionTombstones = 256;
constexpr qint64 kIncomingCompletionTombstoneTtlMs = 60'000;
const QString kRemovalQuarantinePrefix = QStringLiteral(".mouffette-removing-");

struct ValidatedManifestFile {
    QString assetId;
    QString fileId;
    QString sha256;
    QString name;
    QString extension;
    QStringList mediaIds;
    qint64 size = 0;
};

bool isCanonicalUuid(const QString& value) {
    static const QRegularExpression pattern(
        QStringLiteral("^[0-9a-fA-F]{8}-[0-9a-fA-F]{4}-[1-5][0-9a-fA-F]{3}-[89aAbB][0-9a-fA-F]{3}-[0-9a-fA-F]{12}$"));
    return pattern.match(value).hasMatch();
}

bool isValidFileId(const QString& value) {
    static const QRegularExpression pattern(QStringLiteral("^[0-9a-f]{64}$"));
    return pattern.match(value).hasMatch();
}

bool isValidPeerId(const QString& value) {
    static const QRegularExpression pattern(QStringLiteral("^[A-Za-z0-9_-]{1,128}$"));
    static const QRegularExpression windowsReserved(
        QStringLiteral("^(con|prn|aux|nul|com[1-9]|lpt[1-9])$"),
        QRegularExpression::CaseInsensitiveOption);
    return pattern.match(value).hasMatch()
        && !windowsReserved.match(value).hasMatch();
}

QString senderCacheNamespace(const QJsonObject& message) {
    // Protocol v3 supplies the authenticated owner as server-authored routing
    // metadata. Never revive the removed sender aliases from client payloads.
    return message.value("ownerEndpointId").toString();
}

bool isValidOpaqueId(const QString& value) {
    return isValidPeerId(value);
}

bool isAllowedMediaExtension(const QString& extension) {
    static const QSet<QString> allowed = {
        QStringLiteral("png"), QStringLiteral("jpg"), QStringLiteral("jpeg"),
        QStringLiteral("webp"), QStringLiteral("avif"), QStringLiteral("mp4")
    };
    return allowed.contains(extension.toLower());
}

bool isValidCanvasSessionId(const QString& value) {
    static const QRegularExpression pattern(QStringLiteral("^[A-Za-z0-9_-]{1,512}$"));
    return pattern.match(value).hasMatch();
}

bool isSafeDisplayName(const QString& value) {
    if (value.isEmpty() || value.size() > 255 || value == QLatin1String(".") || value == QLatin1String("..")
        || value.contains(QLatin1Char('/')) || value.contains(QLatin1Char('\\'))) {
        return false;
    }
    for (const QChar ch : value) {
        const ushort code = ch.unicode();
        if (code < 0x20 || code == 0x7f) return false;
    }
    return true;
}

bool normalizeAndValidateExtension(const QString& value, QString& normalized) {
    if (value != value.trimmed()) return false;
    normalized = value.toLower();
    if (normalized.isEmpty()) return true;
    static const QRegularExpression pattern(QStringLiteral("^[a-z0-9]{1,16}$"));
    return pattern.match(normalized).hasMatch();
}

bool parseManifestSize(const QJsonValue& value, qint64& size) {
    if (!value.isDouble()) return false;
    const double raw = value.toDouble(-1.0);
    if (!std::isfinite(raw) || raw < 1.0 || raw > static_cast<double>(kMaxIncomingFileBytes)
        || std::floor(raw) != raw) {
        return false;
    }
    size = static_cast<qint64>(raw);
    return true;
}

bool parsePositiveGeneration(const QJsonValue& value, quint64& generation) {
    if (!value.isDouble()) return false;
    const double raw = value.toDouble(-1.0);
    if (!std::isfinite(raw) || raw < 1.0
        || raw > 9007199254740991.0 || std::floor(raw) != raw) {
        return false;
    }
    generation = static_cast<quint64>(raw);
    return true;
}

bool parseNonNegativeOffset(const QJsonValue& value, qint64& offset) {
    if (!value.isDouble()) return false;
    const double raw = value.toDouble(-1.0);
    if (!std::isfinite(raw) || raw < 0.0
        || raw > static_cast<double>(std::numeric_limits<qint64>::max())
        || std::floor(raw) != raw) {
        return false;
    }
    offset = static_cast<qint64>(raw);
    return true;
}

bool syncFile(QFile* file) {
    if (!file || !file->isOpen() || !file->flush()) return false;
    const qintptr handle = file->handle();
    if (handle < 0) return false;
#ifdef Q_OS_WIN
    return ::_commit(static_cast<int>(handle)) == 0;
#else
    return ::fsync(static_cast<int>(handle)) == 0;
#endif
}

QString sha256ForFile(const QString& path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return {};
    QCryptographicHash hash(QCryptographicHash::Sha256);
    while (!file.atEnd()) {
        const QByteArray block = file.read(1024 * 1024);
        if (block.isEmpty() && file.error() != QFileDevice::NoError) return {};
        hash.addData(QByteArrayView(block));
    }
    return QString::fromLatin1(hash.result().toHex());
}

bool parseChunkIndex(const QJsonValue& value, int& chunkIndex) {
    if (!value.isDouble()) return false;
    const double raw = value.toDouble(-1.0);
    if (!std::isfinite(raw) || raw < 0.0
        || raw > static_cast<double>(std::numeric_limits<int>::max())
        || std::floor(raw) != raw) {
        return false;
    }
    chunkIndex = static_cast<int>(raw);
    return true;
}

bool decodeCanonicalChunk(const QJsonValue& value, QByteArray& decoded) {
    if (!value.isString()) return false;
    const QString encodedText = value.toString();
    if (encodedText.isEmpty() || encodedText.size() > kMaxEncodedChunkCharacters) return false;
    const QByteArray encoded = encodedText.toLatin1();
    if (QString::fromLatin1(encoded) != encodedText) return false;
    decoded = QByteArray::fromBase64(encoded, QByteArray::AbortOnBase64DecodingErrors);
    return !decoded.isEmpty()
        && decoded.size() <= kMaxIncomingChunkBytes
        && decoded.toBase64() == encoded;
}

bool filesHaveIdenticalContents(const QString& firstPath, const QString& secondPath) {
    QFile first(firstPath);
    QFile second(secondPath);
    if (!first.open(QIODevice::ReadOnly) || !second.open(QIODevice::ReadOnly)
        || first.size() != second.size()) {
        return false;
    }

    constexpr qint64 comparisonBlockSize = 1024 * 1024;
    while (!first.atEnd() && !second.atEnd()) {
        const QByteArray firstBlock = first.read(comparisonBlockSize);
        const QByteArray secondBlock = second.read(comparisonBlockSize);
        if (firstBlock != secondBlock
            || (firstBlock.isEmpty() && first.error() != QFileDevice::NoError)
            || (secondBlock.isEmpty() && second.error() != QFileDevice::NoError)) {
            return false;
        }
    }
    return first.atEnd() && second.atEnd()
        && first.error() == QFileDevice::NoError
        && second.error() == QFileDevice::NoError;
}

QString incomingUploadsRoot() {
    const QString base = RuntimeProfile::cacheLocation();
    const QString path = QFileInfo(QDir(base).filePath(QStringLiteral("Mouffette/Uploads"))).absoluteFilePath();
    const QString canonicalPath = QFileInfo(path).canonicalFilePath();
    return canonicalPath.isEmpty() ? path : canonicalPath;
}

bool pathIsInsideDirectory(const QString& path, const QString& directory) {
    const QString cleanPath = QDir::cleanPath(QFileInfo(path).absoluteFilePath());
    QString cleanDirectory = QDir::cleanPath(QFileInfo(directory).absoluteFilePath());
    if (!cleanDirectory.endsWith(QDir::separator())) cleanDirectory += QDir::separator();
    return cleanPath.startsWith(cleanDirectory) && cleanPath != cleanDirectory;
}

void removeEmptyUploadParentsForFile(const QString& filePath) {
    if (filePath.isEmpty()) return;

    const QString rootPath = incomingUploadsRoot();
    const QString uploadDirectoryPath = QFileInfo(filePath).absolutePath();
    if (!pathIsInsideDirectory(uploadDirectoryPath, rootPath)) return;

    QDir uploadDirectory(uploadDirectoryPath);
    if (!uploadDirectory.exists()
        || !uploadDirectory.entryInfoList(QDir::AllEntries | QDir::NoDotAndDotDot).isEmpty()
        || !QDir().rmdir(uploadDirectoryPath)) {
        return;
    }

    const QString senderDirectoryPath = QFileInfo(uploadDirectoryPath).absolutePath();
    if (!pathIsInsideDirectory(senderDirectoryPath, rootPath)) return;

    QDir senderDirectory(senderDirectoryPath);
    if (senderDirectory.exists()
        && senderDirectory.entryInfoList(QDir::AllEntries | QDir::NoDotAndDotDot).isEmpty()) {
        QDir().rmdir(senderDirectoryPath);
    }
}

} // namespace

UploadManager::UploadManager(FileManager* fileManager,
                             QObject* parent,
                             const QString& remoteCacheRoot)
    : QObject(parent), m_fileManager(fileManager) {
    m_lastAcceptedAction.invalidate();
    m_outgoingStateAge.start();

    m_outgoingPumpTimer = new QTimer(this);
    m_outgoingPumpTimer->setSingleShot(true);
    m_outgoingPumpTimer->setInterval(0);
    connect(m_outgoingPumpTimer, &QTimer::timeout,
            this, &UploadManager::pumpOutgoingUpload);

    m_outgoingStallTimer = new QTimer(this);
    m_outgoingStallTimer->setSingleShot(true);
    m_outgoingStallTimer->setInterval(0);
    connect(m_outgoingStallTimer, &QTimer::timeout, this, [this]() {
        if (m_outgoingState == OutgoingState::Streaming
            && !m_currentUploadId.isEmpty()) {
            failOutgoingUpload(QStringLiteral("Upload transport stalled"));
        }
    });

    m_outgoingStartAckTimer = new QTimer(this);
    m_outgoingStartAckTimer->setSingleShot(true);
    m_outgoingStartAckTimer->setInterval(0);
    connect(m_outgoingStartAckTimer, &QTimer::timeout, this, [this]() {
        if (m_outgoingState == OutgoingState::AwaitingTargetReady
            && !m_currentUploadId.isEmpty()) {
            failOutgoingUpload(QStringLiteral(
                "Remote client did not accept the upload in time"));
        }
    });

    m_outgoingAckTimer = new QTimer(this);
    m_outgoingAckTimer->setSingleShot(true);
    m_outgoingAckTimer->setInterval(0);
    connect(m_outgoingAckTimer, &QTimer::timeout, this, [this]() {
        if (m_outgoingState == OutgoingState::AwaitingValidation
            && !m_currentUploadId.isEmpty()) {
            failOutgoingUpload(QStringLiteral(
                "Remote client did not validate the upload in time"));
        }
    });

    m_incomingStallTimer = new QTimer(this);
    m_incomingStallTimer->setSingleShot(true);
    m_incomingStallTimer->setInterval(0);
    connect(m_incomingStallTimer, &QTimer::timeout, this, [this]() {
        if (m_incoming.uploadId.isEmpty()) return;
        rejectIncomingUpload(m_incoming.senderId, m_incoming.uploadId,
                             QStringLiteral("Incoming upload stalled"), true);
    });

    m_remoteCacheStore = new RemoteCacheStore(remoteCacheRoot, this);
    QString cacheError;
    m_remoteCacheReady = m_remoteCacheStore->initialize(&cacheError);
    QString logicalCleanupError;
    m_receiverAdvertisementReady = m_remoteCacheReady
        && m_remoteCacheStore->receiverAdvertisementSafe(&logicalCleanupError);
    m_receiverCleanupError = m_remoteCacheReady
        ? (m_receiverAdvertisementReady ? QString() : logicalCleanupError)
        : (cacheError.isEmpty()
               ? QStringLiteral("cache_store_not_initialized") : cacheError);
    if (!m_remoteCacheReady) {
        qCritical() << "UploadManager: remote cache initialization failed:" << cacheError;
    }
    connect(m_remoteCacheStore, &RemoteCacheStore::physicalCleanupFailed,
            this, [this](const QString& senderEndpointId,
                         const QString& remoteSessionId,
                         quint64 generation,
                         const QString& teardownId,
                         const QString& errorCode) {
        emit remoteSessionCacheCleanupError(senderEndpointId, remoteSessionId,
                                            generation, teardownId, errorCode);
    });
    cleanupOrphanedIncomingCache();

    m_uploadScheduler = new UploadScheduler(
        AppConfig::instance().uploadConcurrency(), this);
    connect(m_uploadScheduler, &UploadScheduler::uploadStartRequested,
            this, &UploadManager::startScheduledUpload);
}

UploadManager::~UploadManager() {
    if (m_ws && m_outgoingTransportRegistered) {
        m_ws->endUploadSession();
        m_outgoingTransportRegistered = false;
    }
    const auto transfers = m_parallelOutgoingByUpload.values();
    m_parallelOutgoingByUpload.clear();
    m_parallelUploadBySession.clear();
    for (ParallelOutgoingTransfer* transfer : transfers) {
        if (!transfer) continue;
        if (m_ws && transfer->transportRegistered) m_ws->endUploadSession();
        if (transfer->fileHandle.isOpen()) transfer->fileHandle.close();
        delete transfer;
    }
}

UploadManager::ParallelOutgoingTransfer*
UploadManager::parallelForUpload(const QString& uploadId) const {
    return m_parallelOutgoingByUpload.value(uploadId, nullptr);
}

UploadManager::ParallelOutgoingTransfer*
UploadManager::parallelForSession(const QString& remoteSessionId) const {
    return parallelForUpload(m_parallelUploadBySession.value(remoteSessionId));
}

UploadManager::ParallelOutgoingTransfer*
UploadManager::parallelForTarget(const QString& targetEndpointId) const {
    if (targetEndpointId.isEmpty()) return nullptr;
    for (ParallelOutgoingTransfer* transfer : m_parallelOutgoingByUpload) {
        if (transfer && transfer->targetEndpointId == targetEndpointId) return transfer;
    }
    return nullptr;
}

QString UploadManager::activeUploadTargetClientId() const {
    if (ParallelOutgoingTransfer* transfer = parallelForTarget(m_targetClientId)) {
        return transfer->targetEndpointId;
    }
    if (!m_uploadTargetClientId.isEmpty()) return m_uploadTargetClientId;
    return m_targetClientId;
}

bool UploadManager::hasActiveUpload() const {
    return !m_targetClientId.isEmpty()
        ? m_remoteInventoryTargets.contains(m_targetClientId)
        : (m_uploadActive || !m_remoteInventoryTargets.isEmpty());
}

UploadManager::OutgoingState UploadManager::outgoingState() const {
    if (ParallelOutgoingTransfer* transfer = parallelForTarget(m_targetClientId)) {
        return transfer->state;
    }
    if (m_targetClientId.isEmpty() || m_uploadTargetClientId == m_targetClientId) {
        return m_outgoingState;
    }
    return OutgoingState::Idle;
}

bool UploadManager::isUploading() const {
    const OutgoingState state = outgoingState();
    return state == OutgoingState::Queued
        || state == OutgoingState::AwaitingTargetReady
        || state == OutgoingState::Streaming
        || state == OutgoingState::Suspended;
}

bool UploadManager::isCancelling() const {
    return outgoingState() == OutgoingState::Cancelling;
}

bool UploadManager::isFinalizing() const {
    return outgoingState() == OutgoingState::AwaitingValidation;
}

bool UploadManager::isBusy() const {
    return outgoingState() != OutgoingState::Idle || isRemoving();
}

QString UploadManager::currentUploadId() const {
    if (ParallelOutgoingTransfer* transfer = parallelForTarget(m_targetClientId)) {
        return transfer->uploadId;
    }
    return (m_targetClientId.isEmpty() || m_uploadTargetClientId == m_targetClientId)
        ? m_currentUploadId : QString();
}

QString UploadManager::currentRemoteSessionId() const {
    if (ParallelOutgoingTransfer* transfer = parallelForTarget(m_targetClientId)) {
        return transfer->remoteSessionId;
    }
    return (m_targetClientId.isEmpty() || m_uploadTargetClientId == m_targetClientId)
        ? m_outgoingRemoteSessionId : QString();
}

quint64 UploadManager::currentRemoteSessionGeneration() const {
    if (ParallelOutgoingTransfer* transfer = parallelForTarget(m_targetClientId)) {
        return transfer->generation;
    }
    return (m_targetClientId.isEmpty() || m_uploadTargetClientId == m_targetClientId)
        ? m_outgoingGeneration : 0;
}

int UploadManager::activeOutgoingTransferCount() const {
    return (m_currentUploadId.isEmpty() ? 0 : 1)
        + m_parallelOutgoingByUpload.size();
}

void UploadManager::setOutgoingState(OutgoingState state) {
    if (m_outgoingState == state) return;
    m_outgoingState = state;
    m_outgoingStateAge.restart();
}

void UploadManager::cleanupOrphanedIncomingCache() {
    if (!m_fileManager) return;
    const QString rootPath = m_remoteCacheStore
        ? m_remoteCacheStore->rootPath() : incomingUploadsRoot();
    const QFileInfo rootInfo(rootPath);
    if (!rootInfo.exists()) return;
    if (!rootInfo.isDir() || rootInfo.isSymLink()) {
        qWarning() << "UploadManager: refusing unsafe orphan-cache root";
        return;
    }

    QSet<QString> trackedPaths;
    for (const QString& fileId : m_fileManager->getAllFileIds()) {
        const QString path = m_fileManager->getFilePathForId(fileId);
        if (!path.isEmpty() && pathIsInsideDirectory(path, rootPath)) {
            trackedPaths.insert(QDir::cleanPath(QFileInfo(path).absoluteFilePath()));
        }
    }
    const auto hasTrackedPathUnder = [&trackedPaths](const QString& directory) {
        for (const QString& trackedPath : trackedPaths) {
            if (pathIsInsideDirectory(trackedPath, directory)) return true;
        }
        return false;
    };
    const auto originalQuarantineName = [](const QString& name) {
        if (!name.startsWith(kRemovalQuarantinePrefix)) return QString();
        const qsizetype uuidStart = kRemovalQuarantinePrefix.size();
        constexpr qsizetype uuidLength = 36;
        if (name.size() <= uuidStart + uuidLength
            || name.at(uuidStart + uuidLength) != QLatin1Char('-')
            || QUuid(name.mid(uuidStart, uuidLength)).isNull()) {
            return QString();
        }
        return name.mid(uuidStart + uuidLength + 1);
    };

    QDir root(rootPath);
    const QFileInfoList senderEntries = root.entryInfoList(
        QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System);
    for (const QFileInfo& senderEntry : senderEntries) {
        if (senderEntry.fileName() == QLatin1String(".remote-cache-state")
            || senderEntry.fileName() == QLatin1String(".quarantine")) {
            continue;
        }
        if (senderEntry.isSymLink()) {
            qWarning() << "UploadManager: refusing legacy cache symlink";
            continue;
        }
        if (!senderEntry.isDir()) continue;

        const QString restoredSenderName = originalQuarantineName(senderEntry.fileName());
        if (!restoredSenderName.isEmpty()) {
            const QString originalSenderPath = root.absoluteFilePath(restoredSenderName);
            if (hasTrackedPathUnder(originalSenderPath)
                && !QFileInfo::exists(originalSenderPath)) {
                if (!root.rename(senderEntry.fileName(), restoredSenderName)) {
                    qCritical() << "UploadManager: could not restore interrupted legacy cache removal";
                }
            } else if (!QDir(senderEntry.absoluteFilePath()).removeRecursively()) {
                qWarning() << "UploadManager: could not purge stale legacy cache quarantine";
            }
            continue;
        }

        QDir senderDirectory(senderEntry.absoluteFilePath());
        const QFileInfoList uploadEntries = senderDirectory.entryInfoList(
            QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System);
        for (const QFileInfo& uploadEntry : uploadEntries) {
            if (uploadEntry.isSymLink()) {
                qWarning() << "UploadManager: refusing legacy upload symlink";
                continue;
            }
            if (!uploadEntry.isDir()) continue;
            // Protocol-v3 scopes are owned exclusively by RemoteCacheStore's
            // intent/tombstone transaction. In particular, never let this
            // legacy orphan sweep recursively delete a live scope whose atomic
            // quarantine is blocked: doing so would erase the evidence while
            // incorrectly making receiver advertisement appear safe.
            if (QFileInfo::exists(QDir(uploadEntry.absoluteFilePath())
                                      .filePath(QStringLiteral(".scope.json")))) {
                continue;
            }
            if (!hasTrackedPathUnder(uploadEntry.absoluteFilePath())) {
                if (!QDir(uploadEntry.absoluteFilePath()).removeRecursively()) {
                    qWarning() << "UploadManager: could not purge orphaned legacy upload staging";
                }
                continue;
            }

            QDir uploadDirectory(uploadEntry.absoluteFilePath());
            const QFileInfoList cachedFiles = uploadDirectory.entryInfoList(
                QDir::Files | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System);
            for (const QFileInfo& cachedFile : cachedFiles) {
                const QString originalFileName = originalQuarantineName(cachedFile.fileName());
                if (originalFileName.isEmpty()) continue;
                const QString originalPath = uploadDirectory.absoluteFilePath(originalFileName);
                const QString cleanOriginalPath = QDir::cleanPath(
                    QFileInfo(originalPath).absoluteFilePath());
                if (trackedPaths.contains(cleanOriginalPath)
                    && !QFileInfo::exists(originalPath)) {
                    if (!uploadDirectory.rename(cachedFile.fileName(), originalFileName)) {
                        qCritical() << "UploadManager: could not restore interrupted legacy file removal";
                    }
                } else if (!QFile::remove(cachedFile.absoluteFilePath())) {
                    qWarning() << "UploadManager: could not purge stale legacy file quarantine";
                }
            }
        }
        if (senderDirectory.entryInfoList(QDir::AllEntries | QDir::NoDotAndDotDot
                                          | QDir::Hidden | QDir::System).isEmpty()) {
            root.rmdir(senderEntry.fileName());
        }
    }
}

void UploadManager::setWebSocketClient(WebSocketClient* client) {
    QObject::disconnect(m_uploadBytesWrittenConnection);
    QObject::disconnect(m_uploadTransportLostConnection);
    for (const QMetaObject::Connection& connection : std::as_const(m_webSocketConnections)) {
        QObject::disconnect(connection);
    }
    m_webSocketConnections.clear();
    if (m_ws) {
        QObject::disconnect(this, &UploadManager::protocolV3UploadResponseReady,
                            m_ws, &WebSocketClient::sendUploadProtocolResponse);
    }
    m_ws = client;
    if (!client) return;

    m_uploadBytesWrittenConnection = connect(
        client, &WebSocketClient::uploadTransportBytesWritten,
        this, [this](qint64) {
            if (m_outgoingState == OutgoingState::Streaming
                && m_outgoingStallTimer && m_outgoingStallTimer->interval() > 0) {
                m_outgoingStallTimer->start();
            }
            if (m_outgoingState == OutgoingState::Streaming) scheduleOutgoingPump();
            for (ParallelOutgoingTransfer* transfer : m_parallelOutgoingByUpload) {
                if (!transfer || transfer->state != OutgoingState::Streaming) continue;
                if (transfer->stallTimer && transfer->stallTimer->interval() > 0) {
                    transfer->stallTimer->start();
                }
                scheduleParallelPump(transfer);
            }
        });
    m_uploadTransportLostConnection = connect(
        client, &WebSocketClient::uploadTransportLost,
        this, [this](const QString& reason) {
            if (m_outgoingState != OutgoingState::Idle
                && m_outgoingState != OutgoingState::Cancelling
                && !m_currentUploadId.isEmpty()) {
                suspendOutgoingForResume(reason);
            }
            const auto transfers = m_parallelOutgoingByUpload.values();
            for (ParallelOutgoingTransfer* transfer : transfers) {
                if (transfer && transfer->state != OutgoingState::Cancelling) {
                    suspendParallel(transfer);
                }
            }
            // Losing only the optional payload socket is not a RemoteSession
            // failure. Rebind this upload to the authenticated control socket
            // immediately, using the durable target offsets as the authority.
            QTimer::singleShot(0, this, [this]() {
                if (m_outgoingState == OutgoingState::Suspended && m_ws
                    && m_ws->isConnected()) {
                    resumeOutgoingUpload();
                }
                const auto transfers = m_parallelOutgoingByUpload.values();
                for (ParallelOutgoingTransfer* transfer : transfers) {
                    if (transfer && transfer->state == OutgoingState::Suspended
                        && m_ws && m_ws->isConnected()) {
                        resumeParallel(transfer);
                    }
                }
            });
        });

    m_webSocketConnections.append(connect(
        client, &WebSocketClient::uploadMessageReceived,
        this, &UploadManager::handleUploadProtocolMessage));
    m_webSocketConnections.append(connect(
        client, &WebSocketClient::disconnected,
        this, &UploadManager::onConnectionLost));
    m_webSocketConnections.append(connect(
        client, &WebSocketClient::remoteSessionOpened,
        this, &UploadManager::applyRemoteSessionEnvelope));
    m_webSocketConnections.append(connect(
        client, &WebSocketClient::remoteSessionResumed,
        this, &UploadManager::applyRemoteSessionEnvelope));
    m_webSocketConnections.append(connect(
        client, &WebSocketClient::remoteSessionLeaseStateChanged,
        this, &UploadManager::applyRemoteSessionEnvelope));
    m_webSocketConnections.append(connect(
        client, &WebSocketClient::remoteSessionTerminating,
        this, &UploadManager::applyRemoteSessionEnvelope));
    m_webSocketConnections.append(connect(
        client, &WebSocketClient::remoteSessionClosed,
        this, &UploadManager::applyRemoteSessionEnvelope));
    m_webSocketConnections.append(connect(
        client, &WebSocketClient::leaseExpired,
        this, [this](const QString&, quint64) {
            QSet<QString> outgoingSessions;
            if (!m_outgoingRemoteSessionId.isEmpty()) {
                outgoingSessions.insert(m_outgoingRemoteSessionId);
            }
            for (ParallelOutgoingTransfer* transfer : m_parallelOutgoingByUpload) {
                if (transfer) outgoingSessions.insert(transfer->remoteSessionId);
            }
            for (const QString& sessionId : outgoingSessions) {
                terminateRemoteSessionUpload(
                    sessionId, QStringLiteral("Remote session lease expired"));
            }
            beginTerminalIncomingCleanup(QStringLiteral("lease_expired"));
            emit terminalIncomingCleanupRequired(
                QStringLiteral("lease_expired"));
        }));
    m_webSocketConnections.append(connect(
        client, &WebSocketClient::serverRestarted,
        this, [this](const QString&, const QString&) {
            beginTerminalIncomingCleanup(QStringLiteral("server_restart"));
            emit terminalIncomingCleanupRequired(
                QStringLiteral("server_restart"));
        }));
    m_webSocketConnections.append(connect(
        client, &WebSocketClient::serverPolicyReceived,
        this, [this](const QJsonObject& policy) {
            const int idleTimeout = policy.value(QStringLiteral("uploadIdleTimeoutMs")).toInt();
            const int targetAckTimeout =
                policy.value(QStringLiteral("uploadTargetAckTimeoutMs")).toInt();
            if (idleTimeout > 0) {
                m_outgoingStallTimer->setInterval(idleTimeout);
                m_incomingStallTimer->setInterval(idleTimeout);
                for (ParallelOutgoingTransfer* transfer : m_parallelOutgoingByUpload) {
                    if (transfer && transfer->stallTimer) {
                        transfer->stallTimer->setInterval(idleTimeout);
                    }
                }
            }
            if (targetAckTimeout > 0) {
                m_outgoingStartAckTimer->setInterval(targetAckTimeout);
                m_outgoingAckTimer->setInterval(targetAckTimeout);
                for (ParallelOutgoingTransfer* transfer : m_parallelOutgoingByUpload) {
                    if (!transfer) continue;
                    if (transfer->startAckTimer) {
                        transfer->startAckTimer->setInterval(targetAckTimeout);
                    }
                    if (transfer->ackTimer) {
                        transfer->ackTimer->setInterval(targetAckTimeout);
                    }
                }
            }
        }));
    connect(this, &UploadManager::protocolV3UploadResponseReady,
            client, &WebSocketClient::sendUploadProtocolResponse,
            Qt::UniqueConnection);

    const QJsonObject policy = client->serverPolicy();
    if (!policy.isEmpty()) {
        const int idleTimeout = policy.value(QStringLiteral("uploadIdleTimeoutMs")).toInt();
        const int targetAckTimeout =
            policy.value(QStringLiteral("uploadTargetAckTimeoutMs")).toInt();
        if (idleTimeout > 0) {
            m_outgoingStallTimer->setInterval(idleTimeout);
            m_incomingStallTimer->setInterval(idleTimeout);
            for (ParallelOutgoingTransfer* transfer : m_parallelOutgoingByUpload) {
                if (transfer && transfer->stallTimer) {
                    transfer->stallTimer->setInterval(idleTimeout);
                }
            }
        }
        if (targetAckTimeout > 0) {
            m_outgoingStartAckTimer->setInterval(targetAckTimeout);
            m_outgoingAckTimer->setInterval(targetAckTimeout);
            for (ParallelOutgoingTransfer* transfer : m_parallelOutgoingByUpload) {
                if (!transfer) continue;
                if (transfer->startAckTimer) {
                    transfer->startAckTimer->setInterval(targetAckTimeout);
                }
                if (transfer->ackTimer) {
                    transfer->ackTimer->setInterval(targetAckTimeout);
                }
            }
        }
    }
}
void UploadManager::setTargetClientId(const QString& id) {
    if (m_targetClientId == id) return;
    m_targetClientId = id;
    m_uploadActive = hasActiveUpload();
    emit uiStateChanged();
}

void UploadManager::forceResetForClient(const QString& clientId) {
    const auto parallelTransfers = m_parallelOutgoingByUpload.values();
    for (ParallelOutgoingTransfer* transfer : parallelTransfers) {
        if (!transfer || (!clientId.isEmpty()
                          && transfer->targetEndpointId != clientId)) continue;
        if (m_uploadScheduler) {
            m_uploadScheduler->cancelUpload(transfer->remoteSessionId,
                                            transfer->schedulerGeneration,
                                            transfer->uploadId);
        }
        removeParallel(transfer, false, false);
    }
    if (clientId.isEmpty()) {
        m_remoteInventoryTargets.clear();
        m_committedAssetsByTarget.clear();
        m_pendingAssetRemovals.clear();
    } else {
        m_remoteInventoryTargets.remove(clientId);
        m_committedAssetsByTarget.remove(clientId);
        const QStringList pendingIds = m_pendingAssetRemovals.keys();
        for (const QString& removalId : pendingIds) {
            if (m_pendingAssetRemovals.value(removalId).asset.targetEndpointId
                == clientId) {
                m_pendingAssetRemovals.remove(removalId);
            }
        }
    }
    if (!clientId.isEmpty()) {
        const bool matchesPrimary = !m_uploadTargetClientId.isEmpty()
            && m_uploadTargetClientId == clientId;
        const bool matchesLegacyIdleProjection = m_uploadTargetClientId.isEmpty()
            && m_targetClientId == clientId
            && m_outgoingState == OutgoingState::Idle;
        if (!matchesPrimary && !matchesLegacyIdleProjection) {
            m_uploadActive = hasActiveUpload();
            emit uiStateChanged();
            return;
        }
    }

    resetToInitial();
    emit uiStateChanged();
}

bool UploadManager::toggleUpload(const QVector<UploadFileInfo>& files) {
    if (!m_ws || !m_ws->isConnected() || m_targetClientId.isEmpty()) {
        qWarning() << "UploadManager: Not connected or no target set";
        return false;
    }
    
    // Anti-spam protection: check if we can accept a new action
    if (!canAcceptNewAction()) {
        qInfo() << "UploadManager: Action ignored due to rate limiting";
        return false;
    }
    const bool selectedAssetRemovalPending = std::any_of(
        m_pendingAssetRemovals.cbegin(), m_pendingAssetRemovals.cend(),
        [this](const PendingAssetRemoval& removal) {
            return removal.asset.targetEndpointId == m_targetClientId;
        });
    if (selectedAssetRemovalPending) {
        qInfo() << "UploadManager: Remote removal acknowledgement pending; toggle ignored";
        return false;
    }
    if ((m_uploadTargetClientId == m_targetClientId
         && m_outgoingState != OutgoingState::Idle)
        || parallelForTarget(m_targetClientId)) {
        qInfo() << "UploadManager: Transfer already active; duplicate action ignored";
        return false;
    }
    
    if (hasActiveUpload()) {
        // If active state but we are provided with additional files, start a new upload for them
        if (!files.isEmpty()) {
            startUpload(files);
            return currentUploadId().isEmpty() == false;
        }
        return false;
    }
    if (files.isEmpty()) {
        qInfo() << "UploadManager: No files provided";
        return false;
    }
    startUpload(files);
    return currentUploadId().isEmpty() == false;
}

bool UploadManager::requestAssetRemoval(const QString& targetEndpointId,
                                        const QString& localFileId,
                                        const QString& reason) {
    if (!m_ws || !m_fileManager || targetEndpointId.isEmpty()
        || localFileId.isEmpty()) {
        return false;
    }

    RemoteSessionCoordinator* sessions = m_ws->remoteSessionCoordinator();
    const RemoteSessionCoordinator::Binding binding = sessions
        ? sessions->outgoingForPeer(targetEndpointId)
        : RemoteSessionCoordinator::Binding();

    // Once a RemoteSession is gone, its target-side teardown owns the purge.
    // Clear stale local bookkeeping without attempting a target-addressed
    // command that could accidentally affect a later session.
    if (binding.remoteSessionId.isEmpty()) {
        m_fileManager->unmarkFileUploadedToClient(localFileId, targetEndpointId);
        m_committedAssetsByTarget.remove(targetEndpointId);
        m_remoteInventoryTargets.remove(targetEndpointId);
        emit assetRemovalCommitted(targetEndpointId, {localFileId});
        emit uiStateChanged();
        return true;
    }
    const bool resumableGrace = binding.phase == QLatin1String("Grace");
    if ((!binding.active && !resumableGrace)
        || binding.ownerEndpointId != m_ws->endpointId()) {
        emit assetRemovalFailed(targetEndpointId, binding.remoteSessionId,
                                {localFileId},
                                QStringLiteral("The remote session is not active"));
        return false;
    }

    // An external source deletion wins over an in-flight transfer. Abort it
    // first and close the whole session through the caller: until a complete
    // target ACK arrives there is no durable inventory tuple safe to remove.
    if (ParallelOutgoingTransfer* transfer = parallelForTarget(targetEndpointId)) {
        const bool containsFile = std::any_of(
            transfer->assets.cbegin(), transfer->assets.cend(),
            [&localFileId](const OutgoingAsset& asset) {
                return asset.localFileIds.contains(localFileId);
            });
        if (containsFile) {
            failParallel(transfer, QStringLiteral("Source asset was removed"));
            emit assetRemovalFailed(targetEndpointId, binding.remoteSessionId,
                                    {localFileId},
                                    QStringLiteral("Source changed during upload"));
            return false;
        }
    }
    if (m_uploadTargetClientId == targetEndpointId
        && !m_currentUploadId.isEmpty()) {
        const bool containsFile = std::any_of(
            m_outgoingAssets.cbegin(), m_outgoingAssets.cend(),
            [&localFileId](const OutgoingAsset& asset) {
                return asset.localFileIds.contains(localFileId);
            });
        if (containsFile) {
            failOutgoingUpload(QStringLiteral("Source asset was removed"));
            emit assetRemovalFailed(targetEndpointId, binding.remoteSessionId,
                                    {localFileId},
                                    QStringLiteral("Source changed during upload"));
            return false;
        }
    }

    CommittedRemoteAsset asset;
    if (!findCommittedAsset(targetEndpointId, localFileId, &asset)) {
        if (!m_fileManager->isFileUploadedToClient(localFileId,
                                                   targetEndpointId)) {
            return true;
        }
        emit assetRemovalFailed(targetEndpointId, binding.remoteSessionId,
                                {localFileId},
                                QStringLiteral("Validated remote inventory is unavailable"));
        return false;
    }
    QStringList survivingAliases;
    for (const QString& aliasFileId : asset.localFileIds) {
        if (aliasFileId == localFileId) continue;
        if (!m_fileManager->getMediaIdsForFile(aliasFileId).isEmpty()
            || !m_fileManager->getIdeaIdsForFile(aliasFileId).isEmpty()) {
            survivingAliases.append(aliasFileId);
        }
    }
    if (!survivingAliases.isEmpty()) {
        auto target = m_committedAssetsByTarget.find(targetEndpointId);
        if (target != m_committedAssetsByTarget.end()) {
            auto stored = target->find(asset.assetId);
            if (stored != target->end()) {
                stored->localFileIds.removeAll(localFileId);
            }
        }
        m_fileManager->unmarkFileUploadedToClient(localFileId, targetEndpointId);
        emit assetRemovalCommitted(targetEndpointId, {localFileId});
        return true;
    }
    asset.generation = binding.generation;
    if (asset.remoteSessionId != binding.remoteSessionId) {
        emit assetRemovalFailed(targetEndpointId, binding.remoteSessionId,
                                asset.localFileIds,
                                QStringLiteral("Validated remote inventory belongs to an expired session"));
        return false;
    }
    for (auto it = m_pendingAssetRemovals.cbegin();
         it != m_pendingAssetRemovals.cend(); ++it) {
        if (it->asset.targetEndpointId == targetEndpointId
            && it->asset.assetId == asset.assetId) {
            return true;
        }
    }

    const int timeoutMs = m_ws->serverPolicy()
        .value(QStringLiteral("removalAckTimeoutMs")).toInt();
    if (timeoutMs <= 0) {
        emit assetRemovalFailed(targetEndpointId, binding.remoteSessionId,
                                asset.localFileIds,
                                QStringLiteral("Server removal policy is unavailable"));
        return false;
    }
    const QString removalId =
        QUuid::createUuid().toString(QUuid::WithoutBraces).toLower();
    PendingAssetRemoval pending;
    pending.removalId = removalId;
    pending.asset = asset;
    pending.reason = reason.trimmed().isEmpty()
        ? QStringLiteral("source_removed") : reason.trimmed().left(128);
    m_pendingAssetRemovals.insert(removalId, pending);
    if (binding.active) {
        // A transport can disappear between reading the binding and sending.
        // Keep the immutable intent alive until its original timeout; a
        // signed session resume will replay the same removalId and tuple.
        auto stored = m_pendingAssetRemovals.find(removalId);
        if (stored != m_pendingAssetRemovals.end()) {
            sendPendingAssetRemoval(stored.value());
        }
    }
    QTimer::singleShot(timeoutMs, this, [this, removalId]() {
        if (m_pendingAssetRemovals.contains(removalId)) {
            failAssetRemoval(removalId,
                             QStringLiteral("Remote asset removal confirmation timed out"));
        }
    });
    emit uiStateChanged();
    return true;
}

bool UploadManager::sendPendingAssetRemoval(PendingAssetRemoval& pending) {
    if (!m_ws || pending.removalId.isEmpty()) return false;
    RemoteSessionCoordinator* sessions = m_ws->remoteSessionCoordinator();
    const RemoteSessionCoordinator::Binding binding = sessions
        ? sessions->byId(pending.asset.remoteSessionId)
        : RemoteSessionCoordinator::Binding();
    if (!binding.active || binding.ownerEndpointId != m_ws->endpointId()
        || binding.remoteSessionId != pending.asset.remoteSessionId
        || binding.generation < 1) {
        return false;
    }
    pending.asset.generation = binding.generation;
    if (pending.lastSentGeneration == binding.generation) return true;
    const bool sent = m_ws->sendUploadRemove(
        pending.asset.remoteSessionId, pending.asset.generation,
        pending.removalId, pending.asset.uploadId, pending.asset.assetId,
        pending.asset.size, pending.asset.sha256, pending.reason);
    if (sent) pending.lastSentGeneration = binding.generation;
    return sent;
}

bool UploadManager::assetRemovalMessageMatches(
    const PendingAssetRemoval& pending,
    const QJsonObject& message,
    bool requireDerivedFields) const {
    quint64 generation = 0;
    quint64 connectionGeneration = 0;
    qint64 offset = -1;
    qint64 size = -1;
    if (!m_ws
        || !parsePositiveGeneration(message.value(QStringLiteral("generation")),
                                    generation)
        || !parsePositiveGeneration(
            message.value(QStringLiteral("connectionGeneration")),
            connectionGeneration)
        || !parseNonNegativeOffset(message.value(QStringLiteral("offset")), offset)
        || !parseManifestSize(message.value(QStringLiteral("size")), size)) {
        return false;
    }
    const QString ownerEndpointId =
        message.value(QStringLiteral("ownerEndpointId")).toString();
    const QString targetEndpointId =
        message.value(QStringLiteral("targetEndpointId")).toString();
    return connectionGeneration == m_ws->connectionGeneration()
        && generation == pending.asset.generation
        && message.value(QStringLiteral("remoteSessionId")).toString()
            == pending.asset.remoteSessionId
        && message.value(QStringLiteral("removalId")).toString()
            == pending.removalId
        && message.value(QStringLiteral("uploadId")).toString()
            == pending.asset.uploadId
        && message.value(QStringLiteral("assetId")).toString()
            == pending.asset.assetId
        && offset == pending.asset.size && size == pending.asset.size
        && message.value(QStringLiteral("sha256")).toString()
            == pending.asset.sha256
        && (!requireDerivedFields
            || (message.value(QStringLiteral("fileId")).toString()
                    == pending.asset.sha256
                && message.value(QStringLiteral("extension")).toString()
                    == pending.asset.extension))
        && (ownerEndpointId.isEmpty() || ownerEndpointId == m_ws->endpointId())
        && (targetEndpointId.isEmpty()
            || targetEndpointId == pending.asset.targetEndpointId);
}

void UploadManager::rememberCommittedAssets(
    const QString& targetEndpointId,
    const QString& remoteSessionId,
    quint64 generation,
    const QString& uploadId,
    const QVector<OutgoingAsset>& assets) {
    if (targetEndpointId.isEmpty() || remoteSessionId.isEmpty()
        || generation == 0 || uploadId.isEmpty()) {
        return;
    }
    QHash<QString, CommittedRemoteAsset>& inventory =
        m_committedAssetsByTarget[targetEndpointId];
    for (const OutgoingAsset& outgoing : assets) {
        if (outgoing.assetId.isEmpty() || outgoing.sha256.isEmpty()
            || outgoing.size < 1) {
            continue;
        }
        // A local repository ID may point at new bytes after a source change.
        // Remove its former association before recording the server's exact
        // last-writer-wins asset inventory entry.
        for (auto existing = inventory.begin(); existing != inventory.end();) {
            bool overlaps = false;
            for (const QString& localFileId : outgoing.localFileIds) {
                if (existing->localFileIds.contains(localFileId)) {
                    overlaps = true;
                    break;
                }
            }
            if (overlaps && existing.key() != outgoing.assetId) {
                existing = inventory.erase(existing);
            } else {
                ++existing;
            }
        }
        CommittedRemoteAsset committed;
        committed.targetEndpointId = targetEndpointId;
        committed.remoteSessionId = remoteSessionId;
        committed.generation = generation;
        committed.uploadId = uploadId;
        committed.assetId = outgoing.assetId;
        committed.sha256 = outgoing.sha256;
        committed.extension = outgoing.extension;
        committed.localFileIds = outgoing.localFileIds;
        committed.size = outgoing.size;
        inventory.insert(committed.assetId, committed);
    }
    if (!inventory.isEmpty()) {
        m_remoteInventoryTargets.insert(targetEndpointId);
    }
}

bool UploadManager::findCommittedAsset(const QString& targetEndpointId,
                                       const QString& localFileId,
                                       CommittedRemoteAsset* asset) const {
    const auto target = m_committedAssetsByTarget.constFind(targetEndpointId);
    if (target == m_committedAssetsByTarget.cend()) {
        return false;
    }
    for (auto it = target->cbegin(); it != target->cend(); ++it) {
        if (it->localFileIds.contains(localFileId)) {
            if (asset) *asset = it.value();
            return true;
        }
    }
    return false;
}

void UploadManager::forgetCommittedAsset(const CommittedRemoteAsset& asset) {
    auto target = m_committedAssetsByTarget.find(asset.targetEndpointId);
    if (target == m_committedAssetsByTarget.end()) {
        return;
    }
    const auto current = target->constFind(asset.assetId);
    if (current != target->cend()
        && current->remoteSessionId == asset.remoteSessionId
        && current->uploadId == asset.uploadId
        && current->sha256 == asset.sha256) {
        target->remove(asset.assetId);
    }
    if (target->isEmpty()) {
        m_committedAssetsByTarget.erase(target);
        m_remoteInventoryTargets.remove(asset.targetEndpointId);
    }
}

void UploadManager::forgetRemoteSessionInventory(const QString& remoteSessionId) {
    if (remoteSessionId.isEmpty()) return;
    forgetIncomingUploadCompletions(remoteSessionId);
    const QStringList targets = m_committedAssetsByTarget.keys();
    for (const QString& targetId : targets) {
        auto target = m_committedAssetsByTarget.find(targetId);
        if (target == m_committedAssetsByTarget.end()) continue;
        for (auto asset = target->begin(); asset != target->end();) {
            if (asset->remoteSessionId == remoteSessionId) {
                asset = target->erase(asset);
            } else {
                ++asset;
            }
        }
        if (target->isEmpty()) {
            m_committedAssetsByTarget.erase(target);
            m_remoteInventoryTargets.remove(targetId);
        }
    }
    const QStringList removalIds = m_pendingAssetRemovals.keys();
    for (const QString& removalId : removalIds) {
        if (m_pendingAssetRemovals.value(removalId).asset.remoteSessionId
            == remoteSessionId) {
            m_pendingAssetRemovals.remove(removalId);
        }
    }
}

void UploadManager::failAssetRemoval(const QString& removalId,
                                     const QString& reason) {
    const auto it = m_pendingAssetRemovals.find(removalId);
    if (it == m_pendingAssetRemovals.end()) return;
    const PendingAssetRemoval pending = it.value();
    m_pendingAssetRemovals.erase(it);
    emit assetRemovalFailed(pending.asset.targetEndpointId,
                            pending.asset.remoteSessionId,
                            pending.asset.localFileIds,
                            reason.left(512));
    emit uiStateChanged();
}

void UploadManager::requestCancel() {
    if (ParallelOutgoingTransfer* transfer = parallelForTarget(m_targetClientId)) {
        if (!m_ws || transfer->stateAge.elapsed() < CANCEL_GUARD_MS
            || transfer->state == OutgoingState::Cancelling
            || transfer->state == OutgoingState::Idle) return;
        setParallelState(transfer, OutgoingState::Cancelling);
        stopParallel(transfer);
        m_ws->sendUploadAbort(transfer->remoteSessionId, transfer->generation,
                              transfer->uploadId, QStringLiteral("User cancelled"));
        if (transfer->cancelTimer) {
            const int timeout = m_ws->serverPolicy()
                .value(QStringLiteral("uploadTargetAckTimeoutMs")).toInt();
            if (timeout > 0) transfer->cancelTimer->start(timeout);
        }
        emit uiStateChanged();
        return;
    }
    if (!m_ws || m_outgoingRemoteSessionId.isEmpty()
        || m_currentUploadId.isEmpty()) return;
    if (!canRequestCancel()) return;

    recordAcceptedAction();
    setOutgoingState(OutgoingState::Cancelling);
    stopOutgoingPump();
    m_ws->sendUploadAbort(m_outgoingRemoteSessionId, m_outgoingGeneration,
                          m_currentUploadId, QStringLiteral("User cancelled"));
    // Cancellation only discards this upload's staging directory. It must never
    // remove files validated by an earlier incremental transfer.
    emit uiStateChanged();
    // A lost abort acknowledgement cannot keep the UI stuck. The upload ID is
    // terminal on the server, so this local completion is idempotent.
    if (!m_cancelFallbackTimer) {
        m_cancelFallbackTimer = new QTimer(this);
        m_cancelFallbackTimer->setSingleShot(true);
        connect(m_cancelFallbackTimer, &QTimer::timeout, this, [this]() {
            if (m_outgoingState == OutgoingState::Cancelling) {
                finishLocalCancellation();
            }
        });
    }
    const int cancelTimeout = m_ws->serverPolicy()
        .value(QStringLiteral("uploadTargetAckTimeoutMs")).toInt();
    if (cancelTimeout > 0) m_cancelFallbackTimer->start(cancelTimeout);
}

void UploadManager::startUpload(const QVector<UploadFileInfo>& files) {
    const bool selectedAssetRemovalPending = std::any_of(
        m_pendingAssetRemovals.cbegin(), m_pendingAssetRemovals.cend(),
        [this](const PendingAssetRemoval& removal) {
            return removal.asset.targetEndpointId == m_targetClientId;
        });
    if (selectedAssetRemovalPending) {
        qWarning() << "UploadManager: remote removal is still pending";
        return;
    }
    if (!m_ws || !m_fileManager || files.isEmpty()
        || files.size() > kMaxIncomingFiles) {
        qWarning() << "UploadManager: refusing upload with invalid file count";
        return;
    }

    RemoteSessionCoordinator* sessions = m_ws->remoteSessionCoordinator();
    const RemoteSessionCoordinator::Binding binding = sessions
        ? sessions->outgoingForPeer(m_targetClientId)
        : RemoteSessionCoordinator::Binding();
    if (!binding.active || binding.ownerEndpointId != m_ws->endpointId()) {
        qWarning() << "UploadManager: an Active owner RemoteSession is required";
        return;
    }
    if ((!m_currentUploadId.isEmpty()
         && m_outgoingRemoteSessionId == binding.remoteSessionId)
        || parallelForSession(binding.remoteSessionId)) {
        qWarning() << "UploadManager: one upload per RemoteSession is already queued or active";
        return;
    }

    QMap<QString, OutgoingAsset> assetsByDigest;
    QSet<QString> allMediaIds;
    for (const UploadFileInfo& file : files) {
        const QFileInfo info(file.path);
        const QString extension = file.extension.toLower();
        const QString name = file.name;
        if (file.size < 1 || file.size > kMaxIncomingFileBytes
            || !isValidFileId(file.fileId)
            || !isValidOpaqueId(file.mediaId)
            || !info.exists() || !info.isFile() || info.isSymLink()
            || info.size() != file.size || !isSafeDisplayName(name)
            || !isAllowedMediaExtension(extension)
            || QFileInfo(name).suffix().compare(extension, Qt::CaseInsensitive) != 0
            || !MediaFilePolicy::isAcceptedLocalFile(file.path)) {
            qWarning() << "UploadManager: refusing unsupported media file"
                       << "or excessive upload size (video uploads must be valid MP4 files)";
            return;
        }

        // The legacy repository ID is not a content digest yet. Protocol v3
        // identity is always the full-file SHA-256; keep local IDs only for UI
        // and RemoteFileTracker bookkeeping.
        const QString digest = sha256ForFile(file.path);
        if (!isValidFileId(digest)) {
            qWarning() << "UploadManager: could not establish immutable asset identity";
            return;
        }

        QStringList mediaIds;
        const QList<QString> associated = m_fileManager->getMediaIdsForFile(file.fileId);
        mediaIds.reserve(associated.size() + 1);
        for (const QString& mediaId : associated) {
            if (isValidOpaqueId(mediaId) && !mediaIds.contains(mediaId)) {
                mediaIds.append(mediaId);
            }
        }
        if (!mediaIds.contains(file.mediaId)) mediaIds.append(file.mediaId);
        std::sort(mediaIds.begin(), mediaIds.end());

        OutgoingAsset asset = assetsByDigest.value(digest);
        if (!asset.assetId.isEmpty()
            && (asset.size != file.size || asset.extension != extension)) {
            qWarning() << "UploadManager: identical asset identity has conflicting metadata";
            return;
        }
        if (asset.assetId.isEmpty()) {
            asset.assetId = digest;
            asset.sha256 = digest;
            asset.path = file.path;
            asset.name = name;
            asset.extension = extension;
            asset.size = file.size;
        }
        if (!asset.localFileIds.contains(file.fileId)) {
            asset.localFileIds.append(file.fileId);
        }
        for (const QString& mediaId : std::as_const(mediaIds)) {
            if (allMediaIds.contains(mediaId)
                && !asset.mediaIds.contains(mediaId)) {
                qWarning() << "UploadManager: a media ID maps to multiple upload assets";
                return;
            }
            allMediaIds.insert(mediaId);
            if (!asset.mediaIds.contains(mediaId)) asset.mediaIds.append(mediaId);
        }
        std::sort(asset.mediaIds.begin(), asset.mediaIds.end());
        std::sort(asset.localFileIds.begin(), asset.localFileIds.end());
        assetsByDigest.insert(digest, asset);
    }

    if (assetsByDigest.isEmpty() || assetsByDigest.size() > kMaxIncomingFiles) return;
    qint64 declaredTotalBytes = 0;
    QJsonArray manifest;
    QVector<OutgoingAsset> normalizedAssets;
    normalizedAssets.reserve(assetsByDigest.size());
    for (auto iterator = assetsByDigest.cbegin(); iterator != assetsByDigest.cend(); ++iterator) {
        const OutgoingAsset& asset = iterator.value();
        if (declaredTotalBytes > kMaxIncomingUploadBytes - asset.size) {
            qWarning() << "UploadManager: refusing upload above the session byte limit";
            return;
        }
        declaredTotalBytes += asset.size;
        QJsonArray mediaIds;
        for (const QString& mediaId : asset.mediaIds) mediaIds.append(mediaId);
        manifest.append(QJsonObject{
            {QStringLiteral("assetId"), asset.assetId},
            {QStringLiteral("fileId"), asset.sha256},
            {QStringLiteral("sha256"), asset.sha256},
            {QStringLiteral("name"), asset.name},
            {QStringLiteral("extension"), asset.extension},
            {QStringLiteral("size"), static_cast<double>(asset.size)},
            {QStringLiteral("mediaIds"), mediaIds}
        });
        normalizedAssets.append(asset);
    }

    const bool usePrimarySlot = m_currentUploadId.isEmpty()
        && m_outgoingState == OutgoingState::Idle;
    if (!usePrimarySlot) {
        auto* transfer = new ParallelOutgoingTransfer;
        transfer->targetEndpointId = m_targetClientId;
        transfer->remoteSessionId = binding.remoteSessionId;
        transfer->uploadId = QUuid::createUuid().toString(QUuid::WithoutBraces);
        transfer->generation = binding.generation;
        transfer->schedulerGeneration = binding.generation;
        transfer->remoteInventoryBeforeStart =
            m_remoteInventoryTargets.contains(m_targetClientId);
        transfer->assets = normalizedAssets;
        transfer->manifest = manifest;
        transfer->totalBytes = declaredTotalBytes;
        transfer->totalFiles = normalizedAssets.size();
        for (const OutgoingAsset& asset : std::as_const(transfer->assets)) {
            transfer->durableOffsets.insert(asset.assetId, 0);
        }
        initializeParallelTimers(transfer);
        m_parallelOutgoingByUpload.insert(transfer->uploadId, transfer);
        m_parallelUploadBySession.insert(transfer->remoteSessionId,
                                         transfer->uploadId);
        recordAcceptedAction();
        emit uiStateChanged();
        m_uploadScheduler->setSessionState(binding.remoteSessionId,
                                           UploadScheduler::SessionState::Active);
        const UploadScheduler::UploadRequest request{
            binding.remoteSessionId, binding.generation, transfer->uploadId,
            transfer->assets.first().assetId
        };
        if (m_uploadScheduler->enqueue(request)
            != UploadScheduler::EnqueueResult::Enqueued) {
            failParallel(transfer,
                         QStringLiteral("Upload scheduler rejected the request"));
        }
        return;
    }

    m_uploadWasActiveBeforeStart =
        m_remoteInventoryTargets.contains(m_targetClientId);
    m_uploadTargetClientId = m_targetClientId;
    m_outgoingRemoteSessionId = binding.remoteSessionId;
    m_outgoingGeneration = binding.generation;
    m_schedulerGeneration = binding.generation;
    m_currentUploadId = QUuid::createUuid().toString(QUuid::WithoutBraces);
    setOutgoingState(OutgoingState::Queued);
    m_lastPercent = 0;
    m_filesCompleted = 0;
    m_totalFiles = normalizedAssets.size();
    m_totalBytes = declaredTotalBytes;
    m_sentBytes = 0;
    m_remoteAcknowledgedBytes = 0;
    m_remoteProgressReceived = false;
    stopOutgoingPump();
    m_outgoingFiles = files;
    m_outgoingAssets = normalizedAssets;
    m_outgoingManifest = manifest;
    m_outgoingDurableOffsets.clear();
    for (const OutgoingAsset& asset : std::as_const(m_outgoingAssets)) {
        m_outgoingDurableOffsets.insert(asset.assetId, 0);
    }
    m_outgoingFileIndex = 0;
    m_outgoingChunkIndex = 0;
    m_outgoingSentForFile = 0;
    m_outgoingPayloadCompleteSent = false;
    m_waitingForResume = false;
    resetProgressTracking();
    recordAcceptedAction();
    emit uiStateChanged();
    m_uploadScheduler->setSessionState(binding.remoteSessionId,
                                       UploadScheduler::SessionState::Active);
    const UploadScheduler::UploadRequest request{
        binding.remoteSessionId, binding.generation, m_currentUploadId,
        m_outgoingAssets.first().assetId
    };
    if (m_uploadScheduler->enqueue(request)
        != UploadScheduler::EnqueueResult::Enqueued) {
        onUploadRejected(m_currentUploadId,
                         QStringLiteral("Upload scheduler rejected the request"));
    }
}

void UploadManager::initializeParallelTimers(ParallelOutgoingTransfer* transfer) {
    if (!transfer) return;
    const QString uploadId = transfer->uploadId;
    transfer->pumpTimer = new QTimer(this);
    transfer->pumpTimer->setSingleShot(true);
    transfer->pumpTimer->setInterval(0);
    connect(transfer->pumpTimer, &QTimer::timeout, this, [this, uploadId]() {
        if (ParallelOutgoingTransfer* current = parallelForUpload(uploadId)) {
            pumpParallel(current);
        }
    });

    auto makeWatchdog = [this, uploadId](const QString& error,
                                         OutgoingState expected) {
        QTimer* timer = new QTimer(this);
        timer->setSingleShot(true);
        timer->setInterval(0);
        connect(timer, &QTimer::timeout, this, [this, uploadId, error, expected]() {
            ParallelOutgoingTransfer* current = parallelForUpload(uploadId);
            if (current && current->state == expected) failParallel(current, error);
        });
        return timer;
    };
    transfer->stallTimer = makeWatchdog(
        QStringLiteral("Upload transport stalled"), OutgoingState::Streaming);
    transfer->startAckTimer = makeWatchdog(
        QStringLiteral("Remote client did not accept the upload in time"),
        OutgoingState::AwaitingTargetReady);
    transfer->ackTimer = makeWatchdog(
        QStringLiteral("Remote client did not validate the upload in time"),
        OutgoingState::AwaitingValidation);
    transfer->cancelTimer = new QTimer(this);
    transfer->cancelTimer->setSingleShot(true);
    transfer->cancelTimer->setInterval(0);
    connect(transfer->cancelTimer, &QTimer::timeout, this, [this, uploadId]() {
        ParallelOutgoingTransfer* current = parallelForUpload(uploadId);
        if (current && current->state == OutgoingState::Cancelling) {
            cancelParallel(current);
        }
    });

    const QJsonObject policy = m_ws ? m_ws->serverPolicy() : QJsonObject();
    const int idleTimeout = policy.value(QStringLiteral("uploadIdleTimeoutMs")).toInt();
    const int targetAckTimeout =
        policy.value(QStringLiteral("uploadTargetAckTimeoutMs")).toInt();
    if (idleTimeout > 0) transfer->stallTimer->setInterval(idleTimeout);
    if (targetAckTimeout > 0) {
        transfer->startAckTimer->setInterval(targetAckTimeout);
        transfer->ackTimer->setInterval(targetAckTimeout);
        transfer->cancelTimer->setInterval(targetAckTimeout);
    }
    transfer->stateAge.start();
}

void UploadManager::setParallelState(ParallelOutgoingTransfer* transfer,
                                     OutgoingState state) {
    if (!transfer || transfer->state == state) return;
    transfer->state = state;
    transfer->stateAge.restart();
}

void UploadManager::startParallelScheduled(ParallelOutgoingTransfer* transfer) {
    if (!transfer || !m_ws || transfer->state != OutgoingState::Queued) return;
    RemoteSessionCoordinator* sessions = m_ws->remoteSessionCoordinator();
    const RemoteSessionCoordinator::Binding binding = sessions
        ? sessions->byId(transfer->remoteSessionId)
        : RemoteSessionCoordinator::Binding();
    if (!binding.active || binding.ownerEndpointId != m_ws->endpointId()
        || binding.generation != transfer->generation) {
        failParallel(transfer, QStringLiteral("Remote session is not active"));
        return;
    }
    if (!m_ws->beginUploadSession(true)) {
        failParallel(transfer,
                     QStringLiteral("Upload transport is unavailable before transfer start"));
        return;
    }
    transfer->transportRegistered = true;
    setParallelState(transfer, OutgoingState::AwaitingTargetReady);
    if (!m_ws->sendUploadStart(transfer->remoteSessionId, transfer->generation,
                               transfer->uploadId, transfer->manifest)) {
        failParallel(transfer,
                     QStringLiteral("Upload transport failed before transfer started"));
        return;
    }
    if (transfer->startAckTimer && transfer->startAckTimer->interval() > 0) {
        transfer->startAckTimer->start();
    }
    if (transfer->targetEndpointId == m_targetClientId) emit uiStateChanged();
}

bool UploadManager::resumeParallel(ParallelOutgoingTransfer* transfer) {
    if (!transfer || !m_ws || transfer->state != OutgoingState::Suspended) return false;
    RemoteSessionCoordinator* sessions = m_ws->remoteSessionCoordinator();
    const RemoteSessionCoordinator::Binding binding = sessions
        ? sessions->byId(transfer->remoteSessionId)
        : RemoteSessionCoordinator::Binding();
    if (!binding.active || binding.ownerEndpointId != m_ws->endpointId()) return false;

    transfer->generation = binding.generation;
    m_uploadScheduler->setSessionState(binding.remoteSessionId,
                                       UploadScheduler::SessionState::Active);
    if (!m_ws->beginUploadSession(true)) return false;
    transfer->transportRegistered = true;
    const bool wasAwaitingValidation =
        transfer->stateBeforeSuspend == OutgoingState::AwaitingValidation;
    setParallelState(transfer, wasAwaitingValidation
        ? OutgoingState::AwaitingValidation
        : OutgoingState::AwaitingTargetReady);
    transfer->waitingForResume = !wasAwaitingValidation;
    const bool sent = wasAwaitingValidation
        ? m_ws->sendUploadStart(transfer->remoteSessionId, transfer->generation,
                                transfer->uploadId, transfer->manifest)
        : m_ws->sendUploadResume(transfer->remoteSessionId, transfer->generation,
                                 transfer->uploadId);
    if (!sent) {
        m_ws->endUploadSession();
        transfer->transportRegistered = false;
        setParallelState(transfer, OutgoingState::Suspended);
        return false;
    }
    QTimer* watchdog = wasAwaitingValidation
        ? transfer->ackTimer : transfer->startAckTimer;
    if (watchdog && watchdog->interval() > 0) watchdog->start();
    if (transfer->targetEndpointId == m_targetClientId) emit uiStateChanged();
    return true;
}

void UploadManager::suspendParallel(ParallelOutgoingTransfer* transfer) {
    if (!transfer || transfer->state == OutgoingState::Idle
        || transfer->state == OutgoingState::Queued
        || transfer->state == OutgoingState::Cancelling
        || transfer->state == OutgoingState::Suspended) return;
    transfer->stateBeforeSuspend = transfer->state;
    stopParallel(transfer);
    if (transfer->transportRegistered && m_ws) m_ws->endUploadSession();
    transfer->transportRegistered = false;
    transfer->waitingForResume = true;
    setParallelState(transfer, OutgoingState::Suspended);
    m_uploadScheduler->setSessionState(transfer->remoteSessionId,
                                       UploadScheduler::SessionState::Grace);
    if (transfer->targetEndpointId == m_targetClientId) emit uiStateChanged();
}

void UploadManager::scheduleParallelPump(ParallelOutgoingTransfer* transfer) {
    if (!transfer || !transfer->pumpTimer || transfer->pumpTimer->isActive()
        || transfer->state != OutgoingState::Streaming
        || transfer->payloadCompleteSent) return;
    transfer->pumpTimer->start();
}

void UploadManager::stopParallel(ParallelOutgoingTransfer* transfer) {
    if (!transfer) return;
    if (transfer->pumpTimer) transfer->pumpTimer->stop();
    if (transfer->stallTimer) transfer->stallTimer->stop();
    if (transfer->startAckTimer) transfer->startAckTimer->stop();
    if (transfer->ackTimer) transfer->ackTimer->stop();
    if (transfer->fileHandle.isOpen()) transfer->fileHandle.close();
}

QJsonArray UploadManager::parallelAssetStates(
    const ParallelOutgoingTransfer* transfer, bool complete) const {
    QJsonArray states;
    if (!transfer) return states;
    for (const OutgoingAsset& asset : transfer->assets) {
        states.append(QJsonObject{
            {QStringLiteral("assetId"), asset.assetId},
            {QStringLiteral("offset"), static_cast<double>(
                 complete ? asset.size
                          : transfer->durableOffsets.value(asset.assetId, 0))},
            {QStringLiteral("size"), static_cast<double>(asset.size)},
            {QStringLiteral("sha256"), asset.sha256}
        });
    }
    return states;
}

void UploadManager::emitParallelProgress(
    const ParallelOutgoingTransfer* transfer) {
    if (!transfer || transfer->targetEndpointId != m_targetClientId
        || transfer->totalFiles <= 0) return;
    const int percent = std::clamp(
        std::max(transfer->localPercent, transfer->remotePercent), 0,
        transfer->remotePercent > 0 ? 100 : 99);
    emit uploadProgress(percent, transfer->remoteFilesCompleted,
                        transfer->totalFiles);
}

bool UploadManager::applyParallelOffsets(ParallelOutgoingTransfer* transfer,
                                         const QJsonArray& assets,
                                         bool resetSendCursor,
                                         QString* errorMessage) {
    if (!transfer || assets.size() != transfer->assets.size()) {
        if (errorMessage) *errorMessage = QStringLiteral("Incomplete upload inventory");
        return false;
    }
    QHash<QString, qint64> offsets;
    for (const QJsonValue& value : assets) {
        if (!value.isObject()) {
            if (errorMessage) *errorMessage = QStringLiteral("Malformed upload inventory");
            return false;
        }
        const QJsonObject state = value.toObject();
        const QString assetId = state.value(QStringLiteral("assetId")).toString();
        const auto assetIt = std::find_if(
            transfer->assets.cbegin(), transfer->assets.cend(),
            [&assetId](const OutgoingAsset& candidate) {
                return candidate.assetId == assetId;
            });
        qint64 offset = -1;
        qint64 size = -1;
        if (assetIt == transfer->assets.cend() || offsets.contains(assetId)
            || !parseNonNegativeOffset(state.value(QStringLiteral("offset")), offset)
            || !parseManifestSize(state.value(QStringLiteral("size")), size)
            || size != assetIt->size || offset > size
            || state.value(QStringLiteral("sha256")).toString() != assetIt->sha256
            || offset < transfer->durableOffsets.value(assetId, 0)) {
            if (errorMessage) *errorMessage = QStringLiteral("Mismatched upload inventory");
            return false;
        }
        offsets.insert(assetId, offset);
    }

    qint64 durableBytes = 0;
    int completed = 0;
    for (const OutgoingAsset& asset : transfer->assets) {
        if (!offsets.contains(asset.assetId)) {
            if (errorMessage) *errorMessage = QStringLiteral("Incomplete upload inventory");
            return false;
        }
        const qint64 offset = offsets.value(asset.assetId);
        durableBytes += offset;
        if (offset == asset.size) ++completed;
        const int percent = static_cast<int>(std::round(
            offset * 100.0 / static_cast<double>(asset.size)));
        for (const QString& localFileId : asset.localFileIds) {
            transfer->remoteFilePercents[localFileId] = percent;
            if (transfer->targetEndpointId == m_targetClientId) {
                emit fileUploadProgress(localFileId, percent);
            }
        }
    }
    transfer->durableOffsets = offsets;
    transfer->remoteAcknowledgedBytes = durableBytes;
    transfer->remotePercent = transfer->totalBytes > 0
        ? std::clamp(static_cast<int>(std::round(
              durableBytes * 100.0 / static_cast<double>(transfer->totalBytes))), 0, 100)
        : 0;
    transfer->remoteFilesCompleted = completed;
    emitParallelProgress(transfer);

    if (resetSendCursor) {
        if (transfer->fileHandle.isOpen()) transfer->fileHandle.close();
        transfer->sentBytes = durableBytes;
        transfer->fileIndex = 0;
        while (transfer->fileIndex < transfer->assets.size()
               && offsets.value(transfer->assets.at(transfer->fileIndex).assetId)
                    == transfer->assets.at(transfer->fileIndex).size) {
            ++transfer->fileIndex;
        }
        transfer->sentForFile = transfer->fileIndex < transfer->assets.size()
            ? offsets.value(transfer->assets.at(transfer->fileIndex).assetId) : 0;
        transfer->payloadCompleteSent = false;
    }
    return true;
}

void UploadManager::pumpParallel(ParallelOutgoingTransfer* transfer) {
    if (!transfer || transfer->pumpRunning
        || transfer->state != OutgoingState::Streaming
        || transfer->payloadCompleteSent) return;
    QScopedValueRollback<bool> guard(transfer->pumpRunning, true);
    if (!m_ws || !m_ws->isUploadSessionTransportAvailable()) {
        failParallel(transfer, QStringLiteral("Upload transport was interrupted"));
        return;
    }

    constexpr qint64 chunkBytes = 128 * 1024;
    constexpr qint64 maximumChunkWireBytes = ((chunkBytes + 2) / 3) * 4 + 4096;
    int chunksQueued = 0;
    while (chunksQueued < kMaxChunksPerPump
           && transfer->state == OutgoingState::Streaming
           && !transfer->payloadCompleteSent) {
        const qint64 queuedBytes = m_ws->uploadTransportBytesToWrite();
        if (queuedBytes < 0) {
            failParallel(transfer, QStringLiteral("Upload transport was interrupted"));
            return;
        }
        if (queuedBytes > kMaxQueuedUploadBytes - maximumChunkWireBytes
            || transfer->sentBytes - transfer->remoteAcknowledgedBytes
                >= kMaxUnacknowledgedRemoteBytes) return;

        if (transfer->fileIndex >= transfer->assets.size()) {
            if (transfer->sentBytes != transfer->totalBytes
                || !m_ws->sendUploadComplete(
                    transfer->remoteSessionId, transfer->generation,
                    transfer->uploadId, parallelAssetStates(transfer, true))) {
                failParallel(transfer, QStringLiteral(
                    "Upload transport was interrupted before completion"));
                return;
            }
            transfer->payloadCompleteSent = true;
            setParallelState(transfer, OutgoingState::AwaitingValidation);
            if (transfer->stallTimer) transfer->stallTimer->stop();
            if (transfer->ackTimer && transfer->ackTimer->interval() > 0) {
                transfer->ackTimer->start();
            }
            if (transfer->targetEndpointId == m_targetClientId) emit uiStateChanged();
            return;
        }

        const OutgoingAsset asset = transfer->assets.at(transfer->fileIndex);
        if (!transfer->fileHandle.isOpen()) {
            transfer->fileHandle.setFileName(asset.path);
            if (!transfer->fileHandle.open(QIODevice::ReadOnly)
                || transfer->fileHandle.size() != asset.size
                || !transfer->fileHandle.seek(transfer->sentForFile)) {
                failParallel(transfer,
                    QStringLiteral("Source asset is unavailable or changed: %1")
                        .arg(asset.name));
                return;
            }
            if (transfer->targetEndpointId == m_targetClientId) {
                for (const QString& localFileId : asset.localFileIds) {
                    emit fileUploadStarted(localFileId);
                }
            }
        }

        const qint64 remaining = asset.size - transfer->sentForFile;
        const qint64 requested = std::min(chunkBytes, remaining);
        if (requested <= 0) {
            failParallel(transfer, QStringLiteral("Invalid source asset state"));
            return;
        }
        const QByteArray chunk = transfer->fileHandle.read(requested);
        if (chunk.size() != requested
            || transfer->fileHandle.error() != QFileDevice::NoError
            || !m_ws->sendUploadChunk(
                transfer->remoteSessionId, transfer->generation,
                transfer->uploadId, asset.assetId, transfer->sentForFile,
                asset.sha256, chunk)) {
            failParallel(transfer, QStringLiteral("Upload transport was interrupted"));
            return;
        }
        ++chunksQueued;
        transfer->sentForFile += chunk.size();
        transfer->sentBytes += chunk.size();
        if (transfer->stallTimer && transfer->stallTimer->interval() > 0) {
            transfer->stallTimer->start();
        }
        const int filePercent = std::clamp(static_cast<int>(std::round(
            transfer->sentForFile * 100.0 / static_cast<double>(asset.size))), 0, 99);
        for (const QString& localFileId : asset.localFileIds) {
            transfer->localFilePercents[localFileId] = filePercent;
            if (transfer->targetEndpointId == m_targetClientId) {
                emit fileUploadProgress(localFileId, filePercent);
            }
        }
        transfer->localPercent = transfer->totalBytes > 0
            ? std::clamp(static_cast<int>(std::round(
                  transfer->sentBytes * 100.0
                  / static_cast<double>(transfer->totalBytes))), 0, 99)
            : 0;
        emitParallelProgress(transfer);

        if (transfer->sentForFile == asset.size) {
            const bool exact = transfer->fileHandle.size() == asset.size
                && transfer->fileHandle.pos() == asset.size
                && transfer->fileHandle.error() == QFileDevice::NoError;
            transfer->fileHandle.close();
            if (!exact) {
                failParallel(transfer,
                    QStringLiteral("Source file changed during upload: %1")
                        .arg(asset.name));
                return;
            }
            ++transfer->fileIndex;
            transfer->sentForFile = 0;
            if (transfer->targetEndpointId == m_targetClientId) {
                for (const QString& localFileId : asset.localFileIds) {
                    emit fileUploadFinished(localFileId);
                }
            }
        }
    }
    if (transfer->state == OutgoingState::Streaming
        && !transfer->payloadCompleteSent) scheduleParallelPump(transfer);
}

void UploadManager::removeParallel(ParallelOutgoingTransfer* transfer,
                                   bool preserveRemoteInventory,
                                   bool releaseScheduler,
                                   bool schedulerSuccess) {
    if (!transfer) return;
    const QString uploadId = transfer->uploadId;
    const QString sessionId = transfer->remoteSessionId;
    const QString targetId = transfer->targetEndpointId;
    const quint64 schedulerGeneration = transfer->schedulerGeneration;
    const bool wasActive = m_uploadScheduler && m_uploadScheduler->isUploadActive(
        sessionId, schedulerGeneration, uploadId);

    m_parallelOutgoingByUpload.remove(uploadId);
    if (m_parallelUploadBySession.value(sessionId) == uploadId) {
        m_parallelUploadBySession.remove(sessionId);
    }
    stopParallel(transfer);
    if (transfer->cancelTimer) transfer->cancelTimer->stop();
    if (transfer->transportRegistered && m_ws) m_ws->endUploadSession();
    transfer->transportRegistered = false;
    if (preserveRemoteInventory) m_remoteInventoryTargets.insert(targetId);
    else if (!transfer->remoteInventoryBeforeStart) m_remoteInventoryTargets.remove(targetId);

    if (m_uploadScheduler && releaseScheduler) {
        if (wasActive) {
            if (schedulerSuccess) {
                m_uploadScheduler->completeUpload(sessionId, schedulerGeneration, uploadId);
            } else {
                m_uploadScheduler->failUpload(sessionId, schedulerGeneration, uploadId);
            }
        } else {
            m_uploadScheduler->cancelUpload(sessionId, schedulerGeneration, uploadId);
        }
    }
    for (QTimer* timer : {transfer->pumpTimer, transfer->stallTimer,
                          transfer->startAckTimer, transfer->ackTimer,
                          transfer->cancelTimer}) {
        if (timer) timer->deleteLater();
    }
    delete transfer;
    m_uploadActive = hasActiveUpload();
    emit uiStateChanged();
}

void UploadManager::failParallel(ParallelOutgoingTransfer* transfer,
                                 const QString& reason) {
    if (!transfer || !m_parallelOutgoingByUpload.contains(transfer->uploadId)) return;
    if (transfer->pumpRunning) {
        const QString uploadId = transfer->uploadId;
        transfer->pumpRunning = false;
        QTimer::singleShot(0, this, [this, uploadId, reason]() {
            if (ParallelOutgoingTransfer* current = parallelForUpload(uploadId)) {
                failParallel(current, reason);
            }
        });
        return;
    }
    const QString uploadId = transfer->uploadId;
    const bool preserve = transfer->remoteInventoryBeforeStart;
    stopParallel(transfer);
    if (m_ws && transfer->state != OutgoingState::Queued) {
        m_ws->sendUploadAbort(transfer->remoteSessionId, transfer->generation,
                              uploadId, reason);
        m_ws->cancelUploadId(uploadId);
    }
    emit uploadRejected(uploadId, reason.left(512));
    removeParallel(transfer, preserve, true, false);
}

void UploadManager::finishParallel(ParallelOutgoingTransfer* transfer) {
    if (!transfer || transfer->state != OutgoingState::AwaitingValidation) return;
    const bool selected = transfer->targetEndpointId == m_targetClientId;
    const QString uploadId = transfer->uploadId;
    for (const OutgoingAsset& asset : std::as_const(transfer->assets)) {
        for (const QString& localFileId : asset.localFileIds) {
            m_fileManager->markFileUploadedToClient(localFileId,
                                                    transfer->targetEndpointId);
        }
    }
    rememberCommittedAssets(transfer->targetEndpointId,
                            transfer->remoteSessionId,
                            transfer->generation,
                            transfer->uploadId,
                            transfer->assets);
    if (selected) emit uploadProgress(100, transfer->totalFiles, transfer->totalFiles);
    removeParallel(transfer, true, true, true);
    // Every concurrent transfer has its own terminal history event, including
    // one which completed after the user selected another target.
    emit uploadFinished(uploadId);
}

void UploadManager::cancelParallel(ParallelOutgoingTransfer* transfer) {
    if (!transfer) return;
    const QString uploadId = transfer->uploadId;
    const bool preserve = transfer->remoteInventoryBeforeStart;
    if (m_ws) m_ws->cancelUploadId(uploadId);
    removeParallel(transfer, preserve, true, false);
    emit uploadCancelled(uploadId);
}

void UploadManager::handleParallelMessage(ParallelOutgoingTransfer* transfer,
                                          const QJsonObject& message) {
    if (!transfer) return;
    const QString type = message.value(QStringLiteral("type")).toString();
    if (type == QLatin1String("upload_resume_ready")
        || type == QLatin1String("upload_ready")) {
        if (transfer->state != OutgoingState::AwaitingTargetReady
            || !message.value(QStringLiteral("assets")).isArray()) return;
        QString error;
        if (!applyParallelOffsets(transfer,
                message.value(QStringLiteral("assets")).toArray(), true, &error)) {
            failParallel(transfer, error);
            return;
        }
        if (type == QLatin1String("upload_resume_ready")) {
            transfer->waitingForResume = true;
            if (transfer->startAckTimer && transfer->startAckTimer->interval() > 0) {
                transfer->startAckTimer->start();
            }
            return;
        }
        transfer->waitingForResume = false;
        if (transfer->startAckTimer) transfer->startAckTimer->stop();
        setParallelState(transfer, OutgoingState::Streaming);
        if (transfer->stallTimer && transfer->stallTimer->interval() > 0) {
            transfer->stallTimer->start();
        }
        if (transfer->targetEndpointId == m_targetClientId) emit uiStateChanged();
        scheduleParallelPump(transfer);
        return;
    }
    if (type == QLatin1String("upload_progress")) {
        qint64 durableBytes = -1;
        qint64 totalSize = -1;
        if (!message.value(QStringLiteral("assets")).isArray()
            || !parseNonNegativeOffset(message.value(QStringLiteral("durableBytes")),
                                       durableBytes)
            || !parseNonNegativeOffset(message.value(QStringLiteral("totalSize")),
                                       totalSize)
            || totalSize != transfer->totalBytes || durableBytes > totalSize) {
            failParallel(transfer, QStringLiteral("Malformed upload progress inventory"));
            return;
        }
        QString error;
        if (!applyParallelOffsets(transfer,
                message.value(QStringLiteral("assets")).toArray(), false, &error)
            || durableBytes != transfer->remoteAcknowledgedBytes) {
            failParallel(transfer, error.isEmpty()
                ? QStringLiteral("Mismatched durable upload byte count") : error);
            return;
        }
        if (transfer->state == OutgoingState::Streaming) {
            if (transfer->stallTimer && transfer->stallTimer->interval() > 0) {
                transfer->stallTimer->start();
            }
            scheduleParallelPump(transfer);
        }
        return;
    }
    if (type == QLatin1String("upload_finished")) {
        if (!message.value(QStringLiteral("assets")).isArray()) return;
        QString error;
        if (!applyParallelOffsets(transfer,
                message.value(QStringLiteral("assets")).toArray(), false, &error)) {
            failParallel(transfer, error);
            return;
        }
        for (const OutgoingAsset& asset : std::as_const(transfer->assets)) {
            if (transfer->durableOffsets.value(asset.assetId) != asset.size) {
                failParallel(transfer, QStringLiteral("Incomplete final upload inventory"));
                return;
            }
        }
        finishParallel(transfer);
        return;
    }
    if (type == QLatin1String("upload_rejected")) {
        failParallel(transfer,
            message.value(QStringLiteral("reason")).toString(
                message.value(QStringLiteral("code")).toString(
                    QStringLiteral("Upload rejected"))));
        return;
    }
    if (type == QLatin1String("upload_aborted")
        || type == QLatin1String("upload_abort_ack")) {
        if (transfer->state == OutgoingState::Cancelling) cancelParallel(transfer);
        else failParallel(transfer, QStringLiteral("Upload aborted"));
        return;
    }
    if (type == QLatin1String("upload_abort")) {
        failParallel(transfer,
            message.value(QStringLiteral("reason")).toString(
                QStringLiteral("Upload aborted by remote session")));
    }
}

void UploadManager::startScheduledUpload(
    const UploadScheduler::UploadRequest& request) {
    if (ParallelOutgoingTransfer* transfer = parallelForUpload(request.uploadId)) {
        if (transfer->remoteSessionId == request.remoteSessionId
            && transfer->schedulerGeneration == request.connectionGeneration
            && transfer->state == OutgoingState::Queued) {
            startParallelScheduled(transfer);
        }
        return;
    }
    if (m_outgoingState != OutgoingState::Queued
        || request.uploadId != m_currentUploadId
        || request.remoteSessionId != m_outgoingRemoteSessionId
        || request.connectionGeneration != m_schedulerGeneration) return;

    RemoteSessionCoordinator* sessions = m_ws ? m_ws->remoteSessionCoordinator() : nullptr;
    const RemoteSessionCoordinator::Binding binding = sessions
        ? sessions->byId(m_outgoingRemoteSessionId)
        : RemoteSessionCoordinator::Binding();
    if (!binding.active || binding.ownerEndpointId != m_ws->endpointId()
        || binding.generation != m_outgoingGeneration) {
        failOutgoingUpload(QStringLiteral("Remote session is not active"));
        return;
    }
    if (!m_ws->beginUploadSession(true)) {
        failOutgoingUpload(QStringLiteral(
            "Upload transport is unavailable before transfer start"));
        return;
    }
    m_outgoingTransportRegistered = true;
    setOutgoingState(OutgoingState::AwaitingTargetReady);
    if (!m_ws->sendUploadStart(m_outgoingRemoteSessionId, m_outgoingGeneration,
                               m_currentUploadId, m_outgoingManifest)) {
        failOutgoingUpload(QStringLiteral(
            "Upload transport failed before transfer started"));
        return;
    }
    if (m_outgoingStartAckTimer && m_outgoingStartAckTimer->interval() > 0) {
        m_outgoingStartAckTimer->start();
    }
}

bool UploadManager::resumeOutgoingUpload() {
    if (!m_ws || m_currentUploadId.isEmpty()
        || m_outgoingRemoteSessionId.isEmpty()
        || m_outgoingState != OutgoingState::Suspended) return false;
    RemoteSessionCoordinator* sessions = m_ws->remoteSessionCoordinator();
    const RemoteSessionCoordinator::Binding binding = sessions
        ? sessions->byId(m_outgoingRemoteSessionId)
        : RemoteSessionCoordinator::Binding();
    if (!binding.active || binding.ownerEndpointId != m_ws->endpointId()) return false;

    m_outgoingGeneration = binding.generation;
    m_uploadScheduler->setSessionState(binding.remoteSessionId,
                                       UploadScheduler::SessionState::Active);
    if (!m_ws->beginUploadSession(true)) return false;
    m_outgoingTransportRegistered = true;
    const bool wasAwaitingValidation =
        m_stateBeforeSuspend == OutgoingState::AwaitingValidation;
    setOutgoingState(wasAwaitingValidation
                         ? OutgoingState::AwaitingValidation
                         : OutgoingState::AwaitingTargetReady);
    m_waitingForResume = !wasAwaitingValidation;
    const bool sent = wasAwaitingValidation
        ? m_ws->sendUploadStart(m_outgoingRemoteSessionId,
                                m_outgoingGeneration,
                                m_currentUploadId,
                                m_outgoingManifest)
        : m_ws->sendUploadResume(m_outgoingRemoteSessionId,
                                 m_outgoingGeneration,
                                 m_currentUploadId);
    if (!sent) {
        m_ws->endUploadSession();
        m_outgoingTransportRegistered = false;
        setOutgoingState(OutgoingState::Suspended);
        return false;
    }
    if (wasAwaitingValidation) {
        if (m_outgoingAckTimer && m_outgoingAckTimer->interval() > 0) {
            m_outgoingAckTimer->start();
        }
    } else if (m_outgoingStartAckTimer
               && m_outgoingStartAckTimer->interval() > 0) {
        m_outgoingStartAckTimer->start();
    }
    emit uiStateChanged();
    return true;
}

void UploadManager::suspendOutgoingForResume(const QString& reason) {
    Q_UNUSED(reason);
    if (m_currentUploadId.isEmpty()
        || m_outgoingState == OutgoingState::Idle
        || m_outgoingState == OutgoingState::Cancelling
        || m_outgoingState == OutgoingState::Suspended) return;
    m_stateBeforeSuspend = m_outgoingState;
    stopOutgoingPump();
    if (m_ws && m_outgoingTransportRegistered) m_ws->endUploadSession();
    m_outgoingTransportRegistered = false;
    m_waitingForResume = true;
    setOutgoingState(OutgoingState::Suspended);
    if (m_uploadScheduler && !m_outgoingRemoteSessionId.isEmpty()) {
        m_uploadScheduler->setSessionState(
            m_outgoingRemoteSessionId, UploadScheduler::SessionState::Grace);
    }
    emit uiStateChanged();
}

void UploadManager::applyRemoteSessionEnvelope(const QJsonObject& envelope) {
    const QString remoteSessionId =
        envelope.value(QStringLiteral("remoteSessionId")).toString();
    if (remoteSessionId.isEmpty()) return;
    const QString type = envelope.value(QStringLiteral("type")).toString();
    const QString phase = envelope.value(QStringLiteral("phase")).toString();
    quint64 generation = 0;
    if (!parsePositiveGeneration(
            envelope.value(QStringLiteral("generation")), generation)) {
        return;
    }

    if (type == QLatin1String("remote_session_closed")
        || phase == QLatin1String("Terminating")
        || phase == QLatin1String("CleanupPending")
        || phase == QLatin1String("Closed")) {
        forgetRemoteSessionInventory(remoteSessionId);
        terminateRemoteSessionUpload(
            remoteSessionId,
            envelope.value(QStringLiteral("reason")).toString(
                QStringLiteral("Remote session closed")));
        return;
    }

    const bool active = phase == QLatin1String("Active");
    if (active && generation > 0) {
        for (auto target = m_committedAssetsByTarget.begin();
             target != m_committedAssetsByTarget.end(); ++target) {
            for (auto asset = target->begin(); asset != target->end(); ++asset) {
                if (asset->remoteSessionId == remoteSessionId) {
                    asset->generation = generation;
                }
            }
        }
        for (auto removal = m_pendingAssetRemovals.begin();
             removal != m_pendingAssetRemovals.end(); ++removal) {
            if (removal->asset.remoteSessionId == remoteSessionId) {
                removal->asset.generation = generation;
            }
        }

        // Completed incoming uploads have no in-memory transfer to perform the
        // cache descriptor rebind. Advance every matching live scope before a
        // replayed removal or teardown can be accepted in the new generation.
        if (m_remoteCacheStore && m_remoteCacheReady) {
            QString enumerationError;
            const QList<RemoteCacheStore::Scope> scopes =
                m_remoteCacheStore->liveScopes(&enumerationError);
            if (enumerationError.isEmpty()) {
                for (const RemoteCacheStore::Scope& scope : scopes) {
                    if (scope.remoteSessionId == remoteSessionId
                        && scope.generation < generation) {
                        QString ignored;
                        m_remoteCacheStore->rebindSessionGeneration(
                            scope, generation, &ignored);
                    }
                }
            }
        }

        // Re-send only when the authenticated RemoteSession generation has
        // advanced. This recovers both an intent created during Grace and an
        // ACK lost after the target already committed. The removalId and
        // immutable asset tuple stay unchanged, and the original timeout is
        // never restarted.
        for (auto removal = m_pendingAssetRemovals.begin();
             removal != m_pendingAssetRemovals.end(); ++removal) {
            if (removal->asset.remoteSessionId == remoteSessionId
                && removal->lastSentGeneration != generation) {
                sendPendingAssetRemoval(removal.value());
            }
        }
    }
    if (m_uploadScheduler) {
        m_uploadScheduler->setSessionState(
            remoteSessionId,
            active ? UploadScheduler::SessionState::Active
                   : UploadScheduler::SessionState::Grace);
    }
    if (ParallelOutgoingTransfer* parallel = parallelForSession(remoteSessionId)) {
        if (!active) {
            suspendParallel(parallel);
            return;
        }
        if (generation > 0) parallel->generation = generation;
        if (parallel->state == OutgoingState::Suspended) {
            const QString uploadId = parallel->uploadId;
            QTimer::singleShot(0, this, [this, uploadId]() {
                if (ParallelOutgoingTransfer* current = parallelForUpload(uploadId)) {
                    resumeParallel(current);
                }
            });
        }
        return;
    }
    if (remoteSessionId != m_outgoingRemoteSessionId
        || m_currentUploadId.isEmpty()) return;
    if (!active) {
        suspendOutgoingForResume();
        return;
    }
    if (generation > 0) m_outgoingGeneration = generation;
    if (m_outgoingState == OutgoingState::Suspended) {
        QTimer::singleShot(0, this, [this]() { resumeOutgoingUpload(); });
    }
}

void UploadManager::terminateRemoteSessionUpload(
    const QString& remoteSessionId, const QString& reason) {
    if (remoteSessionId.isEmpty()) return;
    forgetRemoteSessionInventory(remoteSessionId);
    if (m_uploadScheduler) m_uploadScheduler->cancelSession(remoteSessionId);
    if (ParallelOutgoingTransfer* parallel = parallelForSession(remoteSessionId)) {
        const QString uploadId = parallel->uploadId;
        const QString targetId = parallel->targetEndpointId;
        if (m_ws) m_ws->cancelUploadId(uploadId);
        if (m_fileManager && !targetId.isEmpty()) {
            m_fileManager->unmarkAllForClient(targetId);
        }
        m_remoteInventoryTargets.remove(targetId);
        removeParallel(parallel, false, false);
        emit uploadRejected(uploadId, reason.left(512));
    }
    if (remoteSessionId != m_outgoingRemoteSessionId) return;

    const QString uploadId = m_currentUploadId;
    stopOutgoingPump();
    if (m_ws) {
        if (!uploadId.isEmpty()) m_ws->cancelUploadId(uploadId);
        if (m_outgoingTransportRegistered) m_ws->endUploadSession();
        m_outgoingTransportRegistered = false;
    }
    if (m_fileManager && !m_uploadTargetClientId.isEmpty()) {
        // Session teardown purges its entire remote inventory, not merely the
        // current incremental batch. This is the only network-loss path which
        // returns project media to Not uploaded.
        m_fileManager->unmarkAllForClient(m_uploadTargetClientId);
    }
    m_remoteInventoryTargets.remove(m_uploadTargetClientId);
    m_uploadActive = false;
    clearOutgoingTransfer(false);
    m_outgoingRemoteSessionId.clear();
    m_outgoingGeneration = 0;
    m_schedulerGeneration = 0;
    if (!uploadId.isEmpty()) {
        emit uploadRejected(uploadId, reason.left(512));
    }
    emit uiStateChanged();
}

QJsonArray UploadManager::outgoingAssetStates(bool complete) const {
    QJsonArray states;
    for (const OutgoingAsset& asset : m_outgoingAssets) {
        states.append(QJsonObject{
            {QStringLiteral("assetId"), asset.assetId},
            {QStringLiteral("offset"), static_cast<double>(
                 complete ? asset.size
                          : m_outgoingDurableOffsets.value(asset.assetId, 0))},
            {QStringLiteral("size"), static_cast<double>(asset.size)},
            {QStringLiteral("sha256"), asset.sha256}
        });
    }
    return states;
}

bool UploadManager::applyAuthoritativeOffsets(const QJsonArray& assets,
                                              bool resetSendCursor,
                                              QString* errorMessage) {
    if (assets.size() != m_outgoingAssets.size()) {
        if (errorMessage) *errorMessage = QStringLiteral("Incomplete upload inventory");
        return false;
    }
    QHash<QString, qint64> offsets;
    for (const QJsonValue& value : assets) {
        if (!value.isObject()) {
            if (errorMessage) *errorMessage = QStringLiteral("Malformed upload inventory");
            return false;
        }
        const QJsonObject state = value.toObject();
        const QString assetId = state.value(QStringLiteral("assetId")).toString();
        auto assetIt = std::find_if(
            m_outgoingAssets.cbegin(), m_outgoingAssets.cend(),
            [&assetId](const OutgoingAsset& candidate) {
                return candidate.assetId == assetId;
            });
        qint64 offset = -1;
        qint64 size = -1;
        if (assetIt == m_outgoingAssets.cend() || offsets.contains(assetId)
            || !parseNonNegativeOffset(state.value(QStringLiteral("offset")), offset)
            || !parseManifestSize(state.value(QStringLiteral("size")), size)
            || size != assetIt->size || offset > size
            || state.value(QStringLiteral("sha256")).toString() != assetIt->sha256
            || offset < m_outgoingDurableOffsets.value(assetId, 0)) {
            if (errorMessage) *errorMessage = QStringLiteral("Mismatched upload inventory");
            return false;
        }
        offsets.insert(assetId, offset);
    }

    qint64 durableBytes = 0;
    int completedAssets = 0;
    for (const OutgoingAsset& asset : m_outgoingAssets) {
        if (!offsets.contains(asset.assetId)) {
            if (errorMessage) *errorMessage = QStringLiteral("Incomplete upload inventory");
            return false;
        }
        const qint64 offset = offsets.value(asset.assetId);
        durableBytes += offset;
        if (offset == asset.size) ++completedAssets;
        const int percent = static_cast<int>(std::round(
            offset * 100.0 / static_cast<double>(asset.size)));
        for (const QString& localFileId : asset.localFileIds) {
            updatePerFileRemoteProgress(localFileId, percent);
        }
    }
    m_outgoingDurableOffsets = offsets;
    m_remoteAcknowledgedBytes = durableBytes;
    const int percent = m_totalBytes > 0
        ? std::clamp(static_cast<int>(std::round(
              durableBytes * 100.0 / static_cast<double>(m_totalBytes))), 0, 100)
        : 0;
    updateRemoteProgress(percent, completedAssets);

    if (resetSendCursor) {
        if (m_outgoingFileHandle.isOpen()) m_outgoingFileHandle.close();
        m_sentBytes = durableBytes;
        m_outgoingFileIndex = 0;
        while (m_outgoingFileIndex < m_outgoingAssets.size()
               && offsets.value(m_outgoingAssets.at(m_outgoingFileIndex).assetId)
                    == m_outgoingAssets.at(m_outgoingFileIndex).size) {
            ++m_outgoingFileIndex;
        }
        m_outgoingSentForFile = m_outgoingFileIndex < m_outgoingAssets.size()
            ? offsets.value(m_outgoingAssets.at(m_outgoingFileIndex).assetId)
            : 0;
        m_outgoingChunkIndex = static_cast<int>(
            m_outgoingSentForFile / (128 * 1024));
        m_outgoingPayloadCompleteSent = false;
    }
    return true;
}

void UploadManager::releaseSchedulerSlot(bool success) {
    if (!m_uploadScheduler || m_outgoingRemoteSessionId.isEmpty()
        || m_currentUploadId.isEmpty() || m_schedulerGeneration == 0) return;
    if (m_uploadScheduler->isUploadActive(
            m_outgoingRemoteSessionId, m_schedulerGeneration, m_currentUploadId)) {
        if (success) {
            m_uploadScheduler->completeUpload(
                m_outgoingRemoteSessionId, m_schedulerGeneration, m_currentUploadId);
        } else {
            m_uploadScheduler->failUpload(
                m_outgoingRemoteSessionId, m_schedulerGeneration, m_currentUploadId);
        }
    } else {
        m_uploadScheduler->cancelUpload(
            m_outgoingRemoteSessionId, m_schedulerGeneration, m_currentUploadId);
    }
}

void UploadManager::clearOutgoingTransfer(bool preserveRemoteInventory) {
    stopOutgoingPump();
    if (m_ws && m_outgoingTransportRegistered) m_ws->endUploadSession();
    m_outgoingTransportRegistered = false;
    m_uploadActive = preserveRemoteInventory;
    m_uploadWasActiveBeforeStart = false;
    m_currentUploadId.clear();
    m_outgoingFiles.clear();
    m_outgoingAssets.clear();
    m_outgoingManifest = QJsonArray();
    m_outgoingDurableOffsets.clear();
    m_outgoingFileIndex = 0;
    m_outgoingChunkIndex = 0;
    m_outgoingSentForFile = 0;
    m_outgoingPayloadCompleteSent = false;
    m_waitingForResume = false;
    m_stateBeforeSuspend = OutgoingState::Idle;
    m_lastPercent = 0;
    m_filesCompleted = 0;
    m_totalFiles = 0;
    m_sentBytes = 0;
    m_totalBytes = 0;
    m_remoteAcknowledgedBytes = 0;
    m_remoteProgressReceived = false;
    resetProgressTracking();
    setOutgoingState(OutgoingState::Idle);
}

void UploadManager::scheduleOutgoingPump() {
    if (!m_outgoingPumpTimer || m_outgoingPumpTimer->isActive()
        || m_outgoingState != OutgoingState::Streaming
        || m_outgoingPayloadCompleteSent
        || m_currentUploadId.isEmpty()) {
        return;
    }
    m_outgoingPumpTimer->start();
}

void UploadManager::stopOutgoingPump() {
    if (m_outgoingPumpTimer) m_outgoingPumpTimer->stop();
    if (m_outgoingStallTimer) m_outgoingStallTimer->stop();
    if (m_outgoingStartAckTimer) m_outgoingStartAckTimer->stop();
    if (m_outgoingAckTimer) m_outgoingAckTimer->stop();
    if (m_outgoingFileHandle.isOpen()) m_outgoingFileHandle.close();
}

void UploadManager::failOutgoingUpload(const QString& reason) {
    const QString uploadId = m_currentUploadId;
    if (uploadId.isEmpty()) return;

    stopOutgoingPump();
    if (m_ws && m_outgoingState != OutgoingState::Queued
        && !m_outgoingRemoteSessionId.isEmpty() && m_outgoingGeneration > 0) {
        // WebSocketClient keeps termination ordered on the pinned transport,
        // falling back to the authenticated control channel after transport loss.
        m_ws->sendUploadAbort(m_outgoingRemoteSessionId, m_outgoingGeneration,
                              uploadId, reason);
    }
    onUploadRejected(uploadId, reason);
}

void UploadManager::pumpOutgoingUpload() {
    if (m_outgoingPumpRunning || m_outgoingState != OutgoingState::Streaming
        || m_outgoingPayloadCompleteSent
        || m_currentUploadId.isEmpty()) {
        return;
    }
    QScopedValueRollback<bool> pumpGuard(m_outgoingPumpRunning, true);

    if (!m_ws || !m_ws->isUploadSessionTransportAvailable()) {
        failOutgoingUpload(QStringLiteral("Upload transport was interrupted"));
        return;
    }

    constexpr qint64 chunkBytes = 128 * 1024;
    constexpr qint64 maximumChunkWireBytes = ((chunkBytes + 2) / 3) * 4 + 4096;
    int chunksQueuedThisPass = 0;

    while (chunksQueuedThisPass < kMaxChunksPerPump
           && m_outgoingState == OutgoingState::Streaming
           && !m_outgoingPayloadCompleteSent) {
        const qint64 queuedBytes = m_ws->uploadTransportBytesToWrite();
        if (queuedBytes < 0) {
            failOutgoingUpload(QStringLiteral("Upload transport was interrupted"));
            return;
        }
        if (queuedBytes > kMaxQueuedUploadBytes - maximumChunkWireBytes) {
            // bytesWritten will schedule the next pump. The stall timer covers
            // a peer or network that stops draining the bounded queue.
            return;
        }
        if (m_sentBytes - m_remoteAcknowledgedBytes
            >= kMaxUnacknowledgedRemoteBytes) {
            // The target's receipt acknowledgement, not merely the local TCP
            // queue, controls this window. This bounds memory on the relay and
            // target even when they are much slower than the sender.
            return;
        }

        if (m_outgoingFileIndex >= m_outgoingAssets.size()) {
            if (m_sentBytes != m_totalBytes) {
                failOutgoingUpload(QStringLiteral("Source files were not read completely"));
                return;
            }
            if (!m_ws->sendUploadComplete(m_outgoingRemoteSessionId,
                                          m_outgoingGeneration,
                                          m_currentUploadId,
                                          outgoingAssetStates(true))) {
                failOutgoingUpload(QStringLiteral(
                    "Upload transport was interrupted before completion"));
                return;
            }

            m_outgoingPayloadCompleteSent = true;
            setOutgoingState(OutgoingState::AwaitingValidation);
            if (m_outgoingPumpTimer) m_outgoingPumpTimer->stop();
            if (m_outgoingStallTimer) m_outgoingStallTimer->stop();
            if (m_outgoingAckTimer && m_outgoingAckTimer->interval() > 0) {
                m_outgoingAckTimer->start();
            }
            emit uiStateChanged();
            return;
        }

        const OutgoingAsset fileInfo = m_outgoingAssets.at(m_outgoingFileIndex);
        if (!m_outgoingFileHandle.isOpen()) {
            m_outgoingFileHandle.setFileName(fileInfo.path);
            if (!m_outgoingFileHandle.open(QIODevice::ReadOnly)) {
                failOutgoingUpload(QStringLiteral("Could not open source file: %1")
                                       .arg(fileInfo.name));
                return;
            }
            if (fileInfo.size < 1 || m_outgoingFileHandle.size() != fileInfo.size) {
                failOutgoingUpload(QStringLiteral("Source file changed before upload: %1")
                                       .arg(fileInfo.name));
                return;
            }
            if (!m_outgoingFileHandle.seek(m_outgoingSentForFile)) {
                failOutgoingUpload(QStringLiteral("Could not seek source asset: %1")
                                       .arg(fileInfo.name));
                return;
            }
            m_outgoingChunkIndex = static_cast<int>(
                m_outgoingSentForFile / (128 * 1024));
            for (const QString& localFileId : fileInfo.localFileIds) {
                emit fileUploadStarted(localFileId);
            }
            if (m_outgoingState != OutgoingState::Streaming) {
                return;
            }
        }

        const qint64 remaining = fileInfo.size - m_outgoingSentForFile;
        if (remaining <= 0) {
            failOutgoingUpload(QStringLiteral("Invalid source file state: %1")
                                   .arg(fileInfo.name));
            return;
        }
        const qint64 requestedBytes = std::min(chunkBytes, remaining);
        const QByteArray chunk = m_outgoingFileHandle.read(requestedBytes);
        if (chunk.size() != requestedBytes
            || m_outgoingFileHandle.error() != QFileDevice::NoError) {
            failOutgoingUpload(QStringLiteral("Could not read source file: %1")
                                   .arg(fileInfo.name));
            return;
        }

        const qint64 offset = m_outgoingSentForFile;
        if (!m_ws->sendUploadChunk(m_outgoingRemoteSessionId, m_outgoingGeneration,
                                   m_currentUploadId, fileInfo.assetId, offset,
                                   fileInfo.sha256, chunk)) {
            failOutgoingUpload(QStringLiteral("Upload transport was interrupted"));
            return;
        }
        ++m_outgoingChunkIndex;
        ++chunksQueuedThisPass;
        m_outgoingSentForFile += chunk.size();
        m_sentBytes += chunk.size();
        if (m_outgoingStallTimer && m_outgoingStallTimer->interval() > 0) {
            m_outgoingStallTimer->start();
        }

        const int filePercent = static_cast<int>(std::round(
            m_outgoingSentForFile * 100.0 / static_cast<double>(fileInfo.size)));
        for (const QString& localFileId : fileInfo.localFileIds) {
            updatePerFileLocalProgress(localFileId, filePercent);
        }
        if (m_outgoingState != OutgoingState::Streaming) {
            return;
        }

        const int globalPercent = m_totalBytes > 0
            ? std::clamp(static_cast<int>(std::round(
                  m_sentBytes * 100.0 / static_cast<double>(m_totalBytes))), 0, 99)
            : 0;
        updateLocalProgress(globalPercent, m_outgoingFileIndex);
        if (m_outgoingState != OutgoingState::Streaming) {
            return;
        }

        if (m_outgoingSentForFile == fileInfo.size) {
            const bool exactSourceStillPresent = m_outgoingFileHandle.size() == fileInfo.size
                && m_outgoingFileHandle.pos() == fileInfo.size
                && m_outgoingFileHandle.error() == QFileDevice::NoError;
            m_outgoingFileHandle.close();
            if (!exactSourceStillPresent) {
                failOutgoingUpload(QStringLiteral("Source file changed during upload: %1")
                                       .arg(fileInfo.name));
                return;
            }

            ++m_outgoingFileIndex;
            m_outgoingChunkIndex = 0;
            m_outgoingSentForFile = 0;
            for (const QString& localFileId : fileInfo.localFileIds) {
                updatePerFileLocalProgress(localFileId, 99);
            }
            updateLocalProgress(globalPercent, m_outgoingFileIndex);
            for (const QString& localFileId : fileInfo.localFileIds) {
                emit fileUploadFinished(localFileId);
            }
            if (m_outgoingState != OutgoingState::Streaming) {
                return;
            }
        }
    }

    if (m_outgoingState == OutgoingState::Streaming
        && !m_outgoingPayloadCompleteSent) {
        scheduleOutgoingPump();
    }
}

// collectSceneFiles removed; files now gathered by caller (MainWindow)

void UploadManager::resetToInitial() {
    if (!m_currentUploadId.isEmpty()) releaseSchedulerSlot(false);
    clearOutgoingTransfer(false);
    if (m_cancelFallbackTimer) m_cancelFallbackTimer->stop();
    m_uploadTargetClientId.clear();
    m_outgoingRemoteSessionId.clear();
    m_outgoingGeneration = 0;
    m_schedulerGeneration = 0;
    m_activeSessionIdentity.clear();
    m_activeIdeaId.clear();
}

void UploadManager::finishLocalCancellation() {
    if (m_outgoingState != OutgoingState::Cancelling) return;
    const QString cancelledUploadId = m_currentUploadId;
    const bool preserveExistingRemoteFiles = m_uploadWasActiveBeforeStart;

    if (m_cancelFallbackTimer) m_cancelFallbackTimer->stop();
    if (m_ws) {
        m_ws->cancelUploadId(cancelledUploadId);
    }
    if (m_uploadScheduler && !m_outgoingRemoteSessionId.isEmpty()) {
        m_uploadScheduler->cancelUpload(m_outgoingRemoteSessionId,
                                        m_schedulerGeneration,
                                        cancelledUploadId);
    }
    clearOutgoingTransfer(preserveExistingRemoteFiles);

    emit uploadCancelled(cancelledUploadId);
    emit uiStateChanged();
}

void UploadManager::resetProgressTracking() {
    m_lastLocalPercent = 0;
    m_lastLocalFilesCompleted = 0;
    m_lastRemotePercent = 0;
    m_lastRemoteFilesCompleted = 0;
    m_effectivePercent = -1;
    m_effectiveFilesCompleted = -1;
    m_localFilePercents.clear();
    m_remoteFilePercents.clear();
    m_effectiveFilePercents.clear();
}

void UploadManager::updateLocalProgress(int percent, int filesCompleted) {
    if (m_totalFiles <= 0) return;
    percent = std::clamp(percent, 0, 99);
    filesCompleted = std::clamp(filesCompleted, 0, m_totalFiles);
    if (percent > m_lastLocalPercent) {
        m_lastLocalPercent = percent;
    }
    if (filesCompleted > m_lastLocalFilesCompleted) {
        m_lastLocalFilesCompleted = filesCompleted;
    }
    emitEffectiveProgressIfChanged();
}

void UploadManager::updateRemoteProgress(int percent, int filesCompleted) {
    if (m_totalFiles <= 0) m_totalFiles = std::max(0, filesCompleted);
    percent = std::clamp(percent, 0, 100);
    filesCompleted = std::clamp(filesCompleted, 0, std::max(1, m_totalFiles));
    if (percent > m_lastRemotePercent) {
        m_lastRemotePercent = percent;
    }
    if (filesCompleted > m_lastRemoteFilesCompleted) {
        m_lastRemoteFilesCompleted = filesCompleted;
    }
    m_remoteProgressReceived = true;
    emitEffectiveProgressIfChanged();
}

void UploadManager::emitEffectiveProgressIfChanged() {
    if (m_totalFiles <= 0 || m_uploadTargetClientId != m_targetClientId) return;
    const int effectivePercent = std::clamp(std::max(m_lastLocalPercent, m_lastRemotePercent), 0, m_remoteProgressReceived ? 100 : 99);
    // A locally-sent file is not complete until the target has validated it.
    const int effectiveFilesCompleted = std::clamp(m_remoteProgressReceived ? m_lastRemoteFilesCompleted : 0,
                                                  0,
                                                  m_totalFiles);
    if (effectivePercent == m_effectivePercent && effectiveFilesCompleted == m_effectiveFilesCompleted) {
        return;
    }
    m_effectivePercent = effectivePercent;
    m_effectiveFilesCompleted = effectiveFilesCompleted;
    emit uploadProgress(m_effectivePercent, m_effectiveFilesCompleted, m_totalFiles);
}

void UploadManager::updatePerFileLocalProgress(const QString& fileId, int percent) {
    if (fileId.isEmpty()) return;
    percent = std::clamp(percent, 0, 99);
    int& localEntry = m_localFilePercents[fileId];
    if (percent <= localEntry) return;
    localEntry = percent;
    emitEffectivePerFileProgress(fileId);
}

void UploadManager::updatePerFileRemoteProgress(const QString& fileId, int percent) {
    if (fileId.isEmpty()) return;
    percent = std::clamp(percent, 0, 100);
    int& remoteEntry = m_remoteFilePercents[fileId];
    if (percent <= remoteEntry) return;
    remoteEntry = percent;
    emitEffectivePerFileProgress(fileId);
}

void UploadManager::emitEffectivePerFileProgress(const QString& fileId) {
    if (m_uploadTargetClientId != m_targetClientId) return;
    const int local = m_localFilePercents.value(fileId, 0);
    const int remote = m_remoteFilePercents.value(fileId, 0);
    int effective = std::max(local, remote);
    if (remote >= 100) {
        effective = 100;
    } else {
        effective = std::clamp(effective, 0, 99);
    }
    int& cached = m_effectiveFilePercents[fileId];
    if (effective == cached) return;
    cached = effective;
    emit fileUploadProgress(fileId, effective);
}

void UploadManager::clearIncomingChunkTracking(const QString& uploadId) {
    if (uploadId.isEmpty()) return;
    const QString prefix = uploadId + QLatin1Char(':');
    auto it = m_expectedChunkIndex.begin();
    while (it != m_expectedChunkIndex.end()) {
        if (it.key().startsWith(prefix)) it = m_expectedChunkIndex.erase(it);
        else ++it;
    }
}

void UploadManager::closeIncomingFiles(bool flush)
{
    for (auto it = m_incoming.openFiles.begin(); it != m_incoming.openFiles.end(); ++it) {
        QFile* file = it.value();
        if (!file) continue;
        if (flush && file->isOpen()) {
            syncFile(file);
        }
        file->close();
        delete file;
    }
    m_incoming.openFiles.clear();
}

void UploadManager::suspendIncomingForResume()
{
    if (m_incoming.uploadId.isEmpty()) return;
    closeIncomingFiles(true);
    m_incoming.suspendedForResume = true;
    if (m_incomingStallTimer) m_incomingStallTimer->stop();
}

QJsonArray UploadManager::incomingAssetOffsets() const
{
    QJsonArray assets;
    QStringList assetIds = m_incoming.expectedSizes.keys();
    std::sort(assetIds.begin(), assetIds.end());
    for (const QString& assetId : assetIds) {
        QJsonObject asset;
        asset.insert(QStringLiteral("assetId"), assetId);
        asset.insert(QStringLiteral("offset"),
                     static_cast<double>(m_incoming.receivedByFile.value(assetId, 0)));
        asset.insert(QStringLiteral("size"),
                     static_cast<double>(m_incoming.expectedSizes.value(assetId, 0)));
        asset.insert(QStringLiteral("sha256"),
                     m_incoming.assetIdToSha256.value(assetId));
        assets.append(asset);
    }
    return assets;
}

void UploadManager::pruneIncomingUploadCompletions(qint64 nowEpochMs)
{
    for (auto it = m_incomingUploadCompletionTombstones.begin();
         it != m_incomingUploadCompletionTombstones.end();) {
        if (it->expiresAtEpochMs <= nowEpochMs) {
            it = m_incomingUploadCompletionTombstones.erase(it);
        } else {
            ++it;
        }
    }
    while (m_incomingUploadCompletionTombstones.size()
           > kMaxIncomingCompletionTombstones) {
        auto oldest = m_incomingUploadCompletionTombstones.begin();
        for (auto it = oldest; it != m_incomingUploadCompletionTombstones.end(); ++it) {
            if (it->expiresAtEpochMs < oldest->expiresAtEpochMs) oldest = it;
        }
        m_incomingUploadCompletionTombstones.erase(oldest);
    }
}

void UploadManager::rememberIncomingUploadCompletion(
    const QString& senderEndpointId,
    const QString& remoteSessionId,
    quint64 generation,
    quint64 sourceConnectionGeneration,
    const QString& uploadId,
    const QJsonArray& assets)
{
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    pruneIncomingUploadCompletions(now);
    IncomingUploadCompletionTombstone tombstone;
    tombstone.senderEndpointId = senderEndpointId;
    tombstone.remoteSessionId = remoteSessionId;
    tombstone.generation = generation;
    tombstone.sourceConnectionGeneration = sourceConnectionGeneration;
    tombstone.uploadId = uploadId;
    tombstone.assets = assets;
    tombstone.expiresAtEpochMs = now + kIncomingCompletionTombstoneTtlMs;
    m_incomingUploadCompletionTombstones.insert(uploadId, tombstone);
    pruneIncomingUploadCompletions(now);
}

bool UploadManager::replayIncomingUploadCompletion(
    const QJsonObject& message,
    const QString& senderEndpointId,
    const QString& remoteSessionId,
    quint64 generation,
    quint64 sourceConnectionGeneration)
{
    pruneIncomingUploadCompletions(QDateTime::currentMSecsSinceEpoch());
    const QString uploadId = message.value(QStringLiteral("uploadId")).toString();
    auto completed = m_incomingUploadCompletionTombstones.find(uploadId);
    if (completed == m_incomingUploadCompletionTombstones.end()
        || completed->senderEndpointId != senderEndpointId
        || completed->remoteSessionId != remoteSessionId
        || generation < completed->generation
        || (generation == completed->generation
            && sourceConnectionGeneration != completed->sourceConnectionGeneration)
        || !message.value(QStringLiteral("assets")).isArray()
        || message.value(QStringLiteral("assets")).toArray() != completed->assets) {
        return false;
    }

    if (m_ws && m_ws->remoteSessionCoordinator()) {
        const RemoteSessionCoordinator::Binding binding =
            m_ws->remoteSessionCoordinator()->byId(remoteSessionId);
        if (!binding.active || binding.generation != generation
            || binding.ownerEndpointId != senderEndpointId
            || binding.targetEndpointId != m_ws->endpointId()
            || binding.ownerConnectionGeneration != sourceConnectionGeneration) {
            return false;
        }
    }

    completed->generation = generation;
    completed->sourceConnectionGeneration = sourceConnectionGeneration;
    emitIncomingV3Response(QStringLiteral("upload_finished"), senderEndpointId,
                           remoteSessionId, generation, uploadId,
                           {{QStringLiteral("assets"), completed->assets},
                            {QStringLiteral("replay"), true}});
    return true;
}

void UploadManager::forgetIncomingUploadCompletions(
    const QString& remoteSessionId)
{
    for (auto it = m_incomingUploadCompletionTombstones.begin();
         it != m_incomingUploadCompletionTombstones.end();) {
        if (it->remoteSessionId == remoteSessionId) {
            it = m_incomingUploadCompletionTombstones.erase(it);
        } else {
            ++it;
        }
    }
}

void UploadManager::emitIncomingV3Response(const QString& type,
                                           const QString& senderEndpointId,
                                           const QString& remoteSessionId,
                                           quint64 generation,
                                           const QString& uploadId,
                                           const QJsonObject& extra)
{
    QJsonObject response = extra;
    response.insert(QStringLiteral("type"), type);
    Q_UNUSED(senderEndpointId);
    response.insert(QStringLiteral("remoteSessionId"), remoteSessionId);
    response.insert(QStringLiteral("generation"), static_cast<double>(generation));
    response.insert(QStringLiteral("uploadId"), uploadId);
    emit protocolV3UploadResponseReady(response);
}

RemoteCacheStore::CommitResult UploadManager::teardownRemoteSession(
    const QString& senderEndpointId,
    const QString& remoteSessionId,
    quint64 generation,
    const QString& teardownId)
{
    m_lastTeardownRemovedFileCount = 0;
    forgetIncomingUploadCompletions(remoteSessionId);
    const RemoteCacheStore::Scope scope {
        senderEndpointId, remoteSessionId, generation
    };
    if (!m_remoteCacheStore || !m_remoteCacheReady) {
        RemoteCacheStore::CommitResult unavailable;
        unavailable.outcome = RemoteCacheStore::CommitOutcome::CleanupError;
        unavailable.teardownId = teardownId;
        unavailable.errorCode = QStringLiteral("cache_store_not_initialized");
        emit remoteSessionCacheCleanupError(senderEndpointId, remoteSessionId,
                                            generation, teardownId,
                                            unavailable.errorCode);
        return unavailable;
    }

    QString beginError;
    if (!m_remoteCacheStore->beginTeardown(scope, teardownId, &beginError)) {
        const RemoteCacheStore::CommitResult replay =
            m_remoteCacheStore->commitTeardown(scope, teardownId);
        if (!replay.acknowledgementSafe()) {
            emit remoteSessionCacheCleanupError(senderEndpointId, remoteSessionId,
                                                generation, teardownId,
                                                replay.errorCode);
        }
        return replay;
    }

    if (m_incoming.senderId == senderEndpointId
        && m_incoming.remoteSessionId == remoteSessionId) {
        const QString uploadId = m_incoming.uploadId;
        closeIncomingFiles(false);
        clearIncomingChunkTracking(uploadId);
        m_canceledIncoming.remove(uploadId);
        m_incoming = IncomingUploadSession();
        if (m_incomingStallTimer) m_incomingStallTimer->stop();
    }

    m_lastTeardownRemovedFileCount = detachReceivedMappingsForScope(scope);

    const RemoteCacheStore::CommitResult committed =
        m_remoteCacheStore->commitTeardown(scope, teardownId);
    if (committed.acknowledgementSafe()) {
        emit remoteSessionCacheCommitted(senderEndpointId, remoteSessionId,
                                         generation, committed.teardownId,
                                         m_lastTeardownRemovedFileCount,
                                         committed.quarantinedBytes);
    } else {
        emit remoteSessionCacheCleanupError(senderEndpointId, remoteSessionId,
                                            generation, committed.teardownId,
                                            committed.errorCode);
    }
    return committed;
}

int UploadManager::detachReceivedMappingsForScope(
    const RemoteCacheStore::Scope& scope)
{
    if (!m_fileManager || !m_remoteCacheStore) {
        return 0;
    }
    int removed = 0;
    const QList<QString> fileIds = m_fileManager->getAllFileIds();
    for (const QString& fileId : fileIds) {
        const QString mappedPath = m_fileManager->getFilePathForId(fileId);
        if (!mappedPath.isEmpty()
            && m_remoteCacheStore->ownsPath(scope, mappedPath)) {
            m_fileManager->removeReceivedFileMapping(fileId);
            ++removed;
        }
    }
    return removed;
}

void UploadManager::beginTerminalIncomingCleanup(const QString& reasonCode)
{
    // This is only the command/writer barrier. Render-facing mappings and the
    // live cache namespace deliberately remain intact until MainWindow has
    // observed RemoteSceneController::teardownSettled for every incoming
    // RemoteSession that existed at the terminal edge.
    m_terminalIncomingCleanupAwaitingRenderer = true;
    m_receiverAdvertisementReady = false;
    m_receiverCleanupReason = reasonCode.trimmed().isEmpty()
        ? QStringLiteral("terminal_session") : reasonCode.trimmed();
    m_receiverCleanupError = QStringLiteral("renderer_teardown_pending");
    m_incomingUploadCompletionTombstones.clear();
    suspendIncomingForResume();
    emit receiverAdvertisementReadinessChanged(false, m_receiverCleanupError);
}

UploadManager::BulkTeardownResult
UploadManager::completeTerminalIncomingCleanup(const QString& reasonCode)
{
    // Contract: the application calls this only from the renderer settlement
    // barrier. Clearing the guard first makes a failed logical commit eligible
    // for the explicit recovery path, while advertisement remains fail-closed.
    m_terminalIncomingCleanupAwaitingRenderer = false;
    if (!reasonCode.trimmed().isEmpty()) {
        m_receiverCleanupReason = reasonCode.trimmed();
    }

    const BulkTeardownResult result =
        teardownAllIncomingRemoteSessions(m_receiverCleanupReason);
    QString logicalCleanupError;
    const bool logicallySafe = m_remoteCacheStore && m_remoteCacheReady
        && m_remoteCacheStore->receiverAdvertisementSafe(&logicalCleanupError);
    m_receiverAdvertisementReady = result.allLogicallyCommitted()
        && logicallySafe;
    m_receiverCleanupError = m_receiverAdvertisementReady
        ? QString()
        : (!result.errorCode.isEmpty() ? result.errorCode
           : (!logicalCleanupError.isEmpty() ? logicalCleanupError
              : QStringLiteral("cleanup_error")));
    emit receiverAdvertisementReadinessChanged(
        receiverReadyForAdvertisement(), m_receiverCleanupError);
    return result;
}

UploadManager::BulkTeardownResult
UploadManager::teardownAllIncomingRemoteSessions(const QString& reasonCode)
{
    BulkTeardownResult summary;
    m_lastTeardownRemovedFileCount = 0;
    if (!m_remoteCacheStore || !m_remoteCacheReady) {
        summary.errorCode = QStringLiteral("cache_store_not_initialized");
        summary.cleanupErrorScopes = 1;
        return summary;
    }

    QString enumerationError;
    const QList<RemoteCacheStore::Scope> scopes =
        m_remoteCacheStore->liveScopes(&enumerationError);
    if (!enumerationError.isEmpty()) {
        summary.errorCode = enumerationError;
        summary.cleanupErrorScopes = 1;
    }
    summary.discoveredScopes = scopes.size();

    for (const RemoteCacheStore::Scope& scope : scopes) {
        const QString teardownId =
            QUuid::createUuid().toString(QUuid::WithoutBraces).toLower();
        QString beginError;
        if (!m_remoteCacheStore->beginProvisionalTeardown(
                scope, teardownId, reasonCode, &beginError)) {
            ++summary.cleanupErrorScopes;
            if (summary.errorCode.isEmpty()) {
                summary.errorCode = beginError;
            }
            emit remoteSessionCacheCleanupError(
                scope.senderEndpointId, scope.remoteSessionId, scope.generation,
                teardownId, beginError);
            continue;
        }

        // Persisting the intent is the command barrier. Only then is it safe
        // to detach the active writer and render-facing FileManager mappings.
        if (m_incoming.senderId == scope.senderEndpointId
            && m_incoming.remoteSessionId == scope.remoteSessionId) {
            const QString uploadId = m_incoming.uploadId;
            closeIncomingFiles(false);
            clearIncomingChunkTracking(uploadId);
            m_canceledIncoming.remove(uploadId);
            m_incoming = IncomingUploadSession();
            if (m_incomingStallTimer) {
                m_incomingStallTimer->stop();
            }
        }

        const int removedMappings = detachReceivedMappingsForScope(scope);
        summary.removedFileMappings += removedMappings;
        const RemoteCacheStore::CommitResult committed =
            m_remoteCacheStore->commitTeardown(scope, teardownId);
        summary.quarantinedBytes += committed.quarantinedBytes;
        if (committed.acknowledgementSafe()) {
            ++summary.committedScopes;
            emit remoteSessionCacheCommitted(
                scope.senderEndpointId, scope.remoteSessionId, scope.generation,
                committed.teardownId, removedMappings,
                committed.quarantinedBytes);
        } else {
            ++summary.cleanupErrorScopes;
            if (summary.errorCode.isEmpty()) {
                summary.errorCode = committed.errorCode;
            }
            emit remoteSessionCacheCleanupError(
                scope.senderEndpointId, scope.remoteSessionId, scope.generation,
                committed.teardownId, committed.errorCode);
        }
    }
    m_lastTeardownRemovedFileCount = summary.removedFileMappings;
    return summary;
}

bool UploadManager::retryReceiverAdvertisementCleanup()
{
    if (receiverReadyForAdvertisement()) return true;
    if (m_terminalIncomingCleanupAwaitingRenderer) {
        m_receiverCleanupError = QStringLiteral("renderer_teardown_pending");
        emit receiverAdvertisementReadinessChanged(false,
                                                   m_receiverCleanupError);
        return false;
    }

    // initialize() is intentionally replayable: it retries durable teardown
    // intents as well as a previously failed directory initialization. Merely
    // enumerating liveScopes() is insufficient because a failed intent already
    // removes that scope from the live inventory.
    QString initializationError;
    m_remoteCacheReady = m_remoteCacheStore
        && m_remoteCacheStore->initialize(&initializationError);
    if (!m_remoteCacheReady) {
        m_receiverAdvertisementReady = false;
        m_receiverCleanupError = initializationError.isEmpty()
            ? QStringLiteral("cache_store_not_initialized")
            : initializationError;
        emit receiverAdvertisementReadinessChanged(
            false, m_receiverCleanupError);
        return false;
    }

    QString logicalCleanupError;
    if (!m_remoteCacheStore->receiverAdvertisementSafe(&logicalCleanupError)) {
        m_receiverAdvertisementReady = false;
        m_receiverCleanupError = logicalCleanupError.isEmpty()
            ? QStringLiteral("logical_cleanup_not_committed")
            : logicalCleanupError;
        emit receiverAdvertisementReadinessChanged(
            false, m_receiverCleanupError);
        return false;
    }

    const BulkTeardownResult result =
        teardownAllIncomingRemoteSessions(m_receiverCleanupReason);
    logicalCleanupError.clear();
    const bool logicallySafe = m_remoteCacheStore->receiverAdvertisementSafe(
        &logicalCleanupError);
    m_receiverAdvertisementReady = result.allLogicallyCommitted()
        && logicallySafe;
    m_receiverCleanupError = m_receiverAdvertisementReady
        ? QString()
        : (!result.errorCode.isEmpty() ? result.errorCode
           : (!logicalCleanupError.isEmpty() ? logicalCleanupError
              : QStringLiteral("cleanup_error")));
    emit receiverAdvertisementReadinessChanged(
        receiverReadyForAdvertisement(), m_receiverCleanupError);
    return receiverReadyForAdvertisement();
}

bool UploadManager::discardActiveIncomingSession(bool rememberRejectedUpload) {
    const QString senderId = m_incoming.senderId;
    const QString uploadId = m_incoming.uploadId;
    const QString remoteSessionId = m_incoming.remoteSessionId;
    const quint64 generation = m_incoming.generation;
    const QString cacheDirPath = m_incoming.cacheDirPath;
    const QHash<QString, QString> ownedPaths = m_incoming.filePaths;
    bool cleanupSucceeded = true;

    for (auto it = m_incoming.openFiles.begin(); it != m_incoming.openFiles.end(); ++it) {
        if (!it.value()) continue;
        it.value()->close();
        delete it.value();
    }
    m_incoming.openFiles.clear();

    for (auto it = ownedPaths.constBegin(); it != ownedPaths.constEnd(); ++it) {
        const QString path = it.value();
        const RemoteCacheStore::Scope scope{senderId, remoteSessionId, generation};
        const bool v3Owned = !remoteSessionId.isEmpty() && m_remoteCacheStore
            && m_remoteCacheStore->ownsPath(scope, path);
        if (path.isEmpty() || cacheDirPath.isEmpty()
            || !pathIsInsideDirectory(path, cacheDirPath)
            || (!remoteSessionId.isEmpty() && !v3Owned)) {
            cleanupSucceeded = false;
            continue;
        }

        const QString fileId = m_incoming.assetIdToFileId.value(it.key());
        const QString mappedPath = m_fileManager->getFilePathForId(fileId);
        const bool removed = !QFileInfo::exists(path) || QFile::remove(path);
        if (!removed) {
            qWarning() << "UploadManager: could not remove partial upload file";
            cleanupSucceeded = false;
            continue;
        }
        if (!mappedPath.isEmpty()
            && QDir::cleanPath(QFileInfo(mappedPath).absoluteFilePath())
                == QDir::cleanPath(QFileInfo(path).absoluteFilePath())) {
            m_fileManager->removeReceivedFileMapping(fileId);
        }
    }

    const QString activeRoot = m_remoteCacheStore
        ? m_remoteCacheStore->rootPath() : incomingUploadsRoot();
    if (!cacheDirPath.isEmpty() && pathIsInsideDirectory(cacheDirPath, activeRoot)) {
        QDir directory(cacheDirPath);
        if (directory.exists()) {
            if (!directory.entryInfoList(QDir::AllEntries | QDir::NoDotAndDotDot
                                         | QDir::Hidden | QDir::System).isEmpty()) {
                cleanupSucceeded = false;
            } else {
                const QString senderDirectoryPath = QFileInfo(cacheDirPath).absolutePath();
                if (!QDir().rmdir(cacheDirPath)) {
                    cleanupSucceeded = false;
                } else if (pathIsInsideDirectory(senderDirectoryPath, activeRoot)) {
                    QDir senderDirectory(senderDirectoryPath);
                    if (senderDirectory.entryInfoList(
                            QDir::AllEntries | QDir::NoDotAndDotDot).isEmpty()) {
                        QDir().rmdir(senderDirectoryPath);
                    }
                }
            }
        }
    } else if (!cacheDirPath.isEmpty()) {
        cleanupSucceeded = false;
    }

    clearIncomingChunkTracking(uploadId);
    m_incoming = IncomingUploadSession();
    if (m_incomingStallTimer) m_incomingStallTimer->stop();

    if (rememberRejectedUpload && !uploadId.isEmpty() && uploadId.size() <= 128) {
        if (m_canceledIncoming.size() >= 256) m_canceledIncoming.clear();
        m_canceledIncoming.insert(uploadId);
    }
    if (!cleanupSucceeded && remoteSessionId.isEmpty()
        && !senderId.isEmpty() && !uploadId.isEmpty()) {
        QTimer::singleShot(1000, this, [this, senderId, uploadId]() {
            if (!removeResidualIncomingStaging(senderId, uploadId)) {
                qWarning() << "UploadManager: deferred partial-upload cleanup still failed"
                           << "uploadId" << uploadId.left(16);
            }
        });
    }
    return cleanupSucceeded;
}

bool UploadManager::removeResidualIncomingStaging(const QString& senderId,
                                                  const QString& uploadId) {
    if (!isValidPeerId(senderId) || !isCanonicalUuid(uploadId)) return false;
    const QString rootPath = incomingUploadsRoot();
    const QString senderPath = QDir(rootPath).absoluteFilePath(senderId);
    const QString stagingPath = QDir(senderPath).absoluteFilePath(uploadId);
    const QFileInfo stagingInfo(stagingPath);
    if (!stagingInfo.exists()) return true;

    const QFileInfo senderInfo(senderPath);
    const QString senderCanonical = senderInfo.canonicalFilePath();
    const QString stagingCanonical = stagingInfo.canonicalFilePath();
    if (senderCanonical.isEmpty() || stagingCanonical.isEmpty()
        || !senderInfo.isDir() || senderInfo.isSymLink()
        || !stagingInfo.isDir() || stagingInfo.isSymLink()
        || !pathIsInsideDirectory(senderCanonical, rootPath)
        || !pathIsInsideDirectory(stagingCanonical, senderCanonical)) {
        return false;
    }
    for (const QString& fileId : m_fileManager->getAllFileIds()) {
        const QString mappedPath = m_fileManager->getFilePathForId(fileId);
        if (!mappedPath.isEmpty()
            && pathIsInsideDirectory(mappedPath, stagingCanonical)) {
            qWarning() << "UploadManager: refusing to remove mapped residual staging";
            return false;
        }
    }
    if (!QDir(stagingCanonical).removeRecursively()) return false;

    QDir senderDirectory(senderCanonical);
    if (senderDirectory.entryInfoList(QDir::AllEntries | QDir::NoDotAndDotDot
                                      | QDir::Hidden | QDir::System).isEmpty()) {
        QDir().rmdir(senderCanonical);
    }
    return !QFileInfo::exists(stagingPath);
}

void UploadManager::rejectIncomingUpload(const QString& senderId,
                                         const QString& uploadId,
                                         const QString& reason,
                                         bool discardMatchingSession,
                                         const QString& remoteSessionId,
                                         quint64 generation) {
    const bool matchesActive = !m_incoming.uploadId.isEmpty()
        && m_incoming.uploadId == uploadId
        && (senderId.isEmpty() || m_incoming.senderId == senderId);
    const QString effectiveSender = senderId.isEmpty() && matchesActive ? m_incoming.senderId : senderId;
    const QString effectiveRemoteSessionId = remoteSessionId.isEmpty() && matchesActive
        ? m_incoming.remoteSessionId : remoteSessionId;
    const quint64 effectiveGeneration = generation == 0 && matchesActive
        ? m_incoming.generation : generation;

    qWarning() << "UploadManager: rejecting incoming upload:" << reason;
    if (discardMatchingSession && matchesActive) {
        discardActiveIncomingSession(true);
    }
    if (!effectiveSender.isEmpty() && !effectiveRemoteSessionId.isEmpty()
        && effectiveGeneration > 0 && !uploadId.isEmpty()) {
        emitIncomingV3Response(QStringLiteral("upload_rejected"), effectiveSender,
                               effectiveRemoteSessionId, effectiveGeneration,
                               uploadId,
                               {{QStringLiteral("code"), QStringLiteral("target_rejected")},
                                {QStringLiteral("reason"), reason.left(512)}});
    }
}

bool UploadManager::cleanupIncomingSession(bool deleteDiskContents,
                                           bool notifySender,
                                           const QString& senderOverride,
                                           const QString& cacheDirOverride,
                                           const QString& uploadIdOverride,
                                           const QString& ideaOverride) {
    Q_UNUSED(notifySender);
    QString senderId = senderOverride;
    QString cacheDirPath = cacheDirOverride;
    QString uploadId = uploadIdOverride;
    QString canvasSessionId = ideaOverride;
    QStringList fileIds;
    QHash<QString, QString> ownedPaths;
    bool matchesActiveSession = false;

    if (!m_incoming.senderId.isEmpty()) {
        const bool sameSender = senderId.isEmpty() || senderId == m_incoming.senderId;
        const bool sameScope = ideaOverride.isEmpty() || ideaOverride == DEFAULT_IDEA_ID
            || ideaOverride == m_incoming.canvasSessionId;
        if (sameSender && sameScope) {
            matchesActiveSession = true;
            senderId = m_incoming.senderId;
        }
    }

    if (matchesActiveSession) {
        if (uploadId.isEmpty()) uploadId = m_incoming.uploadId;
        if (cacheDirPath.isEmpty()) cacheDirPath = m_incoming.cacheDirPath;
        // Phase 3: canvasSessionId is MANDATORY - fallback to incoming canvasSessionId or DEFAULT_IDEA_ID
        if (canvasSessionId.isEmpty()) {
            canvasSessionId = m_incoming.canvasSessionId.isEmpty() ? DEFAULT_IDEA_ID : m_incoming.canvasSessionId;
        }

        for (auto it = m_incoming.openFiles.begin(); it != m_incoming.openFiles.end(); ++it) {
            if (it.value()) {
                it.value()->flush();
                it.value()->close();
                delete it.value();
            }
        }
        m_incoming.openFiles.clear();
        for (const QString& assetId : m_incoming.expectedSizes.keys()) {
            const QString fileId = m_incoming.assetIdToFileId.value(assetId);
            if (!fileId.isEmpty()) fileIds.append(fileId);
        }
        ownedPaths = m_incoming.filePaths;

        clearIncomingChunkTracking(uploadId);
        m_canceledIncoming.remove(uploadId);

        m_incoming = IncomingUploadSession();
        if (m_incomingStallTimer) m_incomingStallTimer->stop();
    } else {
        if (cacheDirPath.isEmpty() && !senderId.isEmpty()) {
            const QString base = RuntimeProfile::cacheLocation();
            cacheDirPath = base + "/Mouffette/Uploads/" + senderId;
        }
        if (uploadId.isEmpty()) uploadId = uploadIdOverride;
    }

    if (!uploadIdOverride.isEmpty() && uploadIdOverride != uploadId) {
        clearIncomingChunkTracking(uploadIdOverride);
        m_canceledIncoming.remove(uploadIdOverride);
    }

    // Phase 3: canvasSessionId is MANDATORY - check if it's a specific idea or default
    const bool ideaScoped = (canvasSessionId != DEFAULT_IDEA_ID);
    if (deleteDiskContents && ideaScoped) {
        for (auto it = ownedPaths.constBegin(); it != ownedPaths.constEnd(); ++it) {
            if (!cacheDirPath.isEmpty() && pathIsInsideDirectory(it.value(), cacheDirPath)) {
                const QFileInfo info(it.value());
                if (info.exists() && !QFile::remove(it.value())) {
                    qWarning() << "UploadManager: failed to remove partial cached file";
                    return false;
                }
            }
        }
    }

    QSet<QString> removalIds;
    for (const QString& fid : fileIds) {
        if (!fid.isEmpty()) {
            removalIds.insert(fid);
        }
    }
    if (ideaScoped) {
        const QSet<QString> ideaFiles = m_fileManager->getFileIdsForIdea(canvasSessionId);
        removalIds.unite(ideaFiles);
    }

    if (!ideaScoped) {
        QString quarantinedRootPath;
        if (deleteDiskContents && !cacheDirPath.isEmpty()) {
            QDir dir(cacheDirPath);
            if (dir.exists()) {
                const QFileInfo rootInfo(cacheDirPath);
                QDir parent(rootInfo.absolutePath());
                const QString quarantineName = QStringLiteral("%1%2-%3")
                    .arg(kRemovalQuarantinePrefix,
                         QUuid::createUuid().toString(QUuid::WithoutBraces),
                         rootInfo.fileName());
                quarantinedRootPath = parent.absoluteFilePath(quarantineName);
                if (rootInfo.isSymLink() || !rootInfo.isDir()
                    || QFileInfo::exists(quarantinedRootPath)
                    || !parent.rename(rootInfo.fileName(), quarantineName)) {
                    qWarning() << "UploadManager: failed to quarantine cache directory during cleanup";
                    return false;
                }
                qDebug() << "UploadManager: quarantined cache directory during cleanup";
            }
        }

        // Commit repository/tracker changes only after every filesystem
        // operation succeeded, so a retry still has authoritative mappings.
        if (!cacheDirPath.isEmpty()) {
            m_fileManager->removeReceivedFileMappingsUnderPathPrefix(cacheDirPath + "/");
        } else if (!removalIds.isEmpty()) {
            for (const QString& fid : removalIds) {
                m_fileManager->removeReceivedFileMapping(fid);
            }
        }
        if (!quarantinedRootPath.isEmpty()
            && !QDir(quarantinedRootPath).removeRecursively()) {
            // The authoritative mappings are already gone and the original
            // namespace is empty. A later startup sweep safely removes this
            // inaccessible quarantine without making the unload fail.
            qWarning() << "UploadManager: could not purge quarantined cache directory";
        }
    } else {
        struct IdeaCleanupOperation {
            QString fileId;
            QString path;
            QString quarantinePath;
            bool removeMapping = false;
            bool quarantined = false;
        };
        QVector<IdeaCleanupOperation> operations;
        const QString quarantineTransaction = QUuid::createUuid().toString(
            QUuid::WithoutBraces);

        for (const QString& fid : removalIds) {
            if (fid.isEmpty()) continue;

            const QString path = m_fileManager->getFilePathForId(fid);
            const QSet<QString> currentIdeas = m_fileManager->getIdeaIdsForFile(fid);
            if (!currentIdeas.contains(canvasSessionId)) {
                // Active partial files are not registered/associated yet and
                // were already removed through ownedPaths above.
                continue;
            }
            if (path.isEmpty() || cacheDirPath.isEmpty()
                || !pathIsInsideDirectory(path, cacheDirPath)) {
                qWarning() << "UploadManager: refusing idea cleanup outside sender root";
                return false;
            }

            QSet<QString> remainingIdeas = currentIdeas;
            remainingIdeas.remove(canvasSessionId);
            const bool removeMapping = remainingIdeas.isEmpty();

            QString quarantinePath;
            if (deleteDiskContents && removeMapping) {
                const QFileInfo info(path);
                if (info.exists()) {
                    if (!info.isFile() || info.isSymLink()) {
                        qWarning() << "UploadManager: refusing to remove a non-regular cached file";
                        return false;
                    }
                    quarantinePath = QDir(info.absolutePath()).filePath(
                        QStringLiteral("%1%2-%3")
                            .arg(kRemovalQuarantinePrefix,
                                 quarantineTransaction, info.fileName()));
                    if (QFileInfo::exists(quarantinePath)) {
                        qWarning() << "UploadManager: cleanup quarantine already exists";
                        return false;
                    }
                }
            }
            operations.append({fid, path, quarantinePath, removeMapping, false});
        }

        // Rename every file first. Renames on the same filesystem are atomic;
        // if any one fails, roll all prior files back before touching mappings.
        for (qsizetype index = 0; index < operations.size(); ++index) {
            IdeaCleanupOperation& operation = operations[index];
            if (operation.quarantinePath.isEmpty()) continue;
            if (!QFile::rename(operation.path, operation.quarantinePath)) {
                qWarning() << "UploadManager: failed to quarantine cached file"
                           << "canvas" << canvasSessionId.left(16);
                for (qsizetype rollback = index; rollback-- > 0;) {
                    IdeaCleanupOperation& prior = operations[rollback];
                    if (!prior.quarantined) continue;
                    if (!QFile::rename(prior.quarantinePath, prior.path)) {
                        qCritical() << "UploadManager: failed to roll back quarantined file";
                    }
                    prior.quarantined = false;
                }
                return false;
            }
            operation.quarantined = true;
        }

        // No mapping or association changes until all files are safely out of
        // the live namespace, keeping a failed removal retryable.
        for (IdeaCleanupOperation& operation : operations) {
            m_fileManager->dissociateFileFromIdea(operation.fileId, canvasSessionId);
            if (operation.removeMapping) {
                m_fileManager->removeReceivedFileMapping(operation.fileId);
            }
        }

        for (const IdeaCleanupOperation& operation : std::as_const(operations)) {
            if (operation.quarantined && !QFile::remove(operation.quarantinePath)) {
                qWarning() << "UploadManager: could not purge quarantined cached file";
            }
            if (operation.removeMapping) removeEmptyUploadParentsForFile(operation.path);
        }

        if (deleteDiskContents && !cacheDirPath.isEmpty()) {
            QDir dir(cacheDirPath);
            if (dir.exists() && dir.entryInfoList(QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot).isEmpty()) {
                if (!dir.rmdir(cacheDirPath)) {
                    qWarning() << "UploadManager: failed to remove empty cache directory";
                }
            }
        }
    }

    if (!matchesActiveSession && !uploadId.isEmpty()) {
        clearIncomingChunkTracking(uploadId);
        m_canceledIncoming.remove(uploadId);
    }
    return true;
}

void UploadManager::handleIncomingAssetRemoval(const QJsonObject& message) {
    const QString senderEndpointId = senderCacheNamespace(message);
    const QString remoteSessionId =
        message.value(QStringLiteral("remoteSessionId")).toString();
    const QString removalId =
        message.value(QStringLiteral("removalId")).toString();
    const QString uploadId =
        message.value(QStringLiteral("uploadId")).toString();
    const QString assetId =
        message.value(QStringLiteral("assetId")).toString();
    const QString sha256 =
        message.value(QStringLiteral("sha256")).toString();
    const QString fileId =
        message.value(QStringLiteral("fileId")).toString();
    const QString extension =
        message.value(QStringLiteral("extension")).toString();
    quint64 generation = 0;
    qint64 offset = -1;
    qint64 size = -1;

    QJsonObject response{
        {QStringLiteral("type"), QStringLiteral("upload_removed")},
        {QStringLiteral("remoteSessionId"), remoteSessionId},
        {QStringLiteral("generation"), message.value(QStringLiteral("generation"))},
        {QStringLiteral("removalId"), removalId},
        {QStringLiteral("uploadId"), uploadId},
        {QStringLiteral("assetId"), assetId},
        {QStringLiteral("offset"), message.value(QStringLiteral("offset"))},
        {QStringLiteral("size"), message.value(QStringLiteral("size"))},
        {QStringLiteral("sha256"), sha256},
        {QStringLiteral("fileId"), fileId},
        {QStringLiteral("extension"), extension}
    };
    const auto reject = [this, &response](const QString& errorCode) {
        response.insert(QStringLiteral("success"), false);
        response.insert(QStringLiteral("result"), QStringLiteral("cleanup_error"));
        response.insert(QStringLiteral("cacheQuarantined"), false);
        response.insert(QStringLiteral("errorCode"), errorCode.left(128));
        response.insert(QStringLiteral("reason"), errorCode.left(512));
        emit protocolV3UploadResponseReady(response);
    };

    if (!m_remoteCacheStore || !m_remoteCacheReady || !m_fileManager
        || !RemoteCacheStore::isValidEndpointId(senderEndpointId)
        || !RemoteCacheStore::isValidSessionId(remoteSessionId)
        || !isCanonicalUuid(removalId) || !isValidOpaqueId(uploadId)
        || !RemoteCacheStore::isValidAssetId(assetId)
        || !parsePositiveGeneration(message.value(QStringLiteral("generation")),
                                    generation)
        || !parseNonNegativeOffset(message.value(QStringLiteral("offset")), offset)
        || !parseManifestSize(message.value(QStringLiteral("size")), size)
        || offset != size || !isValidFileId(sha256) || fileId != sha256
        || extension != extension.toLower()
        || !isAllowedMediaExtension(extension)) {
        reject(QStringLiteral("invalid_asset_removal"));
        return;
    }

    const RemoteCacheStore::Scope scope{
        senderEndpointId, remoteSessionId, generation
    };
    QString pathError;
    const QString expectedPath = m_remoteCacheStore->assetPath(
        scope, assetId, RemoteCacheStore::AssetArea::Validated,
        extension, &pathError);
    const QString mappedPath = m_fileManager->getFilePathForId(fileId);
    if (expectedPath.isEmpty()) {
        reject(pathError.isEmpty() ? QStringLiteral("remote_asset_path_unavailable")
                                   : pathError);
        return;
    }
    const QFileInfo mappedInfo(mappedPath);
    const QFileInfo expectedInfo(expectedPath);
    const QString canonicalMappedPath = mappedPath.isEmpty()
        ? QString() : mappedInfo.canonicalFilePath();
    const QString canonicalExpectedPath = expectedInfo.canonicalFilePath();
    if (!mappedPath.isEmpty()
        && (canonicalMappedPath.isEmpty() || canonicalExpectedPath.isEmpty()
            || QDir::cleanPath(canonicalMappedPath)
                != QDir::cleanPath(canonicalExpectedPath))) {
        reject(QStringLiteral("remote_asset_mapping_mismatch"));
        return;
    }

    // SceneRun teardown has already settled on the server before this command
    // is relayed. Drop the remaining memory cache handle before the atomic
    // rename so Windows cannot keep the validated file accessible.
    m_fileManager->releaseFileMemory(fileId);
    const RemoteCacheStore::AssetRemovalDescriptor removal{
        removalId,
        uploadId,
        assetId,
        fileId,
        sha256,
        offset,
        size,
        extension
    };
    const RemoteCacheStore::AssetRemovalResult removed =
        m_remoteCacheStore->removeValidatedAsset(scope, removal);
    if (!removed.acknowledgementSafe()) {
        reject(removed.errorCode.isEmpty()
                   ? QStringLiteral("asset_quarantine_failed")
                   : removed.errorCode);
        return;
    }
    if (!mappedPath.isEmpty()) {
        m_fileManager->removeReceivedFileMapping(fileId);
    }
    // A successful unload invalidates the completed-upload replay result: a
    // late duplicate upload_complete must never claim that the removed asset
    // is still present in the target inventory.
    m_incomingUploadCompletionTombstones.remove(uploadId);
    response.insert(QStringLiteral("success"), true);
    response.insert(QStringLiteral("result"), QStringLiteral("committed"));
    response.insert(QStringLiteral("cacheQuarantined"), true);
    response.insert(QStringLiteral("removedFileCount"),
                    mappedPath.isEmpty() ? 0 : 1);
    response.insert(QStringLiteral("quarantinedBytes"),
                    static_cast<double>(removed.quarantinedBytes));
    emit protocolV3UploadResponseReady(response);
}

void UploadManager::handleAssetRemovalResult(const QJsonObject& message) {
    const QString removalId =
        message.value(QStringLiteral("removalId")).toString();
    const auto pendingIt = m_pendingAssetRemovals.find(removalId);
    if (pendingIt == m_pendingAssetRemovals.end()) {
        return;
    }
    const PendingAssetRemoval pending = pendingIt.value();
    if (!assetRemovalMessageMatches(pending, message)) {
        qWarning() << "UploadManager: ignored stale or mismatched asset removal result";
        return;
    }
    if (!message.value(QStringLiteral("success")).toBool(false)
        || message.value(QStringLiteral("result")).toString()
            != QLatin1String("committed")
        || !message.value(QStringLiteral("cacheQuarantined")).toBool(false)) {
        failAssetRemoval(
            removalId,
            message.value(QStringLiteral("reason")).toString(
                message.value(QStringLiteral("errorCode")).toString(
                    message.value(QStringLiteral("code")).toString(
                        QStringLiteral("Remote asset cleanup failed")))));
        return;
    }

    m_pendingAssetRemovals.erase(pendingIt);
    forgetCommittedAsset(pending.asset);
    for (const QString& localFileId : pending.asset.localFileIds) {
        m_fileManager->unmarkFileUploadedToClient(localFileId,
                                                  pending.asset.targetEndpointId);
    }
    emit assetRemovalCommitted(pending.asset.targetEndpointId,
                               pending.asset.localFileIds);
    emit uiStateChanged();
}

void UploadManager::handleUploadProtocolMessage(const QJsonObject& message) {
    if (message.value(QStringLiteral("protocolVersion")).toInt(-1) != 3 || !m_ws) return;
    const QString type = message.value(QStringLiteral("type")).toString();
    if (type == QLatin1String("error")
        && message.value(QStringLiteral("scope")).toString()
            == QLatin1String("upload_remove")) {
        const QString removalId =
            message.value(QStringLiteral("removalId")).toString();
        const auto pending = m_pendingAssetRemovals.constFind(removalId);
        if (pending == m_pendingAssetRemovals.cend()
            || !assetRemovalMessageMatches(pending.value(), message, false)) {
            qWarning() << "UploadManager: ignored uncorrelated asset removal error";
            return;
        }
        failAssetRemoval(removalId,
            message.value(QStringLiteral("message")).toString(
                message.value(QStringLiteral("code")).toString(
                    QStringLiteral("Remote asset removal was rejected"))));
        return;
    }
    const QString uploadId = message.value(QStringLiteral("uploadId")).toString();
    const QString remoteSessionId =
        message.value(QStringLiteral("remoteSessionId")).toString();
    quint64 generation = 0;
    const bool validGeneration = parsePositiveGeneration(
        message.value(QStringLiteral("generation")), generation);
    const bool unboundRejection = type == QLatin1String("upload_rejected")
        && remoteSessionId.isEmpty()
        && !message.contains(QStringLiteral("generation"));
    if (!validGeneration && !unboundRejection) return;
    const RemoteSessionCoordinator::Binding binding =
        m_ws->remoteSessionCoordinator()
            ? m_ws->remoteSessionCoordinator()->byId(remoteSessionId)
            : RemoteSessionCoordinator::Binding();
    const bool localIsTarget = !binding.remoteSessionId.isEmpty()
        && binding.targetEndpointId == m_ws->endpointId();
    const bool localIsOwner = !binding.remoteSessionId.isEmpty()
        && binding.ownerEndpointId == m_ws->endpointId();

    if (type == QLatin1String("upload_remove")) {
        quint64 sourceConnectionGeneration = 0;
        if (localIsTarget
            && parsePositiveGeneration(
                message.value(QStringLiteral("connectionGeneration")),
                sourceConnectionGeneration)
            && sourceConnectionGeneration == binding.ownerConnectionGeneration) {
            handleIncomingAssetRemoval(message);
        }
        return;
    }
    if (type == QLatin1String("upload_removed")) {
        if (localIsOwner) handleAssetRemovalResult(message);
        return;
    }

    if (type == QLatin1String("upload_start")
        || type == QLatin1String("upload_resume")
        || type == QLatin1String("upload_chunk")
        || type == QLatin1String("upload_complete")) {
        if (localIsTarget) handleIncomingMessage(message);
        return;
    }
    if (type == QLatin1String("upload_abort") && localIsTarget) {
        handleIncomingMessage(message);
        return;
    }

    if (ParallelOutgoingTransfer* parallel = parallelForUpload(uploadId)) {
        if (unboundRejection
            || (localIsOwner && remoteSessionId == parallel->remoteSessionId
                && generation == parallel->generation)) {
            handleParallelMessage(parallel, message);
        }
        return;
    }

    if (uploadId.isEmpty() || uploadId != m_currentUploadId
        || m_outgoingState == OutgoingState::Idle) return;
    if (!unboundRejection
        && (!localIsOwner || remoteSessionId != m_outgoingRemoteSessionId
            || generation != m_outgoingGeneration)) return;

    if (type == QLatin1String("upload_resume_ready")) {
        if (m_outgoingState != OutgoingState::AwaitingTargetReady
            || !message.value(QStringLiteral("assets")).isArray()) return;
        QString error;
        if (!applyAuthoritativeOffsets(
                message.value(QStringLiteral("assets")).toArray(), true, &error)) {
            failOutgoingUpload(error);
            return;
        }
        m_waitingForResume = true;
        if (m_outgoingStartAckTimer && m_outgoingStartAckTimer->interval() > 0) {
            m_outgoingStartAckTimer->start();
        }
        return;
    }
    if (type == QLatin1String("upload_ready")) {
        if (m_outgoingState != OutgoingState::AwaitingTargetReady
            || !message.value(QStringLiteral("assets")).isArray()) return;
        QString error;
        if (!applyAuthoritativeOffsets(
                message.value(QStringLiteral("assets")).toArray(), true, &error)) {
            failOutgoingUpload(error);
            return;
        }
        m_waitingForResume = false;
        if (m_outgoingStartAckTimer) m_outgoingStartAckTimer->stop();
        setOutgoingState(OutgoingState::Streaming);
        if (m_outgoingStallTimer && m_outgoingStallTimer->interval() > 0) {
            m_outgoingStallTimer->start();
        }
        emit uiStateChanged();
        scheduleOutgoingPump();
        return;
    }
    if (type == QLatin1String("upload_progress")) {
        if (!message.value(QStringLiteral("assets")).isArray()) return;
        qint64 durableBytes = -1;
        qint64 totalSize = -1;
        if (!parseNonNegativeOffset(message.value(QStringLiteral("durableBytes")),
                                    durableBytes)
            || !parseNonNegativeOffset(message.value(QStringLiteral("totalSize")),
                                       totalSize)
            || totalSize != m_totalBytes || durableBytes > totalSize) {
            failOutgoingUpload(QStringLiteral("Malformed upload progress inventory"));
            return;
        }
        QString error;
        if (!applyAuthoritativeOffsets(
                message.value(QStringLiteral("assets")).toArray(), false, &error)
            || durableBytes != m_remoteAcknowledgedBytes) {
            failOutgoingUpload(error.isEmpty()
                                   ? QStringLiteral("Mismatched durable upload byte count")
                                   : error);
            return;
        }
        if (m_outgoingState == OutgoingState::Streaming) {
            if (m_outgoingStallTimer && m_outgoingStallTimer->interval() > 0) {
                m_outgoingStallTimer->start();
            }
            scheduleOutgoingPump();
        }
        return;
    }
    if (type == QLatin1String("upload_finished")) {
        if (!message.value(QStringLiteral("assets")).isArray()) return;
        QString error;
        if (!applyAuthoritativeOffsets(
                message.value(QStringLiteral("assets")).toArray(), false, &error)) {
            failOutgoingUpload(error);
            return;
        }
        for (const OutgoingAsset& asset : std::as_const(m_outgoingAssets)) {
            if (m_outgoingDurableOffsets.value(asset.assetId) != asset.size) {
                failOutgoingUpload(QStringLiteral("Incomplete final upload inventory"));
                return;
            }
        }
        onUploadFinished(uploadId);
        return;
    }
    if (type == QLatin1String("upload_rejected")) {
        onUploadRejected(uploadId,
            message.value(QStringLiteral("reason")).toString(
                message.value(QStringLiteral("code")).toString(
                    QStringLiteral("Upload rejected"))));
        return;
    }
    if (type == QLatin1String("upload_aborted")
        || type == QLatin1String("upload_abort_ack")) {
        if (m_outgoingState == OutgoingState::Cancelling) finishLocalCancellation();
        else onUploadRejected(uploadId, QStringLiteral("Upload aborted"));
        return;
    }
    if (type == QLatin1String("upload_abort") && localIsOwner) {
        onUploadRejected(uploadId,
            message.value(QStringLiteral("reason")).toString(
                QStringLiteral("Upload aborted by remote session")));
    }
}

void UploadManager::onUploadFinished(const QString& uploadId) {
    if (uploadId != m_currentUploadId) return;
    if (m_outgoingState != OutgoingState::AwaitingValidation) return;
    stopOutgoingPump();
    updateRemoteProgress(100, m_totalFiles > 0 ? m_totalFiles : m_filesCompleted);
    
    // Only the target's exact, complete asset inventory commits local markers.
    for (const OutgoingAsset& asset : std::as_const(m_outgoingAssets)) {
        for (const QString& localFileId : asset.localFileIds) {
            m_fileManager->markFileUploadedToClient(localFileId,
                                                    m_uploadTargetClientId);
        }
    }
    rememberCommittedAssets(m_uploadTargetClientId,
                            m_outgoingRemoteSessionId,
                            m_outgoingGeneration,
                            uploadId,
                            m_outgoingAssets);
    m_remoteInventoryTargets.insert(m_uploadTargetClientId);
    releaseSchedulerSlot(true);
    clearOutgoingTransfer(true);
    emit uploadFinished(uploadId);
    emit uiStateChanged();
}

void UploadManager::onUploadRejected(const QString& uploadId, const QString& reason) {
    if (uploadId.isEmpty() || uploadId != m_currentUploadId
        || m_outgoingState == OutgoingState::Idle) return;
    if (m_outgoingState == OutgoingState::Cancelling) {
        finishLocalCancellation();
        return;
    }

    const bool preserveExistingRemoteFiles = m_uploadWasActiveBeforeStart;
    if (m_ws) {
        m_ws->cancelUploadId(uploadId);
    }
    releaseSchedulerSlot(false);
    emit uploadRejected(uploadId, reason.left(512));
    clearOutgoingTransfer(preserveExistingRemoteFiles);
    emit uiStateChanged();
}

void UploadManager::onConnectionLost() {
    // Transport loss is non-terminal until the strict RemoteSession lease
    // expires. Preserve the immutable manifest, source mapping and last durable
    // offsets so a signed resume can continue without a new upload ID.
    suspendOutgoingForResume();
    const auto transfers = m_parallelOutgoingByUpload.values();
    for (ParallelOutgoingTransfer* transfer : transfers) {
        suspendParallel(transfer);
    }

    cleanupIncomingCacheForConnectionLoss();
}

void UploadManager::cleanupIncomingCacheForConnectionLoss() {
    // The RemoteSession lease, not the transport callback, is terminal. Keep
    // durable contiguous bytes for a possible signed resume before 3000 ms.
    suspendIncomingForResume();
}

// Incoming side (target) - replicate subset of MainWindow logic for assembling files
void UploadManager::handleIncomingMessage(const QJsonObject& message) {
    const QString type = message.value("type").toString();
    if (type == "upload_start") {
        const QString senderId = senderCacheNamespace(message);
        const QString remoteSessionId = message.value("remoteSessionId").toString();
        const QString uploadId = message.value("uploadId").toString();
        quint64 generation = 0;
        quint64 sourceConnectionGeneration = 0;
        if (!m_remoteCacheReady || message.value("protocolVersion").toInt(-1) != 3
            || !RemoteCacheStore::isValidEndpointId(senderId)
            || !RemoteCacheStore::isValidSessionId(remoteSessionId)
            || !isValidOpaqueId(uploadId)
            || !parsePositiveGeneration(message.value("generation"), generation)
            || !parsePositiveGeneration(message.value("connectionGeneration"),
                                        sourceConnectionGeneration)) {
            rejectIncomingUpload(senderId, uploadId, QStringLiteral("Invalid upload identifiers"),
                                 false, remoteSessionId, generation);
            return;
        }
        if (!m_incoming.uploadId.isEmpty()) {
            rejectIncomingUpload(senderId, uploadId,
                                 QStringLiteral("Remote client is already receiving another upload"),
                                 false, remoteSessionId, generation);
            return;
        }
        if (!message.value("files").isArray()) {
            rejectIncomingUpload(senderId, uploadId, QStringLiteral("Invalid upload manifest"),
                                 false, remoteSessionId, generation);
            return;
        }

        const QJsonArray files = message.value("files").toArray();
        if (files.isEmpty() || files.size() > kMaxIncomingFiles) {
            rejectIncomingUpload(senderId, uploadId,
                                 QStringLiteral("Upload manifest has an invalid file count"),
                                 false, remoteSessionId, generation);
            return;
        }

        QVector<ValidatedManifestFile> validatedFiles;
        validatedFiles.reserve(files.size());
        QSet<QString> seenAssetIds;
        QSet<QString> seenMediaIds;
        qint64 totalSize = 0;
        QString manifestError;

        for (const QJsonValue& value : files) {
            if (!value.isObject()) {
                manifestError = QStringLiteral("Upload manifest contains a non-object entry");
                break;
            }
            const QJsonObject fileObject = value.toObject();
            if (!fileObject.value("assetId").isString()
                || !fileObject.value("fileId").isString()
                || !fileObject.value("sha256").isString()
                || !fileObject.value("name").isString()
                || !fileObject.value("extension").isString()
                || !fileObject.value("mediaIds").isArray()) {
                manifestError = QStringLiteral("Upload manifest entry has missing or invalid fields");
                break;
            }

            ValidatedManifestFile file;
            file.assetId = fileObject.value("assetId").toString();
            file.fileId = fileObject.value("fileId").toString();
            file.sha256 = fileObject.value("sha256").toString();
            file.name = fileObject.value("name").toString();
            if (!RemoteCacheStore::isValidAssetId(file.assetId)
                || seenAssetIds.contains(file.assetId)
                || !isValidFileId(file.fileId) || file.sha256 != file.fileId) {
                manifestError = QStringLiteral("Upload manifest contains invalid asset identity");
                break;
            }
            if (!isSafeDisplayName(file.name)
                || !normalizeAndValidateExtension(fileObject.value("extension").toString(), file.extension)
                || !isAllowedMediaExtension(file.extension)
                || QFileInfo(file.name).suffix().compare(file.extension, Qt::CaseInsensitive) != 0) {
                manifestError = QStringLiteral("Upload manifest contains an invalid filename or extension");
                break;
            }
            if (!parseManifestSize(fileObject.value("size"), file.size)
                || totalSize > kMaxIncomingUploadBytes - file.size) {
                manifestError = QStringLiteral("Upload manifest contains an invalid or excessive file size");
                break;
            }
            const QJsonArray mediaIds = fileObject.value("mediaIds").toArray();
            if (mediaIds.isEmpty() || mediaIds.size() > 4096) {
                manifestError = QStringLiteral("Upload manifest has an invalid media identifier list");
                break;
            }
            for (const QJsonValue& mediaIdValue : mediaIds) {
                const QString mediaId = mediaIdValue.toString();
                if (!mediaIdValue.isString() || !isValidOpaqueId(mediaId)
                    || seenMediaIds.contains(mediaId)) {
                    manifestError = QStringLiteral("Upload manifest contains an invalid or duplicate media identifier");
                    break;
                }
                seenMediaIds.insert(mediaId);
                file.mediaIds.append(mediaId);
            }
            if (!manifestError.isEmpty()) break;

            seenAssetIds.insert(file.assetId);
            totalSize += file.size;
            validatedFiles.append(file);
        }

        if (!manifestError.isEmpty()) {
            rejectIncomingUpload(senderId, uploadId, manifestError, false,
                                 remoteSessionId, generation);
            return;
        }

        const RemoteCacheStore::Scope scope {senderId, remoteSessionId, generation};
        QString cacheError;
        if (!m_remoteCacheStore->ensureSession(scope, &cacheError)) {
            rejectIncomingUpload(senderId, uploadId,
                                 QStringLiteral("Remote cache rejected the session: %1").arg(cacheError),
                                 false, remoteSessionId, generation);
            return;
        }

        m_incoming.senderId = senderId;
        m_incoming.remoteSessionId = remoteSessionId;
        m_incoming.generation = generation;
        m_incoming.sourceConnectionGeneration = sourceConnectionGeneration;
        m_incoming.uploadId = uploadId;
        m_incoming.canvasSessionId = remoteSessionId;
        m_incoming.totalFiles = validatedFiles.size();
        m_incoming.totalSize = totalSize;
        m_canceledIncoming.remove(uploadId);

        for (const ValidatedManifestFile& file : std::as_const(validatedFiles)) {
            const QString fullPath = m_remoteCacheStore->assetPath(
                scope, file.assetId, RemoteCacheStore::AssetArea::Staging,
                file.extension, &cacheError);
            if (fullPath.isEmpty()) {
                rejectIncomingUpload(senderId, uploadId,
                                     QStringLiteral("Remote cache rejected the asset: %1").arg(cacheError),
                                     true, remoteSessionId, generation);
                return;
            }
            if (m_incoming.cacheDirPath.isEmpty()) {
                m_incoming.cacheDirPath = QFileInfo(fullPath).absolutePath();
            }

            auto* output = new QFile(fullPath);
            if (!output->open(QIODevice::WriteOnly | QIODevice::NewOnly)) {
                delete output;
                rejectIncomingUpload(senderId, uploadId,
                                     QStringLiteral("Remote client could not create an upload file"),
                                     true, remoteSessionId, generation);
                return;
            }

            m_incoming.openFiles.insert(file.assetId, output);
            m_incoming.expectedSizes.insert(file.assetId, file.size);
            m_incoming.receivedByFile.insert(file.assetId, 0);
            m_incoming.filePaths.insert(file.assetId, fullPath);
            m_incoming.assetIdToFileId.insert(file.assetId, file.fileId);
            m_incoming.assetIdToSha256.insert(file.assetId, file.sha256);
            m_incoming.assetIdToName.insert(file.assetId, file.name);
            m_incoming.assetIdToExtension.insert(file.assetId, file.extension);
            m_incoming.assetIdToMediaIds.insert(file.assetId, file.mediaIds);
            m_expectedChunkIndex.insert(uploadId + QLatin1Char(':') + file.assetId, 0);
        }

        restartIncomingStallTimer();
        emitIncomingV3Response(QStringLiteral("upload_ready"), senderId,
                               remoteSessionId, generation, uploadId,
                               {{QStringLiteral("assets"), incomingAssetOffsets()}});
        emitIncomingV3Response(QStringLiteral("upload_progress"), senderId,
                               remoteSessionId, generation, uploadId,
                               {{QStringLiteral("durableBytes"), 0.0},
                                {QStringLiteral("totalSize"), static_cast<double>(totalSize)},
                                {QStringLiteral("assets"), incomingAssetOffsets()}});
    } else if (type == "upload_resume") {
        const QString senderId = senderCacheNamespace(message);
        const QString remoteSessionId = message.value("remoteSessionId").toString();
        const QString uploadId = message.value("uploadId").toString();
        quint64 generation = 0;
        quint64 sourceConnectionGeneration = 0;
        if (message.value("protocolVersion").toInt(-1) != 3
            || !parsePositiveGeneration(message.value("generation"), generation)
            || !parsePositiveGeneration(message.value("connectionGeneration"),
                                        sourceConnectionGeneration)
            || senderId != m_incoming.senderId
            || remoteSessionId != m_incoming.remoteSessionId
            || uploadId != m_incoming.uploadId
            || generation < m_incoming.generation
            || sourceConnectionGeneration < m_incoming.sourceConnectionGeneration
            || !message.value("assets").isArray()) {
            rejectIncomingUpload(senderId, uploadId,
                                 QStringLiteral("Invalid upload resume binding"), true,
                                 remoteSessionId, generation);
            return;
        }

        const RemoteCacheStore::Scope oldScope {
            senderId, remoteSessionId, m_incoming.generation
        };
        if (generation > m_incoming.generation) {
            QString cacheError;
            if (!m_remoteCacheStore->rebindSessionGeneration(oldScope, generation,
                                                              &cacheError)) {
                rejectIncomingUpload(senderId, uploadId,
                                     QStringLiteral("Remote cache rejected resume: %1").arg(cacheError),
                                     true, remoteSessionId, generation);
                return;
            }
            m_incoming.generation = generation;
        }
        const RemoteCacheStore::Scope resumedScope {
            senderId, remoteSessionId, generation
        };

        QHash<QString, qint64> durableOffsets;
        for (const QJsonValue& value : message.value("assets").toArray()) {
            if (!value.isObject()) {
                rejectIncomingUpload(senderId, uploadId,
                                     QStringLiteral("Invalid upload resume inventory"), true,
                                     remoteSessionId, generation);
                return;
            }
            const QJsonObject asset = value.toObject();
            const QString assetId = asset.value("assetId").toString();
            qint64 offset = -1;
            qint64 size = -1;
            if (!RemoteCacheStore::isValidAssetId(assetId)
                || durableOffsets.contains(assetId)
                || !parseNonNegativeOffset(asset.value("offset"), offset)
                || !parseManifestSize(asset.value("size"), size)
                || !m_incoming.expectedSizes.contains(assetId)
                || size != m_incoming.expectedSizes.value(assetId)
                || asset.value("sha256").toString()
                    != m_incoming.assetIdToSha256.value(assetId)
                || offset > size) {
                rejectIncomingUpload(senderId, uploadId,
                                     QStringLiteral("Invalid upload resume inventory"), true,
                                     remoteSessionId, generation);
                return;
            }
            durableOffsets.insert(assetId, offset);
        }
        if (durableOffsets.size() != m_incoming.expectedSizes.size()) {
            rejectIncomingUpload(senderId, uploadId,
                                 QStringLiteral("Incomplete upload resume inventory"), true,
                                 remoteSessionId, generation);
            return;
        }

        closeIncomingFiles(true);
        m_incoming.received = 0;
        for (auto it = durableOffsets.constBegin(); it != durableOffsets.constEnd(); ++it) {
            const QString assetId = it.key();
            const qint64 offset = it.value();
            const QString path = m_incoming.filePaths.value(assetId);
            const QFileInfo info(path);
            if (!m_remoteCacheStore->ownsPath(resumedScope, path)
                || !info.isFile() || info.isSymLink() || info.size() < offset) {
                rejectIncomingUpload(senderId, uploadId,
                                     QStringLiteral("Upload resume staging is unavailable"), true,
                                     remoteSessionId, generation);
                return;
            }
            auto* output = new QFile(path);
            if (!output->open(QIODevice::ReadWrite) || !output->resize(offset)
                || !output->seek(offset)) {
                delete output;
                rejectIncomingUpload(senderId, uploadId,
                                     QStringLiteral("Upload resume staging could not be reopened"),
                                     true, remoteSessionId, generation);
                return;
            }
            m_incoming.openFiles.insert(assetId, output);
            m_incoming.receivedByFile.insert(assetId, offset);
            m_expectedChunkIndex.insert(uploadId + QLatin1Char(':') + assetId,
                                        offset);
            m_incoming.received += offset;
        }
        m_incoming.suspendedForResume = false;
        m_incoming.sourceConnectionGeneration = sourceConnectionGeneration;
        m_incoming.lastProgressBytesReported = m_incoming.received;
        restartIncomingStallTimer();
        emitIncomingV3Response(QStringLiteral("upload_ready"), senderId,
                               remoteSessionId, generation, uploadId,
                               {{QStringLiteral("assets"), incomingAssetOffsets()}});
    } else if (type == "upload_chunk") {
        const QString senderId = senderCacheNamespace(message);
        const QString remoteSessionId = message.value("remoteSessionId").toString();
        const QString uploadId = message.value("uploadId").toString();
        quint64 generation = 0;
        quint64 sourceConnectionGeneration = 0;
        if (message.value("protocolVersion").toInt(-1) != 3
            || !parsePositiveGeneration(message.value("generation"), generation)
            || !parsePositiveGeneration(message.value("connectionGeneration"),
                                        sourceConnectionGeneration)) {
            rejectIncomingUpload(senderId, uploadId,
                                 QStringLiteral("Invalid upload generation"), false,
                                 remoteSessionId, generation);
            return;
        }
        if (m_canceledIncoming.contains(uploadId)) return;
        if (uploadId != m_incoming.uploadId || senderId != m_incoming.senderId
            || remoteSessionId != m_incoming.remoteSessionId
            || generation != m_incoming.generation
            || sourceConnectionGeneration != m_incoming.sourceConnectionGeneration
            || m_incoming.suspendedForResume) {
            rejectIncomingUpload(senderId, uploadId, QStringLiteral("No matching upload session"),
                                 false, remoteSessionId, generation);
            return;
        }
        const RemoteCacheStore::Scope scope {senderId, remoteSessionId, generation};
        if (!m_remoteCacheStore->acceptsCommands(scope)) {
            rejectIncomingUpload(senderId, uploadId, QStringLiteral("Remote session is terminating"),
                                 false, remoteSessionId, generation);
            return;
        }
        const QString assetId = message.value("assetId").toString();
        QFile* qf = m_incoming.openFiles.value(assetId, nullptr);
        if (!RemoteCacheStore::isValidAssetId(assetId) || !qf || !qf->isOpen()) {
            rejectIncomingUpload(senderId, uploadId,
                                 QStringLiteral("Upload chunk references an unknown asset"), true,
                                 remoteSessionId, generation);
            return;
        }

        qint64 offset = -1;
        qint64 declaredChunkSize = -1;
        QByteArray data;
        if (!parseNonNegativeOffset(message.value("offset"), offset)
            || !parseManifestSize(message.value("size"), declaredChunkSize)
            || !decodeCanonicalChunk(message.value("data"), data)
            || declaredChunkSize != data.size()
            || message.value("sha256").toString()
                != m_incoming.assetIdToSha256.value(assetId)) {
            rejectIncomingUpload(senderId, uploadId,
                                 QStringLiteral("Upload chunk is malformed or too large"), true,
                                 remoteSessionId, generation);
            return;
        }

        const QString key = uploadId + QLatin1Char(':') + assetId;
        if (!m_expectedChunkIndex.contains(key)) {
            rejectIncomingUpload(senderId, uploadId, QStringLiteral("Upload chunk state is missing"),
                                 true, remoteSessionId, generation);
            return;
        }
        const qint64 expected = m_expectedChunkIndex.value(key);
        if (offset != expected) {
            rejectIncomingUpload(senderId, uploadId, QStringLiteral("Upload chunks arrived out of order"),
                                 true, remoteSessionId, generation);
            return;
        }

        const qint64 receivedForFile = m_incoming.receivedByFile.value(assetId, -1);
        const qint64 expectedForFile = m_incoming.expectedSizes.value(assetId, -1);
        if (receivedForFile < 0 || expectedForFile < 1
            || data.size() > expectedForFile - receivedForFile
            || qf->size() != receivedForFile
            || !qf->seek(receivedForFile)) {
            rejectIncomingUpload(senderId, uploadId,
                                 QStringLiteral("Upload chunk exceeds the declared asset size"), true,
                                 remoteSessionId, generation);
            return;
        }

        const qint64 written = qf->write(data);
        if (written != data.size() || !syncFile(qf)) {
            rejectIncomingUpload(senderId, uploadId,
                                 QStringLiteral("Remote client could not durably write the upload"),
                                 true, remoteSessionId, generation);
            return;
        }
        m_expectedChunkIndex[key] = expected + written;
        m_incoming.receivedByFile[assetId] = receivedForFile + written;
        m_incoming.received += written;
        restartIncomingStallTimer();

        emitIncomingV3Response(QStringLiteral("upload_progress"), senderId,
                               remoteSessionId, generation, uploadId,
                               {{QStringLiteral("durableBytes"),
                                 static_cast<double>(m_incoming.received)},
                                {QStringLiteral("totalSize"),
                                 static_cast<double>(m_incoming.totalSize)},
                                {QStringLiteral("assets"), incomingAssetOffsets()}});
        m_incoming.lastProgressBytesReported = m_incoming.received;
    } else if (type == "upload_complete") {
        const QString senderId = senderCacheNamespace(message);
        const QString remoteSessionId = message.value("remoteSessionId").toString();
        const QString uploadId = message.value("uploadId").toString();
        quint64 generation = 0;
        quint64 sourceConnectionGeneration = 0;
        if (message.value("protocolVersion").toInt(-1) != 3
            || !parsePositiveGeneration(message.value("generation"), generation)
            || !parsePositiveGeneration(message.value("connectionGeneration"),
                                        sourceConnectionGeneration)) {
            rejectIncomingUpload(senderId, uploadId, QStringLiteral("Invalid upload generation"),
                                 false, remoteSessionId, generation);
            return;
        }
        if (m_incoming.uploadId.isEmpty()
            && m_incomingUploadCompletionTombstones.contains(uploadId)) {
            if (!replayIncomingUploadCompletion(
                    message, senderId, remoteSessionId, generation,
                    sourceConnectionGeneration)) {
                rejectIncomingUpload(
                    senderId, uploadId,
                    QStringLiteral("Upload completion replay inventory does not match"),
                    false, remoteSessionId, generation);
            }
            return;
        }
        if (m_canceledIncoming.contains(uploadId)) return;
        if (uploadId != m_incoming.uploadId || senderId != m_incoming.senderId
            || remoteSessionId != m_incoming.remoteSessionId
            || generation != m_incoming.generation
            || sourceConnectionGeneration != m_incoming.sourceConnectionGeneration
            || m_incoming.suspendedForResume) {
            rejectIncomingUpload(senderId, uploadId, QStringLiteral("No matching upload session"),
                                 false, remoteSessionId, generation);
            return;
        }
        const RemoteCacheStore::Scope scope {senderId, remoteSessionId, generation};
        if (!m_remoteCacheStore->acceptsCommands(scope)
            || !message.value("assets").isArray()) {
            rejectIncomingUpload(senderId, uploadId,
                                 QStringLiteral("Upload completion is not valid for this session"),
                                 true, remoteSessionId, generation);
            return;
        }

        QSet<QString> completionAssets;
        for (const QJsonValue& value : message.value("assets").toArray()) {
            if (!value.isObject()) {
                rejectIncomingUpload(senderId, uploadId,
                                     QStringLiteral("Invalid upload completion inventory"), true,
                                     remoteSessionId, generation);
                return;
            }
            const QJsonObject asset = value.toObject();
            const QString assetId = asset.value("assetId").toString();
            qint64 offset = -1;
            qint64 size = -1;
            if (!RemoteCacheStore::isValidAssetId(assetId)
                || completionAssets.contains(assetId)
                || !parseNonNegativeOffset(asset.value("offset"), offset)
                || !parseManifestSize(asset.value("size"), size)
                || size != m_incoming.expectedSizes.value(assetId, -1)
                || offset != size
                || asset.value("sha256").toString()
                    != m_incoming.assetIdToSha256.value(assetId)) {
                rejectIncomingUpload(senderId, uploadId,
                                     QStringLiteral("Invalid upload completion inventory"), true,
                                     remoteSessionId, generation);
                return;
            }
            completionAssets.insert(assetId);
        }
        if (completionAssets.size() != m_incoming.expectedSizes.size()) {
            rejectIncomingUpload(senderId, uploadId,
                                 QStringLiteral("Incomplete upload completion inventory"), true,
                                 remoteSessionId, generation);
            return;
        }

        bool closeSucceeded = true;
        for (auto it = m_incoming.openFiles.begin(); it != m_incoming.openFiles.end(); ++it) {
            if (!it.value()) {
                closeSucceeded = false;
                continue;
            }
            closeSucceeded = syncFile(it.value()) && closeSucceeded;
            it.value()->close();
            closeSucceeded = (it.value()->error() == QFileDevice::NoError) && closeSucceeded;
            delete it.value();
        }
        m_incoming.openFiles.clear();

        QString completionError;
        QStringList assetIds = m_incoming.expectedSizes.keys();
        std::sort(assetIds.begin(), assetIds.end());
        if (!closeSucceeded || assetIds.size() != m_incoming.totalFiles) {
            completionError = QStringLiteral("Remote client could not finalize every upload file");
        }

        QHash<QString, QString> selectedPathByAsset;
        QHash<QString, QString> selectedPathByFileId;
        for (const QString& assetId : std::as_const(assetIds)) {
            if (!completionError.isEmpty()) break;
            const QString fileId = m_incoming.assetIdToFileId.value(assetId);
            const QString expectedDigest = m_incoming.assetIdToSha256.value(assetId);
            const qint64 expectedSize = m_incoming.expectedSizes.value(assetId, -1);
            const qint64 receivedSize = m_incoming.receivedByFile.value(assetId, -1);
            const QString path = m_incoming.filePaths.value(assetId);
            const QFileInfo info(path);
            if (expectedSize < 1 || receivedSize != expectedSize
                || !info.exists() || !info.isFile() || info.isSymLink()
                || info.size() != expectedSize || !m_remoteCacheStore->ownsPath(scope, path)
                || sha256ForFile(path) != expectedDigest) {
                completionError = QStringLiteral("Upload is incomplete or its size does not match the manifest");
                break;
            }
            if (!MediaFilePolicy::isAcceptedLocalFile(path)) {
                completionError = QStringLiteral("Unsupported or invalid media file: %1")
                                      .arg(m_incoming.assetIdToName.value(assetId, assetId));
                break;
            }

            QString selectedPath = selectedPathByFileId.value(fileId);
            const QString expectedExtension = m_incoming.assetIdToExtension.value(assetId);
            if (!selectedPath.isEmpty()
                && QFileInfo(selectedPath).suffix().compare(expectedExtension,
                                                            Qt::CaseInsensitive) != 0) {
                completionError = QStringLiteral(
                    "Upload file identifier was reused with a different extension");
                break;
            }
            const QString existingPath = m_fileManager->getFilePathForId(fileId);
            if (selectedPath.isEmpty() && !existingPath.isEmpty()) {
                const QFileInfo existingInfo(existingPath);
                const QString existingCanonicalPath = existingInfo.canonicalFilePath();
                if (existingCanonicalPath.isEmpty() || !existingInfo.isFile()
                    || existingInfo.isSymLink()
                    || !m_remoteCacheStore->ownsPath(scope, existingCanonicalPath)
                    || existingInfo.size() != expectedSize
                    || existingInfo.suffix().compare(expectedExtension, Qt::CaseInsensitive) != 0
                    || !MediaFilePolicy::isAcceptedLocalFile(existingCanonicalPath)
                    || sha256ForFile(existingCanonicalPath) != expectedDigest) {
                    completionError = QStringLiteral(
                        "Upload file identifier collided with a different or foreign target file");
                    break;
                }
                selectedPath = existingCanonicalPath;
            }
            if (selectedPath.isEmpty()) {
                QString cacheError;
                selectedPath = m_remoteCacheStore->assetPath(
                    scope, assetId, RemoteCacheStore::AssetArea::Validated,
                    m_incoming.assetIdToExtension.value(assetId), &cacheError);
                if (selectedPath.isEmpty()) {
                    completionError = QStringLiteral("Remote cache rejected validation: %1")
                                          .arg(cacheError);
                    break;
                }
                const QFileInfo selectedInfo(selectedPath);
                if (selectedInfo.exists()
                    && (selectedInfo.isSymLink() || !selectedInfo.isFile()
                        || selectedInfo.size() != expectedSize
                        || sha256ForFile(selectedPath) != expectedDigest)) {
                    completionError = QStringLiteral("Validated asset identity collided");
                    break;
                }
            }
            selectedPathByAsset.insert(assetId, selectedPath);
            selectedPathByFileId.insert(fileId, selectedPath);
        }
        if (!completionError.isEmpty()) {
            rejectIncomingUpload(senderId, uploadId, completionError, true,
                                 remoteSessionId, generation);
            return;
        }

        for (const QString& assetId : std::as_const(assetIds)) {
            const QString stagingPath = m_incoming.filePaths.value(assetId);
            const QString selectedPath = selectedPathByAsset.value(assetId);
            if (QDir::cleanPath(stagingPath) == QDir::cleanPath(selectedPath)) continue;
            if (!QFileInfo::exists(selectedPath)) {
                QFile staging(stagingPath);
                if (!staging.rename(selectedPath)) {
                    completionError = QStringLiteral("Remote client could not promote a validated asset");
                    break;
                }
            } else if (!QFile::remove(stagingPath)) {
                completionError = QStringLiteral("Remote client could not remove a duplicate staging asset");
                break;
            }
        }
        if (!completionError.isEmpty()) {
            rejectIncomingUpload(senderId, uploadId, completionError, true,
                                 remoteSessionId, generation);
            return;
        }

        for (auto it = selectedPathByFileId.constBegin();
             it != selectedPathByFileId.constEnd(); ++it) {
            const QString existingPath = m_fileManager->getFilePathForId(it.key());
            if (existingPath.isEmpty()) {
                m_fileManager->registerReceivedFilePath(it.key(), it.value());
            }
            if (QDir::cleanPath(QFileInfo(m_fileManager->getFilePathForId(it.key())).absoluteFilePath())
                != QDir::cleanPath(QFileInfo(it.value()).absoluteFilePath())) {
                completionError = QStringLiteral("Remote client could not register a validated asset");
                break;
            }
            m_fileManager->associateFileWithIdea(it.key(), remoteSessionId);
        }
        if (!completionError.isEmpty()) {
            rejectIncomingUpload(senderId, uploadId, completionError, true,
                                 remoteSessionId, generation);
            return;
        }

        const QJsonArray completedAssets = incomingAssetOffsets();
        const QString completedStagingPath = m_incoming.cacheDirPath;
        rememberIncomingUploadCompletion(
            senderId, remoteSessionId, generation,
            sourceConnectionGeneration, uploadId, completedAssets);
        if (m_incomingStallTimer) m_incomingStallTimer->stop();
        clearIncomingChunkTracking(uploadId);
        m_canceledIncoming.remove(uploadId);
        m_incoming = IncomingUploadSession();
        QDir().rmdir(completedStagingPath);
        emitIncomingV3Response(QStringLiteral("upload_finished"), senderId,
                               remoteSessionId, generation, uploadId,
                               {{QStringLiteral("assets"), completedAssets}});
    } else if (type == "upload_abort") {
        const QString abortedId = message.value("uploadId").toString();
        const QString senderClientId = senderCacheNamespace(message);
        const QString remoteSessionId = message.value("remoteSessionId").toString();
        quint64 generation = 0;
        quint64 sourceConnectionGeneration = 0;
        if (message.value("protocolVersion").toInt(-1) != 3
            || !parsePositiveGeneration(message.value("generation"), generation)
            || !parsePositiveGeneration(message.value("connectionGeneration"),
                                        sourceConnectionGeneration)
            || !RemoteCacheStore::isValidEndpointId(senderClientId)
            || !RemoteCacheStore::isValidSessionId(remoteSessionId)
            || !isValidOpaqueId(abortedId)) {
            return;
        }
        bool cleanupConfirmed = false;
        if (!abortedId.isEmpty() && abortedId == m_incoming.uploadId
            && senderClientId == m_incoming.senderId
            && remoteSessionId == m_incoming.remoteSessionId
            && generation == m_incoming.generation
            && sourceConnectionGeneration == m_incoming.sourceConnectionGeneration) {
            cleanupConfirmed = discardActiveIncomingSession(true);
        } else if (m_canceledIncoming.contains(abortedId)) {
            cleanupConfirmed = true;
        }
        if (cleanupConfirmed) {
            emitIncomingV3Response(QStringLiteral("upload_abort_ack"), senderClientId,
                                   remoteSessionId, generation, abortedId,
                                   {{QStringLiteral("success"), true}});
        }
    }
}

bool UploadManager::canAcceptNewAction() const {
    if (m_lastAcceptedAction.isValid()
        && m_lastAcceptedAction.elapsed() < MIN_ACTION_INTERVAL_MS) {
        return false;
    }
    for (auto it = m_pendingAssetRemovals.cbegin();
         it != m_pendingAssetRemovals.cend(); ++it) {
        if (it->asset.targetEndpointId == m_targetClientId) return false;
    }
    if (parallelForTarget(m_targetClientId)) return false;
    return m_currentUploadId.isEmpty()
        || m_uploadTargetClientId != m_targetClientId;
}

void UploadManager::recordAcceptedAction() {
    m_lastAcceptedAction.restart();
}

bool UploadManager::canRequestCancel() const {
    if (ParallelOutgoingTransfer* transfer = parallelForTarget(m_targetClientId)) {
        return (transfer->state == OutgoingState::Queued
                || transfer->state == OutgoingState::AwaitingTargetReady
                || transfer->state == OutgoingState::Streaming
                || transfer->state == OutgoingState::AwaitingValidation
                || transfer->state == OutgoingState::Suspended)
            && transfer->stateAge.isValid()
            && transfer->stateAge.elapsed() >= CANCEL_GUARD_MS;
    }
    return m_uploadTargetClientId == m_targetClientId
        && (m_outgoingState == OutgoingState::Queued
            || m_outgoingState == OutgoingState::AwaitingTargetReady
            || m_outgoingState == OutgoingState::Streaming
            || m_outgoingState == OutgoingState::AwaitingValidation
            || m_outgoingState == OutgoingState::Suspended)
        && m_outgoingStateAge.isValid()
        && m_outgoingStateAge.elapsed() >= CANCEL_GUARD_MS;
}

void UploadManager::restartIncomingStallTimer() {
    if (m_incomingStallTimer && m_incomingStallTimer->interval() > 0
        && !m_incoming.uploadId.isEmpty()) {
        m_incomingStallTimer->start();
    }
}
