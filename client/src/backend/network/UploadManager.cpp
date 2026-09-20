#include "backend/network/RetryPolicy.h"
#include "backend/network/NetworkDiagnostics.h"
#include "backend/network/SceneRunCoordinator.h"
#include <QtConcurrent/QtConcurrentRun>
#include <QFutureWatcher>
#include "backend/media/MediaResidencyManager.h"
#include "backend/network/UploadManager.h"
#include "backend/network/WebSocketClient.h"
#include "backend/network/RemoteSessionCoordinator.h"
#include "backend/network/ProtocolConstants.h"
#include "backend/config/AppConfig.h"
#include "backend/runtime/RuntimeProfile.h"
#include "backend/files/FileManager.h"
#include "backend/files/PathSafety.h"
#include "backend/domain/media/MediaFilePolicy.h"
#include "MediaFormatContract.h"
#include <QFileInfo>
#include <QCryptographicHash>
#include <QJsonObject>
#include <QPointer>
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
constexpr qsizetype kMaxIncomingChunkBytes = 32 * 1024;
constexpr qsizetype kMaxEncodedChunkCharacters = ((kMaxIncomingChunkBytes + 2) / 3) * 4;
constexpr qint64 kMaxQueuedUploadBytes = 512LL * 1024;

constexpr qint64 kIncomingProgressAckIntervalBytes = 512LL * 1024;
constexpr int kMaxChunksPerPump = 1;
constexpr int kMaxIncomingCompletionTombstones = 256;
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
    // Protocol v5 supplies the authenticated owner as server-authored routing
    // metadata. Never revive the removed sender aliases from client payloads.
    return message.value("ownerEndpointId").toString();
}

bool isValidOpaqueId(const QString& value) {
    return isValidPeerId(value);
}

bool isValidClientWorkspaceId(const QString& value) {
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

QString sha256ForFile(const QString& path, const std::shared_ptr<std::atomic_bool>& cancelled = {}) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return {};
    QCryptographicHash hash(QCryptographicHash::Sha256);
    while (!file.atEnd()) {
        if (cancelled && cancelled->load(std::memory_order_relaxed)) return {};
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
    const QString path = QFileInfo(QDir(base).filePath(QStringLiteral("Uploads"))).absoluteFilePath();
    const QString canonicalPath = QFileInfo(path).canonicalFilePath();
    return canonicalPath.isEmpty() ? path : canonicalPath;
}

bool pathIsInsideDirectory(const QString& path, const QString& directory) {
    return PathSafety::isDescendant(path, directory);
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
    m_incomingWritePool.setMaxThreadCount(1);
    connect(&MediaResidencyManager::instance(), &MediaResidencyManager::ownerChanged,
            this, [this](const QString& owner) {
        const auto it = m_residentIncoming.constFind(owner);
        if (it != m_residentIncoming.cend()) publishResidency(it->sessionId);
    });
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
            suspendOutgoingForResume(QStringLiteral("Upload transport stalled"));
        }
    });

    m_outgoingStartAckTimer = new QTimer(this);
    m_outgoingStartAckTimer->setSingleShot(true);
    m_outgoingStartAckTimer->setInterval(0);
    connect(m_outgoingStartAckTimer, &QTimer::timeout, this, [this]() {
        if (m_outgoingState == OutgoingState::AwaitingTargetReady
            && !m_currentUploadId.isEmpty()) {
            suspendOutgoingForResume(QStringLiteral("Remote client did not accept the upload in time"));
        }
    });

    m_outgoingAckTimer = new QTimer(this);
    m_outgoingAckTimer->setSingleShot(true);
    m_outgoingAckTimer->setInterval(0);
    connect(m_outgoingAckTimer, &QTimer::timeout, this, [this]() {
        if (m_outgoingState == OutgoingState::AwaitingValidation
            && !m_currentUploadId.isEmpty()) {
            suspendOutgoingForResume(QStringLiteral("Remote client did not validate the upload in time"));
        }
    });

    m_remoteCacheStore = new RemoteCacheStore(remoteCacheRoot, this);
    m_remoteCacheStore->setRetentionPolicy(AppConfig::instance().remoteMediaRetentionMs(),
        static_cast<qint64>(AppConfig::instance().remoteMediaCacheMaxMiB()) * 1024 * 1024);
    m_remoteCacheStore->setCleanupRetryPolicy({AppConfig::instance().deferredCleanupRetryMs(),
        AppConfig::instance().deferredCleanupRetryMaxMs(), AppConfig::instance().reconnectJitterPercent()});
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
    connect(&MediaResidencyManager::instance(),
            &MediaResidencyManager::backgroundWorkFinished,
            this, [this](const QString&) { settleIncomingFileReaders(); });
    connect(m_remoteCacheStore, &RemoteCacheStore::teardownFinished, this,
            [this](const QString& sender, const QString& session, quint64 generation,
                   const QString& teardown) {
        const auto result = m_remoteCacheStore->teardownResult({sender, session, generation}, teardown);
        if (result.acknowledgementSafe()) {
            const auto job = m_terminalCacheTeardowns.constFind(session);
            if (job != m_terminalCacheTeardowns.cend() && job->scope.generation == generation
                && job->scope.senderEndpointId == sender && job->teardownId == teardown)
                m_terminalCacheTeardowns.remove(session);
            emit remoteSessionCacheCommitted(sender, session, generation, result.teardownId,
                                              m_teardownRemovedMappings.take(session), result.quarantinedBytes);
        } else if (result.outcome != RemoteCacheStore::CommitOutcome::Pending) {
            emit remoteSessionCacheCleanupError(sender, session, generation, teardown, result.errorCode);
        }
        emit incomingFileReadersChanged();
    });
    connect(m_remoteCacheStore, &RemoteCacheStore::recoveryFinished, this,
            [this](bool ready, const QString& error) {
        m_remoteCacheReady = ready;
        m_receiverAdvertisementReady = ready && !m_terminalIncomingCleanupAwaitingRenderer;
        m_receiverCleanupError = m_receiverAdvertisementReady ? QString()
            : (error.isEmpty() ? QStringLiteral("renderer_teardown_pending") : error);
        if (m_receiverAdvertisementReady) {
            for (auto it = m_terminalCacheTeardowns.cbegin(); it != m_terminalCacheTeardowns.cend(); ++it)
                m_teardownRemovedMappings.remove(it.key());
            m_terminalCacheTeardowns.clear();
        }
        emit receiverAdvertisementReadinessChanged(receiverReadyForAdvertisement(), m_receiverCleanupError);
        emit incomingFileReadersChanged();
    });
    connect(this, &UploadManager::receiverAdvertisementReadinessChanged, this,
            [this](bool ready, const QString&) {
        if (ready) {
            m_receiverCleanupRetries.cancelAll();
            m_receiverCleanupAttempt = 0;
        } else scheduleReceiverCleanup();
    });
    if (!receiverReadyForAdvertisement()) scheduleReceiverCleanup();
    cleanupOrphanedIncomingCache();

    m_uploadScheduler = new UploadScheduler(
        AppConfig::instance().uploadConcurrency(), this);
    connect(m_uploadScheduler, &UploadScheduler::uploadStartRequested,
            this, &UploadManager::startScheduledUpload);
}

UploadManager::~UploadManager() {
    for (const auto& cancelled : std::as_const(m_uploadVerificationCancellation)) cancelled->store(true);
    for (const auto* incoming : std::as_const(m_incomingUploads)) {
        if (incoming && incoming->validationCancelled) incoming->validationCancelled->store(true);
        if (incoming && incoming->writeCancelled) incoming->writeCancelled->store(true);
    }
    m_incomingWritePool.waitForDone();
    for (const QString& owner : m_residentIncoming.keys())
        MediaResidencyManager::instance().release(owner);
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
    const auto incomingUploads = m_incomingUploads.values();
    m_incomingUploads.clear();
    for (IncomingUploadSession* incoming : incomingUploads) {
        if (!incoming) continue;
        closeIncomingFiles(*incoming, false);
        delete incoming;
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
    if (m_outgoingRamByTarget.contains(m_targetClientId)) return m_targetClientId;
    if (m_pendingUploadVerification.contains(m_targetClientId)) return m_targetClientId;
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
    if (m_outgoingRamByTarget.contains(m_targetClientId)) return OutgoingState::LoadingInRam;
    if (m_pendingUploadVerification.contains(m_targetClientId)) return OutgoingState::Queued;
    if (m_waitingVerifiedUploads.contains(m_targetClientId)) return OutgoingState::Suspended;
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

bool UploadManager::isLoadingInRam() const {
    return m_outgoingRamByTarget.contains(m_targetClientId);
}

int UploadManager::ramFilesCompleted() const {
    return m_outgoingRamByTarget.value(m_targetClientId).settledAssets.size();
}

int UploadManager::ramFilesTotal() const {
    const auto batch = m_outgoingRamByTarget.constFind(m_targetClientId);
    if (batch != m_outgoingRamByTarget.cend()) return batch->assets.size();
    if (const auto* transfer = parallelForTarget(m_targetClientId)) return transfer->totalFiles;
    return m_uploadTargetClientId == m_targetClientId ? m_totalFiles : 0;
}

bool UploadManager::isBusy() const {
    return outgoingState() != OutgoingState::Idle || isRemoving();
}

QString UploadManager::currentUploadId() const {
    if (m_outgoingRamByTarget.contains(m_targetClientId))
        return m_outgoingRamByTarget.value(m_targetClientId).uploadId;
    if (m_waitingVerifiedUploads.contains(m_targetClientId)) return m_waitingVerifiedUploads.value(m_targetClientId).second;
    if (m_pendingUploadVerification.contains(m_targetClientId))
        return m_verifyingUploadIds.value(m_targetClientId);
    if (ParallelOutgoingTransfer* transfer = parallelForTarget(m_targetClientId)) {
        return transfer->uploadId;
    }
    return (m_targetClientId.isEmpty() || m_uploadTargetClientId == m_targetClientId)
        ? m_currentUploadId : QString();
}

QString UploadManager::currentRemoteSessionId() const {
    if (m_outgoingRamByTarget.contains(m_targetClientId))
        return m_outgoingRamByTarget.value(m_targetClientId).remoteSessionId;
    if (ParallelOutgoingTransfer* transfer = parallelForTarget(m_targetClientId)) {
        return transfer->remoteSessionId;
    }
    return (m_targetClientId.isEmpty() || m_uploadTargetClientId == m_targetClientId)
        ? m_outgoingRemoteSessionId : QString();
}

quint64 UploadManager::currentRemoteSessionGeneration() const {
    if (m_outgoingRamByTarget.contains(m_targetClientId))
        return m_outgoingRamByTarget.value(m_targetClientId).generation;
    if (ParallelOutgoingTransfer* transfer = parallelForTarget(m_targetClientId)) {
        return transfer->generation;
    }
    return (m_targetClientId.isEmpty() || m_uploadTargetClientId == m_targetClientId)
        ? m_outgoingGeneration : 0;
}

int UploadManager::activeOutgoingTransferCount() const {
    return (m_currentUploadId.isEmpty() ? 0 : 1)
        + m_parallelOutgoingByUpload.size() + m_outgoingRamByTarget.size();
}

void UploadManager::setOutgoingState(OutgoingState state) {
    if (m_outgoingState == state) return;
    NetworkDiagnostics::record(QStringLiteral("upload_transition"), {
        {"uploadId", m_currentUploadId}, {"remoteSessionId", m_outgoingRemoteSessionId},
        {"generation", static_cast<double>(m_outgoingGeneration)},
        {"before", QString::number(static_cast<int>(m_outgoingState))},
        {"after", QString::number(static_cast<int>(state))},
        {"sentBytes", static_cast<double>(m_sentBytes)},
        {"confirmedBytes", static_cast<double>(m_remoteAcknowledgedBytes)}});
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
        for (const QString& path : m_fileManager->getRecordedFilePathsForId(fileId)) {
            if (!path.isEmpty() && pathIsInsideDirectory(path, rootPath))
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
            qWarning() << "UploadManager: refusing obsolete cache symlink";
            continue;
        }
        if (!senderEntry.isDir()) continue;

        const QString restoredSenderName = originalQuarantineName(senderEntry.fileName());
        if (!restoredSenderName.isEmpty()) {
            const QString originalSenderPath = root.absoluteFilePath(restoredSenderName);
            if (hasTrackedPathUnder(originalSenderPath)
                && !QFileInfo::exists(originalSenderPath)) {
                if (!root.rename(senderEntry.fileName(), restoredSenderName)) {
                    qCritical() << "UploadManager: could not restore interrupted cache removal";
                }
            } else if (!QDir(senderEntry.absoluteFilePath()).removeRecursively()) {
                qWarning() << "UploadManager: could not purge stale cache quarantine";
            }
            continue;
        }

        QDir senderDirectory(senderEntry.absoluteFilePath());
        const QFileInfoList uploadEntries = senderDirectory.entryInfoList(
            QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System);
        for (const QFileInfo& uploadEntry : uploadEntries) {
            if (uploadEntry.isSymLink()) {
                qWarning() << "UploadManager: refusing obsolete upload symlink";
                continue;
            }
            if (!uploadEntry.isDir()) continue;
            // Protocol-v5 scopes are owned exclusively by RemoteCacheStore's
            // intent/tombstone transaction. In particular, never let this
            // orphan sweep recursively delete a live scope whose atomic
            // quarantine is blocked: doing so would erase the evidence while
            // incorrectly making receiver advertisement appear safe.
            if (QFileInfo::exists(QDir(uploadEntry.absoluteFilePath())
                                      .filePath(QStringLiteral(".scope.json")))) {
                continue;
            }
            if (!hasTrackedPathUnder(uploadEntry.absoluteFilePath())) {
                if (!QDir(uploadEntry.absoluteFilePath()).removeRecursively()) {
                    qWarning() << "UploadManager: could not purge orphaned upload staging";
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
                        qCritical() << "UploadManager: could not restore interrupted file removal";
                    }
                } else if (!QFile::remove(cachedFile.absoluteFilePath())) {
                    qWarning() << "UploadManager: could not purge stale file quarantine";
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
        QObject::disconnect(this, &UploadManager::uploadProtocolResponseReady,
                            m_ws, &WebSocketClient::sendUploadProtocolResponse);
    }
    m_ws = client;
    if (!client) return;

    m_uploadBytesWrittenConnection = connect(
        client, &WebSocketClient::uploadTransportBytesWritten,
        this, [this](qint64) {
            if (m_outgoingState == OutgoingState::Suspended) resumeOutgoingUpload();
            const auto waiting = m_parallelOutgoingByUpload.values();
            for (auto* transfer : waiting)
                if (transfer && transfer->state == OutgoingState::Suspended) resumeParallel(transfer);
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
            // Data channel loss preserves IDs and durable offsets. Control
            // remains available, but never carries file payloads.
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

    m_webSocketConnections.append(connect(client, &WebSocketClient::mediaResidencyReceived,
        this, &UploadManager::receiveResidency));
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
    m_webSocketConnections.append(connect(client, &WebSocketClient::remoteSessionLogicallyClosed,
        this, &UploadManager::applyRemoteSessionEnvelope));
    m_webSocketConnections.append(connect(client, &WebSocketClient::remoteSessionRecoveryExpired,
        this, [this](const QString& id, quint64) {
            terminateRemoteSessionUpload(id, QStringLiteral("Remote session recovery expired"));
            beginIncomingFileReaderTeardown({id});
        }));
    m_webSocketConnections.append(connect(client, &WebSocketClient::sessionsInvalidated,
        this, [this](const QString& reason, const QString&, quint64) {
            QSet<QString> ids;
            if (!m_outgoingRemoteSessionId.isEmpty()) ids.insert(m_outgoingRemoteSessionId);
            for (const auto* transfer : std::as_const(m_parallelOutgoingByUpload))
                if (transfer) ids.insert(transfer->remoteSessionId);
            for (const QString& id : ids) terminateRemoteSessionUpload(id, reason);
            beginTerminalIncomingCleanup(reason);
            emit terminalIncomingCleanupRequired(reason);
        }));
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
                for (IncomingUploadSession* incoming : m_incomingUploads) {
                    if (incoming && incoming->stallTimer) {
                        incoming->stallTimer->setInterval(idleTimeout);
                    }
                }
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
    connect(this, &UploadManager::uploadProtocolResponseReady,
            client, &WebSocketClient::sendUploadProtocolResponse,
            Qt::UniqueConnection);

    const QJsonObject policy = client->serverPolicy();
    if (!policy.isEmpty()) {
        const int idleTimeout = policy.value(QStringLiteral("uploadIdleTimeoutMs")).toInt();
        const int targetAckTimeout =
            policy.value(QStringLiteral("uploadTargetAckTimeoutMs")).toInt();
        if (idleTimeout > 0) {
            m_outgoingStallTimer->setInterval(idleTimeout);
            for (IncomingUploadSession* incoming : m_incomingUploads) {
                if (incoming && incoming->stallTimer) {
                    incoming->stallTimer->setInterval(idleTimeout);
                }
            }
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
QString UploadManager::residencyOwnerId(const QString& sessionId, quint64 generation,
                                        const QString& sha256)
{
    return QStringLiteral("remote:%1:%2:%3").arg(sessionId).arg(generation).arg(sha256);
}

bool UploadManager::remoteMediaReady(const QString& target, const QString& sha256) const
{
    if (!m_ws) return false;
    const auto binding = m_ws->sceneRunCoordinator()->sessionForPeer(target);
    if (m_cancelledRemoteAssets.value(binding.remoteSessionId).contains(sha256)) return false;
    const auto report = m_remoteResidency.value(binding.remoteSessionId);
    if ((binding.phase != QLatin1String("Active") && binding.phase != QLatin1String("Grace")) || report.value(QStringLiteral("generation")).toInteger()
        != binding.generation) return false;
    for (const auto& value : report.value(QStringLiteral("assets")).toArray()) {
        const auto asset = value.toObject();
        const auto committed = m_committedAssetsByTarget.value(target).value(
            asset.value(QStringLiteral("assetId")).toString());
        const QString reportedUpload = asset.value(QStringLiteral("uploadId")).toString();
        if (!reportedUpload.isEmpty() && !committed.uploadId.isEmpty()
            && reportedUpload != committed.uploadId) continue;
        if (asset.value(QStringLiteral("sha256")).toString() == sha256
            && asset.value(QStringLiteral("state")).toString() == QLatin1String("ready")) return true;
    }
    return false;
}

UploadManager::SourceUploadStatus UploadManager::sourceUploadStatus(
    const QString& targetEndpointId, const QString& fileId) const
{
    SourceUploadStatus result;
    if (targetEndpointId.isEmpty() || fileId.isEmpty()) return result;
    const auto ram = m_outgoingRamByTarget.constFind(targetEndpointId);
    if (ram != m_outgoingRamByTarget.cend()) {
        for (const auto& asset : ram->assets) {
            if (!asset.localFileIds.contains(fileId)) continue;
            if (ram->failedAssets.contains(asset.assetId)) return result;
            result.state = ram->settledAssets.contains(asset.assetId)
                ? SourceUploadStatus::Uploaded : SourceUploadStatus::Uploading;
            result.progress = 100;
            return result;
        }
    }
    if (m_fileManager && m_fileManager->isFileUploadedToClient(fileId, targetEndpointId)) {
        result.state = SourceUploadStatus::Uploaded;
        result.progress = 100;
        return result;
    }
    if (m_verifyingFileIdsByTarget.value(targetEndpointId).contains(fileId)) {
        result.state = SourceUploadStatus::Uploading;
        return result;
    }
    const auto waiting = m_waitingVerifiedUploads.constFind(targetEndpointId);
    if (waiting != m_waitingVerifiedUploads.cend()) {
        for (const auto& file : waiting->first) {
            if (file.fileId == fileId) {
                result.state = SourceUploadStatus::Uploading;
                return result;
            }
        }
    }
    if (const auto* transfer = parallelForTarget(targetEndpointId)) {
        for (const auto& asset : transfer->assets) {
            if (!asset.localFileIds.contains(fileId)) continue;
            const int remote = transfer->remoteFilePercents.value(fileId);
            result.state = SourceUploadStatus::Uploading;
            result.progress = std::clamp(remote,
                                         0, 99);
            return result;
        }
    }
    if (m_uploadTargetClientId == targetEndpointId && m_outgoingState != OutgoingState::Idle) {
        for (const auto& asset : m_outgoingAssets) {
            if (!asset.localFileIds.contains(fileId)) continue;
            const int remote = m_remoteFilePercents.value(fileId);
            result.state = SourceUploadStatus::Uploading;
            result.progress = std::clamp(m_effectiveFilePercents.value(fileId), 0, 99);
            return result;
        }
    }
    return result;
}

void UploadManager::publishResidency(const QString& sessionId)
{
    if (!m_ws || !m_ws->isConnected()) return;
    const auto binding = m_ws->sceneRunCoordinator()->sessionById(sessionId);
    if ((!binding.active || !m_ws->canIssueSessionCommands(binding.remoteSessionId)) || binding.targetEndpointId != m_ws->endpointId()) return;
    QJsonArray assets;
    auto& manager = MediaResidencyManager::instance();
    for (auto it = m_residentIncoming.cbegin(); it != m_residentIncoming.cend(); ++it) {
        if (it->sessionId != sessionId || it->generation != binding.generation) continue;
        assets.append(QJsonObject{{QStringLiteral("assetId"), it->assetId},
            {QStringLiteral("sha256"), it->sha256},
            {QStringLiteral("uploadId"), it->uploadId},
            {QStringLiteral("state"), manager.state(it.key())},
            {QStringLiteral("progress"), manager.progress(it.key())},
            {QStringLiteral("error"), manager.errorString(it.key()).left(1024)}});
    }
    const QJsonArray previous = m_publishedResidency.value(sessionId);
    if (previous == assets && m_publishedResidency.contains(sessionId)) return;
    bool critical = false;
    QJsonArray changed;
    for (const auto& asset : assets) {
        if (!previous.contains(asset)) {
            changed.append(asset);
            const auto row = asset.toObject();
            critical |= !row.value(QStringLiteral("error")).toString().isEmpty()
                || row.value(QStringLiteral("state")).toString() == QLatin1String("failed");
        }
    }
    const qint64 now = MouffetteClock::nowMs();
    const qint64 remaining = 250 - (now - m_lastResidencyPublication.value(sessionId, -250));
    if (!critical && remaining > 0) {
        if (!m_pendingResidencyPublications.contains(sessionId)) {
            m_pendingResidencyPublications.insert(sessionId);
            QTimer::singleShot(static_cast<int>(remaining), this, [this, sessionId] {
                m_pendingResidencyPublications.remove(sessionId);
                publishResidency(sessionId);
            });
        }
        return;
    }
    const bool delta = m_publishedResidency.contains(sessionId) && assets.size() == previous.size();
    if (m_ws->sendMediaResidency(sessionId, binding.generation,
                            ++m_residencySequences[sessionId], delta ? changed : assets, delta)) {
        m_publishedResidency.insert(sessionId, assets);
        m_lastResidencyPublication.insert(sessionId, now);
    }
}

void UploadManager::receiveResidency(const QJsonObject& envelope)
{
    const QString sessionId = envelope.value(QStringLiteral("remoteSessionId")).toString();
    const auto previous = m_remoteResidency.value(sessionId);
    const qint64 generation = envelope.value(QStringLiteral("generation")).toInteger();
    const qint64 sequence = envelope.value(QStringLiteral("sequence")).toInteger();
    if (sequence < 1 || (previous.value(QStringLiteral("generation")).toInteger() == generation
        && previous.value(QStringLiteral("sequence")).toInteger() >= sequence)) return;
    const QString target = envelope.value(QStringLiteral("targetEndpointId")).toString();
    MediaResidencyManager::instance().clearRemoteStates(target);
    QJsonObject merged = envelope;
    if (envelope.value(QStringLiteral("delta")).toBool()
        && previous.value(QStringLiteral("generation")).toInteger() == generation) {
        QJsonArray assets = previous.value(QStringLiteral("assets")).toArray();
        for (const auto& changed : envelope.value(QStringLiteral("assets")).toArray()) {
            const QString assetId = changed.toObject().value(QStringLiteral("assetId")).toString();
            for (qsizetype i = assets.size(); i > 0; --i)
                if (assets.at(i - 1).toObject().value(QStringLiteral("assetId")).toString() == assetId) assets.removeAt(i - 1);
            assets.append(changed);
        }
        merged.insert(QStringLiteral("assets"), assets);
    }
    m_remoteResidency.insert(sessionId, merged);
    for (const auto& value : merged.value(QStringLiteral("assets")).toArray()) {
        const auto asset = value.toObject();
        if (m_cancelledRemoteAssets.value(sessionId).contains(
                asset.value(QStringLiteral("sha256")).toString())) continue;
        MediaResidencyManager::instance().setRemoteState(
            asset.value(QStringLiteral("sha256")).toString(), target,
            asset.value(QStringLiteral("state")).toString(),
            asset.value(QStringLiteral("progress")).toDouble(),
            asset.value(QStringLiteral("error")).toString());
    }
    updateRamLoading(sessionId);
    emit uiStateChanged();
}

void UploadManager::beginRamLoading(const QString& target, const QString& session,
                                    quint64 generation, const QString& uploadId,
                                    const QVector<OutgoingAsset>& assets)
{
    OutgoingRamBatch batch;
    batch.targetEndpointId = target;
    batch.remoteSessionId = session;
    batch.generation = generation;
    batch.uploadId = uploadId;
    batch.assets = assets;
    m_outgoingRamByTarget.insert(target, batch);
    rememberCommittedAssets(target, session, generation, uploadId, assets);
}

void UploadManager::updateRamLoading(const QString& session)
{
    const auto report = m_remoteResidency.value(session);
    const auto targets = m_outgoingRamByTarget.keys();
    for (const QString& target : targets) {
        auto batch = m_outgoingRamByTarget.find(target);
        if (batch == m_outgoingRamByTarget.end() || batch->remoteSessionId != session
            || report.value(QStringLiteral("generation")).toInteger() != batch->generation) continue;
        for (const auto& value : report.value(QStringLiteral("assets")).toArray()) {
            const auto row = value.toObject();
            const QString rowUpload = row.value(QStringLiteral("uploadId")).toString();
            if (!rowUpload.isEmpty() && rowUpload != batch->uploadId) continue;
            const QString assetId = row.value(QStringLiteral("assetId")).toString();
            const auto asset = std::find_if(batch->assets.cbegin(), batch->assets.cend(),
                [&assetId](const OutgoingAsset& candidate) { return candidate.assetId == assetId; });
            if (asset == batch->assets.cend() || batch->settledAssets.contains(assetId)
                || row.value(QStringLiteral("sha256")).toString() != asset->sha256) continue;
            const QString state = row.value(QStringLiteral("state")).toString();
            const bool ready = state == QLatin1String("ready");
            const bool failed = state == QLatin1String("error") || state == QLatin1String("failed")
                || state == QLatin1String("capacity_insufficient");
            if (!ready && !failed) continue;
            batch->settledAssets.insert(assetId);
            if (failed) batch->failedAssets.insert(assetId);
            for (const auto& localFileId : asset->localFileIds) {
                if (ready) m_fileManager->markFileUploadedToClient(localFileId, target);
                else m_fileManager->unmarkFileUploadedToClient(localFileId, target);
            }
        }
        if (batch->settledAssets.size() != batch->assets.size()) continue;
        const QString uploadId = batch->uploadId;
        const int failed = batch->failedAssets.size();
        m_outgoingRamByTarget.erase(batch);
        if (failed) emit uploadRamFailed(uploadId, failed);
        else emit uploadFinished(uploadId);
    }
}

void UploadManager::sendOrQueueUploadAbort(const QString& session, quint64 generation,
                                          const QString& uploadId)
{
    if (m_ws && !m_ws->sendUploadAbort(session, generation, uploadId,
                                      QStringLiteral("User cancelled")))
        m_pendingUploadAborts.insert(uploadId, session);
}

void UploadManager::cancelRamLoading(const QString& target)
{
    const auto batch = m_outgoingRamByTarget.take(target);
    if (batch.uploadId.isEmpty()) return;
    sendOrQueueUploadAbort(batch.remoteSessionId, batch.generation, batch.uploadId);
    for (const auto& asset : batch.assets) {
        m_cancelledRemoteAssets[batch.remoteSessionId].insert(asset.sha256);
        CommittedRemoteAsset committed;
        if (!asset.localFileIds.isEmpty()
            && findCommittedAsset(target, asset.localFileIds.first(), &committed))
            forgetCommittedAsset(committed);
        for (const auto& fileId : asset.localFileIds)
            m_fileManager->unmarkFileUploadedToClient(fileId, target);
    }
    if (m_ws) m_ws->cancelUploadId(batch.uploadId);
    emit uploadCancelled(batch.uploadId);
    emit uiStateChanged();
}

void UploadManager::releaseResidency(const QString& sessionId, const QString& sha256)
{
    for (const auto& owner : m_residentIncoming.keys()) {
        const auto entry = m_residentIncoming.value(owner);
        if (entry.sessionId != sessionId || (!sha256.isEmpty() && entry.sha256 != sha256)) continue;
        m_retiringResidencyPaths[sessionId].insert(entry.path);
        m_residentIncoming.remove(owner);
        MediaResidencyManager::instance().release(owner);
    }
    if (sha256.isEmpty()) m_residencySequences.remove(sessionId);
}

void UploadManager::beginIncomingFileReaderTeardown(const QSet<QString>& sessionIds)
{
    for (IncomingUploadSession* incoming : std::as_const(m_incomingUploads)) {
        if (incoming && (sessionIds.isEmpty() || sessionIds.contains(incoming->remoteSessionId)))
            suspendIncomingForResume(*incoming);
    }
    QSet<QString> sessions;
    for (const auto& asset : std::as_const(m_residentIncoming)) {
        if (sessionIds.isEmpty() || sessionIds.contains(asset.sessionId)) sessions.insert(asset.sessionId);
    }
    for (const auto& session : sessions) releaseResidency(session);
}

bool UploadManager::incomingFileReadersSettled(const QSet<QString>& sessionIds) const
{
    for (auto it = m_validationReadersBySession.cbegin(); it != m_validationReadersBySession.cend(); ++it)
        if (it.value() > 0 && (sessionIds.isEmpty() || sessionIds.contains(it.key()))) return false;
    const auto& manager = MediaResidencyManager::instance();
    for (auto it = m_retiringResidencyPaths.cbegin(); it != m_retiringResidencyPaths.cend(); ++it) {
        if (!sessionIds.isEmpty() && !sessionIds.contains(it.key())) continue;
        for (const auto& path : it.value()) if (manager.hasBackgroundWorkForPath(path)) return false;
    }
    for (const auto& asset : m_residentIncoming) {
        if ((sessionIds.isEmpty() || sessionIds.contains(asset.sessionId))
            && manager.hasBackgroundWorkForPath(asset.path)) return false;
    }
    return true;
}

void UploadManager::settleIncomingFileReaders()
{
    auto& manager = MediaResidencyManager::instance();
    for (auto it = m_retiringResidencyPaths.begin(); it != m_retiringResidencyPaths.end();) {
        for (auto path = it->begin(); path != it->end();) {
            if (!manager.hasBackgroundWorkForPath(*path)) path = it->erase(path); else ++path;
        }
        if (it->isEmpty()) it = m_retiringResidencyPaths.erase(it); else ++it;
    }
    const auto discards = m_deferredIncomingDiscards;
    for (auto it = discards.cbegin(); it != discards.cend(); ++it) {
        if (m_validationReadersByUpload.value(it.key()) > 0) continue;
        m_deferredIncomingDiscards.remove(it.key());
        if (discardIncomingUpload(it.key(), it.value())) {
            const auto abort = m_deferredIncomingAborts.take(it.key());
            if (!abort.isEmpty()) handleIncomingMessage(abort);
        }
    }
    const auto removals = m_deferredIncomingRemovals;
    for (auto it = removals.cbegin(); it != removals.cend(); ++it) {
        const QString session = it->value(QStringLiteral("remoteSessionId")).toString();
        if (!incomingFileReadersSettled({session})) continue;
        m_deferredIncomingRemovals.remove(it.key());
        handleIncomingAssetRemoval(it.value());
    }
    const auto aborts = m_deferredIncomingAborts;
    for (auto it = aborts.cbegin(); it != aborts.cend(); ++it) {
        if (!m_incomingUploadCompletionTombstones.contains(it.key())) continue;
        const QString session = it->value(QStringLiteral("remoteSessionId")).toString();
        if (!incomingFileReadersSettled({session})) continue;
        m_deferredIncomingAborts.remove(it.key());
        handleIncomingMessage(it.value());
    }
    const auto uploads = m_incomingUploads.values();
    for (auto* incoming : uploads) {
        if (!incoming || incoming->deferredResume.isEmpty()
            || m_validationReadersByUpload.value(incoming->uploadId) > 0) continue;
        const auto resume = incoming->deferredResume;
        incoming->deferredResume = {};
        handleIncomingMessage(resume);
    }
    emit incomingFileReadersChanged();
}

void UploadManager::setTargetClientId(const QString& id) {
    if (m_targetClientId == id) return;
    m_targetClientId = id;
    m_uploadActive = hasActiveUpload();
    emit uiStateChanged();
}

void UploadManager::forceResetForClient(const QString& clientId) {
    for (const auto& target : m_outgoingRamByTarget.keys()) {
        if (!clientId.isEmpty() && target != clientId) continue;
        emit uploadCancelled(m_outgoingRamByTarget.take(target).uploadId);
    }
    for (const QString& target : m_waitingVerifiedUploads.keys()) {
        if (!clientId.isEmpty() && clientId != target) continue;
        emit uploadCancelled(m_waitingVerifiedUploads.take(target).second);
    }
    for (const QString& target : m_pendingUploadVerification.keys()) {
        if (!clientId.isEmpty() && clientId != target) continue;
        m_pendingUploadVerification.remove(target);
        m_verifyingFileIdsByTarget.remove(target);
        if (const auto cancelled = m_uploadVerificationCancellation.take(target)) cancelled->store(true);
        emit uploadCancelled(m_verifyingUploadIds.take(target));
    }
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
        const bool matchesSelectedIdleTarget = m_uploadTargetClientId.isEmpty()
            && m_targetClientId == clientId
            && m_outgoingState == OutgoingState::Idle;
        if (!matchesPrimary && !matchesSelectedIdleTarget) {
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
        || parallelForTarget(m_targetClientId)
        || m_pendingUploadVerification.contains(m_targetClientId)
        || m_waitingVerifiedUploads.contains(m_targetClientId)
        || m_outgoingRamByTarget.contains(m_targetClientId)) {
        qInfo() << "UploadManager: Transfer already active; duplicate action ignored";
        return false;
    }
    
    if (hasActiveUpload()) {
        // If active state but we are provided with additional files, start a new upload for them
        if (!files.isEmpty()) {
            startUpload(files);
            return !currentUploadId().isEmpty() || m_pendingUploadVerification.contains(m_targetClientId);
        }
        return false;
    }
    if (files.isEmpty()) {
        qInfo() << "UploadManager: No files provided";
        return false;
    }
    startUpload(files);
    return !currentUploadId().isEmpty() || m_pendingUploadVerification.contains(m_targetClientId);
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
    if (((!binding.active || !m_ws->canIssueSessionCommands(binding.remoteSessionId)) && !resumableGrace)
        || binding.ownerEndpointId != m_ws->endpointId()) {
        emit assetRemovalFailed(targetEndpointId, binding.remoteSessionId,
                                {localFileId},
                                QStringLiteral("The remote session is not active"));
        return false;
    }

    // Verification is part of the transfer lifecycle, even though no remote
    // staging asset exists yet. Cancel only this endpoint's matching batch.
    if (m_verifyingFileIdsByTarget.value(targetEndpointId).contains(localFileId)) {
        m_pendingUploadVerification.remove(targetEndpointId);
        m_verifyingFileIdsByTarget.remove(targetEndpointId);
        if (const auto cancelled = m_uploadVerificationCancellation.take(targetEndpointId))
            cancelled->store(true);
        emit uploadCancelled(m_verifyingUploadIds.take(targetEndpointId));
        emit uiStateChanged();
        return true;
    }

    const auto ramBatch = m_outgoingRamByTarget.constFind(targetEndpointId);
    if (ramBatch != m_outgoingRamByTarget.cend()
        && std::any_of(ramBatch->assets.cbegin(), ramBatch->assets.cend(),
            [&localFileId](const OutgoingAsset& asset) { return asset.localFileIds.contains(localFileId); })) {
        cancelRamLoading(targetEndpointId);
        return true;
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
            || !m_fileManager->getProjectIdsForFile(aliasFileId).isEmpty()) {
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
    if (binding.active && m_ws->canIssueSessionCommands(binding.remoteSessionId)) {
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

bool UploadManager::requestUnload(
    const QString& targetEndpointId,
    const QSet<QString>& knownLocalFileIds) {
    if (!m_ws || !m_fileManager || targetEndpointId.isEmpty()) {
        return false;
    }

    if (m_outgoingRamByTarget.contains(targetEndpointId)
        || parallelForTarget(targetEndpointId)
        || (m_uploadTargetClientId == targetEndpointId
            && m_outgoingState != OutgoingState::Idle)) {
        qInfo() << "UploadManager: Cannot unload while a transfer is active for"
                << targetEndpointId;
        return false;
    }

    for (auto it = m_pendingAssetRemovals.cbegin();
         it != m_pendingAssetRemovals.cend(); ++it) {
        if (it->asset.targetEndpointId == targetEndpointId) {
            qInfo() << "UploadManager: Unload already pending for"
                    << targetEndpointId;
            return false;
        }
    }

    // Use both projections. WorkspaceManager knows what the canvas believes is
    // remote, while this inventory contains the immutable tuples accepted by
    // protocol v6. Their union makes unload self-healing if one UI update was
    // delayed, without ever inventing a server-side asset identity.
    QSet<QString> localFileIds = knownLocalFileIds;
    const auto inventory = m_committedAssetsByTarget.constFind(targetEndpointId);
    if (inventory != m_committedAssetsByTarget.cend()) {
        for (auto asset = inventory->cbegin(); asset != inventory->cend(); ++asset) {
            for (const QString& localFileId : asset->localFileIds) {
                if (!localFileId.isEmpty()) localFileIds.insert(localFileId);
            }
        }
    }
    if (localFileIds.isEmpty()) {
        qWarning() << "UploadManager: Cannot unload without validated remote inventory for"
                   << targetEndpointId;
        return false;
    }

    // requestAssetRemoval() deduplicates aliases that resolve to the same
    // immutable remote asset. The final alias sends the authenticated removal
    // command; distinct assets may safely wait for their ACKs in parallel.
    const QList<QString> orderedFileIds = localFileIds.values();
    for (const QString& localFileId : orderedFileIds) {
        if (!requestAssetRemoval(targetEndpointId, localFileId,
                                 QStringLiteral("user_unload"))) {
            return false;
        }
    }
    return true;
}

bool UploadManager::sendPendingAssetRemoval(PendingAssetRemoval& pending) {
    if (!m_ws || pending.removalId.isEmpty()) return false;
    RemoteSessionCoordinator* sessions = m_ws->remoteSessionCoordinator();
    const RemoteSessionCoordinator::Binding binding = sessions
        ? sessions->byId(pending.asset.remoteSessionId)
        : RemoteSessionCoordinator::Binding();
    if ((!binding.active || !m_ws->canIssueSessionCommands(binding.remoteSessionId)) || binding.ownerEndpointId != m_ws->endpointId()
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
        // Remove its previous association before recording the server's exact
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
    auto report = m_remoteResidency.find(asset.remoteSessionId);
    if (report != m_remoteResidency.end()) {
        QJsonArray retained;
        auto& residency = MediaResidencyManager::instance();
        residency.clearRemoteStates(asset.targetEndpointId);
        for (const auto& value : report->value(QStringLiteral("assets")).toArray()) {
            const auto row = value.toObject();
            if (row.value(QStringLiteral("sha256")).toString() == asset.sha256) continue;
            retained.append(row);
            residency.setRemoteState(row.value(QStringLiteral("sha256")).toString(),
                asset.targetEndpointId, row.value(QStringLiteral("state")).toString(),
                row.value(QStringLiteral("progress")).toDouble(),
                row.value(QStringLiteral("error")).toString());
        }
        report->insert(QStringLiteral("assets"), retained);
    }

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
    if (m_outgoingRamByTarget.contains(m_targetClientId)) {
        cancelRamLoading(m_targetClientId);
        return;
    }
    if (m_waitingVerifiedUploads.contains(m_targetClientId)) {
        const auto request = m_waitingVerifiedUploads.take(m_targetClientId);
        emit uploadCancelled(request.second);
        emit uiStateChanged();
        return;
    }
    if (m_pendingUploadVerification.remove(m_targetClientId)) {
        m_verifyingFileIdsByTarget.remove(m_targetClientId);
        if (const auto cancelled = m_uploadVerificationCancellation.take(m_targetClientId)) cancelled->store(true);
        emit uploadCancelled(m_verifyingUploadIds.take(m_targetClientId));
        emit uiStateChanged();
        return;
    }
    if (ParallelOutgoingTransfer* transfer = parallelForTarget(m_targetClientId)) {
        if (!m_ws || transfer->state == OutgoingState::Cancelling
            || transfer->state == OutgoingState::Idle) return;
        for (const auto& asset : transfer->assets) {
            m_cancelledRemoteAssets[transfer->remoteSessionId].insert(asset.sha256);
            for (const auto& fileId : asset.localFileIds)
                m_fileManager->unmarkFileUploadedToClient(fileId, transfer->targetEndpointId);
        }
        setParallelState(transfer, OutgoingState::Cancelling);
        if (m_uploadScheduler->isSessionTerminal(transfer->remoteSessionId)) { cancelParallel(transfer); return; }
        stopParallel(transfer);
        sendOrQueueUploadAbort(transfer->remoteSessionId, transfer->generation, transfer->uploadId);
        cancelParallel(transfer);
        return;
    }
    if (!m_ws || m_outgoingRemoteSessionId.isEmpty()
        || m_currentUploadId.isEmpty()) return;
    if (!canRequestCancel()) return;

    for (const auto& asset : m_outgoingAssets) {
        m_cancelledRemoteAssets[m_outgoingRemoteSessionId].insert(asset.sha256);
        for (const auto& fileId : asset.localFileIds)
            m_fileManager->unmarkFileUploadedToClient(fileId, m_uploadTargetClientId);
    }

    recordAcceptedAction();
    setOutgoingState(OutgoingState::Cancelling);
    if (m_uploadScheduler->isSessionTerminal(m_outgoingRemoteSessionId)) { finishLocalCancellation(); return; }
    stopOutgoingPump();
    sendOrQueueUploadAbort(m_outgoingRemoteSessionId, m_outgoingGeneration, m_currentUploadId);
    finishLocalCancellation();
}

void UploadManager::startUpload(const QVector<UploadFileInfo>& files) {
    if (!m_ws || files.isEmpty() || m_pendingUploadVerification.contains(m_targetClientId)) return;
    const QString target = m_targetClientId;
    const quint64 token = ++m_uploadVerificationGeneration;
    m_pendingUploadVerification.insert(target, token);
    QSet<QString> sourceIds;
    for (const auto& file : files) sourceIds.insert(file.fileId);
    m_verifyingFileIdsByTarget.insert(target, sourceIds);
    const auto cancelled = std::make_shared<std::atomic_bool>(false);
    m_uploadVerificationCancellation.insert(target, cancelled);
    const QString verifiedUploadId = QUuid::createUuid().toString(QUuid::WithoutBraces);
    m_verifyingUploadIds.insert(target, verifiedUploadId);
    emit uiStateChanged();
    auto* watcher = new QFutureWatcher<QString>(this);
    connect(watcher, &QFutureWatcher<QString>::finished, this, [this, watcher, files, target, token, verifiedUploadId]() {
        const QString error = watcher->result();
        watcher->deleteLater();
        if (m_pendingUploadVerification.value(target) != token) return;
        m_pendingUploadVerification.remove(target);
        m_uploadVerificationCancellation.remove(target);
        m_verifyingUploadIds.remove(target);
        m_verifyingFileIdsByTarget.remove(target);
        if (!error.isEmpty()) {
            emit uploadRejected(verifiedUploadId, error);
            emit uiStateChanged();
            return;
        }
        const QString selected = m_targetClientId;
        m_targetClientId = target;
        startVerifiedUpload(files, verifiedUploadId);
        if (m_currentUploadId != verifiedUploadId
            && !m_parallelOutgoingByUpload.contains(verifiedUploadId)
            && m_waitingVerifiedUploads.value(target).second != verifiedUploadId)
            emit uploadRejected(verifiedUploadId, QStringLiteral("Upload preparation became invalid; try again"));
        m_targetClientId = selected;
        emit uiStateChanged();
    });
    watcher->setFuture(QtConcurrent::run([files, cancelled]() -> QString {
        for (const auto& file : files) {
            if (cancelled->load(std::memory_order_relaxed)) return {};
            if (!MediaFilePolicy::validateLocalFileMetadata(file.path).accepted()
                || sha256ForFile(file.path, cancelled) != file.fileId)
                return QStringLiteral("A source media changed or is invalid; import it again before uploading");
        }
        return {};
    }));
}

void UploadManager::startVerifiedUpload(const QVector<UploadFileInfo>& files, const QString& verifiedUploadId) {
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
    if ((!binding.active || !m_ws->canIssueSessionCommands(binding.remoteSessionId)) || binding.ownerEndpointId != m_ws->endpointId()) {
        m_waitingVerifiedUploads.insert(m_targetClientId, {files, verifiedUploadId});
        emit uiStateChanged();
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
            || !MediaFormatContract::isCanonicalMediaExtension(extension)
            || QFileInfo(name).suffix().compare(extension, Qt::CaseInsensitive) != 0) {
            qWarning() << "UploadManager: refusing unsupported media file"
                       << "or excessive upload size (video uploads must be valid MP4 files)";
            return;
        }

        // Identity and structural validation completed on the worker thread.
        const QString digest = file.fileId;
        if (!isValidFileId(digest) || digest != file.fileId) {
            qWarning() << "UploadManager: source identity changed before upload";
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
    // A previous decode attempt must not finish this new batch with its cached
    // failure. Fresh residency messages also carry the immutable upload ID.
    auto oldReport = m_remoteResidency.find(binding.remoteSessionId);
    if (oldReport != m_remoteResidency.end()) {
        QJsonArray retained;
        for (const auto& row : oldReport->value(QStringLiteral("assets")).toArray())
            if (!assetsByDigest.contains(row.toObject().value(QStringLiteral("assetId")).toString()))
                retained.append(row);
        oldReport->insert(QStringLiteral("assets"), retained);
    }
    for (auto asset = assetsByDigest.cbegin(); asset != assetsByDigest.cend(); ++asset)
        m_cancelledRemoteAssets[binding.remoteSessionId].remove(asset->sha256);
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
        transfer->uploadId = verifiedUploadId;
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
    m_currentUploadId = verifiedUploadId;
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
    m_outgoingStartAccepted = false;
    m_outgoingWindowBytes = 64 * 1024;
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
            if (current && current->state == expected) suspendParallel(current);
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
    NetworkDiagnostics::record(QStringLiteral("upload_transition"), {
        {"uploadId", transfer->uploadId}, {"remoteSessionId", transfer->remoteSessionId},
        {"before", QString::number(static_cast<int>(transfer->state))},
        {"after", QString::number(static_cast<int>(state))},
        {"sentBytes", static_cast<double>(transfer->sentBytes)},
        {"confirmedBytes", static_cast<double>(transfer->remoteAcknowledgedBytes)}});
    transfer->state = state;
    transfer->stateAge.restart();
}

void UploadManager::startParallelScheduled(ParallelOutgoingTransfer* transfer) {
    if (!transfer || !m_ws || transfer->state != OutgoingState::Queued) return;
    RemoteSessionCoordinator* sessions = m_ws->remoteSessionCoordinator();
    const RemoteSessionCoordinator::Binding binding = sessions
        ? sessions->byId(transfer->remoteSessionId)
        : RemoteSessionCoordinator::Binding();
    if ((!binding.active || !m_ws->canIssueSessionCommands(binding.remoteSessionId)) || binding.ownerEndpointId != m_ws->endpointId()
        || binding.generation != transfer->generation) {
        suspendParallel(transfer);
        return;
    }
    if (!m_ws->beginUploadSession(true)) {
        suspendParallel(transfer);
        return;
    }
    transfer->transportRegistered = true;
    setParallelState(transfer, OutgoingState::AwaitingTargetReady);
    if (!m_ws->sendUploadStart(transfer->remoteSessionId, transfer->generation,
                               transfer->uploadId, transfer->manifest)) {
        suspendParallel(transfer);
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
    if ((!binding.active || !m_ws->canIssueSessionCommands(binding.remoteSessionId)) || binding.ownerEndpointId != m_ws->endpointId()) return false;

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
    const bool sent = (wasAwaitingValidation || !transfer->startAccepted)
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
    const QString id = transfer->uploadId;
    QTimer::singleShot(500, this, [this, id] { if (auto* current = parallelForUpload(id)) resumeParallel(current); });
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
        transfer->remotePercent, 0,
        transfer->remotePercent > 0 ? 100 : 99);
    emit uploadProgress(percent, transfer->remoteFilesCompleted,
                        transfer->totalFiles);
}

bool UploadManager::applyParallelOffsets(ParallelOutgoingTransfer* transfer,
                                         const QJsonArray& assets,
                                         bool resetSendCursor,
                                         QString* errorMessage) {
    if (!transfer || (resetSendCursor && assets.size() != transfer->assets.size())) {
        if (errorMessage) *errorMessage = QStringLiteral("Incomplete upload inventory");
        return false;
    }
    QHash<QString, qint64> offsets = resetSendCursor ? QHash<QString, qint64>() : transfer->durableOffsets;
    QSet<QString> seen;
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
        if (assetIt == transfer->assets.cend() || seen.contains(assetId)
            || !parseNonNegativeOffset(state.value(QStringLiteral("offset")), offset)
            || !parseManifestSize(state.value(QStringLiteral("size")), size)
            || size != assetIt->size || offset > size
            || state.value(QStringLiteral("sha256")).toString() != assetIt->sha256
            || offset < transfer->durableOffsets.value(assetId, 0)) {
            if (errorMessage) *errorMessage = QStringLiteral("Mismatched upload inventory");
            return false;
        }
        seen.insert(assetId);
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
                emit fileUploadProgress(localFileId, qMin(percent, 99));
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
    if (m_ws && !m_ws->canIssueSessionCommands(transfer->remoteSessionId)) {
        suspendParallel(transfer);
        return;
    }
    if (!m_ws || !m_ws->isUploadSessionTransportAvailable()) {
        suspendParallel(transfer);
        return;
    }

    constexpr qint64 chunkBytes = 32 * 1024;
    constexpr qint64 maximumChunkWireBytes = ((chunkBytes + 2) / 3) * 4 + 4096;
    int chunksQueued = 0;
    while (chunksQueued < kMaxChunksPerPump
           && transfer->state == OutgoingState::Streaming
           && !transfer->payloadCompleteSent) {
        const qint64 queuedBytes = m_ws->uploadTransportBytesToWrite();
        if (queuedBytes < 0) {
            suspendParallel(transfer);
            return;
        }
        if (queuedBytes > kMaxQueuedUploadBytes - maximumChunkWireBytes
            || transfer->sentBytes - transfer->remoteAcknowledgedBytes
                >= transfer->windowBytes) return;

        if (transfer->fileIndex >= transfer->assets.size()) {
            if (transfer->sentBytes != transfer->totalBytes) {
                failParallel(transfer, QStringLiteral(
                    "Source files were not read completely"));
                return;
            }
            const bool allBytesDurable =
                transfer->remoteAcknowledgedBytes == transfer->totalBytes
                && std::all_of(
                    transfer->assets.cbegin(), transfer->assets.cend(),
                    [transfer](const OutgoingAsset& asset) {
                        return transfer->durableOffsets.value(asset.assetId, -1)
                            == asset.size;
                    });
            if (!allBytesDurable) {
                // Completion is a commit request, not a send-queue marker.
                // Wait until the target has fsynced every byte and the server
                // has echoed that durable inventory back to this sender.
                return;
            }
            if (!m_ws->sendUploadComplete(
                    transfer->remoteSessionId, transfer->generation,
                    transfer->uploadId, parallelAssetStates(transfer, true))) {
                suspendParallel(transfer);
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
        const qint64 requested = std::min({chunkBytes, remaining, transfer->windowBytes - (transfer->sentBytes - transfer->remoteAcknowledgedBytes)});
        if (requested <= 0) {
            failParallel(transfer, QStringLiteral("Invalid source asset state"));
            return;
        }
        const QByteArray chunk = transfer->fileHandle.read(requested);
        if (chunk.size() != requested || transfer->fileHandle.error() != QFileDevice::NoError) {
            failParallel(transfer, QStringLiteral("Source asset changed during upload"));
            return;
        }
        if (!m_ws->sendUploadChunk(transfer->remoteSessionId, transfer->generation,
                transfer->uploadId, asset.assetId, transfer->sentForFile, asset.sha256, chunk)) {
            suspendParallel(transfer);
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
                emit fileUploadProgress(localFileId, transfer->remoteFilePercents.value(localFileId));
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
            if (selected) emit fileUploadProgress(localFileId, 100);
        }
    }
    const QString session = transfer->remoteSessionId;
    beginRamLoading(transfer->targetEndpointId, session, transfer->generation,
                    uploadId, transfer->assets);
    if (selected) emit uploadProgress(100, transfer->totalFiles, transfer->totalFiles);
    removeParallel(transfer, true, true, true);
    updateRamLoading(session);
    emit uiStateChanged();
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
    transfer->windowBytes = std::clamp<qint64>(message.value(QStringLiteral("windowBytes")).toInteger(transfer->windowBytes), 1, 256 * 1024);
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
        transfer->startAccepted = true;
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
        if (!message.value(QStringLiteral("assets")).isArray()
            || message.value(QStringLiteral("assets")).toArray().size() != transfer->assets.size()) return;
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
        if (message.value(QStringLiteral("temporary")).toBool()
            || message.value(QStringLiteral("errorClass")).toString() == QLatin1String("temporary")) {
            suspendParallel(transfer);
            QTimer::singleShot(500, this, [this, id = transfer->uploadId] { if (auto* current = parallelForUpload(id)) resumeParallel(current); });
            return;
        }
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
    if ((!binding.active || !m_ws->canIssueSessionCommands(binding.remoteSessionId)) || binding.ownerEndpointId != m_ws->endpointId()
        || binding.generation != m_outgoingGeneration) {
        suspendOutgoingForResume(QStringLiteral("Remote session is not active"));
        return;
    }
    if (!m_ws->beginUploadSession(true)) {
        suspendOutgoingForResume(QStringLiteral("Upload transport is unavailable before transfer start"));
        return;
    }
    m_outgoingTransportRegistered = true;
    setOutgoingState(OutgoingState::AwaitingTargetReady);
    if (!m_ws->sendUploadStart(m_outgoingRemoteSessionId, m_outgoingGeneration,
                               m_currentUploadId, m_outgoingManifest)) {
        suspendOutgoingForResume(QStringLiteral("Upload transport failed before transfer started"));
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
    if ((!binding.active || !m_ws->canIssueSessionCommands(binding.remoteSessionId)) || binding.ownerEndpointId != m_ws->endpointId()) return false;

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
    const bool sent = (wasAwaitingValidation || !m_outgoingStartAccepted)
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
    NetworkDiagnostics::record(QStringLiteral("upload_paused"), {{"uploadId", m_currentUploadId},
        {"remoteSessionId", m_outgoingRemoteSessionId}, {"reason", reason.left(128)}});
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
    QTimer::singleShot(500, this, [this] { resumeOutgoingUpload(); });
    emit uiStateChanged();
}

void UploadManager::applyRemoteSessionEnvelope(const QJsonObject& envelope) {
    const QString residencySession = envelope.value(QStringLiteral("remoteSessionId")).toString();
    const QString residencyPhase = envelope.value(QStringLiteral("phase")).toString();
    if (envelope.value(QStringLiteral("type")).toString() == QLatin1String("remote_session_resumed"))
        m_publishedResidency.remove(residencySession);
    if (residencyPhase == QLatin1String("Closed") || residencyPhase == QLatin1String("CleanupPending")
        || residencyPhase == QLatin1String("Terminating")) {
        m_remoteResidency.remove(residencySession);
        MediaResidencyManager::instance().clearRemoteStates(
            envelope.value(QStringLiteral("targetEndpointId")).toString());
    } else {
        QTimer::singleShot(0, this, [this, residencySession]() { publishResidency(residencySession); });
    }

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

    const bool active = phase == QLatin1String("Active")
        && (!m_ws || m_ws->canIssueSessionCommands(remoteSessionId));
    if (!active) {
        for (auto* incoming : std::as_const(m_incomingUploads)) {
            if (incoming && incoming->remoteSessionId == remoteSessionId && !incoming->suspendedForResume)
                suspendIncomingForResume(*incoming);
        }
    }
    if (active && generation > 0) {
        for (auto batch = m_outgoingRamByTarget.begin(); batch != m_outgoingRamByTarget.end(); ++batch)
            if (batch->remoteSessionId == remoteSessionId) batch->generation = generation;
        for (const auto& uploadId : m_pendingUploadAborts.keys()) {
            if (m_pendingUploadAborts.value(uploadId) == remoteSessionId && m_ws
                && m_ws->sendUploadAbort(remoteSessionId, generation, uploadId,
                                         QStringLiteral("User cancelled")))
                m_pendingUploadAborts.remove(uploadId);
        }
        if (m_remoteResidency.contains(remoteSessionId))
            m_remoteResidency[remoteSessionId].insert(QStringLiteral("generation"), static_cast<double>(generation));
        const auto owners = m_residentIncoming.keys();
        for (const auto& oldOwner : owners) {
            auto record = m_residentIncoming.value(oldOwner);
            if (record.sessionId != remoteSessionId || record.generation >= generation) continue;
            record.generation = generation;
            const QString owner = residencyOwnerId(remoteSessionId, generation, record.sha256);
            if (!m_residentIncoming.contains(owner)) {
                m_residentIncoming.insert(owner, record);
                MediaResidencyManager::instance().acquire(owner, record.path, record.sha256);
            }
            // Acquire first so a Live scene never sees its last residency
            // reference disappear while the authenticated scope advances.
            m_residentIncoming.remove(oldOwner);
            MediaResidencyManager::instance().release(oldOwner);
        }

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
                        // In-flight uploads own their rebind after all old
                        // writers have settled and RESUME supplies durable offsets.
                        const bool hasIncoming = std::any_of(m_incomingUploads.cbegin(), m_incomingUploads.cend(),
                            [&remoteSessionId](const IncomingUploadSession* incoming) {
                                return incoming && incoming->remoteSessionId == remoteSessionId;
                            });
                        if (hasIncoming || !incomingFileReadersSettled({remoteSessionId})) continue;
                        QString ignored;
                        if (m_remoteCacheStore->rebindSessionGeneration(
                                scope, generation, &ignored)) {
                            m_fileManager->rebindReceivedFileScope(
                                scope, {scope.senderEndpointId,
                                        scope.remoteSessionId, generation});
                        }
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
    if (active && m_ws && envelope.value(QStringLiteral("ownerEndpointId")).toString() == m_ws->endpointId()) {
        const QString target = envelope.value(QStringLiteral("targetEndpointId")).toString();
        if (m_waitingVerifiedUploads.contains(target)) {
            const auto waiting = m_waitingVerifiedUploads.take(target);
            QScopedValueRollback<QString> selectedTarget(m_targetClientId, target);
            startVerifiedUpload(waiting.first, waiting.second);
        }
        if (auto* pending = parallelForTarget(target); pending
            && pending->state == OutgoingState::Suspended
            && pending->remoteSessionId != remoteSessionId
            && m_uploadScheduler->isSessionTerminal(pending->remoteSessionId)) {
            const QString previousUploadId = pending->uploadId;
            m_parallelOutgoingByUpload.remove(pending->uploadId);
            m_parallelUploadBySession.remove(pending->remoteSessionId);
            for (auto* timer : {pending->pumpTimer, pending->stallTimer, pending->startAckTimer, pending->ackTimer, pending->cancelTimer}) {
                if (timer) { timer->stop(); timer->deleteLater(); }
            }
            pending->remoteSessionId = remoteSessionId;
            pending->generation = pending->schedulerGeneration = generation;
            pending->uploadId = QUuid::createUuid().toString(QUuid::WithoutBraces);
            pending->startAccepted = false;
            pending->waitingForResume = pending->payloadCompleteSent = false;
            pending->sentBytes = pending->sentForFile = pending->remoteAcknowledgedBytes = 0;
            pending->fileIndex = pending->localPercent = pending->remotePercent = 0;
            pending->durableOffsets.clear();
            pending->remoteFilePercents.clear();
            for (const auto& asset : pending->assets) pending->durableOffsets.insert(asset.assetId, 0);
            setParallelState(pending, OutgoingState::Queued);
            initializeParallelTimers(pending);
            m_parallelOutgoingByUpload.insert(pending->uploadId, pending);
            m_parallelUploadBySession.insert(remoteSessionId, pending->uploadId);
            emit uploadReidentified(previousUploadId, pending->uploadId, target);
            m_uploadScheduler->setSessionState(remoteSessionId, UploadScheduler::SessionState::Active);
            m_uploadScheduler->enqueue({remoteSessionId, generation, pending->uploadId, pending->assets.first().assetId});
        }
        if (!m_currentUploadId.isEmpty() && m_uploadTargetClientId == target
            && m_outgoingState == OutgoingState::Suspended
            && m_outgoingRemoteSessionId != remoteSessionId
            && m_uploadScheduler->isSessionTerminal(m_outgoingRemoteSessionId)) {
            const QString previousUploadId = m_currentUploadId;
            m_outgoingRemoteSessionId = remoteSessionId;
            m_outgoingGeneration = m_schedulerGeneration = generation;
            m_currentUploadId = QUuid::createUuid().toString(QUuid::WithoutBraces);
            emit uploadReidentified(previousUploadId, m_currentUploadId, target);
            m_outgoingStartAccepted = m_waitingForResume = m_outgoingPayloadCompleteSent = false;
            m_sentBytes = m_remoteAcknowledgedBytes = m_outgoingSentForFile = 0;
            m_outgoingFileIndex = m_outgoingChunkIndex = 0;
            m_outgoingDurableOffsets.clear();
            for (const auto& asset : m_outgoingAssets) m_outgoingDurableOffsets.insert(asset.assetId, 0);
            resetProgressTracking();
            setOutgoingState(OutgoingState::Queued);
            m_uploadScheduler->setSessionState(remoteSessionId, UploadScheduler::SessionState::Active);
            m_uploadScheduler->enqueue({remoteSessionId, generation, m_currentUploadId, m_outgoingAssets.first().assetId});
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
    for (const auto& target : m_outgoingRamByTarget.keys()) {
        const auto batch = m_outgoingRamByTarget.value(target);
        if (batch.remoteSessionId != remoteSessionId) continue;
        m_outgoingRamByTarget.remove(target);
        for (const auto& asset : batch.assets)
            for (const auto& fileId : asset.localFileIds)
                m_fileManager->unmarkFileUploadedToClient(fileId, target);
        emit uploadRamFailed(batch.uploadId, batch.assets.size());
    }
    m_cancelledRemoteAssets.remove(remoteSessionId);
    for (const auto& uploadId : m_pendingUploadAborts.keys())
        if (m_pendingUploadAborts.value(uploadId) == remoteSessionId)
            m_pendingUploadAborts.remove(uploadId);
    forgetRemoteSessionInventory(remoteSessionId);
    // A network session is terminal, but the user's uncancelled upload intent
    // remains in memory until a new authenticated session can submit it.
    if (auto* transfer = parallelForSession(remoteSessionId)) {
        suspendParallel(transfer);
        if (m_ws) m_ws->cancelUploadId(transfer->uploadId);
        m_remoteInventoryTargets.remove(transfer->targetEndpointId);
        if (m_fileManager) m_fileManager->unmarkAllForClient(transfer->targetEndpointId);
    }
    if (remoteSessionId == m_outgoingRemoteSessionId && !m_currentUploadId.isEmpty()) {
        suspendOutgoingForResume(reason);
        if (m_ws) m_ws->cancelUploadId(m_currentUploadId);
        m_uploadActive = false;
        m_remoteInventoryTargets.remove(m_uploadTargetClientId);
        if (m_fileManager) m_fileManager->unmarkAllForClient(m_uploadTargetClientId);
    }
    if (m_uploadScheduler) m_uploadScheduler->cancelSession(remoteSessionId);
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
    if (resetSendCursor && assets.size() != m_outgoingAssets.size()) {
        if (errorMessage) *errorMessage = QStringLiteral("Incomplete upload inventory");
        return false;
    }
    QHash<QString, qint64> offsets = resetSendCursor ? QHash<QString, qint64>() : m_outgoingDurableOffsets;
    QSet<QString> seen;
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
        if (assetIt == m_outgoingAssets.cend() || seen.contains(assetId)
            || !parseNonNegativeOffset(state.value(QStringLiteral("offset")), offset)
            || !parseManifestSize(state.value(QStringLiteral("size")), size)
            || size != assetIt->size || offset > size
            || state.value(QStringLiteral("sha256")).toString() != assetIt->sha256
            || offset < m_outgoingDurableOffsets.value(assetId, 0)) {
            if (errorMessage) *errorMessage = QStringLiteral("Mismatched upload inventory");
            return false;
        }
        seen.insert(assetId);
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
            m_outgoingSentForFile / (32 * 1024));
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
    m_outgoingStartAccepted = false;
    m_outgoingWindowBytes = 64 * 1024;
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

    if (m_ws && !m_ws->canIssueSessionCommands(m_outgoingRemoteSessionId)) {
        suspendOutgoingForResume(QStringLiteral("Session awaiting reconciliation"));
        return;
    }
    if (!m_ws || !m_ws->isUploadSessionTransportAvailable()) {
        suspendOutgoingForResume(QStringLiteral("Upload transport was interrupted"));
        return;
    }

    constexpr qint64 chunkBytes = 32 * 1024;
    constexpr qint64 maximumChunkWireBytes = ((chunkBytes + 2) / 3) * 4 + 4096;
    int chunksQueuedThisPass = 0;

    while (chunksQueuedThisPass < kMaxChunksPerPump
           && m_outgoingState == OutgoingState::Streaming
           && !m_outgoingPayloadCompleteSent) {
        const qint64 queuedBytes = m_ws->uploadTransportBytesToWrite();
        if (queuedBytes < 0) {
            suspendOutgoingForResume(QStringLiteral("Upload transport was interrupted"));
            return;
        }
        if (queuedBytes > kMaxQueuedUploadBytes - maximumChunkWireBytes) {
            // bytesWritten will schedule the next pump. The stall timer covers
            // a peer or network that stops draining the bounded queue.
            return;
        }
        if (m_sentBytes - m_remoteAcknowledgedBytes
            >= m_outgoingWindowBytes) {
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
            const bool allBytesDurable =
                m_remoteAcknowledgedBytes == m_totalBytes
                && std::all_of(
                    m_outgoingAssets.cbegin(), m_outgoingAssets.cend(),
                    [this](const OutgoingAsset& asset) {
                        return m_outgoingDurableOffsets.value(asset.assetId, -1)
                            == asset.size;
                    });
            if (!allBytesDurable) {
                // upload_progress will schedule the next pump after the target
                // has durably committed the final outstanding bytes.
                return;
            }
            if (!m_ws->sendUploadComplete(m_outgoingRemoteSessionId,
                                          m_outgoingGeneration,
                                          m_currentUploadId,
                                          outgoingAssetStates(true))) {
                suspendOutgoingForResume(QStringLiteral("Upload transport was interrupted before completion"));
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
                m_outgoingSentForFile / (32 * 1024));
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
        const qint64 requestedBytes = std::min({chunkBytes, remaining, m_outgoingWindowBytes - (m_sentBytes - m_remoteAcknowledgedBytes)});
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
            suspendOutgoingForResume(QStringLiteral("Upload transport was interrupted"));
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

// collectSceneFiles removed; files now gathered by caller (ApplicationRuntime)

void UploadManager::resetToInitial() {
    if (!m_currentUploadId.isEmpty()) releaseSchedulerSlot(false);
    clearOutgoingTransfer(false);
    if (m_cancelFallbackTimer) m_cancelFallbackTimer->stop();
    m_uploadTargetClientId.clear();
    m_outgoingRemoteSessionId.clear();
    m_outgoingGeneration = 0;
    m_schedulerGeneration = 0;
    m_activeWorkspaceEndpointId.clear();
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
    const int effectivePercent = std::clamp(m_lastRemotePercent, 0, m_remoteProgressReceived ? 100 : 99);
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
    Q_UNUSED(local);
    const int effective = std::clamp(remote, 0, 99);
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

void UploadManager::closeIncomingFiles(IncomingUploadSession& incoming,
                                       bool flush)
{
    for (auto it = incoming.openFiles.begin(); it != incoming.openFiles.end(); ++it) {
        QFile* file = it.value();
        if (!file) continue;
        // Each asynchronous write already committed its durable offset. These
        // owner-thread handles never buffer payload bytes.
        Q_UNUSED(flush);
        file->close();
        delete file;
    }
    incoming.openFiles.clear();
}

void UploadManager::suspendIncomingForResume(IncomingUploadSession& incoming)
{
    if (incoming.uploadId.isEmpty()) return;
    if (incoming.writeCancelled) incoming.writeCancelled->store(true);
    ++incoming.writeEpoch;
    closeIncomingFiles(incoming, false);
    incoming.suspendedForResume = true;
    if (incoming.validationCancelled) incoming.validationCancelled->store(true);
    ++incoming.completionValidationEpoch;
    incoming.completionValidationPending = false;
    incoming.completionValidationDone = false;
    incoming.verifiedDigestsByPath.clear();
    if (incoming.stallTimer) incoming.stallTimer->stop();
}

QJsonArray UploadManager::incomingAssetOffsets(
    const IncomingUploadSession& incoming) const
{
    QJsonArray assets;
    QStringList assetIds = incoming.expectedSizes.keys();
    std::sort(assetIds.begin(), assetIds.end());
    for (const QString& assetId : assetIds) {
        QJsonObject asset;
        asset.insert(QStringLiteral("assetId"), assetId);
        asset.insert(QStringLiteral("offset"),
                     static_cast<double>(incoming.receivedByFile.value(assetId, 0)));
        asset.insert(QStringLiteral("size"),
                     static_cast<double>(incoming.expectedSizes.value(assetId, 0)));
        asset.insert(QStringLiteral("sha256"),
                     incoming.assetIdToSha256.value(assetId));
        assets.append(asset);
    }
    return assets;
}

void UploadManager::pruneIncomingUploadCompletions(qint64 nowMonotonicMs)
{
    for (auto it = m_incomingUploadCompletionTombstones.begin();
         it != m_incomingUploadCompletionTombstones.end();) {
        if (it->expiresAtMonotonicMs <= nowMonotonicMs) {
            it = m_incomingUploadCompletionTombstones.erase(it);
        } else {
            ++it;
        }
    }
    while (m_incomingUploadCompletionTombstones.size()
           > kMaxIncomingCompletionTombstones) {
        auto oldest = m_incomingUploadCompletionTombstones.begin();
        for (auto it = oldest; it != m_incomingUploadCompletionTombstones.end(); ++it) {
            if (it->expiresAtMonotonicMs < oldest->expiresAtMonotonicMs) oldest = it;
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
    const qint64 now = MouffetteClock::nowMs();
    pruneIncomingUploadCompletions(now);
    IncomingUploadCompletionTombstone tombstone;
    tombstone.senderEndpointId = senderEndpointId;
    tombstone.remoteSessionId = remoteSessionId;
    tombstone.generation = generation;
    tombstone.sourceConnectionGeneration = sourceConnectionGeneration;
    tombstone.uploadId = uploadId;
    tombstone.assets = assets;
    tombstone.expiresAtMonotonicMs = now
        + AppConfig::instance().incomingUploadCompletionTtlMs();
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
    pruneIncomingUploadCompletions(MouffetteClock::nowMs());
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
        if ((!binding.active || !m_ws->canIssueSessionCommands(binding.remoteSessionId)) || binding.generation != generation
            || binding.ownerEndpointId != senderEndpointId
            || binding.targetEndpointId != m_ws->endpointId()
            || binding.ownerConnectionGeneration != sourceConnectionGeneration) {
            return false;
        }
    }

    completed->generation = generation;
    completed->sourceConnectionGeneration = sourceConnectionGeneration;
    emitIncomingResponse(QStringLiteral("upload_finished"), senderEndpointId,
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

void UploadManager::emitIncomingResponse(const QString& type,
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
    emit uploadProtocolResponseReady(response);
}

RemoteCacheStore::CommitResult UploadManager::teardownRemoteSession(
    const QString& senderEndpointId,
    const QString& remoteSessionId,
    quint64 generation,
    const QString& teardownId)
{
    m_lastTeardownRemovedFileCount = 0;
    beginIncomingFileReaderTeardown({remoteSessionId});
    if (!incomingFileReadersSettled({remoteSessionId})) {
        RemoteCacheStore::CommitResult pending;
        pending.outcome = RemoteCacheStore::CommitOutcome::CleanupError;
        pending.teardownId = teardownId;
        pending.errorCode = QStringLiteral("file_readers_pending");
        return pending;
    }
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


    QStringList matchingUploads;
    for (auto it = m_incomingUploads.cbegin(); it != m_incomingUploads.cend(); ++it) {
        const IncomingUploadSession* incoming = it.value();
        if (incoming && incoming->senderId == senderEndpointId
            && incoming->remoteSessionId == remoteSessionId) {
            matchingUploads.append(it.key());
        }
    }
    for (const QString& uploadId : matchingUploads) {
        discardIncomingUpload(uploadId, false, true);
    }

    m_lastTeardownRemovedFileCount = detachReceivedMappingsForScope(scope);
    if (m_lastTeardownRemovedFileCount > 0)
        m_teardownRemovedMappings[remoteSessionId] += m_lastTeardownRemovedFileCount;
    const RemoteCacheStore::CommitResult committed =
        m_remoteCacheStore->requestTeardown(scope, teardownId);
    if (committed.acknowledgementSafe())
        m_lastTeardownRemovedFileCount = m_teardownRemovedMappings.take(remoteSessionId);
    if (committed.outcome != RemoteCacheStore::CommitOutcome::Pending
        && !committed.acknowledgementSafe()) {
        emit remoteSessionCacheCleanupError(senderEndpointId, remoteSessionId,
                                            generation, committed.teardownId,
                                            committed.errorCode);
    }
    return committed;
}

int UploadManager::detachReceivedMappingsForScope(
    const RemoteCacheStore::Scope& scope)
{
    releaseResidency(scope.remoteSessionId);
    if (!m_fileManager || !m_remoteCacheStore) {
        return 0;
    }
    return m_fileManager->removeReceivedFileMappingsForScope(scope);
}

void UploadManager::beginTerminalIncomingCleanup(
    const QString& reasonCode,
    const QSet<QString>& remoteSessionIds)
{
    // This is only the command/writer barrier. Render-facing mappings and the
    // live cache namespace deliberately remain intact until ApplicationRuntime has
    // observed RemoteSceneController::teardownSettled for every incoming
    // RemoteSession that existed at the terminal edge.
    beginIncomingFileReaderTeardown(remoteSessionIds);
    if (remoteSessionIds.isEmpty()) {
        m_terminalIncomingCleanupAwaitingRenderer = true;
        m_receiverAdvertisementReady = false;
    }
    m_receiverCleanupReason = reasonCode.trimmed().isEmpty()
        ? QStringLiteral("terminal_session") : reasonCode.trimmed();
    m_receiverCleanupError = QStringLiteral("renderer_teardown_pending");
    if (remoteSessionIds.isEmpty()) m_incomingUploadCompletionTombstones.clear();
    else for (const QString& id : remoteSessionIds) forgetIncomingUploadCompletions(id);
    for (IncomingUploadSession* incoming : std::as_const(m_incomingUploads)) {
        if (incoming && (remoteSessionIds.isEmpty()
                         || remoteSessionIds.contains(incoming->remoteSessionId))) {
            suspendIncomingForResume(*incoming);
        }
    }
    if (remoteSessionIds.isEmpty())
        emit receiverAdvertisementReadinessChanged(false, m_receiverCleanupError);
}

UploadManager::BulkTeardownResult
UploadManager::completeTerminalIncomingCleanup(
    const QString& reasonCode,
    const QSet<QString>& remoteSessionIds)
{
    if (!incomingFileReadersSettled(remoteSessionIds)) {
        BulkTeardownResult pending;
        pending.cleanupErrorScopes = 1;
        pending.errorCode = QStringLiteral("file_readers_pending");
        return pending;
    }
    // Contract: the application calls this only from the renderer settlement
    // barrier. Clearing the guard first makes a failed logical commit eligible
    // for the explicit recovery path, while advertisement remains fail-closed.
    const bool globalCleanup = m_terminalIncomingCleanupAwaitingRenderer
        || !m_receiverAdvertisementReady;
    m_terminalIncomingCleanupAwaitingRenderer = false;
    if (!reasonCode.trimmed().isEmpty()) {
        m_receiverCleanupReason = reasonCode.trimmed();
    }

    const BulkTeardownResult result =
        teardownAllIncomingRemoteSessions(m_receiverCleanupReason,
                                          remoteSessionIds);
    if (!remoteSessionIds.isEmpty() && !globalCleanup) return result;
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
UploadManager::teardownAllIncomingRemoteSessions(
    const QString& reasonCode,
    const QSet<QString>& remoteSessionIds)
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
    for (const auto& scope : scopes) {
        if (!remoteSessionIds.isEmpty() && !remoteSessionIds.contains(scope.remoteSessionId)) continue;
        if (!m_terminalCacheTeardowns.contains(scope.remoteSessionId)) {
            m_terminalCacheTeardowns.insert(scope.remoteSessionId, {
                scope, QUuid::createUuid().toString(QUuid::WithoutBraces).toLower(), reasonCode});
        }
    }
    const auto jobs = m_terminalCacheTeardowns;
    for (const auto& job : jobs) {
        const auto& scope = job.scope;
        if (!remoteSessionIds.isEmpty() && !remoteSessionIds.contains(scope.remoteSessionId)) continue;
        ++summary.discoveredScopes;
        QStringList matchingUploads;
        for (const auto* incoming : std::as_const(m_incomingUploads)) {
            if (incoming && incoming->remoteSessionId == scope.remoteSessionId)
                matchingUploads.append(incoming->uploadId);
        }
        for (const QString& uploadId : matchingUploads) discardIncomingUpload(uploadId, false, true);
        const int removedMappings = detachReceivedMappingsForScope(scope);
        if (removedMappings > 0) m_teardownRemovedMappings[scope.remoteSessionId] += removedMappings;
        summary.removedFileMappings += removedMappings;
        const auto committed = m_remoteCacheStore->requestTeardown(scope, job.teardownId, job.reason);
        summary.quarantinedBytes += committed.quarantinedBytes;
        if (committed.acknowledgementSafe()) {
            ++summary.committedScopes;
            m_terminalCacheTeardowns.remove(scope.remoteSessionId);
            m_teardownRemovedMappings.remove(scope.remoteSessionId);
        } else if (committed.outcome == RemoteCacheStore::CommitOutcome::Pending) {
            ++summary.pendingScopes;
        } else {
            ++summary.cleanupErrorScopes;
            if (summary.errorCode.isEmpty()) summary.errorCode = committed.errorCode;
            emit remoteSessionCacheCleanupError(scope.senderEndpointId, scope.remoteSessionId,
                scope.generation, job.teardownId, committed.errorCode);
        }
    }
    m_lastTeardownRemovedFileCount = summary.removedFileMappings;
    return summary;
}

bool UploadManager::retryReceiverAdvertisementCleanup()
{
    if (receiverReadyForAdvertisement()) return true;
    if (!m_remoteCacheStore || m_remoteCacheStore->teardownPending()
        || m_remoteCacheStore->recoveryPending() || !incomingFileReadersSettled()) return false;
    if (m_terminalIncomingCleanupAwaitingRenderer) {
        m_receiverCleanupError = QStringLiteral("renderer_teardown_pending");
        emit receiverAdvertisementReadinessChanged(false,
                                                   m_receiverCleanupError);
        return false;
    }

    // Recover durable intents on the serialized I/O queue. In particular a
    // slow fsync or quarantine scan must never hold up heartbeats.
    m_remoteCacheStore->requestRecovery();
    return false;
}

bool UploadManager::discardIncomingUpload(const QString& uploadId,
                                          bool rememberRejectedUpload, bool preserveBytes) {
    IncomingUploadSession* incoming = m_incomingUploads.value(uploadId, nullptr);
    if (!incoming) return false;
    if (m_validationReadersByUpload.value(uploadId) > 0) {
        suspendIncomingForResume(*incoming);
        m_deferredIncomingDiscards.insert(uploadId,
            rememberRejectedUpload || m_deferredIncomingDiscards.value(uploadId));
        return false;
    }
    if (incoming->remoteSessionId.isEmpty() && m_stagingCleanupAttempts.size() >= 4096
        && !m_stagingCleanupAttempts.contains(incoming->senderId + QLatin1Char('/') + uploadId)) {
        qWarning() << "Staging cleanup capacity exhausted; retaining incoming obligation" << uploadId;
        return false;
    }
    const QString senderId = incoming->senderId;
    const QString remoteSessionId = incoming->remoteSessionId;
    const quint64 generation = incoming->generation;
    const QString cacheDirPath = incoming->cacheDirPath;
    const QHash<QString, QString> ownedPaths = incoming->filePaths;
    bool cleanupSucceeded = true;
    if (!preserveBytes && m_remoteCacheStore) {
        for (const QString& digest : incoming->assetIdToSha256) {
            if (!m_remoteCacheStore->purgeRetainedAsset(senderId, digest)) return false;
        }
    }

    for (auto it = incoming->openFiles.begin(); it != incoming->openFiles.end(); ++it) {
        if (!it.value()) continue;
        it.value()->close();
        delete it.value();
    }
    incoming->openFiles.clear();

    for (auto it = ownedPaths.constBegin(); it != ownedPaths.constEnd(); ++it) {
        const QString path = it.value();
        const RemoteCacheStore::Scope scope{senderId, remoteSessionId, generation};
        const bool sessionOwned = !remoteSessionId.isEmpty() && m_remoteCacheStore
            && m_remoteCacheStore->ownsPath(scope, path);
        if (path.isEmpty() || cacheDirPath.isEmpty()
            || (remoteSessionId.isEmpty() && !pathIsInsideDirectory(path, cacheDirPath))
            || (!remoteSessionId.isEmpty() && !sessionOwned)) {
            cleanupSucceeded = false;
            continue;
        }

        const QString fileId = incoming->assetIdToFileId.value(it.key());
        const QString mappedPath = remoteSessionId.isEmpty()
            ? m_fileManager->getFilePathForId(fileId)
            : m_fileManager->getReceivedFilePath(scope, fileId);
        if (preserveBytes || (!mappedPath.isEmpty() && QDir::cleanPath(mappedPath) == QDir::cleanPath(path))) continue;
        QFile::remove(path + QStringLiteral(".manifest.json"));
        const bool removed = !QFileInfo::exists(path) || QFile::remove(path);
        if (!removed) {
            qWarning() << "UploadManager: could not remove partial upload file";
            cleanupSucceeded = false;
            continue;
        }
        if (!mappedPath.isEmpty()
            && QDir::cleanPath(QFileInfo(mappedPath).absoluteFilePath())
                == QDir::cleanPath(QFileInfo(path).absoluteFilePath())) {
            if (remoteSessionId.isEmpty()) {
                m_fileManager->removeReceivedFileMapping(fileId);
            } else {
                m_fileManager->removeReceivedFileMapping(scope, fileId);
            }
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
    if (incoming->stallTimer) {
        incoming->stallTimer->stop();
        incoming->stallTimer->deleteLater();
    }
    m_incomingUploads.remove(uploadId);
    delete incoming;

    if (rememberRejectedUpload && !uploadId.isEmpty() && uploadId.size() <= 128) {
        if (m_canceledIncoming.size() >= 256) m_canceledIncoming.clear();
        m_canceledIncoming.insert(uploadId);
    }
    if (!cleanupSucceeded && remoteSessionId.isEmpty()
        && !senderId.isEmpty() && !uploadId.isEmpty()) {
        scheduleStagingCleanup(senderId, uploadId);
    }
    return cleanupSucceeded;
}

void UploadManager::scheduleReceiverCleanup()
{
    const QString key = QStringLiteral("receiver");
    if (receiverReadyForAdvertisement() || m_receiverCleanupRetries.contains(key)) return;
    const auto& config = AppConfig::instance();
    const int delay = RetryPolicy{config.deferredCleanupRetryMs(), config.deferredCleanupRetryMaxMs(),
                                 config.reconnectJitterPercent()}.delay(m_receiverCleanupAttempt);
    m_receiverCleanupAttempt = RetryPolicy::increment(m_receiverCleanupAttempt);
    m_receiverCleanupRetries.schedule(key, delay, [this] {
        if (!receiverReadyForAdvertisement() && !m_terminalIncomingCleanupAwaitingRenderer
            && !m_remoteCacheStore->teardownPending() && incomingFileReadersSettled())
            retryReceiverAdvertisementCleanup();
        scheduleReceiverCleanup();
    });
}

void UploadManager::scheduleStagingCleanup(const QString& senderId, const QString& uploadId)
{
    const QString key = senderId + QLatin1Char('/') + uploadId;
    if (m_stagingCleanupRetries.contains(key)) return;
    const auto& config = AppConfig::instance();
    const int attempt = m_stagingCleanupAttempts.value(key);
    m_stagingCleanupAttempts.insert(key, RetryPolicy::increment(attempt));
    const int delay = RetryPolicy{config.deferredCleanupRetryMs(), config.deferredCleanupRetryMaxMs(),
                                 config.reconnectJitterPercent()}.delay(attempt);
    m_stagingCleanupRetries.schedule(key, delay, [this, senderId, uploadId, key] {
        if (removeResidualIncomingStaging(senderId, uploadId)) m_stagingCleanupAttempts.remove(key);
        else {
            qWarning() << "staging_cleanup_retry" << "uploadId" << uploadId << "attempt" << m_stagingCleanupAttempts.value(key);
            scheduleStagingCleanup(senderId, uploadId);
        }
    });
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
        for (const QString& mappedPath : m_fileManager->getRecordedFilePathsForId(fileId)) {
            if (!mappedPath.isEmpty()
                && pathIsInsideDirectory(mappedPath, stagingCanonical)) {
                qWarning() << "UploadManager: refusing to remove mapped residual staging";
                return false;
            }
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
    IncomingUploadSession* incoming = m_incomingUploads.value(uploadId, nullptr);
    const bool matchesActive = incoming
        && (senderId.isEmpty() || incoming->senderId == senderId);
    const QString effectiveSender = senderId.isEmpty() && matchesActive
        ? incoming->senderId : senderId;
    const QString effectiveRemoteSessionId = remoteSessionId.isEmpty() && matchesActive
        ? incoming->remoteSessionId : remoteSessionId;
    const quint64 effectiveGeneration = generation == 0 && matchesActive
        ? incoming->generation : generation;

    qWarning() << "UploadManager: rejecting incoming upload:" << reason;
    if (discardMatchingSession && matchesActive) {
        discardIncomingUpload(uploadId, true);
    }
    if (!effectiveSender.isEmpty() && !effectiveRemoteSessionId.isEmpty()
        && effectiveGeneration > 0 && !uploadId.isEmpty()) {
        emitIncomingResponse(QStringLiteral("upload_rejected"), effectiveSender,
                             effectiveRemoteSessionId, effectiveGeneration,
                             uploadId,
                             {{QStringLiteral("code"), QStringLiteral("target_rejected")},
                              {QStringLiteral("reason"), reason.left(512)}});
    }
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
        emit uploadProtocolResponseReady(response);
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
        || !MediaFormatContract::isCanonicalMediaExtension(extension)) {
        reject(QStringLiteral("invalid_asset_removal"));
        return;
    }

    if (m_pendingDiskRemovals.contains(removalId)) {
        const auto pending = m_pendingDiskRemovals.value(removalId);
        static const QStringList immutableFields = {
            QStringLiteral("remoteSessionId"), QStringLiteral("removalId"),
            QStringLiteral("uploadId"), QStringLiteral("assetId"),
            QStringLiteral("offset"), QStringLiteral("size"),
            QStringLiteral("sha256"), QStringLiteral("fileId"), QStringLiteral("extension")
        };
        bool same = senderCacheNamespace(pending) == senderEndpointId;
        for (const auto& field : immutableFields) same &= pending.value(field) == message.value(field);
        if (!same) reject(QStringLiteral("asset_removal_conflict"));
        // A resume may redispatch the same transaction on a newer generation.
        // Its original durable callback remains authoritative and must finish.
        return;
    }

    const RemoteCacheStore::Scope scope{
        senderEndpointId, remoteSessionId, generation
    };
    QString pathError;
    const QString expectedPath = m_remoteCacheStore->assetPath(
        scope, assetId, RemoteCacheStore::AssetArea::Validated,
        extension, &pathError);
    const QString mappedPath = m_fileManager->getReceivedFilePath(scope, fileId);
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
    releaseResidency(scope.remoteSessionId, fileId);
    if (!incomingFileReadersSettled({scope.remoteSessionId})) {
        m_deferredIncomingRemovals.insert(removalId, message);
        return;
    }
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
    m_pendingDiskRemovals.insert(removalId, message);
    ++m_validationReadersBySession[remoteSessionId];
    const QPointer<UploadManager> guard(this);
    const bool queued = m_remoteCacheStore->requestAssetRemoval(scope, removal,
        [guard, scope, removalId, uploadId, fileId, mappedPath, response](
            const RemoteCacheStore::AssetRemovalResult& removed) mutable {
        if (!guard) return;
        auto* self = guard.data();
        self->m_pendingDiskRemovals.remove(removalId);
        if (--self->m_validationReadersBySession[scope.remoteSessionId] <= 0)
            self->m_validationReadersBySession.remove(scope.remoteSessionId);
        if (!removed.acknowledgementSafe()) {
            const QString error = removed.errorCode.isEmpty()
                ? QStringLiteral("asset_quarantine_failed") : removed.errorCode;
            response.insert(QStringLiteral("success"), false);
            response.insert(QStringLiteral("result"), QStringLiteral("cleanup_error"));
            response.insert(QStringLiteral("cacheQuarantined"), false);
            response.insert(QStringLiteral("errorCode"), error.left(128));
            response.insert(QStringLiteral("reason"), error.left(512));
        } else {
            if (!mappedPath.isEmpty()) {
                self->releaseResidency(scope.remoteSessionId, fileId);
                self->m_fileManager->removeReceivedFileMapping(scope, fileId);
                self->m_fileManager->dissociateFileFromProject(fileId, scope.remoteSessionId);
            }
            self->m_incomingUploadCompletionTombstones.remove(uploadId);
            response.insert(QStringLiteral("success"), true);
            response.insert(QStringLiteral("result"), QStringLiteral("committed"));
            response.insert(QStringLiteral("cacheQuarantined"), true);
            response.insert(QStringLiteral("removedFileCount"), mappedPath.isEmpty() ? 0 : 1);
            response.insert(QStringLiteral("quarantinedBytes"), static_cast<double>(removed.quarantinedBytes));
        }
        emit self->uploadProtocolResponseReady(response);
        self->settleIncomingFileReaders();
    });
    if (!queued) {
        m_pendingDiskRemovals.remove(removalId);
        if (--m_validationReadersBySession[remoteSessionId] <= 0)
            m_validationReadersBySession.remove(remoteSessionId);
        reject(QStringLiteral("cleanup_queue_full"));
    }
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
    if (message.value(QStringLiteral("protocolVersion")).toInt(-1)
            != MouffetteProtocol::Version
        || !m_ws) return;
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

    m_outgoingWindowBytes = std::clamp<qint64>(message.value(QStringLiteral("windowBytes")).toInteger(m_outgoingWindowBytes), 1, 256 * 1024);
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
        m_outgoingStartAccepted = true;
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
        if (!message.value(QStringLiteral("assets")).isArray()
            || message.value(QStringLiteral("assets")).toArray().size() != m_outgoingAssets.size()) return;
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
        if (message.value(QStringLiteral("temporary")).toBool()
            || message.value(QStringLiteral("errorClass")).toString() == QLatin1String("temporary")) {
            suspendOutgoingForResume(message.value(QStringLiteral("code")).toString());
            QTimer::singleShot(500, this, [this] { resumeOutgoingUpload(); });
            return;
        }
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
    
    // Durable bytes release the transfer slot; decoded RAM readiness is the
    // separate terminal barrier for the user's upload operation.
    for (const OutgoingAsset& asset : std::as_const(m_outgoingAssets)) {
        for (const QString& localFileId : asset.localFileIds) {
            emit fileUploadProgress(localFileId, 100);
        }
    }
    const QString session = m_outgoingRemoteSessionId;
    beginRamLoading(m_uploadTargetClientId, session, m_outgoingGeneration,
                    uploadId, m_outgoingAssets);
    m_remoteInventoryTargets.insert(m_uploadTargetClientId);
    releaseSchedulerSlot(true);
    clearOutgoingTransfer(true);
    updateRamLoading(session);
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
    // durable contiguous bytes for a possible signed resume before its
    // configured orphan deadline.
    for (IncomingUploadSession* incoming : std::as_const(m_incomingUploads)) {
        if (incoming) suspendIncomingForResume(*incoming);
    }
}

// Incoming side (target) - replicate subset of ApplicationRuntime logic for assembling files
void UploadManager::handleIncomingMessage(const QJsonObject& message) {
    const QString type = message.value("type").toString();
    if (type == "upload_start") {
        const QString senderId = senderCacheNamespace(message);
        const QString remoteSessionId = message.value("remoteSessionId").toString();
        const QString uploadId = message.value("uploadId").toString();
        quint64 generation = 0;
        quint64 sourceConnectionGeneration = 0;
        if (!m_remoteCacheReady
            || message.value("protocolVersion").toInt(-1)
                != MouffetteProtocol::Version
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
                || !MediaFormatContract::isCanonicalMediaExtension(file.extension)
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

        if (auto* existing = m_incomingUploads.value(uploadId)) {
            bool exact = existing->senderId == senderId && existing->remoteSessionId == remoteSessionId
                && existing->generation <= generation && existing->totalFiles == validatedFiles.size();
            for (const auto& file : validatedFiles) {
                exact = exact && existing->assetIdToSha256.value(file.assetId) == file.sha256
                    && existing->expectedSizes.value(file.assetId, -1) == file.size
                    && existing->assetIdToExtension.value(file.assetId) == file.extension
                    && existing->assetIdToName.value(file.assetId) == file.name
                    && existing->assetIdToMediaIds.value(file.assetId) == file.mediaIds;
            }
            if (!exact) {
                rejectIncomingUpload(senderId, uploadId, QStringLiteral("Conflicting upload start replay"), false, remoteSessionId, generation);
            } else if (existing->initializationPending) {
                return;
            } else if (existing->suspendedForResume || existing->generation != generation
                       || existing->sourceConnectionGeneration != sourceConnectionGeneration) {
                QJsonObject resume = message;
                resume.insert(QStringLiteral("type"), QStringLiteral("upload_resume"));
                resume.insert(QStringLiteral("assets"), incomingAssetOffsets(*existing));
                handleIncomingMessage(resume);
            } else {
                emitIncomingResponse(QStringLiteral("upload_ready"), senderId, remoteSessionId, generation,
                    uploadId, {{QStringLiteral("assets"), incomingAssetOffsets(*existing)}});
            }
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

        auto* incomingSession = new IncomingUploadSession;
        m_incomingUploads.insert(uploadId, incomingSession);
        IncomingUploadSession& incoming = *incomingSession;
        incoming.stallTimer = new QTimer(this);
        incoming.stallTimer->setSingleShot(true);
        const int advertisedIdleTimeout = m_ws
            ? m_ws->serverPolicy().value(
                  QStringLiteral("uploadIdleTimeoutMs")).toInt()
            : 0;
        incoming.stallTimer->setInterval(advertisedIdleTimeout > 0
            ? advertisedIdleTimeout
            : AppConfig::instance().uploadIdleTimeoutMs());
        connect(incoming.stallTimer, &QTimer::timeout, this,
                [this, uploadId]() {
            IncomingUploadSession* stalled = m_incomingUploads.value(
                uploadId, nullptr);
            if (!stalled) return;
            suspendIncomingForResume(*stalled);
        });
        incoming.senderId = senderId;
        incoming.targetEndpointId = message.value(QStringLiteral("targetEndpointId")).toString(m_ws ? m_ws->endpointId() : QString());
        incoming.remoteSessionId = remoteSessionId;
        incoming.generation = generation;
        incoming.sourceConnectionGeneration = sourceConnectionGeneration;
        incoming.uploadId = uploadId;
        incoming.totalFiles = validatedFiles.size();
        incoming.totalSize = totalSize;
        m_canceledIncoming.remove(uploadId);

        for (const ValidatedManifestFile& file : std::as_const(validatedFiles)) {
            const QString fullPath = m_remoteCacheStore->stagingAssetPath(
                scope, uploadId, file.assetId, file.extension, &cacheError);
            if (fullPath.isEmpty()) {
                rejectIncomingUpload(senderId, uploadId,
                                     QStringLiteral("Remote cache rejected the asset: %1").arg(cacheError),
                                     true, remoteSessionId, generation);
                return;
            }
            if (incoming.cacheDirPath.isEmpty()) {
                incoming.cacheDirPath = QFileInfo(fullPath).absolutePath();
            }

            incoming.expectedSizes.insert(file.assetId, file.size);
            incoming.receivedByFile.insert(file.assetId, 0);
            incoming.filePaths.insert(file.assetId, fullPath);
            incoming.assetIdToFileId.insert(file.assetId, file.fileId);
            incoming.assetIdToSha256.insert(file.assetId, file.sha256);
            incoming.assetIdToName.insert(file.assetId, file.name);
            incoming.assetIdToExtension.insert(file.assetId, file.extension);
            incoming.assetIdToMediaIds.insert(file.assetId, file.mediaIds);
            m_expectedChunkIndex.insert(uploadId + QLatin1Char(':') + file.assetId, 0);
        }
        // Hash verification and retained-byte copying can take seconds on a
        // slow disk. Neither blocks the network thread or its heartbeats.
        incoming.initializationPending = true;
        incoming.writeCancelled = std::make_shared<std::atomic_bool>(false);
        const auto cancelled = incoming.writeCancelled;
        const auto paths = incoming.filePaths;
        const QString target = incoming.targetEndpointId;
        ++m_validationReadersBySession[remoteSessionId];
        ++m_validationReadersByUpload[uploadId];
        using InitialFiles = QPair<QHash<QString, qint64>, QString>;
        auto* watcher = new QFutureWatcher<InitialFiles>(this);
        connect(watcher, &QFutureWatcher<InitialFiles>::finished, this,
            [this, watcher, incomingSession, uploadId, senderId, remoteSessionId, generation, cancelled] {
                const auto result = watcher->result();
                watcher->deleteLater();
                if (--m_validationReadersBySession[remoteSessionId] == 0) m_validationReadersBySession.remove(remoteSessionId);
                if (--m_validationReadersByUpload[uploadId] == 0) m_validationReadersByUpload.remove(uploadId);
                auto* live = m_incomingUploads.value(uploadId);
                if (live && live == incomingSession) {
                    live->initializationPending = false;
                    if (!cancelled->load() && result.second.isEmpty()) {
                        live->received = 0;
                        for (auto offset = result.first.cbegin(); offset != result.first.cend(); ++offset) {
                            live->receivedByFile.insert(offset.key(), offset.value());
                            live->received += offset.value();
                            m_expectedChunkIndex.insert(uploadId + QLatin1Char(':') + offset.key(), offset.value());
                        }
                        restartIncomingStallTimer(*live);
                        emitIncomingResponse(QStringLiteral("upload_ready"), senderId, remoteSessionId, generation, uploadId,
                            {{QStringLiteral("assets"), incomingAssetOffsets(*live)}});
                    } else if (!cancelled->load()) {
                        rejectIncomingUpload(senderId, uploadId, result.second, true, remoteSessionId, generation);
                    }
                }
                settleIncomingFileReaders();
            });
        watcher->setFuture(QtConcurrent::run(&m_incomingWritePool,
            [root = m_remoteCacheStore->rootPath(), paths, validatedFiles, scope, target, cancelled]() -> InitialFiles {
                RemoteCacheStore disk(root);
                QHash<QString, qint64> offsets;
                for (const auto& file : validatedFiles) {
                    if (cancelled->load()) return {offsets, QStringLiteral("upload_cancelled")};
                    const QString path = paths.value(file.assetId);
                    if (!disk.ownsPath(scope, path) || QFileInfo(path).isSymLink())
                        return {offsets, QStringLiteral("unsafe_asset_path")};
                    if (QFileInfo::exists(path) && !QFile::remove(path))
                        return {offsets, QStringLiteral("stale_asset_reset_failed")};
                    QFile::remove(path + QStringLiteral(".manifest.json"));
                    const qint64 offset = disk.restoreRetainedAsset(scope, target, path, file.sha256, file.size, file.extension);
                    QFile output(path);
                    if (!output.open(offset > 0 ? QIODevice::ReadWrite : (QIODevice::WriteOnly | QIODevice::NewOnly))
                        || !syncFile(&output)
                        || !disk.checkpointAsset(scope, target, path, file.sha256, file.size, offset, file.extension))
                        return {offsets, QStringLiteral("asset_checkpoint_failed")};
                    offsets.insert(file.assetId, offset);
                }
                return {offsets, {}};
            }));
    } else if (type == "upload_resume") {
        const QString senderId = senderCacheNamespace(message);
        const QString remoteSessionId = message.value("remoteSessionId").toString();
        const QString uploadId = message.value("uploadId").toString();
        quint64 generation = 0;
        quint64 sourceConnectionGeneration = 0;
        if (message.value("protocolVersion").toInt(-1)
                != MouffetteProtocol::Version
            || !parsePositiveGeneration(message.value("generation"), generation)
            || !parsePositiveGeneration(message.value("connectionGeneration"),
                                        sourceConnectionGeneration)) {
            rejectIncomingUpload(senderId, uploadId,
                                 QStringLiteral("Invalid upload resume binding"), false,
                                 remoteSessionId, generation);
            return;
        }
        IncomingUploadSession* incomingSession =
            m_incomingUploads.value(uploadId, nullptr);
        if (!incomingSession) {
            rejectIncomingUpload(senderId, uploadId,
                                 QStringLiteral("No resumable upload exists"), false,
                                 remoteSessionId, generation);
            return;
        }
        IncomingUploadSession& incoming = *incomingSession;
        if (type == QLatin1String("upload_complete") && incoming.completionPromotionPending) return;
        if (senderId != incoming.senderId
            || remoteSessionId != incoming.remoteSessionId
            || generation < incoming.generation
            || sourceConnectionGeneration < incoming.sourceConnectionGeneration
            || !message.value("assets").isArray()) {
            rejectIncomingUpload(senderId, uploadId,
                                 QStringLiteral("Invalid upload resume binding"), true,
                                 remoteSessionId, generation);
            return;
        }

        if (m_validationReadersByUpload.value(uploadId) > 0) {
            suspendIncomingForResume(incoming);
            incoming.deferredResume = message;
            return;
        }
        incoming.deferredResume = {};
        incoming.writeCancelled = std::make_shared<std::atomic_bool>(false);
        ++incoming.writeEpoch;
        const RemoteCacheStore::Scope oldScope {
            senderId, remoteSessionId, incoming.generation
        };
        if (generation > incoming.generation) {
            QString cacheError;
            if (!m_remoteCacheStore->rebindSessionGeneration(oldScope, generation,
                                                              &cacheError)) {
                rejectIncomingUpload(senderId, uploadId,
                                     QStringLiteral("Remote cache rejected resume: %1").arg(cacheError),
                                     true, remoteSessionId, generation);
                return;
            }
            if (!m_fileManager->rebindReceivedFileScope(
                    oldScope, {senderId, remoteSessionId, generation})) {
                rejectIncomingUpload(senderId, uploadId,
                                     QStringLiteral("Received-file scope rejected resume"),
                                     true, remoteSessionId, generation);
                return;
            }
            incoming.generation = generation;
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
                || !incoming.expectedSizes.contains(assetId)
                || size != incoming.expectedSizes.value(assetId)
                || asset.value("sha256").toString()
                    != incoming.assetIdToSha256.value(assetId)
                || offset > size) {
                rejectIncomingUpload(senderId, uploadId,
                                     QStringLiteral("Invalid upload resume inventory"), true,
                                     remoteSessionId, generation);
                return;
            }
            durableOffsets.insert(assetId, offset);
        }
        if (durableOffsets.size() != incoming.expectedSizes.size()) {
            rejectIncomingUpload(senderId, uploadId,
                                 QStringLiteral("Incomplete upload resume inventory"), true,
                                 remoteSessionId, generation);
            return;
        }

        closeIncomingFiles(incoming, true);
        incoming.initializationPending = true;
        incoming.sourceConnectionGeneration = sourceConnectionGeneration;
        const auto cancelled = incoming.writeCancelled;
        const auto paths = incoming.filePaths;
        const auto sizes = incoming.expectedSizes;
        const auto hashes = incoming.assetIdToSha256;
        const auto extensions = incoming.assetIdToExtension;
        const QString target = incoming.targetEndpointId;
        ++m_validationReadersBySession[remoteSessionId];
        ++m_validationReadersByUpload[uploadId];
        using ResumeFiles = QPair<QHash<QString, qint64>, QString>;
        auto* watcher = new QFutureWatcher<ResumeFiles>(this);
        connect(watcher, &QFutureWatcher<ResumeFiles>::finished, this,
            [this, watcher, incomingSession, uploadId, senderId, remoteSessionId, generation, cancelled] {
                const auto result = watcher->result();
                watcher->deleteLater();
                if (--m_validationReadersBySession[remoteSessionId] == 0) m_validationReadersBySession.remove(remoteSessionId);
                if (--m_validationReadersByUpload[uploadId] == 0) m_validationReadersByUpload.remove(uploadId);
                auto* live = m_incomingUploads.value(uploadId);
                if (live && live == incomingSession) {
                    live->initializationPending = false;
                    if (!cancelled->load() && result.second.isEmpty()) {
                        live->received = 0;
                        for (auto offset = result.first.cbegin(); offset != result.first.cend(); ++offset) {
                            live->receivedByFile.insert(offset.key(), offset.value());
                            live->queuedByFile.insert(offset.key(), offset.value());
                            live->received += offset.value();
                            m_expectedChunkIndex.insert(uploadId + QLatin1Char(':') + offset.key(), offset.value());
                        }
                        live->suspendedForResume = false;
                        live->lastProgressBytesReported = live->received;
                        restartIncomingStallTimer(*live);
                        emitIncomingResponse(QStringLiteral("upload_ready"), senderId, remoteSessionId, generation, uploadId,
                            {{QStringLiteral("assets"), incomingAssetOffsets(*live)}});
                    } else if (!cancelled->load()) {
                        rejectIncomingUpload(senderId, uploadId, result.second, true, remoteSessionId, generation);
                    }
                }
                settleIncomingFileReaders();
            });
        watcher->setFuture(QtConcurrent::run(&m_incomingWritePool,
            [root = m_remoteCacheStore->rootPath(), paths, sizes, hashes, extensions, durableOffsets,
             scope = resumedScope, target, cancelled]() -> ResumeFiles {
                RemoteCacheStore disk(root);
                QHash<QString, qint64> offsets;
                for (auto requested = durableOffsets.cbegin(); requested != durableOffsets.cend(); ++requested) {
                    if (cancelled->load()) return {offsets, QStringLiteral("upload_cancelled")};
                    const QString asset = requested.key();
                    const QString path = paths.value(asset);
                    const qint64 confirmed = disk.checkpointedAssetOffset(scope, target, path,
                        hashes.value(asset), sizes.value(asset), extensions.value(asset));
                    if (confirmed < requested.value()) return {offsets, QStringLiteral("durable_checkpoint_mismatch")};
                    QFile output(path);
                    if (!output.open(QIODevice::ReadWrite) || !output.resize(confirmed) || !syncFile(&output)
                        || !disk.checkpointAsset(scope, target, path, hashes.value(asset), sizes.value(asset), confirmed, extensions.value(asset)))
                        return {offsets, QStringLiteral("asset_checkpoint_failed")};
                    offsets.insert(asset, confirmed);
                }
                return {offsets, {}};
            }));
    } else if (type == "upload_chunk") {
        const QString senderId = senderCacheNamespace(message);
        const QString remoteSessionId = message.value("remoteSessionId").toString();
        const QString uploadId = message.value("uploadId").toString();
        quint64 generation = 0;
        quint64 sourceConnectionGeneration = 0;
        if (message.value("protocolVersion").toInt(-1)
                != MouffetteProtocol::Version
            || !parsePositiveGeneration(message.value("generation"), generation)
            || !parsePositiveGeneration(message.value("connectionGeneration"),
                                        sourceConnectionGeneration)) {
            rejectIncomingUpload(senderId, uploadId,
                                 QStringLiteral("Invalid upload generation"), false,
                                 remoteSessionId, generation);
            return;
        }
        if (m_canceledIncoming.contains(uploadId)) return;
        IncomingUploadSession* incomingSession =
            m_incomingUploads.value(uploadId, nullptr);
        if (!incomingSession) {
            rejectIncomingUpload(senderId, uploadId,
                                 QStringLiteral("No matching upload session"), false,
                                 remoteSessionId, generation);
            return;
        }
        IncomingUploadSession& incoming = *incomingSession;
        if (type == QLatin1String("upload_complete") && incoming.completionPromotionPending) return;
        if (senderId != incoming.senderId
            || remoteSessionId != incoming.remoteSessionId
            || generation != incoming.generation
            || sourceConnectionGeneration != incoming.sourceConnectionGeneration
            || incoming.suspendedForResume) {
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
        if (!RemoteCacheStore::isValidAssetId(assetId) || incoming.initializationPending
            || !incoming.filePaths.contains(assetId) || !incoming.expectedSizes.contains(assetId)) {
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
                != incoming.assetIdToSha256.value(assetId)) {
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

        const qint64 expectedForFile = incoming.expectedSizes.value(assetId, -1);
        if (offset < 0 || expectedForFile < 1 || data.size() > expectedForFile - offset) {
            rejectIncomingUpload(senderId, uploadId,
                QStringLiteral("Upload chunk exceeds the declared asset size"), true, remoteSessionId, generation);
            return;
        }
        constexpr qint64 maximumQueuedWriteBytes = 8 * 1024 * 1024;
        if (data.size() > maximumQueuedWriteBytes - m_queuedIncomingWriteBytes) {
            rejectIncomingUpload(senderId, uploadId,
                QStringLiteral("Durable upload window exceeded"), true, remoteSessionId, generation);
            return;
        }
        if (!incoming.writeCancelled) incoming.writeCancelled = std::make_shared<std::atomic_bool>(false);
        const auto cancelled = incoming.writeCancelled;
        const quint64 writeEpoch = incoming.writeEpoch;
        const QString path = incoming.filePaths.value(assetId);
        const qint64 byteCount = data.size();
        m_expectedChunkIndex[key] = offset + byteCount;
        incoming.queuedByFile[assetId] = offset + byteCount;
        m_queuedIncomingWriteBytes += byteCount;
        ++m_validationReadersBySession[remoteSessionId];
        ++m_validationReadersByUpload[uploadId];
        restartIncomingStallTimer(incoming);
        auto* watcher = new QFutureWatcher<bool>(this);
        connect(watcher, &QFutureWatcher<bool>::finished, this,
            [this, watcher, uploadId, senderId, remoteSessionId, assetId, generation,
             sourceConnectionGeneration, incomingSession, writeEpoch, cancelled, offset, byteCount]() {
                const bool durable = watcher->result();
                watcher->deleteLater();
                m_queuedIncomingWriteBytes -= byteCount;
                if (--m_validationReadersBySession[remoteSessionId] == 0)
                    m_validationReadersBySession.remove(remoteSessionId);
                if (--m_validationReadersByUpload[uploadId] == 0)
                    m_validationReadersByUpload.remove(uploadId);
                auto* live = m_incomingUploads.value(uploadId, nullptr);
                const bool current = live && live == incomingSession
                    && live->generation == generation
                    && live->sourceConnectionGeneration == sourceConnectionGeneration
                    && live->writeEpoch == writeEpoch && !live->suspendedForResume
                    && !cancelled->load();
                if (current && durable && live->receivedByFile.value(assetId, -1) == offset) {
                    live->receivedByFile[assetId] = offset + byteCount;
                    live->received += byteCount;
                    restartIncomingStallTimer(*live);
                    emitIncomingResponse(QStringLiteral("upload_progress"), senderId,
                        remoteSessionId, generation, uploadId,
                        {{QStringLiteral("durableBytes"), static_cast<double>(live->received)},
                         {QStringLiteral("totalSize"), static_cast<double>(live->totalSize)},
                         {QStringLiteral("delta"), true},
                         {QStringLiteral("assets"), QJsonArray{QJsonObject{
                             {QStringLiteral("assetId"), assetId},
                             {QStringLiteral("offset"), static_cast<double>(live->receivedByFile.value(assetId))},
                             {QStringLiteral("size"), static_cast<double>(live->expectedSizes.value(assetId))},
                             {QStringLiteral("sha256"), live->assetIdToSha256.value(assetId)}}}}});
                    live->lastProgressBytesReported = live->received;
                } else if (current) {
                    cancelled->store(true);
                    rejectIncomingUpload(senderId, uploadId,
                        QStringLiteral("Remote client could not durably write the upload"), true,
                        remoteSessionId, generation);
                }
                settleIncomingFileReaders();
            });
        watcher->setFuture(QtConcurrent::run(&m_incomingWritePool,
            [path, data, offset, cancelled, root = m_remoteCacheStore->rootPath(),
             scope = RemoteCacheStore::Scope{senderId, remoteSessionId, generation},
             target = incoming.targetEndpointId, sha = incoming.assetIdToSha256.value(assetId),
             size = incoming.expectedSizes.value(assetId), extension = incoming.assetIdToExtension.value(assetId)]() {
                if (cancelled->load()) return false;
                QFile file(path);
                const QFileInfo info(path);
                if (!info.isFile() || info.isSymLink()
                    || !file.open(QIODevice::ReadWrite) || file.size() != offset
                    || !file.seek(offset) || cancelled->load()) return false;
                if (file.write(data) != data.size() || !syncFile(&file)) return false;
                RemoteCacheStore disk(root);
                return disk.checkpointAsset(scope, target, path, sha, size, offset + data.size(), extension);
            }));
    } else if (type == "upload_complete") {
        const QString senderId = senderCacheNamespace(message);
        const QString remoteSessionId = message.value("remoteSessionId").toString();
        const QString uploadId = message.value("uploadId").toString();
        quint64 generation = 0;
        quint64 sourceConnectionGeneration = 0;
        if (message.value("protocolVersion").toInt(-1)
                != MouffetteProtocol::Version
            || !parsePositiveGeneration(message.value("generation"), generation)
            || !parsePositiveGeneration(message.value("connectionGeneration"),
                                        sourceConnectionGeneration)) {
            rejectIncomingUpload(senderId, uploadId, QStringLiteral("Invalid upload generation"),
                                 false, remoteSessionId, generation);
            return;
        }
        if (!m_incomingUploads.contains(uploadId)
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
        IncomingUploadSession* incomingSession =
            m_incomingUploads.value(uploadId, nullptr);
        if (!incomingSession) {
            rejectIncomingUpload(senderId, uploadId,
                                 QStringLiteral("No matching upload session"), false,
                                 remoteSessionId, generation);
            return;
        }
        IncomingUploadSession& incoming = *incomingSession;
        if (type == QLatin1String("upload_complete") && incoming.completionPromotionPending) return;
        if (senderId != incoming.senderId
            || remoteSessionId != incoming.remoteSessionId
            || generation != incoming.generation
            || sourceConnectionGeneration != incoming.sourceConnectionGeneration
            || incoming.suspendedForResume) {
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
                || size != incoming.expectedSizes.value(assetId, -1)
                || offset != size
                || asset.value("sha256").toString()
                    != incoming.assetIdToSha256.value(assetId)) {
                rejectIncomingUpload(senderId, uploadId,
                                     QStringLiteral("Invalid upload completion inventory"), true,
                                     remoteSessionId, generation);
                return;
            }
            completionAssets.insert(assetId);
        }
        if (completionAssets.size() != incoming.expectedSizes.size()) {
            rejectIncomingUpload(senderId, uploadId,
                                 QStringLiteral("Incomplete upload completion inventory"), true,
                                 remoteSessionId, generation);
            return;
        }

        bool closeSucceeded = true;
        for (auto it = incoming.openFiles.begin(); it != incoming.openFiles.end(); ++it) {
            if (!it.value()) {
                closeSucceeded = false;
                continue;
            }
            // Payload bytes were already fsynced by the serialized I/O worker.
            it.value()->close();
            closeSucceeded = (it.value()->error() == QFileDevice::NoError) && closeSucceeded;
            delete it.value();
        }
        incoming.openFiles.clear();

        QString completionError;
        QStringList assetIds = incoming.expectedSizes.keys();
        std::sort(assetIds.begin(), assetIds.end());
        if (!closeSucceeded || assetIds.size() != incoming.totalFiles) {
            completionError = QStringLiteral("Remote client could not finalize every upload file");
        }

        if (incoming.completionValidationPending) return;
        if (completionError.isEmpty() && !incoming.completionValidationDone) {
            QHash<QString, QString> expectedByPath;
            for (const QString& assetId : std::as_const(assetIds)) {
                const QString digest = incoming.assetIdToSha256.value(assetId);
                expectedByPath.insert(incoming.filePaths.value(assetId), digest);
                const QString existing = m_fileManager->getReceivedFilePath(
                    scope, incoming.assetIdToFileId.value(assetId));
                if (!existing.isEmpty()) expectedByPath.insert(existing, digest);
                QString ignored;
                const QString promoted = m_remoteCacheStore->assetPath(scope, assetId,
                    RemoteCacheStore::AssetArea::Validated,
                    incoming.assetIdToExtension.value(assetId), &ignored);
                if (!promoted.isEmpty() && QFileInfo::exists(promoted)) expectedByPath.insert(promoted, digest);
            }
            const auto validationCancelled = std::make_shared<std::atomic_bool>(false);
            incoming.validationCancelled = validationCancelled;
            const quint64 validationEpoch = ++incoming.completionValidationEpoch;
            incoming.completionValidationPending = true;
            ++m_validationReadersBySession[incoming.remoteSessionId];
            ++m_validationReadersByUpload[uploadId];
            const QString validationSessionId = incoming.remoteSessionId;
            if (incoming.stallTimer) incoming.stallTimer->stop();
            auto* watcher = new QFutureWatcher<QHash<QString, QString>>(this);
            connect(watcher, &QFutureWatcher<QHash<QString, QString>>::finished,
                    this, [this, watcher, message, uploadId, incomingSession, generation,
                           sourceConnectionGeneration, validationSessionId, validationEpoch]() {
                const auto result = watcher->result();
                watcher->deleteLater();
                if (--m_validationReadersBySession[validationSessionId] == 0)
                    m_validationReadersBySession.remove(validationSessionId);
                if (--m_validationReadersByUpload[uploadId] == 0)
                    m_validationReadersByUpload.remove(uploadId);
                settleIncomingFileReaders();
                auto* live = m_incomingUploads.value(uploadId, nullptr);
                if (!live || live != incomingSession || live->generation != generation
                    || live->sourceConnectionGeneration != sourceConnectionGeneration
                    || live->suspendedForResume
                    || live->completionValidationEpoch != validationEpoch) return;
                live->verifiedDigestsByPath = result;
                live->completionValidationPending = false;
                live->completionValidationDone = true;
                handleIncomingMessage(message);
            });
            watcher->setFuture(QtConcurrent::run([expectedByPath, validationCancelled]() {
                QHash<QString, QString> verified;
                for (auto it = expectedByPath.cbegin(); it != expectedByPath.cend(); ++it) {
                    if (validationCancelled->load(std::memory_order_relaxed)) break;
                    if (MediaFilePolicy::validateLocalFileMetadata(it.key()).accepted())
                        verified.insert(it.key(), sha256ForFile(it.key(), validationCancelled));
                }
                return verified;
            }));
            return;
        }

        QHash<QString, QString> selectedPathByAsset;
        QHash<QString, QString> selectedPathByFileId;
        for (const QString& assetId : std::as_const(assetIds)) {
            if (!completionError.isEmpty()) break;
            const QString fileId = incoming.assetIdToFileId.value(assetId);
            const QString expectedDigest = incoming.assetIdToSha256.value(assetId);
            const qint64 expectedSize = incoming.expectedSizes.value(assetId, -1);
            const qint64 receivedSize = incoming.receivedByFile.value(assetId, -1);
            const QString path = incoming.filePaths.value(assetId);
            const QFileInfo info(path);
            if (expectedSize < 1 || receivedSize != expectedSize) {
                qWarning().noquote()
                    << "UploadManager: incomplete durable inventory"
                    << "asset=" << assetId.left(12)
                    << "expected=" << expectedSize
                    << "received=" << receivedSize;
                completionError = QStringLiteral(
                    "Upload is incomplete: the remote client durably received %1 of %2 bytes")
                                      .arg(receivedSize)
                                      .arg(expectedSize);
                break;
            }
            if (!info.exists() || !info.isFile() || info.isSymLink()) {
                qWarning().noquote()
                    << "UploadManager: staging file unavailable at finalization"
                    << "asset=" << assetId.left(12)
                    << "path=" << QDir::toNativeSeparators(path);
                completionError = QStringLiteral(
                    "Remote upload staging file is unavailable during finalization");
                break;
            }
            if (info.size() != expectedSize) {
                qWarning().noquote()
                    << "UploadManager: staging size mismatch"
                    << "asset=" << assetId.left(12)
                    << "expected=" << expectedSize
                    << "onDisk=" << info.size();
                completionError = QStringLiteral(
                    "Remote upload size mismatch: expected %1 bytes, found %2")
                                      .arg(expectedSize)
                                      .arg(info.size());
                break;
            }
            if (!m_remoteCacheStore->ownsPath(scope, path)) {
                qWarning().noquote()
                    << "UploadManager: cache ownership verification failed"
                    << "asset=" << assetId.left(12)
                    << "path=" << QDir::toNativeSeparators(path);
                completionError = QStringLiteral(
                    "Remote upload cache ownership verification failed during finalization");
                break;
            }
            const QString actualDigest = incoming.verifiedDigestsByPath.value(path);
            if (actualDigest != expectedDigest) {
                qWarning().noquote()
                    << "UploadManager: staging checksum mismatch"
                    << "asset=" << assetId.left(12)
                    << "expected=" << expectedDigest.left(12)
                    << "actual=" << actualDigest.left(12);
                completionError = actualDigest.isEmpty()
                    ? QStringLiteral(
                          "Remote client could not read the completed upload for verification")
                    : QStringLiteral(
                          "Remote upload checksum does not match the source manifest");
                break;
            }
            if (!incoming.verifiedDigestsByPath.contains(path)) {
                completionError = QStringLiteral("Unsupported or invalid media file: %1")
                                      .arg(incoming.assetIdToName.value(assetId, assetId));
                break;
            }

            QString selectedPath = selectedPathByFileId.value(fileId);
            const QString expectedExtension = incoming.assetIdToExtension.value(assetId);
            if (!selectedPath.isEmpty()
                && QFileInfo(selectedPath).suffix().compare(expectedExtension,
                                                            Qt::CaseInsensitive) != 0) {
                completionError = QStringLiteral(
                    "Upload file identifier was reused with a different extension");
                break;
            }
            const QString existingPath =
                m_fileManager->getReceivedFilePath(scope, fileId);
            if (selectedPath.isEmpty() && !existingPath.isEmpty()) {
                const QFileInfo existingInfo(existingPath);
                const QString existingCanonicalPath = existingInfo.canonicalFilePath();
                if (existingCanonicalPath.isEmpty() || !existingInfo.isFile()
                    || existingInfo.isSymLink()
                    || !m_remoteCacheStore->ownsPath(scope, existingCanonicalPath)
                    || existingInfo.size() != expectedSize
                    || existingInfo.suffix().compare(expectedExtension, Qt::CaseInsensitive) != 0
                    || incoming.verifiedDigestsByPath.value(existingPath) != expectedDigest) {
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
                    incoming.assetIdToExtension.value(assetId), &cacheError);
                if (selectedPath.isEmpty()) {
                    completionError = QStringLiteral("Remote cache rejected validation: %1")
                                          .arg(cacheError);
                    break;
                }
                const QFileInfo selectedInfo(selectedPath);
                if (selectedInfo.exists()
                    && (selectedInfo.isSymLink() || !selectedInfo.isFile()
                        || selectedInfo.size() != expectedSize
                        || incoming.verifiedDigestsByPath.value(selectedPath) != expectedDigest)) {
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

        if (incoming.completionPromotionPending) return;
        incoming.completionPromotionPending = true;
        if (!incoming.validationCancelled) incoming.validationCancelled = std::make_shared<std::atomic_bool>(false);
        const auto cancelled = incoming.validationCancelled;
        const quint64 epoch = incoming.completionValidationEpoch;
        const auto stagingPaths = incoming.filePaths;
        const auto hashes = incoming.assetIdToSha256;
        const auto sizes = incoming.expectedSizes;
        const auto extensions = incoming.assetIdToExtension;
        const QString target = incoming.targetEndpointId;
        ++m_validationReadersBySession[remoteSessionId];
        ++m_validationReadersByUpload[uploadId];
        using Promotion = QPair<QHash<QString, QString>, QString>;
        auto* watcher = new QFutureWatcher<Promotion>(this);
        connect(watcher, &QFutureWatcher<Promotion>::finished, this,
            [this, watcher, incomingSession, uploadId, senderId, remoteSessionId, generation,
             sourceConnectionGeneration, epoch, cancelled, selectedPathByAsset, selectedPathByFileId] {
                const auto result = watcher->result();
                watcher->deleteLater();
                if (--m_validationReadersBySession[remoteSessionId] == 0) m_validationReadersBySession.remove(remoteSessionId);
                if (--m_validationReadersByUpload[uploadId] == 0) m_validationReadersByUpload.remove(uploadId);
                auto* live = m_incomingUploads.value(uploadId);
                if (live && live == incomingSession) {
                    // A cancellation can arrive after rename. Track the actual
                    // paths before the teardown/RESUME barrier is released.
                    for (auto path = result.first.cbegin(); path != result.first.cend(); ++path)
                        live->filePaths.insert(path.key(), path.value());
                    live->completionPromotionPending = false;
                    if (!cancelled->load() && live->generation == generation
                        && live->sourceConnectionGeneration == sourceConnectionGeneration
                        && live->completionValidationEpoch == epoch && !live->suspendedForResume) {
                        if (result.second.isEmpty())
                            finishIncomingPromotion(uploadId, senderId, remoteSessionId, generation,
                                sourceConnectionGeneration, selectedPathByAsset, selectedPathByFileId);
                        else rejectIncomingUpload(senderId, uploadId, result.second, true, remoteSessionId, generation);
                    }
                }
                settleIncomingFileReaders();
            });
        watcher->setFuture(QtConcurrent::run(&m_incomingWritePool,
            [root = m_remoteCacheStore->rootPath(), scope, target, stagingPaths, selectedPathByAsset,
             hashes, sizes, extensions, cancelled]() -> Promotion {
                RemoteCacheStore disk(root);
                QHash<QString, QString> promoted;
                for (auto asset = selectedPathByAsset.cbegin(); asset != selectedPathByAsset.cend(); ++asset) {
                    if (cancelled->load()) return {promoted, QStringLiteral("upload_cancelled")};
                    const QString stagingPath = stagingPaths.value(asset.key());
                    const QString selectedPath = asset.value();
                    if (!disk.ownsPath(scope, stagingPath) || !disk.ownsPath(scope, selectedPath))
                        return {promoted, QStringLiteral("unsafe_asset_promotion")};
                    if (QDir::cleanPath(stagingPath) != QDir::cleanPath(selectedPath)) {
                        if (!QFileInfo::exists(selectedPath)) {
                            if (!QFile::rename(stagingPath, selectedPath))
                                return {promoted, QStringLiteral("asset_promotion_failed")};
                            QFile::rename(stagingPath + QStringLiteral(".manifest.json"), selectedPath + QStringLiteral(".manifest.json"));
                        } else if (!QFile::remove(stagingPath)) {
                            return {promoted, QStringLiteral("duplicate_staging_remove_failed")};
                        }
                    }
                    promoted.insert(asset.key(), selectedPath);
                    if (!disk.checkpointAsset(scope, target, selectedPath, hashes.value(asset.key()),
                            sizes.value(asset.key()), sizes.value(asset.key()), extensions.value(asset.key())))
                        return {promoted, QStringLiteral("asset_checkpoint_failed")};
                    if (stagingPath != selectedPath) QFile::remove(stagingPath + QStringLiteral(".manifest.json"));
                }
                return {promoted, {}};
            }));
    } else if (type == "upload_abort") {
        const QString abortedId = message.value("uploadId").toString();
        const QString senderClientId = senderCacheNamespace(message);
        const QString remoteSessionId = message.value("remoteSessionId").toString();
        quint64 generation = 0;
        quint64 sourceConnectionGeneration = 0;
        if (message.value("protocolVersion").toInt(-1)
                != MouffetteProtocol::Version
            || !parsePositiveGeneration(message.value("generation"), generation)
            || !parsePositiveGeneration(message.value("connectionGeneration"),
                                        sourceConnectionGeneration)
            || !RemoteCacheStore::isValidEndpointId(senderClientId)
            || !RemoteCacheStore::isValidSessionId(remoteSessionId)
            || !isValidOpaqueId(abortedId)) {
            return;
        }
        bool cleanupConfirmed = false;
        IncomingUploadSession* incoming =
            m_incomingUploads.value(abortedId, nullptr);
        if (incoming
            && senderClientId == incoming->senderId
            && remoteSessionId == incoming->remoteSessionId
            && generation == incoming->generation
            && sourceConnectionGeneration == incoming->sourceConnectionGeneration) {
            cleanupConfirmed = discardIncomingUpload(abortedId, true);
            if (!cleanupConfirmed && m_validationReadersByUpload.value(abortedId) > 0)
                m_deferredIncomingAborts.insert(abortedId, message);
        } else if (m_incomingUploadCompletionTombstones.contains(abortedId)) {
            cleanupConfirmed = discardCompletedIncomingUpload(message);
        } else if (m_canceledIncoming.contains(abortedId)) {
            cleanupConfirmed = true;
        }
        if (cleanupConfirmed) {
            emitIncomingResponse(QStringLiteral("upload_abort_ack"), senderClientId,
                                 remoteSessionId, generation, abortedId,
                                 {{QStringLiteral("success"), true}});
        }
    }
}

bool UploadManager::discardCompletedIncomingUpload(const QJsonObject& message)
{
    const QString uploadId = message.value(QStringLiteral("uploadId")).toString();
    auto completed = m_incomingUploadCompletionTombstones.find(uploadId);
    const QString sender = senderCacheNamespace(message);
    const QString session = message.value(QStringLiteral("remoteSessionId")).toString();
    const quint64 generation = message.value(QStringLiteral("generation")).toInteger();
    if (completed == m_incomingUploadCompletionTombstones.end()
        || completed->senderEndpointId != sender || completed->remoteSessionId != session
        || completed->generation != generation
        || completed->sourceConnectionGeneration != quint64(
            message.value(QStringLiteral("connectionGeneration")).toInteger())) return false;
    if (m_pendingCompletedIncomingAborts.contains(uploadId)) return false;

    // A control-channel abort can arrive after promotion but before the
    // server receives the data-channel upload_finished. No server inventory
    // exists in that race, so the receiver retains and fulfils the cleanup.
    // Cancel every decoder first: waiting on one asset must never allow the
    // remaining assets in this batch to keep allocating RAM.
    for (qsizetype i = completed->assets.size(); i > 0; --i) {
        const auto asset = completed->assets.at(i - 1).toObject();
        const QString digest = asset.value(QStringLiteral("sha256")).toString();
        const auto resident = m_residentIncoming.constFind(residencyOwnerId(session, generation, digest));
        if (resident != m_residentIncoming.cend() && resident->uploadId != uploadId) {
            completed->assets.removeAt(i - 1);
            continue;
        }
        releaseResidency(session, digest);
    }
    while (!completed->assets.isEmpty()) {
        const auto asset = completed->assets.first().toObject();
        const QString digest = asset.value(QStringLiteral("sha256")).toString();
        const QString owner = residencyOwnerId(session, generation, digest);
        const auto resident = m_residentIncoming.constFind(owner);
        if (resident != m_residentIncoming.cend() && resident->uploadId != uploadId) {
            completed->assets.removeFirst();
            continue;
        }
        const RemoteCacheStore::Scope scope{sender, session, generation};
        const QString mappedPath = m_fileManager->getReceivedFilePath(scope, digest);
        if (!incomingFileReadersSettled({session})) {
            m_deferredIncomingAborts.insert(uploadId, message);
            return false;
        }
        if (mappedPath.isEmpty()) {
            completed->assets.removeFirst();
            continue;
        }
        const QString assetId = asset.value(QStringLiteral("assetId")).toString();
        const QString removalId = QUuid::createUuidV5(
            QUuid(QStringLiteral("4969bfdd-6a71-43b1-bacf-871bd6b22f43")),
            (uploadId + QLatin1Char(':') + assetId).toUtf8()).toString(QUuid::WithoutBraces);
        const qint64 size = asset.value(QStringLiteral("size")).toInteger();
        const RemoteCacheStore::AssetRemovalDescriptor removal{
            removalId, uploadId, assetId, digest, digest, size, size,
            QFileInfo(mappedPath).suffix().toLower()
        };
        m_pendingCompletedIncomingAborts.insert(uploadId);
        ++m_validationReadersBySession[session];
        const QPointer<UploadManager> guard(this);
        const bool queued = m_remoteCacheStore->requestAssetRemoval(scope, removal,
            [guard, scope, uploadId, digest, assetId, message](const RemoteCacheStore::AssetRemovalResult& result) {
                if (!guard) return;
                auto* self = guard.data();
                self->m_pendingCompletedIncomingAborts.remove(uploadId);
                if (--self->m_validationReadersBySession[scope.remoteSessionId] <= 0)
                    self->m_validationReadersBySession.remove(scope.remoteSessionId);
                if (result.acknowledgementSafe()) {
                    self->m_fileManager->removeReceivedFileMapping(scope, digest);
                    self->m_fileManager->dissociateFileFromProject(digest, scope.remoteSessionId);
                    auto tombstone = self->m_incomingUploadCompletionTombstones.find(uploadId);
                    if (tombstone != self->m_incomingUploadCompletionTombstones.end()) {
                        for (qsizetype i = tombstone->assets.size(); i > 0; --i)
                            if (tombstone->assets.at(i - 1).toObject().value(QStringLiteral("assetId")).toString() == assetId)
                                tombstone->assets.removeAt(i - 1);
                    }
                }
                QTimer::singleShot(result.acknowledgementSafe() ? 0 : 250, self,
                    [self, message] { self->handleIncomingMessage(message); });
                self->settleIncomingFileReaders();
            });
        if (!queued) {
            m_pendingCompletedIncomingAborts.remove(uploadId);
            if (--m_validationReadersBySession[session] <= 0) m_validationReadersBySession.remove(session);
            QTimer::singleShot(250, this, [this, message] { handleIncomingMessage(message); });
        }
        return false;
    }
    m_incomingUploadCompletionTombstones.erase(completed);
    m_canceledIncoming.insert(uploadId);
    publishResidency(session);
    return true;
}

void UploadManager::finishIncomingPromotion(const QString& uploadId, const QString& senderId,
    const QString& remoteSessionId, quint64 generation, quint64 sourceConnectionGeneration,
    const QHash<QString, QString>& selectedPathByAsset, const QHash<QString, QString>& selectedPathByFileId)
{
    auto* incomingSession = m_incomingUploads.value(uploadId);
    if (!incomingSession || incomingSession->generation != generation
        || incomingSession->sourceConnectionGeneration != sourceConnectionGeneration
        || incomingSession->suspendedForResume) return;
    IncomingUploadSession& incoming = *incomingSession;
    const RemoteCacheStore::Scope scope{senderId, remoteSessionId, generation};
    QString completionError;
        for (auto it = selectedPathByFileId.constBegin();
             it != selectedPathByFileId.constEnd(); ++it) {
            const QString existingPath =
                m_fileManager->getReceivedFilePath(scope, it.key());
            if (existingPath.isEmpty()
                && !m_fileManager->registerReceivedFilePath(
                    scope, it.key(), it.value())) {
                completionError = QStringLiteral(
                    "Remote client could not register a validated asset");
                break;
            }
            const QString registeredCanonical = QFileInfo(
                m_fileManager->getReceivedFilePath(scope, it.key())).canonicalFilePath();
            const QString selectedCanonical = QFileInfo(it.value()).canonicalFilePath();
            if (registeredCanonical.isEmpty() || selectedCanonical.isEmpty()
                || QDir::cleanPath(registeredCanonical) != QDir::cleanPath(selectedCanonical)) {
                completionError = QStringLiteral("Remote client could not register a validated asset");
                break;
            }
            m_fileManager->associateFileWithProject(it.key(), remoteSessionId);
        }
        if (!completionError.isEmpty()) {
            rejectIncomingUpload(senderId, uploadId, completionError, true,
                                 remoteSessionId, generation);
            return;
        }

        // Transfer completion and decoded-memory readiness are independent barriers.
        for (const QString& assetId : selectedPathByAsset.keys()) {
            const QString sha256 = incoming.assetIdToSha256.value(assetId);
            const QString owner = residencyOwnerId(remoteSessionId, generation, sha256);
            m_residentIncoming.insert(owner, {remoteSessionId, generation, assetId, sha256,
                                             selectedPathByAsset.value(assetId), uploadId});
        }
        const QJsonArray completedAssets = incomingAssetOffsets(incoming);
        const QString completedStagingPath = incoming.cacheDirPath;
        rememberIncomingUploadCompletion(
            senderId, remoteSessionId, generation,
            sourceConnectionGeneration, uploadId, completedAssets);
        if (incoming.stallTimer) {
            incoming.stallTimer->stop();
            incoming.stallTimer->deleteLater();
        }
        clearIncomingChunkTracking(uploadId);
        m_canceledIncoming.remove(uploadId);
        m_incomingUploads.remove(uploadId);
        delete incomingSession;
        QDir().rmdir(completedStagingPath);
        emitIncomingResponse(QStringLiteral("upload_finished"), senderId,
                             remoteSessionId, generation, uploadId,
                             {{QStringLiteral("assets"), completedAssets}});
        // Queue acquire until upload_finished is sent, so the server inventory exists.
        for (auto it = m_residentIncoming.cbegin(); it != m_residentIncoming.cend(); ++it) {
            if (it->sessionId != remoteSessionId || it->generation != generation
                || !selectedPathByAsset.contains(it->assetId)) continue;
            MediaResidencyManager::instance().acquire(it.key(),
                selectedPathByAsset.value(it->assetId), it->sha256);
            MediaResidencyManager::instance().retry(it.key());
        }
        publishResidency(remoteSessionId);
}

bool UploadManager::canAcceptNewAction() const {
    if (m_lastAcceptedAction.isValid()
        && m_lastAcceptedAction.elapsed()
            < AppConfig::instance().uploadActionMinIntervalMs()) {
        return false;
    }
    for (auto it = m_pendingAssetRemovals.cbegin();
         it != m_pendingAssetRemovals.cend(); ++it) {
        if (it->asset.targetEndpointId == m_targetClientId) return false;
    }
    if (parallelForTarget(m_targetClientId)) return false;
    if (m_outgoingRamByTarget.contains(m_targetClientId)) return false;
    return m_currentUploadId.isEmpty()
        || m_uploadTargetClientId != m_targetClientId;
}

void UploadManager::recordAcceptedAction() {
    m_lastAcceptedAction.restart();
}

bool UploadManager::canRequestCancel() const {
    if (m_outgoingRamByTarget.contains(m_targetClientId)) return true;
    if (m_pendingUploadVerification.contains(m_targetClientId)
        || m_waitingVerifiedUploads.contains(m_targetClientId)) return true;
    if (ParallelOutgoingTransfer* transfer = parallelForTarget(m_targetClientId)) {
        return (transfer->state == OutgoingState::Queued
                || transfer->state == OutgoingState::AwaitingTargetReady
                || transfer->state == OutgoingState::Streaming
                || transfer->state == OutgoingState::AwaitingValidation
                || transfer->state == OutgoingState::Suspended);
    }
    return m_uploadTargetClientId == m_targetClientId
        && (m_outgoingState == OutgoingState::Queued
            || m_outgoingState == OutgoingState::AwaitingTargetReady
            || m_outgoingState == OutgoingState::Streaming
            || m_outgoingState == OutgoingState::AwaitingValidation
            || m_outgoingState == OutgoingState::Suspended);
}

void UploadManager::restartIncomingStallTimer(
    IncomingUploadSession& incoming) {
    if (incoming.stallTimer && incoming.stallTimer->interval() > 0
        && !incoming.uploadId.isEmpty()) {
        incoming.stallTimer->start();
    }
}
