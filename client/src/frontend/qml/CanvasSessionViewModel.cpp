#include "frontend/qml/CanvasSessionViewModel.h"

#include "frontend/rendering/canvas/MediaListModel.h"
#include "frontend/rendering/canvas/QuickCanvasController.h"
#include "frontend/rendering/canvas/QuickCanvasHost.h"
#include "frontend/qml/MediaSettingsViewModel.h"
#include "backend/network/UploadManager.h"
#include "shared/rendering/ICanvasHost.h"

#include <QAbstractItemModel>
#include <QSortFilterProxyModel>
#include <QTimer>

CanvasSessionViewModel::CanvasSessionViewModel(QString sessionId,
                                               ICanvasHost* canvas,
                                               std::function<void()> uploadAction,
                                               UploadManager* uploadManager,
                                               std::function<bool()> remoteFilesPresent,
                                               std::function<bool()> hasUnuploadedFiles,
                                               std::function<bool()> hasProject,
                                               QObject* parent)
    : QObject(parent)
    , m_sessionId(std::move(sessionId))
    , m_uploadAction(std::move(uploadAction))
    , m_uploadManager(uploadManager)
    , m_remoteFilesPresent(std::move(remoteFilesPresent))
    , m_hasUnuploadedFiles(std::move(hasUnuploadedFiles))
    , m_hasProject(std::move(hasProject))
    , m_mediaSettings(new MediaSettingsViewModel(this))
    , m_overlayMediaModel(new QSortFilterProxyModel(this))
{
    m_overlayMediaModel->setSortRole(MediaListModel::ZRole);
    m_overlayMediaModel->sort(0, Qt::DescendingOrder);
    if (m_uploadManager) {
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

QObject* CanvasSessionViewModel::mediaModel() const
{
    return m_overlayMediaModel;
}

int CanvasSessionViewModel::mediaCount() const
{
    return typedMediaModel() ? typedMediaModel()->rowCount() : 0;
}

QObject* CanvasSessionViewModel::mediaSettings() const
{
    return m_mediaSettings;
}

QObject* CanvasSessionViewModel::canvasController() const
{
    auto* host = qobject_cast<QuickCanvasHost*>(m_canvas.data());
    return host ? host->controller() : nullptr;
}

bool CanvasSessionViewModel::remoteCommandsEnabled() const
{
    return m_canvas && m_canvas->overlayActionsEnabled();
}

bool CanvasSessionViewModel::hasProject() const
{
    return m_hasProject && m_hasProject();
}

bool CanvasSessionViewModel::mediaEditingEnabled() const
{
    return hasProject() && m_canvas && m_canvas->projectEditingEnabled();
}

bool CanvasSessionViewModel::canvasNavigation() const
{
    return m_canvas != nullptr;
}

bool CanvasSessionViewModel::mediaSync() const
{
    return hasProject() && remoteCommandsEnabled();
}

QString CanvasSessionViewModel::canvasNavigationUnavailableReason() const
{
    return m_canvas ? QString() : QStringLiteral("Canvas is unavailable");
}

QString CanvasSessionViewModel::mediaEditingUnavailableReason() const
{
    if (!hasProject()) return QStringLiteral("Create a project first");
    if (!m_canvas) return QStringLiteral("Canvas is unavailable");
    return m_canvas->projectEditingEnabled()
        ? QString() : QStringLiteral("Project editing is unavailable");
}

QString CanvasSessionViewModel::mediaSyncUnavailableReason() const
{
    if (!hasProject()) return QStringLiteral("Create a project first");
    return remoteCommandsEnabled()
        ? QString() : QStringLiteral("Launch a remote session first");
}

QString CanvasSessionViewModel::activeTool() const
{
    return m_canvas && m_canvas->currentTool() == ICanvasHost::Tool::Text
        ? QStringLiteral("text") : QStringLiteral("selection");
}

void CanvasSessionViewModel::setSettingsVisible(bool visible)
{
    if (visible && !mediaEditingEnabled()) return;
    if (m_settingsVisible == visible) return;
    m_settingsVisible = visible;
    emit settingsVisibleChanged();
}

QString CanvasSessionViewModel::remoteSceneActionText() const
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

CanvasSessionViewModel::SceneActionState
CanvasSessionViewModel::remoteSceneActionState() const
{
    const ICanvasHost* canvas = m_canvas;
    if (!canvas) return SceneActionState::Unavailable;
    if (canvas->remoteSceneStopping()) return SceneActionState::Stopping;
    if (canvas->remoteSceneLaunching()) return SceneActionState::Starting;
    if (canvas->remoteSceneLaunched()) return SceneActionState::Active;
    return canvas->remoteSceneActionEnabled()
        ? SceneActionState::Ready : SceneActionState::Unavailable;
}

bool CanvasSessionViewModel::remoteSceneActionEnabled() const
{
    const ICanvasHost* canvas = m_canvas;
    return !m_actionPending && canvas && canvas->remoteSceneActionEnabled();
}

int CanvasSessionViewModel::remoteSceneActionTone() const
{
    switch (remoteSceneActionState()) {
    case SceneActionState::Starting:
    case SceneActionState::Stopping: return UploadingTone;
    case SceneActionState::Active: return RemoteTone;
    default: return NormalTone;
    }
}

QString CanvasSessionViewModel::remoteSceneUnavailableReason() const
{
    if (!hasProject()) return QStringLiteral("Create a project first");
    if (!m_canvas || m_canvas->enumerateMediaItems().isEmpty()) {
        return QStringLiteral("Add media to the project first");
    }
    if (!m_canvas->hasActiveScreens()) return QStringLiteral("No target screens available");
    if (!remoteCommandsEnabled()) return QStringLiteral("Launch a remote session first");
    if (m_uploadManager && m_uploadManager->isBusy()) {
        return QStringLiteral("A media transfer is in progress");
    }
    return {};
}

QString CanvasSessionViewModel::testSceneActionText() const
{
    return testSceneActionState() == SceneActionState::Active
        ? QStringLiteral("Stop Test Scene") : QStringLiteral("Launch Test Scene");
}

CanvasSessionViewModel::SceneActionState
CanvasSessionViewModel::testSceneActionState() const
{
    const ICanvasHost* canvas = m_canvas;
    if (!canvas) return SceneActionState::Unavailable;
    if (canvas->testSceneLaunched()) return SceneActionState::Active;
    return canvas->testSceneActionEnabled()
        ? SceneActionState::Ready : SceneActionState::Unavailable;
}

bool CanvasSessionViewModel::testSceneActionEnabled() const
{
    const ICanvasHost* canvas = m_canvas;
    return !m_actionPending && canvas && canvas->testSceneActionEnabled();
}

int CanvasSessionViewModel::testSceneActionTone() const
{
    return testSceneActionState() == SceneActionState::Active ? TestTone : NormalTone;
}

QString CanvasSessionViewModel::testSceneUnavailableReason() const
{
    if (!hasProject()) return QStringLiteral("Create a project first");
    if (!m_canvas || m_canvas->enumerateMediaItems().isEmpty()) {
        return QStringLiteral("Add media to the project first");
    }
    if (m_canvas->remoteSceneLaunched()) return QStringLiteral("Stop the remote scene first");
    return {};
}

QString CanvasSessionViewModel::uploadActionText() const
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

CanvasSessionViewModel::UploadState CanvasSessionViewModel::uploadState() const
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

bool CanvasSessionViewModel::uploadBelongsToSession() const
{
    // Workspace IDs are persistent identities; transport endpoint IDs change
    // on reconnect and must never be compared to a workspace ID.
    return m_uploadManager && m_uploadManager->activeSessionIdentity() == m_sessionId;
}

bool CanvasSessionViewModel::uploadActionEnabled() const
{
    const UploadState state = uploadState();
    return !m_actionPending && (state == UploadState::Ready
        || state == UploadState::Uploaded || state == UploadState::Uploading);
}

QString CanvasSessionViewModel::uploadUnavailableReason() const
{
    if (!hasProject()) return QStringLiteral("Create a project first");
    if (!m_canvas || m_canvas->enumerateMediaItems().isEmpty()) {
        return QStringLiteral("Add media to the project first");
    }
    if (!remoteCommandsEnabled()) return QStringLiteral("Launch a remote session first");
    if (m_canvas->remoteSceneLaunched()) return QStringLiteral("Stop the remote scene first");
    return {};
}

int CanvasSessionViewModel::uploadActionTone() const
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

void CanvasSessionViewModel::setCanvas(ICanvasHost* canvas)
{
    if (m_canvas == canvas && typedMediaModel()) {
        setLoading(false);
        return;
    }
    if (MediaListModel* previous = typedMediaModel()) {
        disconnect(previous, nullptr, this, nullptr);
    }
    m_canvas = canvas;
    m_overlayMediaModel->setSourceModel(typedMediaModel());
    if (m_canvas) {
        connect(m_canvas, &ICanvasHost::actionStateChanged,
                this, &CanvasSessionViewModel::actionStateChanged,
                Qt::UniqueConnection);
        connect(m_canvas, &ICanvasHost::toolChanged,
                this, &CanvasSessionViewModel::activeToolChanged,
                Qt::UniqueConnection);
    }
    auto* host = qobject_cast<QuickCanvasHost*>(m_canvas.data());
    m_mediaSettings->setController(host ? host->controller() : nullptr);
    if (MediaListModel* model = typedMediaModel()) {
        connect(model, &QAbstractItemModel::rowsInserted,
                this, &CanvasSessionViewModel::mediaCountChanged);
        connect(model, &QAbstractItemModel::rowsRemoved,
                this, &CanvasSessionViewModel::mediaCountChanged);
        connect(model, &QAbstractItemModel::modelReset,
                this, &CanvasSessionViewModel::mediaCountChanged);
    }
    setLoading(!canvas);
    emit mediaModelChanged();
    emit mediaCountChanged();
    emit actionStateChanged();
    emit activeToolChanged();
}

void CanvasSessionViewModel::setLoading(bool loading)
{
    if (m_loading == loading) return;
    m_loading = loading;
    emit loadingChanged();
}

void CanvasSessionViewModel::refreshCapabilities()
{
    if (!hasProject() && m_settingsVisible) {
        m_settingsVisible = false;
        emit settingsVisibleChanged();
    }
    emit actionStateChanged();
}

void CanvasSessionViewModel::selectMedia(const QString& mediaId, bool additive)
{
    auto* host = qobject_cast<QuickCanvasHost*>(m_canvas.data());
    if (host && host->controller()) {
        host->controller()->selectMedia(mediaId, additive);
    }
}

bool CanvasSessionViewModel::beginFileDrag(const QVariantList& urls,
                                           qreal x, qreal y)
{
    auto* host = qobject_cast<QuickCanvasHost*>(m_canvas.data());
    return mediaEditingEnabled() && host && host->controller()
        && host->controller()->beginLocalFileDrag(urls, x, y);
}

bool CanvasSessionViewModel::updateFileDrag(qreal x, qreal y)
{
    auto* host = qobject_cast<QuickCanvasHost*>(m_canvas.data());
    return mediaEditingEnabled() && host && host->controller()
        && host->controller()->updateLocalFileDrag(x, y);
}

bool CanvasSessionViewModel::commitFileDrop(qreal x, qreal y)
{
    auto* host = qobject_cast<QuickCanvasHost*>(m_canvas.data());
    return mediaEditingEnabled() && host && host->controller()
        && host->controller()->commitLocalFileDrop(x, y);
}

void CanvasSessionViewModel::cancelFileDrag()
{
    auto* host = qobject_cast<QuickCanvasHost*>(m_canvas.data());
    if (host && host->controller()) host->controller()->cancelLocalFileDrag();
}

void CanvasSessionViewModel::setActiveTool(const QString& tool)
{
    if (!m_canvas) return;
    if (tool == QLatin1String("text") && !mediaEditingEnabled()) return;
    m_canvas->setCurrentTool(tool == QLatin1String("text")
        ? ICanvasHost::Tool::Text : ICanvasHost::Tool::Selection);
}

void CanvasSessionViewModel::toggleRemoteScene()
{
    if (remoteSceneActionEnabled() && m_canvas) {
        dispatchAction([this] {
            if (m_canvas && hasProject() && m_canvas->remoteSceneActionEnabled())
                m_canvas->triggerRemoteSceneAction();
        });
    }
}

void CanvasSessionViewModel::toggleTestScene()
{
    if (testSceneActionEnabled() && m_canvas) {
        dispatchAction([this] {
            if (m_canvas && hasProject() && m_canvas->testSceneActionEnabled())
                m_canvas->triggerTestSceneAction();
        });
    }
}

void CanvasSessionViewModel::triggerUploadAction()
{
    if (!uploadActionEnabled() || !m_uploadAction) return;
    const UploadState requestedState = uploadState();
    dispatchAction([this, requestedState] {
        if (uploadState() == requestedState && m_uploadAction) m_uploadAction();
    });
}

void CanvasSessionViewModel::dispatchAction(std::function<void()> action)
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

MediaListModel* CanvasSessionViewModel::typedMediaModel() const
{
    auto* host = qobject_cast<QuickCanvasHost*>(m_canvas.data());
    return host && host->controller() ? host->controller()->mediaListModel() : nullptr;
}
