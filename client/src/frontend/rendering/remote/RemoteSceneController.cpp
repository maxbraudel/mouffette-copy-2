#include "frontend/rendering/remote/RemoteSceneController.h"
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
#include <QQmlContext>
#include <QQmlEngine>
#include <QQuickItem>
#include <QPointer>
#include <QHash>
#include <QSet>
#include <QVariant>
#include <QCoreApplication>
#include <QEvent>
#include <QEventLoop>
#include <QAccessible>
#include <QDateTime>
#include <cmath>
#include "backend/files/FileManager.h"
#include "backend/platform/macos/MacWindowManager.h"
#include <algorithm>
#include <limits>
#include <memory>
#include <QVideoFrameFormat>


namespace {
constexpr qint64 kSeekPositionToleranceMs = 120;
constexpr qint64 kStartFrameTimestampToleranceMs = 25;
constexpr qint64 kDecoderSyncToleranceMs = 25;
constexpr int kLivePlaybackWarmupFrames = 2;
// Clock differences larger than a short LAN scheduling/network allowance make
// an absolute wall-clock deadline less accurate than the bounded relative
// delay carried alongside it.
constexpr qint64 kActivationClockToleranceMs = 250;
constexpr qint64 kMaxActivationDelayMs = 5000;
constexpr int kActivationWatchdogGraceMs = 2000;
constexpr qint64 kVideoSyncPositionToleranceMs = 400;
constexpr qint64 kMaxVideoSyncTransitMs = 2000;
constexpr int kVideoSyncWatchdogMs = 3000;
constexpr qint64 kMaxVideoPositionMs = 7LL * 24LL * 60LL * 60LL * 1000LL;
constexpr int kMaxVideoSyncItems = 512;
constexpr int kMaxRemoteScreens = 64;
constexpr int kMaxRemoteMediaItems = 512;
constexpr int kMaxRemoteSpansPerMedia = 64;
constexpr int kMaxRemoteTotalSpans = 4096;
constexpr int kMaxRemoteIdentifierLength = 128;
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
        connect(m_ws, &WebSocketClient::remoteSceneStartReceived, this, &RemoteSceneController::onRemoteSceneStart);
        connect(m_ws, &WebSocketClient::remoteSceneActivateReceived, this, &RemoteSceneController::onRemoteSceneActivate);
        connect(m_ws, &WebSocketClient::remoteSceneVideoSyncReceived, this, &RemoteSceneController::onRemoteSceneVideoSync);
        connect(m_ws, &WebSocketClient::remoteSceneStopReceived, this, &RemoteSceneController::onRemoteSceneStop);
        connect(m_ws, &WebSocketClient::disconnected,
                this, &RemoteSceneController::onConnectionLost,
                Qt::UniqueConnection);
        connect(m_ws, &WebSocketClient::connectionError,
                this, &RemoteSceneController::onConnectionError,
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

void RemoteSceneController::resetSceneSynchronization() {
    if (m_sceneReadyTimeout) {
        m_sceneReadyTimeout->stop();
        QObject::disconnect(m_sceneReadyTimeout, nullptr, this, nullptr);
        m_sceneReadyTimeout->deleteLater();
        m_sceneReadyTimeout = nullptr;
    }
    if (m_activationTimer) {
        m_activationTimer->stop();
        QObject::disconnect(m_activationTimer, nullptr, this, nullptr);
        m_activationTimer->deleteLater();
        m_activationTimer = nullptr;
    }
    if (m_videoSyncWatchdog) {
        m_videoSyncWatchdog->stop();
        QObject::disconnect(m_videoSyncWatchdog, nullptr, this, nullptr);
        m_videoSyncWatchdog->deleteLater();
        m_videoSyncWatchdog = nullptr;
    }
    m_pendingSenderClientId.clear();
    m_pendingSceneInstanceId.clear();
    m_totalMediaToPrime = 0;
    m_mediaReadyCount = 0;
    m_sceneActivationRequested = false;
    m_sceneActivated = false;
    m_activationEpochMs = 0;
    m_activationClockPlausible = false;
    m_lastVideoSyncSequence = 0;
    m_videoSyncWatchdogTripped = false;
}

void RemoteSceneController::armVideoSyncWatchdog() {
    if (!m_sceneActivated || m_pendingSceneInstanceId.isEmpty()) return;

    const bool hasVideo = std::any_of(
        m_mediaItems.cbegin(), m_mediaItems.cend(), [](const auto& item) {
            return item && item->type == QLatin1String("video") && item->player;
        });
    if (!hasVideo) return;

    if (!m_videoSyncWatchdog) {
        m_videoSyncWatchdog = new QTimer(this);
        m_videoSyncWatchdog->setSingleShot(true);
        m_videoSyncWatchdog->setTimerType(Qt::CoarseTimer);
        connect(m_videoSyncWatchdog, &QTimer::timeout,
                this, &RemoteSceneController::handleVideoSyncWatchdogTimeout);
    }
    m_videoSyncWatchdog->start(kVideoSyncWatchdogMs);
}

void RemoteSceneController::handleVideoSyncWatchdogTimeout() {
    if (!m_sceneActivated || m_pendingSceneInstanceId.isEmpty()) return;

    m_videoSyncWatchdogTripped = true;
    qWarning() << "RemoteSceneController: authoritative video sync timed out;"
                  " freezing and muting remote playback"
               << m_pendingSceneInstanceId;

    for (const auto& item : m_mediaItems) {
        if (!item || item->type != QLatin1String("video")) continue;

        item->playAuthorized = false;
        item->awaitingDecoderSync = false;
        item->awaitingLivePlayback = false;
        item->repeatActive = false;
        item->lastRepeatTriggerMs = 0;
        if (item->playTimer) item->playTimer->stop();
        if (item->pauseTimer) item->pauseTimer->stop();
        if (item->muteTimer) item->muteTimer->stop();
        if (item->player
            && item->player->playbackState() == QMediaPlayer::PlayingState) {
            item->player->pause();
        }
        if (item->audio) {
            applyAudioMuteState(item, true, true);
        }
        // Do not clear frameSource or alter visibility. The last successfully
        // rendered frame remains on every span until a valid owner/run sync
        // resumes playback or a correlated STOP tears the scene down.
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
            m_ws->sendRemoteSceneValidationResult(senderClientId, sceneInstanceId, false, errorMsg);
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
                m_ws->sendRemoteSceneValidationResult(senderClientId, sceneInstanceId, true);
                if (m_sceneActivated) {
                    m_ws->sendRemoteSceneLaunched(senderClientId, sceneInstanceId);
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
    QStringList unsupportedVideoNames;
    QStringList invalidMediaEntries;
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
        const QString path = m_fileManager->getFilePathForId(fileId);
        if (path.isEmpty() || !QFile::exists(path)) {
            QString fileName = mediaObj.value("fileName").toString();
            if (fileName.isEmpty()) {
                fileName = fileId;
            }
            missingFileNames.append(fileName);
        } else {
            const MediaFilePolicy::Kind actualKind = MediaFilePolicy::classifyLocalFile(path);
            QString fileName = mediaObj.value("fileName").toString();
            if (fileName.isEmpty()) fileName = fileId;
            if (type == QLatin1String("video") && actualKind != MediaFilePolicy::Kind::Mp4Video) {
                unsupportedVideoNames.append(fileName);
            } else if (type == QLatin1String("image") && actualKind != MediaFilePolicy::Kind::Image) {
                invalidMediaEntries.append(fileName);
            }
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

    if (!unsupportedVideoNames.isEmpty()) {
        failWithMessage(QStringLiteral("Unsupported video format (MP4 required): %1")
                            .arg(unsupportedVideoNames.join(QStringLiteral(", "))));
        return;
    }

    qDebug() << "RemoteSceneController: validation successful, preparing scene from" << senderClientId;

    const quint64 epoch = ++m_sceneEpoch;
    clearScene();
    // Flush deferred deletions multiple times to ensure ALL nested widget deletions complete
    drainDeferredDeletes(5, true);
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
    m_sceneReadyTimeout->start(11000);

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
        if (!window.quickWidget || window.quickWidget->status() == QQuickWidget::Error
            || !window.quickWidget->rootObject()) {
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
        
        // Process any remaining deferred deletions before showing windows
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);

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
    const qint64 fallbackDelayMs = std::clamp<qint64>(
        activationDelayMs, 0, kMaxActivationDelayMs);
    const bool absoluteDeadlinePlausible =
        absoluteRemainingMs >= -kActivationClockToleranceMs
        && absoluteRemainingMs <= kMaxActivationDelayMs
        && qAbs(absoluteRemainingMs - fallbackDelayMs) <= kActivationClockToleranceMs;
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
    // ACTIVATE is a commit, but retain a bounded watchdog until its timer fires.
    // This prevents malformed/future timestamps from pinning a prepared scene.
    if (m_sceneReadyTimeout) {
        m_sceneReadyTimeout->start(static_cast<int>(remainingMs) + kActivationWatchdogGraceMs);
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

    if (!m_enabled || !m_sceneActivated) return;
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
        && sampleAgeMs >= -kActivationClockToleranceMs
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
    m_videoSyncWatchdogTripped = false;

    for (const auto& item : m_mediaItems) {
        if (!item || item->type != QLatin1String("video") || !item->player) continue;
        const auto stateIt = stateByMediaId.constFind(item->mediaId);
        if (stateIt == stateByMediaId.cend()) continue;
        const QJsonObject state = stateIt.value();

        const bool playing = state.value("playing").toBool(false);
        const bool muted = state.value("muted").toBool(true);
        const bool visible = state.value("visible").toBool(false);
        const bool repeatAvailable = state.value("repeatAvailable").toBool(false);

        if (!m_videoSyncWatchdogTripped && item->muted != muted) {
            applyAudioMuteState(item, muted, true);
        }

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

        if (playing && !decoderTransitionInProgress && !m_videoSyncWatchdogTripped) {
            item->playAuthorized = true;
            item->pausedAtEnd = false;
            restoreVideoOutput(item);
            ensureVideoOutputsAttached(item);
            if (item->player->playbackState() != QMediaPlayer::PlayingState) {
                item->player->play();
            }
        }
    }

    armVideoSyncWatchdog();
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
            m_ws->sendRemoteSceneStopResult(
                senderClientId, resultSceneInstanceId, success, error);
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

    m_teardownInProgress = true;

    // CRITICAL: Cancel pending window show timer to prevent showing windows after scene cleared
    if (m_windowShowTimer) {
        m_windowShowTimer->stop();
        m_windowShowTimer->deleteLater();
        m_windowShowTimer = nullptr;
    }

    resetSceneSynchronization();
    
    // CRITICAL: Stop all fade animations first to prevent accessing deleted graphics items
    // Find all QVariantAnimation children and stop them immediately
    QList<QVariantAnimation*> animations = findChildren<QVariantAnimation*>();
    for (QVariantAnimation* anim : animations) {
        if (anim) {
            anim->stop();
            QObject::disconnect(anim, nullptr, nullptr, nullptr);
            anim->deleteLater();
        }
    }
    
    // Defensive teardown to handle rapid start/stop without use-after-free
    for (const auto& item : m_mediaItems) {
        teardownMediaItem(item);
    }
    m_mediaItems.clear();
    
    // Close remote screen windows so overlays disappear immediately after stop.
    // This releases their native cocoa windows while coordinating with Qt's
    // accessibility bridge to avoid macOS crashes when rapidly restarting scenes.
    for (auto it = m_screenWindows.begin(); it != m_screenWindows.end(); ++it) {
        ScreenWindow& sw = it.value();
        if (!sw.window) {
            continue;
        }

        QWidget* window = sw.window;
        sw.sceneEpoch = 0;

        QObject::disconnect(window, nullptr, nullptr, nullptr);
        window->hide();

        // Notify accessibility clients that the overlay is no longer visible.
        QAccessibleEvent hideEvent(window, QAccessible::ObjectHide);
        QAccessible::updateAccessibility(&hideEvent);

#ifdef Q_OS_MAC
        MacWindowManager::orderOutWindow(window);
#endif

        // Manually purge Qt's accessibility cache for this widget (fix for QTBUG-95134)
        QAccessibleInterface* iface = QAccessible::queryAccessibleInterface(window);
        if (iface) {
            QAccessible::Id id = QAccessible::uniqueId(iface);
            QAccessible::deleteAccessibleInterface(id);
        }

        if (sw.mediaModel) {
            sw.mediaModel->clearAll();
            sw.mediaModel = nullptr;
        }
        sw.mediaEntries.clear();
        if (sw.quickWidget) {
            QObject::disconnect(sw.quickWidget, nullptr, nullptr, nullptr);
            sw.quickWidget->setSource(QUrl());
            sw.quickWidget->deleteLater();
            sw.quickWidget = nullptr;
        }

        window->close();
        window->lower();

        QAccessibleEvent destroyEvent(window, QAccessible::ObjectDestroyed);
        QAccessible::updateAccessibility(&destroyEvent);

        window->setParent(nullptr);
        window->deleteLater();

        sw.window = nullptr;
    }

    m_screenWindows.clear();

    // Make sure deferred deletions run to completion before allowing another scene start
    // On macOS, process more cycles to ensure accessibility cleanup (QTBUG-95134)
#ifdef Q_OS_MAC
    drainDeferredDeletes(6, true);
#else
    drainDeferredDeletes(4, true);
#endif

    m_teardownInProgress = false;

    // Cancel any pending restart cooldown timer and restart if we still have a deferred request
    if (m_sceneRestartDelayTimer) {
        m_sceneRestartDelayTimer->stop();
        m_sceneRestartDelayTimer->deleteLater();
        m_sceneRestartDelayTimer = nullptr;
    }

    if (!m_sceneStartInProgress) {
        if (m_deferredSceneStart.valid) {
            m_restartCooldownActive = true;
            scheduleSceneRestartCooldown();
        } else {
            dispatchDeferredSceneStart();
        }
    }
}

void RemoteSceneController::dispatchDeferredSceneStart() {
    if (!m_deferredSceneStart.valid) {
        return;
    }

    if (m_restartCooldownActive) {
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

void RemoteSceneController::drainDeferredDeletes(int passes, bool allowEventProcessing) {
    if (passes <= 0) {
        return;
    }

    for (int i = 0; i < passes; ++i) {
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        if (allowEventProcessing) {
            QCoreApplication::processEvents(QEventLoop::ExcludeUserInputEvents);
        }
    }
}

void RemoteSceneController::scheduleSceneRestartCooldown() {
    // Increase cooldown on macOS to give accessibility bridge more time to clear (QTBUG-95134)
#ifdef Q_OS_MAC
    constexpr int kRestartCooldownMs = 150;
#else
    constexpr int kRestartCooldownMs = 60;
#endif

    if (!m_sceneRestartDelayTimer) {
        m_sceneRestartDelayTimer = new QTimer(this);
        m_sceneRestartDelayTimer->setSingleShot(true);
        connect(m_sceneRestartDelayTimer, &QTimer::timeout, this, [this]() {
            m_restartCooldownActive = false;
            if (m_sceneRestartDelayTimer) {
                m_sceneRestartDelayTimer->deleteLater();
                m_sceneRestartDelayTimer = nullptr;
            }
            dispatchDeferredSceneStart();
        });
    }

    if (m_sceneRestartDelayTimer->isActive()) {
        m_sceneRestartDelayTimer->stop();
    }
    m_sceneRestartDelayTimer->start(kRestartCooldownMs);
}

void RemoteSceneController::teardownMediaItem(const std::shared_ptr<RemoteMediaItem>& item) {
    if (!item) return;

    auto stopAndDeleteTimer = [](QTimer*& timer) {
        if (!timer) return;
        timer->stop();
        QObject::disconnect(timer, nullptr, nullptr, nullptr);
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

    cancelAudioFade(item, false);
    if (item->visualFadeAnimation) {
        QVariantAnimation* animation = item->visualFadeAnimation.data();
        QObject::disconnect(animation, nullptr, this, nullptr);
        animation->stop();
        animation->deleteLater();
        item->visualFadeAnimation = nullptr;
    }

    QObject::disconnect(item->deferredStartConn);
    QObject::disconnect(item->primingConn);
    QObject::disconnect(item->mirrorConn);
    item->pausedAtEnd = false;
    item->hideEndTriggered = false;
    item->muteEndTriggered = false;

    if (item->primingSink) {
        QObject::disconnect(item->primingSink, nullptr, nullptr, nullptr);
        item->primingSink->deleteLater();
        item->primingSink = nullptr;
    }

    if (item->liveSink) {
        QObject::disconnect(item->liveSink, nullptr, nullptr, nullptr);
        item->liveSink->deleteLater();
        item->liveSink = nullptr;
    }

    if (item->player) {
        QMediaPlayer* player = item->player;
        QObject::disconnect(player, nullptr, nullptr, nullptr);
        if (player->playbackState() != QMediaPlayer::StoppedState) {
        player->stop();
        }
        player->setVideoSink(nullptr);
        player->setSource(QUrl());
    }

    if (item->audio) {
        QObject::disconnect(item->audio, nullptr, nullptr, nullptr);
        item->audio->setMuted(true);
        item->audio->setVolume(0.0);
    }
    item->muted = true;

    item->spans.clear();

    if (item->player) {
        item->player->deleteLater();
        item->player = nullptr;
    }
    if (item->audio) {
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
    if (m_sceneReadyTimeout) {
        m_sceneReadyTimeout->start(10000);
    }
    if (m_ws && !m_pendingSenderClientId.isEmpty() && !m_pendingSceneInstanceId.isEmpty()) {
        m_ws->sendRemoteSceneValidationResult(
            m_pendingSenderClientId, m_pendingSceneInstanceId, true);
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
    if (m_videoSyncWatchdogTripped) return;

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
    m_sceneActivated = true;
    m_sceneActivationRequested = false;
    if (m_activationTimer) m_activationTimer->stop();

    if (m_sceneReadyTimeout) {
        m_sceneReadyTimeout->stop();
    }

    const quint64 activationEpoch = m_sceneEpoch;
    for (auto it = m_screenWindows.begin(); it != m_screenWindows.end(); ++it) {
        ScreenWindow& sw = it.value();
        if (!sw.window || sw.sceneEpoch != activationEpoch) {
            continue;
        }

        sw.window->show();
#ifdef Q_OS_MAC
        const int screenId = it.key();
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
                if (m_videoSyncWatchdogTripped) return;
                applyAudioMuteState(item, false);
            };
            
            if (unmuteDelayMs > 0) {
                QTimer::singleShot(unmuteDelayMs, this, unmuteCallback);
            } else {
                QTimer::singleShot(0, this, unmuteCallback);
            }
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
    armVideoSyncWatchdog();

    const QString sender = m_pendingSenderClientId;
    const QString sceneInstanceId = m_pendingSceneInstanceId;
    if (m_ws && !sender.isEmpty() && !sceneInstanceId.isEmpty()) {
        m_ws->sendRemoteSceneLaunched(sender, sceneInstanceId);
    }
}

void RemoteSceneController::handleSceneReadyTimeout() {
    const QString sender = m_pendingSenderClientId;
    qWarning() << "RemoteSceneController: timed out waiting for remote media to load" << sender;
    if (m_ws && !sender.isEmpty()) {
        m_ws->sendRemoteSceneValidationResult(
            sender,
            m_pendingSceneInstanceId,
            false,
            m_sceneActivationRequested
                ? QStringLiteral("Timed out waiting for scene activation")
                : QStringLiteral("Timed out waiting for remote media to load"));
    }
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
                                        m_ws->sendRemoteSceneValidationResult(
                                            owner,
                                            failedSceneInstanceId,
                                            false,
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
            for (int i=1;i<=5;++i) QTimer::singleShot(i*500, this, [attemptLoadVid]() { attemptLoadVid(); });
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
