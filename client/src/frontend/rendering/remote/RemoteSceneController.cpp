#include "frontend/rendering/remote/RemoteSceneController.h"
#include "backend/network/SceneRunCoordinator.h"
#include "backend/network/WebSocketClient.h"
#include "backend/domain/media/TextRenderState.h"
#include "backend/domain/media/MediaFilePolicy.h"
#include "frontend/rendering/canvas/CanvasQmlTypes.h"
#include "frontend/rendering/canvas/MediaListModel.h"
#include "frontend/rendering/remote/RemoteVideoFrameItem.h"
#include "frontend/ui/notifications/ToastNotificationSystem.h"
#include <QJsonArray>
#include <QScreen>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QFileInfo>
#include <QFile>
#include <QDebug>
#include <QVideoFrame>
#include <QVariantAnimation>
#include <QEasingCurve>
#include <QWidget>
#include <QAudioOutput>
#include <QMediaPlayer>
#include <QVideoSink>
#include <QMetaObject>
#include <QUrl>
#include <QThread>
#include <QQuickWidget>
#include <QQuickWindow>
#include <QQmlContext>
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
#include <algorithm>
#include <chrono>
#include <limits>
#include <memory>
#include <QVideoFrameFormat>


namespace {
constexpr qint64 kSeekPositionToleranceMs = 120;
constexpr qint64 kStartFrameTimestampToleranceMs = 25;
constexpr qint64 kDecoderSyncToleranceMs = 25;
constexpr int kLivePlaybackWarmupFrames = 2;
constexpr qint64 kVideoSyncPositionToleranceMs = 400;
constexpr qint64 kMaxVideoSyncTransitMs = 2000;
constexpr qint64 kMaxVideoPositionMs = 7LL * 24LL * 60LL * 60LL * 1000LL;
constexpr int kMaxVideoSyncItems = 512;
constexpr int kMaxRemoteScreens = 64;
constexpr int kMaxRemoteMediaItems = 512;
constexpr int kMaxRemoteSpansPerMedia = 64;
constexpr int kMaxRemoteTotalSpans = 4096;
constexpr int kMaxRemoteIdentifierLength = 128;
constexpr qint64 kMaxSafeJsonInteger = 9007199254740991LL;
constexpr double kMaxRemoteCoordinate = 100000000.0;
constexpr double kMaxRemoteDimension = 10000000.0;
constexpr double kMaxLegacyNormalizedMagnitude = 10000.0;
constexpr double kNormalizedRectEpsilon = 0.0001;

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

bool readFiniteRect(const QJsonObject& object,
                    const char* xKey,
                    const char* yKey,
                    const char* widthKey,
                    const char* heightKey,
                    double& x,
                    double& y,
                    double& width,
                    double& height) {
    return readFiniteNumber(object, xKey, x)
        && readFiniteNumber(object, yKey, y)
        && readFiniteNumber(object, widthKey, width)
        && readFiniteNumber(object, heightKey, height);
}

bool isValidUnitRect(double x, double y, double width, double height) {
    return width > 0.0 && height > 0.0
        && x >= -kNormalizedRectEpsilon
        && y >= -kNormalizedRectEpsilon
        && width <= 1.0 + kNormalizedRectEpsilon
        && height <= 1.0 + kNormalizedRectEpsilon
        && x + width <= 1.0 + kNormalizedRectEpsilon
        && y + height <= 1.0 + kNormalizedRectEpsilon;
}

qint64 localSteadyMilliseconds()
{
	using namespace std::chrono;
	return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

qint64 repeatLeadMarginMs(qint64 durationMs) {
    if (durationMs <= 0) return 120;
    qint64 leadMargin = std::clamp<qint64>(durationMs / 48, 15LL, 120LL);
    if (leadMargin >= durationMs) {
        leadMargin = std::max<qint64>(durationMs / 4, 1LL);
        if (leadMargin >= durationMs) {
            leadMargin = std::max<qint64>(durationMs - 1, 1LL);
        }
    }
    return leadMargin;
}

qint64 frameTimestampMs(const QVideoFrame& frame) {
    if (!frame.isValid()) {
        return -1;
    }
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
    const qint64 startTimeUs = frame.startTime();
    if (startTimeUs >= 0) {
        return startTimeUs / 1000;
    }
#else
    const qint64 startTimeUs = frame.startTime();
    if (startTimeUs >= 0) {
        return startTimeUs / 1000;
    }
    const QVariant metaTimestamp = frame.metaData(QVideoFrame::StartTime);
    if (metaTimestamp.isValid()) {
        bool ok = false;
        const qint64 micro = metaTimestamp.toLongLong(&ok);
        if (ok) {
            return micro / 1000;
        }
    }
#endif
    return -1;
}

QImage convertFrameToImage(const QVideoFrame& frame) {
    if (!frame.isValid()) {
        return {};
    }

    QImage direct = frame.toImage();
    if (!direct.isNull()) {
        if (direct.format() != QImage::Format_RGBA8888 && direct.format() != QImage::Format_ARGB32_Premultiplied) {
            direct = direct.convertToFormat(QImage::Format_RGBA8888);
        }
        return direct;
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

    return mapped;
}

} // namespace

RemoteSceneController::RemoteSceneController(FileManager* fileManager, WebSocketClient* ws, QObject* parent)
    : QObject(parent)
    , m_fileManager(fileManager)
    , m_ws(ws) {
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
        connect(m_ws, &WebSocketClient::leaseExpired,
                this, &RemoteSceneController::onConnectionLost,
                Qt::UniqueConnection);
    }
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
    const bool hasRendererState = !m_pendingRemoteSessionId.isEmpty()
        || !m_pendingSceneInstanceId.isEmpty()
        || !m_startingSceneInstanceId.isEmpty()
        || m_deferredSceneStart.valid
        || !m_mediaItems.isEmpty()
        || !m_screenWindows.isEmpty();

    if (m_teardownInProgress) {
        if (m_teardownGraphRemoteSessionId != remoteSessionId) return false;
        m_teardownSessionWaiters.insert(remoteSessionId);
        scheduleTeardownBarrierCompletion();
        return true;
    }

    if (!ownsRequestedSession) {
		if (m_lastTornDownRemoteSessionId == remoteSessionId) {
			// Replay settlement asynchronously for an idempotent caller whose
			// previous ACK may have been lost. Callers uniformly wait for the
			// signal, even when this renderer graph was already retired.
			QMetaObject::invokeMethod(this, [this, remoteSessionId]() {
				if (m_lastTornDownRemoteSessionId == remoteSessionId) {
					emit teardownSettled(remoteSessionId, true);
				}
			}, Qt::QueuedConnection);
			return true;
		}
        // No graph means this session has nothing to settle. Never allow a
        // teardown for session X to clear an active graph belonging to Y.
        if (hasRendererState) return false;
        m_teardownSessionWaiters.insert(remoteSessionId);
        m_teardownGraphRemoteSessionId = remoteSessionId;
        m_teardownInProgress = true;
        ++m_teardownBarrierEpoch;
        scheduleTeardownBarrierCompletion();
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
        || envelope.value(QStringLiteral("targetDeviceId")).toString() != m_ws->deviceId()) return;
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

    m_pendingRemoteSessionId = envelope.value(QStringLiteral("remoteSessionId")).toString();
    m_lastTornDownRemoteSessionId.clear();
	m_pendingSessionGeneration = static_cast<quint64>(generation);
	m_pendingSceneRevision = static_cast<quint64>(revision);
    m_pendingSceneDigest = envelope.value(QStringLiteral("digest")).toString();
    m_scenePreparedReported = false;
    m_sceneArmedReported = false;
    m_firstFrameReported = false;
	m_firstFramePresentedServerMonotonicMs = -1;
	m_firstFramePresentedLocalSteadyMs = -1;

    QJsonObject scene = incomingScene;
    scene.insert(QStringLiteral("sceneInstanceId"), runId);
    m_prepareChecklist = SceneRunCoordinator::createLocalChecklist(scene);
    for (qsizetype index = 0; index < m_prepareChecklist.size(); ++index) {
        QJsonObject item = m_prepareChecklist.at(index).toObject();
        item.insert(QStringLiteral("ready"), false);
        m_prepareChecklist.replace(index, item);
    }
    onRemoteSceneStart(envelope.value(QStringLiteral("ownerDeviceId")).toString(), scene);

    if (m_pendingSceneInstanceId != runId) {
        if (m_deferredSceneStart.valid) return;
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
        if (!window.window || !window.quickWidget || !window.mediaModel
            || window.quickWidget->status() != QQuickWidget::Ready
            || !window.quickWidget->rootObject()) {
            return false;
        }
    }
    return true;
}

void RemoteSceneController::onScenePreparedEnvelope(const QJsonObject& envelope)
{
    if (!matchesSceneEnvelope(envelope) || !m_scenePreparedReported
        || !envelope.value(QStringLiteral("allPrepared")).toBool(false)
        || m_sceneArmedReported || !m_ws) return;
    const qint64 uncertainty = m_ws->sceneClockUncertaintyMs();
	qint64 maximum = -1;
	if (!readBoundedInt64(m_ws->serverPolicy(), "sceneMaxClockSkewMs",
						0, kMaxSafeJsonInteger, maximum)
		|| uncertainty < 0 || uncertainty > maximum) {
        sendPrepareResult(false, QStringLiteral("Clock synchronization uncertainty exceeds policy"));
        return;
    }
    m_sceneArmedReported = m_ws->sendSceneArmed(m_pendingSceneInstanceId, uncertainty);
}

void RemoteSceneController::onSceneCommitEnvelope(const QJsonObject& envelope)
{
    if (!matchesSceneEnvelope(envelope) || !m_sceneArmedReported || !m_ws) return;
	qint64 startServerMonotonicMs = -1;
	qint64 maximum = -1;
	qint64 startEpochMs = -1;
	qint64 policyMaximum = -1;
	qint64 activationLeadMs = -1;
	if (!readBoundedInt64(envelope, "startServerMonotonicMs",
						0, kMaxSafeJsonInteger, startServerMonotonicMs)
		|| !readBoundedInt64(envelope, "maximumClockUncertaintyMs",
						0, kMaxSafeJsonInteger, maximum)
		|| !readBoundedInt64(envelope, "startEpochMs",
						1, kMaxSafeJsonInteger, startEpochMs)
		|| !readBoundedInt64(m_ws->serverPolicy(), "sceneMaxClockSkewMs",
						0, kMaxSafeJsonInteger, policyMaximum)
		|| !readBoundedInt64(m_ws->serverPolicy(), "sceneActivationLeadMs",
						1, std::numeric_limits<int>::max(), activationLeadMs)
		|| maximum != policyMaximum) {
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
    const QJsonObject& snapshot,
    quint64 sequence,
    qint64 sampleAgeMs)
{
    auto reject = [this, sequence](const QString& reason) {
        qWarning() << "RemoteSceneController: rejecting authoritative state snapshot:"
                   << reason;
        emit authoritativeSnapshotRejected(sequence, reason);
        return false;
    };

    if (!m_enabled || m_pendingSceneInstanceId.isEmpty() || m_teardownInProgress) {
        return reject(QStringLiteral("No active renderer graph"));
    }
    if (sequence < 1 || sequence > 9007199254740991ULL
        || sequence <= static_cast<quint64>(m_lastVideoSyncSequence)) {
        return reject(QStringLiteral("Snapshot sequence is stale"));
    }
    if (sampleAgeMs < 0 || sampleAgeMs > kMaxSafeJsonInteger) {
        return reject(QStringLiteral("Snapshot sample age is invalid"));
    }

    const QJsonValue sceneValue = snapshot.value(QStringLiteral("scene"));
    const QJsonValue videosValue = snapshot.value(QStringLiteral("videos"));
    double capturedEpochMs = 0.0;
    if (!sceneValue.isObject() || !videosValue.isArray()
        || !readFiniteNumber(snapshot, "capturedEpochMs", capturedEpochMs)
		|| capturedEpochMs < 0.0
		|| capturedEpochMs > static_cast<double>(kMaxSafeJsonInteger)
		|| std::floor(capturedEpochMs) != capturedEpochMs) {
        return reject(QStringLiteral("Snapshot is incomplete"));
    }

    const QJsonObject scene = sceneValue.toObject();
    int schemaVersion = 0;
    if (!readBoundedInteger(scene, "renderSchemaVersion", 2, 2, schemaVersion)
        || !scene.value(QStringLiteral("screens")).isArray()
        || !scene.value(QStringLiteral("media")).isArray()) {
        return reject(QStringLiteral("Scene snapshot schema is invalid"));
    }

    const QJsonArray screens = scene.value(QStringLiteral("screens")).toArray();
    if (screens.size() != m_screenWindows.size()) {
        return reject(QStringLiteral("Scene snapshot screen set is incomplete"));
    }
    QSet<int> snapshotScreenIds;
    for (qsizetype index = 0; index < screens.size(); ++index) {
        if (!screens.at(index).isObject()) {
            return reject(QStringLiteral("Scene snapshot contains a malformed screen"));
        }
        const QJsonObject screen = screens.at(index).toObject();
        int screenId = -1;
        int x = 0;
        int y = 0;
        int width = 0;
        int height = 0;
        if (!readBoundedInteger(screen, "id", 0, 1000000, screenId)
            || !readBoundedInteger(screen, "x", -100000000, 100000000, x)
            || !readBoundedInteger(screen, "y", -100000000, 100000000, y)
            || !readBoundedInteger(screen, "width", 1, 100000, width)
            || !readBoundedInteger(screen, "height", 1, 100000, height)
            || !screen.value(QStringLiteral("primary")).isBool()
            || snapshotScreenIds.contains(screenId)
			|| !m_screenWindows.contains(screenId)
			|| screen != m_screenWindows.value(screenId).sourceScreenDefinition) {
            return reject(QStringLiteral("Scene snapshot screen identity is invalid"));
        }
        snapshotScreenIds.insert(screenId);
    }

    QHash<QString, std::shared_ptr<RemoteMediaItem>> currentByMediaId;
    int expectedVideoCount = 0;
    for (const auto& item : std::as_const(m_mediaItems)) {
        if (!item || item->mediaId.isEmpty() || currentByMediaId.contains(item->mediaId)) {
            return reject(QStringLiteral("Current renderer graph has invalid media identity"));
        }
        currentByMediaId.insert(item->mediaId, item);
        if (item->type == QLatin1String("video")) ++expectedVideoCount;
    }

    const QJsonArray mediaArray = scene.value(QStringLiteral("media")).toArray();
    if (mediaArray.size() != currentByMediaId.size()) {
        return reject(QStringLiteral("Scene snapshot media set is incomplete"));
    }

    struct StagedMediaState {
        std::shared_ptr<RemoteMediaItem> item;
        QJsonObject state;
        QHash<int, QJsonObject> spansByScreen;
        QJsonObject video;
    };
    QList<StagedMediaState> staged;
    staged.reserve(mediaArray.size());
    QHash<QString, qsizetype> stagedIndexByMediaId;

    auto requiredBool = [](const QJsonObject& object, const char* key) {
        return object.value(QLatin1String(key)).isBool();
    };
    auto requiredString = [](const QJsonObject& object, const char* key,
                             int maximumLength = 4096) {
        const QJsonValue value = object.value(QLatin1String(key));
        return value.isString() && value.toString().size() <= maximumLength;
    };
    auto requiredFinite = [](const QJsonObject& object, const char* key,
                             double minimum, double maximum) {
        double value = 0.0;
        return readFiniteNumber(object, key, value)
            && value >= minimum && value <= maximum;
    };

    for (qsizetype index = 0; index < mediaArray.size(); ++index) {
        if (!mediaArray.at(index).isObject()) {
            return reject(QStringLiteral("Scene snapshot contains malformed media"));
        }
        const QJsonObject state = mediaArray.at(index).toObject();
        const QString mediaId = state.value(QStringLiteral("mediaId")).toString();
        const auto currentIt = currentByMediaId.constFind(mediaId);
        if (!state.value(QStringLiteral("mediaId")).isString()
            || mediaId.isEmpty() || mediaId.size() > kMaxRemoteIdentifierLength
            || currentIt == currentByMediaId.cend()
            || stagedIndexByMediaId.contains(mediaId)) {
            return reject(QStringLiteral("Scene snapshot media identity is invalid"));
        }
        const std::shared_ptr<RemoteMediaItem> item = currentIt.value();
        if (!requiredString(state, "type", 16)
            || state.value(QStringLiteral("type")).toString() != item->type
            || !requiredString(state, "fileId", kMaxRemoteIdentifierLength)
            || state.value(QStringLiteral("fileId")).toString() != item->fileId) {
            return reject(QStringLiteral("Scene snapshot attempts to replace media identity"));
        }

        double x = 0.0;
        double y = 0.0;
        double width = 0.0;
        double height = 0.0;
        int baseWidth = 0;
        int baseHeight = 0;
        int repeatCount = 0;
        int automationDelay = 0;
        if (!readFiniteRect(state, "x", "y", "width", "height",
                            x, y, width, height)
            || std::abs(x) > kMaxRemoteCoordinate
            || std::abs(y) > kMaxRemoteCoordinate
            || width <= 0.0 || width > kMaxRemoteDimension
            || height <= 0.0 || height > kMaxRemoteDimension
            || !readBoundedInteger(state, "baseWidth", 0,
                                   static_cast<int>(kMaxRemoteDimension), baseWidth)
            || !readBoundedInteger(state, "baseHeight", 0,
                                   static_cast<int>(kMaxRemoteDimension), baseHeight)
            || !requiredFinite(state, "z", -kMaxRemoteCoordinate, kMaxRemoteCoordinate)
            || !requiredFinite(state, "contentOpacity", 0.0, 1.0)
            || !requiredBool(state, "visible")
            || !requiredBool(state, "autoDisplay")
            || !requiredBool(state, "autoHide")
            || !requiredBool(state, "hideWhenVideoEnds")
            || !readBoundedInteger(state, "autoDisplayDelayMs",
                                   -static_cast<int>(kMaxVideoPositionMs),
                                   static_cast<int>(kMaxVideoPositionMs), automationDelay)
            || !readBoundedInteger(state, "autoHideDelayMs",
                                   -static_cast<int>(kMaxVideoPositionMs),
                                   static_cast<int>(kMaxVideoPositionMs), automationDelay)
            || !requiredFinite(state, "fadeInSeconds", 0.0, 3600.0)
            || !requiredFinite(state, "fadeOutSeconds", 0.0, 3600.0)) {
            return reject(QStringLiteral("Scene snapshot media state is malformed"));
        }

        const QJsonValue spansValue = state.value(QStringLiteral("spans"));
        if (!spansValue.isArray()) {
            return reject(QStringLiteral("Scene snapshot media spans are missing"));
        }
        const QJsonArray spans = spansValue.toArray();
        if (spans.size() != item->spans.size()) {
            return reject(QStringLiteral("Scene snapshot would reconstruct media spans"));
        }
        QSet<int> currentSpanScreens;
        for (const RemoteMediaItem::Span& span : item->spans) {
            currentSpanScreens.insert(span.screenId);
        }
        QHash<int, QJsonObject> spansByScreen;
        for (const QJsonValue& spanValue : spans) {
            if (!spanValue.isObject()) {
                return reject(QStringLiteral("Scene snapshot contains a malformed span"));
            }
            const QJsonObject span = spanValue.toObject();
            int screenId = -1;
            double normX = 0.0;
            double normY = 0.0;
            double normWidth = 0.0;
            double normHeight = 0.0;
            double destX = 0.0;
            double destY = 0.0;
            double destWidth = 0.0;
            double destHeight = 0.0;
            double sourceX = 0.0;
            double sourceY = 0.0;
            double sourceWidth = 0.0;
            double sourceHeight = 0.0;
            if (!readBoundedInteger(span, "screenId", 0, 1000000, screenId)
                || !currentSpanScreens.contains(screenId)
                || spansByScreen.contains(screenId)
                || !readFiniteRect(span, "normX", "normY", "normW", "normH",
                                   normX, normY, normWidth, normHeight)
                || std::abs(normX) > kMaxLegacyNormalizedMagnitude
                || std::abs(normY) > kMaxLegacyNormalizedMagnitude
                || normWidth <= 0.0 || normWidth > kMaxLegacyNormalizedMagnitude
                || normHeight <= 0.0 || normHeight > kMaxLegacyNormalizedMagnitude
                || !readFiniteRect(span,
                                   "spanDestNormX", "spanDestNormY",
                                   "spanDestNormW", "spanDestNormH",
                                   destX, destY, destWidth, destHeight)
                || !isValidUnitRect(destX, destY, destWidth, destHeight)
                || !readFiniteRect(span,
                                   "spanSourceNormX", "spanSourceNormY",
                                   "spanSourceNormW", "spanSourceNormH",
                                   sourceX, sourceY, sourceWidth, sourceHeight)
                || !isValidUnitRect(sourceX, sourceY, sourceWidth, sourceHeight)) {
                return reject(QStringLiteral("Scene snapshot span topology is invalid"));
            }
            spansByScreen.insert(screenId, span);
        }

        if (item->type == QLatin1String("text")) {
            int fontSize = 0;
            int fontWeight = 0;
            int fontPixelSize = 0;
            const QString horizontal =
                state.value(QStringLiteral("horizontalAlignment")).toString().toLower();
            const QString vertical =
                state.value(QStringLiteral("verticalAlignment")).toString().toLower();
            if (!requiredString(state, "text", 1000000)
                || !requiredString(state, "fontFamily", 1024)
                || !readBoundedInteger(state, "fontSize", 1, 1000, fontSize)
                || !requiredBool(state, "fontBold")
                || !requiredBool(state, "fontItalic")
                || !requiredBool(state, "fontUnderline")
                || !requiredBool(state, "fontUppercase")
                || !readBoundedInteger(state, "fontWeight", 0, 900, fontWeight)
                || !readBoundedInteger(state, "fontPixelSize", 0, 4096, fontPixelSize)
                || !requiredString(state, "textColor", 64)
                || !requiredFinite(state, "textBorderWidthPercent", 0.0, 1000.0)
                || !requiredFinite(state, "textOutlineWidthPx", 0.0, 100000.0)
                || !requiredString(state, "textBorderColor", 64)
                || !requiredBool(state, "textFitToTextEnabled")
                || !requiredBool(state, "textHighlightEnabled")
                || !requiredString(state, "textHighlightColor", 64)
                || !requiredFinite(state, "uniformScale", -100000.0, 100000.0)
                || std::abs(state.value(QStringLiteral("uniformScale")).toDouble()) < 1e-6
                || (horizontal != QLatin1String("left")
                    && horizontal != QLatin1String("center")
                    && horizontal != QLatin1String("right"))
                || (vertical != QLatin1String("top")
                    && vertical != QLatin1String("center")
                    && vertical != QLatin1String("bottom"))) {
                return reject(QStringLiteral("Scene snapshot text state is malformed"));
            }
        } else if (item->type == QLatin1String("video")) {
            int delay = 0;
			// The commit path delegates runtime seek/play state to the existing
			// video synchronizer. Guarantee its complete graph precondition here,
			// before any text/image/QML mutation begins.
			if (!item->player || !item->audio
				|| !requiredBool(state, "autoPlay")
                || !requiredBool(state, "autoPause")
                || !requiredBool(state, "muted")
                || !requiredBool(state, "continuousLoop")
                || !requiredBool(state, "repeatEnabled")
                || !requiredBool(state, "autoUnmute")
                || !requiredBool(state, "autoMute")
                || !requiredBool(state, "muteWhenVideoEnds")
                || !readBoundedInteger(state, "repeatCount", 0, 1000000, repeatCount)
                || !readBoundedInteger(state, "autoPlayDelayMs",
                                       -static_cast<int>(kMaxVideoPositionMs),
                                       static_cast<int>(kMaxVideoPositionMs), delay)
                || !readBoundedInteger(state, "autoPauseDelayMs",
                                       -static_cast<int>(kMaxVideoPositionMs),
                                       static_cast<int>(kMaxVideoPositionMs), delay)
                || !readBoundedInteger(state, "autoUnmuteDelayMs",
                                       -static_cast<int>(kMaxVideoPositionMs),
                                       static_cast<int>(kMaxVideoPositionMs), delay)
                || !readBoundedInteger(state, "autoMuteDelayMs",
                                       -static_cast<int>(kMaxVideoPositionMs),
                                       static_cast<int>(kMaxVideoPositionMs), delay)
                || !requiredFinite(state, "volume", 0.0, 1.0)
                || !requiredFinite(state, "audioFadeInSeconds", 0.0, 3600.0)
                || !requiredFinite(state, "audioFadeOutSeconds", 0.0, 3600.0)) {
                return reject(QStringLiteral("Scene snapshot video configuration is malformed"));
            }
        }

        stagedIndexByMediaId.insert(mediaId, staged.size());
        staged.append({item, state, spansByScreen, {}});
    }

    const QJsonArray videos = videosValue.toArray();
    if (videos.size() != expectedVideoCount) {
        return reject(QStringLiteral("Video state set is incomplete"));
    }
    QSet<QString> seenVideoIds;
    for (const QJsonValue& videoValue : videos) {
        if (!videoValue.isObject()) {
            return reject(QStringLiteral("Video state contains a malformed entry"));
        }
        const QJsonObject video = videoValue.toObject();
        const QString mediaId = video.value(QStringLiteral("mediaId")).toString();
        const auto stagedIt = stagedIndexByMediaId.constFind(mediaId);
        double position = 0.0;
        double duration = 0.0;
        if (!video.value(QStringLiteral("mediaId")).isString()
            || stagedIt == stagedIndexByMediaId.cend()
            || staged.at(stagedIt.value()).item->type != QLatin1String("video")
            || seenVideoIds.contains(mediaId)
            || !readFiniteNumber(video, "positionMs", position)
            || position < 0.0 || position > static_cast<double>(kMaxVideoPositionMs)
            || !readFiniteNumber(video, "durationMs", duration)
            || duration < 0.0 || duration > static_cast<double>(kMaxVideoPositionMs)
            || !requiredBool(video, "playing")
            || !requiredBool(video, "muted")
            || !requiredBool(video, "visible")
            || !requiredBool(video, "repeatAvailable")
            || video.value(QStringLiteral("visible")).toBool()
                != staged.at(stagedIt.value()).state
                       .value(QStringLiteral("visible")).toBool()
            || video.value(QStringLiteral("muted")).toBool()
                != staged.at(stagedIt.value()).state
                       .value(QStringLiteral("muted")).toBool()) {
            return reject(QStringLiteral("Video state is incomplete or ambiguous"));
        }
        seenVideoIds.insert(mediaId);
        staged[stagedIt.value()].video = video;
    }

    // Commit begins only after the entire scene and every video state have
    // validated. No object, QML row or player is touched above this point.
    for (StagedMediaState& stagedState : staged) {
        const QJsonObject& state = stagedState.state;
        const std::shared_ptr<RemoteMediaItem>& item = stagedState.item;
        item->baseWidth = state.value(QStringLiteral("baseWidth")).toInt();
        item->baseHeight = state.value(QStringLiteral("baseHeight")).toInt();
        item->z = state.value(QStringLiteral("z")).toDouble();
        item->contentOpacity = state.value(QStringLiteral("contentOpacity")).toDouble();
        item->autoDisplay = state.value(QStringLiteral("autoDisplay")).toBool();
        item->autoDisplayDelayMs = state.value(QStringLiteral("autoDisplayDelayMs")).toInt();
        item->autoHide = state.value(QStringLiteral("autoHide")).toBool();
        item->autoHideDelayMs = state.value(QStringLiteral("autoHideDelayMs")).toInt();
        item->hideWhenVideoEnds = state.value(QStringLiteral("hideWhenVideoEnds")).toBool();
        item->fadeInSeconds = state.value(QStringLiteral("fadeInSeconds")).toDouble();
        item->fadeOutSeconds = state.value(QStringLiteral("fadeOutSeconds")).toDouble();

        for (RemoteMediaItem::Span& span : item->spans) {
            const QJsonObject source = stagedState.spansByScreen.value(span.screenId);
            span.nx = source.value(QStringLiteral("normX")).toDouble();
            span.ny = source.value(QStringLiteral("normY")).toDouble();
            span.nw = source.value(QStringLiteral("normW")).toDouble();
            span.nh = source.value(QStringLiteral("normH")).toDouble();
            span.destNx = source.value(QStringLiteral("spanDestNormX")).toDouble();
            span.destNy = source.value(QStringLiteral("spanDestNormY")).toDouble();
            span.destNw = source.value(QStringLiteral("spanDestNormW")).toDouble();
            span.destNh = source.value(QStringLiteral("spanDestNormH")).toDouble();
            span.srcNx = source.value(QStringLiteral("spanSourceNormX")).toDouble();
            span.srcNy = source.value(QStringLiteral("spanSourceNormY")).toDouble();
            span.srcNw = source.value(QStringLiteral("spanSourceNormW")).toDouble();
            span.srcNh = source.value(QStringLiteral("spanSourceNormH")).toDouble();
        }

        if (item->type == QLatin1String("text")) {
            item->text = state.value(QStringLiteral("text")).toString();
            item->fontFamily = state.value(QStringLiteral("fontFamily")).toString();
            item->fontSize = state.value(QStringLiteral("fontSize")).toInt();
            item->fontBold = state.value(QStringLiteral("fontBold")).toBool();
            item->fontItalic = state.value(QStringLiteral("fontItalic")).toBool();
            item->fontUnderline = state.value(QStringLiteral("fontUnderline")).toBool();
            item->fontUppercase = state.value(QStringLiteral("fontUppercase")).toBool();
            item->fontWeight = state.value(QStringLiteral("fontWeight")).toInt();
            item->fontPixelSize = state.value(QStringLiteral("fontPixelSize")).toInt();
            item->textColor = state.value(QStringLiteral("textColor")).toString();
            item->textBorderWidthPercent =
                state.value(QStringLiteral("textBorderWidthPercent")).toDouble();
            item->textOutlineWidthPx =
                state.value(QStringLiteral("textOutlineWidthPx")).toDouble();
            item->textBorderColor = state.value(QStringLiteral("textBorderColor")).toString();
            item->fitToTextEnabled =
                state.value(QStringLiteral("textFitToTextEnabled")).toBool();
            item->highlightEnabled =
                state.value(QStringLiteral("textHighlightEnabled")).toBool();
            item->textHighlightColor =
                state.value(QStringLiteral("textHighlightColor")).toString();
            item->uniformScale = state.value(QStringLiteral("uniformScale")).toDouble();
            const QString horizontal =
                state.value(QStringLiteral("horizontalAlignment")).toString().toLower();
            item->horizontalAlignment = horizontal == QLatin1String("left")
                ? RemoteMediaItem::HorizontalAlignment::Left
                : (horizontal == QLatin1String("right")
                       ? RemoteMediaItem::HorizontalAlignment::Right
                       : RemoteMediaItem::HorizontalAlignment::Center);
            const QString vertical =
                state.value(QStringLiteral("verticalAlignment")).toString().toLower();
            item->verticalAlignment = vertical == QLatin1String("top")
                ? RemoteMediaItem::VerticalAlignment::Top
                : (vertical == QLatin1String("bottom")
                       ? RemoteMediaItem::VerticalAlignment::Bottom
                       : RemoteMediaItem::VerticalAlignment::Center);
        } else if (item->type == QLatin1String("video")) {
            item->autoPlay = state.value(QStringLiteral("autoPlay")).toBool();
            item->autoPlayDelayMs = state.value(QStringLiteral("autoPlayDelayMs")).toInt();
            item->autoPause = state.value(QStringLiteral("autoPause")).toBool();
            item->autoPauseDelayMs = state.value(QStringLiteral("autoPauseDelayMs")).toInt();
            item->continuousLoop = state.value(QStringLiteral("continuousLoop")).toBool();
            item->repeatEnabled = state.value(QStringLiteral("repeatEnabled")).toBool();
            item->repeatCount = state.value(QStringLiteral("repeatCount")).toInt();
            item->volume = state.value(QStringLiteral("volume")).toDouble();
            item->autoUnmute = state.value(QStringLiteral("autoUnmute")).toBool();
            item->autoUnmuteDelayMs = state.value(QStringLiteral("autoUnmuteDelayMs")).toInt();
            item->autoMute = state.value(QStringLiteral("autoMute")).toBool();
            item->autoMuteDelayMs = state.value(QStringLiteral("autoMuteDelayMs")).toInt();
            item->muteWhenVideoEnds =
                state.value(QStringLiteral("muteWhenVideoEnds")).toBool();
            item->audioFadeInSeconds =
                state.value(QStringLiteral("audioFadeInSeconds")).toDouble();
            item->audioFadeOutSeconds =
                state.value(QStringLiteral("audioFadeOutSeconds")).toDouble();
        }

        const bool visible = item->type == QLatin1String("video")
            ? stagedState.video.value(QStringLiteral("visible")).toBool()
            : state.value(QStringLiteral("visible")).toBool();
        item->contentVisible = visible;
        item->displayReady = visible;
        item->displayStarted = visible;
        item->hiding = false;
        updatePublishedMediaItem(item);
        setRemoteMediaVisualState(item, visible ? item->contentOpacity : 0.0, visible);
    }

    if (expectedVideoCount > 0) {
        const qint64 approximateSampleEpoch =
            QDateTime::currentMSecsSinceEpoch() - sampleAgeMs;
        onRemoteSceneVideoSync(m_pendingSenderClientId, m_pendingSceneInstanceId,
                               static_cast<qint64>(sequence), approximateSampleEpoch,
                               videos);
    } else {
        m_lastVideoSyncSequence = static_cast<qint64>(sequence);
    }
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
		it->firstFrameSwapsRemaining = 0;
	}
}

void RemoteSceneController::onRemoteSceneStart(const QString& senderClientId, const QJsonObject& scene) {
    if (!m_enabled) return;

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

    if (sceneInstanceId.isEmpty()) {
        rejectStart(QStringLiteral("Scene instance identifier is missing"));
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
    const QList<QScreen*> localScreens = QGuiApplication::screens();
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
            || (screenObject.contains(QStringLiteral("primary"))
                && !screenObject.value(QStringLiteral("primary")).isBool())
            || declaredScreenIds.contains(screenId)) {
            failWithMessage(QStringLiteral("Invalid or duplicate screen declaration at index %1")
                                .arg(screenIndex));
            return;
        }
        declaredScreenIds.insert(screenId);
    }

    QSet<QString> declaredMediaIds;
    int totalSpanCount = 0;
    for (qsizetype mediaIndex = 0; mediaIndex < media.size(); ++mediaIndex) {
        const QJsonValue mediaValue = media.at(mediaIndex);
        if (!mediaValue.isObject()) {
            failWithMessage(QStringLiteral("Invalid media entry at index %1").arg(mediaIndex));
            return;
        }

        const QJsonObject mediaObject = mediaValue.toObject();
        const QJsonValue mediaIdValue = mediaObject.value(QStringLiteral("mediaId"));
        const QJsonValue typeValue = mediaObject.value(QStringLiteral("type"));
        const QString mediaId = mediaIdValue.toString();
        const QString type = typeValue.toString();
        if (!mediaIdValue.isString()
            || mediaId.isEmpty()
            || mediaId.size() > kMaxRemoteIdentifierLength
            || declaredMediaIds.contains(mediaId)
            || !typeValue.isString()
            || (type != QLatin1String("text")
                && type != QLatin1String("image")
                && type != QLatin1String("video"))) {
            failWithMessage(QStringLiteral("Invalid or duplicate media declaration at index %1")
                                .arg(mediaIndex));
            return;
        }
        declaredMediaIds.insert(mediaId);

        double mediaX = 0.0;
        double mediaY = 0.0;
        double mediaWidth = 0.0;
        double mediaHeight = 0.0;
        if (!readFiniteRect(mediaObject, "x", "y", "width", "height",
                            mediaX, mediaY, mediaWidth, mediaHeight)
            || std::abs(mediaX) > kMaxRemoteCoordinate
            || std::abs(mediaY) > kMaxRemoteCoordinate
            || mediaWidth <= 0.0
            || mediaHeight <= 0.0
            || mediaWidth > kMaxRemoteDimension
            || mediaHeight > kMaxRemoteDimension) {
            failWithMessage(QStringLiteral("Invalid geometry for media %1").arg(mediaId));
            return;
        }

        for (const char* dimensionKey : {"baseWidth", "baseHeight"}) {
            if (!mediaObject.contains(QLatin1String(dimensionKey))) continue;
            double dimension = 0.0;
            if (!readFiniteNumber(mediaObject, dimensionKey, dimension)
                || std::floor(dimension) != dimension
                || dimension < 0.0
                || dimension > kMaxRemoteDimension) {
                failWithMessage(QStringLiteral("Invalid base dimensions for media %1").arg(mediaId));
                return;
            }
        }
        if ((mediaObject.contains(QStringLiteral("visible"))
             && !mediaObject.value(QStringLiteral("visible")).isBool())) {
            failWithMessage(QStringLiteral("Invalid visibility for media %1").arg(mediaId));
            return;
        }
        if (mediaObject.contains(QStringLiteral("z"))) {
            double z = 0.0;
            if (!readFiniteNumber(mediaObject, "z", z)
                || std::abs(z) > kMaxRemoteCoordinate) {
                failWithMessage(QStringLiteral("Invalid stacking value for media %1").arg(mediaId));
                return;
            }
        }

        const auto optionalBooleanIsValid = [&](const char* key) {
            const QJsonValue value = mediaObject.value(QLatin1String(key));
            return value.isUndefined() || value.isBool();
        };
        for (const char* booleanKey : {
                 "autoDisplay", "autoPlay", "autoPause", "autoHide",
                 "hideWhenVideoEnds", "continuousLoop", "repeatEnabled",
                 "muted", "autoUnmute", "autoMute", "muteWhenVideoEnds"}) {
            if (!optionalBooleanIsValid(booleanKey)) {
                failWithMessage(QStringLiteral("Invalid state field for media %1").arg(mediaId));
                return;
            }
        }

        for (const char* delayKey : {
                 "autoDisplayDelayMs", "autoPlayDelayMs", "autoPauseDelayMs",
                 "autoHideDelayMs", "autoUnmuteDelayMs", "autoMuteDelayMs"}) {
            if (!mediaObject.contains(QLatin1String(delayKey))) continue;
            double delay = 0.0;
            if (!readFiniteNumber(mediaObject, delayKey, delay)
                || std::floor(delay) != delay
                || std::abs(delay) > static_cast<double>(kMaxVideoPositionMs)) {
                failWithMessage(QStringLiteral("Invalid automation delay for media %1").arg(mediaId));
                return;
            }
        }

        for (const char* fadeKey : {
                 "fadeInSeconds", "fadeOutSeconds",
                 "audioFadeInSeconds", "audioFadeOutSeconds"}) {
            if (!mediaObject.contains(QLatin1String(fadeKey))) continue;
            double seconds = 0.0;
            if (!readFiniteNumber(mediaObject, fadeKey, seconds)
                || seconds < 0.0 || seconds > 3600.0) {
                failWithMessage(QStringLiteral("Invalid fade duration for media %1").arg(mediaId));
                return;
            }
        }

        for (const char* unitKey : {"contentOpacity", "volume"}) {
            if (!mediaObject.contains(QLatin1String(unitKey))) continue;
            double value = 0.0;
            if (!readFiniteNumber(mediaObject, unitKey, value)
                || value < 0.0 || value > 1.0) {
                failWithMessage(QStringLiteral("Invalid normalized state for media %1").arg(mediaId));
                return;
            }
        }

        if (mediaObject.contains(QStringLiteral("repeatCount"))) {
            int repeatCount = 0;
            if (!readBoundedInteger(mediaObject, "repeatCount", 0, 1000000, repeatCount)) {
                failWithMessage(QStringLiteral("Invalid repeat count for media %1").arg(mediaId));
                return;
            }
        }
        for (const char* positionKey : {"startPositionMs", "displayedFrameTimestampMs"}) {
            if (!mediaObject.contains(QLatin1String(positionKey))) continue;
            double position = 0.0;
            if (!readFiniteNumber(mediaObject, positionKey, position)
                || position < 0.0
                || position > static_cast<double>(kMaxVideoPositionMs)) {
                failWithMessage(QStringLiteral("Invalid video position for media %1").arg(mediaId));
                return;
            }
        }

        const QJsonValue spansValue = mediaObject.value(QStringLiteral("spans"));
        if (!spansValue.isArray()) {
            failWithMessage(QStringLiteral("Media %1 has no valid screen span").arg(mediaId));
            return;
        }
        const QJsonArray spans = spansValue.toArray();
        if (spans.isEmpty() || spans.size() > kMaxRemoteSpansPerMedia
            || totalSpanCount > kMaxRemoteTotalSpans - spans.size()) {
            failWithMessage(QStringLiteral("Invalid span count for media %1").arg(mediaId));
            return;
        }
        totalSpanCount += static_cast<int>(spans.size());

        QSet<int> mediaScreenIds;
        for (qsizetype spanIndex = 0; spanIndex < spans.size(); ++spanIndex) {
            const QJsonValue spanValue = spans.at(spanIndex);
            if (!spanValue.isObject()) {
                failWithMessage(QStringLiteral("Invalid span %1 for media %2")
                                    .arg(spanIndex).arg(mediaId));
                return;
            }

            const QJsonObject spanObject = spanValue.toObject();
            int screenId = -1;
            double normX = 0.0;
            double normY = 0.0;
            double normWidth = 0.0;
            double normHeight = 0.0;
            if (!readBoundedInteger(spanObject, "screenId", 0, 1000000, screenId)
                || !declaredScreenIds.contains(screenId)
                || mediaScreenIds.contains(screenId)
                || !readFiniteRect(spanObject, "normX", "normY", "normW", "normH",
                                   normX, normY, normWidth, normHeight)
                || std::abs(normX) > kMaxLegacyNormalizedMagnitude
                || std::abs(normY) > kMaxLegacyNormalizedMagnitude
                || normWidth <= 0.0
                || normHeight <= 0.0
                || normWidth > kMaxLegacyNormalizedMagnitude
                || normHeight > kMaxLegacyNormalizedMagnitude) {
                failWithMessage(QStringLiteral("Invalid span %1 for media %2")
                                    .arg(spanIndex).arg(mediaId));
                return;
            }
            mediaScreenIds.insert(screenId);

            const bool hasDestination = spanObject.contains(QStringLiteral("spanDestNormX"))
                || spanObject.contains(QStringLiteral("spanDestNormY"))
                || spanObject.contains(QStringLiteral("spanDestNormW"))
                || spanObject.contains(QStringLiteral("spanDestNormH"));
            if (hasDestination) {
                double x = 0.0;
                double y = 0.0;
                double width = 0.0;
                double height = 0.0;
                if (!readFiniteRect(spanObject,
                                    "spanDestNormX", "spanDestNormY",
                                    "spanDestNormW", "spanDestNormH",
                                    x, y, width, height)
                    || !isValidUnitRect(x, y, width, height)) {
                    failWithMessage(QStringLiteral("Invalid destination span %1 for media %2")
                                        .arg(spanIndex).arg(mediaId));
                    return;
                }
            }

            const bool hasSource = spanObject.contains(QStringLiteral("spanSourceNormX"))
                || spanObject.contains(QStringLiteral("spanSourceNormY"))
                || spanObject.contains(QStringLiteral("spanSourceNormW"))
                || spanObject.contains(QStringLiteral("spanSourceNormH"));
            if (hasSource) {
                double x = 0.0;
                double y = 0.0;
                double width = 0.0;
                double height = 0.0;
                if (!readFiniteRect(spanObject,
                                    "spanSourceNormX", "spanSourceNormY",
                                    "spanSourceNormW", "spanSourceNormH",
                                    x, y, width, height)
                    || !isValidUnitRect(x, y, width, height)) {
                    failWithMessage(QStringLiteral("Invalid source span %1 for media %2")
                                        .arg(spanIndex).arg(mediaId));
                    return;
                }
            }
        }
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
        const QString path = m_fileManager
            ? m_fileManager->getFilePathForId(fileId) : QString();
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

    const MediaFilePolicy::PreparationValidationResult preparation =
        MediaFilePolicy::validatePreparationAssets(preparationAssets);
    if (!preparation.accepted) {
        if (preparation.errorCode == QLatin1String("decoded_rgba_budget_exceeded")) {
            failWithMessage(QStringLiteral(
                "Scene preparation exceeds the 1 GiB decoded RGBA image budget"));
            return;
        }

        const QString fileName = preparationFileNames
            .value(preparation.failedAssetId, preparation.failedAssetId);
        if (preparationTypes.value(preparation.failedAssetId)
            == QLatin1String("video")) {
            failWithMessage(QStringLiteral(
                "Unsupported video format or codec (MP4 required): %1").arg(fileName));
        } else {
            failWithMessage(QStringLiteral("Invalid image asset %1 (%2)")
                                .arg(fileName,
                                     preparation.errorCode.isEmpty()
                                         ? QStringLiteral("media_validation_failed")
                                         : preparation.errorCode));
        }
        return;
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
        if (!window.quickWidget || window.quickWidget->status() != QQuickWidget::Ready
            || !window.quickWidget->rootObject() || !window.mediaModel) {
            failWithMessage(QStringLiteral("Qt Quick remote renderer failed to initialize"));
            ++m_sceneEpoch;
            clearScene();
            return;
        }
    }
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
    m_windowShowTimer->setInterval(10);
    
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
    const qint64 activationLeadMs = policy
        .value(QStringLiteral("sceneActivationLeadMs")).toInt();
    const qint64 maximumClockSkewMs = policy
        .value(QStringLiteral("sceneMaxClockSkewMs")).toInt(-1);
    const qint64 leaseTimeoutMs = policy
        .value(QStringLiteral("leaseTimeoutMs")).toInt();
    if (activationLeadMs <= 0 || maximumClockSkewMs < 0 || leaseTimeoutMs <= 0) {
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
	// ACTIVATE is a commit. Keep only the server-policy-derived lease bound
	// until its timer fires; no independent client deadline may pre-empt it.
    if (m_sceneReadyTimeout) {
        m_sceneReadyTimeout->start(static_cast<int>(remainingMs + leaseTimeoutMs));
    }
    if (remainingMs <= 0) {
        QMetaObject::invokeMethod(this, &RemoteSceneController::activateScene, Qt::QueuedConnection);
    } else {
        m_activationTimer->start(static_cast<int>(remainingMs));
    }
}

void RemoteSceneController::onRemoteSceneVideoSync(const QString& senderClientId,
                                                   const QString& sceneInstanceId,
                                                   qint64 sequence,
                                                   qint64 sampledEpochMs,
                                                   const QJsonArray& videos) {
    if (QThread::currentThread() != thread()) {
        QMetaObject::invokeMethod(this,
            [this, senderClientId, sceneInstanceId, sequence, sampledEpochMs, videos]() {
                onRemoteSceneVideoSync(
                    senderClientId, sceneInstanceId, sequence, sampledEpochMs, videos);
            },
            Qt::QueuedConnection);
        return;
    }

    if (!m_enabled) return;
    if (senderClientId != m_pendingSenderClientId
        || sceneInstanceId.isEmpty()
        || sceneInstanceId != m_pendingSceneInstanceId) {
        qWarning() << "RemoteSceneController: ignoring foreign/stale video sync"
                   << sceneInstanceId;
        return;
    }
    if (sequence <= 0 || sequence <= m_lastVideoSyncSequence) return;

    const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
    const qint64 sampleAgeMs = sampledEpochMs > 0 ? nowMs - sampledEpochMs : -1;
    const bool canProjectTransit = m_activationClockPlausible
        && sampleAgeMs >= -m_ws->serverPolicy()
                              .value(QStringLiteral("sceneMaxClockSkewMs")).toInt()
        && sampleAgeMs <= kMaxVideoSyncTransitMs;
    const qint64 projectedTransitMs = canProjectTransit
        ? std::clamp<qint64>(sampleAgeMs, 0, kMaxVideoSyncTransitMs)
        : 0;

    QHash<QString, QJsonObject> stateByMediaId;
    const qsizetype stateCount = std::min<qsizetype>(
        videos.size(), static_cast<qsizetype>(kMaxVideoSyncItems));
    for (qsizetype i = 0; i < stateCount; ++i) {
        if (!videos.at(i).isObject()) continue;
        const QJsonObject state = videos.at(i).toObject();
        const QString mediaId = state.value("mediaId").toString();
        if (mediaId.isEmpty() || mediaId.size() > 128 || stateByMediaId.contains(mediaId)) continue;
        if (!state.value("positionMs").isDouble()
            || !state.value("durationMs").isDouble()
            || !state.value("playing").isBool()
            || !state.value("muted").isBool()
            || !state.value("visible").isBool()
            || !state.value("repeatAvailable").isBool()) {
            continue;
        }
        const double positionValue = state.value("positionMs").toDouble(-1.0);
        const double durationValue = state.value("durationMs").toDouble(-1.0);
        if (!std::isfinite(positionValue)
            || positionValue < 0.0
            || positionValue > static_cast<double>(kMaxVideoPositionMs)
            || !std::isfinite(durationValue)
            || durationValue < 0.0
            || durationValue > static_cast<double>(kMaxVideoPositionMs)) {
            continue;
        }
        stateByMediaId.insert(mediaId, state);
    }

    int expectedVideoCount = 0;
    int synchronizedVideoCount = 0;
    for (const auto& item : m_mediaItems) {
        if (!item || item->type != QLatin1String("video") || !item->player) continue;
        ++expectedVideoCount;
        if (stateByMediaId.contains(item->mediaId)) ++synchronizedVideoCount;
    }
    const bool completeSnapshot = expectedVideoCount > 0
        && synchronizedVideoCount == expectedVideoCount;
    if (!completeSnapshot) {
        qWarning() << "RemoteSceneController: ignoring incomplete video sync snapshot"
                   << synchronizedVideoCount << "/" << expectedVideoCount;
        return;
    }
    // Commit ordering only after the full snapshot validates. A malformed high
    // sequence must not poison the stream and block a later valid correction.
    m_lastVideoSyncSequence = sequence;
    for (const auto& item : m_mediaItems) {
        if (!item || item->type != QLatin1String("video") || !item->player) continue;
        const auto stateIt = stateByMediaId.constFind(item->mediaId);
        if (stateIt == stateByMediaId.cend()) continue;
        const QJsonObject state = stateIt.value();

        const bool playing = state.value("playing").toBool(false);
        const bool muted = state.value("muted").toBool(true);
        const bool visible = state.value("visible").toBool(false);
        const bool repeatAvailable = state.value("repeatAvailable").toBool(false);

        applyAudioMuteState(item, muted, true);

        if (item->contentVisible != visible) {
            if (item->visualFadeAnimation) {
                QVariantAnimation* animation = item->visualFadeAnimation.data();
                QObject::disconnect(animation, nullptr, this, nullptr);
                animation->stop();
                animation->deleteLater();
                item->visualFadeAnimation = nullptr;
            }
            item->contentVisible = visible;
            item->displayStarted = visible;
            item->displayReady = visible;
            item->hiding = false;
            setRemoteMediaVisualState(item, visible ? item->contentOpacity : 0.0, visible);
        }

        if (!playing) {
            item->playAuthorized = false;
            item->awaitingDecoderSync = false;
            item->awaitingLivePlayback = false;
            if (item->player->playbackState() == QMediaPlayer::PlayingState) {
                item->player->pause();
            }
        }

        const bool decoderTransitionInProgress = playing
            && (item->awaitingDecoderSync
                || (item->awaitingLivePlayback && !item->livePlaybackStarted));
        if (!decoderTransitionInProgress) {
            qint64 desiredPositionMs = static_cast<qint64>(
                std::llround(state.value("positionMs").toDouble(0.0)));
            qint64 durationMs = item->player->duration();
            if (durationMs <= 0) {
                const double durationValue = state.value("durationMs").toDouble(0.0);
                if (std::isfinite(durationValue)
                    && durationValue > 0.0
                    && durationValue <= static_cast<double>(kMaxVideoPositionMs)) {
                    durationMs = static_cast<qint64>(std::llround(durationValue));
                }
            }

            if (playing && projectedTransitMs > 0) {
                desiredPositionMs = std::min<qint64>(
                    kMaxVideoPositionMs, desiredPositionMs + projectedTransitMs);
            }
            if (durationMs > 0) {
                if (playing && repeatAvailable && desiredPositionMs >= durationMs) {
                    desiredPositionMs %= durationMs;
                } else {
                    desiredPositionMs = std::min(desiredPositionMs, durationMs);
                }
            }

            const qint64 currentPositionMs = std::max<qint64>(0, item->player->position());
            qint64 positionErrorMs = desiredPositionMs - currentPositionMs;
            if (playing && repeatAvailable && durationMs > 0) {
                const qint64 wrappedForward = positionErrorMs + durationMs;
                const qint64 wrappedBackward = positionErrorMs - durationMs;
                if (qAbs(wrappedForward) < qAbs(positionErrorMs)) positionErrorMs = wrappedForward;
                if (qAbs(wrappedBackward) < qAbs(positionErrorMs)) positionErrorMs = wrappedBackward;
            }

            if (qAbs(positionErrorMs) > kVideoSyncPositionToleranceMs) {
                item->repeatActive = false;
                item->lastRepeatTriggerMs = 0;
                item->authoritativeSeekGuardUntilMs = nowMs + 250;
                item->player->setPosition(desiredPositionMs);
            }
        }

        if (playing && !decoderTransitionInProgress) {
            item->playAuthorized = true;
            item->pausedAtEnd = false;
            restoreVideoOutput(item);
            ensureVideoOutputsAttached(item);
            if (item->player->playbackState() != QMediaPlayer::PlayingState) {
                item->player->play();
            }
        }
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
        TOAST_WARNING("Remote scene stopped: server connection lost", 3500);
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

        QWidget* window = sw.window;
        sw.sceneEpoch = 0;

        QObject::disconnect(window, nullptr, nullptr, nullptr);
        window->hide();

#ifdef Q_OS_MAC
        MacWindowManager::orderOutWindow(window);
#endif

        // Do not synchronously reset the QML model or source here. VideoItem
        // may have Qt.callLater work queued from component creation; destroying
        // its context in the middle of this network callback makes that work run
        // against an invalid QML object. The hidden window owns the quick widget
        // and model, so one deleteLater() on the window retires the entire tree
        // safely at the normal event-loop boundary.
        sw.mediaModel = nullptr;
        sw.mediaEntries.clear();
		QWindow* nativeWindow = window->windowHandle();
        QQuickWindow* quickWindow = nullptr;
        if (sw.quickWidget) {
            quickWindow = sw.quickWidget->quickWindow();
            QObject::disconnect(sw.quickWidget, nullptr, nullptr, nullptr);
            sw.quickWidget = nullptr;
        }

        // Register the destruction observers after all wildcard disconnects;
        // otherwise QObject::disconnect(sender, nullptr, nullptr, nullptr)
        // would silently remove the barrier itself.
        trackTeardownObjectTree(window);
		trackTeardownObjectTree(nativeWindow);
        trackTeardownObjectTree(quickWindow);

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
    if (!root) return;
    const QList<QObject*> descendants = root->findChildren<QObject*>();
    for (QObject* child : descendants) trackTeardownObject(child);
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

    if (sessionWaiters.isEmpty()) {
        emit teardownSettled(QString(), true);
    } else {
        for (const QString& remoteSessionId : sessionWaiters) {
            m_lastTornDownRemoteSessionId = remoteSessionId;
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

void RemoteSceneController::teardownMediaItem(const std::shared_ptr<RemoteMediaItem>& item) {
    if (!item) return;

    auto stopAndDeleteTimer = [this](QTimer*& timer) {
        if (!timer) return;
        timer->stop();
        QObject::disconnect(timer, nullptr, nullptr, nullptr);
        trackTeardownObjectTree(timer);
        timer->deleteLater();
        timer = nullptr;
    };

    stopAndDeleteTimer(item->displayTimer);
    stopAndDeleteTimer(item->playTimer);
    stopAndDeleteTimer(item->pauseTimer);
    stopAndDeleteTimer(item->hideTimer);
    stopAndDeleteTimer(item->muteTimer);
    stopAndDeleteTimer(item->hideEndDelayTimer);
    stopAndDeleteTimer(item->muteEndDelayTimer);
	for (const QPointer<QTimer>& timerPointer : std::as_const(item->auxiliaryTimers)) {
		QTimer* timer = timerPointer.data();
		if (!timer) continue;
		timer->stop();
		QObject::disconnect(timer, nullptr, nullptr, nullptr);
		trackTeardownObjectTree(timer);
		timer->deleteLater();
	}
	item->auxiliaryTimers.clear();

    QVariantAnimation* audioFade = item->audioFadeAnimation.data();
    cancelAudioFade(item, false);
    trackTeardownObjectTree(audioFade);
    if (item->visualFadeAnimation) {
        QVariantAnimation* animation = item->visualFadeAnimation.data();
        QObject::disconnect(animation, nullptr, this, nullptr);
        animation->stop();
        trackTeardownObjectTree(animation);
        animation->deleteLater();
        item->visualFadeAnimation = nullptr;
    }

    QObject::disconnect(item->deferredStartConn);
    QObject::disconnect(item->primingConn);
    QObject::disconnect(item->mirrorConn);
    item->pausedAtEnd = false;
    item->hideEndTriggered = false;
    item->muteEndTriggered = false;

    if (item->player) {
        QMediaPlayer* player = item->player;
        QObject::disconnect(player, nullptr, nullptr, nullptr);
        if (player->playbackState() != QMediaPlayer::StoppedState) {
            player->stop();
        }
        player->setVideoSink(nullptr);
        player->setAudioOutput(nullptr);
        player->setSource(QUrl());
    }

    // Both sinks are QObject children of the player. Disconnect them and let
    // deletion of that single ownership root retire the complete video-output
    // tree; independently posting deletes for children and parent makes teardown
    // ordering needlessly fragile.
    if (item->primingSink) {
        QObject::disconnect(item->primingSink, nullptr, nullptr, nullptr);
        item->primingSink = nullptr;
    }

    if (item->liveSink) {
        QObject::disconnect(item->liveSink, nullptr, nullptr, nullptr);
        item->liveSink = nullptr;
    }

    if (item->audio) {
        QObject::disconnect(item->audio, nullptr, nullptr, nullptr);
        item->audio->setMuted(true);
        item->audio->setVolume(0.0);
    }
    item->muted = true;

    item->spans.clear();

    if (item->player) {
        trackTeardownObjectTree(item->player);
        item->player->deleteLater();
        item->player = nullptr;
    }
    if (item->audio) {
        trackTeardownObjectTree(item->audio);
        item->audio->deleteLater();
        item->audio = nullptr;
    }

    item->loaded = false;
    item->primedFirstFrame = false;
    item->primedFrame = QVideoFrame();
    item->primedFrameSticky = false;
    item->lastFrameImage = QImage();
    if (item->frameSource) {
        item->frameSource->clear();
        trackTeardownObjectTree(item->frameSource.data());
        item->frameSource->deleteLater();
        item->frameSource = nullptr;
    }
    item->playAuthorized = false;
    item->hiding = false;
    item->readyNotified = false;
    item->fadeInPending = false;
    item->pendingDisplayDelayMs = -1;
    item->pendingPlayDelayMs = -1;
    item->pendingPauseDelayMs = -1;
    item->startPositionMs = 0;
    item->hasStartPosition = false;
    item->displayTimestampMs = -1;
    item->hasDisplayTimestamp = false;
    item->awaitingStartFrame = false;
    item->awaitingDecoderSync = false;
    item->decoderSyncTargetMs = -1;
    item->awaitingLivePlayback = false;
    item->livePlaybackStarted = false;
    item->liveWarmupFramesRemaining = 0;
    item->lastLiveFrameTimestampMs = -1;
    item->videoOutputsAttached = false;
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
        startPendingPauseTimerIfEligible(item);
    }
}

void RemoteSceneController::startSceneActivationIfReady() {
    if (m_sceneActivated || m_sceneActivationRequested) return;
    if (m_totalMediaToPrime > 0 && m_mediaReadyCount < m_totalMediaToPrime) return;

    // PREPARE is complete. Keep all windows hidden and every automation timer
    // stopped until the host sends ACTIVATE with a shared wall-clock epoch.
    m_sceneActivationRequested = true;
    if (m_sceneReadyTimeout && m_ws) {
        const QJsonObject policy = m_ws ? m_ws->serverPolicy() : QJsonObject();
        const int activationLeadMs = policy
            .value(QStringLiteral("sceneActivationLeadMs")).toInt();
        const int leaseTimeoutMs = policy
            .value(QStringLiteral("leaseTimeoutMs")).toInt();
        if (activationLeadMs <= 0 || leaseTimeoutMs <= 0) {
            sendPrepareResult(false, QStringLiteral("Invalid scene activation policy"));
            ++m_sceneEpoch;
            clearScene();
            return;
        }
        m_sceneReadyTimeout->start(activationLeadMs + leaseTimeoutMs);
    }
    if (m_ws && !m_pendingSenderClientId.isEmpty() && !m_pendingSceneInstanceId.isEmpty()) {
        sendPrepareResult(true);
    }
}

void RemoteSceneController::startDeferredTimers() {
    for (const auto& item : m_mediaItems) {
        if (!item) continue;
        if (item->displayTimer && item->pendingDisplayDelayMs >= 0) {
            item->displayTimer->start(item->pendingDisplayDelayMs);
            item->pendingDisplayDelayMs = -1;
        }
        if (item->playTimer && item->pendingPlayDelayMs >= 0) {
            if (item->pendingPlayDelayMs == 0) {
                if (item->playTimer->isActive()) item->playTimer->stop();
                triggerAutoPlayNow(item, item->sceneEpoch);
            } else {
                item->playTimer->start(item->pendingPlayDelayMs);
            }
            item->pendingPlayDelayMs = -1;
        }
        startPendingPauseTimerIfEligible(item);
        if (item->fadeInPending && item->displayReady && !item->displayStarted && !autoDisplayDelayActive(item)) {
            fadeIn(item);
        }
    }
}

void RemoteSceneController::startPendingPauseTimerIfEligible(const std::shared_ptr<RemoteMediaItem>& item) {
    if (!item) return;
    if (!item->pauseTimer) return;
    if (item->pendingPauseDelayMs < 0) return;
    if (!m_sceneActivated) return;
    if (!item->playAuthorized) return;

    item->pauseTimer->start(item->pendingPauseDelayMs);
    item->pendingPauseDelayMs = -1;
}

void RemoteSceneController::triggerAutoPlayNow(const std::shared_ptr<RemoteMediaItem>& item, quint64 epoch) {
    if (!item) return;
    if (epoch != m_sceneEpoch) return;
    if (!item->player) return;
    item->playAuthorized = true;
    item->repeatActive = false;
    item->lastRepeatTriggerMs = 0;
    restoreVideoOutput(item);
    item->pausedAtEnd = false;
    item->repeatRemaining = (item->repeatEnabled && item->repeatCount > 0) ? item->repeatCount : 0;
    item->awaitingLivePlayback = true;
    item->livePlaybackStarted = false;
    item->liveWarmupFramesRemaining = kLivePlaybackWarmupFrames;
    item->lastLiveFrameTimestampMs = -1;

    if (item->loaded) {
        const qint64 startPos = item->hasStartPosition ? effectiveStartPosition(item) : 0;
        if (item->player->position() != startPos) {
            item->player->setPosition(startPos);
        }

        const bool canGate = item->primedFirstFrame && item->primingSink;
        if (canGate) {
            item->awaitingDecoderSync = true;
            if (item->decoderSyncTargetMs < 0) {
                item->decoderSyncTargetMs = targetDisplayTimestamp(item);
            }
            item->player->setVideoSink(item->primingSink);
            item->videoOutputsAttached = false;
            if (item->primedFrame.isValid()) {
                applyPrimedFrameToSinks(item);
            }
        } else {
            ensureVideoOutputsAttached(item);
            if (item->primedFrame.isValid()) {
                applyPrimedFrameToSinks(item);
            }
        }
        item->player->play();
        startPendingPauseTimerIfEligible(item);
        return;
    }

    QObject::disconnect(item->deferredStartConn);
    std::weak_ptr<RemoteMediaItem> weakItem = item;
    item->deferredStartConn = QObject::connect(item->player, &QMediaPlayer::mediaStatusChanged, item->player, [this, epoch, weakItem](QMediaPlayer::MediaStatus s) {
        auto item = weakItem.lock();
        if (!item) return;
        if (epoch != m_sceneEpoch || !item->playAuthorized) return;
        if (s == QMediaPlayer::LoadedMedia || s == QMediaPlayer::BufferedMedia) {
            QObject::disconnect(item->deferredStartConn);
            if (item->player) {
                const qint64 startPos = item->hasStartPosition ? effectiveStartPosition(item) : 0;
                if (item->player->position() != startPos) {
                    item->player->setPosition(startPos);
                }
                    item->awaitingLivePlayback = true;
                    item->livePlaybackStarted = false;
                    item->liveWarmupFramesRemaining = kLivePlaybackWarmupFrames;
                    item->lastLiveFrameTimestampMs = -1;
                const bool canGate = item->primedFirstFrame && item->primingSink;
                if (canGate) {
                    item->awaitingDecoderSync = true;
                    if (item->decoderSyncTargetMs < 0) {
                        item->decoderSyncTargetMs = targetDisplayTimestamp(item);
                    }
                    item->player->setVideoSink(item->primingSink);
                    item->videoOutputsAttached = false;
                    if (item->primedFrame.isValid()) {
                        applyPrimedFrameToSinks(item);
                    }
                } else {
                    ensureVideoOutputsAttached(item);
                    if (item->primedFrame.isValid()) {
                        applyPrimedFrameToSinks(item);
                    }
                }
                item->player->play();
                startPendingPauseTimerIfEligible(item);
            }
            item->pausedAtEnd = false;
            item->repeatRemaining = (item->repeatEnabled && item->repeatCount > 0) ? item->repeatCount : 0;
        }
    });
}

void RemoteSceneController::applyImageToSpans(const std::shared_ptr<RemoteMediaItem>& item, const QImage& image) const {
    if (!item || image.isNull() || !item->frameSource) return;
    item->frameSource->setFrame(image);
}

bool RemoteSceneController::autoDisplayDelayActive(const std::shared_ptr<RemoteMediaItem>& item) const {
    if (!item) return false;
    if (!item->autoDisplay) return false;
    if (item->displayStarted) return false;

    if (item->pendingDisplayDelayMs > 0) {
        return true;
    }
    if (item->displayTimer && item->displayTimer->isActive() && item->displayTimer->interval() > 0) {
        return true;
    }
    if (!m_sceneActivated && item->autoDisplayDelayMs > 0) {
        return true;
    }
    return false;
}

void RemoteSceneController::applyPrimedFrameToSinks(const std::shared_ptr<RemoteMediaItem>& item) {
    if (!item) return;
    if (!item->primedFrame.isValid()) return;
    if (!item->primedFrameSticky) return;

    QImage image = convertFrameToImage(item->primedFrame);
    if (!image.isNull()) {
        item->lastFrameImage = image;
    } else {
        // Some hardware frames cannot be mapped a second time. Reuse the CPU
        // copy proven during PREPARE rather than dropping the primed image.
        image = item->lastFrameImage;
    }
    if (image.isNull()) return;

    // Feed the passive surfaces while opacity is still zero.  Readiness must
    // never depend on a later display timer, otherwise delayed videos deadlock
    // the scene activation barrier.
    applyImageToSpans(item, item->lastFrameImage);
}

void RemoteSceneController::clearRenderedFrames(const std::shared_ptr<RemoteMediaItem>& item) {
    if (!item) return;
    if (item->awaitingLivePlayback && !item->livePlaybackStarted) return;

    item->lastFrameImage = QImage();
    if (item->frameSource) item->frameSource->clear();
}

void RemoteSceneController::ensureVideoOutputsAttached(const std::shared_ptr<RemoteMediaItem>& item) {
    if (!item) return;
    if (item->videoOutputsAttached) {
        applyPrimedFrameToSinks(item);
        return;
    }
    if (!item->player) return;

    if (!item->liveSink) {
        item->liveSink = new QVideoSink(item->player);
    }

    item->player->setVideoSink(item->liveSink);
    QObject::disconnect(item->mirrorConn);

    if (item->liveSink) {
        std::weak_ptr<RemoteMediaItem> weakItem = item;
        const quint64 epoch = item->sceneEpoch;
        item->mirrorConn = QObject::connect(item->liveSink, &QVideoSink::videoFrameChanged, item->liveSink, [this, epoch, weakItem](const QVideoFrame& frame) {
            if (!frame.isValid()) return;
            auto item = weakItem.lock();
            if (!item) return;
            if (epoch != m_sceneEpoch) return;
            // Safety check: verify live sink still exists
            if (!item->liveSink) return;

            if (item->holdLastFrameAtEnd) {
                return;
            }

            item->primedFrame = frame;

            const qint64 ts = frameTimestampMs(frame);
            if (item->awaitingLivePlayback && !item->livePlaybackStarted) {
                bool advancedFrame = false;
                if (ts >= 0 && ts != item->lastLiveFrameTimestampMs) {
                    item->lastLiveFrameTimestampMs = ts;
                    advancedFrame = true;
                } else if (ts < 0) {
                    advancedFrame = true;
                }
                if (advancedFrame && item->liveWarmupFramesRemaining > 0) {
                    --item->liveWarmupFramesRemaining;
                }
                if (item->liveWarmupFramesRemaining <= 0) {
                    finalizeLivePlaybackStart(item, frame);
                }
            }

            // The passive multi-screen renderer needs a CPU image only while
            // pixels can actually be seen. Keep the newest native frame above
            // so a later fade-in can resume immediately, but avoid a 1080p
            // readback/upload cycle for every frame while the item is hidden.
            if (item->renderVisible && item->renderOpacity > 0.0001) {
                QImage converted = convertFrameToImage(frame);
                if (!converted.isNull()) {
                    item->lastFrameImage = converted;
                    applyImageToSpans(item, item->lastFrameImage);
                }
            }
        });
    }

    item->videoOutputsAttached = true;
    applyPrimedFrameToSinks(item);
}

void RemoteSceneController::finalizeLivePlaybackStart(const std::shared_ptr<RemoteMediaItem>& item, const QVideoFrame& frame) {
    if (!item) return;
    if (item->livePlaybackStarted) return;
    item->awaitingLivePlayback = false;
    item->livePlaybackStarted = true;
    item->liveWarmupFramesRemaining = 0;
    if (frame.isValid()) {
        item->primedFrame = frame;
        item->primedFrameSticky = true;
        const qint64 ts = frameTimestampMs(frame);
        if (ts >= 0) {
            item->displayTimestampMs = ts;
            item->hasDisplayTimestamp = true;
            item->lastLiveFrameTimestampMs = ts;
        }
    }
    const bool displayDelayOutstanding = autoDisplayDelayActive(item);
    if (item->fadeInPending && item->displayReady && !item->displayStarted && !displayDelayOutstanding) {
        fadeIn(item);
    } else if (!item->displayStarted && item->displayReady && !displayDelayOutstanding) {
        fadeIn(item);
    }
    startPendingPauseTimerIfEligible(item);
}

void RemoteSceneController::activateScene() {
    if (m_sceneActivated || !m_sceneActivationRequested) return;
	const quint64 activationEpoch = m_sceneEpoch;
	bool activationGraphReady = remoteRenderGraphsReady();
	for (auto it = m_screenWindows.cbegin();
		 activationGraphReady && it != m_screenWindows.cend(); ++it) {
		activationGraphReady = it->sceneEpoch == activationEpoch
			&& it->quickWidget && it->quickWidget->quickWindow();
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
		QQuickWindow* renderWindow = sw.quickWidget->quickWindow();

		// Install the swap observer only after show(). Any preparation frame
		// emitted while the window was hidden is therefore outside this
		// activation generation and cannot satisfy the barrier.
		sw.window->show();

        const int screenId = it.key();
        m_screensAwaitingFirstFrame.insert(screenId);
		sw.firstFrameSwapsRemaining = 2;
        QObject::disconnect(sw.firstFrameConnection);
        sw.firstFrameConnection = connect(
            renderWindow, &QQuickWindow::frameSwapped, this,
            [this, activationEpoch, screenId]() {
				if (activationEpoch != m_sceneEpoch || !m_sceneActivated) {
                    return;
                }
                auto windowIt = m_screenWindows.find(screenId);
				if (windowIt == m_screenWindows.end()
					|| windowIt->sceneEpoch != activationEpoch
					|| !windowIt->window || !windowIt->quickWidget) {
					return;
				}

				ScreenWindow& presentedWindow = windowIt.value();
				QWindow* nativeWindow = presentedWindow.window->windowHandle();
				if (!presentedWindow.window->isVisible()
					|| !presentedWindow.quickWidget->isVisible()
					|| !nativeWindow || !nativeWindow->isExposed()) {
					// A hidden/off-screen render pass is not a presented frame. Keep
					// the barrier armed and request another compositor cycle.
					presentedWindow.quickWidget->update();
					if (QQuickWindow* quickWindow = presentedWindow.quickWidget->quickWindow()) {
						quickWindow->update();
					}
					return;
                }

				if (presentedWindow.firstFrameSwapsRemaining > 0) {
					--presentedWindow.firstFrameSwapsRemaining;
					if (presentedWindow.firstFrameSwapsRemaining > 0) {
						presentedWindow.quickWidget->update();
						if (QQuickWindow* quickWindow = presentedWindow.quickWidget->quickWindow()) {
							quickWindow->update();
						}
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

        sw.quickWidget->update();
		renderWindow->update();
#ifdef Q_OS_MAC
        QTimer::singleShot(0, this, [this, activationEpoch, screenId]() {
            if (activationEpoch != m_sceneEpoch) {
                return;
            }

            auto macIt = m_screenWindows.find(screenId);
            if (macIt == m_screenWindows.end()) {
                return;
            }

            ScreenWindow& macWindow = macIt.value();
            if (macWindow.sceneEpoch != activationEpoch || !macWindow.window) {
                return;
            }

            MacWindowManager::setWindowAsGlobalOverlay(macWindow.window, /*clickThrough*/ true);
        });
#endif
    }

    // Mute all videos at scene start and schedule automatic unmute if enabled
    const quint64 epoch = m_sceneEpoch;
    for (const auto& item : m_mediaItems) {
        if (!item || item->type != "video") continue;
        if (!item->audio) continue;
        
        // The authoritative host scene always begins muted; automatic unmute
        // starts from the common activation epoch below.
        applyAudioMuteState(item, true, true);
        
        // Schedule automatic unmute if enabled
        if (item->autoUnmute) {
            const int unmuteDelayMs = std::max(0, item->autoUnmuteDelayMs);
            auto unmuteCallback = [this, item, epoch]() {
                if (epoch != m_sceneEpoch) return; // Scene changed
                if (!item || !item->audio) return; // Item deleted
                if (!m_sceneActivated) return; // Scene stopped
                applyAudioMuteState(item, false);
            };
            
			QTimer* unmuteTimer = new QTimer(this);
			unmuteTimer->setSingleShot(true);
			item->auxiliaryTimers.append(unmuteTimer);
			connect(unmuteTimer, &QTimer::timeout, this, unmuteCallback);
			unmuteTimer->start(unmuteDelayMs);
        }

        item->hideEndTriggered = false;
        item->muteEndTriggered = false;

        if (item->autoMute && !item->muteWhenVideoEnds) {
            scheduleMuteTimer(item);
        } else if (item->muteTimer) {
            item->muteTimer->stop();
        }
    }

    startDeferredTimers();
    // No local show()/repaint acknowledgement is emitted. Each target screen
    // must reach QQuickWindow::frameSwapped above; if one never does, no
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

qint64 RemoteSceneController::effectiveStartPosition(const std::shared_ptr<RemoteMediaItem>& item) const {
    if (!item) return 0;
    if (!item->hasStartPosition) return 0;
    qint64 target = item->startPositionMs;
    if (target < 0) target = 0;
    if (item->player) {
        const qint64 dur = item->player->duration();
        if (dur > 0 && target >= dur) {
            target = std::max<qint64>(qint64(0), dur - 1);
        }
    }
    return target;
}

qint64 RemoteSceneController::targetDisplayTimestamp(const std::shared_ptr<RemoteMediaItem>& item) const {
    if (!item) return 0;
    if (item->hasDisplayTimestamp && item->displayTimestampMs >= 0) {
        qint64 ts = item->displayTimestampMs;
        if (item->player) {
            const qint64 dur = item->player->duration();
            if (dur > 0 && ts >= dur) {
                ts = std::max<qint64>(qint64(0), dur - 1);
            }
        }
        return std::max<qint64>(0, ts);
    }
    return effectiveStartPosition(item);
}

void RemoteSceneController::freezeVideoOutput(const std::shared_ptr<RemoteMediaItem>& item) {
    if (!item || item->type != "video") return;
    if (item->holdLastFrameAtEnd) return;
    if (!item->primedFrame.isValid()) {
        return;
    }

    QImage image = convertFrameToImage(item->primedFrame);
    if (image.isNull()) {
        qWarning() << "RemoteSceneController: unable to convert final video frame for" << item->mediaId;
    } else {
        item->lastFrameImage = image;
    }

    item->holdLastFrameAtEnd = true;

    if (!item->lastFrameImage.isNull()) {
        applyImageToSpans(item, item->lastFrameImage);
        // Freezing updates pixels only. Preserve runtime visibility so a video
        // played while hidden (or already auto-hidden) cannot reappear.
        setRemoteMediaVisualState(item, item->renderOpacity, item->renderVisible);
    }
    // Handle mute-on-end with optional delay
    if (item->muteWhenVideoEnds && item->audio && !item->muteEndTriggered) {
        const int muteDelayMs = item->autoMuteDelayMs;
        if (muteDelayMs > 0) {
            if (!item->muteEndDelayTimer) {
                item->muteEndDelayTimer = new QTimer(this);
                item->muteEndDelayTimer->setSingleShot(true);
                std::weak_ptr<RemoteMediaItem> weakItem = item;
                QObject::connect(item->muteEndDelayTimer, &QTimer::timeout, this, [this, weakItem]() {
                    auto locked = weakItem.lock();
                    if (!locked || !locked->audio) return;
                    if (locked->sceneEpoch != m_sceneEpoch) return;
                    applyAudioMuteState(locked, true);
                    locked->muteEndTriggered = true;
                });
            }
            item->muteEndDelayTimer->start(muteDelayMs);
        } else {
            applyAudioMuteState(item, true);
            item->muteEndTriggered = true;
        }
    }

    // Handle hide-on-end with optional delay
    if (item->hideWhenVideoEnds && !item->hideEndTriggered) {
        const int hideDelayMs = item->autoHideDelayMs;
        if (hideDelayMs > 0) {
            if (!item->hideEndDelayTimer) {
                item->hideEndDelayTimer = new QTimer(this);
                item->hideEndDelayTimer->setSingleShot(true);
                std::weak_ptr<RemoteMediaItem> weakItem = item;
                QObject::connect(item->hideEndDelayTimer, &QTimer::timeout, this, [this, weakItem]() {
                    auto locked = weakItem.lock();
                    if (!locked) return;
                    if (locked->sceneEpoch != m_sceneEpoch) return;
                    locked->hideEndTriggered = true;
                    fadeOutAndHide(locked);
                });
            }
            item->hideEndDelayTimer->start(hideDelayMs);
        } else {
            item->hideEndTriggered = true;
            fadeOutAndHide(item);
        }
    }
}

void RemoteSceneController::restoreVideoOutput(const std::shared_ptr<RemoteMediaItem>& item) {
    if (!item || item->type != "video") return;

    item->holdLastFrameAtEnd = false;
}

void RemoteSceneController::seekToConfiguredStart(const std::shared_ptr<RemoteMediaItem>& item) {
    if (!item) return;
    if (!item->player) return;
    const qint64 target = item->hasStartPosition ? effectiveStartPosition(item) : 0;
    const qint64 current = item->player->position();
    const bool needsSeek = qAbs(current - target) > kSeekPositionToleranceMs;
    item->awaitingStartFrame = !item->primedFirstFrame
        && (item->hasDisplayTimestamp || (item->hasStartPosition && target > 0));
    if (needsSeek) {
        item->player->setPosition(target);
        qDebug() << "RemoteSceneController: seekToConfiguredStart" << item->mediaId
                 << "target" << target
                 << "current" << current
                 << "awaiting" << item->awaitingStartFrame;
    } else {
        if (current != target) {
            item->player->setPosition(target);
        }
    }
    if (!item->awaitingStartFrame) {
        startPendingPauseTimerIfEligible(item);
    }
}

void RemoteSceneController::resetWindowForNewScene(ScreenWindow& sw, int screenId, int x, int y, int w, int h, bool primary) {
    if (!sw.window || !sw.quickWidget || !sw.mediaModel) return;

    QObject::disconnect(sw.firstFrameConnection);
    sw.firstFrameConnection = {};
	sw.firstFrameSwapsRemaining = 0;

    sw.x = x;
    sw.y = y;
    sw.w = w;
    sw.h = h;
    sw.sceneEpoch = m_sceneEpoch;

    sw.window->hide();
    sw.window->setGeometry(x, y, w, h);
    sw.window->setWindowTitle(primary ? "Remote Scene (Primary)" : "Remote Scene");
    sw.mediaEntries.clear();
    sw.mediaModel->clearAll();
    sw.quickWidget->resize(w, h);

#ifdef Q_OS_MAC
    MacWindowManager::setWindowAsGlobalOverlay(sw.window, /*clickThrough*/ true);
#endif
}

QWidget* RemoteSceneController::ensureScreenWindow(int screenId, int x, int y, int w, int h, bool primary) {
    ScreenWindow& sw = m_screenWindows[screenId];

    if (!sw.window) {
        registerCanvasQmlTypes();
        sw.window = new QWidget();
        
        // Force native window on macOS to avoid accessibility crashes (QTBUG-95134)
        
        sw.window->setAttribute(Qt::WA_TransparentForMouseEvents, true);
        sw.window->setWindowFlag(Qt::FramelessWindowHint, true);
        sw.window->setWindowFlag(Qt::WindowStaysOnTopHint, true);
#ifdef Q_OS_WIN
        sw.window->setWindowFlag(Qt::Tool, true);
        sw.window->setWindowFlag(Qt::WindowDoesNotAcceptFocus, true);
        sw.window->setAttribute(Qt::WA_ShowWithoutActivating, true);
#endif
        sw.window->setAttribute(Qt::WA_TranslucentBackground, true);
        sw.window->setAttribute(Qt::WA_NoSystemBackground, true);
        sw.window->setAttribute(Qt::WA_OpaquePaintEvent, false);
        sw.window->setObjectName(QString("RemoteScreenWindow_%1").arg(screenId));

        sw.quickWidget = new QQuickWidget(sw.window);
        sw.quickWidget->setResizeMode(QQuickWidget::SizeRootObjectToView);
        sw.quickWidget->setClearColor(Qt::transparent);
        sw.quickWidget->setAttribute(Qt::WA_AlwaysStackOnTop, true);
        sw.quickWidget->setAttribute(Qt::WA_TranslucentBackground, true);
        sw.quickWidget->setAttribute(Qt::WA_NoSystemBackground, true);
        sw.quickWidget->setAttribute(Qt::WA_TransparentForMouseEvents, true);
        sw.mediaModel = new MediaListModel(sw.quickWidget);
        sw.quickWidget->setSource(QUrl(QStringLiteral("qrc:/qml/RemoteSceneRoot.qml")));

        if (sw.quickWidget->status() == QQuickWidget::Error || !sw.quickWidget->rootObject()) {
            qCritical() << "RemoteSceneController: Qt Quick remote renderer failed to initialize for screen" << screenId;
            for (const QQmlError& error : sw.quickWidget->errors()) {
                qCritical().noquote() << error.toString();
            }
        } else {
            sw.quickWidget->rootObject()->setProperty("mediaListModel", QVariant::fromValue(sw.mediaModel));
            connect(sw.quickWidget->rootObject(), SIGNAL(spanReady(QString,QString)),
                    this, SLOT(onRemoteSpanReady(QString,QString)));
        }

        auto* layout = new QHBoxLayout(sw.window);
        layout->setContentsMargins(0, 0, 0, 0);
        layout->setSpacing(0);
        layout->addWidget(sw.quickWidget);
    }

    resetWindowForNewScene(sw, screenId, x, y, w, h, primary);
    return sw.window;
}

void RemoteSceneController::buildWindows(const QJsonArray& screensArray) {
    // Map host screen list to local physical screens by index.
    const QList<QScreen*> localScreens = QGuiApplication::screens();
    int hostIndex = 0;
    for (const auto& v : screensArray) {
        QJsonObject o = v.toObject();
        int hostScreenId = o.value("id").toInt();
        // The snapshot is validated against the current topology before any
        // window is built. Never fold excess host screens onto the primary:
        // that creates multiple full-screen overlays on the same display.
        QScreen* target = (hostIndex < localScreens.size()) ? localScreens[hostIndex] : nullptr;
        if (!target) {
            qWarning() << "RemoteSceneController: local screen disappeared during preparation"
                       << hostIndex;
            ++hostIndex;
            continue;
        }
        const QRect geom = target->geometry();
        const bool primary = target == QGuiApplication::primaryScreen();
        ensureScreenWindow(hostScreenId, geom.x(), geom.y(), geom.width(), geom.height(), primary);
		auto windowIt = m_screenWindows.find(hostScreenId);
		if (windowIt != m_screenWindows.end()) {
			windowIt->sourceScreenDefinition = o;
		}
        ++hostIndex;
    }
    qDebug() << "RemoteSceneController: created" << m_screenWindows.size() << "remote screen windows (host screens:" << screensArray.size() << ", local screens:" << localScreens.size() << ")";
}

void RemoteSceneController::publishScreenModel(int screenId) {
    auto it = m_screenWindows.find(screenId);
    if (it == m_screenWindows.end() || !it->mediaModel) return;
    it->mediaModel->updateFromList(it->mediaEntries);
}

void RemoteSceneController::publishMediaSpan(const std::shared_ptr<RemoteMediaItem>& item,
                                             RemoteMediaItem::Span& span) {
    if (!item) return;
    auto windowIt = m_screenWindows.find(span.screenId);
    if (windowIt == m_screenWindows.end() || !windowIt->quickWidget || !windowIt->mediaModel) {
        qWarning() << "RemoteSceneController: no Qt Quick surface for span" << span.spanId;
        return;
    }

    const qreal surfaceWidth = std::max<qreal>(1.0, windowIt->w);
    const qreal surfaceHeight = std::max<qreal>(1.0, windowIt->h);
    QVariantMap media;
    // MediaListModel uses mediaId as its stable row key.  remoteMediaId keeps
    // the protocol identifier used by readiness and automation.
    media.insert(QStringLiteral("mediaId"), span.spanId);
    media.insert(QStringLiteral("remoteMediaId"), item->mediaId);
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
    media.insert(QStringLiteral("renderVisible"), item->renderVisible);
    media.insert(QStringLiteral("renderOpacity"), item->renderOpacity);

    if (item->type == QLatin1String("image")) {
        const QString path = m_fileManager ? m_fileManager->getFilePathForId(item->fileId) : QString();
        media.insert(QStringLiteral("sourceUrl"), path.isEmpty() ? QString() : QUrl::fromLocalFile(path).toString());
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
        media.insert(QStringLiteral("textFontPixelSize"), std::max(1, item->fontPixelSize));
        media.insert(QStringLiteral("textFontWeight"), item->fontWeight > 0
                     ? item->fontWeight : (item->fontBold ? 700 : 400));
        media.insert(QStringLiteral("textItalic"), item->fontItalic);
        media.insert(QStringLiteral("textUnderline"), item->fontUnderline);
        media.insert(QStringLiteral("textUppercase"), item->fontUppercase);
        media.insert(QStringLiteral("textHorizontalAlignment"), horizontal);
        media.insert(QStringLiteral("textVerticalAlignment"), vertical);
        media.insert(QStringLiteral("fitToTextEnabled"), item->fitToTextEnabled);
        media.insert(QStringLiteral("textColor"), item->textColor);
        media.insert(QStringLiteral("textOutlineWidthPercent"), item->textBorderWidthPercent);
        media.insert(QStringLiteral("textOutlineWidthPx"), item->textOutlineWidthPx);
        media.insert(QStringLiteral("textOutlineColor"), item->textBorderColor);
        media.insert(QStringLiteral("textHighlightEnabled"), item->highlightEnabled);
        media.insert(QStringLiteral("textHighlightColor"), item->textHighlightColor);
    }

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
        for (QVariant& entry : windowIt->mediaEntries) {
            QVariantMap media = entry.toMap();
            if (media.value(QStringLiteral("spanId")).toString() != span.spanId
                || media.value(QStringLiteral("remoteMediaId")).toString()
                    != item->mediaId) {
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
                             std::max(1, item->fontPixelSize));
                media.insert(QStringLiteral("textFontWeight"), item->fontWeight > 0
                             ? item->fontWeight : (item->fontBold ? 700 : 400));
                media.insert(QStringLiteral("textItalic"), item->fontItalic);
                media.insert(QStringLiteral("textUnderline"), item->fontUnderline);
                media.insert(QStringLiteral("textUppercase"), item->fontUppercase);
                media.insert(QStringLiteral("textHorizontalAlignment"), horizontal);
                media.insert(QStringLiteral("textVerticalAlignment"), vertical);
                media.insert(QStringLiteral("fitToTextEnabled"), item->fitToTextEnabled);
                media.insert(QStringLiteral("textColor"), item->textColor);
                media.insert(QStringLiteral("textOutlineWidthPercent"),
                             item->textBorderWidthPercent);
                media.insert(QStringLiteral("textOutlineWidthPx"), item->textOutlineWidthPx);
                media.insert(QStringLiteral("textOutlineColor"), item->textBorderColor);
                media.insert(QStringLiteral("textHighlightEnabled"), item->highlightEnabled);
                media.insert(QStringLiteral("textHighlightColor"), item->textHighlightColor);
            }
            entry = media;
            changedScreens.insert(span.screenId);
            break;
        }
    }
    for (int screenId : std::as_const(changedScreens)) publishScreenModel(screenId);
}

void RemoteSceneController::setRemoteMediaVisualState(const std::shared_ptr<RemoteMediaItem>& item,
                                                      qreal opacity,
                                                      bool visible) {
    if (!item) return;
    item->renderOpacity = std::clamp<qreal>(opacity, 0.0, 1.0);
    item->renderVisible = visible;
    QSet<int> changedScreens;
    for (const auto& span : item->spans) {
        auto windowIt = m_screenWindows.find(span.screenId);
        if (windowIt == m_screenWindows.end()) continue;
        for (QVariant& entry : windowIt->mediaEntries) {
            QVariantMap map = entry.toMap();
            if (map.value(QStringLiteral("spanId")).toString() != span.spanId) continue;
            map.insert(QStringLiteral("renderOpacity"), item->renderOpacity);
            map.insert(QStringLiteral("renderVisible"), item->renderVisible);
            entry = map;
            changedScreens.insert(span.screenId);
            break;
        }
    }
    for (int screenId : std::as_const(changedScreens)) publishScreenModel(screenId);
}

bool RemoteSceneController::allSpansReady(const std::shared_ptr<RemoteMediaItem>& item) const {
    if (!item || item->spans.isEmpty()) return false;
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

void RemoteSceneController::buildMedia(const QJsonArray& mediaArray) {
    // Keep the historical array order for schema v1; schema v2 also carries an
    // explicit z value, which the shared QML delegate applies authoritatively.
    m_totalMediaToPrime = mediaArray.size();
    for (int idx = mediaArray.size() - 1; idx >= 0; --idx) {
        const auto& v = mediaArray.at(idx);
        QJsonObject m = v.toObject();
    auto item = std::make_shared<RemoteMediaItem>();
    item->mediaId = m.value("mediaId").toString();
    item->fileId = m.value("fileId").toString();
    item->type = m.value("type").toString();
    item->fileName = m.value("fileName").toString();
    item->sceneEpoch = m_sceneEpoch;
    // Schema v1 has no explicit z and is serialized topmost-first. Because we
    // still iterate backwards, a descending implicit value preserves that
    // historical stack. Schema v2 uses the authoritative value from the host.
    const double implicitZ = static_cast<double>(mediaArray.size() - idx);
    item->z = m.value("z").toDouble(implicitZ);
    if (!std::isfinite(item->z)) item->z = implicitZ;
    item->contentVisible = m.value("visible").toBool(true);
    item->renderVisible = item->contentVisible;
    
    // Parse base dimensions for all media types (needed for scaling)
    item->baseWidth = m.value("baseWidth").toInt(0);
    item->baseHeight = m.value("baseHeight").toInt(0);
    
    // Parse text-specific properties if this is a text item
    if (item->type == "text") {
        item->text = m.value("text").toString();
        item->fontFamily = m.value("fontFamily").toString("Impact");
        item->fontSize = std::clamp(m.value("fontSize").toInt(12), 1, 1000);
        item->fontBold = m.value("fontBold").toBool(false);
        item->fontItalic = m.value("fontItalic").toBool(false);
        item->fontUnderline = m.value("fontUnderline").toBool(false);
        item->fontUppercase = m.value("fontUppercase").toBool(false);
        item->fontWeight = m.value("fontWeight").toInt(0);
        if (item->fontWeight > 0) item->fontWeight = std::clamp(item->fontWeight, 100, 900);
        item->fontPixelSize = std::clamp(m.value("fontPixelSize").toInt(0), 0, 4096);
        item->textColor = m.value("textColor").toString("#FFFFFF");
        item->textBorderWidthPercent = m.value("textBorderWidthPercent").toDouble(0.0);
        item->textOutlineWidthPx = m.value("textOutlineWidthPx").toDouble(-1.0);
        item->textBorderColor = m.value("textBorderColor").toString();
        item->fitToTextEnabled = m.value("textFitToTextEnabled").toBool(false);
        item->highlightEnabled = m.value("textHighlightEnabled").toBool(false);
        item->textHighlightColor = m.value("textHighlightColor").toString();
        double uniformScale = m.value("uniformScale").toDouble(1.0);
        if (!std::isfinite(uniformScale) || std::abs(uniformScale) < 1e-6) {
            uniformScale = 1.0;
        }
        item->uniformScale = uniformScale;

        if (item->fontPixelSize <= 0) {
            QFont legacyFont(item->fontFamily, std::max(1, item->fontSize));
            legacyFont.setBold(item->fontBold);
            legacyFont.setItalic(item->fontItalic);
            legacyFont.setUnderline(item->fontUnderline);
            item->fontPixelSize = TextRenderMetrics::effectiveFontPixelSize(legacyFont, item->uniformScale);
        }
        if (!std::isfinite(item->textOutlineWidthPx) || item->textOutlineWidthPx < 0.0) {
            item->textOutlineWidthPx = TextRenderMetrics::outlinePixels(
                item->textBorderWidthPercent, item->fontPixelSize);
        }

        const QString hAlign = m.value("horizontalAlignment").toString("center").toLower();
        if (hAlign == QLatin1String("left")) {
            item->horizontalAlignment = RemoteMediaItem::HorizontalAlignment::Left;
        } else if (hAlign == QLatin1String("right")) {
            item->horizontalAlignment = RemoteMediaItem::HorizontalAlignment::Right;
        } else {
            item->horizontalAlignment = RemoteMediaItem::HorizontalAlignment::Center;
        }

        const QString vAlign = m.value("verticalAlignment").toString("center").toLower();
        if (vAlign == QLatin1String("top")) {
            item->verticalAlignment = RemoteMediaItem::VerticalAlignment::Top;
        } else if (vAlign == QLatin1String("bottom")) {
            item->verticalAlignment = RemoteMediaItem::VerticalAlignment::Bottom;
        } else {
            item->verticalAlignment = RemoteMediaItem::VerticalAlignment::Center;
        }
    }
        // Parse spans if present
        if (m.contains("spans") && m.value("spans").isArray()) {
            const QJsonArray spans = m.value("spans").toArray();
            for (int spanIndex = 0; spanIndex < spans.size(); ++spanIndex) {
                const auto& sv = spans.at(spanIndex);
                const QJsonObject so = sv.toObject();
                RemoteMediaItem::Span s; s.screenId = so.value("screenId").toInt(-1);
                s.nx = so.value("normX").toDouble(); s.ny = so.value("normY").toDouble(); s.nw = so.value("normW").toDouble(); s.nh = so.value("normH").toDouble();
                s.destNx = so.contains("spanDestNormX") ? so.value("spanDestNormX").toDouble() : s.nx;
                s.destNy = so.contains("spanDestNormY") ? so.value("spanDestNormY").toDouble() : s.ny;
                s.destNw = so.contains("spanDestNormW") ? so.value("spanDestNormW").toDouble() : s.nw;
                s.destNh = so.contains("spanDestNormH") ? so.value("spanDestNormH").toDouble() : s.nh;
                s.srcNx = so.contains("spanSourceNormX") ? so.value("spanSourceNormX").toDouble() : 0.0;
                s.srcNy = so.contains("spanSourceNormY") ? so.value("spanSourceNormY").toDouble() : 0.0;
                s.srcNw = so.contains("spanSourceNormW") ? so.value("spanSourceNormW").toDouble() : 1.0;
                s.srcNh = so.contains("spanSourceNormH") ? so.value("spanSourceNormH").toDouble() : 1.0;
                s.spanId = QStringLiteral("%1:%2:%3")
                    .arg(item->mediaId).arg(s.screenId).arg(spanIndex);
                item->spans.append(s);
            }
        }
        if (item->spans.isEmpty()) {
            qWarning() << "RemoteSceneController: media item" << item->mediaId << "missing spans; skipping placement";
        }
        item->autoDisplay = m.value("autoDisplay").toBool(false);
        item->autoDisplayDelayMs = m.value("autoDisplayDelayMs").toInt(0);
        item->autoPlay = m.value("autoPlay").toBool(false);
        item->autoPlayDelayMs = m.value("autoPlayDelayMs").toInt(0);
        item->autoPause = m.value("autoPause").toBool(false);
        item->autoPauseDelayMs = m.value("autoPauseDelayMs").toInt(0);
        item->autoHide = m.value("autoHide").toBool(false);
        item->autoHideDelayMs = m.value("autoHideDelayMs").toInt(0);
        item->hideWhenVideoEnds = m.value("hideWhenVideoEnds").toBool(false);
        item->fadeInSeconds = m.value("fadeInSeconds").toDouble(0.0);
        item->fadeOutSeconds = m.value("fadeOutSeconds").toDouble(0.0);
        item->contentOpacity = std::clamp(m.value("contentOpacity").toDouble(1.0), 0.0, 1.0);
        item->continuousLoop = m.value("continuousLoop").toBool(false);
        item->repeatEnabled = m.value("repeatEnabled").toBool(false);
        item->repeatCount = std::max(0, m.value("repeatCount").toInt(0));
        item->repeatRemaining = 0;
        item->repeatActive = false;
        if (item->type == "video") {
            item->muted = m.value("muted").toBool(false);
            item->volume = m.value("volume").toDouble(1.0);
            item->autoUnmute = m.value("autoUnmute").toBool(false);
            item->autoUnmuteDelayMs = m.value("autoUnmuteDelayMs").toInt(0);
            item->autoMute = m.value("autoMute").toBool(false);
            item->autoMuteDelayMs = m.value("autoMuteDelayMs").toInt(0);
            item->muteWhenVideoEnds = m.value("muteWhenVideoEnds").toBool(false);
            item->audioFadeInSeconds = std::max(0.0, m.value("audioFadeInSeconds").toDouble(0.0));
            item->audioFadeOutSeconds = std::max(0.0, m.value("audioFadeOutSeconds").toDouble(0.0));
            if (m.contains("startPositionMs")) {
                const qint64 startPos = static_cast<qint64>(std::llround(m.value("startPositionMs").toDouble(0.0)));
                item->startPositionMs = std::max<qint64>(0, startPos);
                item->hasStartPosition = true;
                item->awaitingStartFrame = item->startPositionMs > 0;
            } else {
                item->startPositionMs = 0;
                item->hasStartPosition = false;
                item->awaitingStartFrame = false;
            }
            if (m.contains("displayedFrameTimestampMs")) {
                const qint64 displayTs = static_cast<qint64>(std::llround(m.value("displayedFrameTimestampMs").toDouble(-1.0)));
                if (displayTs >= 0) {
                    item->displayTimestampMs = displayTs;
                    item->hasDisplayTimestamp = true;
                }
            }
            item->awaitingStartFrame = item->hasDisplayTimestamp
                || (item->hasStartPosition && item->startPositionMs > 0);
            item->frameSource = new RemoteVideoFrameSource(this);
            // QMediaPlayer streams MP4 from the validated local path. Ensure a
            // previous cache user cannot leave a whole video resident in RAM.
            m_fileManager->releaseFileMemory(item->fileId);
        }
        m_mediaItems.append(item);
        scheduleMedia(item);
    }

    m_totalMediaToPrime = m_mediaItems.size();
}

void RemoteSceneController::scheduleMedia(const std::shared_ptr<RemoteMediaItem>& item) {
    if (!item) return;
    if (item->spans.isEmpty()) {
        qWarning() << "RemoteSceneController: ignoring media with no spans" << item->mediaId << item->type;
        return;
    }
    scheduleMediaMulti(item);
}

void RemoteSceneController::scheduleMediaMulti(const std::shared_ptr<RemoteMediaItem>& item) {
    if (item->spans.isEmpty()) return;
    const quint64 epoch = item->sceneEpoch;
    item->hiding = false;
    if (item->hideTimer) {
        item->hideTimer->stop();
    }
    for (auto& span : item->spans) {
        publishMediaSpan(item, span);
    }


    // Content loading
    std::weak_ptr<RemoteMediaItem> weakItem = item;

    if (item->type == "text") {
        // QML reports readiness once the shared text delegate has been created.
        item->loaded = false;
    } else if (item->type == "image") {
        // Image.Ready is reported by ImageItem through RemoteSceneRoot.
        item->loaded = false;
    } else if (item->type == "video") {
        // The existing decoder remains authoritative; its QVideoSink feeds one
        // shared frame source rendered by every passive QML span.
        item->player = new QMediaPlayer(this);
        item->audio = new QAudioOutput(this);
        item->audio->setMuted(item->muted); 
        item->audio->setVolume(std::clamp(item->volume, 0.0, 1.0));
        item->player->setAudioOutput(item->audio);
        item->videoOutputsAttached = false;
        QObject::connect(item->player, &QMediaPlayer::mediaStatusChanged, item->player, [this,epoch,weakItem](QMediaPlayer::MediaStatus s){
            auto item = weakItem.lock();
            if (!item) return;
            if (epoch != m_sceneEpoch) return;
            if (s == QMediaPlayer::LoadedMedia || s == QMediaPlayer::BufferedMedia) {
                item->loaded = true;
                seekToConfiguredStart(item);
                evaluateItemReadiness(item);
            } else if (s == QMediaPlayer::EndOfMedia && item->player) {
                // EndOfMedia reached: ensure we freeze on the final frame when not looping.
                if (item->repeatActive) return;
                const bool canRepeat = item->playAuthorized
                    && (item->continuousLoop || (item->repeatEnabled && item->repeatRemaining > 0));
                if (canRepeat) {
                    item->repeatActive = true;
                    item->lastRepeatTriggerMs = QDateTime::currentMSecsSinceEpoch();
                    if (!item->continuousLoop) --item->repeatRemaining;
                    item->pausedAtEnd = false;
                    item->holdLastFrameAtEnd = false;
                    restoreVideoOutput(item);
                    item->player->setPosition(0);
                    item->player->play();
                } else {
                    if (!item->pausedAtEnd) {
                        item->pausedAtEnd = true;
                        item->player->pause();
                    }
                    freezeVideoOutput(item);
                }
            }
        });
        QObject::connect(item->player, &QMediaPlayer::positionChanged, item->player, [this,epoch,weakItem](qint64 pos){
            auto item = weakItem.lock();
            if (!item) return;
            if (epoch != m_sceneEpoch) return;
            if (!item->player) return;

            const qint64 dur = item->player->duration();
            const qint64 repeatWindowMs = repeatLeadMarginMs(dur);
            const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
            if (nowMs < item->authoritativeSeekGuardUntilMs) return;
            const bool repeatJustSettled = item->repeatActive
                && (pos <= repeatWindowMs
                    || (item->lastRepeatTriggerMs > 0
                        && (nowMs - item->lastRepeatTriggerMs) > 500));
            if (repeatJustSettled) {
                item->repeatActive = false;
                item->lastRepeatTriggerMs = 0;
            }

            if (dur <= 0 || pos <= 0) return;

            const bool repeatAvailable = item->playAuthorized
                && (item->continuousLoop || (item->repeatEnabled && item->repeatRemaining > 0));
            if (repeatAvailable) {
                if (!repeatJustSettled && !item->repeatActive && pos >= (dur - repeatWindowMs)) {
                    item->repeatActive = true;
                    item->lastRepeatTriggerMs = nowMs;
                    item->pausedAtEnd = false;
                    item->player->setPosition(0);
                    item->player->play();
                    if (!item->continuousLoop) --item->repeatRemaining;
                }
                return;
            }

            // Handle pre-end mute trigger for negative delays
            if (item->muteWhenVideoEnds && !item->muteEndTriggered && item->autoMuteDelayMs < 0 && item->audio) {
                const qint64 offsetMs = -static_cast<qint64>(item->autoMuteDelayMs);
                if ((dur - pos) <= offsetMs) {
                    applyAudioMuteState(item, true);
                    item->muteEndTriggered = true;
                }
            }

            // Handle pre-end hide trigger for negative delays
            if (item->hideWhenVideoEnds && !item->hideEndTriggered && item->autoHideDelayMs < 0) {
                const qint64 offsetMs = -static_cast<qint64>(item->autoHideDelayMs);
                if ((dur - pos) <= offsetMs) {
                    item->hideEndTriggered = true;
                    fadeOutAndHide(item);
                }
            }

            if (item->pausedAtEnd) return;
        });
        QObject::connect(item->player, &QMediaPlayer::errorOccurred, item->player, [this,epoch,weakItem](QMediaPlayer::Error e, const QString& err){ auto item = weakItem.lock(); if (!item) return; if (epoch != m_sceneEpoch) return; if (e != QMediaPlayer::NoError) qWarning() << "RemoteSceneController: player error" << int(e) << err << "for" << item->mediaId; });
    auto attemptLoadVid = [this, epoch, weakItem]() {
            auto item = weakItem.lock();
            if (!item) return false;
            if (epoch != m_sceneEpoch) return false;
            QString path = m_fileManager->getFilePathForId(item->fileId);
            if (!path.isEmpty() && QFileInfo::exists(path)) {
                item->pausedAtEnd = false;
                item->player->setSource(QUrl::fromLocalFile(path));

                item->player->setLoops(QMediaPlayer::Once);
                item->repeatRemaining = (item->repeatEnabled && item->repeatCount > 0)
                                             ? item->repeatCount
                                             : 0;

                // Prime the first frame if not already done
                if (!item->primedFirstFrame) {
                    if (!item->primingSink) {
                        item->primingSink = new QVideoSink(item->player);
                    }
                    QVideoSink* sink = item->primingSink;
                    if (item->player && sink) {
                        item->player->setVideoSink(sink);
                        item->videoOutputsAttached = false;
                    }
                    if (sink) {
                        qDebug() << "RemoteSceneController: start priming(multi)" << item->mediaId
                                 << "startMs" << (item->hasStartPosition ? item->startPositionMs : qint64(-1))
                                 << "displayTs" << (item->hasDisplayTimestamp ? item->displayTimestampMs : qint64(-1))
                                 << "awaitingStart" << item->awaitingStartFrame;
                        item->primingConn = QObject::connect(sink, &QVideoSink::videoFrameChanged, sink, [this,epoch,weakItem](const QVideoFrame& frame){
                            if (!frame.isValid()) {
                                return;
                            }
                            auto item = weakItem.lock();
                            if (!item) return;
                            if (epoch != m_sceneEpoch) return;
                            // Safety check: verify priming sink still exists
                            if (!item->primingSink) return;
                            // Safety check: verify player still exists
                            if (!item->player) return;
                            const qint64 desired = targetDisplayTimestamp(item);
                            const qint64 frameTime = frameTimestampMs(frame);
                            const bool hasFrameTimestamp = frameTime >= 0;
                            const qint64 playerPos = item->player ? item->player->position() : -1;
                            const qint64 reference = hasFrameTimestamp ? frameTime : playerPos;

                            auto logDecision = [&](const QString& stage, const QString& reason, bool accepted) {
                                qDebug() << "RemoteSceneController: priming(multi)" << stage
                                         << "media" << item->mediaId
                                         << "reason" << reason
                                         << "desired" << desired
                                         << "frameTs" << (hasFrameTimestamp ? frameTime : qint64(-1))
                                         << "playerPos" << playerPos
                                         << "delta" << ((desired >= 0 && (hasFrameTimestamp || playerPos >= 0)) ? ((hasFrameTimestamp ? frameTime : playerPos) - desired) : qint64(0))
                                         << "displayTs" << (item->hasDisplayTimestamp ? item->displayTimestampMs : qint64(-1))
                                         << "startMs" << (item->hasStartPosition ? item->startPositionMs : qint64(-1))
                                         << "awaiting" << item->awaitingStartFrame
                                         << "accepted" << accepted;
                            };

                            if (!item->primedFirstFrame) {
                                bool frameReady = true;
                                bool overshoot = false;
                                if (item->awaitingStartFrame && desired >= 0) {
                                    if (reference >= 0) {
                                        const qint64 tolerance = hasFrameTimestamp
                                            ? kStartFrameTimestampToleranceMs
                                            : kSeekPositionToleranceMs;
                                        if (reference < desired - tolerance) {
                                            frameReady = false;
                                        } else if (reference > desired + tolerance) {
                                            frameReady = false;
                                            overshoot = true;
                                        }
                                    }
                                    if (!frameReady) {
                                        logDecision(QStringLiteral("reject"), overshoot ? QStringLiteral("overshoot") : QStringLiteral("pre-start"), false);
                                        if (item->player) {
                                            if (overshoot) {
                                                item->player->pause();
                                                item->player->setPosition(desired);
                                            }
                                            if (item->player->playbackState() != QMediaPlayer::PlayingState) {
                                                item->player->play();
                                            }
                                        }
                                        item->primedFrame = QVideoFrame();
                                        item->primedFrameSticky = false;
                                        clearRenderedFrames(item);
                                        return;
                                    }
                                }

                                const QImage convertedFrame = convertFrameToImage(frame);
                                if (convertedFrame.isNull()) {
                                    qWarning() << "RemoteSceneController: first video frame is not renderable"
                                               << item->mediaId;
                                    const QString owner = m_pendingSenderClientId;
                                    const QString failedSceneInstanceId = m_pendingSceneInstanceId;
                                    if (m_ws && !owner.isEmpty() && !failedSceneInstanceId.isEmpty()) {
                                        m_ws->sendScenePrepared(
                                            failedSceneInstanceId,
                                            false,
                                            m_prepareChecklist,
                                            QStringLiteral("video_frame_unrenderable"),
                                            QStringLiteral("Video decoder produced a frame that cannot be rendered"));
                                    }
                                    const quint64 failureEpoch = ++m_sceneEpoch;
                                    QMetaObject::invokeMethod(this, [this, failureEpoch]() {
                                        if (failureEpoch == m_sceneEpoch) clearScene();
                                    }, Qt::QueuedConnection);
                                    return;
                                }

                                logDecision(QStringLiteral("accept"), QStringLiteral("frame within tolerance"), true);

                                item->awaitingStartFrame = false;
                                item->primedFirstFrame = true;
                                item->primedFrame = frame;
                                item->primedFrameSticky = true;
                                item->lastFrameImage = convertedFrame;
                                if (hasFrameTimestamp) {
                                    item->displayTimestampMs = frameTime;
                                    item->hasDisplayTimestamp = true;
                                }
                                item->decoderSyncTargetMs = desired;
                                item->livePlaybackStarted = false;
                                item->lastLiveFrameTimestampMs = -1;
                                if (item->autoPlay) {
                                    item->awaitingLivePlayback = true;
                                    item->liveWarmupFramesRemaining = kLivePlaybackWarmupFrames;
                                } else {
                                    item->awaitingLivePlayback = false;
                                    item->liveWarmupFramesRemaining = 0;
                                }
                                if (item->player) {
                                    item->player->pause();
                                    if (item->player->position() != desired) {
                                        item->player->setPosition(desired >= 0 ? desired : 0);
                                    }
                                }
                                applyImageToSpans(item, convertedFrame);
                                evaluateItemReadiness(item);
                                // Allow display even if awaiting live playback, so autoDisplay works immediately
                                if (item->autoDisplay && item->displayReady && !item->displayStarted && !autoDisplayDelayActive(item)) {
                                    fadeIn(item);
                                }
                                return;
                            }

                            if (!item->awaitingDecoderSync) {
                                return;
                            }

                            const qint64 target = (item->decoderSyncTargetMs >= 0) ? item->decoderSyncTargetMs : desired;
                            if (target < 0) {
                                return;
                            }
                            const qint64 gateReference = reference;
                            if (gateReference >= 0 && gateReference >= target - kDecoderSyncToleranceMs) {
                                qDebug() << "RemoteSceneController: decoder sync reached" << item->mediaId
                                         << "target" << target
                                         << "frameTs" << (hasFrameTimestamp ? frameTime : qint64(-1))
                                         << "playerPos" << playerPos;
                                item->awaitingDecoderSync = false;
                                item->decoderSyncTargetMs = -1;
                                if (hasFrameTimestamp) {
                                    item->displayTimestampMs = frameTime;
                                    item->hasDisplayTimestamp = true;
                                }
                                item->primedFrame = frame;
                                item->primedFrameSticky = false;
                                QObject::disconnect(item->primingConn);
                                item->primingConn = {};
                                if (item->primingSink) {
                                    QObject::disconnect(item->primingSink, nullptr, nullptr, nullptr);
                                    auto* sinkToDelete = item->primingSink;
                                    item->primingSink = nullptr;
                                    sinkToDelete->deleteLater();
                                }
                                if (item->liveWarmupFramesRemaining <= 0) {
                                    item->liveWarmupFramesRemaining = kLivePlaybackWarmupFrames;
                                }
                                ensureVideoOutputsAttached(item);
                                if (item->player && item->player->playbackState() != QMediaPlayer::PlayingState) {
                                    item->player->play();
                                }
                                startPendingPauseTimerIfEligible(item);
                            }
                        });
                    } else {
                        qWarning() << "RemoteSceneController: primary video sink unavailable for priming" << item->mediaId;
                    }
                    if (item->audio) applyAudioMuteState(item, true, true);
                    item->pausedAtEnd = false;
                    if (item->player && item->player->playbackState() != QMediaPlayer::PlayingState) {
                        item->player->play();
                    }
                }
                return true;
            }
            return false;
        };
        if (!attemptLoadVid()) {
			// Upload inventory is already committed before PREPARE. Do not run a
			// client-side retry schedule that can outlive or second-guess the
			// server's preparation deadline; the policy timeout fails this graph.
			qWarning() << "RemoteSceneController: video source unavailable during preparation"
					   << item->mediaId;
        }
    }

    // Display/play scheduling
    if (item->autoDisplay) {
        int delay = std::max(0, item->autoDisplayDelayMs);
        // Mark displayReady immediately so fade-in can trigger once delay elapses
        item->displayReady = true;
        item->displayTimer = new QTimer(this); item->displayTimer->setSingleShot(true);
        connect(item->displayTimer, &QTimer::timeout, this, [this,epoch,weakItem]() {
            auto item = weakItem.lock();
            if (!item) return;
            if (epoch != m_sceneEpoch) return;
            fadeIn(item);
        });
        item->pendingDisplayDelayMs = delay;
        if (m_sceneActivated) {
            item->displayTimer->start(delay);
            item->pendingDisplayDelayMs = -1;
        }
    } else {
        item->pendingDisplayDelayMs = -1;
    }
    if (item->player && item->autoPlay) {
        int playDelay = std::max(0, item->autoPlayDelayMs);
        item->playTimer = new QTimer(this);
        item->playTimer->setSingleShot(true);
        connect(item->playTimer, &QTimer::timeout, this, [this,epoch,weakItem]() {
            triggerAutoPlayNow(weakItem.lock(), epoch);
        });
        item->pendingPlayDelayMs = playDelay;
        if (m_sceneActivated) {
            if (playDelay == 0) {
                if (item->playTimer->isActive()) item->playTimer->stop();
                triggerAutoPlayNow(item, epoch);
                qDebug() << "RemoteSceneController: immediate play for (multi-span)" << item->mediaId;
            } else {
                item->playTimer->start(playDelay);
            }
            item->pendingPlayDelayMs = -1;
        }
        
        // Pause scheduling: pause video after configured delay if autoPause enabled
        if (item->autoPause) {
            int pauseDelay = std::max(0, item->autoPauseDelayMs);
            item->pauseTimer = new QTimer(this);
            item->pauseTimer->setSingleShot(true);
            connect(item->pauseTimer, &QTimer::timeout, this, [this,epoch,weakItem]() {
                auto item = weakItem.lock();
                if (!item) return;
                if (epoch != m_sceneEpoch) return;
                if (!item->player) return;
                // Cancel any decoder-sync continuation as well as the raw
                // player. The backend can be transiently paused for a re-seek
                // when this timer fires, so the logical state must be cleared
                // unconditionally or a later priming frame can call play().
                item->playAuthorized = false;
                item->awaitingDecoderSync = false;
                item->awaitingLivePlayback = false;
                item->player->pause();
                qDebug() << "RemoteSceneController: auto-paused video (multi-span)" << item->mediaId;
            });
            item->pendingPauseDelayMs = pauseDelay;
            if (m_sceneActivated) {
                if (item->awaitingStartFrame) {
                    qDebug() << "RemoteSceneController: deferring pause until start frame for (multi-span)" << item->mediaId << "delay" << pauseDelay;
                } else {
                    startPendingPauseTimerIfEligible(item);
                    if (pauseDelay == 0) {
                        qDebug() << "RemoteSceneController: immediate pause scheduled for (multi-span)" << item->mediaId;
                    }
                }
            }
        } else {
            item->pendingPauseDelayMs = -1;
        }
    } else {
        item->pendingPlayDelayMs = -1;
        item->pendingPauseDelayMs = -1;
    }

    evaluateItemReadiness(item);
}

void RemoteSceneController::fadeIn(const std::shared_ptr<RemoteMediaItem>& item) {
    if (!item) return;
    if (!m_sceneActivated) {
        item->fadeInPending = true;
        item->displayReady = true;
        return;
    }
    // Only block fade-in if awaiting live playback AND autoDisplay is false
    // (autoDisplay items wait for their configured display delay instead)
    if (item->awaitingLivePlayback && !item->livePlaybackStarted && !item->autoDisplay) {
        item->fadeInPending = true;
        return;
    }
    if (item->displayStarted) return;
    item->fadeInPending = false;
    item->displayStarted = true;
    item->displayReady = true;
    item->hiding = false;
    item->contentVisible = true;
    if (item->hideTimer) {
        item->hideTimer->stop();
    }
    if (item->type == "video" && item->primedFrameSticky) {
        applyPrimedFrameToSinks(item);
    }
    const int durMs = int(item->fadeInSeconds * 1000.0);
    std::weak_ptr<RemoteMediaItem> weakItem = item;
    // Match the host timeline: auto-hide delay is measured from the moment
    // fade-in starts, not from the moment the fade finishes.
    scheduleHideTimer(item);
    if (item->spans.isEmpty()) {
        qWarning() << "RemoteSceneController: fadeIn requested with no spans" << item->mediaId;
        return;
    }

    if (item->visualFadeAnimation) {
        QVariantAnimation* previous = item->visualFadeAnimation.data();
        QObject::disconnect(previous, nullptr, this, nullptr);
        previous->stop();
        previous->deleteLater();
        item->visualFadeAnimation = nullptr;
    }

    if (durMs <= 10) {
        setRemoteMediaVisualState(item, item->contentOpacity, true);
        return;
    }

    setRemoteMediaVisualState(item, item->renderOpacity, true);
    auto* animation = new QVariantAnimation(this);
    animation->setStartValue(item->renderOpacity);
    animation->setEndValue(item->contentOpacity);
    animation->setDuration(durMs);
    animation->setEasingCurve(QEasingCurve::Linear);
    connect(animation, &QVariantAnimation::valueChanged, this, [this, weakItem](const QVariant& value) {
        auto locked = weakItem.lock();
        if (!locked) return;
        setRemoteMediaVisualState(locked, value.toReal(), true);
    });
    connect(animation, &QVariantAnimation::finished, this, [weakItem, animation]() {
        auto locked = weakItem.lock();
        if (locked && locked->visualFadeAnimation == animation) locked->visualFadeAnimation = nullptr;
        animation->deleteLater();
    });
    item->visualFadeAnimation = animation;
    animation->start();
}

void RemoteSceneController::scheduleHideTimer(const std::shared_ptr<RemoteMediaItem>& item) {
    if (!item) return;
    if (!item->autoHide) return;
    if (item->hideWhenVideoEnds) return;
    if (item->hiding) return;
    const int delayMs = std::max(0, item->autoHideDelayMs);
    if (!item->hideTimer) {
        item->hideTimer = new QTimer(this);
        item->hideTimer->setSingleShot(true);
        std::weak_ptr<RemoteMediaItem> weakItem = item;
        connect(item->hideTimer, &QTimer::timeout, this, [this, weakItem]() {
            auto locked = weakItem.lock();
            if (!locked) return;
            fadeOutAndHide(locked);
        });
    }
    if (!item->hideTimer) return;
    item->hideTimer->stop();
    item->hideTimer->start(delayMs);
}

void RemoteSceneController::cancelAudioFade(const std::shared_ptr<RemoteMediaItem>& item, bool applyFinalState) {
    if (!item) return;
    if (!item->audioFadeAnimation) return;
    QVariantAnimation* anim = item->audioFadeAnimation.data();
    QObject::disconnect(anim, nullptr, this, nullptr);
    anim->stop();
    anim->deleteLater();
    item->audioFadeAnimation = nullptr;
    if (applyFinalState && item->audio) {
        const qreal targetVolume = item->muted ? 0.0 : std::clamp<qreal>(item->volume, 0.0, 1.0);
        item->audio->setMuted(item->muted);
        item->audio->setVolume(targetVolume);
    }
}

void RemoteSceneController::applyAudioMuteState(const std::shared_ptr<RemoteMediaItem>& item, bool muted, bool skipFade) {
    if (!item) return;
    if (!item->audio) return;

    const qreal clampedTargetVolume = muted ? 0.0 : std::clamp<qreal>(item->volume, 0.0, 1.0);
    const double fadeSeconds = skipFade ? 0.0 : (muted ? item->audioFadeOutSeconds : item->audioFadeInSeconds);

    const bool deviceMuted = item->audio->isMuted();
    const qreal deviceVolume = std::clamp<qreal>(item->audio->volume(), 0.0, 1.0);

    if (muted == item->muted && !item->audioFadeAnimation) {
        if (deviceMuted == muted && std::abs(deviceVolume - clampedTargetVolume) < 0.0001) {
            item->audio->setMuted(muted);
            item->audio->setVolume(clampedTargetVolume);
            return;
        }
    }

    cancelAudioFade(item, false);

    if (fadeSeconds <= 0.0) {
        item->audio->setMuted(muted);
        item->audio->setVolume(clampedTargetVolume);
        item->muted = muted;
        return;
    }

    qreal startVolume = deviceVolume;
    if (!muted && (deviceMuted || item->muted)) {
        startVolume = 0.0;
    }
    const qreal endVolume = muted ? 0.0 : clampedTargetVolume;

    if (std::abs(startVolume - endVolume) < 0.0001) {
        item->audio->setMuted(muted);
        item->audio->setVolume(endVolume);
        item->muted = muted;
        return;
    }

    item->audio->setMuted(false);
    item->audio->setVolume(startVolume);

    std::weak_ptr<RemoteMediaItem> weakItem = item;
    auto* anim = new QVariantAnimation(this);
    anim->setDuration(static_cast<int>(fadeSeconds * 1000.0));
    anim->setStartValue(startVolume);
    anim->setEndValue(endVolume);
    // Match ResizableVideoItem's authoritative audio envelope.
    anim->setEasingCurve(QEasingCurve::Linear);
    connect(anim, &QVariantAnimation::valueChanged, this, [weakItem](const QVariant& v) {
        auto locked = weakItem.lock();
        if (!locked || !locked->audio) return;
        const qreal value = std::clamp<qreal>(v.toDouble(), 0.0, 1.0);
        locked->audio->setVolume(value);
    });
    connect(anim, &QVariantAnimation::finished, this, [weakItem, muted, endVolume, anim]() {
        auto locked = weakItem.lock();
        if (!locked || !locked->audio) {
            anim->deleteLater();
            return;
        }
        locked->audio->setMuted(muted);
        locked->audio->setVolume(endVolume);
        if (locked->audioFadeAnimation == anim) {
            locked->audioFadeAnimation = nullptr;
        }
        anim->deleteLater();
    });
    connect(anim, &QObject::destroyed, this, [weakItem, anim]() {
        auto locked = weakItem.lock();
        if (!locked) return;
        if (locked->audioFadeAnimation == anim) {
            locked->audioFadeAnimation = nullptr;
        }
    });

    item->audioFadeAnimation = anim;
    item->muted = muted;
    anim->start();
}

void RemoteSceneController::scheduleMuteTimer(const std::shared_ptr<RemoteMediaItem>& item) {
    if (!item) return;
    if (!item->autoMute) return;
    if (item->muteWhenVideoEnds) return;
    if (!item->audio) return;
    const int delayMs = std::max(0, item->autoMuteDelayMs);
    if (item->muteTimer) {
        item->muteTimer->stop();
    }
    if (delayMs == 0) {
        applyAudioMuteState(item, true);
        return;
    }
    if (!item->muteTimer) {
        item->muteTimer = new QTimer(this);
        item->muteTimer->setSingleShot(true);
        std::weak_ptr<RemoteMediaItem> weakItem = item;
        connect(item->muteTimer, &QTimer::timeout, this, [this, weakItem]() {
            auto locked = weakItem.lock();
            if (!locked) return;
            if (locked->sceneEpoch != m_sceneEpoch) return;
            if (!locked->audio) return;
            applyAudioMuteState(locked, true);
        });
    }
    item->muteTimer->start(delayMs);
}

void RemoteSceneController::fadeOutAndHide(const std::shared_ptr<RemoteMediaItem>& item) {
    if (!item) return;
    if (item->hiding) return;
    item->hiding = true;
    if (item->hideTimer) {
        item->hideTimer->stop();
    }
    const int durMs = int(std::max(0.0, item->fadeOutSeconds) * 1000.0);
    std::weak_ptr<RemoteMediaItem> weakItem = item;
    auto finalize = [this, weakItem]() {
        auto locked = weakItem.lock();
        if (!locked) return;
        locked->contentVisible = false;
        setRemoteMediaVisualState(locked, 0.0, false);
        locked->displayStarted = false;
        locked->displayReady = false;
        locked->hiding = false;
    };

    if (item->spans.isEmpty()) {
        qWarning() << "RemoteSceneController: fadeOut requested with no spans" << item->mediaId;
        finalize();
        return;
    }
    if (durMs <= 10) {
        finalize();
        return;
    }

    if (item->visualFadeAnimation) {
        QVariantAnimation* previous = item->visualFadeAnimation.data();
        QObject::disconnect(previous, nullptr, this, nullptr);
        previous->stop();
        previous->deleteLater();
        item->visualFadeAnimation = nullptr;
    }

    auto* animation = new QVariantAnimation(this);
    animation->setStartValue(item->renderOpacity);
    animation->setEndValue(0.0);
    animation->setDuration(durMs);
    animation->setEasingCurve(QEasingCurve::Linear);
    connect(animation, &QVariantAnimation::valueChanged, this, [this, weakItem](const QVariant& value) {
        auto locked = weakItem.lock();
        if (!locked) return;
        setRemoteMediaVisualState(locked, value.toReal(), true);
    });
    connect(animation, &QVariantAnimation::finished, this, [weakItem, finalize, animation]() {
        auto locked = weakItem.lock();
        if (locked && locked->visualFadeAnimation == animation) locked->visualFadeAnimation = nullptr;
        animation->deleteLater();
        finalize();
    });
    item->visualFadeAnimation = animation;
    animation->start();
}
