#include "frontend/qml/ClientWorkspaceViewModel.h"

#include "frontend/rendering/canvas/MediaListModel.h"
#include "frontend/rendering/canvas/QuickCanvasController.h"
#include "frontend/rendering/canvas/QuickCanvasHost.h"
#include "frontend/qml/MediaSettingsViewModel.h"
#include "frontend/qml/TimelineController.h"
#include "frontend/ui/notifications/ToastNotificationSystem.h"
#include "backend/network/UploadManager.h"
#include "backend/domain/media/CanvasMedia.h"
#include "backend/media/MediaResidencyManager.h"
#include "shared/rendering/ICanvasHost.h"

#include <QAbstractItemModel>
#include <QFileInfo>
#include <algorithm>
#include <QTimer>

ClientWorkspaceViewModel::ClientWorkspaceViewModel(QString workspaceEndpointId,
                                               ICanvasHost* canvas,
                                               std::function<void()> uploadAction,
                                               UploadManager* uploadManager,
                                               std::function<bool()> remoteFilesPresent,
                                               std::function<bool()> hasUnuploadedFiles,
                                               std::function<bool()> hasProject,
                                               QObject* parent)
    : QObject(parent)
    , m_workspaceEndpointId(std::move(workspaceEndpointId))
    , m_uploadAction(std::move(uploadAction))
    , m_uploadManager(uploadManager)
    , m_remoteFilesPresent(std::move(remoteFilesPresent))
    , m_hasUnuploadedFiles(std::move(hasUnuploadedFiles))
    , m_hasProject(std::move(hasProject))
    , m_mediaSettings(new MediaSettingsViewModel(this))
    , m_timeline(new TimelineController(this))
    , m_overlayMediaModel(new MediaListModel(this))
{
    connect(this, &ClientWorkspaceViewModel::actionStateChanged,
            this, &ClientWorkspaceViewModel::refreshSources);
    if (m_uploadManager) {
        connect(m_uploadManager, &UploadManager::fileUploadProgress, this,
                [this](const QString&, int) { refreshSources(); });
        connect(m_uploadManager, &UploadManager::uiStateChanged, this, [this] {
            if (!uploadBelongsToSession() || !m_uploadManager->isBusy()
                || uploadState() == UploadState::Preparing) {
                m_uploadPercent = m_uploadFilesCompleted = m_uploadFilesTotal = 0;
            }
            emit actionStateChanged();
        });
        connect(m_uploadManager, &UploadManager::uploadProgress, this,
                [this](int percent, int completed, int total) {
            if (!uploadBelongsToSession()) return;
            m_uploadPercent = percent;
            m_uploadFilesCompleted = completed;
            m_uploadFilesTotal = total;
            emit actionStateChanged();
        });
    }
    setCanvas(canvas);
}

QObject* ClientWorkspaceViewModel::mediaModel() const
{
    return m_overlayMediaModel;
}

int ClientWorkspaceViewModel::mediaCount() const
{
    return m_overlayMediaModel->rowCount();
}

void ClientWorkspaceViewModel::scheduleSourceRefresh()
{
    if (m_sourceRefreshQueued) return;
    m_sourceRefreshQueued = true;
    QTimer::singleShot(0, this, [this] {
        m_sourceRefreshQueued = false;
        refreshSources();
    });
}

void ClientWorkspaceViewModel::refreshSources()
{
    QHash<QString, QVariantMap> sources;
    QHash<QString, QString> knownPathIds;
    const auto media = m_canvas ? m_canvas->enumerateMediaItems() : QList<CanvasMedia*>();
    const bool hasMedia = !media.isEmpty();
    if (m_hasCanvasMedia != hasMedia) {
        m_hasCanvasMedia = hasMedia;
        emit hasCanvasMediaChanged();
    }
    const auto canonicalPath = [](const CanvasMedia* item) {
        const QFileInfo info(item->sourcePath());
        const QString canonical = info.canonicalFilePath();
        return canonical.isEmpty() ? info.absoluteFilePath() : canonical;
    };
    for (const auto* item : media) {
        if (item && !item->isText() && !item->fileId().isEmpty())
            knownPathIds.insert(canonicalPath(item), item->fileId());
    }
    for (const auto* item : media) {
        if (!item || item->isText() || item->sourcePath().isEmpty()) continue;
        const QString path = canonicalPath(item);
        const QString fileId = item->fileId().isEmpty() ? knownPathIds.value(path) : item->fileId();
        const QString key = fileId.isEmpty() ? QStringLiteral("path:") + path : QStringLiteral("sha256:") + fileId;
        QSize dimensions = item->nativeSourceSize();
        const auto asset = MediaResidencyManager::instance().asset(item->residencyOwnerId());
        if (asset) dimensions = asset->displaySize;
        QVariantMap row{{QStringLiteral("rowKey"), key},
                        {QStringLiteral("sourceId"), fileId},
                        {QStringLiteral("sourcePath"), path},
                        {QStringLiteral("displayName"), QFileInfo(path).fileName()},
                        {QStringLiteral("mediaType"), item->typeName()},
                        {QStringLiteral("width"), dimensions.width()},
                        {QStringLiteral("height"), dimensions.height()},
                        {QStringLiteral("sourceSizeBytes"), item->sourceSizeBytes()},
                        {QStringLiteral("uploadState"), QStringLiteral("not_uploaded")},
                        {QStringLiteral("uploadProgress"), 0},
                        {QStringLiteral("remoteCached"), false}};
        if (m_uploadManager) {
            const auto status = m_uploadManager->sourceUploadStatus(m_workspaceEndpointId, fileId);
            row[QStringLiteral("uploadState")] = status.state == UploadManager::SourceUploadStatus::Uploaded
                ? QStringLiteral("uploaded") : status.state == UploadManager::SourceUploadStatus::Uploading
                    ? QStringLiteral("uploading") : QStringLiteral("not_uploaded");
            row[QStringLiteral("uploadProgress")] = status.progress;
            row[QStringLiteral("remoteCached")] = m_uploadManager->remoteMediaReady(m_workspaceEndpointId, fileId);
        } else {
            // Standalone editor hosts have no transfer service. Preserve their
            // local presentation state without coupling source identity to an occurrence.
            row[QStringLiteral("uploadState")] = item->uploadState() == CanvasMedia::UploadState::Uploaded
                ? QStringLiteral("uploaded") : item->uploadState() == CanvasMedia::UploadState::Uploading
                    ? QStringLiteral("uploading") : QStringLiteral("not_uploaded");
            row[QStringLiteral("uploadProgress")] = item->uploadProgress();
        }
        auto found = sources.find(key);
        if (found == sources.end()) sources.insert(key, row);
        else {
            const auto oldState = found->value(QStringLiteral("uploadState")).toString();
            const int progress = std::max(found->value(QStringLiteral("uploadProgress")).toInt(),
                                          row.value(QStringLiteral("uploadProgress")).toInt());
            if (path < found->value(QStringLiteral("sourcePath")).toString()) *found = row;
            if (!m_uploadManager) {
                if (oldState == QLatin1String("uploaded") || row.value(QStringLiteral("uploadState")).toString() == QLatin1String("uploaded"))
                    (*found)[QStringLiteral("uploadState")] = QStringLiteral("uploaded");
                else if (oldState == QLatin1String("uploading") || row.value(QStringLiteral("uploadState")).toString() == QLatin1String("uploading"))
                    (*found)[QStringLiteral("uploadState")] = QStringLiteral("uploading");
                (*found)[QStringLiteral("uploadProgress")] = progress;
            }
        }
    }
    QVariantList rows;
    for (auto it = sources.cbegin(); it != sources.cend(); ++it) rows.append(it.value());
    std::sort(rows.begin(), rows.end(), [](const QVariant& a, const QVariant& b) {
        const auto left = a.toMap(), right = b.toMap();
        const int names = QString::compare(left.value(QStringLiteral("displayName")).toString(),
            right.value(QStringLiteral("displayName")).toString(), Qt::CaseInsensitive);
        return names ? names < 0 : left.value(QStringLiteral("rowKey")).toString() < right.value(QStringLiteral("rowKey")).toString();
    });
    const int previousCount = mediaCount();
    m_overlayMediaModel->updateFromList(rows);
    if (previousCount != mediaCount()) emit mediaCountChanged();
}

QObject* ClientWorkspaceViewModel::mediaSettings() const
{
    return m_mediaSettings;
}

QObject* ClientWorkspaceViewModel::timeline() const
{
    return m_timeline;
}

QObject* ClientWorkspaceViewModel::canvasController() const
{
    auto* host = qobject_cast<QuickCanvasHost*>(m_canvas.data());
    return host ? host->controller() : nullptr;
}

bool ClientWorkspaceViewModel::remoteCommandsEnabled() const
{
    return m_canvas && m_canvas->overlayActionsEnabled();
}

bool ClientWorkspaceViewModel::hasProject() const
{
    return m_hasProject && m_hasProject();
}

bool ClientWorkspaceViewModel::hasScreens() const
{
    return m_canvas && m_canvas->hasActiveScreens();
}

bool ClientWorkspaceViewModel::mediaEditingEnabled() const
{
    auto* host = qobject_cast<QuickCanvasHost*>(m_canvas.data());
    return hasProject() && host && host->controller()->editingEnabled();
}

bool ClientWorkspaceViewModel::canvasNavigation() const
{
    return m_canvas != nullptr;
}

bool ClientWorkspaceViewModel::mediaSync() const
{
    return hasProject() && remoteCommandsEnabled();
}

QString ClientWorkspaceViewModel::canvasNavigationUnavailableReason() const
{
    return m_canvas ? QString() : QStringLiteral("Canvas is unavailable");
}

QString ClientWorkspaceViewModel::mediaEditingUnavailableReason() const
{
    if (!hasProject()) return QStringLiteral("Create a project first");
    if (!m_canvas) return QStringLiteral("Canvas is unavailable");
    return mediaEditingEnabled()
        ? QString() : QStringLiteral("Project editing is unavailable while a scene is running");
}

QString ClientWorkspaceViewModel::mediaSyncUnavailableReason() const
{
    if (!hasProject()) return QStringLiteral("Create a project first");
    return remoteCommandsEnabled()
        ? QString() : QStringLiteral("Launch a remote session first");
}

QString ClientWorkspaceViewModel::activeTool() const
{
    return m_canvas && m_canvas->currentTool() == ICanvasHost::Tool::Text
        ? QStringLiteral("text") : QStringLiteral("selection");
}

void ClientWorkspaceViewModel::setSettingsVisible(bool visible)
{
    if (visible && !mediaEditingEnabled()) return;
    if (m_settingsVisible == visible) return;
    m_settingsVisible = visible;
    emit settingsVisibleChanged();
}

QString ClientWorkspaceViewModel::remoteSceneActionText() const
{
    switch (remoteSceneActionState()) {
    case SceneActionState::Starting: return QStringLiteral("Launching Remote Scene...");
    case SceneActionState::Active: return QStringLiteral("Stop Remote Scene");
    case SceneActionState::Stopping: return QStringLiteral("Stopping Remote Scene...");
    case SceneActionState::Unavailable:
    case SceneActionState::Ready: return QStringLiteral("Launch Remote Scene");
    }
    return QStringLiteral("Launch Remote Scene");
}

ClientWorkspaceViewModel::SceneActionState
ClientWorkspaceViewModel::remoteSceneActionState() const
{
    const ICanvasHost* canvas = m_canvas;
    if (!canvas) return SceneActionState::Unavailable;
    if (canvas->remoteSceneStopping()) return SceneActionState::Stopping;
    if (canvas->remoteSceneLaunching()) return SceneActionState::Starting;
    if (canvas->remoteSceneLaunched()) return SceneActionState::Active;
    return canvas->remoteSceneActionEnabled()
        ? SceneActionState::Ready : SceneActionState::Unavailable;
}

bool ClientWorkspaceViewModel::remoteSceneActionEnabled() const
{
    const ICanvasHost* canvas = m_canvas;
    return !m_actionPending && hasProject() && canvas && canvas->remoteSceneActionEnabled();
}

int ClientWorkspaceViewModel::remoteSceneActionTone() const
{
    switch (remoteSceneActionState()) {
    case SceneActionState::Starting:
    case SceneActionState::Stopping: return UploadingTone;
    case SceneActionState::Active: return RemoteTone;
    default: return NormalTone;
    }
}

QString ClientWorkspaceViewModel::remoteSceneUnavailableReason() const
{
    if (!hasProject()) return QStringLiteral("Create a project first");
    if (!m_canvas) return QStringLiteral("Canvas is unavailable");
    if (const auto* host = qobject_cast<const QuickCanvasHost*>(m_canvas.data()))
        return host->remoteSceneUnavailableReason();
    return m_canvas->remoteSceneActionEnabled() ? QString()
        : QStringLiteral("The remote scene is currently unavailable");
}

QString ClientWorkspaceViewModel::testSceneActionText() const
{
    return testSceneActionState() == SceneActionState::Active
        ? QStringLiteral("Pause Preview") : QStringLiteral("Play Preview");
}

ClientWorkspaceViewModel::SceneActionState
ClientWorkspaceViewModel::testSceneActionState() const
{
    const ICanvasHost* canvas = m_canvas;
    if (!canvas) return SceneActionState::Unavailable;
    if (canvas->testSceneLaunched()) return SceneActionState::Active;
    return canvas->testSceneActionEnabled()
        ? SceneActionState::Ready : SceneActionState::Unavailable;
}

bool ClientWorkspaceViewModel::testSceneActionEnabled() const
{
    const ICanvasHost* canvas = m_canvas;
    return !m_actionPending && canvas && canvas->testSceneActionEnabled();
}

int ClientWorkspaceViewModel::testSceneActionTone() const
{
    return testSceneActionState() == SceneActionState::Active ? TestTone : NormalTone;
}

QString ClientWorkspaceViewModel::testSceneUnavailableReason() const
{
    if (!hasProject()) return QStringLiteral("Create a project first");
    if (!m_canvas) return QStringLiteral("Canvas is unavailable");
    if (m_canvas->remoteSceneLaunched()) return QStringLiteral("Stop the remote scene first");
    if (const auto* host = qobject_cast<const QuickCanvasHost*>(m_canvas.data()))
        return host->mediaReadinessReason(false);
    return {};
}

QString ClientWorkspaceViewModel::uploadActionText() const
{
    switch (uploadState()) {
    case UploadState::Preparing: return QStringLiteral("Preparing…");
    case UploadState::Uploading:
        return m_uploadFilesTotal > 0
            ? QStringLiteral("Uploading (%1/%2) %3%")
                  .arg(m_uploadFilesCompleted).arg(m_uploadFilesTotal)
                  .arg(m_uploadPercent)
            : QStringLiteral("Uploading…");
    case UploadState::Finalizing: return QStringLiteral("Finalizing…");
    case UploadState::Cancelling: return QStringLiteral("Cancelling…");
    case UploadState::Uploaded: return QStringLiteral("Unload");
    case UploadState::Removing: return QStringLiteral("Removing…");
    case UploadState::Unavailable:
    case UploadState::Ready: return QStringLiteral("Upload");
    }
    return QStringLiteral("Upload");
}

ClientWorkspaceViewModel::UploadState ClientWorkspaceViewModel::uploadState() const
{
    if (!hasProject() || !m_uploadManager || !remoteCommandsEnabled()) {
        return UploadState::Unavailable;
    }
    const bool activeForSession = uploadBelongsToSession();
    if (activeForSession) {
        if (m_uploadManager->isCancelling()) return UploadState::Cancelling;
        if (m_uploadManager->isFinalizing()) return UploadState::Finalizing;
        const auto state = m_uploadManager->outgoingState();
        if (state == UploadManager::OutgoingState::AwaitingTargetReady
            || state == UploadManager::OutgoingState::Queued
            || state == UploadManager::OutgoingState::Suspended) {
            return UploadState::Preparing;
        }
        if (m_uploadManager->isUploading()) return UploadState::Uploading;
    }
    if (m_uploadManager->isRemoving() && activeForSession) {
        return UploadState::Removing;
    }
    if (m_uploadManager->isBusy()) return UploadState::Unavailable;
    const bool hasRemote = m_remoteFilesPresent && m_remoteFilesPresent();
    const bool hasUnuploaded = m_hasUnuploadedFiles && m_hasUnuploadedFiles();
    if (!m_canvas || m_canvas->enumerateMediaItems().isEmpty()) {
        return UploadState::Unavailable;
    }
    if (const ICanvasHost* canvas = m_canvas;
        canvas && (canvas->remoteSceneLaunched()
                   || canvas->remoteSceneLaunching()
                   || canvas->remoteSceneStopping())) {
        return UploadState::Unavailable;
    }
    if (hasRemote && !hasUnuploaded) return UploadState::Uploaded;
    return UploadState::Ready;
}

bool ClientWorkspaceViewModel::uploadBelongsToSession() const
{
    // Workspace IDs are persistent identities; transport endpoint IDs change
    // on reconnect and must never be compared to a workspace ID.
    return m_uploadManager && m_uploadManager->activeWorkspaceEndpointId() == m_workspaceEndpointId;
}

bool ClientWorkspaceViewModel::uploadActionEnabled() const
{
    const UploadState state = uploadState();
    return !m_actionPending && m_uploadAction && (state == UploadState::Ready
        || state == UploadState::Uploaded || state == UploadState::Uploading);
}

QString ClientWorkspaceViewModel::uploadUnavailableReason() const
{
    if (!hasProject()) return QStringLiteral("Create a project first");
    if (!m_canvas) return QStringLiteral("Canvas is unavailable");
    const UploadState state = uploadState();
    // Cancellation remains available during upload, even if the last local
    // media was removed after the transfer started.
    if (m_uploadAction && (state == UploadState::Ready || state == UploadState::Uploaded
                          || state == UploadState::Uploading)) return {};
    if (m_canvas->enumerateMediaItems().isEmpty()) {
        return QStringLiteral("Add media to the project first");
    }
    if (!m_uploadManager || !m_uploadAction) return QStringLiteral("Media upload is unavailable");
    if (!remoteCommandsEnabled()) return QStringLiteral("Launch a remote session first");
    if (m_canvas->remoteSceneLaunching()) return QStringLiteral("The remote scene is starting. Please wait");
    if (m_canvas->remoteSceneStopping()) return QStringLiteral("The remote scene is stopping. Please wait");
    if (m_canvas->remoteSceneLaunched()) return QStringLiteral("Stop the remote scene first");
    switch (state) {
    case UploadState::Preparing:
        return m_uploadManager->outgoingState() == UploadManager::OutgoingState::Suspended
            ? QStringLiteral("The upload is paused while the remote computer reconnects")
            : QStringLiteral("The upload is being prepared. Please wait");
    case UploadState::Finalizing: return QStringLiteral("The upload is being finalized. Please wait");
    case UploadState::Cancelling: return QStringLiteral("The upload cancellation is in progress. Please wait");
    case UploadState::Removing: return QStringLiteral("Remote media are being removed. Please wait");
    case UploadState::Unavailable: return QStringLiteral("Another media transfer is in progress. Wait for it to finish");
    default: return {};
    }
}

int ClientWorkspaceViewModel::uploadActionTone() const
{
    switch (uploadState()) {
    case UploadState::Uploaded: return UploadedTone;
    case UploadState::Preparing:
    case UploadState::Uploading:
    case UploadState::Finalizing:
    case UploadState::Cancelling: return UploadingTone;
    default: return NormalTone;
    }
}

void ClientWorkspaceViewModel::setCanvas(ICanvasHost* canvas)
{
    if (m_canvas == canvas && typedMediaModel()) {
        setLoading(false);
        return;
    }
    if (MediaListModel* previous = typedMediaModel()) {
        disconnect(previous, nullptr, this, nullptr);
    }
    m_canvas = canvas;
    if (m_canvas) {
        connect(m_canvas, &ICanvasHost::actionStateChanged,
                this, &ClientWorkspaceViewModel::actionStateChanged,
                Qt::UniqueConnection);
        connect(m_canvas, &ICanvasHost::toolChanged,
                this, &ClientWorkspaceViewModel::activeToolChanged,
                Qt::UniqueConnection);
    }
    auto* host = qobject_cast<QuickCanvasHost*>(m_canvas.data());
    m_mediaSettings->setController(host ? host->controller() : nullptr);
    m_timeline->setHost(host);
    if (host) connect(host->controller(), &QuickCanvasController::editingEnabledChanged,
                      this, &ClientWorkspaceViewModel::actionStateChanged, Qt::UniqueConnection);
    if (MediaListModel* model = typedMediaModel()) {
        connect(model, &QAbstractItemModel::rowsInserted,
                this, &ClientWorkspaceViewModel::refreshSources);
        connect(model, &QAbstractItemModel::rowsRemoved,
                this, &ClientWorkspaceViewModel::refreshSources);
        connect(model, &QAbstractItemModel::modelReset,
                this, &ClientWorkspaceViewModel::refreshSources);
        connect(model, &QAbstractItemModel::dataChanged,
                this, &ClientWorkspaceViewModel::scheduleSourceRefresh);
    }
    refreshSources();
    setLoading(!canvas);
    emit mediaModelChanged();
    emit mediaCountChanged();
    emit actionStateChanged();
    emit activeToolChanged();
}

void ClientWorkspaceViewModel::setLoading(bool loading)
{
    if (m_loading == loading) return;
    m_loading = loading;
    emit loadingChanged();
}

void ClientWorkspaceViewModel::refreshCapabilities()
{
    if (!hasProject() && m_settingsVisible) {
        m_settingsVisible = false;
        emit settingsVisibleChanged();
    }
    emit actionStateChanged();
}

void ClientWorkspaceViewModel::selectMedia(const QString& mediaId, bool additive)
{
    auto* host = qobject_cast<QuickCanvasHost*>(m_canvas.data());
    if (host && host->controller()) {
        host->controller()->selectMedia(mediaId, additive);
    }
}

bool ClientWorkspaceViewModel::beginFileDrag(const QVariantList& urls,
                                           qreal x, qreal y)
{
    auto* host = qobject_cast<QuickCanvasHost*>(m_canvas.data());
    return mediaEditingEnabled() && host && host->controller()
        && host->controller()->beginLocalFileDrag(urls, x, y);
}

bool ClientWorkspaceViewModel::updateFileDrag(qreal x, qreal y)
{
    auto* host = qobject_cast<QuickCanvasHost*>(m_canvas.data());
    return mediaEditingEnabled() && host && host->controller()
        && host->controller()->updateLocalFileDrag(x, y);
}

bool ClientWorkspaceViewModel::commitFileDrop(qreal x, qreal y)
{
    auto* host = qobject_cast<QuickCanvasHost*>(m_canvas.data());
    return mediaEditingEnabled() && host && host->controller()
        && host->controller()->commitLocalFileDrop(x, y);
}

void ClientWorkspaceViewModel::cancelFileDrag()
{
    auto* host = qobject_cast<QuickCanvasHost*>(m_canvas.data());
    if (host && host->controller()) host->controller()->cancelLocalFileDrag();
}

void ClientWorkspaceViewModel::setActiveTool(const QString& tool)
{
    if (!m_canvas) return;
    if (tool == QLatin1String("text") && !mediaEditingEnabled()) return;
    m_canvas->setCurrentTool(tool == QLatin1String("text")
        ? ICanvasHost::Tool::Text : ICanvasHost::Tool::Selection);
}

void ClientWorkspaceViewModel::toggleRemoteScene()
{
    if (m_actionPending) {
        TOAST_INFO("An action is already being processed. Please wait");
        return;
    }
    const QString reason = remoteSceneUnavailableReason();
    if (!reason.isEmpty()) { TOAST_INFO(reason); return; }
    dispatchAction([this] {
        const QString reason = remoteSceneUnavailableReason();
        if (!reason.isEmpty()) { TOAST_INFO(reason); return; }
        m_canvas->triggerRemoteSceneAction();
    });
}

void ClientWorkspaceViewModel::toggleTestScene()
{
    if (testSceneActionEnabled() && m_canvas) {
        dispatchAction([this] {
            if (m_canvas && hasProject() && m_canvas->testSceneActionEnabled())
                m_canvas->triggerTestSceneAction();
        });
    }
}

void ClientWorkspaceViewModel::triggerUploadAction()
{
    if (m_actionPending) {
        TOAST_INFO("An action is already being processed. Please wait");
        return;
    }
    const QString reason = uploadUnavailableReason();
    if (!reason.isEmpty()) { TOAST_INFO(reason); return; }
    const UploadState requestedState = uploadState();
    dispatchAction([this, requestedState] {
        const QString reason = uploadUnavailableReason();
        if (!reason.isEmpty()) { TOAST_INFO(reason); return; }
        if (uploadState() != requestedState) {
            TOAST_INFO("The upload state has changed. Please try again");
            return;
        }
        m_uploadAction();
    });
}

void ClientWorkspaceViewModel::dispatchAction(std::function<void()> action)
{
    // Lock synchronously, before validation or network work. Even a local
    // rejection must publish the settled state and release the client lock.
    m_actionPending = true;
    emit actionStateChanged();
    QTimer::singleShot(0, this, [this, action = std::move(action)] {
        action();
        m_actionPending = false;
        emit actionStateChanged();
    });
}

MediaListModel* ClientWorkspaceViewModel::typedMediaModel() const
{
    auto* host = qobject_cast<QuickCanvasHost*>(m_canvas.data());
    return host && host->controller() ? host->controller()->mediaListModel() : nullptr;
}
