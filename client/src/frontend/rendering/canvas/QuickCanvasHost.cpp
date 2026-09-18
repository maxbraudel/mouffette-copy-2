#include "backend/media/MediaResidencyManager.h"
#include "backend/media/ResidentVideoPlayer.h"
#include "backend/media/TimelineVideoPlayback.h"
#include <QUuid>
#include "frontend/rendering/canvas/QuickCanvasHost.h"

#include "backend/config/AppConfig.h"
#include "backend/domain/scene/SceneTimeline.h"
#include "backend/domain/canvas/CanvasDocument.h"
#include "backend/domain/media/CanvasMedia.h"
#include "backend/domain/media/MediaFilePolicy.h"
#include "backend/files/FileManager.h"
#include "backend/network/SceneRunCoordinator.h"
#include "backend/network/UploadManager.h"
#include "backend/network/WebSocketClient.h"
#include "backend/runtime/SuspendInclusiveClock.h"
#include "frontend/rendering/canvas/QuickCanvasController.h"
#include "frontend/ui/notifications/ToastNotificationSystem.h"

#include <QDateTime>
#include <QAudioOutput>
#include <QFileInfo>
#include <QJsonArray>
#include <QMap>
#include <QQuickWindow>
#include <QRegularExpression>
#include <QStringList>

#include <algorithm>

namespace {
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
    connect(&MediaResidencyManager::instance(), &MediaResidencyManager::ownerChanged,
            this, [this](const QString& owner) {
        const auto items = m_document->media();
        if (std::none_of(items.cbegin(), items.cend(),
                [&owner](const CanvasMedia* media) {
                    return media && media->residencyOwnerId() == owner;
                })) return;
        if ((m_sceneLaunching || m_sceneLaunched || m_testSceneLaunched)
            && !MediaResidencyManager::instance().ready(owner)) {
            m_testSceneLaunched = false;
            failScene(QStringLiteral("A scene media is no longer fully resident in memory"), true);
        }
        publishActionState();
    });
    connect(&MediaResidencyManager::instance(), &MediaResidencyManager::sceneStopRequested,
            this, [this](const QString& group) {
        if (group != m_residencyGroup || group.isEmpty()) return;
        m_testSceneLaunched = false;
        failScene(QStringLiteral("Scene stopped to release memory under system pressure"), true);
    });
    Q_ASSERT(m_document);
    Q_ASSERT(m_controller);
    m_document->setParent(this);
    m_controller->setParent(this);
    connect(m_controller, &QuickCanvasController::textToolActiveChanged,
            this, &QuickCanvasHost::toolChanged);
    connect(m_document, &CanvasDocument::pendingImportsChanged, this, [this]() {
        if (m_sceneLaunching && m_document->hasPendingImports())
            failScene(QStringLiteral("Scene preparation was invalidated by a media import still being analyzed"), true);
        publishActionState();
    });
    connect(m_document, &CanvasDocument::mediaSourceInvalidated, this,
            [this](const QString&, const QString&) { stopScenesForSourceInvalidation(); });
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
            m_sceneAccepted = false;
            m_localPreparedReported = false;
            m_sceneAllPrepared = false;
            m_sceneArmed = false;
            m_sceneCommitScheduled = false;
            const QString runId = m_sceneRunId;
            m_sceneRunId.clear();
            m_sceneDigest.clear();
            m_localPrepareChecklist = {};
            publishActionState();
            sceneToast(NotificationSeverity::Warning,
                       QStringLiteral("Remote stop acknowledgement timed out; scene stopped locally"),
                       runId, AppConfig::instance().toastWarningDurationMs());
        } else if (m_sceneLaunching) {
            const QString message = !m_sceneAccepted
                ? QStringLiteral("The server did not accept the remote scene request in time")
                : (!m_sceneAllPrepared
                    ? QStringLiteral("The remote client did not finish preparing the scene in time")
                    : (!m_sceneArmed
                        ? QStringLiteral("The clients' clocks did not synchronize within the scene preparation deadline")
                        : (!m_sceneCommitScheduled
                            ? QStringLiteral("The remote clients did not complete launch synchronization in time")
                            : QStringLiteral("The remote scene did not present its first frame in time"))));
            // An unaccepted request has no authoritative server-side SceneRun.
            // Let the server's own preparation deadline discard a lost ACK
            // instead of generating an "unknown scene run" error.
            failScene(message, m_sceneAccepted);
        }
    });
    m_timelineTimer.setTimerType(Qt::PreciseTimer);
    m_timelineTimer.setInterval(16);
    connect(&m_timelineTimer, &QTimer::timeout, this, &QuickCanvasHost::advanceTimeline);
    m_videoSnapshotTimer.setInterval(
        AppConfig::instance().videoSnapshotIntervalMs());
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

void QuickCanvasHost::setActiveProjectId(const QString& id)
{
    m_document->setClientWorkspaceId(id);
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
    connect(m_webSocket, &WebSocketClient::scenePrepareProgressReceived, this,
            [this](const QJsonObject& envelope) {
        if (!matchesScene(envelope) || !m_sceneLaunching || m_sceneAccepted
            || !envelope.value(QStringLiteral("aggregate")).toBool(false)
            || envelope.value(QStringLiteral("stage")).toString()
                != QLatin1String("accepted")) {
            return;
        }
        m_sceneAccepted = true;
        reportLocalScenePrepared();
    });
    connect(m_webSocket, &WebSocketClient::scenePreparedReceived, this,
            [this](const QJsonObject& envelope) {
        if (!matchesScene(envelope) || !m_sceneLaunching
            || !m_localPreparedReported
            || !envelope.value(QStringLiteral("allPrepared")).toBool(false)) return;
        m_sceneAllPrepared = true;
        tryArmRemoteScene();
    });
    connect(m_webSocket, &WebSocketClient::heartbeatSampleReceived, this,
            [this](quint64, qint64, qint64, qint64) {
        // PREPARED is an asynchronous barrier, not a one-shot clock test. A
        // sample can be missing or temporarily outside policy when the second
        // endpoint finishes decoding. Keep the immutable run prepared and arm
        // it as soon as the bounded synchronization burst yields a good sample.
        tryArmRemoteScene();
    });
    connect(m_webSocket, &WebSocketClient::sceneCommitReceived, this,
            [this](const QJsonObject& envelope) {
        if (!matchesScene(envelope) || !m_sceneLaunching || !m_sceneArmed
            || m_sceneCommitScheduled) return;
        const qint64 start = envelope.value(QStringLiteral("startServerMonotonicMs"))
                                 .toInteger(-1);
        const qint64 now = m_webSocket->estimatedServerMonotonicMs();
        const QJsonObject policy = m_webSocket->serverPolicy();
        const qint64 policyActivationLead = policy
            .value(QStringLiteral("sceneActivationLeadMs")).toInteger(-1);
        const qint64 activationLead = envelope
            .value(QStringLiteral("activationLeadMs")).toInteger(-1);
        const qint64 maximumClockSkew = policy
            .value(QStringLiteral("sceneMaxClockSkewMs")).toInteger(-1);
        const qint64 startedTimeout = policy
            .value(QStringLiteral("sceneStartedAckTimeoutMs")).toInteger(-1);
        const qint64 delay = start - now;
        if (start < 0 || now < 0 || policyActivationLead < 1
            || activationLead < 1 || activationLead > policyActivationLead
            || maximumClockSkew < 0 || startedTimeout < 1
            || delay < -maximumClockSkew
            || delay > activationLead + maximumClockSkew) {
            failScene(QStringLiteral("Invalid synchronized launch commitment"), true);
            return;
        }
        m_sceneCommitScheduled = true;
        m_remoteStartServerMs = start;
        const QString scheduledRunId = m_sceneRunId;
        const QString scheduledDigest = m_sceneDigest;
        const qint64 boundedDelay = std::max<qint64>(0, delay);
        // Preparation may consume almost its full deadline. Once COMMIT is
        // authoritative, replace it with a deadline scoped to presentation.
        m_sceneTimeout.start(int(boundedDelay + startedTimeout
            + AppConfig::instance().sceneLaunchTimeoutMarginMs()));
        QTimer::singleShot(int(boundedDelay), this,
                           [this, scheduledRunId, scheduledDigest]() {
            if (!m_sceneLaunching || !m_sceneCommitScheduled
                || m_sceneRunId != scheduledRunId
                || m_sceneDigest != scheduledDigest) return;
            const QString reason = mediaReadinessReason(true);
            if (!reason.isEmpty()) { failScene(reason, true); return; }
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
        m_sceneCommitScheduled = false;
        m_localPrepareChecklist = {};
        m_videoSnapshotTimer.start();
        publishActionState();
        sceneToast(NotificationSeverity::Success,
                   QStringLiteral("Remote scene launched successfully!"),
                   m_sceneRunId);
    });
    connect(m_webSocket, &WebSocketClient::sceneStopReceived, this,
            [this](const QJsonObject& envelope) {
        if (!matchesScene(envelope)) return;
        stopScenePresentation();
        m_sceneLaunching = false;
        m_sceneLaunched = false;
        m_sceneStopping = true;
        m_webSocket->sendSceneStopped(m_sceneRunId, true);
        m_sceneTimeout.start(m_webSocket->serverPolicy()
            .value(QStringLiteral("sceneStopTimeoutMs")).toInt());
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
        m_sceneAccepted = false;
        m_localPreparedReported = false;
        m_sceneAllPrepared = false;
        m_sceneArmed = false;
        m_sceneCommitScheduled = false;
        m_sceneRunId.clear();
        m_sceneDigest.clear();
        m_localPrepareChecklist = {};
        publishActionState();
        sceneToast(NotificationSeverity::Success,
                   QStringLiteral("Remote scene stopped successfully"),
                   runId);
    });
    connect(m_webSocket, &WebSocketClient::sceneErrorReceived, this,
            [this](const QJsonObject& envelope) {
        if (matchesScene(envelope)) {
            const QString code =
                envelope.value(QStringLiteral("code")).toString();
            const QString message = code
                    == QLatin1String("target_scene_already_running")
                ? QStringLiteral(
                    "A scene is already running on this client. Try again shortly.")
                : envelope.value(QStringLiteral("message")).toString(
                    QStringLiteral("Remote scene protocol error"));
            failScene(message, false);
        }
    });
    connect(m_webSocket, &WebSocketClient::remoteSessionResumed, this,
            [this](const QJsonObject& envelope) {
        if (!m_webSocket || m_sceneRunId.isEmpty()) return;
        SceneRunCoordinator* coordinator = m_webSocket->sceneRunCoordinator();
        const SceneRunCoordinator::Run run = coordinator
            ? coordinator->run(m_sceneRunId) : SceneRunCoordinator::Run();
        if (run.remoteSessionId.isEmpty()
            || envelope.value(QStringLiteral("remoteSessionId")).toString()
                != run.remoteSessionId) return;
        if (m_sceneLaunching && m_sceneAllPrepared && !m_sceneCommitScheduled) {
            // sendTextMessage() only confirms local queueing. If the former
            // transport disappeared after ARMED was queued, replay it on the
            // rebound generation once the new clock mapping is usable.
            m_sceneArmed = false;
            m_webSocket->requestSceneClockSynchronization();
            tryArmRemoteScene();
        }
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
            m_targetMachineName = client.getInstanceDisplayName();
            break;
        }
    }
}

void QuickCanvasHost::setScreens(const QList<ScreenInfo>& screens)
{
    m_document->setScreens(screens);
    // A topology replacement never owns the camera. Initial fitting is an
    // explicit workspace transition; subsequent screen changes must preserve
    // the project's absolute coordinates and viewport exactly.
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

void QuickCanvasHost::recenterWithMargin(int marginPx)
{
    if (m_controller) m_controller->recenterView(marginPx);
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

void QuickCanvasHost::updateRemoteCursor(int screenId, const QPointF& screenPosition)
{
    if (m_controller) m_controller->updateRemoteCursor(screenId, screenPosition);
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

void QuickCanvasHost::setProjectEditingEnabled(bool enabled)
{
    if (m_projectEditingEnabled == enabled) return;
    m_projectEditingEnabled = enabled;
    if (m_controller) m_controller->setProjectEditingEnabled(enabled);
    publishActionState();
}

ICanvasHost::Tool QuickCanvasHost::currentTool() const
{
    // Text creation returns to selection inside the controller. Read that same
    // state so the toolbar and later tool requests cannot retain a stale tool.
    return m_controller && m_controller->textToolActive()
        ? Tool::Text : Tool::Selection;
}

void QuickCanvasHost::setCurrentTool(Tool tool)
{
    if (tool == Tool::Text && !m_projectEditingEnabled) return;
    if (currentTool() == tool || m_document->editsLocked()) return;
    if (m_controller) m_controller->setTextToolActive(tool == Tool::Text);
}

bool QuickCanvasHost::remoteSceneActionEnabled() const
{
    if (m_sceneLaunching || m_sceneStopping) return false;
    if (m_sceneLaunched) return true;
    if (m_testSceneLaunched || m_sceneContext) return false;
    return m_projectEditingEnabled && m_actionsEnabled && m_contentAvailable && m_webSocket
        && m_webSocket->isConnected() && !m_targetClientId.isEmpty()
        && m_document->hasActiveScreens() && !m_document->media().isEmpty()
        && (!m_uploadManager || !m_uploadManager->isBusy())
        && mediaReadinessReason(true).isEmpty();
}

bool QuickCanvasHost::testSceneActionEnabled() const
{
    if (m_sceneLaunching || m_sceneStopping || m_sceneLaunched) return false;
    if (m_testSceneLaunched) return true;
    // The editor transport also runs on an empty timeline.
    return m_projectEditingEnabled && !m_sceneContext && mediaReadinessReason(false).isEmpty();
}

QStringList QuickCanvasHost::residencyOwners() const
{
    QStringList owners;
    for (const auto* media : m_document->media())
        if (media && !media->isText()) owners.append(media->residencyOwnerId());
    return owners;
}

bool QuickCanvasHost::remoteMediaCached(const QString& mediaId) const
{
    const auto* media = m_document->mediaById(mediaId);
    return media && !media->isText() && m_uploadManager
        && media->uploadState() == CanvasMedia::UploadState::Uploaded
        && m_uploadManager->remoteMediaReady(m_targetClientId, media->fileId());
}

QString QuickCanvasHost::mediaReadinessReason(bool remote) const
{
    if (m_document->hasPendingImports())
        return QStringLiteral("Wait until every imported media has been analyzed, validated and loaded into memory");
    const auto& manager = MediaResidencyManager::instance();
    for (const auto* media : m_document->media()) {
        if (!media || media->isText()) continue;
        if (!manager.ready(media->residencyOwnerId()))
            return QStringLiteral("Wait until every media is validated and resident in memory (%1)").arg(media->displayName());
        if (media->isVideo() && media->player()) {
            const qint64 duration = media->player()->duration();
            if (!SceneTimeline::validateMediaTrack(media->timelineTrack(), media->typeName(), duration))
                return QStringLiteral("Invalid clips (%1)").arg(media->displayName());
        }
        if (remote && (!m_uploadManager || !m_uploadManager->remoteMediaReady(
                m_targetClientId, manager.sha256(media->residencyOwnerId()))))
            return QStringLiteral("Wait until every media is validated and resident on the remote computer (%1)").arg(media->displayName());
    }
    return {};
}

QJsonArray QuickCanvasHost::buildSceneManifest(const QJsonObject& scene,
                                               QString* errorMessage) const
{
    static const QRegularExpression sha256(QStringLiteral("^[a-f0-9]{64}$"));
    QMap<QString, QJsonObject> assets;
    QMap<QString, QString> missingUploads;
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
        if (!m_fileManager->isFileUploadedToClient(fileId, m_targetClientId)) {
            QString fileName = item.value(QStringLiteral("fileName")).toString().trimmed();
            if (fileName.isEmpty()) fileName = mediaId;
            missingUploads.insert(fileId, fileName.left(120));
        }
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
    }
    if (!missingUploads.isEmpty()) {
        QStringList names;
        for (auto it = missingUploads.cbegin(); it != missingUploads.cend(); ++it) {
            if (names.size() == 3) break;
            names.append(QStringLiteral("\"%1\"").arg(it.value()));
        }
        if (missingUploads.size() > names.size()) {
            names.append(QStringLiteral("and %1 more")
                             .arg(missingUploads.size() - names.size()));
        }
        const QString target = m_targetMachineName.trimmed().isEmpty()
            ? QStringLiteral("the remote client")
            : QStringLiteral("remote client \"%1\"").arg(m_targetMachineName.left(120));
        if (errorMessage) {
            *errorMessage = missingUploads.size() == 1
                ? QStringLiteral(
                    "Cannot launch the remote scene: %1 has not been uploaded to %2. "
                    "Upload it and try again.")
                      .arg(names.constFirst(), target)
                : QStringLiteral(
                    "Cannot launch the remote scene: %1 media files have not been uploaded "
                    "to %2 (%3). Upload them and try again.")
                      .arg(missingUploads.size()).arg(target, names.join(QStringLiteral(", ")));
        }
        return {};
    }
    QJsonArray manifest;
    for (const QJsonObject& asset : assets) manifest.append(asset);
    return SceneRunCoordinator::normalizeManifest(manifest, errorMessage);
}

QJsonArray QuickCanvasHost::localPreparationChecklist(
    const QJsonObject& scene, bool* ready, QString* errorMessage) const
{
    QHash<QString, QString> mediaIdsByItemId;
    QJsonArray checklist = SceneRunCoordinator::createLocalChecklist(scene, &mediaIdsByItemId);
    bool allReady = !m_document->hasPendingImports();
    QString firstError = allReady ? QString()
        : QStringLiteral("Media imports are still being analyzed");
    for (qsizetype i = 0; i < checklist.size(); ++i) {
        QJsonObject entry = checklist.at(i).toObject();
        const QString itemId = entry.value(QStringLiteral("itemId")).toString();
        const QString stage = entry.value(QStringLiteral("stage")).toString();
        QString failure;
        if (stage == QLatin1String("screen_render_graph_ready")) {
            if (!m_controller || !m_controller->renderWindow()) {
                failure = QStringLiteral("The local canvas render window is not ready");
            }
        } else {
            const QString mediaId = mediaIdsByItemId.value(itemId);
            CanvasMedia* media = m_document->mediaById(mediaId);
            if (!media) {
                failure = QStringLiteral("Scene media %1 is no longer available").arg(mediaId);
            } else if (!media->isText() && !MediaResidencyManager::instance().ready(media->residencyOwnerId())) {
                failure = QStringLiteral("Media %1 is not validated and resident in memory").arg(media->displayName());
            } else if (stage == QLatin1String("file_validated") && !media->isText()) {
                const QFileInfo source(media->sourcePath());
                if (!source.exists() || !source.isFile() || source.isSymLink()
                    || source.size() < 1) {
                    failure = QStringLiteral("The local source for \"%1\" is missing or invalid")
                        .arg(media->displayName());
                }
            } else if (media->isVideo()) {
                if (!media->player()) {
                    failure = QStringLiteral("The video player for \"%1\" is not initialized")
                        .arg(media->displayName());
                }
            }
        }
        const bool itemReady = failure.isEmpty();
        entry.insert(QStringLiteral("ready"), itemReady);
        checklist.replace(i, entry);
        allReady = allReady && itemReady;
        if (!itemReady) {
            if (firstError.isEmpty()) firstError = failure;
            qWarning() << "Local scene preparation failed:" << itemId << stage << failure;
        }
    }
    if (ready) *ready = allReady;
    if (errorMessage) *errorMessage = firstError;
    return checklist;
}

void QuickCanvasHost::prepareSceneVideos(std::function<void()> ready)
{
    if (m_videoPreparation) return;
    m_document->setEditsLocked(true);
    const QPointer<QObject> context = new QObject(this);
    m_videoPreparation = context;
    auto finish = [this, context, ready]() {
        if (!context || m_videoPreparation != context) return;
        for (CanvasMedia* media : m_document->media()) {
            if (!media->isText() && !MediaResidencyManager::instance().ready(media->residencyOwnerId())) {
                failScene(QStringLiteral("Media memory availability changed during preparation"), m_sceneAccepted);
                return;
            }
            if (media->isVideo() && (!media->player()
                || !media->player()->preparedAt(SceneTimeline::evaluateVideo(
                    media->timelineTrack(), timelinePositionMs(), media->player()->duration(), m_document->timelineSettings()).sourceTimeMs))) return;
        }
        m_videoPreparation = nullptr;
        context->deleteLater();
        m_localVideosPrepared = true;
        ready();
    };
    for (CanvasMedia* media : m_document->media()) {
        if (!media->isVideo() || !media->player()) continue;
        connect(media->player(), &ResidentVideoPlayer::frameReady, context, finish);
        connect(media->player(), &ResidentVideoPlayer::errorOccurred, context,
                [this, context](QMediaPlayer::Error error, const QString& message) {
            if (context && m_videoPreparation == context && error != QMediaPlayer::NoError)
                failScene(message, m_sceneAccepted);
        }, Qt::QueuedConnection);
    }
    const int timeout = m_webSocket
        ? m_webSocket->serverPolicy().value(QStringLiteral("scenePrepareTimeoutMs")).toInt(5000) : 5000;
    QTimer::singleShot(timeout, context, [this, context]() {
        if (context && m_videoPreparation == context)
            failScene(QStringLiteral("Video start frames did not finish preparing in time"), m_sceneAccepted);
    });
    for (CanvasMedia* media : m_document->media()) {
        if (!context || m_videoPreparation != context) return;
        if (media->isVideo() && media->player()) {
            media->player()->pause();
            // Replace any preview seek queued while this player's source was
            // loading. Its LoadedMedia handler must not restore the draft
            // cursor over the scene's prepared start frame.
            const qint64 sourceTime = SceneTimeline::evaluateVideo(
                media->timelineTrack(), timelinePositionMs(), media->player()->duration(), m_document->timelineSettings()).sourceTimeMs;
            media->setPositionMs(sourceTime);
            media->player()->prepare(sourceTime);
        }
    }
    finish();
}

void QuickCanvasHost::reportLocalScenePrepared()
{
    if (!m_webSocket || !m_sceneLaunching || !m_sceneAccepted
        || m_localPreparedReported || m_localPrepareChecklist.isEmpty()) {
        return;
    }
    if (!m_localVideosPrepared) {
        prepareSceneVideos([this] { reportLocalScenePrepared(); });
        return;
    }
    m_localPreparedReported = true;
    const auto readyCount = std::count_if(
        m_localPrepareChecklist.cbegin(), m_localPrepareChecklist.cend(),
        [](const QJsonValue& value) {
            return value.toObject().value(QStringLiteral("ready")).toBool();
        });
    const int percent = int(readyCount * 100 / m_localPrepareChecklist.size());
    if (!m_webSocket->sendScenePrepareProgress(
            m_sceneRunId, percent, m_localPrepareChecklist)
        || !m_webSocket->sendScenePrepared(
            m_sceneRunId, true, m_localPrepareChecklist)) {
        failScene(QStringLiteral("Could not acknowledge local scene preparation"), true);
    }
}

void QuickCanvasHost::tryArmRemoteScene()
{
    if (!m_webSocket || !m_sceneLaunching || !m_sceneAllPrepared
        || !m_localPreparedReported || m_sceneArmed
        || m_sceneCommitScheduled || m_sceneRunId.isEmpty()) {
        return;
    }

    const QString readinessError = mediaReadinessReason(true);
    if (!readinessError.isEmpty()) {
        failScene(readinessError, true);
        return;
    }
    const qint64 allowed = m_webSocket->serverPolicy()
        .value(QStringLiteral("sceneMaxClockSkewMs")).toInteger(-1);
    const qint64 uncertainty = m_webSocket->sceneClockUncertaintyMs();
    if (allowed < 0 || uncertainty < 0 || uncertainty > allowed) {
        m_webSocket->requestSceneClockSynchronization();
        return;
    }

    m_sceneArmed = m_webSocket->sendSceneArmed(m_sceneRunId, uncertainty);
    if (!m_sceneArmed) {
        // A transport can be replaced between the quality check and the send.
        // The resumed-session and heartbeat paths retry the idempotent ARMED
        // command while the server's preparation deadline remains authoritative.
        m_webSocket->requestSceneClockSynchronization();
    }
}

void QuickCanvasHost::triggerRemoteSceneAction()
{
    if (m_sceneLaunched) {
        if (!m_webSocket || m_sceneRunId.isEmpty()) {
            handleRemoteConnectionLost();
            return;
        }
        const qreal stoppedAt = std::min(timelineNowMs(), timelineStopMs());
        m_timelinePlaying = false;
        m_timelineTimer.stop();
        applyTimeline(stoppedAt, false, true);
        cancelPresentationBarrier();
        m_sceneStopping = true;
        m_videoSnapshotTimer.stop();
        m_webSocket->sendSceneStop(m_sceneRunId);
        m_sceneTimeout.start(m_webSocket->serverPolicy()
            .value(QStringLiteral("sceneStopTimeoutMs")).toInt());
        publishActionState();
        return;
    }
    if (!remoteSceneActionEnabled()) return;
    timelineSeek(0);
    QJsonObject scene = m_document->serializeSceneState();
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
        sceneToast(NotificationSeverity::Error, error, {},
                   AppConfig::instance().toastErrorDurationMs());
        return;
    }
    bool ready = false;
    QString prepareError;
    const QJsonArray checklist = localPreparationChecklist(scene, &ready, &prepareError);
    if (!ready) {
        sceneToast(NotificationSeverity::Error,
                   prepareError.isEmpty()
                       ? QStringLiteral("The local scene is not ready")
                       : prepareError,
                   {}, AppConfig::instance().toastErrorDurationMs());
        return;
    }
    m_residencyGroup = QStringLiteral("canvas-scene:%1").arg(QUuid::createUuid().toString(QUuid::WithoutBraces));
    if (!MediaResidencyManager::instance().pinOwners(residencyOwners(), m_residencyGroup)) {
        m_residencyGroup.clear();
        sceneToast(NotificationSeverity::Error, QStringLiteral("Scene media changed or insufficient RAM remains for playback buffers; try again"));
        return;
    }
    m_runningSceneDefinition = scene;
    m_sceneLaunching = true;
    m_sceneAccepted = false;
    m_localPreparedReported = false;
    m_sceneAllPrepared = false;
    m_sceneArmed = false;
    m_sceneCommitScheduled = false;
    m_firstFrameReported = false;
    m_localPrepareChecklist = checklist;
    m_sceneRunId.clear();
    m_sceneDigest.clear();
    // The transmitted scene is immutable. Lock the draft immediately so the
    // owner cannot diverge from the target while either side is preparing.
    m_document->setEditsLocked(true);
    publishActionState();
    // Overlap the bounded clock-probe burst with renderer preparation so
    // the PREPARED barrier normally has a fresh sample ready immediately.
    m_webSocket->requestSceneClockSynchronization();
    if (!m_webSocket->sendScenePrepare(m_targetClientId, ++m_sceneRevision,
                                       manifest, scene, &m_sceneRunId,
                                       &m_sceneDigest, &error)) {
        failScene(error.isEmpty() ? QStringLiteral("No active remote session") : error,
                  false);
        return;
    }
    const int timeout = m_webSocket->serverPolicy()
        .value(QStringLiteral("scenePrepareTimeoutMs")).toInt();
    m_sceneTimeout.start(timeout);
    sceneToast(NotificationSeverity::Info,
               QStringLiteral("Sending scene to remote client..."),
               m_sceneRunId, AppConfig::instance().toastInfoDurationMs());
}

void QuickCanvasHost::triggerTestSceneAction()
{
    if (m_testSceneLaunched) timelinePause();
    else timelinePlay();
}

qreal QuickCanvasHost::timelinePositionMs() const
{
    return m_document->timelinePositionMs();
}

qreal QuickCanvasHost::timelineStopMs() const
{
    SceneTimeline::SceneSettings settings;
    SceneTimeline::SceneSettings::fromJson(
        (m_runningSceneDefinition.isEmpty() ? m_document->serializeSceneState()
                                           : m_runningSceneDefinition)
            .value(QStringLiteral("timeline")).toObject(), &settings);
    return settings.effectiveStopMs();
}

qreal QuickCanvasHost::timelineNowMs() const
{
    if (!m_timelinePlaying) return timelinePositionMs();
    if (m_timelineRemote && m_webSocket && m_remoteStartServerMs >= 0) {
        const qint64 now = m_webSocket->estimatedServerMonotonicMs();
        if (now >= 0) return std::max<qint64>(0, now - m_remoteStartServerMs);
    }
    return m_timelineAnchorPositionMs + (m_timelineClock.isValid() ? m_timelineClock.elapsed() : 0);
}

void QuickCanvasHost::timelineSeek(qreal positionMs)
{
    if (m_sceneLaunching || m_sceneLaunched || m_sceneStopping || m_timelinePlaying) return;
    const auto& grid = m_document->timelineSettings();
    applyTimeline(grid.timeMs(grid.nearestSlot(positionMs)), false, true);
}

void QuickCanvasHost::timelinePlay()
{
    if (m_sceneLaunching || m_sceneLaunched || m_sceneStopping || m_testSceneLaunched
        || !testSceneActionEnabled()) return;
    if (timelinePositionMs() >= timelineStopMs()) timelineSeek(0);
    else timelineSeek(timelinePositionMs()); // Discard unrecorded property edits.
    m_residencyGroup = QStringLiteral("canvas-preview:%1").arg(QUuid::createUuid().toString(QUuid::WithoutBraces));
    if (!MediaResidencyManager::instance().pinOwners(residencyOwners(), m_residencyGroup)) {
        m_residencyGroup.clear();
        sceneToast(NotificationSeverity::Error, QStringLiteral("Insufficient RAM remains for scene playback buffers"));
        return;
    }
    m_testSceneLaunched = true;
    m_runningSceneDefinition = m_document->serializeSceneState();
    prepareSceneVideos([this] {
        if (m_testSceneLaunched) beginScenePresentation(false);
        publishActionState();
    });
    publishActionState();
    emit timelineTransportChanged();
}

void QuickCanvasHost::timelinePause()
{
    if (!m_testSceneLaunched || m_timelineRemote) return;
    stopScenePresentation();
    const auto& grid = m_document->timelineSettings();
    applyTimeline(grid.timeMs(grid.slotAt(timelinePositionMs())), false, true);
    m_testSceneLaunched = false;
    publishActionState();
}

void QuickCanvasHost::applyTimeline(qreal positionMs, bool playing, bool forceSeek)
{
    m_document->setTimelinePosition(positionMs);
    const qreal time = timelinePositionMs();
    const qint64 clock = MouffetteClock::nowMs();
    for (CanvasMedia* media : m_document->media()) {
        if (!media->isVideo() || !media->player()) continue;
        auto* player = media->player();
        const auto sample = SceneTimeline::evaluateVideo(media->timelineTrack(), time, player->duration(), m_document->timelineSettings());
        if (!sample.clipActive) {
            player->pause();
            if (auto* audio = player->audioOutput()) audio->setMuted(true);
            m_timelineClipIds.remove(media->mediaId());
            m_timelineVideoPlaying.insert(media->mediaId(), false);
            continue;
        }
        const bool shouldPlay = playing && sample.playing;
        const QString previousClip = m_timelineClipIds.value(media->mediaId());
        const bool changedClip = previousClip != sample.clipId;
        const bool discontinuity = changedClip
            && !contiguousTimelineClips(media->timelineTrack(), previousClip, sample.clipId);
        const bool transition = shouldPlay != m_timelineVideoPlaying.value(media->mediaId(), false);
        if (!shouldPlay) player->pause();
        const qint64 error = qAbs(player->position() - sample.sourceTimeMs);
        const bool drift = shouldPlay && error > AppConfig::instance().sceneVideoSyncPositionToleranceMs()
            && clock >= m_timelineSeekGuards.value(media->mediaId(), 0);
        const bool heldFrameMissing = !shouldPlay && error > 0
            && !player->preparedAt(sample.sourceTimeMs)
            && clock >= m_timelineSeekGuards.value(media->mediaId(), 0);
        if (forceSeek || discontinuity || transition || drift || heldFrameMissing) {
            player->setPosition(sample.sourceTimeMs);
            m_timelineSeekGuards.insert(media->mediaId(), clock + AppConfig::instance().sceneAuthoritativeSeekGuardMs());
        }
        m_timelineClipIds.insert(media->mediaId(), sample.clipId);
        m_timelineVideoPlaying.insert(media->mediaId(), shouldPlay);
        if (shouldPlay && !player->isPlaying() && (forceSeek || transition || discontinuity || drift))
            player->play();
        if (auto* audio = player->audioOutput()) {
            audio->setVolume(media->volume());
            audio->setMuted(!shouldPlay || media->muted());
        }
    }
    emit timelineTransportChanged();
}

void QuickCanvasHost::advanceTimeline()
{
    if (!m_timelinePlaying) return;
    const qreal stop = timelineStopMs();
    const qreal time = std::min(std::max(timelineNowMs(), timelinePositionMs()), stop);
    applyTimeline(time, time < stop);
    if (time < stop) return;
    m_timelinePlaying = false;
    m_timelineTimer.stop();
    if (m_timelineRemote && m_webSocket && !m_sceneRunId.isEmpty()) {
        m_sceneStopping = true;
        cancelPresentationBarrier();
        m_videoSnapshotTimer.stop();
        m_webSocket->sendSceneStop(m_sceneRunId, QStringLiteral("timeline_finished"));
        m_sceneTimeout.start(m_webSocket->serverPolicy()
            .value(QStringLiteral("sceneStopTimeoutMs")).toInt());
    } else {
        stopScenePresentation();
        m_testSceneLaunched = false;
    }
    publishActionState();
    emit timelineTransportChanged();
}

void QuickCanvasHost::beginScenePresentation(bool remote)
{
    if (m_sceneContext) return;
    m_document->setEditsLocked(true);
    m_sceneContext = new QObject(this);
    for (CanvasMedia* media : m_document->media()) {
        if (media->isVideo() && media->player()) {
            const QPointer<QObject> context(m_sceneContext);
            connect(media->player(), &ResidentVideoPlayer::errorOccurred, m_sceneContext,
                    [this, context](QMediaPlayer::Error error, const QString& message) {
                if (context && context == m_sceneContext && error != QMediaPlayer::NoError)
                    failScene(message, m_sceneAccepted);
            }, Qt::QueuedConnection);
        }
    }
    m_timelineRemote = remote;
    m_timelineAnchorPositionMs = remote ? 0 : timelinePositionMs();
    if (remote && m_webSocket && m_remoteStartServerMs >= 0) {
        const qint64 now = m_webSocket->estimatedServerMonotonicMs();
        if (now >= 0) m_timelineAnchorPositionMs = std::max<qint64>(0, now - m_remoteStartServerMs);
    }
    m_timelineClock.start();
    m_timelinePlaying = true;
    const qreal now = timelineNowMs();
    applyTimeline(std::min(now, timelineStopMs()), now < timelineStopMs(), true);
    m_timelineTimer.start();
    if (now >= timelineStopMs()) advanceTimeline();
    if (remote && m_timelinePlaying) emit localScenePresentationRequested(m_sceneRevision);
    emit timelineTransportChanged();
}

void QuickCanvasHost::stopScenePresentation()
{
    const qreal stoppedAt = std::min(timelineNowMs(), timelineStopMs());
    m_timelinePlaying = false;
    m_timelineTimer.stop();
    if (m_videoPreparation) delete m_videoPreparation.data();
    m_videoPreparation = nullptr;
    m_localVideosPrepared = false;
    m_videoSnapshotTimer.stop();
    if (m_sceneContext) {
        delete m_sceneContext;
        m_sceneContext = nullptr;
    }
    applyTimeline(stoppedAt, false, true);
    m_timelineRemote = false;
    m_remoteStartServerMs = -1;
    m_timelineClipIds.clear();
    m_timelineVideoPlaying.clear();
    m_timelineSeekGuards.clear();
    MediaResidencyManager::instance().unpinGroup(m_residencyGroup);
    m_residencyGroup.clear();
    m_document->setEditsLocked(false);
    cancelPresentationBarrier();
    m_runningSceneDefinition = {};
    emit timelineTransportChanged();
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
    m_testSceneLaunched = false;
    const QString runId = m_sceneRunId;
    if (notifyServer && m_webSocket && !runId.isEmpty()) {
        m_webSocket->sendSceneStop(runId, QStringLiteral("client_scene_failure"));
    }
    m_sceneTimeout.stop();
    stopScenePresentation();
    m_sceneLaunching = false;
    m_sceneStopping = false;
    m_sceneLaunched = false;
    m_sceneAccepted = false;
    m_localPreparedReported = false;
    m_sceneAllPrepared = false;
    m_sceneArmed = false;
    m_sceneCommitScheduled = false;
    m_sceneRunId.clear();
    m_sceneDigest.clear();
    m_localPrepareChecklist = {};
    publishActionState();
    sceneToast(NotificationSeverity::Error,
               QStringLiteral("Scene launch failed: %1").arg(message),
               runId, AppConfig::instance().toastErrorDurationMs());
}

void QuickCanvasHost::handleRemoteConnectionLost()
{
    // The presentation/priming context can belong to a wholly local test.
    // Network cleanup must preserve its timeline, draft state and RAM pins.
    if (!m_testSceneLaunched) stopScenePresentation();
    m_sceneTimeout.stop();
    m_sceneLaunching = false;
    m_sceneStopping = false;
    m_sceneLaunched = false;
    m_sceneAccepted = false;
    m_localPreparedReported = false;
    m_sceneAllPrepared = false;
    m_sceneArmed = false;
    m_sceneCommitScheduled = false;
    m_sceneRunId.clear();
    m_sceneDigest.clear();
    m_localPrepareChecklist = {};
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
}

void QuickCanvasHost::sendVideoSnapshot()
{
    if (!m_sceneLaunched || !m_webSocket || m_sceneRunId.isEmpty()) return;
    static quint64 sequence = 0;
    m_webSocket->sendSceneStateSnapshot(m_sceneRunId, ++sequence,
        m_webSocket->estimatedServerMonotonicMs(),
        QJsonObject{{QStringLiteral("timelinePositionMs"), static_cast<double>(timelinePositionMs())}});
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
