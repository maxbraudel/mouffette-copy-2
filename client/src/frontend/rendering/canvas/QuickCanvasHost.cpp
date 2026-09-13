#include "frontend/rendering/canvas/QuickCanvasHost.h"

#include "backend/domain/canvas/CanvasDocument.h"
#include "backend/domain/media/CanvasMedia.h"
#include "backend/domain/media/MediaFilePolicy.h"
#include "backend/files/FileManager.h"
#include "backend/network/SceneRunCoordinator.h"
#include "backend/network/UploadManager.h"
#include "backend/network/WebSocketClient.h"
#include "frontend/rendering/canvas/QuickCanvasController.h"
#include "frontend/ui/notifications/ToastNotificationSystem.h"

#include <QDateTime>
#include <QFileInfo>
#include <QJsonArray>
#include <QMap>
#include <QQuickWindow>
#include <QRegularExpression>

#include <algorithm>

namespace {
constexpr int kStopTimeoutMs = 5000;

void sceneToast(NotificationSeverity severity, const QString& message,
                const QString& runId = {}, int duration = -1)
{
    ToastNotificationSystem* system = ToastNotificationSystem::instance();
    if (!system) return;
    NotificationRequest request;
    request.severity = severity;
    request.category = QStringLiteral("Scene");
    request.message = message;
    request.toastDurationMs = duration;
    request.sceneRunId = runId;
    if (!runId.isEmpty()) {
        request.correlationId = NotificationCorrelation::sceneRun(runId);
        request.terminal = severity == NotificationSeverity::Success
            || severity == NotificationSeverity::Error;
    }
    system->publishNotification(request);
}
}

QuickCanvasHost::QuickCanvasHost(CanvasDocument* document,
                                 QuickCanvasController* controller,
                                 QObject* parent)
    : ICanvasHost(parent)
    , m_document(document)
    , m_controller(controller)
{
    Q_ASSERT(m_document);
    Q_ASSERT(m_controller);
    m_document->setParent(this);
    m_controller->setParent(this);
    connect(m_document, &CanvasDocument::mediaAdded,
            this, &QuickCanvasHost::mediaItemAdded);
    connect(m_document, &CanvasDocument::mediaAboutToBeRemoved,
            this, &QuickCanvasHost::mediaItemRemoved);
    connect(m_document, &CanvasDocument::mediaChanged, this,
            [this](const QString& id) {
        if (CanvasMedia* media = m_document->mediaById(id)) {
            emit mediaItemChanged(media);
        }
    });
    m_sceneTimeout.setSingleShot(true);
    connect(&m_sceneTimeout, &QTimer::timeout, this, [this]() {
        if (m_sceneStopping) {
            stopScenePresentation();
            m_sceneStopping = false;
            m_sceneLaunched = false;
            const QString runId = m_sceneRunId;
            m_sceneRunId.clear();
            publishActionState();
            sceneToast(NotificationSeverity::Warning,
                       QStringLiteral("Remote stop acknowledgement timed out; scene stopped locally"),
                       runId, 5000);
        } else if (m_sceneLaunching) {
            failScene(QStringLiteral("Remote scene launch handshake timed out"), true);
        }
    });
    m_videoSnapshotTimer.setInterval(1000);
    connect(&m_videoSnapshotTimer, &QTimer::timeout,
            this, &QuickCanvasHost::sendVideoSnapshot);
}

QuickCanvasHost::~QuickCanvasHost()
{
    cancelPresentationBarrier();
    stopScenePresentation();
}

QuickCanvasHost* QuickCanvasHost::create(QString* errorMessage)
{
    auto* document = new CanvasDocument;
    auto* controller = new QuickCanvasController(document);
    if (!controller->initialize(errorMessage)) {
        delete controller;
        delete document;
        return nullptr;
    }
    return new QuickCanvasHost(document, controller);
}

QList<CanvasMedia*> QuickCanvasHost::enumerateMediaItems() const
{
    return m_document->media();
}

void QuickCanvasHost::deleteMediaItemCanonical(CanvasMedia* mediaItem)
{
    if (mediaItem) m_document->removeMedia(mediaItem->mediaId());
}

void QuickCanvasHost::setActiveIdeaId(const QString& id)
{
    m_document->setCanvasSessionId(id);
}

void QuickCanvasHost::setWebSocketClient(WebSocketClient* client)
{
    if (m_webSocket == client) return;
    if (m_webSocket) disconnect(m_webSocket, nullptr, this, nullptr);
    m_webSocket = client;
    connectWebSocketSignals();
    publishActionState();
}

void QuickCanvasHost::connectWebSocketSignals()
{
    if (!m_webSocket) return;
    connect(m_webSocket, &WebSocketClient::scenePreparedReceived, this,
            [this](const QJsonObject& envelope) {
        if (!matchesScene(envelope) || !m_sceneLaunching
            || !envelope.value(QStringLiteral("allPrepared")).toBool(false)) return;
        const qint64 uncertainty = m_webSocket->sceneClockUncertaintyMs();
        const qint64 allowed = m_webSocket->serverPolicy()
            .value(QStringLiteral("sceneMaxClockSkewMs")).toInteger(-1);
        if (uncertainty < 0 || allowed < 0 || uncertainty > allowed
            || !m_webSocket->sendSceneArmed(m_sceneRunId, uncertainty)) {
            failScene(QStringLiteral("Clock synchronization is not accurate enough"), true);
            return;
        }
        m_sceneArmed = true;
    });
    connect(m_webSocket, &WebSocketClient::sceneCommitReceived, this,
            [this](const QJsonObject& envelope) {
        if (!matchesScene(envelope) || !m_sceneLaunching || !m_sceneArmed) return;
        const qint64 start = envelope.value(QStringLiteral("startServerMonotonicMs"))
                                 .toInteger(-1);
        const qint64 now = m_webSocket->estimatedServerMonotonicMs();
        if (start < 0 || now < 0) {
            failScene(QStringLiteral("Invalid synchronized launch commitment"), true);
            return;
        }
        QTimer::singleShot(int(std::max<qint64>(0, start - now)), this, [this]() {
            if (!m_sceneLaunching) return;
            beginScenePresentation(true);
            startPresentationBarrier();
        });
    });
    connect(m_webSocket, &WebSocketClient::sceneStartedReceived, this,
            [this](const QJsonObject& envelope) {
        if (!matchesScene(envelope)
            || !envelope.value(QStringLiteral("allStarted")).toBool(false)) return;
        m_sceneTimeout.stop();
        m_sceneLaunching = false;
        m_sceneLaunched = true;
        m_videoSnapshotTimer.start();
        publishActionState();
        sceneToast(NotificationSeverity::Success,
                   QStringLiteral("Remote scene launched successfully!"),
                   m_sceneRunId, 3000);
    });
    connect(m_webSocket, &WebSocketClient::sceneStopReceived, this,
            [this](const QJsonObject& envelope) {
        if (!matchesScene(envelope)) return;
        stopScenePresentation();
        m_sceneLaunching = false;
        m_sceneLaunched = false;
        m_sceneStopping = true;
        m_webSocket->sendSceneStopped(m_sceneRunId, true);
        publishActionState();
    });
    connect(m_webSocket, &WebSocketClient::sceneStoppedReceived, this,
            [this](const QJsonObject& envelope) {
        if (!matchesScene(envelope)) return;
        const QString runId = m_sceneRunId;
        m_sceneTimeout.stop();
        stopScenePresentation();
        m_sceneLaunching = false;
        m_sceneLaunched = false;
        m_sceneStopping = false;
        m_sceneArmed = false;
        m_sceneRunId.clear();
        m_sceneDigest.clear();
        publishActionState();
        sceneToast(NotificationSeverity::Success,
                   QStringLiteral("Remote scene stopped successfully"),
                   runId, 3000);
    });
    connect(m_webSocket, &WebSocketClient::sceneErrorReceived, this,
            [this](const QJsonObject& envelope) {
        if (envelope.value(QStringLiteral("sceneRunId")).toString() == m_sceneRunId) {
            failScene(envelope.value(QStringLiteral("message")).toString(
                          QStringLiteral("Remote scene protocol error")), false);
        }
    });
    connect(m_webSocket, &WebSocketClient::remoteSessionResumed, this,
            [this](const QJsonObject& envelope) {
        if (envelope.value(QStringLiteral("requestStateSnapshot")).toBool(false)
            && m_sceneLaunched) sendVideoSnapshot();
    });
}

void QuickCanvasHost::setUploadManager(UploadManager* manager)
{
    if (m_uploadManager == manager) return;
    if (m_uploadManager) disconnect(m_uploadManager, nullptr, this, nullptr);
    m_uploadManager = manager;
    if (m_uploadManager) {
        connect(m_uploadManager, &UploadManager::uiStateChanged,
                this, &QuickCanvasHost::publishActionState);
    }
    publishActionState();
}

void QuickCanvasHost::setFileManager(FileManager* manager)
{
    m_fileManager = manager;
    m_document->setFileManager(manager);
}

void QuickCanvasHost::setRemoteSceneTarget(const QString& id,
                                           const QString& machineName)
{
    m_targetClientId = id;
    m_targetMachineName = machineName;
    publishActionState();
}

void QuickCanvasHost::updateRemoteSceneTargetFromClientList(
    const QList<ClientInfo>& clients)
{
    for (const ClientInfo& client : clients) {
        if (client.endpointId() == m_targetClientId) {
            m_targetMachineName = client.getMachineName();
            break;
        }
    }
}

void QuickCanvasHost::setScreens(const QList<ScreenInfo>& screens)
{
    m_document->setScreens(screens);
    if (m_controller) m_controller->ensureInitialFit();
    publishActionState();
}

bool QuickCanvasHost::hasActiveScreens() const
{
    return m_document->hasActiveScreens();
}

void QuickCanvasHost::requestDeferredInitialRecenter(int marginPx)
{
    QTimer::singleShot(0, this, [this, marginPx]() {
        if (m_controller) m_controller->ensureInitialFit(marginPx);
    });
}

void QuickCanvasHost::recenterWithMargin(int)
{
    if (m_controller) m_controller->recenterView();
}

void QuickCanvasHost::hideContentPreservingState()
{
    m_contentAvailable = false;
    m_document->setContentAvailable(false);
}

void QuickCanvasHost::showContentAfterReconnect()
{
    m_contentAvailable = true;
    m_document->setContentAvailable(true);
}

void QuickCanvasHost::resetTransform()
{
    if (m_controller) m_controller->resetView();
}

void QuickCanvasHost::updateRemoteCursor(int x, int y)
{
    if (m_controller) m_controller->updateRemoteCursor(x, y);
}

void QuickCanvasHost::hideRemoteCursor()
{
    if (m_controller) m_controller->hideRemoteCursor();
}

void QuickCanvasHost::setOverlayActionsEnabled(bool enabled)
{
    if (m_actionsEnabled == enabled) return;
    m_actionsEnabled = enabled;
    if (m_controller) m_controller->setShellActive(enabled);
    publishActionState();
}

void QuickCanvasHost::setCurrentTool(Tool tool)
{
    if (m_tool == tool || m_document->editsLocked()) return;
    m_tool = tool;
    if (m_controller) m_controller->setTextToolActive(tool == Tool::Text);
    emit toolChanged();
}

bool QuickCanvasHost::remoteSceneActionEnabled() const
{
    if (m_sceneLaunching || m_sceneStopping) return false;
    if (m_sceneLaunched) return true;
    return m_actionsEnabled && m_contentAvailable && m_webSocket
        && m_webSocket->isConnected() && !m_targetClientId.isEmpty()
        && m_document->hasActiveScreens() && !m_document->media().isEmpty()
        && (!m_uploadManager || !m_uploadManager->isBusy());
}

bool QuickCanvasHost::testSceneActionEnabled() const
{
    return m_testSceneLaunched
        || (m_actionsEnabled && !m_sceneLaunching && !m_sceneStopping
            && !m_sceneLaunched && !m_document->media().isEmpty());
}

QJsonArray QuickCanvasHost::buildSceneManifest(const QJsonObject& scene,
                                               QString* errorMessage) const
{
    static const QRegularExpression sha256(QStringLiteral("^[a-f0-9]{64}$"));
    QMap<QString, QJsonObject> assets;
    QList<MediaFilePolicy::PreparationAsset> preparation;
    QMap<QString, QString> paths;
    for (const QJsonValue& value : scene.value(QStringLiteral("media")).toArray()) {
        const QJsonObject item = value.toObject();
        const QString type = item.value(QStringLiteral("type")).toString();
        if (type == QLatin1String("text")) continue;
        const QString mediaId = item.value(QStringLiteral("mediaId")).toString();
        const QString fileId = item.value(QStringLiteral("fileId")).toString();
        const QString path = m_fileManager ? m_fileManager->getFilePathForId(fileId) : QString();
        const QFileInfo info(path);
        if (mediaId.isEmpty() || !sha256.match(fileId).hasMatch()
            || !info.exists() || !info.isFile() || info.isSymLink()
            || info.size() < 1) {
            if (errorMessage) *errorMessage = QStringLiteral(
                "A scene asset is missing, changed, or was not uploaded with a SHA-256 identity");
            return {};
        }
        const auto expected = type == QLatin1String("video")
            ? MediaFilePolicy::Kind::Mp4Video : MediaFilePolicy::Kind::Image;
        preparation.append({mediaId, path, expected});
        QJsonObject asset = assets.value(fileId);
        QJsonArray ids = asset.value(QStringLiteral("mediaIds")).toArray();
        ids.append(mediaId);
        asset = {{QStringLiteral("assetId"), fileId},
                 {QStringLiteral("fileId"), fileId},
                 {QStringLiteral("sha256"), fileId},
                 {QStringLiteral("size"), static_cast<double>(info.size())},
                 {QStringLiteral("extension"), info.suffix().toLower()},
                 {QStringLiteral("mediaIds"), ids}};
        assets.insert(fileId, asset);
        paths.insert(fileId, path);
    }
    const auto checked = MediaFilePolicy::validatePreparationAssets(preparation);
    if (!checked.accepted) {
        if (errorMessage) *errorMessage = QStringLiteral("Scene asset validation failed (%1)")
            .arg(checked.errorCode);
        return {};
    }
    for (auto it = paths.cbegin(); it != paths.cend(); ++it) {
        if (!MediaFilePolicy::matchesSha256(it.value(), it.key())) {
            if (errorMessage) *errorMessage = QStringLiteral(
                "A source asset changed after upload; upload it again before launching");
            return {};
        }
    }
    QJsonArray manifest;
    for (const QJsonObject& asset : assets) manifest.append(asset);
    return SceneRunCoordinator::normalizeManifest(manifest, errorMessage);
}

QJsonArray QuickCanvasHost::localPreparationChecklist(
    const QJsonObject& scene, bool* ready, QString* errorMessage) const
{
    QJsonArray checklist = SceneRunCoordinator::createLocalChecklist(scene);
    bool allReady = true;
    for (qsizetype i = 0; i < checklist.size(); ++i) {
        QJsonObject entry = checklist.at(i).toObject();
        const QString mediaId = entry.value(QStringLiteral("mediaId")).toString();
        const QString stage = entry.value(QStringLiteral("stage")).toString();
        CanvasMedia* media = m_document->mediaById(mediaId);
        bool itemReady = media != nullptr;
        if (itemReady && stage == QLatin1String("source_validated")
            && !media->isText()) {
            itemReady = QFileInfo::exists(media->sourcePath());
        }
        if (itemReady && stage.startsWith(QLatin1String("video"))) {
            itemReady = media->player() && media->player()->error() == QMediaPlayer::NoError;
        }
        entry.insert(QStringLiteral("ready"), itemReady);
        checklist.replace(i, entry);
        allReady = allReady && itemReady;
    }
    if (ready) *ready = allReady;
    if (!allReady && errorMessage) {
        *errorMessage = QStringLiteral("A local media runtime could not be prepared");
    }
    return checklist;
}

void QuickCanvasHost::triggerRemoteSceneAction()
{
    if (m_sceneLaunched) {
        if (!m_webSocket || m_sceneRunId.isEmpty()) {
            handleRemoteConnectionLost();
            return;
        }
        m_sceneStopping = true;
        m_videoSnapshotTimer.stop();
        m_webSocket->sendSceneStop(m_sceneRunId);
        m_sceneTimeout.start(kStopTimeoutMs);
        publishActionState();
        return;
    }
    if (!remoteSceneActionEnabled()) return;
    QJsonObject scene = m_document->serializeSceneState();
    scene.remove(QStringLiteral("canvasSessionId"));
    QJsonArray media = scene.value(QStringLiteral("media")).toArray();
    for (qsizetype i = 0; i < media.size(); ++i) {
        QJsonObject item = media.at(i).toObject();
        if (item.value(QStringLiteral("type")).toString() != QLatin1String("text")) {
            item.insert(QStringLiteral("assetId"), item.value(QStringLiteral("fileId")));
        }
        media.replace(i, item);
    }
    scene.insert(QStringLiteral("media"), media);
    QString error;
    const QJsonArray manifest = buildSceneManifest(scene, &error);
    if (!error.isEmpty()) {
        sceneToast(NotificationSeverity::Error, error, {}, 5000);
        return;
    }
    m_sceneLaunching = true;
    m_sceneArmed = false;
    m_firstFrameReported = false;
    publishActionState();
    if (!m_webSocket->sendScenePrepare(m_targetClientId, ++m_sceneRevision,
                                       manifest, scene, &m_sceneRunId,
                                       &m_sceneDigest, &error)) {
        failScene(error.isEmpty() ? QStringLiteral("No active remote session") : error,
                  false);
        return;
    }
    bool ready = false;
    QString prepareError;
    const QJsonArray checklist = localPreparationChecklist(scene, &ready, &prepareError);
    m_webSocket->sendScenePrepareProgress(m_sceneRunId, ready ? 100 : 0, checklist);
    m_webSocket->sendScenePrepared(m_sceneRunId, ready, checklist,
        ready ? QString() : QStringLiteral("local_scene_prepare_failed"),
        prepareError);
    if (!ready) {
        failScene(prepareError, true);
        return;
    }
    const int timeout = m_webSocket->serverPolicy()
        .value(QStringLiteral("scenePrepareTimeoutMs")).toInt(15000);
    m_sceneTimeout.start(qMax(1000, timeout));
    sceneToast(NotificationSeverity::Info,
               QStringLiteral("Sending scene to remote client..."),
               m_sceneRunId, 2000);
}

void QuickCanvasHost::triggerTestSceneAction()
{
    if (m_testSceneLaunched) {
        stopScenePresentation();
        m_testSceneLaunched = false;
    } else if (testSceneActionEnabled()) {
        beginScenePresentation(false);
        m_testSceneLaunched = true;
    }
    publishActionState();
}

void QuickCanvasHost::beginScenePresentation(bool remote)
{
    if (m_sceneContext) return;
    m_sceneContext = new QObject(this);
    m_draftState.clear();
    for (CanvasMedia* media : m_document->media()) {
        DraftMediaState draft;
        draft.media = media;
        draft.visible = media->contentVisible();
        if (media->isVideo()) {
            draft.muted = media->muted();
            draft.playing = media->isPlaying();
            draft.positionMs = media->positionMs();
            media->player()->pause();
        }
        m_draftState.append(draft);
        media->setContentVisible(false);
        media->setAnimatedDisplayOpacity(0.0);
        const MediaSettingsState settings = media->settings();
        const int displayDelay = MediaSettingsSerialization::delayMilliseconds(
            settings.displayDelayEnabled, settings.displayDelayText);
        QTimer::singleShot(displayDelay, m_sceneContext, [media, settings]() {
            if (!media) return;
            media->setContentVisible(settings.displayAutomatically);
            media->setAnimatedDisplayOpacity(1.0);
        });
        if (media->isVideo()) {
            media->setMuted(!settings.unmuteAutomatically);
            if (settings.playAutomatically) {
                const int playDelay = MediaSettingsSerialization::delayMilliseconds(
                    settings.playDelayEnabled, settings.playDelayText);
                QTimer::singleShot(playDelay, m_sceneContext, [media]() {
                    if (media && media->player()) media->player()->play();
                });
            }
        }
    }
    m_document->clearSelection();
    m_document->setEditsLocked(true);
    if (remote) emit localScenePresentationRequested(m_sceneRevision);
}

void QuickCanvasHost::stopScenePresentation()
{
    m_videoSnapshotTimer.stop();
    if (m_sceneContext) {
        delete m_sceneContext;
        m_sceneContext = nullptr;
    }
    for (const DraftMediaState& draft : std::as_const(m_draftState)) {
        CanvasMedia* media = draft.media;
        if (!media) continue;
        media->setContentVisible(draft.visible);
        media->setAnimatedDisplayOpacity(1.0);
        if (media->isVideo()) {
            media->player()->pause();
            media->setPositionMs(draft.positionMs);
            media->setMuted(draft.muted);
            if (draft.playing) media->player()->play();
        }
    }
    m_draftState.clear();
    m_document->setEditsLocked(false);
    cancelPresentationBarrier();
}

void QuickCanvasHost::startPresentationBarrier()
{
    cancelPresentationBarrier();
    QQuickWindow* window = m_controller ? m_controller->renderWindow() : nullptr;
    if (!window || !window->isVisible() || !window->isExposed()) return;
    m_framesRemaining = 2;
    m_frameConnection = connect(window, &QQuickWindow::afterFrameEnd, this,
                                [this, window]() {
        if (!window || !window->isVisible() || !window->isExposed()
            || --m_framesRemaining > 0) {
            if (window) window->update();
            return;
        }
        cancelPresentationBarrier();
        if (!m_webSocket || m_sceneRunId.isEmpty()) return;
        const qint64 presented = m_webSocket->estimatedServerMonotonicMs();
        m_firstFrameReported = presented >= 0
            && m_webSocket->sendSceneStarted(m_sceneRunId, true, presented);
        if (!m_firstFrameReported) {
            failScene(QStringLiteral("Could not confirm the first rendered frame"), true);
        }
    }, Qt::QueuedConnection);
    window->update();
}

void QuickCanvasHost::cancelPresentationBarrier()
{
    disconnect(m_frameConnection);
    m_frameConnection = {};
    m_framesRemaining = 0;
}

bool QuickCanvasHost::matchesScene(const QJsonObject& envelope) const
{
    return !m_sceneRunId.isEmpty()
        && envelope.value(QStringLiteral("sceneRunId")).toString() == m_sceneRunId
        && (m_sceneDigest.isEmpty()
            || envelope.value(QStringLiteral("digest")).toString() == m_sceneDigest);
}

void QuickCanvasHost::failScene(const QString& message, bool notifyServer)
{
    const QString runId = m_sceneRunId;
    if (notifyServer && m_webSocket && !runId.isEmpty()) {
        m_webSocket->sendSceneStop(runId, QStringLiteral("client_scene_failure"));
    }
    m_sceneTimeout.stop();
    stopScenePresentation();
    m_sceneLaunching = false;
    m_sceneStopping = false;
    m_sceneLaunched = false;
    m_sceneArmed = false;
    m_sceneRunId.clear();
    m_sceneDigest.clear();
    publishActionState();
    sceneToast(NotificationSeverity::Error,
               QStringLiteral("Scene launch failed: %1").arg(message),
               runId, 5000);
}

void QuickCanvasHost::handleRemoteConnectionLost()
{
    stopScenePresentation();
    m_sceneTimeout.stop();
    m_sceneLaunching = false;
    m_sceneStopping = false;
    m_sceneLaunched = false;
    m_sceneArmed = false;
    m_sceneRunId.clear();
    m_sceneDigest.clear();
    setOverlayActionsEnabled(false);
}

void QuickCanvasHost::stopScenesForSourceInvalidation()
{
    if ((m_sceneLaunching || m_sceneLaunched || m_sceneStopping)
        && m_webSocket && !m_sceneRunId.isEmpty()) {
        m_webSocket->sendSceneStop(m_sceneRunId,
                                   QStringLiteral("source_invalidated"));
    }
    if (m_testSceneLaunched) m_testSceneLaunched = false;
    handleRemoteConnectionLost();
}

void QuickCanvasHost::publishActionState()
{
    emit actionStateChanged();
    emit remoteSceneLaunchStateChanged(m_sceneLaunched,
                                       m_targetClientId,
                                       m_targetMachineName);
    if (m_uploadManager) emit m_uploadManager->uiStateChanged();
}

void QuickCanvasHost::sendVideoSnapshot()
{
    if (!m_sceneLaunched || !m_webSocket || m_sceneRunId.isEmpty()) return;
    QJsonArray videos;
    for (CanvasMedia* media : m_document->media()) {
        if (!media || !media->isVideo()) continue;
        videos.append(QJsonObject{
            {QStringLiteral("mediaId"), media->mediaId()},
            {QStringLiteral("positionMs"), static_cast<double>(media->positionMs())},
            {QStringLiteral("durationMs"), static_cast<double>(
                 media->player() ? media->player()->duration() : 0)},
            {QStringLiteral("playing"), media->isPlaying()},
            {QStringLiteral("muted"), media->muted()},
            {QStringLiteral("visible"), media->contentVisible()},
            {QStringLiteral("repeatAvailable"), media->repeatEnabled()}});
    }
    static quint64 sequence = 0;
    QJsonObject scene = m_document->serializeSceneState();
    scene.remove(QStringLiteral("canvasSessionId"));
    m_webSocket->sendSceneStateSnapshot(m_sceneRunId, ++sequence,
        m_webSocket->estimatedServerMonotonicMs(),
        QJsonObject{{QStringLiteral("scene"), scene},
                    {QStringLiteral("videos"), videos},
                    {QStringLiteral("capturedEpochMs"),
                     static_cast<double>(QDateTime::currentMSecsSinceEpoch())}});
}

QJsonObject QuickCanvasHost::serializeProjectState() const
{
    return m_document->serializeProjectState();
}

bool QuickCanvasHost::restoreProjectState(
    const QJsonObject& state,
    const QHash<QString, QString>& sourcePathByMediaId,
    QStringList* skippedMediaIds)
{
    return m_document->restoreProjectState(state, sourcePathByMediaId,
                                           skippedMediaIds);
}
