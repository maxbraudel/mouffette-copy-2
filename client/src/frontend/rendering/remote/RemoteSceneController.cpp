#include "backend/media/MediaResidencyManager.h"
#include "backend/media/ResidentVideoPlayer.h"
#include "backend/media/TimelineVideoPlayback.h"
#include "backend/network/UploadManager.h"
#include "frontend/rendering/remote/RemoteSceneController.h"
#include "backend/config/AppConfig.h"
#include "backend/network/SceneRunCoordinator.h"
#include "backend/network/WebSocketClient.h"
#include "backend/domain/media/MediaFilePolicy.h"
#include "frontend/rendering/canvas/CanvasQmlTypes.h"
#include "frontend/rendering/canvas/MediaListModel.h"
#include "frontend/qml/QmlRuntime.h"
#include "frontend/rendering/remote/RemoteVideoFrameItem.h"
#include "frontend/ui/notifications/ToastNotificationSystem.h"
#include <QJsonArray>
#include <QScreen>
#include <QGuiApplication>
#include <QFileInfo>
#include <QFile>
#include <QDebug>
#include <QVideoFrame>
#include <QAudioOutput>
#include <QMediaPlayer>
#include <QVideoSink>
#include <QMetaObject>
#include <QUrl>
#include <QThread>
#include <QQuickWindow>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QQuickItem>
#include <QPointer>
#include <QHash>
#include <QSet>
#include <QVariant>
#include <QDateTime>
#include <cmath>
#include "backend/files/FileManager.h"
#include "backend/platform/macos/MacWindowManager.h"
#include "backend/platform/WindowStackingCoordinator.h"
#include <algorithm>
#include <chrono>
#include <limits>
#include <memory>
#include <utility>
#include <QVideoFrameFormat>
#include <QTransform>


namespace {
constexpr int kMaxRemoteScreens = 64;
constexpr int kMaxRemoteMediaItems = 512;
constexpr int kMaxRemoteIdentifierLength = 128;
constexpr qint64 kMaxSafeJsonInteger = 9007199254740991LL;

bool readFiniteNumber(const QJsonObject& object, const char* key, double& value) {
    const QJsonValue candidate = object.value(QLatin1String(key));
    if (!candidate.isDouble()) return false;
    value = candidate.toDouble();
    return std::isfinite(value);
}

bool readBoundedInteger(const QJsonObject& object,
                        const char* key,
                        int minimum,
                        int maximum,
                        int& value) {
    double numeric = 0.0;
    if (!readFiniteNumber(object, key, numeric)
        || std::floor(numeric) != numeric
        || numeric < static_cast<double>(minimum)
        || numeric > static_cast<double>(maximum)) {
        return false;
    }
    value = static_cast<int>(numeric);
    return true;
}

bool readBoundedInt64(const QJsonObject& object,
					  const char* key,
					  qint64 minimum,
					  qint64 maximum,
					  qint64& value) {
	double numeric = 0.0;
	if (!readFiniteNumber(object, key, numeric)
		|| std::floor(numeric) != numeric
		|| numeric < static_cast<double>(minimum)
		|| numeric > static_cast<double>(maximum)) {
		return false;
	}
	value = static_cast<qint64>(numeric);
	return true;
}

qint64 localSteadyMilliseconds()
{
	using namespace std::chrono;
	return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}



QImage convertFrameToImage(const QVideoFrame& frame) {
    if (!frame.isValid()) {
        return {};
    }

    const auto orient = [&frame](QImage image) {
        if (image.isNull()) return image;
        const int rotation = static_cast<int>(frame.rotation());
        if (rotation) image = image.transformed(QTransform().rotate(rotation));
        if (frame.mirrored()) image = image.flipped(Qt::Horizontal);
        return image;
    };
    QImage direct = frame.toImage();
    if (!direct.isNull()) {
        if (direct.format() != QImage::Format_RGBA8888 && direct.format() != QImage::Format_ARGB32_Premultiplied) {
            direct = direct.convertToFormat(QImage::Format_RGBA8888);
        }
        return orient(direct);
    }

    QVideoFrame copy(frame);
    if (!copy.isValid()) {
        return {};
    }

    if (!copy.map(QVideoFrame::ReadOnly)) {
        return {};
    }

    QImage mapped;
    const QVideoFrameFormat format = copy.surfaceFormat();
    const int width = format.frameWidth();
    const int height = format.frameHeight();
    const int stride = copy.bytesPerLine(0);
    const QImage::Format imgFormat = QVideoFrameFormat::imageFormatFromPixelFormat(format.pixelFormat());
    if (imgFormat != QImage::Format_Invalid && width > 0 && height > 0 && stride > 0) {
        mapped = QImage(copy.bits(0), width, height, stride, imgFormat).copy();
    }

    copy.unmap();

    if (!mapped.isNull() && mapped.format() != QImage::Format_RGBA8888 && mapped.format() != QImage::Format_ARGB32_Premultiplied) {
        mapped = mapped.convertToFormat(QImage::Format_RGBA8888);
    }

    return orient(mapped);
}

} // namespace

RemoteSceneController::RemoteSceneController(FileManager* fileManager, WebSocketClient* ws, QObject* parent)
    : QObject(parent)
    , m_fileManager(fileManager)
    , m_ws(ws) {
    m_screenRefreshTimer.setSingleShot(true);
    m_screenRefreshTimer.setInterval(0);
    connect(&m_screenRefreshTimer, &QTimer::timeout, this, [this] {
        refreshScreenBindings(LocalScreenTopology::screens());
    });
    for (QScreen* screen : QGuiApplication::screens()) watchLocalScreen(screen);
    connect(qGuiApp, &QGuiApplication::screenAdded, this, [this](QScreen* screen) {
        watchLocalScreen(screen);
        m_screenRefreshTimer.start();
    });
    connect(qGuiApp, &QGuiApplication::screenRemoved, this, [this](QScreen* screen) {
        // Direct connection on the GUI thread: hide before Qt migrates the
        // removed screen's windows to the primary display.
        handleLocalScreenRemoved(screen);
        m_screenRefreshTimer.start();
    });
    connect(&MediaResidencyManager::instance(), &MediaResidencyManager::ownerChanged,
            this, [this](const QString& owner) {
        if (m_teardownInProgress || m_pendingSceneInstanceId.isEmpty()) return;
        for (const auto& item : m_mediaItems) {
            if (item->residencyOwner != owner || MediaResidencyManager::instance().ready(owner)) continue;
            const QString sender = m_pendingSenderClientId;
            const QString run = m_pendingSceneInstanceId;
            if (m_ws) m_ws->sendSceneStop(run, QStringLiteral("scene_memory_unavailable"));
            onRemoteSceneStop(sender, run);
            return;
        }
    });
    connect(&MediaResidencyManager::instance(), &MediaResidencyManager::sceneStopRequested,
            this, [this](const QString& group) {
        if (group.isEmpty() || group != m_residencyGroup) return;
        const QString sender = m_pendingSenderClientId;
        const QString run = m_pendingSceneInstanceId;
        if (m_ws) m_ws->sendSceneStop(run, QStringLiteral("memory_pressure"));
        onRemoteSceneStop(sender, run);
    });
    m_timelineTimer.setTimerType(Qt::PreciseTimer);
    m_timelineTimer.setInterval(16);
    connect(&m_timelineTimer, &QTimer::timeout, this, &RemoteSceneController::advanceTimeline);
    if (m_ws) {
        connect(m_ws, &WebSocketClient::scenePrepareReceived,
                this, &RemoteSceneController::onScenePrepareEnvelope);
        connect(m_ws, &WebSocketClient::scenePreparedReceived,
                this, &RemoteSceneController::onScenePreparedEnvelope);
        connect(m_ws, &WebSocketClient::sceneCommitReceived,
                this, &RemoteSceneController::onSceneCommitEnvelope);
        connect(m_ws, &WebSocketClient::sceneStateSnapshotReceived,
                this, &RemoteSceneController::onSceneStateSnapshotEnvelope);
        connect(m_ws, &WebSocketClient::sceneStopReceived,
                this, &RemoteSceneController::onSceneStopEnvelope);
        connect(m_ws, &WebSocketClient::sceneStoppedReceived,
                this, &RemoteSceneController::onSceneStoppedEnvelope);
        connect(m_ws, &WebSocketClient::sceneErrorReceived,
                this, &RemoteSceneController::onSceneErrorEnvelope);
        connect(m_ws, &WebSocketClient::remoteSessionResumed,
                this, &RemoteSceneController::onRemoteSessionResumedEnvelope);
        connect(m_ws, &WebSocketClient::heartbeatSampleReceived,
                this, [this](quint64, qint64, qint64, qint64) {
                    // PREPARED can precede the first sufficiently precise
                    // sample (especially just after authentication/resume).
                    // ARMED is idempotent, so retry the pending barrier when
                    // the synchronization burst publishes each new sample.
                    tryArmPreparedScene();
                    // A compositor frame may land while the latest network
                    // sample is temporarily outside policy. Keep the observed
                    // presentation time and retry as soon as clock quality
                    // recovers instead of silently timing out the live scene.
                    if (m_sceneActivated
                        && m_screensAwaitingFirstFrame.isEmpty()
                        && m_firstFramePresentedLocalSteadyMs >= 0
                        && !m_firstFrameReported) {
                        sendFirstFramePresented();
                    }
                });
        connect(m_ws, &WebSocketClient::sessionsInvalidated, this,
                [this](const QString&, const QString&, quint64) { onConnectionLost(); });
        connect(m_ws, &WebSocketClient::remoteSessionAbsent, this,
                [this](const QString& id, quint64) {
            if (id == m_pendingRemoteSessionId) onConnectionLost();
        });
        connect(m_ws, &WebSocketClient::remoteSessionRecoveryExpired, this,
                [this](const QString& id, quint64) {
            if (id == m_pendingRemoteSessionId) onConnectionLost();
        });
        connect(m_ws, &WebSocketClient::leaseExpired,
                this, &RemoteSceneController::onConnectionLost,
                Qt::UniqueConnection);
    }
}

RemoteCacheStore::Scope RemoteSceneController::receivedFileScope() const
{
    return {m_pendingSenderClientId, m_pendingRemoteSessionId,
            m_pendingSessionGeneration};
}

QString RemoteSceneController::receivedFilePath(const QString& fileId) const
{
    if (!m_fileManager) return {};
    const RemoteCacheStore::Scope scope = receivedFileScope();
    if (scope.senderEndpointId.isEmpty() || scope.remoteSessionId.isEmpty()
        || scope.generation == 0) {
        // Production starts always arrive through a protocol v4 envelope and
        // must fail closed when its scope is incomplete. Tests without a
        // transport resolve their explicitly registered local fixture.
        return m_ws ? QString() : m_fileManager->getFilePathForId(fileId);
    }
    return m_fileManager->getReceivedFilePath(scope, fileId);
}

RemoteSceneController::~RemoteSceneController() {
    clearScene();
    for (auto it = m_screenWindows.begin(); it != m_screenWindows.end(); ++it) {
        ScreenWindow& sw = it.value();
        if (sw.window) {
            sw.window->deleteLater();
            sw.window = nullptr;
        }
    }
    m_screenWindows.clear();
}

bool RemoteSceneController::teardownRemoteSession(const QString& remoteSessionId)
{
    if (remoteSessionId.isEmpty() || QThread::currentThread() != thread()) return false;

    const bool ownsRequestedSession = m_pendingRemoteSessionId == remoteSessionId;
    if (m_teardownInProgress) {
        if (m_teardownGraphRemoteSessionId != remoteSessionId) return false;
        m_teardownSessionWaiters.insert(remoteSessionId);
        scheduleTeardownBarrierCompletion();
        return true;
    }

    if (!ownsRequestedSession) {
        // Session X owns no renderer graph. Its cleanup is therefore already
        // settled, even when an unrelated session Y currently owns the single
        // target-wide scene. Never touch Y while acknowledging X.
        if (m_pendingEmptySessionTeardowns.contains(remoteSessionId)) {
            return true;
        }
        m_pendingEmptySessionTeardowns.insert(remoteSessionId);
        QMetaObject::invokeMethod(this, [this, remoteSessionId]() {
            m_pendingEmptySessionTeardowns.remove(remoteSessionId);
            emit teardownSettled(remoteSessionId, true);
        }, Qt::QueuedConnection);
        return true;
    }

    m_teardownSessionWaiters.insert(remoteSessionId);
    m_teardownGraphRemoteSessionId = remoteSessionId;
    m_deferredSceneStart.valid = false;
    m_startingSenderClientId.clear();
    m_startingSceneInstanceId.clear();
    ++m_sceneEpoch;
    clearScene();
    return true;
}

void RemoteSceneController::resetSceneSynchronization() {
    m_timelineTimer.stop();
    m_timelineClock.invalidate();
    m_timelineStartServerMs = -1;
    m_timelineAnchorMs = 0;
    m_timelinePositionMs = 0;
    m_timelineFinished = false;
    m_batchTimelinePublishing = false;
    m_dirtyTimelineScreens.clear();
	disconnectFirstFrameObservers();
    m_screensAwaitingFirstFrame.clear();
    if (m_sceneReadyTimeout) {
        m_sceneReadyTimeout->stop();
        QObject::disconnect(m_sceneReadyTimeout, nullptr, this, nullptr);
        if (m_teardownInProgress) trackTeardownObjectTree(m_sceneReadyTimeout);
        m_sceneReadyTimeout->deleteLater();
        m_sceneReadyTimeout = nullptr;
    }
    if (m_activationTimer) {
        m_activationTimer->stop();
        QObject::disconnect(m_activationTimer, nullptr, this, nullptr);
        if (m_teardownInProgress) trackTeardownObjectTree(m_activationTimer);
        m_activationTimer->deleteLater();
        m_activationTimer = nullptr;
    }
    m_pendingSenderClientId.clear();
    m_pendingSceneInstanceId.clear();
    m_totalMediaToPrime = 0;
    m_mediaReadyCount = 0;
    m_sceneActivationRequested = false;
    m_sceneActivated = false;
	m_sceneAllPrepared = false;
	m_sceneCommitReceived = false;
	m_committedActivationLeadMs = 0;
	m_firstFrameReported = false;
	m_firstFramePresentedServerMonotonicMs = -1;
	m_firstFramePresentedLocalSteadyMs = -1;
    m_activationEpochMs = 0;
    m_activationClockPlausible = false;
    m_lastVideoSyncSequence = 0;
    if (!m_sceneStartInProgress) {
        m_pendingSceneDigest.clear();
        m_pendingRemoteSessionId.clear();
        m_pendingSessionGeneration = 0;
        m_pendingSceneRevision = 0;
        m_prepareChecklist = {};
        m_scenePreparedReported = false;
        m_sceneArmedReported = false;
    }
}

bool RemoteSceneController::matchesSceneEnvelope(const QJsonObject& envelope) const
{
	qint64 generation = -1;
    return !m_pendingSceneInstanceId.isEmpty()
		&& readBoundedInt64(envelope, "generation", 1,
							kMaxSafeJsonInteger, generation)
        && envelope.value(QStringLiteral("sceneRunId")).toString() == m_pendingSceneInstanceId
        && envelope.value(QStringLiteral("digest")).toString() == m_pendingSceneDigest
        && envelope.value(QStringLiteral("remoteSessionId")).toString() == m_pendingRemoteSessionId
		&& static_cast<quint64>(generation) == m_pendingSessionGeneration;
}

void RemoteSceneController::onScenePrepareEnvelope(const QJsonObject& envelope)
{
    if (!m_enabled || !m_ws
        || envelope.value(QStringLiteral("targetEndpointId")).toString() != m_ws->endpointId()) return;
    const QString runId = envelope.value(QStringLiteral("sceneRunId")).toString();
	qint64 generation = -1;
	qint64 revision = -1;
	if (runId.isEmpty()
		|| !readBoundedInt64(envelope, "generation", 1,
							kMaxSafeJsonInteger, generation)
		|| !readBoundedInt64(envelope, "revision", 1,
							kMaxSafeJsonInteger, revision)) {
		if (!runId.isEmpty()) {
			m_ws->sendScenePrepared(
				runId, false, {}, QStringLiteral("scene_prepare_failed"),
				QStringLiteral("Scene correlation contains an invalid integer"));
		}
		return;
	}
    if ((!m_pendingSceneInstanceId.isEmpty() && m_pendingSceneInstanceId != runId)
        || (!m_startingSceneInstanceId.isEmpty() && m_startingSceneInstanceId != runId)) {
        m_ws->sendScenePrepared(runId, false, {}, QStringLiteral("scene_target_busy"),
                                QStringLiteral("Target is already preparing or presenting another scene"));
        return;
    }

    const QJsonObject incomingScene = envelope.value(QStringLiteral("scene")).toObject();
    QHash<QString, QString> declaredFileByMedia;
    QHash<QString, QString> declaredAssetByMedia;
    for (const QJsonValue& value : envelope.value(QStringLiteral("manifest")).toArray()) {
        const QJsonObject asset = value.toObject();
        const QString assetId = asset.value(QStringLiteral("assetId")).toString();
        const QString fileId = asset.value(QStringLiteral("fileId")).toString();
        for (const QJsonValue& mediaId : asset.value(QStringLiteral("mediaIds")).toArray()) {
            const QString id = mediaId.toString();
            if (declaredFileByMedia.contains(id)) {
                m_ws->sendScenePrepared(runId, false, {},
                                        QStringLiteral("scene_manifest_mismatch"),
                                        QStringLiteral("A media id occurs in multiple assets"));
                return;
            }
            declaredFileByMedia.insert(id, fileId);
            declaredAssetByMedia.insert(id, assetId);
        }
    }
    QSet<QString> referencedMedia;
    for (const QJsonValue& value : incomingScene.value(QStringLiteral("media")).toArray()) {
        const QJsonObject media = value.toObject();
        if (media.value(QStringLiteral("type")).toString() == QLatin1String("text")) continue;
        const QString mediaId = media.value(QStringLiteral("mediaId")).toString();
        if (declaredFileByMedia.value(mediaId)
                != media.value(QStringLiteral("fileId")).toString()
            || declaredAssetByMedia.value(mediaId)
                != media.value(QStringLiteral("assetId")).toString()) {
            m_ws->sendScenePrepared(runId, false, {},
                                    QStringLiteral("scene_manifest_mismatch"),
                                    QStringLiteral("Scene media does not match its validated asset"));
            return;
        }
        const QString memoryOwner = UploadManager::residencyOwnerId(
            envelope.value(QStringLiteral("remoteSessionId")).toString(), generation,
            media.value(QStringLiteral("fileId")).toString());
        if (!MediaResidencyManager::instance().ready(memoryOwner)) {
            m_ws->sendScenePrepared(runId, false, {}, QStringLiteral("scene_memory_unavailable"),
                QStringLiteral("Every media must be validated and resident in memory before preparation"));
            return;
        }
        referencedMedia.insert(mediaId);
    }
    for (auto iterator = declaredFileByMedia.cbegin();
         iterator != declaredFileByMedia.cend(); ++iterator) {
        if (!referencedMedia.contains(iterator.key())) {
            m_ws->sendScenePrepared(runId, false, {},
                                    QStringLiteral("scene_manifest_mismatch"),
                                    QStringLiteral("Manifest contains an unreferenced media id"));
            return;
        }
    }

    m_pendingSenderClientId =
        envelope.value(QStringLiteral("ownerEndpointId")).toString();
    m_pendingRemoteSessionId = envelope.value(QStringLiteral("remoteSessionId")).toString();
	m_pendingSessionGeneration = static_cast<quint64>(generation);
	m_pendingSceneRevision = static_cast<quint64>(revision);
    m_pendingSceneDigest = envelope.value(QStringLiteral("digest")).toString();
    m_scenePreparedReported = false;
    m_sceneAllPrepared = false;
    m_sceneArmedReported = false;
    m_sceneCommitReceived = false;
    m_committedActivationLeadMs = 0;
    m_firstFrameReported = false;
	m_firstFramePresentedServerMonotonicMs = -1;
	m_firstFramePresentedLocalSteadyMs = -1;

    QJsonObject scene = incomingScene;
    scene.insert(QStringLiteral("sceneInstanceId"), runId);
    m_prepareChecklist = SceneRunCoordinator::createLocalChecklist(scene);
    // Clock acquisition is independent from renderer preparation. Starting a
    // bounded probe burst now normally makes the mapping ready by PREPARED.
    m_ws->requestSceneClockSynchronization();
    for (qsizetype index = 0; index < m_prepareChecklist.size(); ++index) {
        QJsonObject item = m_prepareChecklist.at(index).toObject();
        item.insert(QStringLiteral("ready"), false);
        m_prepareChecklist.replace(index, item);
    }
    onRemoteSceneStart(envelope.value(QStringLiteral("ownerEndpointId")).toString(), scene);

    if (m_pendingSceneInstanceId != runId) {
        if (m_deferredSceneStart.valid) return;
        m_pendingSenderClientId.clear();
        m_pendingSceneDigest.clear();
        m_pendingRemoteSessionId.clear();
        m_pendingSessionGeneration = 0;
        m_pendingSceneRevision = 0;
        m_prepareChecklist = {};
        return;
    }

    if (!remoteRenderGraphsReady()) {
        sendPrepareResult(false, QStringLiteral(
            "Qt Quick remote renderer did not produce a ready root object"));
        ++m_sceneEpoch;
        clearScene();
        return;
    }

    // Native/QML screen windows are now constructed but remain hidden.
    for (qsizetype index = 0; index < m_prepareChecklist.size(); ++index) {
        QJsonObject item = m_prepareChecklist.at(index).toObject();
        if (item.value(QStringLiteral("stage")).toString()
            == QLatin1String("screen_render_graph_ready")) {
            item.insert(QStringLiteral("ready"), true);
            m_prepareChecklist.replace(index, item);
        }
    }
    updatePrepareProgress();
    if (m_sceneActivationRequested
        && (m_totalMediaToPrime == 0 || m_mediaReadyCount >= m_totalMediaToPrime)) {
        sendPrepareResult(true);
    }
}

bool RemoteSceneController::remoteRenderGraphsReady() const
{
    if (m_screenWindows.isEmpty()) return false;
    for (auto it = m_screenWindows.cbegin(); it != m_screenWindows.cend(); ++it) {
        const ScreenWindow& window = it.value();
        if (!window.window || !window.mediaModel || !window.targetScreen) {
            return false;
        }
    }
    return true;
}

void RemoteSceneController::onScenePreparedEnvelope(const QJsonObject& envelope)
{
    if (!matchesSceneEnvelope(envelope) || !m_scenePreparedReported
        || !envelope.value(QStringLiteral("allPrepared")).toBool(false)
        || !m_ws) return;
    m_sceneAllPrepared = true;
    tryArmPreparedScene();
}

void RemoteSceneController::onSceneCommitEnvelope(const QJsonObject& envelope)
{
    if (!matchesSceneEnvelope(envelope) || !m_sceneArmedReported || !m_ws) return;
    for (const auto& item : m_mediaItems) {
        if (item->type != QLatin1String("text")
            && !MediaResidencyManager::instance().ready(item->residencyOwner)) {
            m_ws->sendSceneStop(m_pendingSceneInstanceId, QStringLiteral("scene_memory_unavailable"));
            clearScene();
            return;
        }
    }
	qint64 startServerMonotonicMs = -1;
	qint64 maximum = -1;
	qint64 startEpochMs = -1;
	qint64 policyMaximum = -1;
	qint64 activationLeadMs = -1;
	qint64 policyActivationLeadMs = -1;
	if (!readBoundedInt64(envelope, "startServerMonotonicMs",
						0, kMaxSafeJsonInteger, startServerMonotonicMs)
		|| !readBoundedInt64(envelope, "maximumClockUncertaintyMs",
						0, kMaxSafeJsonInteger, maximum)
		|| !readBoundedInt64(envelope, "startEpochMs",
						1, kMaxSafeJsonInteger, startEpochMs)
		|| !readBoundedInt64(envelope, "activationLeadMs",
						1, std::numeric_limits<int>::max(), activationLeadMs)
		|| !readBoundedInt64(m_ws->serverPolicy(), "sceneMaxClockSkewMs",
						0, kMaxSafeJsonInteger, policyMaximum)
		|| !readBoundedInt64(m_ws->serverPolicy(), "sceneActivationLeadMs",
						1, std::numeric_limits<int>::max(), policyActivationLeadMs)
		|| maximum != policyMaximum
		|| activationLeadMs > policyActivationLeadMs) {
		sendPrepareResult(false, QStringLiteral("Invalid synchronized scene commitment"));
		return;
	}
    const qint64 serverNow = m_ws->estimatedServerMonotonicMs();
	const qint64 localUncertainty = m_ws->sceneClockUncertaintyMs();
    if (serverNow < 0 || localUncertainty < 0 || localUncertainty > maximum) {
        sendPrepareResult(false, QStringLiteral("Invalid synchronized scene commitment"));
        return;
    }
	const qint64 remaining = startServerMonotonicMs - serverNow;
	if (remaining < -maximum
		|| remaining > activationLeadMs + maximum
		|| remaining > std::numeric_limits<int>::max()) {
        sendPrepareResult(false, QStringLiteral("Scene commitment missed its activation window"));
        return;
    }
    m_sceneCommitReceived = true;
    m_timelineStartServerMs = startServerMonotonicMs;
    m_committedActivationLeadMs = activationLeadMs;
    onRemoteSceneActivate(
        m_pendingSenderClientId,
        m_pendingSceneInstanceId,
		startEpochMs,
        static_cast<int>(std::max<qint64>(0, remaining)));
}

void RemoteSceneController::onSceneStateSnapshotEnvelope(const QJsonObject& envelope)
{
    if (!matchesSceneEnvelope(envelope) || !m_ws || !m_sceneActivated) return;
    const QJsonValue snapshotValue = envelope.value(QStringLiteral("snapshot"));
    const QJsonValue sequenceValue = envelope.value(QStringLiteral("sequence"));
    const QJsonValue sampledValue =
        envelope.value(QStringLiteral("sampledServerMonotonicMs"));
    const double rawSequence = sequenceValue.toDouble(-1.0);
    const double rawSampled = sampledValue.toDouble(-1.0);
    if (!snapshotValue.isObject() || !sequenceValue.isDouble()
        || !sampledValue.isDouble() || !std::isfinite(rawSequence)
        || std::floor(rawSequence) != rawSequence || rawSequence < 1.0
        || rawSequence > 9007199254740991.0 || !std::isfinite(rawSampled)
        || std::floor(rawSampled) != rawSampled || rawSampled < 0.0
        || rawSampled > 9007199254740991.0) {
        qWarning() << "RemoteSceneController: rejecting malformed state_snapshot envelope";
        return;
    }
    const qint64 sampledServer = static_cast<qint64>(rawSampled);
    const qint64 nowServer = m_ws->estimatedServerMonotonicMs();
	qint64 policyClockSkewMs = -1;
	qint64 policyLeaseMs = -1;
	const qint64 clockUncertaintyMs = m_ws->sceneClockUncertaintyMs();
	if (nowServer < 0
		|| !readBoundedInt64(m_ws->serverPolicy(), "sceneMaxClockSkewMs",
							0, kMaxSafeJsonInteger, policyClockSkewMs)
		|| !readBoundedInt64(m_ws->serverPolicy(), "leaseTimeoutMs",
							1, kMaxSafeJsonInteger, policyLeaseMs)
		|| clockUncertaintyMs < 0 || clockUncertaintyMs > policyClockSkewMs) {
        qWarning() << "RemoteSceneController: rejecting state_snapshot without a synchronized clock";
        return;
    }
	if (sampledServer > nowServer + clockUncertaintyMs) {
		qWarning() << "RemoteSceneController: rejecting state_snapshot sampled in the future";
		return;
	}
	const qint64 sampleAge = std::max<qint64>(0, nowServer - sampledServer);
	if (sampleAge > policyLeaseMs + clockUncertaintyMs) {
		qWarning() << "RemoteSceneController: rejecting state_snapshot older than the session lease";
		return;
	}
    applyAuthoritativeStateSnapshot(
        snapshotValue.toObject(), static_cast<quint64>(rawSequence), sampleAge);
}

bool RemoteSceneController::applyAuthoritativeStateSnapshot(
    const QJsonObject& snapshot, quint64 sequence, qint64 sampleAgeMs)
{
    const auto reject = [this, sequence](const QString& reason) {
        emit authoritativeSnapshotRejected(sequence, reason);
        return false;
    };
    qint64 position = 0;
    if (!m_enabled || m_pendingSceneInstanceId.isEmpty() || m_teardownInProgress)
        return reject(QStringLiteral("No active renderer graph"));
    if (sequence < 1 || sequence > 9007199254740991ULL
        || sequence <= static_cast<quint64>(m_lastVideoSyncSequence))
        return reject(QStringLiteral("Snapshot sequence is stale"));
    if (sampleAgeMs < 0 || sampleAgeMs > kMaxSafeJsonInteger
        || snapshot.size() != 1
        || !readBoundedInt64(snapshot, "timelinePositionMs", 0,
                            m_timelineSettings.effectiveStopMs(), position))
        return reject(QStringLiteral("Invalid timeline snapshot"));
    m_lastVideoSyncSequence = static_cast<qint64>(sequence);
    // The immutable COMMIT remains authoritative. This anchor only supplies a
    // bounded fallback while a fresh clock estimate is being acquired.
    m_timelineAnchorMs = std::min(m_timelineSettings.effectiveStopMs(), position + sampleAgeMs);
    m_timelineClock.start();
    if (m_sceneActivated && !m_timelineFinished) advanceTimeline();
    emit authoritativeSnapshotApplied(sequence);
    return true;
}

void RemoteSceneController::onSceneStopEnvelope(const QJsonObject& envelope)
{
    if (!matchesSceneEnvelope(envelope)) return;
    onRemoteSceneStop(m_pendingSenderClientId, m_pendingSceneInstanceId);
}

void RemoteSceneController::onSceneStoppedEnvelope(const QJsonObject& envelope)
{
    const QString runId = envelope.value(QStringLiteral("sceneRunId")).toString();
    if (runId.isEmpty() || (runId != m_pendingSceneInstanceId
                            && runId != m_lastStoppedSceneInstanceId)) return;
    if (runId == m_pendingSceneInstanceId) clearScene();
}

void RemoteSceneController::onSceneErrorEnvelope(const QJsonObject& envelope)
{
    if (!matchesSceneEnvelope(envelope)) return;
    qWarning() << "Remote scene protocol error:"
               << envelope.value(QStringLiteral("code")).toString();
    ++m_sceneEpoch;
    clearScene();
}

void RemoteSceneController::onRemoteSessionResumedEnvelope(const QJsonObject& envelope)
{
    if (envelope.value(QStringLiteral("remoteSessionId")).toString()
        != m_pendingRemoteSessionId) return;
	qint64 generation = -1;
	if (!readBoundedInt64(envelope, "generation", 1,
						kMaxSafeJsonInteger, generation)) {
		return;
	}
	m_pendingSessionGeneration = static_cast<quint64>(generation);
	if (m_sceneAllPrepared && !m_sceneCommitReceived) {
		// The previous socket may have disappeared after ARMED was merely queued
		// locally. Re-establish the clock map and replay the idempotent command on
		// the rebound RemoteSession generation.
		m_sceneArmedReported = false;
		if (m_ws) m_ws->requestSceneClockSynchronization();
		tryArmPreparedScene();
	}
	// A frame can genuinely reach the compositor while the transport is in its
	// lease grace period. Re-send that exact observation after resumption; never
	// manufacture a new presentation time merely because the socket returned.
	if (m_sceneActivated && m_screensAwaitingFirstFrame.isEmpty()
		&& m_firstFramePresentedLocalSteadyMs >= 0) {
		sendFirstFramePresented(true);
	}
}

void RemoteSceneController::sendPrepareResult(bool success, const QString& detail)
{
    if (!m_ws || m_pendingSceneInstanceId.isEmpty()) return;
    if (success) {
        if (m_scenePreparedReported) return;
        for (const QJsonValue& value : std::as_const(m_prepareChecklist)) {
            if (!value.toObject().value(QStringLiteral("ready")).toBool(false)) return;
        }
        m_scenePreparedReported = m_ws->sendScenePrepared(
            m_pendingSceneInstanceId, true, m_prepareChecklist);
    } else {
        m_ws->sendScenePrepared(
            m_pendingSceneInstanceId, false, m_prepareChecklist,
            QStringLiteral("scene_prepare_failed"), detail);
    }
}

void RemoteSceneController::tryArmPreparedScene()
{
	if (!m_ws || !m_scenePreparedReported || !m_sceneAllPrepared
		|| m_sceneArmedReported || m_sceneCommitReceived
		|| m_pendingSceneInstanceId.isEmpty()) {
		return;
	}

	qint64 maximum = -1;
	const qint64 uncertainty = m_ws->sceneClockUncertaintyMs();
	if (!readBoundedInt64(m_ws->serverPolicy(), "sceneMaxClockSkewMs",
						0, kMaxSafeJsonInteger, maximum)
		|| uncertainty < 0 || uncertainty > maximum) {
		// Clock quality is expected to be transient during startup/resume. Keep
		// the prepared renderer graph intact and let the authoritative SceneRun
		// preparation deadline bound these coalesced synchronization bursts.
		m_ws->requestSceneClockSynchronization();
		return;
	}

	m_sceneArmedReported = m_ws->sendSceneArmed(
		m_pendingSceneInstanceId, uncertainty);
	if (!m_sceneArmedReported) {
		m_ws->requestSceneClockSynchronization();
	}
}

void RemoteSceneController::updatePrepareProgress()
{
    if (!m_ws || m_pendingSceneInstanceId.isEmpty() || m_prepareChecklist.isEmpty()) return;
    int ready = 0;
    for (const QJsonValue& value : std::as_const(m_prepareChecklist)) {
        if (value.toObject().value(QStringLiteral("ready")).toBool(false)) ++ready;
    }
    const int percent = (ready * 100) / m_prepareChecklist.size();
    m_ws->sendScenePrepareProgress(m_pendingSceneInstanceId, percent, m_prepareChecklist);
}

void RemoteSceneController::sendFirstFramePresented(bool forceReplay)
{
	if ((!forceReplay && m_firstFrameReported)
		|| !m_screensAwaitingFirstFrame.isEmpty()
        || !m_ws || m_pendingSceneInstanceId.isEmpty()) return;
	if (m_firstFramePresentedServerMonotonicMs < 0) {
		qint64 maximumClockSkewMs = -1;
		const qint64 uncertaintyMs = m_ws->sceneClockUncertaintyMs();
		const qint64 serverNow = m_ws->estimatedServerMonotonicMs();
		const qint64 localNow = localSteadyMilliseconds();
		if (!readBoundedInt64(m_ws->serverPolicy(), "sceneMaxClockSkewMs",
							0, kMaxSafeJsonInteger, maximumClockSkewMs)
			|| uncertaintyMs < 0 || uncertaintyMs > maximumClockSkewMs
			|| serverNow < 0 || m_firstFramePresentedLocalSteadyMs < 0
			|| localNow < m_firstFramePresentedLocalSteadyMs) {
			return;
		}
		const qint64 elapsedSincePresentation =
			localNow - m_firstFramePresentedLocalSteadyMs;
		if (serverNow < elapsedSincePresentation) return;
		m_firstFramePresentedServerMonotonicMs =
			serverNow - elapsedSincePresentation;
	}
	const qint64 timestamp = m_firstFramePresentedServerMonotonicMs;
	if (timestamp < 0) return;
	m_firstFrameReported = m_ws->sendSceneStarted(
		m_pendingSceneInstanceId, true, timestamp) || m_firstFrameReported;
	if (m_firstFrameReported) disconnectFirstFrameObservers();
}

void RemoteSceneController::disconnectFirstFrameObservers()
{
	for (auto it = m_screenWindows.begin(); it != m_screenWindows.end(); ++it) {
		QObject::disconnect(it->firstFrameConnection);
		it->firstFrameConnection = {};
		it->firstFramePassesRemaining = 0;
	}
}

void RemoteSceneController::onRemoteSceneStart(const QString& senderClientId, const QJsonObject& scene) {
    if (!m_enabled) return;

    int renderSchemaVersion = 0;
    const QJsonArray screens = scene.value("screens").toArray();
    const QJsonArray media = scene.value("media").toArray();
    const QString sceneInstanceId = scene.value("sceneInstanceId").toString();

    auto rejectStart = [&](const QString& errorMsg) {
        qWarning() << "RemoteSceneController: validation failed -" << errorMsg;
        if (m_ws) {
            m_ws->sendScenePrepared(sceneInstanceId, false, {},
                                    QStringLiteral("scene_prepare_failed"), errorMsg);
        }
    };

    if (!readBoundedInteger(scene, "renderSchemaVersion", 3, 3,
                            renderSchemaVersion)
        || !scene.value(QStringLiteral("screens")).isArray()
        || !scene.value(QStringLiteral("media")).isArray()
        || sceneInstanceId.isEmpty()) {
        rejectStart(QStringLiteral("Scene does not conform to render schema 3"));
        return;
    }

    auto isSameRun = [&](const QString& ownerId, const QString& runId) {
        return !ownerId.isEmpty() && !runId.isEmpty()
            && senderClientId == ownerId && sceneInstanceId == runId;
    };

    // START is idempotent for the exact run. It must never act as an implicit
    // STOP/replacement command for a scene owned by another run or sender.
    if (!m_pendingSceneInstanceId.isEmpty()) {
        if (!isSameRun(m_pendingSenderClientId, m_pendingSceneInstanceId)) {
            rejectStart(QStringLiteral("Remote scene target is busy with another scene"));
            return;
        }

        qDebug() << "RemoteSceneController: duplicate start for current scene" << sceneInstanceId;
        if (m_sceneActivationRequested || m_sceneActivated) {
            if (m_ws) {
                m_ws->sendScenePrepared(sceneInstanceId, true, m_prepareChecklist);
				// A duplicate START is a protocol retry, not evidence that a frame
				// reached the compositor. Replay STARTED only after the real barrier
				// completed, preserving the original presentation timestamp.
				if (m_sceneActivated && m_screensAwaitingFirstFrame.isEmpty()
					&& m_firstFramePresentedLocalSteadyMs >= 0) {
					sendFirstFramePresented(true);
                }
            }
        }
        return;
    }

    if (!m_startingSceneInstanceId.isEmpty()) {
        if (isSameRun(m_startingSenderClientId, m_startingSceneInstanceId)) {
            qDebug() << "RemoteSceneController: ignoring duplicate start already being prepared"
                     << sceneInstanceId;
            return;
        }
        rejectStart(QStringLiteral("Remote scene target is busy preparing another scene"));
        return;
    }

    if (m_deferredSceneStart.valid) {
        const QString deferredSceneInstanceId =
            m_deferredSceneStart.scene.value("sceneInstanceId").toString();
        if (isSameRun(m_deferredSceneStart.senderId, deferredSceneInstanceId)) {
            qDebug() << "RemoteSceneController: ignoring duplicate deferred start"
                     << sceneInstanceId;
            return;
        }
        rejectStart(QStringLiteral("Remote scene target already has a queued scene"));
        return;
    }

    if (senderClientId == m_lastStoppedSenderClientId
        && sceneInstanceId == m_lastStoppedSceneInstanceId) {
        rejectStart(QStringLiteral("Scene instance has already been stopped"));
        return;
    }

    if (m_sceneStartInProgress || m_teardownInProgress) {
        qDebug() << "RemoteSceneController: deferring remote scene start while teardown is pending";
        m_deferredSceneStart.senderId = senderClientId;
        m_deferredSceneStart.scene = scene;
        m_deferredSceneStart.valid = true;
        return;
    }

    m_sceneStartInProgress = true;
    m_startingSenderClientId = senderClientId;
    m_startingSceneInstanceId = sceneInstanceId;
    auto cleanup = std::shared_ptr<void>(nullptr, [this, senderClientId, sceneInstanceId](void*) {
        if (m_startingSenderClientId == senderClientId
            && m_startingSceneInstanceId == sceneInstanceId) {
            m_startingSenderClientId.clear();
            m_startingSceneInstanceId.clear();
        }
        m_sceneStartInProgress = false;
        dispatchDeferredSceneStart();
    });

    auto failWithMessage = rejectStart;

    if (screens.isEmpty()) {
        failWithMessage(QStringLiteral("Scene has no screen configuration"));
        return;
    }

    if (media.isEmpty()) {
        failWithMessage(QStringLiteral("Scene has no media items"));
        return;
    }

    if (screens.size() > kMaxRemoteScreens) {
        failWithMessage(QStringLiteral("Scene exceeds the remote screen limit"));
        return;
    }
    const auto localScreens = LocalScreenTopology::screens();
    if (localScreens.size() < screens.size()) {
        failWithMessage(QStringLiteral("Remote display topology is incompatible: %1 screen(s) required, %2 available")
                            .arg(screens.size())
                            .arg(localScreens.size()));
        return;
    }
    if (media.size() > kMaxRemoteMediaItems) {
        failWithMessage(QStringLiteral("Scene exceeds the remote media limit"));
        return;
    }

    // Treat the remotely supplied snapshot as an untrusted protocol payload.
    // Validate it completely before clearScene(), so a malformed START cannot
    // tear down an already prepared surface or create unbounded QML geometry.
    QSet<int> declaredScreenIds;
    for (qsizetype screenIndex = 0; screenIndex < screens.size(); ++screenIndex) {
        const QJsonValue screenValue = screens.at(screenIndex);
        if (!screenValue.isObject()) {
            failWithMessage(QStringLiteral("Invalid screen entry at index %1").arg(screenIndex));
            return;
        }

        const QJsonObject screenObject = screenValue.toObject();
        int screenId = -1;
        int screenWidth = 0;
        int screenHeight = 0;
        int screenX = 0;
        int screenY = 0;
        if (!readBoundedInteger(screenObject, "id", 0, 1000000, screenId)
            || !readBoundedInteger(screenObject, "width", 1, 100000, screenWidth)
            || !readBoundedInteger(screenObject, "height", 1, 100000, screenHeight)
            || !readBoundedInteger(screenObject, "x", -100000000, 100000000, screenX)
            || !readBoundedInteger(screenObject, "y", -100000000, 100000000, screenY)
            || !screenObject.value(QStringLiteral("primary")).isBool()
            || declaredScreenIds.contains(screenId)) {
            failWithMessage(QStringLiteral("Invalid or duplicate screen declaration at index %1")
                                .arg(screenIndex));
            return;
        }
        declaredScreenIds.insert(screenId);
    }

    SceneTimeline::SceneSettings timelineSettings;
    QString timelineError;
    if (!SceneTimeline::SceneSettings::fromJson(scene.value(QStringLiteral("timeline")).toObject(),
                                               &timelineSettings, &timelineError)) {
        failWithMessage(timelineError);
        return;
    }
    QSet<QString> declaredMediaIds;
    for (const auto& mediaValue : media) {
        const QJsonObject object = mediaValue.toObject();
        const QString id = object.value(QStringLiteral("mediaId")).toString();
        const QString type = object.value(QStringLiteral("type")).toString();
        SceneTimeline::ElementState state;
        SceneTimeline::MediaTrack track;
        if (!mediaValue.isObject() || id.isEmpty() || id.size() > kMaxRemoteIdentifierLength
            || declaredMediaIds.contains(id)
            || !SceneTimeline::ElementState::fromMediaJson(object, &state, &timelineError)
            || !SceneTimeline::MediaTrack::fromJson(object.value(QStringLiteral("timeline")).toObject(),
                                                    &track, timelineSettings.maxDurationMs, &timelineError)
            || (type != QLatin1String("video") && !track.clips.isEmpty())
            || std::any_of(track.keyframes.cbegin(), track.keyframes.cend(),
                           [&type](const auto& key) { return key.state.type != type; })) {
            failWithMessage(QStringLiteral("Invalid timeline media %1: %2").arg(id, timelineError));
            return;
        }
        if (type == QLatin1String("video")) {
            qint64 durationMs = 0;
            if (!readBoundedInt64(object, "durationMs", 0, 604800000, durationMs)
                || std::any_of(track.clips.cbegin(), track.clips.cend(),
                               [durationMs](const auto& clip) { return clip.sourceOutMs > durationMs; })) {
                failWithMessage(QStringLiteral("Invalid timeline video duration"));
                return;
            }
        }
        if (type != QLatin1String("text")
            && (object.value(QStringLiteral("fileId")).toString().isEmpty()
                || object.value(QStringLiteral("fileId")).toString().size() > kMaxRemoteIdentifierLength
                || object.value(QStringLiteral("fileName")).toString().isEmpty()
                || object.value(QStringLiteral("fileName")).toString().size() > 1024
                || object.value(QStringLiteral("assetId")).toString().isEmpty())) {
            failWithMessage(QStringLiteral("Invalid timeline asset identity"));
            return;
        }
        declaredMediaIds.insert(id);
    }

    QStringList missingFileNames;
    QStringList invalidMediaEntries;
    QList<MediaFilePolicy::PreparationAsset> preparationAssets;
    QHash<QString, QString> preparationFileNames;
    QHash<QString, QString> preparationTypes;
    for (const QJsonValue& val : media) {
        const QJsonObject mediaObj = val.toObject();
        const QString type = mediaObj.value("type").toString();

        if (type != QLatin1String("text")
            && type != QLatin1String("image")
            && type != QLatin1String("video")) {
            invalidMediaEntries.append(mediaObj.value("mediaId").toString(QStringLiteral("unnamed media")));
            continue;
        }

        // Text items don't have files, skip validation for them
        if (type == "text") {
            continue;
        }
        
        const QString fileId = mediaObj.value("fileId").toString();
        if (fileId.isEmpty()) {
            invalidMediaEntries.append(mediaObj.value("mediaId").toString(QStringLiteral("unnamed media")));
            continue;
        }
        const QString path = receivedFilePath(fileId);
        QString fileName = mediaObj.value("fileName").toString();
        if (fileName.isEmpty()) fileName = fileId;
        if (path.isEmpty() || !QFile::exists(path)) {
            missingFileNames.append(fileName);
        } else {
            const QString mediaId = mediaObj.value("mediaId").toString();
            const MediaFilePolicy::Kind expectedKind = type == QLatin1String("video")
                ? MediaFilePolicy::Kind::Mp4Video
                : MediaFilePolicy::Kind::Image;
            preparationAssets.append({mediaId, path, expectedKind});
            preparationFileNames.insert(mediaId, fileName);
            preparationTypes.insert(mediaId, type);
        }
    }

    if (!invalidMediaEntries.isEmpty()) {
        failWithMessage(QStringLiteral("Invalid media declaration: %1")
                            .arg(invalidMediaEntries.join(QStringLiteral(", "))));
        return;
    }

    if (!missingFileNames.isEmpty()) {
        const QString fileList = missingFileNames.size() <= 3
                                     ? missingFileNames.join(", ")
                                     : QString("%1, %2, and %3 more")
                                           .arg(missingFileNames[0])
                                           .arg(missingFileNames[1])
                                           .arg(missingFileNames.size() - 2);
        failWithMessage(QStringLiteral("Missing %1 file%2: %3")
                            .arg(missingFileNames.size())
                            .arg(missingFileNames.size() > 1 ? "s" : "")
                            .arg(fileList));
        return;
    }

    QStringList memoryOwners;
    for (const auto& value : media) {
        const auto entry = value.toObject();
        if (entry.value(QStringLiteral("type")).toString() == QLatin1String("text")) continue;
        const QString owner = UploadManager::residencyOwnerId(m_pendingRemoteSessionId,
            m_pendingSessionGeneration, entry.value(QStringLiteral("fileId")).toString());
        if (!MediaResidencyManager::instance().ready(owner)) {
            failWithMessage(QStringLiteral("Scene media is not fully resident in memory"));
            return;
        }
        memoryOwners.append(owner);
    }

    qDebug() << "RemoteSceneController: validation successful, preparing scene from" << senderClientId;

    const quint64 epoch = ++m_sceneEpoch;
    clearScene();
    if (m_teardownInProgress) {
        // clearScene() deliberately lets Qt retire multimedia/QML/native-window
        // objects after control returns to the event loop. Re-run this validated
        // request after that bounded teardown barrier instead of pumping a nested
        // event loop from inside the WebSocket callback.
        m_deferredSceneStart.senderId = senderClientId;
        m_deferredSceneStart.scene = scene;
        m_deferredSceneStart.valid = true;
        return;
    }
    if (!m_enabled || epoch != m_sceneEpoch) {
        qDebug() << "RemoteSceneController: scene start superseded during teardown" << sceneInstanceId;
        clearScene();
        return;
    }

    m_residencyGroup = QStringLiteral("remote-scene:%1").arg(sceneInstanceId);
    if (!MediaResidencyManager::instance().pinOwners(memoryOwners, m_residencyGroup)) {
        m_residencyGroup.clear();
        failWithMessage(QStringLiteral("Media memory availability changed during preparation"));
        return;
    }
    m_pendingSenderClientId = senderClientId;
    m_pendingSceneInstanceId = sceneInstanceId;
    m_totalMediaToPrime = media.size();
    m_mediaReadyCount = 0;
    m_sceneActivationRequested = false;
    m_sceneActivated = false;

    if (!m_sceneReadyTimeout) {
        m_sceneReadyTimeout = new QTimer(this);
        m_sceneReadyTimeout->setSingleShot(true);
        connect(m_sceneReadyTimeout, &QTimer::timeout, this, &RemoteSceneController::handleSceneReadyTimeout);
    }
    if (m_ws) {
        const int prepareTimeoutMs = m_ws->serverPolicy()
            .value(QStringLiteral("scenePrepareTimeoutMs")).toInt();
        if (prepareTimeoutMs <= 0) {
            failWithMessage(QStringLiteral(
                "The server did not provide a valid scene preparation policy"));
            ++m_sceneEpoch;
            clearScene();
            return;
        }
        m_sceneReadyTimeout->start(prepareTimeoutMs);
    }

    buildWindows(screens);
    if (!m_enabled || epoch != m_sceneEpoch) {
        qDebug() << "RemoteSceneController: scene start superseded while building windows" << sceneInstanceId;
        clearScene();
        return;
    }
    if (m_screenWindows.size() != screens.size()) {
        failWithMessage(QStringLiteral("Remote display topology changed while preparing the scene"));
        ++m_sceneEpoch;
        clearScene();
        return;
    }
    for (auto it = m_screenWindows.cbegin(); it != m_screenWindows.cend(); ++it) {
        const ScreenWindow& window = it.value();
        if (!window.window || !window.mediaModel) {
            failWithMessage(QStringLiteral("Qt Quick remote renderer failed to initialize"));
            ++m_sceneEpoch;
            clearScene();
            return;
        }
    }
    m_timelineSettings = timelineSettings;
    buildMedia(media);
    if (!m_enabled || epoch != m_sceneEpoch) {
        qDebug() << "RemoteSceneController: scene start superseded while building media" << sceneInstanceId;
        clearScene();
        return;
    }

    // This also covers a PREPARE that was deferred behind destruction of the
    // preceding graph: its original envelope callback has already returned,
    // so readiness must be published from the eventual build itself.
    if (!remoteRenderGraphsReady()) {
        failWithMessage(QStringLiteral("Qt Quick remote renderer failed after media construction"));
        ++m_sceneEpoch;
        clearScene();
        return;
    }
    for (qsizetype index = 0; index < m_prepareChecklist.size(); ++index) {
        QJsonObject checklistItem = m_prepareChecklist.at(index).toObject();
        if (checklistItem.value(QStringLiteral("stage")).toString()
            == QLatin1String("screen_render_graph_ready")) {
            checklistItem.insert(QStringLiteral("ready"), true);
            m_prepareChecklist.replace(index, checklistItem);
        }
    }
    updatePrepareProgress();

    // Cancel any pending window show timer from previous scene
    if (m_windowShowTimer) {
        m_windowShowTimer->stop();
        m_windowShowTimer->deleteLater();
        m_windowShowTimer = nullptr;
    }

    // Create a tracked timer (not singleShot) so we can cancel it in clearScene
    m_windowShowTimer = new QTimer(this);
    m_windowShowTimer->setSingleShot(true);
    m_windowShowTimer->setInterval(
        AppConfig::instance().remoteWindowShowDelayMs());
    
    // Use a slightly longer delay (10ms) to ensure all deferred widget deletions have completed
    // before showing new windows. This prevents crashes in macOS accessibility code when
    // windows are rapidly created/destroyed. The delay is imperceptible to users but critical
    // for avoiding race conditions in Qt's widget deletion machinery.
    connect(m_windowShowTimer, &QTimer::timeout, this, [this, epoch]() {
        // Abort if scene changed (stop/start happened during deferral)
        if (epoch != m_sceneEpoch) return;
        
        startSceneActivationIfReady();
    });
    
    m_windowShowTimer->start();
}

void RemoteSceneController::onRemoteSceneActivate(const QString& senderClientId,
                                                  const QString& sceneInstanceId,
                                                  qint64 activationEpochMs,
                                                  int activationDelayMs) {
    if (QThread::currentThread() != thread()) {
        QMetaObject::invokeMethod(this, [this, senderClientId, sceneInstanceId, activationEpochMs, activationDelayMs]() {
            onRemoteSceneActivate(senderClientId, sceneInstanceId, activationEpochMs, activationDelayMs);
        }, Qt::QueuedConnection);
        return;
    }

    if (!m_enabled || m_sceneActivated || !m_sceneActivationRequested) return;
    if (senderClientId != m_pendingSenderClientId || sceneInstanceId != m_pendingSceneInstanceId) {
        qWarning() << "RemoteSceneController: ignoring stale or foreign activation" << sceneInstanceId;
        return;
    }
    if (activationEpochMs <= 0) {
        qWarning() << "RemoteSceneController: ignoring invalid activation timestamp" << activationEpochMs;
        return;
    }

    if (!m_activationTimer) {
        m_activationTimer = new QTimer(this);
        m_activationTimer->setSingleShot(true);
        m_activationTimer->setTimerType(Qt::PreciseTimer);
        connect(m_activationTimer, &QTimer::timeout, this, &RemoteSceneController::activateScene);
    }

    const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
    const qint64 absoluteRemainingMs = activationEpochMs - nowMs;
    const QJsonObject policy = m_ws ? m_ws->serverPolicy() : QJsonObject();
    const qint64 policyActivationLeadMs = policy
        .value(QStringLiteral("sceneActivationLeadMs")).toInt();
    const qint64 maximumClockSkewMs = policy
        .value(QStringLiteral("sceneMaxClockSkewMs")).toInt(-1);
    const qint64 startedAckTimeoutMs = policy
        .value(QStringLiteral("sceneStartedAckTimeoutMs")).toInt();
    const qint64 activationLeadMs = m_committedActivationLeadMs > 0
        ? m_committedActivationLeadMs : policyActivationLeadMs;
    if (policyActivationLeadMs <= 0 || maximumClockSkewMs < 0
        || activationLeadMs <= 0 || activationLeadMs > policyActivationLeadMs
        || startedAckTimeoutMs <= 0) {
        sendPrepareResult(false, QStringLiteral("Invalid scene activation policy"));
        ++m_sceneEpoch;
        clearScene();
        return;
    }
    const qint64 maximumDelayMs = activationLeadMs + maximumClockSkewMs;
    const qint64 fallbackDelayMs = std::clamp<qint64>(
        activationDelayMs, 0, maximumDelayMs);
    const bool absoluteDeadlinePlausible =
        absoluteRemainingMs >= -maximumClockSkewMs
        && absoluteRemainingMs <= maximumDelayMs
        && qAbs(absoluteRemainingMs - fallbackDelayMs) <= maximumClockSkewMs;
    const qint64 remainingMs = absoluteDeadlinePlausible
        ? std::max<qint64>(0, absoluteRemainingMs)
        : fallbackDelayMs;
    m_activationClockPlausible = absoluteDeadlinePlausible;

    if (!absoluteDeadlinePlausible) {
        qWarning() << "RemoteSceneController: activation clocks diverge; using bounded relative delay"
                   << "absoluteRemainingMs=" << absoluteRemainingMs
                   << "fallbackDelayMs=" << fallbackDelayMs;
    }

    m_activationEpochMs = nowMs + remainingMs;
	// ACTIVATE is a commit. Keep only the server-policy-derived STARTED bound
	// until its timer fires; no independent client deadline may pre-empt it.
    if (m_sceneReadyTimeout) {
        m_sceneReadyTimeout->start(
            static_cast<int>(remainingMs + startedAckTimeoutMs));
    }
    if (remainingMs <= 0) {
        QMetaObject::invokeMethod(this, &RemoteSceneController::activateScene, Qt::QueuedConnection);
    } else {
        m_activationTimer->start(static_cast<int>(remainingMs));
    }
}

void RemoteSceneController::onRemoteSceneStop(const QString& senderClientId,
                                              const QString& sceneInstanceId) {
    if (QThread::currentThread() != thread()) {
        QMetaObject::invokeMethod(this, [this, senderClientId, sceneInstanceId]() {
            onRemoteSceneStop(senderClientId, sceneInstanceId);
        }, Qt::QueuedConnection);
        return;
    }

    const QString deferredSceneInstanceId =
        m_deferredSceneStart.scene.value("sceneInstanceId").toString();
    auto matchesOwnedRun = [&](const QString& ownerId, const QString& runId) {
        if (ownerId.isEmpty() || runId.isEmpty() || senderClientId != ownerId) return false;
        return sceneInstanceId.isEmpty() || sceneInstanceId == runId;
    };

    const bool matchesPreparedOrActive =
        matchesOwnedRun(m_pendingSenderClientId, m_pendingSceneInstanceId);
    const bool matchesStartInProgress =
        matchesOwnedRun(m_startingSenderClientId, m_startingSceneInstanceId);
    const bool matchesDeferred = m_deferredSceneStart.valid
        && matchesOwnedRun(m_deferredSceneStart.senderId, deferredSceneInstanceId);
    const bool explicitStop = !sceneInstanceId.isEmpty();
    const bool replayedCompletedStop = explicitStop
        && !matchesPreparedOrActive
        && !matchesStartInProgress
        && !matchesDeferred
        && senderClientId == m_lastStoppedSenderClientId
        && sceneInstanceId == m_lastStoppedSceneInstanceId;

    auto sendStopResult = [&](const QString& resultSceneInstanceId,
                              bool success,
                              const QString& error = QString()) {
        if (m_ws) {
            m_ws->sendSceneStopped(resultSceneInstanceId, success, error);
        }
    };

    if (replayedCompletedStop) {
        sendStopResult(sceneInstanceId, true);
        return;
    }

    const bool hasOwnedRun = !m_pendingSceneInstanceId.isEmpty()
        || !m_startingSceneInstanceId.isEmpty()
        || (m_deferredSceneStart.valid && !deferredSceneInstanceId.isEmpty());
    const bool accepted = explicitStop
        ? (matchesPreparedOrActive || matchesStartInProgress || matchesDeferred)
        : (!hasOwnedRun || matchesPreparedOrActive || matchesStartInProgress || matchesDeferred);
    if (!accepted) {
        sendStopResult(
            sceneInstanceId,
            false,
            explicitStop
                ? QStringLiteral("Scene instance does not match the active remote scene")
                : QStringLiteral("Generic stop rejected: remote scene belongs to another client"));
        return;
    }

    QString stoppedSceneInstanceId = sceneInstanceId;
    if (stoppedSceneInstanceId.isEmpty()) {
        if (matchesPreparedOrActive) stoppedSceneInstanceId = m_pendingSceneInstanceId;
        else if (matchesStartInProgress) stoppedSceneInstanceId = m_startingSceneInstanceId;
        else if (matchesDeferred) stoppedSceneInstanceId = deferredSceneInstanceId;
    }

    if (matchesDeferred) {
        m_deferredSceneStart.valid = false;
    }

    if (matchesPreparedOrActive || matchesStartInProgress) {
        if (matchesStartInProgress) {
            m_startingSenderClientId.clear();
            m_startingSceneInstanceId.clear();
        }
        ++m_sceneEpoch;
        clearScene();
    }

    if (!stoppedSceneInstanceId.isEmpty()) {
        m_lastStoppedSenderClientId = senderClientId;
        m_lastStoppedSceneInstanceId = stoppedSceneInstanceId;
    }

    sendStopResult(stoppedSceneInstanceId, true);
}

void RemoteSceneController::onConnectionLost() {
    const bool hadScene = !m_mediaItems.isEmpty() || !m_screenWindows.isEmpty();
    m_startingSenderClientId.clear();
    m_startingSceneInstanceId.clear();
    m_deferredSceneStart.valid = false;
    ++m_sceneEpoch;
    clearScene();
    if (hadScene) {
        TOAST_WARNING("Remote scene stopped: server connection lost",
                      AppConfig::instance().toastWarningDurationMs());
    }
}

void RemoteSceneController::onConnectionError(const QString& errorMessage) {
    // WebSocketClient also uses this signal for server-side protocol errors.
    // Those must not tear down a healthy, active scene. A real transport loss
    // is handled authoritatively by disconnected(); if the socket is already
    // down here, converge immediately as a defensive fallback.
    if (m_ws && m_ws->isConnected()) {
        qWarning() << "RemoteSceneController: non-transport WebSocket error ignored for active scene:"
                   << errorMessage;
        return;
    }
    onConnectionLost();
}

void RemoteSceneController::clearScene() {
    if (QThread::currentThread() != thread()) {
        QMetaObject::invokeMethod(this, [this]() { clearScene(); }, Qt::QueuedConnection);
        return;
    }

    // Teardown is idempotent. In particular, a duplicate STOP or a connection
    // notification must not enter destruction again while deleteLater() events
    // from the first request are still pending.
    if (m_teardownInProgress) {
        return;
    }

    m_teardownInProgress = true;
    m_screenRefreshTimer.stop();
    m_teardownCompletionScheduled = false;
    m_pendingTeardownObjects.clear();
    ++m_teardownBarrierEpoch;
    if (m_teardownGraphRemoteSessionId.isEmpty()) {
        m_teardownGraphRemoteSessionId = m_pendingRemoteSessionId;
    }

    // CRITICAL: Cancel pending window show timer to prevent showing windows after scene cleared
    if (m_windowShowTimer) {
        m_windowShowTimer->stop();
        QObject::disconnect(m_windowShowTimer, nullptr, this, nullptr);
        trackTeardownObjectTree(m_windowShowTimer);
        m_windowShowTimer->deleteLater();
        m_windowShowTimer = nullptr;
    }

    resetSceneSynchronization();
    
    // Defensive teardown to handle rapid start/stop without use-after-free
    for (const auto& item : m_mediaItems) {
        teardownMediaItem(item);
    }
    m_mediaItems.clear();
    
    // Hide remote screen windows immediately. Their QObject trees are retired
    // asynchronously below so QML, the scene graph, multimedia and Cocoa can
    // unwind on the normal Qt event-loop boundary.
    for (auto it = m_screenWindows.begin(); it != m_screenWindows.end(); ++it) {
        ScreenWindow& sw = it.value();
        if (!sw.window) {
            continue;
        }

        QQuickWindow* window = sw.window;
        sw.sceneEpoch = 0;

        WindowStackingCoordinator::instance().unregisterWindow(window);
        QObject::disconnect(window, nullptr, nullptr, nullptr);
        window->hide();

#ifdef Q_OS_MAC
        MacWindowManager::orderOutWindow(window);
#endif

        // Do not synchronously reset the QML model here. VideoItem
        // may have Qt.callLater work queued from component creation; destroying
        // its context in the middle of this network callback makes that work run
        // against an invalid QML object. The QQuickWindow owns its visual tree
        // and model, so one deleteLater() retires the complete graph safely at
        // the normal event-loop boundary.
        sw.mediaModel = nullptr;
        sw.mediaEntries.clear();

        // Register the destruction observers after all wildcard disconnects;
        // otherwise QObject::disconnect(sender, nullptr, nullptr, nullptr)
        // would silently remove the barrier itself.
        trackTeardownObjectTree(window);

        window->close();
        window->lower();
        window->deleteLater();

        sw.window = nullptr;
    }

    m_screenWindows.clear();

    // An initial START calls clearScene() before it owns any QObject graph. It
    // can keep the historical synchronous fast path because there is nothing
    // to settle and no RemoteSession teardown is waiting. Every real teardown,
    // including an empty graph, completes on a later event-loop boundary.
    if (m_pendingTeardownObjects.isEmpty()
        && m_sceneStartInProgress
        && m_teardownSessionWaiters.isEmpty()) {
        m_teardownInProgress = false;
        m_teardownGraphRemoteSessionId.clear();
        return;
    }

    scheduleTeardownBarrierCompletion();
}

void RemoteSceneController::trackTeardownObject(QObject* object)
{
    if (!object || m_pendingTeardownObjects.contains(object)) return;
    const quint64 barrierEpoch = m_teardownBarrierEpoch;
    m_pendingTeardownObjects.insert(object);
    connect(object, &QObject::destroyed, this,
            [this, object, barrierEpoch]() {
        if (barrierEpoch != m_teardownBarrierEpoch) return;
        m_pendingTeardownObjects.remove(object);
        scheduleTeardownBarrierCompletion();
    });
}

void RemoteSceneController::trackTeardownObjectTree(QObject* root)
{
    // The QQuickWindow owns its complete QML object tree. A queued barrier
    // after the top-level destruction is sufficient and avoids inspecting
    // visual children from C++.
    trackTeardownObject(root);
}

void RemoteSceneController::scheduleTeardownBarrierCompletion()
{
    if (!m_teardownInProgress || !m_pendingTeardownObjects.isEmpty()
        || m_teardownCompletionScheduled) {
        return;
    }
    m_teardownCompletionScheduled = true;
    const quint64 barrierEpoch = m_teardownBarrierEpoch;
    QMetaObject::invokeMethod(this, [this, barrierEpoch]() {
        completeTeardownBarrier(barrierEpoch);
    }, Qt::QueuedConnection);
}

void RemoteSceneController::completeTeardownBarrier(quint64 barrierEpoch)
{
    if (barrierEpoch != m_teardownBarrierEpoch || !m_teardownInProgress) return;
    m_teardownCompletionScheduled = false;
    if (!m_pendingTeardownObjects.isEmpty()) return;

    const QSet<QString> sessionWaiters = m_teardownSessionWaiters;
    m_teardownSessionWaiters.clear();
    m_teardownGraphRemoteSessionId.clear();
    m_teardownInProgress = false;
    MediaResidencyManager::instance().unpinGroup(m_residencyGroup);
    m_residencyGroup.clear();

    if (sessionWaiters.isEmpty()) {
        emit teardownSettled(QString(), true);
    } else {
        for (const QString& remoteSessionId : sessionWaiters) {
            emit teardownSettled(remoteSessionId, true);
        }
    }
    dispatchDeferredSceneStart();
}

void RemoteSceneController::dispatchDeferredSceneStart() {
    if (!m_deferredSceneStart.valid) {
        return;
    }

    if (!m_enabled) {
        m_deferredSceneStart.valid = false;
        return;
    }

    if (m_sceneStartInProgress || m_teardownInProgress) {
        return;
    }

    PendingSceneRequest request = m_deferredSceneStart;
    m_deferredSceneStart.valid = false;

    QMetaObject::invokeMethod(this, [this, request]() {
        if (!m_enabled) {
            return;
        }
        onRemoteSceneStart(request.senderId, request.scene);
    }, Qt::QueuedConnection);
}

void RemoteSceneController::teardownMediaItem(const std::shared_ptr<RemoteMediaItem>& item)
{
    if (!item) return;
    disconnect(item->mirrorConn);
    if (item->audio) {
        item->audio->setMuted(true);
        item->audio->setVolume(0.0);
        disconnect(item->audio, nullptr, nullptr, nullptr);
    }
    if (item->player) {
        disconnect(item->player, nullptr, nullptr, nullptr);
        item->player->pause();
        item->player->setVideoSink(nullptr);
        item->player->setAudioOutput(nullptr);
        item->player->clearAsset();
        if (item->liveSink) disconnect(item->liveSink, nullptr, nullptr, nullptr);
        item->liveSink = nullptr; // Owned by the player.
        trackTeardownObjectTree(item->player);
        item->player->deleteLater();
        item->player = nullptr;
    }
    if (item->audio) {
        trackTeardownObjectTree(item->audio);
        item->audio->deleteLater();
        item->audio = nullptr;
    }
    if (item->frameSource) {
        item->frameSource->clear();
        trackTeardownObjectTree(item->frameSource);
        item->frameSource->deleteLater();
        item->frameSource = nullptr;
    }
    item->spans.clear();
    item->primedFrame = {};
    item->lastFrameImage = {};
    item->loaded = item->primedFirstFrame = item->readyNotified = false;
    item->videoOutputsAttached = item->timelineVideoPlaying = false;
}

void RemoteSceneController::markItemReady(const std::shared_ptr<RemoteMediaItem>& item) {
    if (!item) return;
    if (item->readyNotified) return;
    item->readyNotified = true;
    ++m_mediaReadyCount;
    for (qsizetype index = 0; index < m_prepareChecklist.size(); ++index) {
        QJsonObject entry = m_prepareChecklist.at(index).toObject();
        if (entry.value(QStringLiteral("itemId")).toString()
                .startsWith(item->mediaId + QLatin1Char('_'))) {
            entry.insert(QStringLiteral("ready"), true);
            m_prepareChecklist.replace(index, entry);
        }
    }
    updatePrepareProgress();
    qDebug() << "RemoteSceneController: media primed" << item->mediaId << "(" << m_mediaReadyCount << "/" << m_totalMediaToPrime << ")";
    startSceneActivationIfReady();
}

void RemoteSceneController::evaluateItemReadiness(const std::shared_ptr<RemoteMediaItem>& item) {
    if (!item) return;
    if (item->readyNotified) return;
    bool ready = false;
    if (item->type == "image") {
        ready = item->loaded && allSpansReady(item);
    } else if (item->type == "video") {
        ready = item->loaded && item->primedFirstFrame && allSpansReady(item);
    } else {
        ready = item->loaded && allSpansReady(item);
    }
    if (ready) {
        markItemReady(item);
    }
}

void RemoteSceneController::startSceneActivationIfReady() {
    if (m_sceneActivated || m_sceneActivationRequested) return;
    if (m_totalMediaToPrime > 0 && m_mediaReadyCount < m_totalMediaToPrime) return;
    if (!remoteRenderGraphsReady()) return;

    // PREPARE is complete. Keep all windows hidden and every automation timer
    // stopped until the server sends COMMIT with a shared monotonic deadline.
    // Do not replace the server's scenePrepareTimeout here: the peer may still
    // be decoding, or both peers may be acquiring a precise clock sample. The
    // COMMIT handler installs the shorter presentation deadline once it exists.
    m_sceneActivationRequested = true;
    if (m_ws && !m_pendingSenderClientId.isEmpty() && !m_pendingSceneInstanceId.isEmpty()) {
        sendPrepareResult(true);
    }
}

void RemoteSceneController::applyImageToSpans(const std::shared_ptr<RemoteMediaItem>& item, const QImage& image) const {
    if (!item || image.isNull() || !item->frameSource) return;
    item->frameSource->setFrame(image);
}

void RemoteSceneController::ensureVideoOutputsAttached(const std::shared_ptr<RemoteMediaItem>& item)
{
    if (!item || !item->player || item->videoOutputsAttached) return;
    item->liveSink = new QVideoSink(item->player);
    const quint64 epoch = item->sceneEpoch;
    std::weak_ptr<RemoteMediaItem> weak = item;
    item->mirrorConn = connect(item->liveSink, &QVideoSink::videoFrameChanged,
        item->liveSink, [this, epoch, weak](const QVideoFrame& frame) {
        const auto item = weak.lock();
        if (!item || epoch != m_sceneEpoch || !frame.isValid()) return;
        // A paused seek can emit an older queued frame; expose only the frame
        // covering the latest cursor. Playing decoders retain their own A/V clock.
        if (!item->timelineVideoPlaying
            && !item->player->preparedAt(item->timelineRequestedSourceMs)) return;
        item->primedFrame = frame;
        if (!item->primedFirstFrame || item->timelinePixelsVisible) {
            const QImage image = convertFrameToImage(frame);
            if (!image.isNull()) {
                item->lastFrameImage = image;
                applyImageToSpans(item, image);
            }
        }
    });
    item->player->setVideoSink(item->liveSink);
    item->videoOutputsAttached = true;
}

void RemoteSceneController::activateScene() {
    // A queued activation timer can run before the lease watchdog after wake.
    // Validate the authority deadline at the side-effect boundary itself.
    if (m_ws && !m_pendingRemoteSessionId.isEmpty()
        && m_ws->sessionRecoveryRemainingMs(m_pendingRemoteSessionId) <= 0) {
        onConnectionLost();
        return;
    }

    if (m_sceneActivated || !m_sceneActivationRequested) return;
	const quint64 activationEpoch = m_sceneEpoch;
	bool activationGraphReady = remoteRenderGraphsReady();
	for (auto it = m_screenWindows.cbegin();
		 activationGraphReady && it != m_screenWindows.cend(); ++it) {
		activationGraphReady = it->sceneEpoch == activationEpoch
			&& it->window;
	}
	if (!activationGraphReady) {
		const qint64 timestamp = m_ws ? m_ws->estimatedServerMonotonicMs() : -1;
		if (m_ws && timestamp >= 0) {
			m_ws->sendSceneStarted(m_pendingSceneInstanceId, false, timestamp);
		}
		qWarning() << "RemoteSceneController: activation failed before first-frame presentation";
		++m_sceneEpoch;
		clearScene();
		return;
	}

    m_sceneActivated = true;
    m_sceneActivationRequested = false;
    if (m_activationTimer) m_activationTimer->stop();

    if (m_sceneReadyTimeout) {
        m_sceneReadyTimeout->stop();
    }

    m_screensAwaitingFirstFrame.clear();
    for (auto it = m_screenWindows.begin(); it != m_screenWindows.end(); ++it) {
        ScreenWindow& sw = it.value();
		QQuickWindow* renderWindow = sw.window;

		// Install the render observer only after show(). Any preparation frame
		// emitted while the window was hidden is therefore outside this
		// activation generation and cannot satisfy the barrier.
		sw.window->show();
        WindowStackingCoordinator::instance().setSceneWindowActive(sw.window, true);

        const int screenId = it.key();
        m_screensAwaitingFirstFrame.insert(screenId);
		// Observe the real QQuickWindow scene graph. The surrounding exposure
		// check prevents a hidden preparation pass from satisfying the barrier.
		sw.firstFramePassesRemaining = 1;
        QObject::disconnect(sw.firstFrameConnection);
        sw.firstFrameConnection = connect(
            renderWindow, &QQuickWindow::afterFrameEnd, this,
            [this, activationEpoch, screenId]() {
				if (activationEpoch != m_sceneEpoch || !m_sceneActivated) {
                    return;
                }
				auto windowIt = m_screenWindows.find(screenId);
				if (windowIt == m_screenWindows.end()
					|| windowIt->sceneEpoch != activationEpoch
					|| !windowIt->window) {
					return;
				}

				ScreenWindow& presentedWindow = windowIt.value();
				if (!presentedWindow.targetScreen || !presentedWindow.window->isVisible()
					|| !presentedWindow.window->isExposed()) {
					// A hidden/off-screen render pass is not a presented frame. Keep
					// the barrier armed and request another compositor cycle.
					if (presentedWindow.targetScreen) presentedWindow.window->update();
					return;
                }

				if (presentedWindow.firstFramePassesRemaining > 0) {
					--presentedWindow.firstFramePassesRemaining;
					if (presentedWindow.firstFramePassesRemaining > 0) {
						presentedWindow.window->update();
						return;
					}
					m_screensAwaitingFirstFrame.remove(screenId);
				}

				if (m_screensAwaitingFirstFrame.isEmpty()) {
					if (m_firstFramePresentedLocalSteadyMs < 0) {
						m_firstFramePresentedLocalSteadyMs =
							localSteadyMilliseconds();
					}
                    sendFirstFramePresented();
                }
			}, Qt::QueuedConnection);

		renderWindow->update();
    }

    m_timelineAnchorMs = 0;
    if (m_ws && m_timelineStartServerMs >= 0) {
        const qint64 now = m_ws->estimatedServerMonotonicMs();
        if (now >= 0) m_timelineAnchorMs = std::max<qint64>(0, now - m_timelineStartServerMs);
    }
    m_timelineClock.start();
    m_timelineFinished = false;
    advanceTimeline();
    if (!m_timelineFinished) m_timelineTimer.start();
    // No local show()/repaint acknowledgement is emitted. Each target screen
    // must complete a post-exposure Qt Quick frame above; if one never does, no
    // `started` acknowledgement is sent and the server's authoritative
    // SceneRun started deadline fails both parties closed.
}

void RemoteSceneController::handleSceneReadyTimeout() {
    qWarning() << "RemoteSceneController: timed out waiting for remote media to load";
    sendPrepareResult(
        false,
        m_sceneActivationRequested
            ? QStringLiteral("Timed out waiting for scene activation")
            : QStringLiteral("Timed out waiting for remote media to load"));
    ++m_sceneEpoch;
    clearScene();
}

void RemoteSceneController::resetWindowForNewScene(ScreenWindow& sw, int screenId, int x, int y, int w, int h, bool primary) {
    if (!sw.window || !sw.mediaModel) return;

    QObject::disconnect(sw.firstFrameConnection);
    sw.firstFrameConnection = {};
	sw.firstFramePassesRemaining = 0;

    sw.x = x;
    sw.y = y;
    sw.w = w;
    sw.h = h;
    sw.sceneEpoch = m_sceneEpoch;

    sw.window->hide();
    sw.window->setGeometry(x, y, w, h);
    sw.window->setTitle(primary ? "Remote Scene (Primary)" : "Remote Scene");
    sw.mediaEntries.clear();
    sw.mediaModel->clearAll();
    sw.window->update();

#ifdef Q_OS_MAC
    MacWindowManager::configureGlobalOverlay(sw.window, /*clickThrough*/ true);
#endif
}

QQuickWindow* RemoteSceneController::ensureScreenWindow(int screenId, int x, int y, int w, int h, bool primary) {
    ScreenWindow& sw = m_screenWindows[screenId];

    if (!sw.window) {
        registerCanvasQmlTypes();
        QQmlComponent component(QmlRuntime::engine(),
                                QUrl(QStringLiteral("qrc:/qt/qml/Mouffette/App/resources/qml/RemoteSceneWindow.qml")));
        auto* mediaModel = new MediaListModel(this);
        QObject* created = component.createWithInitialProperties({
            {QStringLiteral("mediaListModel"),
             QVariant::fromValue<QObject*>(mediaModel)}});
        QQuickWindow* window = qobject_cast<QQuickWindow*>(created);
        if (!window) {
            qCritical() << "RemoteSceneController: Qt Quick remote renderer failed to initialize for screen" << screenId;
            for (const QQmlError& error : component.errors()) {
                qCritical().noquote() << error.toString();
            }
            delete created;
            delete mediaModel;
            return nullptr;
        }

        sw.window = window;
        sw.window->setObjectName(QString("RemoteScreenWindow_%1").arg(screenId));
        sw.window->setFlags(Qt::FramelessWindowHint
                            | Qt::WindowStaysOnTopHint
                            | Qt::Tool
                            | Qt::WindowDoesNotAcceptFocus
                            | Qt::WindowTransparentForInput);
        sw.window->setColor(Qt::transparent);
        WindowStackingCoordinator::instance().registerSceneWindow(sw.window);
        mediaModel->setParent(sw.window);
        sw.mediaModel = mediaModel;
        connect(sw.window, SIGNAL(spanReady(QString,QString)),
                this, SLOT(onRemoteSpanReady(QString,QString)));

    }

    resetWindowForNewScene(sw, screenId, x, y, w, h, primary);
    return sw.window;
}

void RemoteSceneController::buildWindows(const QJsonArray& screensArray) {
    const auto localScreens = LocalScreenTopology::screens();
    int hostIndex = 0;
    for (const auto& value : screensArray) {
        const QJsonObject source = value.toObject();
        const int screenId = source.value("id").toInt();
        if (hostIndex >= localScreens.size()) break;
        const auto& target = localScreens[hostIndex++];
        if (!target.screen) {
            qWarning() << "RemoteSceneController: local screen disappeared during preparation";
            continue;
        }
        const QRect& geometry = target.geometry;
        if (!ensureScreenWindow(screenId, geometry.x(), geometry.y(),
                                geometry.width(), geometry.height(), target.primary)) continue;
        ScreenWindow& window = m_screenWindows[screenId];
        window.sourceScreenDefinition = source;
        window.targetScreen = target.screen;
        window.screenIdentity = target.identity;
        window.window->setScreen(target.screen);
        window.window->setGeometry(geometry);
    }
    qDebug() << "RemoteSceneController: created" << m_screenWindows.size()
             << "remote screen windows (host screens:" << screensArray.size()
             << ", local screens:" << localScreens.size() << ")";
}

void RemoteSceneController::watchLocalScreen(QScreen* screen)
{
    if (!screen) return;
    const auto changed = [this] { m_screenRefreshTimer.start(); };
    connect(screen, &QScreen::geometryChanged, this, changed);
    connect(screen, &QScreen::logicalDotsPerInchChanged, this, changed);
    connect(screen, &QScreen::physicalDotsPerInchChanged, this, changed);
}

void RemoteSceneController::handleLocalScreenRemoved(QScreen* screen)
{
    if (!screen || m_teardownInProgress) return;
    for (auto it = m_screenWindows.begin(); it != m_screenWindows.end(); ++it) {
        ScreenWindow& output = it.value();
        if (output.targetScreen != screen) continue;
        output.targetScreen = nullptr;
        if (output.window) {
            WindowStackingCoordinator::instance().setSceneWindowActive(output.window, false);
            output.window->hide();
#ifdef Q_OS_MACOS
            MacWindowManager::orderOutWindow(output.window);
#endif
        }
        // Once the first presentation barrier has completed, display loss
        // never changes the run. Before that, a missing display cannot satisfy
        // STARTED, even if it presented an earlier frame in this generation.
        if (m_sceneActivated && m_firstFramePresentedLocalSteadyMs < 0) {
            m_screensAwaitingFirstFrame.insert(it.key());
            output.firstFramePassesRemaining = 1;
        }
    }
}

void RemoteSceneController::updateScreenGeometry(int screenId, QScreen* screen,
                                                const QRect& geometry)
{
    auto found = m_screenWindows.find(screenId);
    if (found == m_screenWindows.end() || !found->window || !screen || geometry.isEmpty()) return;
    ScreenWindow& output = found.value();
    const bool returning = !output.targetScreen;
    const bool resized = output.w != geometry.width() || output.h != geometry.height();
    output.targetScreen = screen;
    output.x = geometry.x();
    output.y = geometry.y();
    output.w = geometry.width();
    output.h = geometry.height();
    if (output.window->screen() != screen) output.window->setScreen(screen);
    output.window->setGeometry(geometry);
    if (resized) {
        for (const auto& item : m_mediaItems) {
            if (std::any_of(item->spans.cbegin(), item->spans.cend(),
                            [screenId](const auto& span) { return span.screenId == screenId; })) {
                updatePublishedMediaItem(item);
            }
        }
    }
    if (returning && m_sceneActivated && !m_timelineFinished) {
        // The model, frame source, media players and all envelopes continued
        // running while hidden; show their current state without rescheduling.
        output.window->show();
        WindowStackingCoordinator::instance().setSceneWindowActive(output.window, true);
        output.window->update();
    }
}

void RemoteSceneController::refreshScreenBindings(const QList<LocalScreenTopology::Screen>& screens)
{
    if (m_teardownInProgress || m_screenWindows.isEmpty()) return;
    QSet<QScreen*> occupied;
    // Retain existing associations before reconnecting absent outputs. The
    // enumeration order and protocol source topology may both differ now.
    for (auto it = m_screenWindows.begin(); it != m_screenWindows.end(); ++it) {
        ScreenWindow& output = it.value();
        if (!output.targetScreen) continue;
        const auto present = std::find_if(screens.cbegin(), screens.cend(), [&](const auto& candidate) {
            return candidate.screen == output.targetScreen
                && (output.screenIdentity.isEmpty() || candidate.identity.isEmpty()
                    || candidate.identity == output.screenIdentity);
        });
        if (present == screens.cend()) {
            handleLocalScreenRemoved(output.targetScreen);
        } else {
            if (output.screenIdentity.isEmpty()) output.screenIdentity = present->identity;
            occupied.insert(present->screen);
            updateScreenGeometry(it.key(), present->screen, present->geometry);
        }
    }
    for (auto it = m_screenWindows.begin(); it != m_screenWindows.end(); ++it) {
        ScreenWindow& output = it.value();
        if (output.targetScreen || output.screenIdentity.isEmpty()) continue;
        const LocalScreenTopology::Screen* match = nullptr;
        int matches = 0;
        for (const auto& candidate : screens) {
            if (candidate.screen && candidate.identity == output.screenIdentity) {
                match = &candidate;
                ++matches;
            }
        }
        if (matches != 1 || occupied.contains(match->screen)) continue;
        occupied.insert(match->screen);
        updateScreenGeometry(it.key(), match->screen, match->geometry);
    }
    startSceneActivationIfReady();
}

void RemoteSceneController::publishScreenModel(int screenId) {
    if (m_batchTimelinePublishing) {
        m_dirtyTimelineScreens.insert(screenId);
        return;
    }
    auto it = m_screenWindows.find(screenId);
    if (it == m_screenWindows.end() || !it->mediaModel) return;
    it->mediaModel->updateFromList(it->mediaEntries);
}

void RemoteSceneController::publishMediaSpan(const std::shared_ptr<RemoteMediaItem>& item,
                                             RemoteMediaItem::Span& span) {
    if (!item) return;
    auto windowIt = m_screenWindows.find(span.screenId);
    if (windowIt == m_screenWindows.end() || !windowIt->window
        || !windowIt->mediaModel) {
        qWarning() << "RemoteSceneController: no Qt Quick surface for span" << span.spanId;
        return;
    }

    const qreal surfaceWidth = std::max<qreal>(1.0, windowIt->w);
    const qreal surfaceHeight = std::max<qreal>(1.0, windowIt->h);
    QVariantMap media;
    media.insert(QStringLiteral("rowKey"), span.spanId);
    media.insert(QStringLiteral("mediaId"), item->mediaId);
    media.insert(QStringLiteral("spanId"), span.spanId);
    media.insert(QStringLiteral("mediaType"), item->type);
    media.insert(QStringLiteral("destX"), span.destNx * surfaceWidth);
    media.insert(QStringLiteral("destY"), span.destNy * surfaceHeight);
    media.insert(QStringLiteral("destWidth"), span.destNw * surfaceWidth);
    media.insert(QStringLiteral("destHeight"), span.destNh * surfaceHeight);
    media.insert(QStringLiteral("sourceX"), span.srcNx);
    media.insert(QStringLiteral("sourceY"), span.srcNy);
    media.insert(QStringLiteral("sourceWidth"), span.srcNw);
    media.insert(QStringLiteral("sourceHeight"), span.srcNh);
    media.insert(QStringLiteral("width"), std::max(1, item->baseWidth));
    media.insert(QStringLiteral("height"), std::max(1, item->baseHeight));
    media.insert(QStringLiteral("z"), item->z);
    media.insert(QStringLiteral("contentVisible"), item->contentVisible);
    media.insert(QStringLiteral("renderVisible"), item->renderVisible && span.destNw > 0 && span.destNh > 0);
    media.insert(QStringLiteral("renderOpacity"), item->renderOpacity);

    // MediaVisual gates both image and video readiness on this shared role.
    // A resident frame alone cannot complete the hidden PREPARE barrier.
    if (item->type != QLatin1String("text")) {
        media.insert(QStringLiteral("residencyReady"),
            MediaResidencyManager::instance().ready(item->residencyOwner));
    }
    if (item->type == QLatin1String("image")) {
        media.insert(QStringLiteral("residentFrameSource"),
            QVariant::fromValue(static_cast<QObject*>(item->frameSource.data())));
    } else if (item->type == QLatin1String("video")) {
        media.insert(QStringLiteral("remoteFrameSource"),
                     QVariant::fromValue(static_cast<QObject*>(item->frameSource.data())));
    } else if (item->type == QLatin1String("text")) {
        QString horizontal = QStringLiteral("center");
        if (item->horizontalAlignment == RemoteMediaItem::HorizontalAlignment::Left) horizontal = QStringLiteral("left");
        if (item->horizontalAlignment == RemoteMediaItem::HorizontalAlignment::Right) horizontal = QStringLiteral("right");
        QString vertical = QStringLiteral("center");
        if (item->verticalAlignment == RemoteMediaItem::VerticalAlignment::Top) vertical = QStringLiteral("top");
        if (item->verticalAlignment == RemoteMediaItem::VerticalAlignment::Bottom) vertical = QStringLiteral("bottom");

        media.insert(QStringLiteral("textContent"), item->text);
        media.insert(QStringLiteral("textFontFamily"), item->fontFamily);
        media.insert(QStringLiteral("textFontPixelSize"), std::max<qreal>(1, item->fontPixelSize));
        media.insert(QStringLiteral("textFontWeight"), item->fontWeight);
        media.insert(QStringLiteral("textItalic"), item->fontItalic);
        media.insert(QStringLiteral("textUnderline"), item->fontUnderline);
        media.insert(QStringLiteral("textUppercase"), item->fontUppercase);
        media.insert(QStringLiteral("textHorizontalAlignment"), horizontal);
        media.insert(QStringLiteral("textVerticalAlignment"), vertical);
        media.insert(QStringLiteral("fitToTextEnabled"), item->fitToTextEnabled);
        media.insert(QStringLiteral("textColor"), item->textColor);
        media.insert(QStringLiteral("textOutlineWidthPx"), item->textOutlineWidthPx);
        media.insert(QStringLiteral("textOutlineColor"), item->textBorderColor);
        media.insert(QStringLiteral("textHighlightEnabled"), item->highlightEnabled);
        media.insert(QStringLiteral("textHighlightColor"), item->textHighlightColor);
    }

    span.modelRow = windowIt->mediaEntries.size();
    windowIt->mediaEntries.append(media);
    publishScreenModel(span.screenId);
}

void RemoteSceneController::updatePublishedMediaItem(
    const std::shared_ptr<RemoteMediaItem>& item)
{
    if (!item) return;
    QSet<int> changedScreens;
    for (const RemoteMediaItem::Span& span : item->spans) {
        auto windowIt = m_screenWindows.find(span.screenId);
        if (windowIt == m_screenWindows.end()) continue;
        const qreal surfaceWidth = std::max<qreal>(1.0, windowIt->w);
        const qreal surfaceHeight = std::max<qreal>(1.0, windowIt->h);
        if (span.modelRow >= 0 && span.modelRow < windowIt->mediaEntries.size()) {
            QVariant& entry = windowIt->mediaEntries[span.modelRow];
            QVariantMap media = entry.toMap();
            if (media.value(QStringLiteral("spanId")).toString() != span.spanId
                || media.value(QStringLiteral("mediaId")).toString() != item->mediaId) {
                continue;
            }
            media.insert(QStringLiteral("destX"), span.destNx * surfaceWidth);
            media.insert(QStringLiteral("destY"), span.destNy * surfaceHeight);
            media.insert(QStringLiteral("destWidth"), span.destNw * surfaceWidth);
            media.insert(QStringLiteral("destHeight"), span.destNh * surfaceHeight);
            media.insert(QStringLiteral("sourceX"), span.srcNx);
            media.insert(QStringLiteral("sourceY"), span.srcNy);
            media.insert(QStringLiteral("sourceWidth"), span.srcNw);
            media.insert(QStringLiteral("sourceHeight"), span.srcNh);
            media.insert(QStringLiteral("width"), std::max(1, item->baseWidth));
            media.insert(QStringLiteral("height"), std::max(1, item->baseHeight));
            media.insert(QStringLiteral("z"), item->z);
            media.insert(QStringLiteral("contentVisible"), item->contentVisible);
            media.insert(QStringLiteral("renderVisible"), item->renderVisible && span.destNw > 0 && span.destNh > 0);
            media.insert(QStringLiteral("renderOpacity"), item->renderOpacity);
            if (item->type == QLatin1String("text")) {
                QString horizontal = QStringLiteral("center");
                if (item->horizontalAlignment
                    == RemoteMediaItem::HorizontalAlignment::Left) {
                    horizontal = QStringLiteral("left");
                } else if (item->horizontalAlignment
                           == RemoteMediaItem::HorizontalAlignment::Right) {
                    horizontal = QStringLiteral("right");
                }
                QString vertical = QStringLiteral("center");
                if (item->verticalAlignment
                    == RemoteMediaItem::VerticalAlignment::Top) {
                    vertical = QStringLiteral("top");
                } else if (item->verticalAlignment
                           == RemoteMediaItem::VerticalAlignment::Bottom) {
                    vertical = QStringLiteral("bottom");
                }
                media.insert(QStringLiteral("textContent"), item->text);
                media.insert(QStringLiteral("textFontFamily"), item->fontFamily);
                media.insert(QStringLiteral("textFontPixelSize"),
                             std::max<qreal>(1, item->fontPixelSize));
                media.insert(QStringLiteral("textFontWeight"), item->fontWeight);
                media.insert(QStringLiteral("textItalic"), item->fontItalic);
                media.insert(QStringLiteral("textUnderline"), item->fontUnderline);
                media.insert(QStringLiteral("textUppercase"), item->fontUppercase);
                media.insert(QStringLiteral("textHorizontalAlignment"), horizontal);
                media.insert(QStringLiteral("textVerticalAlignment"), vertical);
                media.insert(QStringLiteral("fitToTextEnabled"), item->fitToTextEnabled);
                media.insert(QStringLiteral("textColor"), item->textColor);
                media.insert(QStringLiteral("textOutlineWidthPx"), item->textOutlineWidthPx);
                media.insert(QStringLiteral("textOutlineColor"), item->textBorderColor);
                media.insert(QStringLiteral("textHighlightEnabled"), item->highlightEnabled);
                media.insert(QStringLiteral("textHighlightColor"), item->textHighlightColor);
            }
            entry = media;
            changedScreens.insert(span.screenId);
        }
    }
    for (int screenId : std::as_const(changedScreens)) publishScreenModel(screenId);
}

bool RemoteSceneController::allSpansReady(const std::shared_ptr<RemoteMediaItem>& item) const {
    if (!item) return false;
    return std::all_of(item->spans.cbegin(), item->spans.cend(), [](const RemoteMediaItem::Span& span) {
        return span.qmlReady;
    });
}

void RemoteSceneController::onRemoteSpanReady(const QString& mediaId, const QString& spanId) {
    for (const auto& item : m_mediaItems) {
        if (!item || item->mediaId != mediaId || item->sceneEpoch != m_sceneEpoch) continue;
        for (auto& span : item->spans) {
            if (span.spanId == spanId) {
                span.qmlReady = true;
                break;
            }
        }
        if (allSpansReady(item) && item->type != QLatin1String("video")) item->loaded = true;
        evaluateItemReadiness(item);
        return;
    }
}

void RemoteSceneController::buildMedia(const QJsonArray& mediaArray)
{
    m_totalMediaToPrime = mediaArray.size();
    for (const auto& value : mediaArray) {
        const QJsonObject media = value.toObject();
        auto item = std::make_shared<RemoteMediaItem>();
        item->mediaId = media.value(QStringLiteral("mediaId")).toString();
        item->fileId = media.value(QStringLiteral("fileId")).toString();
        item->fileName = media.value(QStringLiteral("fileName")).toString();
        item->type = media.value(QStringLiteral("type")).toString();
        item->residencyOwner = UploadManager::residencyOwnerId(m_pendingRemoteSessionId,
            m_pendingSessionGeneration, item->fileId);
        item->sceneEpoch = m_sceneEpoch;
        SceneTimeline::ElementState::fromMediaJson(media, &item->baseState);
        SceneTimeline::MediaTrack::fromJson(media.value(QStringLiteral("timeline")).toObject(),
                                           &item->timeline, m_timelineSettings.maxDurationMs);
        if (item->type != QLatin1String("text")) {
            item->frameSource = new RemoteVideoFrameSource(this);
            const auto resident = MediaResidencyManager::instance().asset(item->residencyOwner);
            if (resident && !resident->video) item->frameSource->setFrame(resident->image);
        }
        m_mediaItems.append(item);
        updateTimelineGeometry(item, SceneTimeline::evaluate(item->baseState, item->timeline, 0));
        scheduleMedia(item);
    }
}

void RemoteSceneController::scheduleMedia(const std::shared_ptr<RemoteMediaItem>& item)
{
    if (!item) return;
    if (item->type != QLatin1String("video")) {
        item->loaded = true;
        evaluateItemReadiness(item);
        return;
    }
    const auto resident = MediaResidencyManager::instance().asset(item->residencyOwner);
    if (!resident || !resident->video || resident->compressedVideo.isEmpty()) {
        sendPrepareResult(false, QStringLiteral("Resident video allocation is unavailable"));
        return;
    }
    const qint64 duration = (resident->durationUs + 999) / 1000;
    for (const auto& clip : item->timeline.clips) {
        if (clip.sourceOutMs > duration) {
            sendPrepareResult(false, QStringLiteral("Video clip exceeds the validated source duration"));
            return;
        }
    }
    item->player = new ResidentVideoPlayer(this);
    item->audio = new QAudioOutput(this);
    item->audio->setMuted(true);
    item->player->setAudioOutput(item->audio);
    item->timelineRequestedSourceMs = SceneTimeline::evaluateVideo(item->timeline, 0, duration).sourceTimeMs;
    const quint64 epoch = item->sceneEpoch;
    std::weak_ptr<RemoteMediaItem> weak = item;
    connect(item->player, &ResidentVideoPlayer::errorOccurred, this,
        [this, epoch, weak](QMediaPlayer::Error error, const QString& message) {
        const auto item = weak.lock();
        if (!item || epoch != m_sceneEpoch || error == QMediaPlayer::NoError) return;
        if (!m_sceneActivated) sendPrepareResult(false, message);
        else if (m_ws) m_ws->sendSceneStop(m_pendingSceneInstanceId, QStringLiteral("video_playback_failed"));
        onRemoteSceneStop(m_pendingSenderClientId, m_pendingSceneInstanceId);
    }, Qt::QueuedConnection);
    connect(item->player, &ResidentVideoPlayer::frameReady, item->player,
        [this, epoch, weak](qint64) {
        const auto item = weak.lock();
        if (!item || epoch != m_sceneEpoch || item->primedFirstFrame) return;
        const auto frame = item->player->preparedFrame(item->timelineRequestedSourceMs);
        if (!frame.isValid()) return;
        item->lastFrameImage = convertFrameToImage(frame);
        if (item->lastFrameImage.isNull()) return;
        item->primedFrame = frame;
        item->loaded = true;
        item->primedFirstFrame = true;
        applyImageToSpans(item, item->lastFrameImage);
        evaluateItemReadiness(item);
    });
    ensureVideoOutputsAttached(item);
    item->player->setAsset(resident);
    item->player->setLoops(QMediaPlayer::Once);
    item->player->prepare(item->timelineRequestedSourceMs);
}

void RemoteSceneController::updateTimelineGeometry(
    const std::shared_ptr<RemoteMediaItem>& item, const SceneTimeline::ElementState& state)
{
    item->baseWidth = std::max(1, qRound(state.baseSize.width()));
    item->baseHeight = std::max(1, qRound(state.baseSize.height()));
    item->z = state.z;
    item->contentVisible = state.visible;
    item->contentOpacity = state.opacity;
    item->renderVisible = state.visible;
    item->renderOpacity = state.opacity;
    item->muted = state.muted;
    item->volume = state.volume;
    item->text = state.text;
    item->fontFamily = state.fontFamily;
    item->fontWeight = state.fontWeight;
    item->fontPixelSize = state.fontPixelSize;
    item->fontItalic = state.italic;
    item->fontUnderline = state.underline;
    item->fontUppercase = state.uppercase;
    item->textColor = state.textColor.name(QColor::HexArgb);
    item->textOutlineWidthPx = state.outlineWidthPercent * state.fontPixelSize / 100.0;
    item->textBorderColor = state.outlineColor.name(QColor::HexArgb);
    item->fitToTextEnabled = state.fitToText;
    item->highlightEnabled = state.highlightEnabled;
    item->textHighlightColor = state.highlightColor.name(QColor::HexArgb);
    item->horizontalAlignment = state.horizontalAlignment == QLatin1String("left")
        ? RemoteMediaItem::HorizontalAlignment::Left : state.horizontalAlignment == QLatin1String("right")
            ? RemoteMediaItem::HorizontalAlignment::Right : RemoteMediaItem::HorizontalAlignment::Center;
    item->verticalAlignment = state.verticalAlignment == QLatin1String("top")
        ? RemoteMediaItem::VerticalAlignment::Top : state.verticalAlignment == QLatin1String("bottom")
            ? RemoteMediaItem::VerticalAlignment::Bottom : RemoteMediaItem::VerticalAlignment::Center;
    const QRectF bounds(state.position, state.size);
    for (auto window = m_screenWindows.cbegin(); window != m_screenWindows.cend(); ++window) {
        const auto screen = window->sourceScreenDefinition;
        const QRectF screenBounds(screen.value(QStringLiteral("x")).toDouble(),
                                  screen.value(QStringLiteral("y")).toDouble(),
                                  screen.value(QStringLiteral("width")).toDouble(),
                                  screen.value(QStringLiteral("height")).toDouble());
        const QRectF intersection = bounds.intersected(screenBounds);
        auto span = std::find_if(item->spans.begin(), item->spans.end(),
            [id = window.key()](const auto& entry) { return entry.screenId == id; });
        if (span == item->spans.end() && intersection.isEmpty()) continue;
        const bool added = span == item->spans.end();
        if (added) {
            RemoteMediaItem::Span entry;
            entry.screenId = window.key();
            entry.spanId = QStringLiteral("%1:%2").arg(item->mediaId).arg(window.key());
            item->spans.append(entry);
            span = item->spans.end() - 1;
        }
        span->nx = (bounds.x() - screenBounds.x()) / screenBounds.width();
        span->ny = (bounds.y() - screenBounds.y()) / screenBounds.height();
        span->nw = bounds.width() / screenBounds.width();
        span->nh = bounds.height() / screenBounds.height();
        if (intersection.isEmpty()) {
            span->destNx = span->destNy = span->destNw = span->destNh = 0;
            span->srcNx = span->srcNy = 0;
            span->srcNw = span->srcNh = 1;
        } else {
            span->destNx = (intersection.x() - screenBounds.x()) / screenBounds.width();
            span->destNy = (intersection.y() - screenBounds.y()) / screenBounds.height();
            span->destNw = intersection.width() / screenBounds.width();
            span->destNh = intersection.height() / screenBounds.height();
            span->srcNx = (intersection.x() - bounds.x()) / bounds.width();
            span->srcNy = (intersection.y() - bounds.y()) / bounds.height();
            span->srcNw = intersection.width() / bounds.width();
            span->srcNh = intersection.height() / bounds.height();
        }
        if (added) publishMediaSpan(item, *span);
    }
    updatePublishedMediaItem(item);
    const bool pixelsVisible = item->renderVisible && item->renderOpacity > 0.0001
        && std::any_of(item->spans.cbegin(), item->spans.cend(),
                       [](const auto& span) { return span.destNw > 0 && span.destNh > 0; });
    if (item->type == QLatin1String("video") && pixelsVisible
        && !item->timelinePixelsVisible && item->primedFrame.isValid()) {
        // Refresh once on entry: decoding continued while CPU readbacks were
        // suspended for invisible pixels.
        const auto image = convertFrameToImage(item->primedFrame);
        if (!image.isNull()) {
            item->lastFrameImage = image;
            applyImageToSpans(item, image);
        }
    }
    item->timelinePixelsVisible = pixelsVisible;
}

void RemoteSceneController::evaluateTimelineAt(qint64 positionMs, bool playing)
{
    m_timelinePositionMs = std::clamp<qint64>(positionMs, 0, m_timelineSettings.effectiveStopMs());
    // Updating a media row must not rescan every output model for every media.
    // All geometry and intrinsic changes become visible together for this tick.
    m_batchTimelinePublishing = true;
    const qint64 clock = localSteadyMilliseconds();
    for (const auto& item : m_mediaItems) {
        const auto state = SceneTimeline::evaluate(item->baseState, item->timeline, m_timelinePositionMs);
        updateTimelineGeometry(item, state);
        if (!item->player) continue;
        const auto video = SceneTimeline::evaluateVideo(item->timeline, m_timelinePositionMs, item->player->duration());
        const bool shouldPlay = playing && video.playing;
        const bool changedClip = item->timelineClipId != video.clipId;
        const bool discontinuity = changedClip
            && !contiguousTimelineClips(item->timeline, item->timelineClipId, video.clipId);
        const bool transition = shouldPlay != item->timelineVideoPlaying;
        item->timelineVideoPlaying = shouldPlay;
        item->timelineRequestedSourceMs = video.sourceTimeMs;
        if (!shouldPlay) item->player->pause();
        const qint64 error = qAbs(item->player->position() - video.sourceTimeMs);
        const bool drift = shouldPlay && error > AppConfig::instance().sceneVideoSyncPositionToleranceMs()
            && clock >= item->timelineSeekGuardUntilMs;
        const bool heldFrameMissing = !shouldPlay && error > 0
            && !item->player->preparedAt(video.sourceTimeMs)
            && clock >= item->timelineSeekGuardUntilMs;
        if (discontinuity || transition || drift || heldFrameMissing) {
            item->player->setPosition(video.sourceTimeMs);
            item->timelineSeekGuardUntilMs = clock + AppConfig::instance().sceneAuthoritativeSeekGuardMs();
        }
        item->timelineClipId = video.clipId;
        if (shouldPlay && !item->player->isPlaying() && (transition || discontinuity || drift))
            item->player->play();
        item->audio->setVolume(state.volume);
        item->audio->setMuted(!shouldPlay || state.muted);
    }
    m_batchTimelinePublishing = false;
    const auto screens = std::exchange(m_dirtyTimelineScreens, {});
    for (int screenId : screens) publishScreenModel(screenId);
}

void RemoteSceneController::advanceTimeline()
{
    if (!m_sceneActivated || m_timelineFinished) return;
    qint64 time = m_timelineAnchorMs + (m_timelineClock.isValid() ? m_timelineClock.elapsed() : 0);
    if (m_ws && m_timelineStartServerMs >= 0) {
        const qint64 now = m_ws->estimatedServerMonotonicMs();
        if (now >= 0) time = std::max<qint64>(0, now - m_timelineStartServerMs);
    }
    time = std::max(time, m_timelinePositionMs);
    const qint64 stop = m_timelineSettings.effectiveStopMs();
    evaluateTimelineAt(std::min(time, stop), time < stop);
    if (time < stop) return;
    m_timelineFinished = true;
    m_timelineTimer.stop();
    disconnectFirstFrameObservers();
    m_screensAwaitingFirstFrame.clear();
    // End output at the agreed deadline even if the owner's STOP packet is
    // delayed. Keep the graph/session binding until the normal stop handshake.
    for (auto& window : m_screenWindows) {
        if (window.window) {
            WindowStackingCoordinator::instance().setSceneWindowActive(window.window, false);
            window.window->hide();
        }
    }
}
