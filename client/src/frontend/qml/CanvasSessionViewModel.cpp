#include "frontend/qml/CanvasSessionViewModel.h"

#include "frontend/rendering/canvas/MediaListModel.h"
#include "frontend/rendering/canvas/QuickCanvasController.h"
#include "frontend/rendering/canvas/QuickCanvasHost.h"
#include "frontend/qml/MediaSettingsViewModel.h"
#include "backend/network/UploadManager.h"
#include "shared/rendering/ICanvasHost.h"

#include <QAbstractItemModel>

CanvasSessionViewModel::CanvasSessionViewModel(QString sessionId,
                                               ICanvasHost* canvas,
                                               std::function<void()> uploadAction,
                                               UploadManager* uploadManager,
                                               std::function<bool()> remoteFilesPresent,
                                               std::function<bool()> hasUnuploadedFiles,
                                               QObject* parent)
    : QObject(parent)
    , m_sessionId(std::move(sessionId))
    , m_canvas(canvas)
    , m_uploadAction(std::move(uploadAction))
    , m_uploadManager(uploadManager)
    , m_remoteFilesPresent(std::move(remoteFilesPresent))
    , m_hasUnuploadedFiles(std::move(hasUnuploadedFiles))
    , m_mediaSettings(new MediaSettingsViewModel(this))
{
    if (m_uploadManager) {
        connect(m_uploadManager, &UploadManager::uiStateChanged,
                this, &CanvasSessionViewModel::actionStateChanged);
        connect(m_uploadManager, &UploadManager::uploadProgress, this,
                [this](int percent, int completed, int total) {
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
    return typedMediaModel();
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

bool CanvasSessionViewModel::actionsEnabled() const
{
    return m_canvas && m_canvas->overlayActionsEnabled();
}

QString CanvasSessionViewModel::activeTool() const
{
    return m_canvas && m_canvas->currentTool() == ICanvasHost::Tool::Text
        ? QStringLiteral("text") : QStringLiteral("selection");
}

void CanvasSessionViewModel::setSettingsVisible(bool visible)
{
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
    return canvas && canvas->remoteSceneActionEnabled();
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
    return canvas && canvas->testSceneActionEnabled();
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
    if (!m_uploadManager || !actionsEnabled()) return UploadState::Unavailable;
    const bool activeForSession = m_uploadManager->hasActiveUpload()
        && m_uploadManager->activeUploadTargetClientId() == m_sessionId;
    if (activeForSession) {
        if (m_uploadManager->isCancelling()) return UploadState::Cancelling;
        if (m_uploadManager->isFinalizing()) return UploadState::Finalizing;
        if (m_uploadManager->outgoingState()
            == UploadManager::OutgoingState::AwaitingTargetReady) {
            return UploadState::Preparing;
        }
        if (m_uploadManager->isUploading()) return UploadState::Uploading;
    }
    if (m_uploadManager->isRemoving()
        && m_uploadManager->targetClientId() == m_sessionId) {
        return UploadState::Removing;
    }
    const bool hasRemote = m_remoteFilesPresent && m_remoteFilesPresent();
    const bool hasUnuploaded = m_hasUnuploadedFiles && m_hasUnuploadedFiles();
    if (hasRemote && !hasUnuploaded) return UploadState::Uploaded;
    if (const ICanvasHost* canvas = m_canvas;
        canvas && (canvas->remoteSceneLaunched()
                   || canvas->remoteSceneLaunching()
                   || canvas->remoteSceneStopping())) {
        return UploadState::Unavailable;
    }
    return UploadState::Ready;
}

bool CanvasSessionViewModel::uploadActionEnabled() const
{
    const UploadState state = uploadState();
    return state == UploadState::Ready || state == UploadState::Uploaded
        || state == UploadState::Preparing || state == UploadState::Uploading;
}

int CanvasSessionViewModel::uploadActionTone() const
{
    switch (uploadState()) {
    case UploadState::Uploaded: return 2;
    case UploadState::Preparing:
    case UploadState::Uploading:
    case UploadState::Finalizing:
    case UploadState::Cancelling: return 3;
    case UploadState::Ready: return 1;
    default: return 0;
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
    return host && host->controller()
        && host->controller()->beginLocalFileDrag(urls, x, y);
}

bool CanvasSessionViewModel::updateFileDrag(qreal x, qreal y)
{
    auto* host = qobject_cast<QuickCanvasHost*>(m_canvas.data());
    return host && host->controller()
        && host->controller()->updateLocalFileDrag(x, y);
}

bool CanvasSessionViewModel::commitFileDrop(qreal x, qreal y)
{
    auto* host = qobject_cast<QuickCanvasHost*>(m_canvas.data());
    return host && host->controller()
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
    m_canvas->setCurrentTool(tool == QLatin1String("text")
        ? ICanvasHost::Tool::Text : ICanvasHost::Tool::Selection);
}

void CanvasSessionViewModel::toggleRemoteScene()
{
    if (m_canvas) m_canvas->triggerRemoteSceneAction();
}

void CanvasSessionViewModel::toggleTestScene()
{
    if (m_canvas) m_canvas->triggerTestSceneAction();
}

void CanvasSessionViewModel::triggerUploadAction()
{
    if (m_uploadAction) m_uploadAction();
}

MediaListModel* CanvasSessionViewModel::typedMediaModel() const
{
    auto* host = qobject_cast<QuickCanvasHost*>(m_canvas.data());
    return host && host->controller() ? host->controller()->mediaListModel() : nullptr;
}
