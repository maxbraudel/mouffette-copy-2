#include "backend/network/UploadManager.h"
#include "backend/network/WebSocketClient.h"
#include "backend/files/FileManager.h"
#include "backend/domain/media/MediaFilePolicy.h"
#include "backend/domain/session/SessionManager.h"  // Phase 3: For DEFAULT_IDEA_ID constant
#include <QGraphicsScene>
#include <QGraphicsItem>
#include <QFileInfo>
#include <QJsonObject>
#include <QJsonArray>
#include <QStandardPaths>
#include <QDir>
#include <QTimer>
#include <QElapsedTimer>
#include <QDebug>
#include <QRegularExpression>
#include <QScopedValueRollback>
#include <QSet>
#include <algorithm>
#include <cmath>
#include <limits>

// Removed dependency on ResizableMediaBase / scene scanning.

namespace {

constexpr int kMaxIncomingFiles = 256;
constexpr qint64 kMaxIncomingFileBytes = 16LL * 1024 * 1024 * 1024;
constexpr qint64 kMaxIncomingUploadBytes = 64LL * 1024 * 1024 * 1024;
constexpr qsizetype kMaxIncomingChunkBytes = 128 * 1024;
constexpr qsizetype kMaxEncodedChunkCharacters = ((kMaxIncomingChunkBytes + 2) / 3) * 4;
constexpr qint64 kMaxQueuedUploadBytes = 2LL * 1024 * 1024;
constexpr int kMaxChunksPerPump = 8;
constexpr int kUploadTransportStallTimeoutMs = 30 * 1000;
constexpr int kUploadFinalAckTimeoutMs = 45 * 1000;
constexpr int kRemovalAckTimeoutMs = 45 * 1000;

struct ValidatedManifestFile {
    QString fileId;
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
    return pattern.match(value).hasMatch();
}

QString senderCacheNamespace(const QJsonObject& message) {
    const QString persistentId = message.value("senderPersistentClientId").toString();
    return persistentId.isEmpty() ? message.value("senderClientId").toString() : persistentId;
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
    QString base = QStandardPaths::writableLocation(QStandardPaths::CacheLocation);
    if (base.isEmpty()) base = QDir::homePath() + QStringLiteral("/.cache");
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

UploadManager::UploadManager(FileManager* fileManager, QObject* parent)
    : QObject(parent), m_fileManager(fileManager) {
    m_lastActionTime.start();
    
    // Setup debounce timer for action throttling
    m_actionDebounceTimer = new QTimer(this);
    m_actionDebounceTimer->setSingleShot(true);
    connect(m_actionDebounceTimer, &QTimer::timeout, this, [this]() {
        m_actionInProgress = false;
    });

    m_outgoingPumpTimer = new QTimer(this);
    m_outgoingPumpTimer->setSingleShot(true);
    m_outgoingPumpTimer->setInterval(0);
    connect(m_outgoingPumpTimer, &QTimer::timeout,
            this, &UploadManager::pumpOutgoingUpload);

    m_outgoingStallTimer = new QTimer(this);
    m_outgoingStallTimer->setSingleShot(true);
    m_outgoingStallTimer->setInterval(kUploadTransportStallTimeoutMs);
    connect(m_outgoingStallTimer, &QTimer::timeout, this, [this]() {
        if (m_uploadInProgress && !m_outgoingPayloadCompleteSent
            && !m_currentUploadId.isEmpty()) {
            failOutgoingUpload(QStringLiteral("Upload transport stalled"));
        }
    });

    m_outgoingAckTimer = new QTimer(this);
    m_outgoingAckTimer->setSingleShot(true);
    m_outgoingAckTimer->setInterval(kUploadFinalAckTimeoutMs);
    connect(m_outgoingAckTimer, &QTimer::timeout, this, [this]() {
        if (m_uploadInProgress && m_outgoingPayloadCompleteSent
            && !m_currentUploadId.isEmpty()) {
            failOutgoingUpload(QStringLiteral(
                "Remote client did not validate the upload in time"));
        }
    });

    m_removalAckTimer = new QTimer(this);
    m_removalAckTimer->setSingleShot(true);
    m_removalAckTimer->setInterval(kRemovalAckTimeoutMs);
    connect(m_removalAckTimer, &QTimer::timeout, this, [this]() {
        if (m_pendingRemovalId.isEmpty()) return;
        const QString expiredRemovalId = m_pendingRemovalId;
        m_pendingRemovalId.clear();
        m_pendingRemovalTargetId.clear();
        m_pendingRemovalCanvasSessionId.clear();
        m_actionInProgress = false;
        emit uploadRejected(expiredRemovalId,
                            QStringLiteral("Remote removal confirmation timed out"));
        emit uiStateChanged();
    });
}

void UploadManager::setWebSocketClient(WebSocketClient* client) {
    QObject::disconnect(m_uploadBytesWrittenConnection);
    QObject::disconnect(m_uploadTransportLostConnection);
    m_ws = client;
    if (!client) return;

    m_uploadBytesWrittenConnection = connect(
        client, &WebSocketClient::uploadTransportBytesWritten,
        this, [this](qint64) {
            if (!m_uploadInProgress || m_outgoingPayloadCompleteSent) return;
            if (m_outgoingStallTimer) m_outgoingStallTimer->start();
            scheduleOutgoingPump();
        });
    m_uploadTransportLostConnection = connect(
        client, &WebSocketClient::uploadTransportLost,
        this, [this](const QString& reason) {
            if (!m_uploadInProgress || m_outgoingPayloadCompleteSent
                || m_currentUploadId.isEmpty()) return;
            failOutgoingUpload(reason);
        });
}
void UploadManager::setTargetClientId(const QString& id) { m_targetClientId = id; }

void UploadManager::forceResetForClient(const QString& clientId) {
    if (!clientId.isEmpty()) {
        const bool matchesUploadTarget = (!m_uploadTargetClientId.isEmpty() && m_uploadTargetClientId == clientId);
        const bool matchesCurrentTarget = (!m_targetClientId.isEmpty() && m_targetClientId == clientId);
        if (!matchesUploadTarget && !matchesCurrentTarget && !m_uploadActive && !m_uploadInProgress && !m_finalizing) {
            return;
        }
    }

    resetToInitial();
    emit uiStateChanged();
}

void UploadManager::toggleUpload(const QVector<UploadFileInfo>& files) {
    if (!m_ws || !m_ws->isConnected() || m_targetClientId.isEmpty()) {
        qWarning() << "UploadManager: Not connected or no target set";
        return;
    }
    
    // Anti-spam protection: check if we can accept a new action
    if (!canAcceptNewAction()) {
        qInfo() << "UploadManager: Action ignored due to rate limiting";
        return;
    }
    
    if (m_cancelFinalizePending) {
        qInfo() << "UploadManager: Cancellation cleanup pending; toggle ignored";
        return;
    }
    if (!m_pendingRemovalId.isEmpty()) {
        qInfo() << "UploadManager: Remote removal acknowledgement pending; toggle ignored";
        return;
    }
    
    // Block new actions while a critical operation is in progress
    if (m_actionInProgress) {
        qInfo() << "UploadManager: Action in progress, toggle ignored";
        return;
    }
    
    if (m_uploadActive) {
        // If active state but we are provided with additional files, start a new upload for them
        if (!files.isEmpty()) {
            startUpload(files);
            return;
        }
        // No new files: behave as unload toggle
        requestUnload();
        return;
    }
    if (m_uploadInProgress) { // cancel
        requestCancel();
        return;
    }
    if (files.isEmpty()) {
        qInfo() << "UploadManager: No files provided";
        return;
    }
    startUpload(files);
}

void UploadManager::requestRemoval(const QString& clientId) {
    if (!m_ws || !m_ws->isConnected() || clientId.isEmpty()) return;
    if (!m_pendingRemovalId.isEmpty()) {
        qInfo() << "UploadManager: A remote removal is already pending";
        return;
    }
    // Phase 3: canvasSessionId is MANDATORY - always set to DEFAULT_IDEA_ID at minimum
    if (m_activeIdeaId.isEmpty()) {
        qWarning() << "UploadManager: requestRemoval has empty canvasSessionId (should never happen), using DEFAULT_IDEA_ID";
        m_activeIdeaId = DEFAULT_IDEA_ID;
    }
    // Ensure subsequent all_files_removed callbacks attribute to the correct target
    m_uploadTargetClientId = clientId;
    m_lastRemovalClientId = clientId;
    m_pendingRemovalId = QUuid::createUuid().toString(QUuid::WithoutBraces);
    m_pendingRemovalTargetId = clientId;
    m_pendingRemovalCanvasSessionId = m_activeIdeaId;
    m_ws->sendRemoveAllFiles(clientId, m_activeIdeaId, m_pendingRemovalId);
    if (m_removalAckTimer) m_removalAckTimer->start();
}

void UploadManager::requestUnload() {
    const QString clientId = m_uploadTargetClientId.isEmpty() ? m_targetClientId : m_uploadTargetClientId;
    if (!m_uploadActive || clientId.isEmpty()) return;
    // Phase 3: canvasSessionId is MANDATORY - always set to DEFAULT_IDEA_ID at minimum
    if (m_activeIdeaId.isEmpty()) {
        qWarning() << "UploadManager: requestUnload has empty canvasSessionId (should never happen), using DEFAULT_IDEA_ID";
        m_activeIdeaId = DEFAULT_IDEA_ID;
    }
    
    // Mark action in progress to prevent spam
    scheduleActionDebounce();
    
    requestRemoval(clientId);
    // Don't reset state here - wait for onAllFilesRemovedRemote() callback
    emit uiStateChanged();
}

void UploadManager::requestCancel() {
    const QString clientId = m_uploadTargetClientId.isEmpty() ? m_targetClientId : m_uploadTargetClientId;
    if (!m_ws || !m_ws->isConnected() || clientId.isEmpty()) return;
    if (!m_uploadInProgress) return;
    if (m_cancelRequested) return;
    // Phase 3: canvasSessionId is MANDATORY - always set to DEFAULT_IDEA_ID at minimum
    if (m_activeIdeaId.isEmpty()) {
        qWarning() << "UploadManager: requestCancel has empty canvasSessionId (should never happen), using DEFAULT_IDEA_ID";
        m_activeIdeaId = DEFAULT_IDEA_ID;
    }
    
    // Mark action in progress to prevent spam
    scheduleActionDebounce();
    
    m_cancelRequested = true;
    m_cancelFinalizePending = true;
    stopOutgoingPump();
    if (!m_currentUploadId.isEmpty()) {
        m_ws->sendUploadAbort(clientId, m_currentUploadId, "User cancelled", m_activeIdeaId);
    }
    // Also request removal of all files to clean remote state
    requestRemoval(clientId);
    // We'll reset final state upon all_files_removed callback
    emit uiStateChanged();
    // Start fallback timer (3s) in case remote never responds
    if (!m_cancelFallbackTimer) {
        m_cancelFallbackTimer = new QTimer(this);
        m_cancelFallbackTimer->setSingleShot(true);
        connect(m_cancelFallbackTimer, &QTimer::timeout, this, [this]() {
            if (m_cancelFinalizePending) {
                finalizeLocalCancelState();
            }
        });
    }
    m_cancelFallbackTimer->start(3000);
}

void UploadManager::startUpload(const QVector<UploadFileInfo>& files) {
    // Prevent concurrent uploads
    if (m_uploadInProgress || m_finalizing || !m_pendingRemovalId.isEmpty()) {
        qWarning() << "UploadManager: Upload already in progress, ignoring new start request";
        return;
    }
    if (files.isEmpty() || files.size() > kMaxIncomingFiles) {
        qWarning() << "UploadManager: refusing upload with invalid file count";
        return;
    }
    qint64 declaredTotalBytes = 0;
    for (const UploadFileInfo& file : files) {
        if (file.size < 1 || file.size > kMaxIncomingFileBytes
            || declaredTotalBytes > kMaxIncomingUploadBytes - file.size
            || !MediaFilePolicy::isAcceptedLocalFile(file.path)) {
            qWarning() << "UploadManager: refusing unsupported media file" << file.path
                       << "or excessive upload size (video uploads must be valid MP4 files)";
            return;
        }
        declaredTotalBytes += file.size;
    }
    // Phase 3: canvasSessionId is MANDATORY - always set to DEFAULT_IDEA_ID at minimum
    if (m_activeIdeaId.isEmpty()) {
        qWarning() << "UploadManager: startUpload has empty canvasSessionId (should never happen), using DEFAULT_IDEA_ID";
        m_activeIdeaId = DEFAULT_IDEA_ID;
    }
    
    m_uploadWasActiveBeforeStart = m_uploadActive;
    m_uploadRejectedDuringSend = false;
    // Capture stable target id for the entire upload session
    m_uploadTargetClientId = m_targetClientId;
    m_currentUploadId = QUuid::createUuid().toString(QUuid::WithoutBraces);
    m_uploadInProgress = true;
    m_cancelRequested = false;
    m_finalizing = false;
    m_lastPercent = 0;
    m_filesCompleted = 0;
    m_totalFiles = files.size();
    m_totalBytes = declaredTotalBytes;
    m_sentBytes = 0;
    m_remoteProgressReceived = false;
    stopOutgoingPump();
    m_outgoingFiles = files;
    m_outgoingFileIndex = 0;
    m_outgoingChunkIndex = 0;
    m_outgoingSentForFile = 0;
    m_outgoingPayloadCompleteSent = false;
    resetProgressTracking();

    // Build manifest with file deduplication info
    QJsonArray manifest;
    
    for (const auto& f : files) {
        QJsonObject obj;
        obj["fileId"] = f.fileId;
        obj["name"] = f.name;
        obj["extension"] = f.extension;
        obj["sizeBytes"] = static_cast<double>(f.size);
        
        // Include all mediaIds that use this fileId
        QList<QString> mediaIds = m_fileManager->getMediaIdsForFile(f.fileId);
        QJsonArray mediaIdArray;
        for (const QString& mediaId : mediaIds) {
            mediaIdArray.append(mediaId);
        }
        obj["mediaIds"] = mediaIdArray;
        
        
        
        manifest.append(obj);
    }

    // Lock UI/session state before prepareUploadChannel() pumps events. This
    // prevents a re-entrant action from starting a second upload mid-handshake.
    scheduleActionDebounce();
    emit uiStateChanged();
    const QString preparingUploadId = m_currentUploadId;
    if (m_ws) {
        m_ws->beginUploadSession(true);
    }
    if (!m_uploadInProgress || m_cancelRequested
        || m_currentUploadId != preparingUploadId) {
        return;
    }

    if (!m_ws || !m_ws->sendUploadStart(m_uploadTargetClientId, manifest,
                                        m_currentUploadId, m_activeIdeaId)) {
        failOutgoingUpload(QStringLiteral("Upload transport failed before transfer started"));
        return;
    }

    if (m_outgoingStallTimer) m_outgoingStallTimer->start();
    scheduleOutgoingPump();
}

void UploadManager::scheduleOutgoingPump() {
    if (!m_outgoingPumpTimer || m_outgoingPumpTimer->isActive()
        || !m_uploadInProgress || m_cancelRequested
        || m_uploadRejectedDuringSend || m_outgoingPayloadCompleteSent
        || m_currentUploadId.isEmpty()) {
        return;
    }
    m_outgoingPumpTimer->start();
}

void UploadManager::stopOutgoingPump() {
    if (m_outgoingPumpTimer) m_outgoingPumpTimer->stop();
    if (m_outgoingStallTimer) m_outgoingStallTimer->stop();
    if (m_outgoingAckTimer) m_outgoingAckTimer->stop();
    if (m_outgoingFileHandle.isOpen()) m_outgoingFileHandle.close();
}

void UploadManager::failOutgoingUpload(const QString& reason) {
    const QString uploadId = m_currentUploadId;
    if (uploadId.isEmpty()) return;

    stopOutgoingPump();
    if (m_ws) {
        // If the pinned payload transport was lost, WebSocketClient sends this
        // terminating abort over the authenticated control connection only.
        m_ws->sendUploadAbort(m_uploadTargetClientId, uploadId,
                              reason, m_activeIdeaId);
    }
    onUploadRejected(uploadId, reason);
}

void UploadManager::pumpOutgoingUpload() {
    if (m_outgoingPumpRunning || !m_uploadInProgress || m_cancelRequested
        || m_uploadRejectedDuringSend || m_outgoingPayloadCompleteSent
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
           && m_uploadInProgress && !m_cancelRequested
           && !m_uploadRejectedDuringSend && !m_outgoingPayloadCompleteSent) {
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

        if (m_outgoingFileIndex >= m_outgoingFiles.size()) {
            if (m_sentBytes != m_totalBytes) {
                failOutgoingUpload(QStringLiteral("Source files were not read completely"));
                return;
            }
            if (!m_ws->sendUploadComplete(m_uploadTargetClientId, m_currentUploadId,
                                          m_activeIdeaId)) {
                failOutgoingUpload(QStringLiteral(
                    "Upload transport was interrupted before completion"));
                return;
            }

            m_outgoingPayloadCompleteSent = true;
            m_finalizing = true;
            if (m_outgoingPumpTimer) m_outgoingPumpTimer->stop();
            if (m_outgoingStallTimer) m_outgoingStallTimer->stop();
            if (m_outgoingAckTimer) m_outgoingAckTimer->start();
            m_actionInProgress = false;
            if (m_actionDebounceTimer) m_actionDebounceTimer->stop();
            emit uiStateChanged();
            return;
        }

        const UploadFileInfo fileInfo = m_outgoingFiles.at(m_outgoingFileIndex);
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
            m_outgoingChunkIndex = 0;
            m_outgoingSentForFile = 0;
            emit fileUploadStarted(fileInfo.fileId);
            if (!m_uploadInProgress || m_cancelRequested
                || m_uploadRejectedDuringSend) {
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

        const int chunkIndex = m_outgoingChunkIndex;
        if (!m_ws->sendUploadChunk(m_uploadTargetClientId, m_currentUploadId,
                                   fileInfo.fileId, chunkIndex, chunk.toBase64(),
                                   m_activeIdeaId)) {
            failOutgoingUpload(QStringLiteral("Upload transport was interrupted"));
            return;
        }
        ++m_outgoingChunkIndex;
        ++chunksQueuedThisPass;
        m_outgoingSentForFile += chunk.size();
        m_sentBytes += chunk.size();
        if (m_outgoingStallTimer) m_outgoingStallTimer->start();

        const int filePercent = static_cast<int>(std::round(
            m_outgoingSentForFile * 100.0 / static_cast<double>(fileInfo.size)));
        updatePerFileLocalProgress(fileInfo.fileId, filePercent);
        if (!m_uploadInProgress || m_cancelRequested
            || m_uploadRejectedDuringSend) {
            return;
        }

        const int globalPercent = m_totalBytes > 0
            ? std::clamp(static_cast<int>(std::round(
                  m_sentBytes * 100.0 / static_cast<double>(m_totalBytes))), 0, 99)
            : 0;
        updateLocalProgress(globalPercent, m_outgoingFileIndex);
        if (!m_uploadInProgress || m_cancelRequested
            || m_uploadRejectedDuringSend) {
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
            updatePerFileLocalProgress(fileInfo.fileId, 99);
            updateLocalProgress(globalPercent, m_outgoingFileIndex);
            emit fileUploadFinished(fileInfo.fileId);
            if (!m_uploadInProgress || m_cancelRequested
                || m_uploadRejectedDuringSend) {
                return;
            }
        }
    }

    if (m_uploadInProgress && !m_cancelRequested
        && !m_uploadRejectedDuringSend && !m_outgoingPayloadCompleteSent) {
        scheduleOutgoingPump();
    }
}

// collectSceneFiles removed; files now gathered by caller (MainWindow)

void UploadManager::resetToInitial() {
    stopOutgoingPump();
    m_uploadActive = false;
    m_uploadInProgress = false;
    m_cancelRequested = false;
    m_uploadRejectedDuringSend = false;
    m_uploadWasActiveBeforeStart = false;
    m_finalizing = false;
    m_cancelFinalizePending = false;
    m_actionInProgress = false;
    m_currentUploadId.clear();
    m_lastPercent = 0;
    m_filesCompleted = 0;
    m_totalFiles = 0;
    m_sentBytes = 0;
    m_totalBytes = 0;
    m_remoteProgressReceived = false;
    m_outgoingFiles.clear();
    m_outgoingFileIndex = 0;
    m_outgoingChunkIndex = 0;
    m_outgoingSentForFile = 0;
    m_outgoingPayloadCompleteSent = false;
    resetProgressTracking();
    if (m_cancelFallbackTimer) m_cancelFallbackTimer->stop();
    if (m_removalAckTimer) m_removalAckTimer->stop();
    if (m_actionDebounceTimer) m_actionDebounceTimer->stop();
    m_uploadTargetClientId.clear();
    m_pendingRemovalId.clear();
    m_pendingRemovalTargetId.clear();
    m_pendingRemovalCanvasSessionId.clear();
    m_activeSessionIdentity.clear();
    m_activeIdeaId.clear();
    if (m_ws) m_ws->endUploadSession();
}

void UploadManager::finalizeLocalCancelState() {
    if (!m_cancelFinalizePending) return;
    const QString targetId = !m_lastRemovalClientId.isEmpty()
                               ? m_lastRemovalClientId
                               : (!m_uploadTargetClientId.isEmpty() ? m_uploadTargetClientId : m_targetClientId);
    m_cancelFinalizePending = false;
    resetToInitial();
    m_lastRemovalClientId = targetId;
    if (!targetId.isEmpty()) {
        m_fileManager->unmarkAllForClient(targetId);
    }
    emit allFilesRemoved();
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
    if (m_totalFiles <= 0) return;
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

void UploadManager::discardActiveIncomingSession(bool rememberRejectedUpload) {
    const QString uploadId = m_incoming.uploadId;
    const QString cacheDirPath = m_incoming.cacheDirPath;
    const QHash<QString, QString> ownedPaths = m_incoming.filePaths;

    for (auto it = m_incoming.openFiles.begin(); it != m_incoming.openFiles.end(); ++it) {
        if (!it.value()) continue;
        it.value()->close();
        delete it.value();
    }
    m_incoming.openFiles.clear();

    for (auto it = ownedPaths.constBegin(); it != ownedPaths.constEnd(); ++it) {
        const QString path = it.value();
        if (path.isEmpty() || cacheDirPath.isEmpty() || !pathIsInsideDirectory(path, cacheDirPath)) continue;

        const QString mappedPath = m_fileManager->getFilePathForId(it.key());
        if (!mappedPath.isEmpty()
            && QDir::cleanPath(QFileInfo(mappedPath).absoluteFilePath())
                == QDir::cleanPath(QFileInfo(path).absoluteFilePath())) {
            m_fileManager->removeReceivedFileMapping(it.key());
        }
        QFile::remove(path);
    }

    if (!cacheDirPath.isEmpty() && pathIsInsideDirectory(cacheDirPath, incomingUploadsRoot())) {
        QDir directory(cacheDirPath);
        if (directory.exists()
            && directory.entryInfoList(QDir::AllEntries | QDir::NoDotAndDotDot).isEmpty()) {
            const QString senderDirectoryPath = QFileInfo(cacheDirPath).absolutePath();
            if (QDir().rmdir(cacheDirPath)
                && pathIsInsideDirectory(senderDirectoryPath, incomingUploadsRoot())) {
                QDir senderDirectory(senderDirectoryPath);
                if (senderDirectory.entryInfoList(QDir::AllEntries | QDir::NoDotAndDotDot).isEmpty()) {
                    QDir().rmdir(senderDirectoryPath);
                }
            }
        }
    }

    clearIncomingChunkTracking(uploadId);
    m_incoming = IncomingUploadSession();

    if (rememberRejectedUpload && !uploadId.isEmpty() && uploadId.size() <= 128) {
        if (m_canceledIncoming.size() >= 256) m_canceledIncoming.clear();
        m_canceledIncoming.insert(uploadId);
    }
}

void UploadManager::rejectIncomingUpload(const QString& senderId,
                                         const QString& uploadId,
                                         const QString& reason,
                                         bool discardMatchingSession) {
    const bool matchesActive = !m_incoming.uploadId.isEmpty()
        && m_incoming.uploadId == uploadId
        && (senderId.isEmpty() || m_incoming.senderId == senderId);
    const QString effectiveSender = senderId.isEmpty() && matchesActive ? m_incoming.senderId : senderId;
    const QString canvasSessionId = matchesActive ? m_incoming.canvasSessionId : QString();

    qWarning() << "UploadManager: Rejecting incoming upload" << uploadId << reason;
    if (discardMatchingSession && matchesActive) {
        discardActiveIncomingSession(true);
    }
    if (m_ws && !effectiveSender.isEmpty() && !uploadId.isEmpty()) {
        m_ws->notifyUploadRejectedToSender(effectiveSender, uploadId, reason, canvasSessionId);
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
        fileIds = m_incoming.expectedSizes.keys();
        ownedPaths = m_incoming.filePaths;

        clearIncomingChunkTracking(uploadId);
        m_canceledIncoming.remove(uploadId);

        m_incoming = IncomingUploadSession();
    } else {
        if (cacheDirPath.isEmpty() && !senderId.isEmpty()) {
            QString base = QStandardPaths::writableLocation(QStandardPaths::CacheLocation);
            if (base.isEmpty()) base = QDir::homePath() + "/.cache";
            cacheDirPath = base + "/Mouffette/Uploads/" + senderId;
        }
        if (uploadId.isEmpty()) uploadId = uploadIdOverride;
    }

    if (!uploadIdOverride.isEmpty() && uploadIdOverride != uploadId) {
        clearIncomingChunkTracking(uploadIdOverride);
        m_canceledIncoming.remove(uploadIdOverride);
    }

    if (deleteDiskContents) {
        for (auto it = ownedPaths.constBegin(); it != ownedPaths.constEnd(); ++it) {
            if (!cacheDirPath.isEmpty() && pathIsInsideDirectory(it.value(), cacheDirPath)) {
                const QFileInfo info(it.value());
                if (info.exists() && !QFile::remove(it.value())) {
                    qWarning() << "UploadManager: Failed to remove partial cached file"
                               << it.value();
                    return false;
                }
            }
        }
    }

    // Phase 3: canvasSessionId is MANDATORY - check if it's a specific idea or default
    const bool ideaScoped = (canvasSessionId != DEFAULT_IDEA_ID);
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
        if (deleteDiskContents && !cacheDirPath.isEmpty()) {
            QDir dir(cacheDirPath);
            if (dir.exists()) {
                if (!dir.removeRecursively()) {
                    qWarning() << "UploadManager: Failed to remove cache directory during cleanup:" << cacheDirPath;
                    return false;
                }
                qDebug() << "UploadManager: Removed cache directory during cleanup:" << cacheDirPath;
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
    } else {
        struct IdeaCleanupOperation {
            QString fileId;
            QString path;
            bool removeMapping = false;
        };
        QVector<IdeaCleanupOperation> operations;

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
                qWarning() << "UploadManager: Refusing idea cleanup outside sender root"
                           << path;
                return false;
            }

            QSet<QString> remainingIdeas = currentIdeas;
            remainingIdeas.remove(canvasSessionId);
            const bool removeMapping = remainingIdeas.isEmpty();

            if (deleteDiskContents && removeMapping) {
                QFileInfo info(path);
                if (info.exists()) {
                    QFile file(path);
                    if (!file.remove()) {
                        qWarning() << "UploadManager: Failed to remove cached file" << path << "for idea" << canvasSessionId;
                        return false;
                    }
                    qDebug() << "UploadManager: Removed cached file" << path << "for idea" << canvasSessionId;
                }
            }
            operations.append({fid, path, removeMapping});
        }

        // No mapping or association is changed until all deletes above have
        // succeeded. This makes a failed removal safely retryable.
        for (const IdeaCleanupOperation& operation : std::as_const(operations)) {
            m_fileManager->dissociateFileFromIdea(operation.fileId, canvasSessionId);
            if (operation.removeMapping) {
                m_fileManager->removeReceivedFileMapping(operation.fileId);
                removeEmptyUploadParentsForFile(operation.path);
            }
        }

        if (deleteDiskContents && !cacheDirPath.isEmpty()) {
            QDir dir(cacheDirPath);
            if (dir.exists() && dir.entryInfoList(QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot).isEmpty()) {
                if (!dir.rmdir(cacheDirPath)) {
                    qWarning() << "UploadManager: Failed to remove empty cache directory" << cacheDirPath;
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

// Slots forwarded from WebSocketClient (sender side)
void UploadManager::onUploadProgress(const QString& uploadId, int percent, int filesCompleted, int totalFiles) {
    if (uploadId != m_currentUploadId) return;
    if (m_cancelRequested) return;
    // Always accept target-side progress; it's authoritative
    m_lastPercent = percent;
    m_filesCompleted = filesCompleted;
    if (totalFiles > 0) {
        m_totalFiles = totalFiles;
    }
    updateRemoteProgress(percent, filesCompleted);
}

void UploadManager::onUploadCompletedFileIds(const QString& uploadId, const QStringList& fileIds) {
    if (uploadId != m_currentUploadId) return;
    if (m_cancelRequested) return;
    if (fileIds.isEmpty()) return;
    emit uploadCompletedFileIds(fileIds);
    for (const QString& fid : fileIds) {
        updatePerFileRemoteProgress(fid, 100);
    }
}

void UploadManager::onUploadFinished(const QString& uploadId) {
    if (uploadId != m_currentUploadId) return;
    if (m_cancelRequested) return;
    stopOutgoingPump();
    updateRemoteProgress(100, m_totalFiles > 0 ? m_totalFiles : m_filesCompleted);
    // Switch to finalizing for a brief moment to align UI state, then finish
    m_uploadInProgress = false;
    m_finalizing = true;
    emit uiStateChanged();
    
    // Mark all uploaded files and media as available on the target client
    for (const auto& f : m_outgoingFiles) {
        m_fileManager->markFileUploadedToClient(f.fileId, m_uploadTargetClientId);
        // File-based tracking covers all media instances
        const QList<QString> mediaIds = m_fileManager->getMediaIdsForFile(f.fileId);
    }
    
    m_uploadActive = true; // switch to active state
    m_uploadInProgress = false;
    m_finalizing = false; // finalization complete
    m_uploadWasActiveBeforeStart = false;
    m_uploadRejectedDuringSend = false;
    m_actionInProgress = false; // Clear action lock
    emit uploadFinished();
    emit uiStateChanged();
    if (m_ws) m_ws->endUploadSession();
}

void UploadManager::onUploadRejected(const QString& uploadId, const QString& reason) {
    if (uploadId.isEmpty() || uploadId != m_currentUploadId) return;

    const bool preserveExistingRemoteFiles = m_uploadWasActiveBeforeStart;
    stopOutgoingPump();
    m_uploadRejectedDuringSend = true;
    m_cancelRequested = true;
    m_uploadInProgress = false;
    m_finalizing = false;
    m_uploadActive = preserveExistingRemoteFiles;
    m_actionInProgress = false;

    if (!m_uploadTargetClientId.isEmpty()) {
        for (const UploadFileInfo& file : std::as_const(m_outgoingFiles)) {
            m_fileManager->unmarkFileUploadedToClient(file.fileId, m_uploadTargetClientId);
        }
    }

    if (m_actionDebounceTimer) m_actionDebounceTimer->stop();
    if (m_ws) {
        m_ws->cancelUploadId(uploadId);
        m_ws->endUploadSession();
    }

    emit uploadRejected(uploadId, reason.left(512));
    emit uiStateChanged();

    m_currentUploadId.clear();
    m_outgoingFiles.clear();
    m_outgoingFileIndex = 0;
    m_outgoingChunkIndex = 0;
    m_outgoingSentForFile = 0;
    m_outgoingPayloadCompleteSent = false;
    m_lastPercent = 0;
    m_filesCompleted = 0;
    m_totalFiles = 0;
    m_sentBytes = 0;
    m_totalBytes = 0;
    m_remoteProgressReceived = false;
    m_uploadWasActiveBeforeStart = false;
    resetProgressTracking();
}

void UploadManager::onAllFilesRemovedRemote(const QString& removalId,
                                            const QString& targetClientId,
                                            const QString& canvasSessionId) {
    if (removalId.isEmpty() || removalId != m_pendingRemovalId
        || targetClientId != m_pendingRemovalTargetId
        || canvasSessionId != m_pendingRemovalCanvasSessionId) {
        qWarning() << "UploadManager: Ignoring stale or mismatched removal acknowledgement"
                   << removalId << targetClientId << canvasSessionId;
        return;
    }

    m_pendingRemovalId.clear();
    m_pendingRemovalTargetId.clear();
    m_pendingRemovalCanvasSessionId.clear();
    if (m_removalAckTimer) m_removalAckTimer->stop();
    if (m_cancelFinalizePending) {
        finalizeLocalCancelState();
        return;
    }

    // Remote side confirmed unload; reset state
    // Clear all uploaded markers for this client so that all items are considered Not uploaded
    const QString removedClientId = !m_lastRemovalClientId.isEmpty()
                                        ? m_lastRemovalClientId
                                        : (!m_uploadTargetClientId.isEmpty() ? m_uploadTargetClientId : m_targetClientId);
    if (!removedClientId.isEmpty()) {
        m_fileManager->unmarkAllForClient(removedClientId);
    }

    // Now reset state
    resetToInitial();

    m_lastRemovalClientId = removedClientId;
    m_actionInProgress = false; // Clear action lock after removal confirmed

    emit allFilesRemoved();
    emit uiStateChanged();
}

void UploadManager::onConnectionLost() {
    // If we were uploading or finalizing, treat it as an aborted session.
    const bool hadOngoing = m_uploadInProgress || m_finalizing;

    if (hadOngoing) {
        stopOutgoingPump();
        // Cancel local flags immediately
        m_cancelRequested = true;
        m_uploadInProgress = false;
        m_finalizing = false;

        // Do not mark anything as uploaded; roll back any optimistic UI
        // Unmark any files that were part of the outgoing batch but not yet confirmed by onUploadFinished
        if (!m_uploadTargetClientId.isEmpty()) {
            for (const auto& f : m_outgoingFiles) {
                m_fileManager->unmarkFileUploadedToClient(f.fileId, m_uploadTargetClientId);
                // File-based unmark covers all media instances
            }
        }

        // Notify UI to recompute button state and progress text
        emit uiStateChanged();

        // Leave m_uploadActive = false so next click starts a fresh upload.
        m_uploadActive = false;
        m_currentUploadId.clear();
        m_lastPercent = 0;
        m_filesCompleted = 0;
        m_totalFiles = 0;
        m_sentBytes = 0;
        m_totalBytes = 0;
        m_remoteProgressReceived = false;
        m_outgoingFiles.clear();
    }

    if (m_ws) {
        m_ws->endUploadSession();
    }

    // Any acknowledgement from the old transport is stale after reconnect.
    m_pendingRemovalId.clear();
    m_pendingRemovalTargetId.clear();
    m_pendingRemovalCanvasSessionId.clear();
    if (m_removalAckTimer) m_removalAckTimer->stop();

    cleanupIncomingCacheForConnectionLoss();
}

void UploadManager::cleanupIncomingCacheForConnectionLoss() {
    // Only discard the active partial transfer. Previously this removed the
    // entire uploads root, including already-validated files from other peers.
    if (!m_incoming.senderId.isEmpty()) {
        qDebug() << "UploadManager: Clearing incoming upload cache for sender" << m_incoming.senderId << "after connection loss";
    }
    discardActiveIncomingSession(false);
    m_expectedChunkIndex.clear();
    m_canceledIncoming.clear();
}

// Incoming side (target) - replicate subset of MainWindow logic for assembling files
void UploadManager::handleIncomingMessage(const QJsonObject& message) {
    const QString type = message.value("type").toString();
    if (type == "upload_start") {
        const QString senderId = senderCacheNamespace(message);
        const QString uploadId = message.value("uploadId").toString();
        const QString canvasSessionId = message.value("canvasSessionId").toString();

        if (!m_incoming.uploadId.isEmpty()) {
            rejectIncomingUpload(senderId, uploadId,
                                 QStringLiteral("Remote client is already receiving another upload"), false);
            return;
        }
        if (!isValidPeerId(senderId) || !isCanonicalUuid(uploadId)
            || !isValidCanvasSessionId(canvasSessionId)) {
            rejectIncomingUpload(senderId, uploadId, QStringLiteral("Invalid upload identifiers"), false);
            return;
        }
        if (!message.value("files").isArray()) {
            rejectIncomingUpload(senderId, uploadId, QStringLiteral("Invalid upload manifest"), false);
            return;
        }

        const QJsonArray files = message.value("files").toArray();
        if (files.isEmpty() || files.size() > kMaxIncomingFiles) {
            rejectIncomingUpload(senderId, uploadId,
                                 QStringLiteral("Upload manifest has an invalid file count"), false);
            return;
        }

        QVector<ValidatedManifestFile> validatedFiles;
        validatedFiles.reserve(files.size());
        QSet<QString> seenFileIds;
        QSet<QString> seenMediaIds;
        qint64 totalSize = 0;
        QString manifestError;

        for (const QJsonValue& value : files) {
            if (!value.isObject()) {
                manifestError = QStringLiteral("Upload manifest contains a non-object entry");
                break;
            }
            const QJsonObject fileObject = value.toObject();
            if (!fileObject.value("fileId").isString()
                || !fileObject.value("name").isString()
                || !fileObject.value("extension").isString()
                || !fileObject.value("mediaIds").isArray()) {
                manifestError = QStringLiteral("Upload manifest entry has missing or invalid fields");
                break;
            }

            ValidatedManifestFile file;
            file.fileId = fileObject.value("fileId").toString();
            file.name = fileObject.value("name").toString();
            if (!isValidFileId(file.fileId) || seenFileIds.contains(file.fileId)) {
                manifestError = QStringLiteral("Upload manifest contains an invalid or duplicate file identifier");
                break;
            }
            if (!isSafeDisplayName(file.name)
                || !normalizeAndValidateExtension(fileObject.value("extension").toString(), file.extension)
                || QFileInfo(file.name).suffix().compare(file.extension, Qt::CaseInsensitive) != 0) {
                manifestError = QStringLiteral("Upload manifest contains an invalid filename or extension");
                break;
            }
            if (!parseManifestSize(fileObject.value("sizeBytes"), file.size)
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
                if (!mediaIdValue.isString()) {
                    manifestError = QStringLiteral("Upload manifest contains an invalid media identifier");
                    break;
                }
                const QString mediaId = mediaIdValue.toString();
                if (!isCanonicalUuid(mediaId) || seenMediaIds.contains(mediaId)) {
                    manifestError = QStringLiteral("Upload manifest contains an invalid or duplicate media identifier");
                    break;
                }
                seenMediaIds.insert(mediaId);
                file.mediaIds.append(mediaId);
            }
            if (!manifestError.isEmpty()) break;

            seenFileIds.insert(file.fileId);
            totalSize += file.size;
            validatedFiles.append(file);
        }

        if (!manifestError.isEmpty()) {
            rejectIncomingUpload(senderId, uploadId, manifestError, false);
            return;
        }

        const QString uploadsRoot = incomingUploadsRoot();
        if (!QDir().mkpath(uploadsRoot)) {
            rejectIncomingUpload(senderId, uploadId,
                                 QStringLiteral("Remote client could not create its upload cache"), false);
            return;
        }
        QDir rootDirectory(uploadsRoot);
        if (!rootDirectory.exists(senderId) && !rootDirectory.mkdir(senderId)) {
            rejectIncomingUpload(senderId, uploadId,
                                 QStringLiteral("Remote client could not create a sender cache"), false);
            return;
        }

        const QString rootCanonicalPath = QFileInfo(uploadsRoot).canonicalFilePath();
        const QString senderPath = rootDirectory.absoluteFilePath(senderId);
        const QFileInfo senderInfo(senderPath);
        const QString senderCanonicalPath = senderInfo.canonicalFilePath();
        if (rootCanonicalPath.isEmpty() || senderCanonicalPath.isEmpty()
            || !senderInfo.isDir() || senderInfo.isSymLink()
            || !pathIsInsideDirectory(senderCanonicalPath, rootCanonicalPath)) {
            rejectIncomingUpload(senderId, uploadId, QStringLiteral("Unsafe sender cache directory"), false);
            return;
        }

        QDir senderDirectory(senderCanonicalPath);
        if (senderDirectory.exists(uploadId) || !senderDirectory.mkdir(uploadId)) {
            rejectIncomingUpload(senderId, uploadId,
                                 QStringLiteral("Upload staging directory already exists"), false);
            return;
        }

        const QString stagingPath = senderDirectory.absoluteFilePath(uploadId);
        const QFileInfo stagingInfo(stagingPath);
        const QString stagingCanonicalPath = stagingInfo.canonicalFilePath();
        if (rootCanonicalPath.isEmpty() || stagingCanonicalPath.isEmpty()
            || !stagingInfo.isDir() || stagingInfo.isSymLink()
            || !pathIsInsideDirectory(stagingCanonicalPath, senderCanonicalPath)) {
            QDir().rmdir(stagingPath);
            rejectIncomingUpload(senderId, uploadId, QStringLiteral("Unsafe upload staging directory"), false);
            return;
        }

        m_incoming.senderId = senderId;
        m_incoming.uploadId = uploadId;
        m_incoming.canvasSessionId = canvasSessionId;
        m_incoming.cacheDirPath = stagingCanonicalPath;
        m_incoming.totalFiles = validatedFiles.size();
        m_incoming.totalSize = totalSize;
        m_canceledIncoming.remove(uploadId);

        for (const ValidatedManifestFile& file : std::as_const(validatedFiles)) {
            QString filename = file.fileId;
            if (!file.extension.isEmpty()) filename += QLatin1Char('.') + file.extension;
            const QString fullPath = QDir(m_incoming.cacheDirPath).absoluteFilePath(filename);
            if (!pathIsInsideDirectory(fullPath, m_incoming.cacheDirPath)) {
                rejectIncomingUpload(senderId, uploadId, QStringLiteral("Unsafe upload path"), true);
                return;
            }

            auto* output = new QFile(fullPath);
            if (!output->open(QIODevice::WriteOnly | QIODevice::NewOnly)) {
                delete output;
                rejectIncomingUpload(senderId, uploadId,
                                     QStringLiteral("Remote client could not create an upload file"), true);
                return;
            }

            m_incoming.openFiles.insert(file.fileId, output);
            m_incoming.expectedSizes.insert(file.fileId, file.size);
            m_incoming.receivedByFile.insert(file.fileId, 0);
            m_incoming.filePaths.insert(file.fileId, fullPath);
            m_incoming.fileIdToName.insert(file.fileId, file.name);
            m_incoming.fileIdToExtension.insert(file.fileId, file.extension);
            for (const QString& mediaId : file.mediaIds) {
                m_incoming.fileIdToMediaId.insert(file.fileId, mediaId);
            }
            m_expectedChunkIndex.insert(uploadId + QLatin1Char(':') + file.fileId, 0);
        }

        if (m_ws) {
            m_ws->notifyUploadProgressToSender(senderId, uploadId, 0, 0,
                                               m_incoming.totalFiles, QStringList());
        }
    } else if (type == "upload_chunk") {
        const QString senderId = senderCacheNamespace(message);
        const QString uploadId = message.value("uploadId").toString();
        if (m_canceledIncoming.contains(uploadId)) return;
        if (uploadId != m_incoming.uploadId || senderId != m_incoming.senderId) {
            rejectIncomingUpload(senderId, uploadId, QStringLiteral("No matching upload session"), false);
            return;
        }
        const QString canvasSessionId = message.value("canvasSessionId").toString();
        if (canvasSessionId != m_incoming.canvasSessionId) {
            rejectIncomingUpload(senderId, uploadId, QStringLiteral("Upload session identifier mismatch"), true);
            return;
        }
        if (!message.value("fileId").isString()) {
            rejectIncomingUpload(senderId, uploadId, QStringLiteral("Upload chunk has no file identifier"), true);
            return;
        }
        const QString fid = message.value("fileId").toString();
        QFile* qf = m_incoming.openFiles.value(fid, nullptr);
        if (!isValidFileId(fid) || !qf || !qf->isOpen()) {
            rejectIncomingUpload(senderId, uploadId, QStringLiteral("Upload chunk references an unknown file"), true);
            return;
        }

        int chunkIndex = -1;
        QByteArray data;
        if (!parseChunkIndex(message.value("chunkIndex"), chunkIndex)
            || !decodeCanonicalChunk(message.value("data"), data)) {
            rejectIncomingUpload(senderId, uploadId, QStringLiteral("Upload chunk is malformed or too large"), true);
            return;
        }

        const QString key = uploadId + QLatin1Char(':') + fid;
        if (!m_expectedChunkIndex.contains(key)) {
            rejectIncomingUpload(senderId, uploadId, QStringLiteral("Upload chunk state is missing"), true);
            return;
        }
        const int expected = m_expectedChunkIndex.value(key);
        if (chunkIndex != expected) {
            rejectIncomingUpload(senderId, uploadId, QStringLiteral("Upload chunks arrived out of order"), true);
            return;
        }

        const qint64 receivedForFile = m_incoming.receivedByFile.value(fid, -1);
        const qint64 expectedForFile = m_incoming.expectedSizes.value(fid, -1);
        if (receivedForFile < 0 || expectedForFile < 1
            || data.size() > expectedForFile - receivedForFile
            || qf->size() != receivedForFile
            || !qf->seek(receivedForFile)) {
            rejectIncomingUpload(senderId, uploadId, QStringLiteral("Upload chunk exceeds the declared file size"), true);
            return;
        }

        const qint64 written = qf->write(data);
        if (written != data.size()) {
            rejectIncomingUpload(senderId, uploadId, QStringLiteral("Remote client could not write the upload"), true);
            return;
        }
        m_expectedChunkIndex[key] = expected + 1;
        m_incoming.receivedByFile[fid] = receivedForFile + written;
        m_incoming.received += written;

        if (m_ws && m_incoming.totalSize > 0) {
            const int percent = std::clamp(static_cast<int>(std::round(
                                               m_incoming.received * 100.0 / m_incoming.totalSize)),
                                           0, 99);
            QJsonArray perFileArr;
            const int filePercent = std::clamp(static_cast<int>(std::round(
                                                   (receivedForFile + written) * 100.0 / expectedForFile)),
                                               0, 99);
            QJsonObject progressObject;
            progressObject["fileId"] = fid;
            progressObject["percent"] = filePercent;
            perFileArr.append(progressObject);
            m_ws->notifyUploadProgressToSender(senderId, uploadId, percent, 0,
                                               m_incoming.totalFiles, QStringList(), perFileArr);
        }
    } else if (type == "upload_complete") {
        const QString senderId = senderCacheNamespace(message);
        const QString uploadId = message.value("uploadId").toString();
        if (m_canceledIncoming.contains(uploadId)) return;
        if (uploadId != m_incoming.uploadId || senderId != m_incoming.senderId) {
            rejectIncomingUpload(senderId, uploadId, QStringLiteral("No matching upload session"), false);
            return;
        }
        const QString canvasSessionId = message.value("canvasSessionId").toString();
        if (canvasSessionId != m_incoming.canvasSessionId) {
            rejectIncomingUpload(senderId, uploadId, QStringLiteral("Upload session identifier mismatch"), true);
            return;
        }

        bool closeSucceeded = true;
        for (auto it = m_incoming.openFiles.begin(); it != m_incoming.openFiles.end(); ++it) {
            if (!it.value()) {
                closeSucceeded = false;
                continue;
            }
            closeSucceeded = it.value()->flush() && closeSucceeded;
            it.value()->close();
            closeSucceeded = (it.value()->error() == QFileDevice::NoError) && closeSucceeded;
            delete it.value();
        }
        m_incoming.openFiles.clear();

        QString completionError;
        QHash<QString, QString> reusableExistingPaths;
        QStringList validatedFileIds = m_incoming.expectedSizes.keys();
        std::sort(validatedFileIds.begin(), validatedFileIds.end());
        if (!closeSucceeded || validatedFileIds.size() != m_incoming.totalFiles) {
            completionError = QStringLiteral("Remote client could not finalize every upload file");
        }

        for (const QString& fileId : std::as_const(validatedFileIds)) {
            if (!completionError.isEmpty()) break;
            const qint64 expectedSize = m_incoming.expectedSizes.value(fileId, -1);
            const qint64 receivedSize = m_incoming.receivedByFile.value(fileId, -1);
            const QString path = m_incoming.filePaths.value(fileId);
            const QFileInfo info(path);
            if (expectedSize < 1 || receivedSize != expectedSize
                || !info.exists() || !info.isFile() || info.size() != expectedSize) {
                completionError = QStringLiteral("Upload is incomplete or its size does not match the manifest");
                break;
            }
            if (!MediaFilePolicy::isAcceptedLocalFile(path)) {
                completionError = QStringLiteral("Unsupported or invalid media file: %1")
                                      .arg(m_incoming.fileIdToName.value(fileId, fileId));
                break;
            }
            const QString existingPath = m_fileManager->getFilePathForId(fileId);
            if (!existingPath.isEmpty()) {
                const QFileInfo existingInfo(existingPath);
                const QString existingCanonicalPath = existingInfo.canonicalFilePath();
                const QString senderRoot = QFileInfo(m_incoming.cacheDirPath).absolutePath();
                const QString expectedExtension = m_incoming.fileIdToExtension.value(fileId);
                if (existingCanonicalPath.isEmpty() || !existingInfo.isFile()
                    || existingInfo.isSymLink()
                    || !pathIsInsideDirectory(existingCanonicalPath, senderRoot)
                    || existingInfo.size() != expectedSize
                    || existingInfo.suffix().compare(expectedExtension, Qt::CaseInsensitive) != 0
                    || !MediaFilePolicy::isAcceptedLocalFile(existingCanonicalPath)
                    || !filesHaveIdenticalContents(existingCanonicalPath, path)) {
                    completionError = QStringLiteral(
                        "Upload file identifier collided with a different or foreign target file");
                    break;
                }
                reusableExistingPaths.insert(fileId, existingCanonicalPath);
            }
        }

        if (!completionError.isEmpty()) {
            rejectIncomingUpload(senderId, uploadId, completionError, true);
            return;
        }

        // Remove byte-identical staging duplicates before changing mappings. A
        // failed delete rejects the request and leaves the existing mapping as
        // the authoritative retry point.
        for (auto it = reusableExistingPaths.constBegin();
             it != reusableExistingPaths.constEnd(); ++it) {
            const QString duplicatePath = m_incoming.filePaths.value(it.key());
            if (duplicatePath.isEmpty() || !QFile::remove(duplicatePath)) {
                completionError = QStringLiteral(
                    "Remote client could not remove an idempotent upload duplicate");
                break;
            }
        }
        if (!completionError.isEmpty()) {
            rejectIncomingUpload(senderId, uploadId, completionError, true);
            return;
        }

        for (const QString& fileId : std::as_const(validatedFileIds)) {
            if (reusableExistingPaths.contains(fileId)) continue;
            const QString path = m_incoming.filePaths.value(fileId);
            m_fileManager->registerReceivedFilePath(fileId, path);
            if (QDir::cleanPath(QFileInfo(m_fileManager->getFilePathForId(fileId)).absoluteFilePath())
                != QDir::cleanPath(QFileInfo(path).absoluteFilePath())) {
                completionError = QStringLiteral("Remote client could not register an upload file");
                break;
            }
        }

        if (!completionError.isEmpty()) {
            rejectIncomingUpload(senderId, uploadId, completionError, true);
            return;
        }

        for (const QString& fileId : std::as_const(validatedFileIds)) {
            m_fileManager->associateFileWithIdea(fileId, canvasSessionId);
        }

        const QString completedStagingPath = m_incoming.cacheDirPath;
        clearIncomingChunkTracking(uploadId);
        m_canceledIncoming.remove(uploadId);
        m_incoming = IncomingUploadSession();
        QDir().rmdir(completedStagingPath); // succeeds only for all-reused uploads
        if (m_ws) {
            m_ws->notifyUploadFinishedToSender(senderId, uploadId, canvasSessionId,
                                               validatedFileIds);
        }
    } else if (type == "upload_abort") {
        const QString abortedId = message.value("uploadId").toString();
        const QString senderClientId = senderCacheNamespace(message);
        if (!abortedId.isEmpty() && abortedId == m_incoming.uploadId
            && (senderClientId.isEmpty() || senderClientId == m_incoming.senderId)) {
            discardActiveIncomingSession(true);
        } else if (!abortedId.isEmpty()) {
            if (m_canceledIncoming.size() >= 256) m_canceledIncoming.clear();
            m_canceledIncoming.insert(abortedId);
            clearIncomingChunkTracking(abortedId);
        }
    } else if (type == "remove_all_files") {
        const QString senderClientId = senderCacheNamespace(message);
        const QString canvasSessionId = message.value("canvasSessionId").toString();
        const QString removalId = message.value("removalId").toString();
        if (!isValidPeerId(senderClientId) || !isValidCanvasSessionId(canvasSessionId)
            || !isCanonicalUuid(removalId)) {
            qWarning() << "UploadManager: Ignoring malformed remove_all_files command";
            return;
        }

        const QString uploadRoot = incomingUploadsRoot();
        QString cacheOverride = QDir(uploadRoot).filePath(senderClientId);
        if (!pathIsInsideDirectory(cacheOverride, uploadRoot)) {
            qWarning() << "UploadManager: Refusing unsafe remove_all_files path";
            return;
        }
        const QFileInfo senderInfo(cacheOverride);
        if (senderInfo.exists()) {
            const QString canonicalSenderRoot = senderInfo.canonicalFilePath();
            if (canonicalSenderRoot.isEmpty() || !senderInfo.isDir() || senderInfo.isSymLink()
                || !pathIsInsideDirectory(canonicalSenderRoot, uploadRoot)) {
                qWarning() << "UploadManager: Refusing unsafe remove_all_files sender root";
                return;
            }
            cacheOverride = canonicalSenderRoot;
        }

        // Always use the sender root here, not the active upload's UUID staging
        // directory. DEFAULT_IDEA_ID means every validated upload for that sender.
        if (!cleanupIncomingSession(true, false, senderClientId, cacheOverride,
                                    QString(), canvasSessionId)) {
            qWarning() << "UploadManager: Remote removal failed; acknowledgement withheld"
                       << removalId;
            return;
        }
        // Clear all expected indices; treat as a hard reset
        m_expectedChunkIndex.clear();
        if (m_ws) {
            m_ws->notifyAllFilesRemovedToSender(senderClientId, removalId, canvasSessionId);
        }
    } else if (type == "connection_lost_cleanup") {
        // A disconnect may only remove the partial transfer owned by this
        // session, never validated files belonging to other uploads.
        const QString senderClientId = senderCacheNamespace(message);
        if (!m_incoming.uploadId.isEmpty()
            && (senderClientId.isEmpty() || senderClientId == m_incoming.senderId)) {
            discardActiveIncomingSession(false);
        }
    } else if (type == "remove_file") {
        const QString senderClientId = senderCacheNamespace(message);
        const QString fileId = message.value("fileId").toString();
        const QString canvasSessionId = message.value("canvasSessionId").toString();

        if (isValidPeerId(senderClientId) && isValidFileId(fileId)
            && isValidCanvasSessionId(canvasSessionId)) {
            const QString uploadRoot = incomingUploadsRoot();
            const QString senderPath = QDir(uploadRoot).absoluteFilePath(senderClientId);
            const QFileInfo senderInfo(senderPath);
            const QString senderRoot = senderInfo.canonicalFilePath();
            if (senderRoot.isEmpty() || !senderInfo.isDir() || senderInfo.isSymLink()
                || !pathIsInsideDirectory(senderRoot, uploadRoot)) {
                qWarning() << "UploadManager: Refusing remove_file for unsafe sender root";
                return;
            }

            const QString mappedPath = m_fileManager->getFilePathForId(fileId);
            if (mappedPath.isEmpty()) {
                qDebug() << "UploadManager: No received mapping found for" << fileId;
                return;
            }

            const QFileInfo mappedInfo(mappedPath);
            QString ownedPath = mappedInfo.canonicalFilePath();
            if (ownedPath.isEmpty()) ownedPath = mappedInfo.absoluteFilePath();
            if (mappedInfo.isSymLink() || !pathIsInsideDirectory(ownedPath, senderRoot)) {
                qWarning() << "UploadManager: Refusing cross-sender or non-upload deletion"
                           << mappedPath;
                return;
            }

            bool shouldRemoveFromDisk = true;
            if (canvasSessionId != DEFAULT_IDEA_ID) {
                const QSet<QString> currentIdeas = m_fileManager->getIdeaIdsForFile(fileId);
                if (!currentIdeas.contains(canvasSessionId)) {
                    qWarning() << "UploadManager: Refusing remove_file for unrelated canvas"
                               << canvasSessionId;
                    return;
                }
                m_fileManager->dissociateFileFromIdea(fileId, canvasSessionId);
                const QSet<QString> remainingIdeas = m_fileManager->getIdeaIdsForFile(fileId);
                shouldRemoveFromDisk = remainingIdeas.isEmpty();
            }

            if (!shouldRemoveFromDisk) {
                qDebug() << "UploadManager: Retaining file" << fileId << "because other ideas still reference it";
                return;
            }

            if (mappedInfo.exists()) {
                QFile file(ownedPath);
                if (file.remove()) {
                    qDebug() << "UploadManager: Removed sender-owned cached file" << ownedPath;
                } else {
                    qWarning() << "UploadManager: Failed to remove mapped cached file" << ownedPath;
                    return;
                }
            }

            m_fileManager->removeReceivedFileMapping(fileId);
            removeEmptyUploadParentsForFile(ownedPath);
        }
    }
}

bool UploadManager::canAcceptNewAction() const {
    // Check minimum time interval between actions
    if (m_lastActionTime.isValid() && m_lastActionTime.elapsed() < MIN_ACTION_INTERVAL_MS) {
        return false;
    }
    
    // Check if an action is currently in progress
    if (m_actionInProgress) {
        return false;
    }
    
    return true;
}

void UploadManager::scheduleActionDebounce() {
    m_actionInProgress = true;
    m_lastActionTime.restart();
    
    if (m_actionDebounceTimer) {
        m_actionDebounceTimer->stop();
        m_actionDebounceTimer->start(ACTION_DEBOUNCE_MS);
    }
}
