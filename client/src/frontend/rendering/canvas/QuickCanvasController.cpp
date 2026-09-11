#include "frontend/rendering/canvas/QuickCanvasController.h"
#include "frontend/rendering/canvas/CanvasSceneStore.h"
#include "frontend/rendering/canvas/TextEditHelper.h"
#include "frontend/rendering/canvas/CanvasQmlTypes.h"
#include "backend/domain/media/MediaRuntimeHooks.h"
#include "backend/domain/media/MediaFilePolicy.h"
#include "backend/domain/media/TextRenderState.h"
#include "frontend/rendering/canvas/GestureCommands.h"
#include "frontend/rendering/canvas/ModelPublisher.h"
#include "frontend/rendering/canvas/PointerSession.h"
#include "frontend/rendering/canvas/QuickCanvasViewAdapter.h"
#include "frontend/rendering/canvas/MediaListModel.h"
#include "frontend/rendering/canvas/SnapEngine.h"
#include "frontend/rendering/canvas/SnapStore.h"
#include "frontend/rendering/canvas/SnapGuidePublisher.h"
#include "frontend/rendering/remote/RemoteVideoFrameItem.h"
#include "frontend/ui/notifications/ToastNotificationSystem.h"

#ifdef Q_OS_MACOS
#include "backend/platform/macos/MacVideoThumbnailer.h"
#elif defined(Q_OS_WIN)
#include "backend/platform/windows/WindowsVideoThumbnailer.h"
#endif

#include <algorithm>
#include <limits>
#include <QGraphicsScene>
#include <QGraphicsItem>
#include <QMetaObject>
#include <QEvent>
#include <QDropEvent>
#include <QDragEnterEvent>
#include <QDragMoveEvent>
#include <QMimeData>
#include <QDragLeaveEvent>
#include <QMouseEvent>
#include <QQuickItem>
#include <QQuickWidget>
#include <QScreen>
#include <QScopedValueRollback>
#include <QLineF>
#include <QGuiApplication>
#include <QTimer>
#include <QVariantList>
#include <QWindow>
#include <QDebug>
#include <QtMath>
#include <QUrl>
#include <QFontInfo>
#include <QFileInfo>
#include <QImageIOHandler>
#include <QImageReader>
#include <QMediaMetaData>
#include <QAudioOutput>
#include <QVideoFrame>
#include <QVideoSink>
#include <QFutureWatcher>
#include <QtConcurrent/QtConcurrentRun>

#include "backend/domain/media/MediaItems.h"
#include "backend/domain/media/TextMediaItem.h"
#include "frontend/rendering/canvas/ScreenCanvas.h"
#include <QMediaPlayer>

namespace {
constexpr int kRemoteCursorDiameterPx = 30;
constexpr qreal kRemoteCursorBorderWidthPx = 2.0;

QSize orientedImageSize(QImageReader& reader) {
    QSize size = reader.size();
    if (size.isEmpty()) {
        return {};
    }

    switch (reader.transformation()) {
    case QImageIOHandler::TransformationRotate90:
    case QImageIOHandler::TransformationRotate270:
    case QImageIOHandler::TransformationMirrorAndRotate90:
    case QImageIOHandler::TransformationFlipAndRotate90:
        size.transpose();
        break;
    default:
        break;
    }
    return size;
}

QImage limitPreviewImage(QImage image) {
    constexpr int kMaximumPreviewEdge = 2048;
    if (image.isNull()) {
        return {};
    }
    if (image.width() <= kMaximumPreviewEdge && image.height() <= kMaximumPreviewEdge) {
        return image;
    }
    return image.scaled(QSize(kMaximumPreviewEdge, kMaximumPreviewEdge),
                        Qt::KeepAspectRatio, Qt::SmoothTransformation);
}

QImage decodeImagePreview(const QString& localPath) {
    QImageReader reader(localPath);
    reader.setAutoTransform(true);
    QSize requested = reader.size();
    if (!requested.isEmpty() && (requested.width() > 2048 || requested.height() > 2048)) {
        requested.scale(QSize(2048, 2048), Qt::KeepAspectRatio);
        reader.setScaledSize(requested);
    }
    return limitPreviewImage(reader.read());
}

QSize nativeVideoDimensions(const QString& localPath) {
#ifdef Q_OS_MACOS
    return MacVideoThumbnailer::videoDimensions(localPath);
#elif defined(Q_OS_WIN)
    return WindowsVideoThumbnailer::videoDimensions(localPath);
#else
    Q_UNUSED(localPath);
    return {};
#endif
}

QImage nativeVideoFirstFrame(const QString& localPath) {
#ifdef Q_OS_MACOS
    return limitPreviewImage(MacVideoThumbnailer::firstFrame(localPath));
#elif defined(Q_OS_WIN)
    return limitPreviewImage(WindowsVideoThumbnailer::firstFrame(localPath));
#else
    Q_UNUSED(localPath);
    return {};
#endif
}

QString uploadStateToString(ResizableMediaBase::UploadState state) {
    switch (state) {
    case ResizableMediaBase::UploadState::NotUploaded:
        return QStringLiteral("not_uploaded");
    case ResizableMediaBase::UploadState::Uploading:
        return QStringLiteral("uploading");
    case ResizableMediaBase::UploadState::Uploaded:
        return QStringLiteral("uploaded");
    }
    return QStringLiteral("not_uploaded");
}

bool uiZonesEquivalent(const QList<ScreenInfo::UIZone>& lhs, const QList<ScreenInfo::UIZone>& rhs) {
    if (lhs.size() != rhs.size()) {
        return false;
    }

    for (int i = 0; i < lhs.size(); ++i) {
        const ScreenInfo::UIZone& leftZone = lhs.at(i);
        const ScreenInfo::UIZone& rightZone = rhs.at(i);
        if (leftZone.type != rightZone.type
            || leftZone.x != rightZone.x
            || leftZone.y != rightZone.y
            || leftZone.width != rightZone.width
            || leftZone.height != rightZone.height) {
            return false;
        }
    }

    return true;
}

bool screenListsEquivalent(const QList<ScreenInfo>& lhs, const QList<ScreenInfo>& rhs) {
    if (lhs.size() != rhs.size()) {
        return false;
    }

    for (int i = 0; i < lhs.size(); ++i) {
        const ScreenInfo& leftScreen = lhs.at(i);
        const ScreenInfo& rightScreen = rhs.at(i);
        if (leftScreen.id != rightScreen.id
            || leftScreen.width != rightScreen.width
            || leftScreen.height != rightScreen.height
            || leftScreen.x != rightScreen.x
            || leftScreen.y != rightScreen.y
            || leftScreen.primary != rightScreen.primary
            || !uiZonesEquivalent(leftScreen.uiZones, rightScreen.uiZones)) {
            return false;
        }
    }

    return true;
}

template <typename T, typename Equals>
void sortAndDeduplicate(QVector<T>& values, Equals equals) {
    std::sort(values.begin(), values.end());
    values.erase(std::unique(values.begin(), values.end(), equals), values.end());
}

QString toCanonicalMediaSourceUrl(const QString& sourcePath) {
    if (sourcePath.isEmpty()) {
        return QString();
    }

    const QString lower = sourcePath.left(8).toLower();
    if (lower.startsWith(QStringLiteral("file://"))
        || lower.startsWith(QStringLiteral("qrc:"))
        || lower.startsWith(QStringLiteral("http://"))
        || lower.startsWith(QStringLiteral("https://"))) {
        return sourcePath;
    }

    const bool isWindowsDrivePath = sourcePath.size() >= 3
        && sourcePath.at(1) == QChar(':')
        && (sourcePath.at(2) == QChar('\\') || sourcePath.at(2) == QChar('/'));
    const bool isWindowsUncPath = sourcePath.startsWith(QStringLiteral("\\\\"));

    if (isWindowsDrivePath || isWindowsUncPath) {
        return QUrl::fromLocalFile(sourcePath).toString();
    }

    const QUrl maybeUrl(sourcePath);
    if (maybeUrl.isValid() && !maybeUrl.scheme().isEmpty()) {
        return maybeUrl.toString();
    }

    return QUrl::fromLocalFile(sourcePath).toString();
}
}

QuickCanvasController::QuickCanvasController(QObject* parent)
    : QObject(parent)
{
    m_sceneStore = new CanvasSceneStore(this);
    m_pointerSession = new PointerSession();
    m_modelPublisher = new ModelPublisher();
    m_snapStore = new SnapStore();
    m_dragSnapSession = new QuickDragSnapSession();
    m_mediaListModel = new MediaListModel(this);
    m_localDragFrameSource = new RemoteVideoFrameSource(this);

    m_mediaSyncTimer = new QTimer(this);
    m_mediaSyncTimer->setSingleShot(true);
    m_mediaSyncTimer->setInterval(16);
    connect(m_mediaSyncTimer, &QTimer::timeout, this, [this]() {
        syncMediaModelFromScene();
    });

    m_resizeDispatchTimer = new QTimer(this);
    m_resizeDispatchTimer->setSingleShot(true);
    m_resizeDispatchTimer->setInterval(8);
    connect(m_resizeDispatchTimer, &QTimer::timeout, this, [this]() {
        if (!m_hasQueuedResize) {
            return;
        }

        const QString mediaId = m_queuedResizeMediaId;
        const QString handleId = m_queuedResizeHandleId;
        const qreal sceneX = m_queuedResizeSceneX;
        const qreal sceneY = m_queuedResizeSceneY;
        const bool snap = m_queuedResizeSnap;
        const bool alt = m_queuedResizeAlt;

        m_hasQueuedResize = false;
        m_executingQueuedResize = true;
        handleMediaResizeRequested(mediaId, handleId, sceneX, sceneY, snap, alt);
        m_executingQueuedResize = false;

        if (m_hasQueuedResize) {
            m_resizeDispatchTimer->start();
        }
    });

    m_videoStateTimer = new QTimer(this);
    m_videoStateTimer->setSingleShot(false);
    m_videoStateTimer->setInterval(50);
    connect(m_videoStateTimer, &QTimer::timeout, this, [this]() {
        pushVideoStateModel();
    });

    m_fadeTickTimer = new QTimer(this);
    m_fadeTickTimer->setSingleShot(true);
    m_fadeTickTimer->setInterval(16);
    connect(m_fadeTickTimer, &QTimer::timeout, this, [this]() {
        pushMediaModelOnly();
    });

    m_initialFitRetryTimer = new QTimer(this);
    m_initialFitRetryTimer->setSingleShot(true);
    m_initialFitRetryTimer->setInterval(16);
    connect(m_initialFitRetryTimer, &QTimer::timeout, this, [this]() {
        if (m_initialFitCompleted) {
            m_initialFitPending = false;
            m_initialFitRetryCount = 0;
            return;
        }

        if (tryInitialFitNow(m_initialFitMarginPx)) {
            m_initialFitPending = false;
            m_initialFitRetryCount = 0;
            return;
        }

        if (m_initialFitRetryCount < 90) {
            ++m_initialFitRetryCount;
            m_initialFitRetryTimer->start();
            return;
        }

        m_initialFitPending = false;
    });
}

QuickCanvasController::~QuickCanvasController() {
    clearLocalDragPreview(false);
    // The controller can be torn down while a pointer is still held. Release
    // the mirrored backend lifecycle before destroying its session state.
    if (m_pointerSession && m_pointerSession->resizeActive()) {
        const auto target = m_mediaItemsById.constFind(m_pointerSession->resizeMediaId());
        if (target != m_mediaItemsById.constEnd() && !target->lifetime.expired()
            && target->item && !target->item->isBeingDeleted()) {
            commitPendingAltResize(target->item);
            target->item->endExternalInteractiveResize();
        }
    }
    delete m_modelPublisher;
    m_modelPublisher = nullptr;
    delete m_pointerSession;
    m_pointerSession = nullptr;
    delete m_snapStore;
    m_snapStore = nullptr;
    delete m_dragSnapSession;
    m_dragSnapSession = nullptr;
}

bool QuickCanvasController::initialize(QWidget* parentWidget, QString* errorMessage) {
    if (m_quickWidget) {
        return true;
    }

    m_quickWidget = new QQuickWidget(parentWidget);
    m_quickWidget->installEventFilter(this);
    m_quickWidget->setAcceptDrops(true);
    m_quickWidget->setResizeMode(QQuickWidget::SizeRootObjectToView);

    // Register custom QML types once before any local or remote engine loads.
    registerCanvasQmlTypes();

    m_quickWidget->setSource(QUrl(QStringLiteral("qrc:/qml/CanvasRoot.qml")));

    if (m_quickWidget->status() != QQuickWidget::Ready || !m_quickWidget->rootObject()) {
        if (errorMessage) {
            QStringList messages;
            const auto qmlErrors = m_quickWidget->errors();
            for (const auto& err : qmlErrors) {
                messages.append(err.toString());
            }
            *errorMessage = messages.isEmpty()
                ? QStringLiteral("Qt Quick canvas root object was not created")
                : messages.join(QStringLiteral(" | "));
        }
        delete m_quickWidget;
        m_quickWidget = nullptr;
        return false;
    }

    m_viewAdapter = new QuickCanvasViewAdapter(m_quickWidget, this);
    m_viewAdapter->initMediaListModel(m_mediaListModel);
    m_quickWidget->rootObject()->setProperty(
        "dropPreviewFrameSource",
        QVariant::fromValue<QObject*>(m_localDragFrameSource));

    setScreenCount(0);
    setShellActive(false);
    setTextToolActive(false);
    setScreens({});
    m_modelPublisher->publishMediaModel(m_viewAdapter, QVariantList{});
    m_modelPublisher->publishSelectionAndSnapModels(m_viewAdapter, QVariantList{}, QVariantList{});

    QObject::connect(
        m_quickWidget->rootObject(), SIGNAL(mediaSelectRequested(QString,bool)),
        this, SLOT(handleMediaSelectRequested(QString,bool)));
    QObject::connect(
        m_quickWidget->rootObject(), SIGNAL(clearSelectionRequested()),
        this, SLOT(handleClearSelectionRequested()));
    QObject::connect(
        m_quickWidget->rootObject(), SIGNAL(dropPreviewContentReady(QString)),
        this, SLOT(handleDropPreviewContentReady(QString)));
    QObject::connect(
        m_quickWidget->rootObject(), SIGNAL(mediaMoveStarted(QString,double,double,bool)),
        this, SLOT(handleMediaMoveStarted(QString,double,double,bool)));
    QObject::connect(
        m_quickWidget->rootObject(), SIGNAL(mediaMoveUpdated(QString,double,double,bool)),
        this, SLOT(handleMediaMoveUpdated(QString,double,double,bool)));
    QObject::connect(
        m_quickWidget->rootObject(), SIGNAL(mediaMoveEnded(QString,double,double,bool)),
        this, SLOT(handleMediaMoveEnded(QString,double,double,bool)));
    QObject::connect(
        m_quickWidget->rootObject(), SIGNAL(mediaResizeRequested(QString,QString,double,double,bool,bool)),
        this, SLOT(handleMediaResizeRequested(QString,QString,double,double,bool,bool)));
    QObject::connect(
        m_quickWidget->rootObject(), SIGNAL(mediaResizeEnded(QString)),
        this, SLOT(handleMediaResizeEnded(QString)));
    QObject::connect(
        m_quickWidget->rootObject(), SIGNAL(textCommitRequested(QString,QString)),
        this, SLOT(handleTextCommitRequested(QString,QString)));
    QObject::connect(
        m_quickWidget->rootObject(), SIGNAL(textLiveUpdateRequested(QString,QString)),
        this, SLOT(handleTextLiveUpdateRequested(QString,QString)));
    QObject::connect(
        m_quickWidget->rootObject(), SIGNAL(textCreateRequested(double,double)),
        this, SLOT(handleTextCreateRequested(double,double)));
    QObject::connect(
        m_quickWidget->rootObject(), SIGNAL(overlayVisibilityToggleRequested(QString,bool)),
        this, SLOT(handleOverlayVisibilityToggle(QString,bool)));
    QObject::connect(
        m_quickWidget->rootObject(), SIGNAL(overlayBringForwardRequested(QString)),
        this, SLOT(handleOverlayBringForward(QString)));
    QObject::connect(
        m_quickWidget->rootObject(), SIGNAL(overlayBringBackwardRequested(QString)),
        this, SLOT(handleOverlayBringBackward(QString)));
    QObject::connect(
        m_quickWidget->rootObject(), SIGNAL(overlayDeleteRequested(QString)),
        this, SLOT(handleOverlayDelete(QString)));
    QObject::connect(
        m_quickWidget->rootObject(), SIGNAL(overlayPlayPauseRequested(QString)),
        this, SLOT(handleOverlayPlayPause(QString)));
    QObject::connect(
        m_quickWidget->rootObject(), SIGNAL(overlayStopRequested(QString)),
        this, SLOT(handleOverlayStop(QString)));
    QObject::connect(
        m_quickWidget->rootObject(), SIGNAL(overlayRepeatToggleRequested(QString)),
        this, SLOT(handleOverlayRepeatToggle(QString)));
    QObject::connect(
        m_quickWidget->rootObject(), SIGNAL(overlayMuteToggleRequested(QString)),
        this, SLOT(handleOverlayMuteToggle(QString)));
    QObject::connect(
        m_quickWidget->rootObject(), SIGNAL(overlayVolumeChangeRequested(QString,double)),
        this, SLOT(handleOverlayVolumeChange(QString,double)));
    // Three-phase seek — begin sets the drag-lock flag before any seek reaches the
    // player, update performs live preview seeks, end commits the final position and
    // releases the lock.  All three use Qt::AutoConnection (synchronous from GUI thread).
    QObject::connect(
        m_quickWidget->rootObject(), SIGNAL(overlaySeekBeginRequested(QString,double)),
        this, SLOT(handleOverlaySeekBegin(QString,double)));
    QObject::connect(
        m_quickWidget->rootObject(), SIGNAL(overlaySeekUpdateRequested(QString,double)),
        this, SLOT(handleOverlaySeekUpdate(QString,double)));
    QObject::connect(
        m_quickWidget->rootObject(), SIGNAL(overlaySeekEndRequested(QString,double)),
        this, SLOT(handleOverlaySeekEnd(QString,double)));
    QObject::connect(
        m_quickWidget->rootObject(), SIGNAL(overlayFitToTextToggleRequested(QString)),
        this, SLOT(handleOverlayFitToTextToggle(QString)));
    QObject::connect(
        m_quickWidget->rootObject(), SIGNAL(overlayHorizontalAlignRequested(QString,QString)),
        this, SLOT(handleOverlayHorizontalAlign(QString,QString)));
    QObject::connect(
        m_quickWidget->rootObject(), SIGNAL(overlayVerticalAlignRequested(QString,QString)),
        this, SLOT(handleOverlayVerticalAlign(QString,QString)));

    m_videoStateTimer->start();

    m_pendingInitialSceneScaleRefresh = true;
    QTimer::singleShot(0, this, [this]() {
        if (m_pendingInitialSceneScaleRefresh) {
            refreshSceneUnitScaleIfNeeded();
        }
    });
    QTimer::singleShot(120, this, [this]() {
        if (m_pendingInitialSceneScaleRefresh) {
            refreshSceneUnitScaleIfNeeded();
        }
    });

    hideRemoteCursor();
    scheduleInitialFitIfNeeded(53);
    return true;
}

bool QuickCanvasController::eventFilter(QObject* watched, QEvent* event) {
    if (watched == m_quickWidget && event) {
        switch (event->type()) {
        case QEvent::Show:
        case QEvent::Polish:
        case QEvent::PolishRequest:
        case QEvent::Resize:
        case QEvent::ScreenChangeInternal:
            refreshSceneUnitScaleIfNeeded();
            scheduleInitialFitIfNeeded(m_initialFitMarginPx);
            break;
        case QEvent::DragEnter: {
            auto* dragEnter = static_cast<QDragEnterEvent*>(event);
            if (remoteSceneLocksEdits()) {
                clearLocalDragPreview(false);
                if (dragEnter) dragEnter->ignore();
                return true;
            }
            QString localPath;
            bool isVideo = false;
            if (dragEnter
                && acceptedSingleLocalMedia(dragEnter->mimeData(), &localPath, &isVideo)) {
                startLocalDragPreview(localPath, isVideo,
                    mapViewPointToScene(dragEnter->position()));
                dragEnter->acceptProposedAction();
                return true;
            }
            clearLocalDragPreview(false);
            if (dragEnter) dragEnter->ignore();
            return true;
        }
        case QEvent::DragLeave: {
            clearLocalDragPreview(true);
            if (auto* dragLeave = static_cast<QDragLeaveEvent*>(event)) {
                dragLeave->accept();
            }
            return true;
        }
        case QEvent::Hide:
        case QEvent::Close:
        case QEvent::WindowDeactivate:
            if (m_localDragAccepted || m_localDragCursorHidden) {
                clearLocalDragPreview(false);
            }
            break;
        case QEvent::DragMove: {
            auto* dragMove = static_cast<QDragMoveEvent*>(event);
            if (remoteSceneLocksEdits() || !m_localDragAccepted) {
                clearLocalDragPreview(false);
                if (dragMove) dragMove->ignore();
                return true;
            }
            // Validation and all file work happen once at DragEnter. Movement is
            // deliberately restricted to two coordinates on the QML model.
            if (dragMove) {
                updateLocalDragPreviewCenter(mapViewPointToScene(dragMove->position()));
                dragMove->acceptProposedAction();
                return true;
            }
            return true;
        }
        case QEvent::Drop: {
            auto* dropEvent = static_cast<QDropEvent*>(event);
            if (remoteSceneLocksEdits() || !m_localDragAccepted) {
                clearLocalDragPreview(false);
                if (dropEvent) dropEvent->ignore();
                return true;
            }
            if (dropEvent) {
                m_localDropPending = true;
                m_localDropSceneCenter = mapViewPointToScene(dropEvent->position());
                m_localDragSceneCenter = m_localDropSceneCenter;
                publishLocalDragPreview(!m_localDragNativeSize.isEmpty());
                if (m_localDragCursorHidden && m_quickWidget) {
                    m_quickWidget->unsetCursor();
                    m_localDragCursorHidden = false;
                }
                maybeCompleteLocalDragPreparation(m_localDragGeneration);
                dropEvent->acceptProposedAction();
                return true;
            }
            return true;
        }
        default:
            break;
        }
    }

    return QObject::eventFilter(watched, event);
}

bool QuickCanvasController::acceptedSingleLocalMedia(const QMimeData* mimeData,
                                                     QString* localPath,
                                                     bool* isVideo) const {
    if (!mimeData || !mimeData->hasUrls()) {
        return false;
    }
    const QList<QUrl> urls = mimeData->urls();
    if (urls.size() != 1 || !urls.first().isLocalFile()) {
        return false;
    }

    const QString path = urls.first().toLocalFile();
    const MediaFilePolicy::Kind kind = MediaFilePolicy::classifyLocalFile(path);
    if (kind != MediaFilePolicy::Kind::Image
        && kind != MediaFilePolicy::Kind::Mp4Video) {
        return false;
    }

    if (localPath) {
        *localPath = QFileInfo(path).canonicalFilePath();
        if (localPath->isEmpty()) {
            *localPath = QFileInfo(path).absoluteFilePath();
        }
    }
    if (isVideo) {
        *isVideo = kind == MediaFilePolicy::Kind::Mp4Video;
    }
    return true;
}

QString QuickCanvasController::localPreviewCacheKey(const QString& localPath) const {
    const QFileInfo info(localPath);
    return QStringLiteral("%1|%2|%3")
        .arg(info.canonicalFilePath().isEmpty() ? info.absoluteFilePath() : info.canonicalFilePath())
        .arg(info.size())
        .arg(info.lastModified().toMSecsSinceEpoch());
}

bool QuickCanvasController::restoreLocalPreviewFromCache(const QString& cacheKey) {
    auto it = m_localPreviewCache.constFind(cacheKey);
    if (it == m_localPreviewCache.constEnd()
        || it->nativeSize.isEmpty() || it->frame.isNull()
        || it->video != m_localDragIsVideo) {
        return false;
    }

    m_localDragNativeSize = it->nativeSize;
    m_localDragFrame = it->frame;
    m_localDragFrameSource->setFrame(m_localDragFrame);
    m_localPreviewCacheLru.removeAll(cacheKey);
    m_localPreviewCacheLru.append(cacheKey);
    return true;
}

void QuickCanvasController::storeLocalPreviewInCache() {
    if (m_localDragCacheKey.isEmpty() || m_localDragNativeSize.isEmpty()
        || m_localDragFrame.isNull()) {
        return;
    }

    constexpr qsizetype kMaximumCacheBytes = 64 * 1024 * 1024;
    constexpr int kMaximumCacheEntries = 16;

    if (auto old = m_localPreviewCache.find(m_localDragCacheKey);
        old != m_localPreviewCache.end()) {
        m_localPreviewCacheBytes -= old->byteCost;
        m_localPreviewCache.erase(old);
    }

    LocalPreviewCacheEntry entry;
    entry.nativeSize = m_localDragNativeSize;
    entry.frame = m_localDragFrame;
    entry.video = m_localDragIsVideo;
    entry.byteCost = std::max<qsizetype>(1, entry.frame.sizeInBytes());
    m_localPreviewCache.insert(m_localDragCacheKey, entry);
    m_localPreviewCacheBytes += entry.byteCost;
    m_localPreviewCacheLru.removeAll(m_localDragCacheKey);
    m_localPreviewCacheLru.append(m_localDragCacheKey);

    while ((!m_localPreviewCacheLru.isEmpty())
           && (m_localPreviewCacheLru.size() > kMaximumCacheEntries
               || m_localPreviewCacheBytes > kMaximumCacheBytes)) {
        const QString oldest = m_localPreviewCacheLru.takeFirst();
        auto old = m_localPreviewCache.find(oldest);
        if (old == m_localPreviewCache.end()) {
            continue;
        }
        m_localPreviewCacheBytes -= old->byteCost;
        m_localPreviewCache.erase(old);
    }
}

void QuickCanvasController::startLocalDragPreview(const QString& localPath,
                                                  bool isVideo,
                                                  const QPointF& sceneCenter) {
    clearLocalDragPreview(false);
    const quint64 generation = ++m_localDragGeneration;

    m_localDragAccepted = true;
    m_localDragIsVideo = isVideo;
    m_localDragPath = localPath;
    m_localDragDisplayName = QFileInfo(localPath).fileName();
    m_localDragCacheKey = localPreviewCacheKey(localPath);
    m_localDragSceneCenter = sceneCenter;
    m_localDropPending = false;
    m_localDropHandoffMediaId.clear();

    if (m_quickWidget && !m_localDragCursorHidden) {
        m_quickWidget->setCursor(Qt::BlankCursor);
        m_localDragCursorHidden = true;
    }

    if (restoreLocalPreviewFromCache(m_localDragCacheKey)) {
        publishLocalDragPreview(true);
        return;
    }

    m_localDragFrameSource->clear();
    publishLocalDragPreview(false);

    if (!isVideo) {
        QImageReader headerReader(localPath);
        headerReader.setAutoTransform(true);
        m_localDragNativeSize = orientedImageSize(headerReader);
        if (m_localDragNativeSize.isEmpty()) {
            failLocalDragPreview(QStringLiteral("Unable to read image dimensions"), generation);
            return;
        }
        publishLocalDragPreview(true);

        auto* watcher = new QFutureWatcher<QImage>(this);
        connect(watcher, &QFutureWatcher<QImage>::finished, this,
                [this, watcher, generation]() {
            const QImage frame = watcher->result();
            watcher->deleteLater();
            if (generation != m_localDragGeneration || !m_localDragAccepted) {
                return;
            }
            if (frame.isNull()) {
                failLocalDragPreview(QStringLiteral("Unable to decode image preview"), generation);
                return;
            }
            m_localDragFrame = frame;
            m_localDragFrameSource->setFrame(frame);
            maybeCompleteLocalDragPreparation(generation);
        });
        watcher->setFuture(QtConcurrent::run([localPath]() {
            return decodeImagePreview(localPath);
        }));
        return;
    }

    auto* sizeWatcher = new QFutureWatcher<QSize>(this);
    connect(sizeWatcher, &QFutureWatcher<QSize>::finished, this,
            [this, sizeWatcher, generation]() {
        const QSize size = sizeWatcher->result();
        sizeWatcher->deleteLater();
        if (generation != m_localDragGeneration || !m_localDragAccepted) {
            return;
        }
        if (m_localDragNativeSize.isEmpty() && !size.isEmpty()) {
            m_localDragNativeSize = size;
        }
        maybeCompleteLocalDragPreparation(generation);
    });
    sizeWatcher->setFuture(QtConcurrent::run([localPath]() {
        return nativeVideoDimensions(localPath);
    }));

    auto* frameWatcher = new QFutureWatcher<QImage>(this);
    connect(frameWatcher, &QFutureWatcher<QImage>::finished, this,
            [this, frameWatcher, generation]() {
        const QImage frame = frameWatcher->result();
        frameWatcher->deleteLater();
        if (generation != m_localDragGeneration || !m_localDragAccepted) {
            return;
        }
        if (m_localDragFrame.isNull() && !frame.isNull()) {
            m_localDragFrame = frame;
            m_localDragFrameSource->setFrame(frame);
        }
        maybeCompleteLocalDragPreparation(generation);
    });
    frameWatcher->setFuture(QtConcurrent::run([localPath]() {
        return nativeVideoFirstFrame(localPath);
    }));

    QTimer::singleShot(150, this, [this, generation]() {
        if (generation == m_localDragGeneration && m_localDragAccepted
            && (m_localDragNativeSize.isEmpty() || m_localDragFrame.isNull())) {
            startVideoPreviewFallback(generation);
        }
    });
    QTimer::singleShot(5000, this, [this, generation]() {
        if (generation == m_localDragGeneration && m_localDragAccepted
            && (m_localDragNativeSize.isEmpty() || m_localDragFrame.isNull())) {
            failLocalDragPreview(QStringLiteral("Unable to read the MP4 dimensions or first frame"),
                                 generation);
        }
    });
}

void QuickCanvasController::startVideoPreviewFallback(quint64 generation) {
    if (generation != m_localDragGeneration || m_localDragFallbackPlayer) {
        return;
    }

    m_localDragFallbackPlayer = new QMediaPlayer(this);
    m_localDragFallbackAudio = new QAudioOutput(m_localDragFallbackPlayer);
    m_localDragFallbackAudio->setMuted(true);
    m_localDragFallbackSink = new QVideoSink(m_localDragFallbackPlayer);
    m_localDragFallbackPlayer->setAudioOutput(m_localDragFallbackAudio);
    m_localDragFallbackPlayer->setVideoSink(m_localDragFallbackSink);

    auto readMetadataSize = [this, generation]() {
        if (generation != m_localDragGeneration || !m_localDragFallbackPlayer
            || !m_localDragNativeSize.isEmpty()) {
            return;
        }
        const QSize size = m_localDragFallbackPlayer->metaData()
                               .value(QMediaMetaData::Resolution).toSize();
        if (!size.isEmpty()) {
            m_localDragNativeSize = size;
            maybeCompleteLocalDragPreparation(generation);
        }
    };
    connect(m_localDragFallbackPlayer, &QMediaPlayer::mediaStatusChanged,
            this, [readMetadataSize](QMediaPlayer::MediaStatus) { readMetadataSize(); });
    connect(m_localDragFallbackPlayer, &QMediaPlayer::metaDataChanged,
            this, readMetadataSize);
    connect(m_localDragFallbackSink, &QVideoSink::videoFrameChanged,
            this, [this, generation](const QVideoFrame& videoFrame) {
        if (generation != m_localDragGeneration || !videoFrame.isValid()
            || !m_localDragFrame.isNull()) {
            return;
        }
        QImage frame = limitPreviewImage(videoFrame.toImage());
        if (frame.isNull()) {
            return;
        }
        m_localDragFrame = frame;
        m_localDragFrameSource->setFrame(frame);
        maybeCompleteLocalDragPreparation(generation);
    });

    m_localDragFallbackPlayer->setSource(QUrl::fromLocalFile(m_localDragPath));
    m_localDragFallbackPlayer->setPosition(0);
    m_localDragFallbackPlayer->play();
}

void QuickCanvasController::stopVideoPreviewFallback() {
    if (m_localDragFallbackPlayer) {
        m_localDragFallbackPlayer->stop();
        m_localDragFallbackPlayer->deleteLater();
    }
    m_localDragFallbackPlayer = nullptr;
    m_localDragFallbackSink = nullptr;
    m_localDragFallbackAudio = nullptr;
}

void QuickCanvasController::updateLocalDragPreviewCenter(const QPointF& sceneCenter) {
    if (!m_localDragAccepted || m_localDropPending) {
        return;
    }
    m_localDragSceneCenter = sceneCenter;
    if (!m_localDragNativeSize.isEmpty()) {
        publishLocalDragPreview(true);
    }
}

void QuickCanvasController::publishLocalDragPreview(bool visible) {
    if (!m_quickWidget || !m_quickWidget->rootObject()) {
        return;
    }

    const qreal unitScale = m_sceneStore && m_sceneStore->sceneUnitScale() > 1e-6
        ? m_sceneStore->sceneUnitScale() : 1.0;
    const QSize size = m_localDragNativeSize;
    const QPointF topLeft = m_localDragSceneCenter
        - QPointF(size.width() * 0.5, size.height() * 0.5);

    QVariantMap preview;
    preview.insert(QStringLiteral("visible"), visible && !size.isEmpty());
    preview.insert(QStringLiteral("pending"), m_localDragAccepted && size.isEmpty());
    const QString phase = !visible || size.isEmpty()
        ? (m_localDragAccepted ? QStringLiteral("preparing") : QStringLiteral("hidden"))
        : (!m_localDropHandoffMediaId.isEmpty()
               ? QStringLiteral("handoff")
               : (m_localDragFrame.isNull()
                      ? QStringLiteral("skeleton")
                      : QStringLiteral("ready")));
    preview.insert(QStringLiteral("phase"), phase);
    preview.insert(QStringLiteral("mediaType"),
                   m_localDragIsVideo ? QStringLiteral("video") : QStringLiteral("image"));
    preview.insert(QStringLiteral("displayName"), m_localDragDisplayName);
    preview.insert(QStringLiteral("x"), topLeft.x() * unitScale);
    preview.insert(QStringLiteral("y"), topLeft.y() * unitScale);
    preview.insert(QStringLiteral("width"), size.width() * unitScale);
    preview.insert(QStringLiteral("height"), size.height() * unitScale);
    preview.insert(QStringLiteral("frameReady"), !m_localDragFrame.isNull());
    preview.insert(QStringLiteral("handoffMediaId"), m_localDropHandoffMediaId);
    m_quickWidget->rootObject()->setProperty("dropPreviewModel", preview);
}

void QuickCanvasController::maybeCompleteLocalDragPreparation(quint64 generation) {
    if (generation != m_localDragGeneration || !m_localDragAccepted) {
        return;
    }
    if (!m_localDragNativeSize.isEmpty()) {
        publishLocalDragPreview(true);
    }
    if (m_localDragNativeSize.isEmpty() || m_localDragFrame.isNull()) {
        return;
    }

    stopVideoPreviewFallback();
    storeLocalPreviewInCache();
    if (m_localDropPending) {
        performPreparedLocalDrop();
    }
}

void QuickCanvasController::performPreparedLocalDrop() {
    if (!m_localDragAccepted || !m_localDropPending
        || m_localDragNativeSize.isEmpty() || m_localDragFrame.isNull()) {
        return;
    }

    m_localDropPending = false;
    m_localDragAccepted = false;
    m_localDragSceneCenter = m_localDropSceneCenter;
    publishLocalDragPreview(true);
    emit preparedLocalFileDropRequested(m_localDragPath,
                                        m_localDragNativeSize,
                                        m_localDragFrame,
                                        m_localDropSceneCenter);

    // The host normally assigns the handoff id synchronously. Treat a missing
    // id as an import failure rather than leaving a permanent preview behind.
    if (m_localDropHandoffMediaId.isEmpty()) {
        failLocalDragPreview(QStringLiteral("The media could not be added to the canvas"),
                             m_localDragGeneration);
    }
}

void QuickCanvasController::beginDropPreviewHandoff(const QString& mediaId) {
    if (mediaId.isEmpty() || m_localDragNativeSize.isEmpty()) {
        return;
    }
    m_localDropHandoffMediaId = mediaId;
    publishLocalDragPreview(true);
}

void QuickCanvasController::handleDropPreviewContentReady(const QString& mediaId) {
    if (mediaId.isEmpty() || mediaId != m_localDropHandoffMediaId) {
        return;
    }
    const quint64 generation = m_localDragGeneration;
    publishLocalDragPreview(false);
    QTimer::singleShot(100, this, [this, generation]() {
        if (generation == m_localDragGeneration) {
            clearLocalDragPreview(false);
        }
    });
}

void QuickCanvasController::failLocalDragPreview(const QString& message,
                                                 quint64 generation) {
    if (generation != m_localDragGeneration) {
        return;
    }
    if (!message.isEmpty()) {
        TOAST_WARNING(message);
    }
    clearLocalDragPreview(true);
}

void QuickCanvasController::clearLocalDragPreview(bool animate, bool restoreCursor) {
    const quint64 generation = ++m_localDragGeneration;
    stopVideoPreviewFallback();

    if (restoreCursor && m_localDragCursorHidden && m_quickWidget) {
        m_quickWidget->unsetCursor();
        m_localDragCursorHidden = false;
    }

    m_localDragAccepted = false;
    m_localDropPending = false;
    if (animate && !m_localDragNativeSize.isEmpty()) {
        publishLocalDragPreview(false);
        QTimer::singleShot(100, this, [this, generation]() {
            if (generation == m_localDragGeneration) {
                clearLocalDragPreview(false);
            }
        });
        return;
    }

    m_localDragPath.clear();
    m_localDragDisplayName.clear();
    m_localDragCacheKey.clear();
    m_localDragNativeSize = {};
    m_localDragFrame = {};
    m_localDragSceneCenter = {};
    m_localDropSceneCenter = {};
    m_localDropHandoffMediaId.clear();
    if (m_localDragFrameSource) {
        m_localDragFrameSource->clear();
    }
    publishLocalDragPreview(false);
}

QWidget* QuickCanvasController::widget() const {
    return m_quickWidget;
}

void QuickCanvasController::setScreenCount(int screenCount) {
    if (!m_quickWidget || !m_quickWidget->rootObject()) {
        return;
    }
    m_quickWidget->rootObject()->setProperty("screenCount", screenCount);
}

void QuickCanvasController::setShellActive(bool active) {
    if (!m_quickWidget || !m_quickWidget->rootObject()) {
        return;
    }
    m_quickWidget->rootObject()->setProperty("remoteActive", active);
}

void QuickCanvasController::setScreens(const QList<ScreenInfo>& screens) {
    if (screenListsEquivalent(m_sceneStore->screens(), screens)) {
        return;
    }

    m_sceneStore->setScreens(screens);
    m_initialFitCompleted = false;
    m_initialFitRetryCount = 0;
    m_initialFitPending = false;
    if (m_initialFitRetryTimer) {
        m_initialFitRetryTimer->stop();
    }
    setScreenCount(screens.size());
    rebuildScreenRects();
    pushStaticLayerModels();
    if (screens.isEmpty()) {
        hideRemoteCursor();
    } else {
        scheduleInitialFitIfNeeded(53);
    }
}

void QuickCanvasController::setMediaScene(QGraphicsScene* scene) {
    if (m_mediaScene == scene) {
        return;
    }

    clearLocalDragPreview(false);

    // Finish the backend-side resize lifecycle while the old scene/index are
    // still available. A detached scene can otherwise leave a surviving text
    // item permanently suppressing its one-time legacy layout synchronization.
    if (m_mediaScene && m_pointerSession->resizeActive()) {
        if (ResizableMediaBase* target = mediaItemById(m_pointerSession->resizeMediaId())) {
            commitPendingAltResize(target);
            target->endExternalInteractiveResize();
        } else {
            clearPendingAltResize();
        }
    }

    if (m_mediaScene) {
        disconnect(m_mediaScene, nullptr, this, nullptr);
    }

    m_mediaScene = scene;
    m_mediaItemsById.clear();

    if (m_mediaScene) {
        ScreenCanvas* screenCanvas = m_mediaScene->views().isEmpty()
            ? nullptr : qobject_cast<ScreenCanvas*>(m_mediaScene->views().first());
        if (screenCanvas && screenCanvas->mediaRuntimeContext()) {
            auto* context = screenCanvas->mediaRuntimeContext();
            const QPointer<QuickCanvasController> self(this);
            context->mediaOpacityAnimationTickNotifier = [self]() {
                if (self) self->handleFadeAnimationTick();
            };
            context->mediaSettingsChangedNotifier = [self](ResizableMediaBase* media) {
                if (self) self->handleMediaSettingsChanged(media);
            };
            for (QGraphicsItem* graphicsItem : m_mediaScene->items()) {
                if (auto* media = dynamic_cast<ResizableMediaBase*>(graphicsItem)) {
                    media->setRuntimeContext(context);
                }
            }
        }
        connect(m_mediaScene, &QGraphicsScene::changed, this, [this](const QList<QRectF>&) {
            if (!m_pointerSession->draggingMedia() && !m_selectionMutationInProgress) {
                scheduleMediaModelSync();
            }
        });
        connect(m_mediaScene, &QGraphicsScene::selectionChanged, this, [this]() {
            if (m_selectionMutationInProgress) {
                return;
            }
            pushSelectionAndSnapModels();
        });
    }

    scheduleMediaModelSync();
}

void QuickCanvasController::rebuildMediaItemIndex() {
    m_mediaItemsById.clear();
    if (!m_mediaScene) {
        return;
    }

    const QList<QGraphicsItem*> sceneItems = m_mediaScene->items();
    for (QGraphicsItem* graphicsItem : sceneItems) {
        auto* media = dynamic_cast<ResizableMediaBase*>(graphicsItem);
        if (!media || media->isBeingDeleted()) {
            continue;
        }
        const QString mediaId = media->mediaId();
        if (mediaId.isEmpty()) {
            continue;
        }
        m_mediaItemsById.insert(mediaId, {media, media->lifetimeGuard()});
    }
}

ResizableMediaBase* QuickCanvasController::mediaItemById(const QString& mediaId) {
    if (!m_mediaScene || mediaId.isEmpty()) {
        return nullptr;
    }

    auto it = m_mediaItemsById.constFind(mediaId);
    if (it != m_mediaItemsById.constEnd()) {
        const MediaItemReference& reference = it.value();
        ResizableMediaBase* candidate = reference.item;
        // All scene/input access is on the GUI thread. Test the token before
        // touching the raw QGraphicsItem, which may have died since publication.
        if (!reference.lifetime.expired() && candidate
            && !candidate->isBeingDeleted() && candidate->scene() == m_mediaScene) {
            return candidate;
        }
    }

    rebuildMediaItemIndex();
    return m_mediaItemsById.value(mediaId).item;
}

bool QuickCanvasController::remoteSceneLocksEdits() const {
    if (!m_mediaScene) {
        return false;
    }
    for (QGraphicsView* view : m_mediaScene->views()) {
        if (auto* canvas = qobject_cast<ScreenCanvas*>(view)) {
            // The remote manifest is immutable from PREPARE until STOP. This
            // also covers the stopping handshake because launched remains true
            // until the matching acknowledgement is accepted.
            if (canvas->isHostSceneActive()
                || canvas->isRemoteSceneLaunching()
                || canvas->isRemoteSceneLaunched()) {
                return true;
            }
        }
    }
    return false;
}

void QuickCanvasController::updateRemoteCursor(int globalX, int globalY) {
    bool ok = false;
    const QPointF mapped = mapRemoteCursorToQuickScene(globalX, globalY, &ok);
    if (!ok) {
        return;
    }
    m_sceneStore->setRemoteCursor(true, mapped.x(), mapped.y());
    pushRemoteCursorState();
}

void QuickCanvasController::hideRemoteCursor() {
    m_sceneStore->setRemoteCursor(false, m_sceneStore->remoteCursorPos().x(), m_sceneStore->remoteCursorPos().y());
    pushRemoteCursorState();
}

void QuickCanvasController::resetView() {
    if (!m_quickWidget || !m_quickWidget->rootObject()) {
        return;
    }
    m_quickWidget->rootObject()->setProperty("viewScale", 1.0);
    m_quickWidget->rootObject()->setProperty("panX", 0.0);
    m_quickWidget->rootObject()->setProperty("panY", 0.0);
}

void QuickCanvasController::recenterView() {
    if (!m_quickWidget || !m_quickWidget->rootObject()) {
        return;
    }

    QRectF bounds;
    bool hasBounds = false;
    for (auto it = m_sceneStore->sceneScreenRects().constBegin(); it != m_sceneStore->sceneScreenRects().constEnd(); ++it) {
        if (!hasBounds) {
            bounds = it.value();
            hasBounds = true;
        } else {
            bounds = bounds.united(it.value());
        }
    }

    if (hasBounds && bounds.width() > 0.0 && bounds.height() > 0.0) {
        QMetaObject::invokeMethod(
            m_quickWidget->rootObject(),
            "fitToBounds",
            Q_ARG(QVariant, bounds.x()),
            Q_ARG(QVariant, bounds.y()),
            Q_ARG(QVariant, bounds.width()),
            Q_ARG(QVariant, bounds.height()),
            Q_ARG(QVariant, 53.0));
        return;
    }

    QMetaObject::invokeMethod(m_quickWidget->rootObject(), "recenterView", Q_ARG(QVariant, 53.0));
}

void QuickCanvasController::ensureInitialFit(int marginPx) {
    if (m_initialFitCompleted) {
        return;
    }

    if (!tryInitialFitNow(marginPx)) {
        scheduleInitialFitIfNeeded(marginPx);
    }
}

void QuickCanvasController::setTextToolActive(bool active) {
    m_textToolActive = active;
    if (!m_quickWidget || !m_quickWidget->rootObject()) {
        return;
    }
    m_quickWidget->rootObject()->setProperty("textToolActive", active);
}

void QuickCanvasController::scheduleMediaModelSync() {
    if (m_mediaSyncPending) {
        return;
    }
    m_mediaSyncPending = true;
    if (m_mediaSyncTimer) {
        m_mediaSyncTimer->start();
    } else {
        syncMediaModelFromScene();
    }
}

void QuickCanvasController::handleMediaSelectRequested(const QString& mediaId, bool additive) {
    if (remoteSceneLocksEdits()) {
        return;
    }
    if (!m_mediaScene || mediaId.isEmpty()) {
        return;
    }

    ResizableMediaBase* target = mediaItemById(mediaId);

    if (!target) {
        return;
    }

    const bool alreadyOnlySelected = target->isSelected()
        && m_mediaScene->selectedItems().size() == 1;
    if (!additive && alreadyOnlySelected) {
        pushSelectionAndSnapModels();
        return;
    }

    if (additive && target->isSelected()) {
        pushSelectionAndSnapModels();
        return;
    }

    // The scene is the sole selection authority, including multi-selection and
    // selection changes initiated outside Quick. Publish the resulting projection
    // once, rather than exposing the intermediate clear during replacement.
    const auto targetLifetime = target->lifetimeGuard();
    {
        QScopedValueRollback<bool> mutationGuard(m_selectionMutationInProgress, true);
        if (!additive) {
            m_mediaScene->clearSelection();
        }
        // Other scene listeners can react synchronously to clearSelection().
        if (!targetLifetime.expired() && !target->isBeingDeleted()
            && target->scene() == m_mediaScene) {
            target->setSelected(true);
        }
    }

    // Never cancel a pending media publication here: it may contain a text edit,
    // fit-to-text resize, style change or newly added item. MediaListModel updates
    // stable delegates in place, so selection needs no destructive-sync workaround.
    pushSelectionAndSnapModels();
}

void QuickCanvasController::handleClearSelectionRequested() {
    if (!m_mediaScene) {
        return;
    }

    if (m_mediaScene->selectedItems().isEmpty()) {
        pushSelectionAndSnapModels();
        return;
    }

    {
        QScopedValueRollback<bool> mutationGuard(m_selectionMutationInProgress, true);
        m_mediaScene->clearSelection();
    }

    // Keep pending content/geometry updates alive, just as on selection.
    pushSelectionAndSnapModels();
}

// Move drag is now fully QML-native (DragHandler on each mediaDelegate inside contentRoot).
// C++ is only notified once at drag-end to commit the final position.
void QuickCanvasController::handleMediaMoveEnded(const QString& mediaId, qreal sceneX, qreal sceneY, bool snap) {
    if (remoteSceneLocksEdits()) {
        m_dragSnapSession->end();
        clearLiveDragSnapPosition();
        m_pointerSession->setDraggingMedia(false);
        if (m_mediaSyncTimer) m_mediaSyncTimer->stop();
        m_mediaSyncPending = false;
        pushMediaModelOnly();
        pushSelectionAndSnapModels();
        return;
    }

    // Commit exactly what was displayed: use the last snapped position that was
    // pushed to QML via pushLiveDragSnapPosition, stored in m_lastSnapSceneX/Y.
    // This avoids re-running the snap engine (which could yield a slightly different
    // result) and guarantees "commit what the user saw on screen".
    // If no snap was active during this drag, fall back to the raw QML position.
    const bool keepSnappedCommit = snap && m_lastSnapWasSnapped;
    qreal finalX = keepSnappedCommit ? m_lastSnapSceneX : sceneX;
    qreal finalY = keepSnappedCommit ? m_lastSnapSceneY : sceneY;

    if (!snap && m_lastSnapWasSnapped) {
        clearLiveDragSnapPosition();
    }

    // End the snap session. The freeze stays alive until QML's onMediaChanged fires,
    // which happens when pushMediaModelOnly() publishes the updated mediaModel below.
    m_dragSnapSession->end();

    if (!m_mediaScene || mediaId.isEmpty()) {
        clearLiveDragSnapPosition();
        if (m_mediaSyncTimer) {
            m_mediaSyncTimer->stop();
        }
        m_mediaSyncPending = false;
        syncMediaModelFromScene();
        return;
    }

    m_pointerSession->setDraggingMedia(true);
    ResizableMediaBase* movedTarget = mediaItemById(mediaId);
    if (movedTarget) {
        movedTarget->suppressNextItemPositionSnap();
        movedTarget->setPos(QPointF(finalX, finalY));
    }
    m_pointerSession->setDraggingMedia(false);

    if (m_mediaSyncTimer) {
        m_mediaSyncTimer->stop();
    }
    m_mediaSyncPending = false;

    // Push the full model from authoritative C++ scene state.
    // setPos was called above so media->scenePos() already returns finalX/Y.
    // pushMediaModelOnly triggers onMediaChanged on the delegate which syncs
    // localX/Y to the committed position and clears the snap freeze in the
    // correct order (localX updated first, then freeze cleared).
    pushMediaModelOnly();
    pushSelectionAndSnapModels();
}

void QuickCanvasController::handleMediaMoveStarted(const QString& mediaId, qreal sceneX, qreal sceneY, bool snap) {
    Q_UNUSED(sceneX)
    Q_UNUSED(sceneY)
    Q_UNUSED(snap)
    // Always clear any stale snap state from a previous session before starting a new one.
    // clearLiveDragSnapPosition also resets m_lastSnapWasSnapped/X/Y.
    clearLiveDragSnapPosition();
    m_modelPublisher->publishSnapGuidesOnly(m_viewAdapter, SnapGuidePublisher::emptyModel());

    if (remoteSceneLocksEdits()) {
        return;
    }

    if (!m_mediaScene || mediaId.isEmpty()) {
        return;
    }
    ResizableMediaBase* item = mediaItemById(mediaId);
    if (!item || m_mediaScene->views().isEmpty()) {
        return;
    }
    syncSnapViewScale();
    auto* sc = qobject_cast<ScreenCanvas*>(m_mediaScene->views().first());
    m_dragSnapSession->begin(item, m_mediaScene, m_sceneStore->sceneScreenRects(), sc);
}

void QuickCanvasController::handleMediaMoveUpdated(const QString& mediaId, qreal sceneX, qreal sceneY, bool snap) {
    if (remoteSceneLocksEdits()) {
        return;
    }
    if (!m_mediaScene || mediaId.isEmpty()) {
        return;
    }
    if (!m_dragSnapSession->active()) {
        return;
    }
    syncSnapViewScale();
    const DragSnapResult result = m_dragSnapSession->update(QPointF(sceneX, sceneY), snap);
    if (result.snapped) {
        pushLiveDragSnapPosition(mediaId, result.snappedPos.x(), result.snappedPos.y());
        const qreal sceneUnitScale = (m_sceneStore->sceneUnitScale() > 1e-6) ? m_sceneStore->sceneUnitScale() : 1.0;
        m_modelPublisher->publishSnapGuidesOnly(
            m_viewAdapter,
            SnapGuidePublisher::buildFromLines(result.guideLines, sceneUnitScale));
    } else {
        clearLiveDragSnapPosition();
        m_modelPublisher->publishSnapGuidesOnly(m_viewAdapter, SnapGuidePublisher::emptyModel());
    }
}

void QuickCanvasController::handleMediaResizeRequested(const QString& mediaId,
                                                       const QString& handleId,
                                                       qreal sceneX,
                                                       qreal sceneY,
                                                       bool snap,
                                                       bool altPressed) {
    if (remoteSceneLocksEdits()) {
        m_hasQueuedResize = false;
        if (m_resizeDispatchTimer) m_resizeDispatchTimer->stop();
        return;
    }

    if (!m_executingQueuedResize) {
        m_queuedResizeMediaId = mediaId;
        m_queuedResizeHandleId = handleId;
        m_queuedResizeSceneX = sceneX;
        m_queuedResizeSceneY = sceneY;
        m_queuedResizeSnap = snap;
        m_queuedResizeAlt = altPressed;
        m_hasQueuedResize = true;
        if (m_resizeDispatchTimer && !m_resizeDispatchTimer->isActive()) {
            m_resizeDispatchTimer->start();
        }
        return;
    }

    if (!m_mediaScene || mediaId.isEmpty() || handleId.isEmpty()) {
        m_pointerSession->setDraggingMedia(false);
        return;
    }

    if (m_pointerSession->resizeActive()) {
        if (mediaId != m_pointerSession->resizeMediaId()
            || handleId != m_pointerSession->resizeHandleId()) {
            return;
        }
    } else {
        m_pointerSession->beginResize(mediaId, handleId);
        m_resizeBaseSize = QSize();
        m_resizeFixedItemPoint = QPointF();
        m_resizeFixedScenePoint = QPointF();
        m_uniformCornerSnapActive = false;
        m_uniformCornerSnapHandle = static_cast<int>(ResizableMediaBase::None);
        m_uniformCornerSnapScale = 1.0;
        m_snapStore->clear();
        // Clear any stale snap drag freeze from a prior drag that wasn't fully resolved.
        clearLiveDragSnapPosition();
        if (m_mediaSyncTimer) {
            m_mediaSyncTimer->stop();
        }
        m_mediaSyncPending = false;
    }

    m_pointerSession->setDraggingMedia(true);

    ResizableMediaBase* target = mediaItemById(mediaId);
    if (!target) {
        m_pointerSession->setDraggingMedia(false);
        endLiveResizeSession(m_pointerSession->resizeMediaId(), m_resizeLastSceneX, m_resizeLastSceneY, m_resizeLastScale);
        m_pointerSession->clearResize();
        m_resizeBaseSize = QSize();
        m_resizeFixedItemPoint = QPointF();
        m_resizeFixedScenePoint = QPointF();
        resetAltResizeState();
        return;
    }

    const QSize baseSizeNow = target->baseSizePx();
    if (baseSizeNow.width() <= 0 || baseSizeNow.height() <= 0) {
        m_pointerSession->setDraggingMedia(false);
        endLiveResizeSession(m_pointerSession->resizeMediaId(), m_resizeLastSceneX, m_resizeLastSceneY, m_resizeLastScale);
        m_pointerSession->clearResize();
        m_resizeBaseSize = QSize();
        m_resizeFixedItemPoint = QPointF();
        m_resizeFixedScenePoint = QPointF();
        resetAltResizeState();
        return;
    }

    ResizableMediaBase::Handle activeHandle = ResizableMediaBase::None;
    QPointF fixedItemPoint;

    if (handleId == QStringLiteral("top-left")) {
        activeHandle = ResizableMediaBase::TopLeft;
        fixedItemPoint = QPointF(baseSizeNow.width(), baseSizeNow.height());
    } else if (handleId == QStringLiteral("top-mid")) {
        activeHandle = ResizableMediaBase::TopMid;
        fixedItemPoint = QPointF(baseSizeNow.width() * 0.5, baseSizeNow.height());
    } else if (handleId == QStringLiteral("top-right")) {
        activeHandle = ResizableMediaBase::TopRight;
        fixedItemPoint = QPointF(0.0, baseSizeNow.height());
    } else if (handleId == QStringLiteral("left-mid")) {
        activeHandle = ResizableMediaBase::LeftMid;
        fixedItemPoint = QPointF(baseSizeNow.width(), baseSizeNow.height() * 0.5);
    } else if (handleId == QStringLiteral("right-mid")) {
        activeHandle = ResizableMediaBase::RightMid;
        fixedItemPoint = QPointF(0.0, baseSizeNow.height() * 0.5);
    } else if (handleId == QStringLiteral("bottom-left")) {
        activeHandle = ResizableMediaBase::BottomLeft;
        fixedItemPoint = QPointF(baseSizeNow.width(), 0.0);
    } else if (handleId == QStringLiteral("bottom-mid")) {
        activeHandle = ResizableMediaBase::BottomMid;
        fixedItemPoint = QPointF(baseSizeNow.width() * 0.5, 0.0);
    } else if (handleId == QStringLiteral("bottom-right")) {
        activeHandle = ResizableMediaBase::BottomRight;
        fixedItemPoint = QPointF(0.0, 0.0);
    } else {
        m_pointerSession->setDraggingMedia(false);
        endLiveResizeSession(m_pointerSession->resizeMediaId(), m_resizeLastSceneX, m_resizeLastSceneY, m_resizeLastScale);
        m_pointerSession->clearResize();
        m_resizeBaseSize = QSize();
        m_resizeFixedItemPoint = QPointF();
        m_resizeFixedScenePoint = QPointF();
        resetAltResizeState();
        return;
    }

    if (m_resizeBaseSize.isEmpty()) {
        clearPendingAltResize();
        target->beginExternalInteractiveResize();
        m_resizeBaseSize = baseSizeNow;
        m_resizeFixedItemPoint = fixedItemPoint;
        const qreal currentScaleAtStart = std::abs(target->scale()) > 1e-6 ? std::abs(target->scale()) : 1.0;
        m_resizeFixedScenePoint = target->scenePos() + m_resizeFixedItemPoint * currentScaleAtStart;
        m_resizeLastSceneX = target->scenePos().x();
        m_resizeLastSceneY = target->scenePos().y();
        m_resizeLastScale = currentScaleAtStart;
        // Only begin the live-resize session (sets liveResizeActive in QML) when NOT
        // starting an alt-resize. Alt-resize uses liveAltResizeActive exclusively;
        // setting liveResizeActive with stale x/y/scale=0/0/1 causes a one-frame
        // snap to the wrong position before liveAltResizeActive takes over.
        if (!altPressed) {
            beginLiveResizeSession(mediaId);
        }
        buildResizeSnapCaches(target);
    }

    // Detect Alt mode transitions within a session
    const bool wasAlt = m_lastResizeWasAlt;
    m_lastResizeWasAlt = altPressed;

    // If Alt was released, reset alt capture state so next Alt press re-captures correctly.
    // Also rebase uniform-scale metrics from current item state.
    if (wasAlt && !altPressed) {
        // The QML delegate has owned non-uniform geometry so far. Commit its
        // last value once, then rebase the uniform path from that exact result.
        commitPendingAltResize(target);
        m_altAxisCaptured   = false;
        m_altCornerCaptured = false;
        // Rebase uniform resize metrics from current item geometry
        const qreal s = std::abs(target->scale()) > 1e-6 ? std::abs(target->scale()) : 1.0;
        m_resizeBaseSize = target->baseSizePx();
        m_resizeFixedItemPoint = computeHandleItemPoint(static_cast<int>(activeHandle), m_resizeBaseSize);
        m_resizeFixedScenePoint = target->scenePos() + m_resizeFixedItemPoint * s;
        m_resizeLastScale = s;
        pushMediaModelOnly();
        beginLiveResizeSession(mediaId);
    }

    const bool axisHandle   = isAxisHandle(static_cast<int>(activeHandle));
    const bool cornerHandle = isCornerHandle(static_cast<int>(activeHandle));

    // ── ALT-RESIZE PATH ─────────────────────────────────────────────────────
    if (altPressed && (axisHandle || cornerHandle)) {
        const qreal currentScale = std::abs(target->scale()) > 1e-6 ? std::abs(target->scale()) : 1.0;

        if (axisHandle) {
            // ── AXIS ALT: stretch one dimension ──────────────────────────
            const bool horizontal = (activeHandle == ResizableMediaBase::LeftMid
                                  || activeHandle == ResizableMediaBase::RightMid);

            if (!m_altAxisCaptured) {
                // First tick: engage alt mode (disables fitToText for text items)
                const bool derivedHandlesBaking = target->beginAltResizeMode();

                // Bake scale into base size unless derived class handles it
                if (!derivedHandlesBaking) {
                    const qreal s = currentScale;
                    if (std::abs(s - 1.0) > 1e-9) {
                        QSize baked = target->baseSizePx();
                        baked.setWidth(std::max(1, int(std::round(baked.width()  * s))));
                        baked.setHeight(std::max(1, int(std::round(baked.height() * s))));
                        target->setBaseSizePx(baked);
                        target->setScale(1.0);
                    }
                }

                m_altOrigBaseSize = target->baseSizePx();

                // Re-evaluate fixed scene point after possible bake
                const qreal s2 = std::abs(target->scale()) > 1e-6 ? std::abs(target->scale()) : 1.0;
                m_resizeFixedItemPoint = computeHandleItemPoint(static_cast<int>(
                    activeHandle == ResizableMediaBase::LeftMid  ? ResizableMediaBase::RightMid :
                    activeHandle == ResizableMediaBase::RightMid ? ResizableMediaBase::LeftMid  :
                    activeHandle == ResizableMediaBase::TopMid   ? ResizableMediaBase::BottomMid :
                                                                   ResizableMediaBase::TopMid),
                    m_altOrigBaseSize);
                m_altFixedScenePoint = target->scenePos() + m_resizeFixedItemPoint * s2;

                // Capture cursor-to-moving-edge initial offset
                const QSize bs = m_altOrigBaseSize;
                const QPointF movingEdgeItem = computeHandleItemPoint(static_cast<int>(activeHandle), bs);
                const QPointF movingEdgeScene = target->scenePos() + movingEdgeItem * s2;
                qreal cursorToEdge = horizontal
                    ? (sceneX - movingEdgeScene.x())
                    : (sceneY - movingEdgeScene.y());
                // Normalize: outward direction is positive
                if (activeHandle == ResizableMediaBase::LeftMid || activeHandle == ResizableMediaBase::TopMid)
                    cursorToEdge = -cursorToEdge;
                m_altAxisInitialOffset = cursorToEdge;
                m_altAxisCaptured = true;

                // On the first tick, beginAltResizeMode() may have changed backend
                // state (e.g. fitToTextEnabled=false for text items). Push the full
                // model now so QML picks up those changes immediately. The live
                // geometry override (liveAltResizeActive=true) ensures the delegate
                // ignores the model's stale width/height, so there is no visual jump.
                pushMediaModelOnly();
            }

            // Compute desired new size along the axis from the fixed anchor (absolute formula).
            // This avoids feedback against already-quantized geometry and keeps motion smooth.
            const qreal s = std::abs(target->scale()) > 1e-6 ? std::abs(target->scale()) : 1.0;
            const QSize bs = m_altOrigBaseSize;
            const bool negativeDirection = (activeHandle == ResizableMediaBase::LeftMid
                                         || activeHandle == ResizableMediaBase::TopMid);
            const qreal dir = negativeDirection ? -1.0 : 1.0;
            const qreal pointerAlongAxis = horizontal ? sceneX : sceneY;
            const qreal fixedAlongAxis = horizontal ? m_altFixedScenePoint.x() : m_altFixedScenePoint.y();
            qreal desiredAxisSize = dir * (pointerAlongAxis - fixedAlongAxis) - m_altAxisInitialOffset;
            desiredAxisSize = std::max<qreal>(1.0, desiredAxisSize);

            // Apply snap if requested
            if (snap && !m_mediaScene->views().isEmpty()) {
            if (auto* sc = qobject_cast<ScreenCanvas*>(m_mediaScene->views().first())) {
                    syncSnapViewScale();
                    const qreal origSize = horizontal ? bs.width() : bs.height();
                    qreal eqScale = (origSize > 0) ? (desiredAxisSize / origSize) : 1.0;
                    eqScale = std::clamp<qreal>(eqScale, 0.05, 100.0);
                    const auto axisSnap = applyAxisSnapWithCachedTargets(
                        target, eqScale, m_altFixedScenePoint, bs, activeHandle, snap, sc);
                    const qreal snappedScale = axisSnap.scale;
                    desiredAxisSize = snappedScale * origSize;
                    // Build snap guide line only when the snap engine actually engaged a target.
                    const qreal sceneUnitScale2 = (m_sceneStore->sceneUnitScale() > 1e-6) ? m_sceneStore->sceneUnitScale() : 1.0;
                    if (axisSnap.snapped) {
                        QVector<QLineF> altAxisLines;
                        if (horizontal) {
                            altAxisLines.append(QLineF(axisSnap.snappedEdgeScenePos, -1e6,
                                                       axisSnap.snappedEdgeScenePos, 1e6));
                        } else {
                            altAxisLines.append(QLineF(-1e6, axisSnap.snappedEdgeScenePos,
                                                       1e6, axisSnap.snappedEdgeScenePos));
                        }
                        m_modelPublisher->publishSnapGuidesOnly(
                            m_viewAdapter,
                            SnapGuidePublisher::buildFromLines(altAxisLines, sceneUnitScale2));
                    } else {
                        m_modelPublisher->publishSnapGuidesOnly(m_viewAdapter, SnapGuidePublisher::emptyModel());
                    }
                }
            }

            // QML already renders the live size. Stage only the final backend
            // geometry so QGraphicsScene indexing and hidden legacy overlays
            // do not run for every pointer tick.
            QSize newBase = bs;
            if (horizontal)
                newBase.setWidth(std::max(1, int(std::round(desiredAxisSize / s))));
            else
                newBase.setHeight(std::max(1, int(std::round(desiredAxisSize / s))));
            // Reposition to keep fixed side anchored
            const QPointF newFixedItem = computeHandleItemPoint(static_cast<int>(
                    activeHandle == ResizableMediaBase::LeftMid  ? ResizableMediaBase::RightMid :
                    activeHandle == ResizableMediaBase::RightMid ? ResizableMediaBase::LeftMid  :
                    activeHandle == ResizableMediaBase::TopMid   ? ResizableMediaBase::BottomMid :
                                                                   ResizableMediaBase::TopMid),
                newBase);
            const QPointF newPos = m_altFixedScenePoint - newFixedItem * s;
            stagePendingAltResize(mediaId, newBase, newPos);

            m_resizeLastSceneX = newPos.x();
            m_resizeLastSceneY = newPos.y();
            m_resizeLastScale  = s;

            const qreal unitScale = (m_sceneStore->sceneUnitScale() > 1e-6) ? m_sceneStore->sceneUnitScale() : 1.0;
            const qreal liveBaseW = horizontal ? std::max<qreal>(1.0, desiredAxisSize / s)
                                               : static_cast<qreal>(bs.width());
            const qreal liveBaseH = horizontal ? static_cast<qreal>(bs.height())
                                               : std::max<qreal>(1.0, desiredAxisSize / s);
            const qreal liveSceneW = liveBaseW * s;
            const qreal liveSceneH = liveBaseH * s;
            const qreal liveSceneX = horizontal
                ? ((activeHandle == ResizableMediaBase::LeftMid) ? (m_altFixedScenePoint.x() - liveSceneW)
                                                                 : m_altFixedScenePoint.x())
                : (m_altFixedScenePoint.x() - liveSceneW * 0.5);
            const qreal liveSceneY = horizontal
                ? (m_altFixedScenePoint.y() - liveSceneH * 0.5)
                : ((activeHandle == ResizableMediaBase::TopMid) ? (m_altFixedScenePoint.y() - liveSceneH)
                                                                : m_altFixedScenePoint.y());

            if (!pushLiveAltResizeGeometry(mediaId,
                    liveSceneX * unitScale, liveSceneY * unitScale,
                    liveBaseW * unitScale,
                    liveBaseH * unitScale,
                    s)) {
                // Exceptional fallback for an unavailable QML live surface.
                commitPendingAltResize(target);
                pushMediaModelOnly();
            }
            return;

        } else {
            // ── CORNER ALT: stretch both dimensions independently ──────
            if (!m_altCornerCaptured) {
                const bool derivedHandlesBaking = target->beginAltResizeMode();

                // Bake scale into base size unless derived class handles it
                if (!derivedHandlesBaking) {
                    const qreal s = currentScale;
                    if (std::abs(s - 1.0) > 1e-9) {
                        QSize baked = target->baseSizePx();
                        baked.setWidth(std::max(1, int(std::round(baked.width()  * s))));
                        baked.setHeight(std::max(1, int(std::round(baked.height() * s))));
                        target->setBaseSizePx(baked);
                        target->setScale(1.0);
                    }
                }

                m_altOrigBaseSize = target->baseSizePx();
                const qreal s2 = std::abs(target->scale()) > 1e-6 ? std::abs(target->scale()) : 1.0;

                // On the first tick, beginAltResizeMode() may have changed backend
                // state (e.g. fitToTextEnabled=false for text items). Push the full
                // model now so QML picks up those changes immediately. The live
                // geometry override (liveAltResizeActive=true) ensures the delegate
                // ignores the model's stale width/height, so there is no visual jump.
                pushMediaModelOnly();

                // Fixed corner is opposite to the dragged corner
                using H = ResizableMediaBase;
                H::Handle oppHandle = (activeHandle == H::TopLeft)     ? H::BottomRight :
                                      (activeHandle == H::TopRight)    ? H::BottomLeft  :
                                      (activeHandle == H::BottomLeft)  ? H::TopRight    :
                                                                         H::TopLeft;
                m_resizeFixedItemPoint = computeHandleItemPoint(static_cast<int>(oppHandle), m_altOrigBaseSize);
                m_altFixedScenePoint   = target->scenePos() + m_resizeFixedItemPoint * s2;

                // Capture initial cursor-to-moving-corner offsets
                const QPointF movingCornerItem  = computeHandleItemPoint(static_cast<int>(activeHandle), m_altOrigBaseSize);
                const QPointF movingCornerScene = target->scenePos() + movingCornerItem * s2;
                qreal dxRaw = sceneX - movingCornerScene.x();
                qreal dyRaw = sceneY - movingCornerScene.y();
                // Normalize outward direction
                if (activeHandle == H::TopLeft  || activeHandle == H::BottomLeft)  dxRaw = -dxRaw;
                if (activeHandle == H::TopLeft  || activeHandle == H::TopRight)    dyRaw = -dyRaw;
                m_altCornerInitialOffsetX = dxRaw;
                m_altCornerInitialOffsetY = dyRaw;
                m_altCornerCaptured = true;
            }

            const qreal s = std::abs(target->scale()) > 1e-6 ? std::abs(target->scale()) : 1.0;
            const QSize bs = m_altOrigBaseSize;

            using H = ResizableMediaBase;
            const qreal dirX = (activeHandle == H::TopLeft || activeHandle == H::BottomLeft) ? -1.0 : 1.0;
            const qreal dirY = (activeHandle == H::TopLeft || activeHandle == H::TopRight) ? -1.0 : 1.0;
            qreal desiredW = dirX * (sceneX - m_altFixedScenePoint.x()) - m_altCornerInitialOffsetX;
            qreal desiredH = dirY * (sceneY - m_altFixedScenePoint.y()) - m_altCornerInitialOffsetY;
            desiredW = std::max<qreal>(1.0, desiredW);
            desiredH = std::max<qreal>(1.0, desiredH);

            // Apply snap if requested
            SnapEngine::CornerSnapResult altCornerSnapResult;
            if (snap && !m_mediaScene->views().isEmpty()) {
                if (auto* sc = qobject_cast<ScreenCanvas*>(m_mediaScene->views().first())) {
                    syncSnapViewScale();
                    altCornerSnapResult = applyCornerSnapWithCachedTargets(
                        static_cast<int>(activeHandle), m_altFixedScenePoint,
                        desiredW, desiredH, snap, sc);
                    if (altCornerSnapResult.snapped) {
                        desiredW = altCornerSnapResult.snappedW;
                        desiredH = altCornerSnapResult.snappedH;
                    }
                }
            }

            // Stage both axes without touching the legacy scene per tick.
            QSize newBase = bs;
            newBase.setWidth(std::max(1,  int(std::round(desiredW / s))));
            newBase.setHeight(std::max(1, int(std::round(desiredH / s))));
            // Reposition to keep fixed corner anchored
            using H2 = ResizableMediaBase;
            const auto oppHandle2 = (activeHandle == static_cast<int>(ResizableMediaBase::TopLeft))
                ? H2::BottomRight : (activeHandle == static_cast<int>(ResizableMediaBase::TopRight))
                ? H2::BottomLeft : (activeHandle == static_cast<int>(ResizableMediaBase::BottomLeft))
                ? H2::TopRight
                : H2::TopLeft;
            const QPointF newFixedItem = computeHandleItemPoint(static_cast<int>(oppHandle2), newBase);
            const QPointF newPos = m_altFixedScenePoint - newFixedItem * s;
            stagePendingAltResize(mediaId, newBase, newPos);

            m_resizeLastSceneX = newPos.x();
            m_resizeLastSceneY = newPos.y();
            m_resizeLastScale  = s;

            const qreal unitScale = (m_sceneStore->sceneUnitScale() > 1e-6) ? m_sceneStore->sceneUnitScale() : 1.0;
            const qreal liveBaseW = std::max<qreal>(1.0, desiredW / s);
            const qreal liveBaseH = std::max<qreal>(1.0, desiredH / s);
            const qreal liveSceneW = liveBaseW * s;
            const qreal liveSceneH = liveBaseH * s;
            const qreal liveSceneX = (activeHandle == H::TopLeft || activeHandle == H::BottomLeft)
                ? (m_altFixedScenePoint.x() - liveSceneW)
                : m_altFixedScenePoint.x();
            const qreal liveSceneY = (activeHandle == H::TopLeft || activeHandle == H::TopRight)
                ? (m_altFixedScenePoint.y() - liveSceneH)
                : m_altFixedScenePoint.y();

            if (!pushLiveAltResizeGeometry(mediaId,
                    liveSceneX * unitScale, liveSceneY * unitScale,
                    liveBaseW * unitScale,
                    liveBaseH * unitScale,
                    s)) {
                commitPendingAltResize(target);
                pushMediaModelOnly();
            }
            if (snap) {
                const qreal unitScaleAC = (m_sceneStore->sceneUnitScale() > 1e-6) ? m_sceneStore->sceneUnitScale() : 1.0;
                if (altCornerSnapResult.snapped) {
                    QVector<QLineF> lines;
                    switch (altCornerSnapResult.kind) {
                        case SnapEngine::CornerSnapKind::Corner:
                        case SnapEngine::CornerSnapKind::EdgeXY:
                            lines.append(QLineF(altCornerSnapResult.snappedEdgeX, -1e6, altCornerSnapResult.snappedEdgeX,  1e6));
                            lines.append(QLineF(-1e6, altCornerSnapResult.snappedEdgeY,  1e6, altCornerSnapResult.snappedEdgeY));
                            break;
                        case SnapEngine::CornerSnapKind::EdgeX:
                            lines.append(QLineF(altCornerSnapResult.snappedEdgeX, -1e6, altCornerSnapResult.snappedEdgeX,  1e6));
                            break;
                        case SnapEngine::CornerSnapKind::EdgeY:
                            lines.append(QLineF(-1e6, altCornerSnapResult.snappedEdgeY,  1e6, altCornerSnapResult.snappedEdgeY));
                            break;
                        case SnapEngine::CornerSnapKind::None:
                        default:
                            break;
                    }
                    m_modelPublisher->publishSnapGuidesOnly(m_viewAdapter, SnapGuidePublisher::buildFromLines(lines, unitScaleAC));
                } else {
                    m_modelPublisher->publishSnapGuidesOnly(m_viewAdapter, SnapGuidePublisher::emptyModel());
                }
            } else {
                m_modelPublisher->publishSnapGuidesOnly(m_viewAdapter, SnapGuidePublisher::emptyModel());
            }
            return;
        }
    }

    // ── UNIFORM SCALE PATH ───────────────────────────────────────────────────
    const QSize baseSize = m_resizeBaseSize;
    fixedItemPoint = m_resizeFixedItemPoint;
    const QPointF fixedScenePoint = m_resizeFixedScenePoint;

    const qreal currentScale = std::abs(target->scale()) > 1e-6 ? std::abs(target->scale()) : 1.0;
    const QPointF movingScenePoint(sceneX, sceneY);

    qreal proposedScale = currentScale;
    if (activeHandle == ResizableMediaBase::LeftMid || activeHandle == ResizableMediaBase::RightMid) {
        const qreal dx = std::abs(movingScenePoint.x() - fixedScenePoint.x());
        proposedScale = std::max<qreal>(0.05, dx / std::max<qreal>(1.0, baseSize.width()));
    } else if (activeHandle == ResizableMediaBase::TopMid || activeHandle == ResizableMediaBase::BottomMid) {
        const qreal dy = std::abs(movingScenePoint.y() - fixedScenePoint.y());
        proposedScale = std::max<qreal>(0.05, dy / std::max<qreal>(1.0, baseSize.height()));
    } else {
        const qreal dx = std::abs(movingScenePoint.x() - fixedScenePoint.x());
        const qreal dy = std::abs(movingScenePoint.y() - fixedScenePoint.y());
        const qreal sx = dx / std::max<qreal>(1.0, baseSize.width());
        const qreal sy = dy / std::max<qreal>(1.0, baseSize.height());
        proposedScale = std::max<qreal>(0.05, std::max(sx, sy));
    }

    // Snap — axis handles: resize to border. Corner handles: snap to corner with lock/hysteresis.
    // While corner snap is active, both position and scale are frozen to avoid size jitter.
    m_uniformCornerSnapped = false;
    bool m_uniformAxisSnapped = false;
    qreal m_uniformAxisSnappedEdge = 0.0;
    if (!snap) {
        m_uniformCornerSnapActive = false;
        m_uniformCornerSnapHandle = static_cast<int>(ResizableMediaBase::None);
        m_uniformCornerSnapScale = 1.0;
    }
    if (snap && !m_mediaScene->views().isEmpty()) {
        if (auto* screenCanvas = qobject_cast<ScreenCanvas*>(m_mediaScene->views().first())) {
            syncSnapViewScale();
            if (activeHandle == ResizableMediaBase::LeftMid || activeHandle == ResizableMediaBase::RightMid
                || activeHandle == ResizableMediaBase::TopMid || activeHandle == ResizableMediaBase::BottomMid) {
                m_uniformCornerSnapActive = false;
                m_uniformCornerSnapHandle = static_cast<int>(ResizableMediaBase::None);
                m_uniformCornerSnapScale = 1.0;
                const auto axisSnap = applyAxisSnapWithCachedTargets(
                    target, proposedScale, fixedScenePoint, baseSize, activeHandle, snap, screenCanvas);
                proposedScale = axisSnap.scale;
                m_uniformAxisSnapped = axisSnap.snapped;
                m_uniformAxisSnappedEdge = axisSnap.snappedEdgeScenePos;
            } else {
                proposedScale = std::clamp<qreal>(proposedScale, 0.05, 100.0);
                const QPointF unsnappedTopLeft = fixedScenePoint - fixedItemPoint * proposedScale;
                QPointF movingCornerScene;
                switch (activeHandle) {
                    case ResizableMediaBase::TopLeft:
                        movingCornerScene = unsnappedTopLeft;
                        break;
                    case ResizableMediaBase::TopRight:
                        movingCornerScene = unsnappedTopLeft + QPointF(baseSize.width() * proposedScale, 0);
                        break;
                    case ResizableMediaBase::BottomLeft:
                        movingCornerScene = unsnappedTopLeft + QPointF(0, baseSize.height() * proposedScale);
                        break;
                    case ResizableMediaBase::BottomRight:
                    default:
                        movingCornerScene = unsnappedTopLeft + QPointF(baseSize.width() * proposedScale,
                                                                        baseSize.height() * proposedScale);
                        break;
                }
                const qreal cornerZone = screenCanvas->cornerSnapDistancePx() / screenCanvas->effectiveViewScale();
                const qreal releaseZone = cornerZone * 1.4;

                if (m_uniformCornerSnapActive
                    && m_uniformCornerSnapHandle == static_cast<int>(activeHandle)) {
                    const qreal dx = movingCornerScene.x() - m_uniformCornerSnappedPt.x();
                    const qreal dy = movingCornerScene.y() - m_uniformCornerSnappedPt.y();
                    const qreal distToLocked = std::hypot(dx, dy);
                    if (distToLocked <= releaseZone) {
                        proposedScale = m_uniformCornerSnapScale;
                        m_uniformCornerSnapped = true;
                    } else {
                        m_uniformCornerSnapActive = false;
                        m_uniformCornerSnapHandle = static_cast<int>(ResizableMediaBase::None);
                        m_uniformCornerSnapScale = 1.0;
                    }
                }

                if (!m_uniformCornerSnapActive) {
                    qreal bestErr = std::numeric_limits<qreal>::max();
                    QPointF bestTarget;
                    for (const QPointF& targetCorner : m_snapStore->corners()) {
                        const qreal dx = std::abs(movingCornerScene.x() - targetCorner.x());
                        const qreal dy = std::abs(movingCornerScene.y() - targetCorner.y());
                        if (dx > cornerZone || dy > cornerZone) {
                            continue;
                        }
                        const qreal err = std::hypot(dx, dy);
                        if (err < bestErr) {
                            bestErr = err;
                            bestTarget = targetCorner;
                        }
                    }

                    if (bestErr < std::numeric_limits<qreal>::max()) {
                        const qreal baseW = std::max<qreal>(1.0, static_cast<qreal>(baseSize.width()));
                        const qreal baseH = std::max<qreal>(1.0, static_cast<qreal>(baseSize.height()));
                        const qreal targetDx = std::abs(bestTarget.x() - fixedScenePoint.x());
                        const qreal targetDy = std::abs(bestTarget.y() - fixedScenePoint.y());
                        const qreal snappedTargetScale = std::max<qreal>(0.05,
                            std::max(targetDx / baseW, targetDy / baseH));

                        m_uniformCornerSnapActive = true;
                        m_uniformCornerSnapHandle = static_cast<int>(activeHandle);
                        // Lock AFTER snap acquisition: use the scale implied by the snapped corner target,
                        // not the pre-snap pointer scale at zone entry.
                        m_uniformCornerSnapScale = std::clamp<qreal>(snappedTargetScale, 0.05, 100.0);
                        m_uniformCornerSnappedPt = bestTarget;
                        proposedScale = m_uniformCornerSnapScale;
                        m_uniformCornerSnapped = true;
                    }
                }
            }
        }
    }

    proposedScale = std::clamp<qreal>(proposedScale, 0.05, 100.0);
    target->setScale(proposedScale);

    QPointF finalPos;
    if (m_uniformCornerSnapped) {
        // Place item so its moving corner is exactly at m_uniformCornerSnappedPt
        switch (activeHandle) {
            case ResizableMediaBase::TopLeft:
                finalPos = m_uniformCornerSnappedPt;
                break;
            case ResizableMediaBase::TopRight:
                finalPos = m_uniformCornerSnappedPt - QPointF(baseSize.width() * proposedScale, 0);
                break;
            case ResizableMediaBase::BottomLeft:
                finalPos = m_uniformCornerSnappedPt - QPointF(0, baseSize.height() * proposedScale);
                break;
            case ResizableMediaBase::BottomRight:
            default:
                finalPos = m_uniformCornerSnappedPt - QPointF(baseSize.width() * proposedScale,
                                                               baseSize.height() * proposedScale);
                break;
        }
    } else {
        finalPos = fixedScenePoint - fixedItemPoint * proposedScale;
    }

    target->suppressNextItemPositionSnap();
    target->setPos(finalPos);
    m_resizeLastSceneX = finalPos.x();
    m_resizeLastSceneY = finalPos.y();
    m_resizeLastScale  = proposedScale;

    if (!pushLiveResizeGeometry(mediaId, finalPos.x(), finalPos.y(), proposedScale)) {
        pushMediaModelOnly();
    }
    // Build snap guide lines directly from the snap result.
    // Never use pushSnapGuidesFromScreenCanvas() here — SnapEngine functions are pure and
    // do not write to ScreenCanvas::SnapGuideItem.
    if (snap) {
        const qreal unitScaleU = (m_sceneStore->sceneUnitScale() > 1e-6) ? m_sceneStore->sceneUnitScale() : 1.0;
        const bool isAxisHandleU = (activeHandle == ResizableMediaBase::LeftMid
            || activeHandle == ResizableMediaBase::RightMid
            || activeHandle == ResizableMediaBase::TopMid
            || activeHandle == ResizableMediaBase::BottomMid);
        if (isAxisHandleU) {
            QVector<QLineF> lines;
            if (m_uniformAxisSnapped) {
                if (activeHandle == ResizableMediaBase::LeftMid || activeHandle == ResizableMediaBase::RightMid) {
                    lines.append(QLineF(m_uniformAxisSnappedEdge, -1e6, m_uniformAxisSnappedEdge,  1e6));
                } else {
                    lines.append(QLineF(-1e6, m_uniformAxisSnappedEdge,  1e6, m_uniformAxisSnappedEdge));
                }
            }
            if (!lines.isEmpty()) {
                m_modelPublisher->publishSnapGuidesOnly(m_viewAdapter, SnapGuidePublisher::buildFromLines(lines, unitScaleU));
            } else {
                m_modelPublisher->publishSnapGuidesOnly(m_viewAdapter, SnapGuidePublisher::emptyModel());
            }
        } else {
            // Corner handle: use m_uniformCornerSnapped / m_uniformCornerSnappedPt set above.
            if (m_uniformCornerSnapped) {
                QVector<QLineF> cornerLines;
                const qreal finalLeft = finalPos.x();
                const qreal finalTop = finalPos.y();
                const qreal finalRight = finalLeft + baseSize.width() * proposedScale;
                const qreal finalBottom = finalTop + baseSize.height() * proposedScale;

                qreal edgeGuideTol = 0.75;
                if (m_mediaScene && !m_mediaScene->views().isEmpty()) {
                    if (auto* sc = qobject_cast<ScreenCanvas*>(m_mediaScene->views().first())) {
                        edgeGuideTol = std::max<qreal>(0.75,
                            (sc->snapDistancePx() / sc->effectiveViewScale()) * 0.15);
                    }
                }

                QVector<qreal> xMatches;
                QVector<qreal> yMatches;
                for (qreal edgeX : m_snapStore->edgesX()) {
                    if (std::abs(edgeX - finalLeft) <= edgeGuideTol || std::abs(edgeX - finalRight) <= edgeGuideTol) {
                        if (!xMatches.contains(edgeX)) {
                            xMatches.append(edgeX);
                        }
                    }
                }
                for (qreal edgeY : m_snapStore->edgesY()) {
                    if (std::abs(edgeY - finalTop) <= edgeGuideTol || std::abs(edgeY - finalBottom) <= edgeGuideTol) {
                        if (!yMatches.contains(edgeY)) {
                            yMatches.append(edgeY);
                        }
                    }
                }

                if (xMatches.size() >= 2 && yMatches.size() >= 2) {
                    for (qreal xGuide : xMatches) {
                        cornerLines.append(QLineF(xGuide, -1e6, xGuide, 1e6));
                    }
                    for (qreal yGuide : yMatches) {
                        cornerLines.append(QLineF(-1e6, yGuide, 1e6, yGuide));
                    }
                } else {
                    cornerLines.append(QLineF(m_uniformCornerSnappedPt.x(), -1e6, m_uniformCornerSnappedPt.x(),  1e6));
                    cornerLines.append(QLineF(-1e6, m_uniformCornerSnappedPt.y(),  1e6, m_uniformCornerSnappedPt.y()));
                }
                m_modelPublisher->publishSnapGuidesOnly(m_viewAdapter, SnapGuidePublisher::buildFromLines(cornerLines, unitScaleU));
            } else {
                m_modelPublisher->publishSnapGuidesOnly(m_viewAdapter, SnapGuidePublisher::emptyModel());
            }
        }
    } else {
        m_modelPublisher->publishSnapGuidesOnly(m_viewAdapter, SnapGuidePublisher::emptyModel());
    }
}

void QuickCanvasController::handleMediaResizeEnded(const QString& mediaId) {
    if (remoteSceneLocksEdits()) {
        if (m_pointerSession->resizeActive()) {
            if (ResizableMediaBase* target = mediaItemById(m_pointerSession->resizeMediaId())) {
                commitPendingAltResize(target);
                target->endExternalInteractiveResize();
            } else {
                clearPendingAltResize();
            }
        }
        m_hasQueuedResize = false;
        if (m_resizeDispatchTimer) m_resizeDispatchTimer->stop();
        m_pointerSession->setDraggingMedia(false);
        m_pointerSession->clearResize();
        m_resizeBaseSize = QSize();
        m_resizeFixedItemPoint = QPointF();
        m_resizeFixedScenePoint = QPointF();
        m_snapStore->clear();
        resetAltResizeState();
        pushMediaModelOnly();
        pushSelectionAndSnapModels();
        return;
    }

    // A late release must not cancel the newer owner's pending update, even
    // before its first timer tick has established an active PointerSession.
    const QString resizeOwner = m_pointerSession->resizeActive()
        ? m_pointerSession->resizeMediaId()
        : (m_hasQueuedResize ? m_queuedResizeMediaId : QString());
    if (!mediaId.isEmpty() && !resizeOwner.isEmpty() && mediaId != resizeOwner) {
        return;
    }

    m_hasQueuedResize = false;
    if (m_resizeDispatchTimer) {
        m_resizeDispatchTimer->stop();
    }

    const QString finalMediaId = !mediaId.isEmpty() ? mediaId : m_pointerSession->resizeMediaId();

    if (!finalMediaId.isEmpty()
        && m_pointerSession->resizeActive()
        && !QGuiApplication::keyboardModifiers().testFlag(Qt::ShiftModifier)
        && m_queuedResizeMediaId == finalMediaId
        && !m_queuedResizeHandleId.isEmpty()) {
        m_executingQueuedResize = true;
        handleMediaResizeRequested(
            m_queuedResizeMediaId,
            m_queuedResizeHandleId,
            m_queuedResizeSceneX,
            m_queuedResizeSceneY,
            false,
            m_queuedResizeAlt);
        m_executingQueuedResize = false;
    }

    qreal finalX = m_resizeLastSceneX;
    qreal finalY = m_resizeLastSceneY;
    qreal finalScale = m_resizeLastScale;
    bool haveFinal = !finalMediaId.isEmpty();
    ResizableMediaBase* finalMedia = nullptr;
    if (m_mediaScene && !finalMediaId.isEmpty()) {
        finalMedia = mediaItemById(finalMediaId);
        if (finalMedia)
            commitPendingAltResize(finalMedia);
        else
            clearPendingAltResize();
        const ResizableMediaBase* media = finalMedia;
        if (media) {
            finalX = media->scenePos().x();
            finalY = media->scenePos().y();
            finalScale = std::abs(media->scale()) > 1e-6 ? std::abs(media->scale()) : 1.0;
            haveFinal = true;
        }
    }

    m_pointerSession->setDraggingMedia(false);
    m_pointerSession->clearResize();
    m_resizeBaseSize = QSize();
    m_resizeFixedItemPoint = QPointF();
    m_resizeFixedScenePoint = QPointF();
    m_snapStore->clear();
    resetAltResizeState();
    if (finalMedia)
        finalMedia->endExternalInteractiveResize();
    // Clear SnapGuideItem on ScreenCanvas so currentSnapGuideLines() returns empty for
    // any subsequent pushSelectionAndSnapModels call — prevents stale lines leaking across sessions.
    if (m_mediaScene && !m_mediaScene->views().isEmpty()) {
        if (auto* sc = qobject_cast<ScreenCanvas*>(m_mediaScene->views().first())) {
            sc->clearSnapGuides();
        }
    }
    if (m_mediaSyncTimer) {
        m_mediaSyncTimer->stop();
    }
    m_mediaSyncPending = false;
    // Publish final model (with correct base size) synchronously BEFORE endLiveResizeSession
    // so QML's endLiveResize fires with up-to-date width/height already in mediaModel,
    // eliminating the flash-back to stale dimensions.
    if (haveFinal) {
        pushMediaModelOnly();
    }
    endLiveResizeSession(finalMediaId, finalX, finalY, finalScale);
    // Refresh chrome/guides.
    pushSelectionAndSnapModels();
}

void QuickCanvasController::handleTextCommitRequested(const QString& mediaId, const QString& text) {
    if (remoteSceneLocksEdits()) {
        return;
    }
    if (!m_mediaScene || mediaId.isEmpty()) {
        return;
    }

    ResizableMediaBase* media = mediaItemById(mediaId);
    if (!media) {
        return;
    }

    auto* textMedia = dynamic_cast<TextMediaItem*>(media);
    if (!textMedia) {
        return;
    }

    textMedia->commitTextFromQuickEditor(text);
    // Committing content must not change selection: this can be the final step
    // of switching to another item, deselecting, or leaving the editor entirely.
    scheduleMediaModelSync();
}

void QuickCanvasController::handleTextLiveUpdateRequested(const QString& mediaId, const QString& text) {
    if (remoteSceneLocksEdits()) {
        return;
    }
    if (!m_mediaScene || mediaId.isEmpty()) {
        return;
    }

    ResizableMediaBase* media = mediaItemById(mediaId);
    if (!media) {
        return;
    }

    auto* textMedia = dynamic_cast<TextMediaItem*>(media);
    if (!textMedia) {
        return;
    }

    // Live edit path: push content changes into C++ immediately so fit-to-text
    // can recompute geometry while typing. Repaint/model publication remains
    // debounced by the existing 16 ms media sync timer.
    textMedia->setTextFromQuickEditorLive(text);
    scheduleMediaModelSync();
}

void QuickCanvasController::handleTextCreateRequested(qreal viewX, qreal viewY) {
    if (remoteSceneLocksEdits()) {
        return;
    }
    const QPointF scenePos = mapViewPointToScene(QPointF(viewX, viewY));
    emit textMediaCreateRequested(scenePos);

    // Force an immediate model sync so the newly-created text item appears in
    // the QML Repeater right away. Without this, creation only schedules a
    // debounced timer and the item stays invisible/non-interactive until the
    // timer fires — which can take tens of milliseconds after the user already
    // tried to interact with it.
    if (m_mediaSyncTimer) {
        m_mediaSyncTimer->stop();
    }
    m_mediaSyncPending = false;
    syncMediaModelFromScene();

    if (m_textToolActive) {
        setTextToolActive(false);
    }
}

void QuickCanvasController::syncMediaModelFromScene() {
    if (m_pointerSession->draggingMedia() || m_pointerSession->resizeActive()) {
        if (m_mediaSyncTimer) {
            m_mediaSyncPending = true;
            m_mediaSyncTimer->start();
        }
        return;
    }

    m_mediaSyncPending = false;

    if (!m_quickWidget || !m_quickWidget->rootObject()) {
        return;
    }

    pushMediaModelOnly();
    pushSelectionAndSnapModels();
}

void QuickCanvasController::pushMediaModelOnly() {
    if (!m_quickWidget || !m_quickWidget->rootObject()) {
        return;
    }

    m_mediaItemsById.clear();

    QVariantList mediaModel;
    if (m_mediaScene) {
        const QList<QGraphicsItem*> sceneItems = m_mediaScene->items();
        QVector<ResizableMediaBase*> orderedMedia;
        orderedMedia.reserve(sceneItems.size());
        for (QGraphicsItem* graphicsItem : sceneItems) {
            auto* media = dynamic_cast<ResizableMediaBase*>(graphicsItem);
            if (!media) {
                continue;
            }
            orderedMedia.push_back(media);
        }

        std::sort(orderedMedia.begin(), orderedMedia.end(), [](const ResizableMediaBase* lhs, const ResizableMediaBase* rhs) {
            if (!lhs || !rhs) {
                return lhs < rhs;
            }
            const QString lhsId = lhs->mediaId();
            const QString rhsId = rhs->mediaId();
            if (lhsId == rhsId) {
                return lhs < rhs;
            }
            return lhsId < rhsId;
        });

        for (ResizableMediaBase* media : orderedMedia) {
            if (!media) {
                continue;
            }

            const QString mediaId = media->mediaId();
            if (!mediaId.isEmpty()) {
                m_mediaItemsById.insert(mediaId, {media, media->lifetimeGuard()});
            }

            const QSize baseSize = media->baseSizePx();
            const QPointF scenePos = media->scenePos();
            const qreal mediaScale = std::abs(media->scale()) > 1e-6 ? std::abs(media->scale()) : 1.0;
            qreal baseWidth = static_cast<qreal>(baseSize.width());
            qreal baseHeight = static_cast<qreal>(baseSize.height());

            if (baseWidth <= 0.0 || baseHeight <= 0.0) {
                const QRectF sceneRect = media->sceneBoundingRect().normalized();
                baseWidth = std::max<qreal>(1.0, sceneRect.width());
                baseHeight = std::max<qreal>(1.0, sceneRect.height());
            }

            const qreal sceneUnitScale = (m_sceneStore->sceneUnitScale() > 1e-6) ? m_sceneStore->sceneUnitScale() : 1.0;

            QVariantMap mediaEntry;
            mediaEntry.insert(QStringLiteral("mediaId"), mediaId);
            mediaEntry.insert(QStringLiteral("mediaType"), media->isTextMedia()
                ? QStringLiteral("text")
                : (media->isVideoMedia() ? QStringLiteral("video") : QStringLiteral("image")));
            mediaEntry.insert(QStringLiteral("x"), scenePos.x() * sceneUnitScale);
            mediaEntry.insert(QStringLiteral("y"), scenePos.y() * sceneUnitScale);
            mediaEntry.insert(QStringLiteral("width"), std::max<qreal>(1.0, baseWidth * sceneUnitScale));
            mediaEntry.insert(QStringLiteral("height"), std::max<qreal>(1.0, baseHeight * sceneUnitScale));
            mediaEntry.insert(QStringLiteral("scale"), mediaScale);
            mediaEntry.insert(QStringLiteral("z"), media->zValue());
            // Selection has its own scene-derived projection. A selection-only
            // change therefore needs no content-model or media-delegate update.
            mediaEntry.insert(QStringLiteral("sourcePath"), media->sourcePath());
            mediaEntry.insert(QStringLiteral("sourceUrl"), toCanonicalMediaSourceUrl(media->sourcePath()));
            mediaEntry.insert(QStringLiteral("uploadState"), uploadStateToString(media->uploadState()));
            mediaEntry.insert(QStringLiteral("displayName"), media->displayName());
            mediaEntry.insert(QStringLiteral("contentVisible"), media->isContentVisible());
            mediaEntry.insert(QStringLiteral("contentOpacity"), media->contentOpacity());
            mediaEntry.insert(QStringLiteral("animatedDisplayOpacity"), media->animatedDisplayOpacity());

            if (auto* vid = dynamic_cast<ResizableVideoItem*>(media)) {
                mediaEntry.insert(QStringLiteral("videoPlayerPtr"),
                    QVariant::fromValue<QObject*>(vid->mediaPlayer()));
                mediaEntry.insert(QStringLiteral("videoSinkPtr"),
                    QVariant::fromValue<QObject*>(vid->videoSink()));
                mediaEntry.insert(QStringLiteral("videoHasRenderedFrame"), vid->hasRenderedFrame());
                mediaEntry.insert(QStringLiteral("videoHasPosterFrame"), vid->hasPosterFrame());
                mediaEntry.insert(QStringLiteral("videoFirstFramePrimed"), vid->firstFramePrimed());
                mediaEntry.insert(QStringLiteral("videoPlaybackErrorCode"), static_cast<int>(vid->lastPlaybackError()));
                mediaEntry.insert(QStringLiteral("videoPlaybackErrorString"), vid->lastPlaybackErrorString());
            }

            if (auto* textMedia = dynamic_cast<TextMediaItem*>(media)) {
                const TextRenderState textState = TextRenderMetrics::fromMediaItem(*textMedia);
                mediaEntry.insert(QStringLiteral("textContent"), textState.text);
                mediaEntry.insert(QStringLiteral("textHorizontalAlignment"), textState.horizontalAlignment);
                mediaEntry.insert(QStringLiteral("textVerticalAlignment"), textState.verticalAlignment);
                mediaEntry.insert(QStringLiteral("fitToTextEnabled"), textState.fitToTextEnabled);
                mediaEntry.insert(QStringLiteral("textFontFamily"), textState.fontFamily);
                mediaEntry.insert(QStringLiteral("textFontPixelSize"), textState.fontPixelSize);
                mediaEntry.insert(QStringLiteral("textFontWeight"), textState.fontWeight);
                mediaEntry.insert(QStringLiteral("textItalic"), textState.italic);
                mediaEntry.insert(QStringLiteral("textUnderline"), textState.underline);
                mediaEntry.insert(QStringLiteral("textUppercase"), textState.uppercase);
                mediaEntry.insert(QStringLiteral("textColor"), textState.textColor.name(QColor::HexArgb));
                mediaEntry.insert(QStringLiteral("textOutlineWidthPercent"), textState.outlineWidthPercent);
                mediaEntry.insert(QStringLiteral("textOutlineWidthPx"), textState.outlineWidthPixels);
                mediaEntry.insert(QStringLiteral("textOutlineColor"), textState.outlineColor.name(QColor::HexArgb));
                mediaEntry.insert(QStringLiteral("textHighlightEnabled"), textState.highlightEnabled);
                mediaEntry.insert(QStringLiteral("textHighlightColor"), textState.highlightColor.name(QColor::HexArgb));
                // All text media items are editable in the Quick Canvas.
                // isEditing() reflects the legacy widget-canvas inline editor state,
                // which is always false in the Quick Canvas path. QML handles editing
                // entirely via TextItem.editing + textCommitRequested signal.
                mediaEntry.insert(QStringLiteral("textEditable"), true);
            } else {
                mediaEntry.insert(QStringLiteral("textContent"), QString());
                mediaEntry.insert(QStringLiteral("textEditable"), false);
            }

            mediaModel.append(mediaEntry);
        }
    }

    m_modelPublisher->publishMediaModel(m_viewAdapter, mediaModel);
}

bool QuickCanvasController::beginLiveResizeSession(const QString& mediaId) {
    if (!m_quickWidget || !m_quickWidget->rootObject() || mediaId.isEmpty()) {
        return false;
    }

    return QMetaObject::invokeMethod(
        m_quickWidget->rootObject(),
        "beginLiveResize",
        Q_ARG(QVariant, mediaId));
}

void QuickCanvasController::buildResizeSnapCaches(ResizableMediaBase* resizingItem) {
    m_snapStore->rebuild(m_sceneStore->sceneScreenRects(), m_mediaScene, resizingItem);
}

void QuickCanvasController::syncSnapViewScale() const {
    if (!m_mediaScene || m_mediaScene->views().isEmpty()) {
        return;
    }
    if (auto* sc = qobject_cast<ScreenCanvas*>(m_mediaScene->views().first())) {
        sc->setExternalViewScale(currentViewScale());
    }
}

SnapEngine::AxisSnapResult QuickCanvasController::applyAxisSnapWithCachedTargets(ResizableMediaBase* target,
                                                                                  qreal proposedScale,
                                                                                  const QPointF& fixedScenePoint,
                                                                                  const QSize& baseSize,
                                                                                  int activeHandleValue,
                                                                                  bool shiftPressed,
                                                                                  ScreenCanvas* screenCanvas) const {
    return SnapEngine::applyAxisSnapWithTargets(
        target,
        proposedScale,
        fixedScenePoint,
        baseSize,
        activeHandleValue,
        shiftPressed,
        screenCanvas,
        *m_snapStore);
}

SnapEngine::CornerSnapResult QuickCanvasController::applyCornerSnapWithCachedTargets(int activeHandleValue,
                                                                                       const QPointF& fixedScenePoint,
                                                                                       qreal proposedW,
                                                                                       qreal proposedH,
                                                                                       bool shiftPressed,
                                                                                       ScreenCanvas* screenCanvas) const {
    return SnapEngine::applyCornerSnapWithTargets(
        activeHandleValue,
        fixedScenePoint,
        proposedW,
        proposedH,
        shiftPressed,
        screenCanvas,
        *m_snapStore);
}

bool QuickCanvasController::endLiveResizeSession(const QString& mediaId,
                                                 qreal sceneX,
                                                 qreal sceneY,
                                                 qreal scale) {
    if (!m_quickWidget || !m_quickWidget->rootObject() || mediaId.isEmpty()) {
        return false;
    }

    return GestureCommands::invokeMediaTransformCommand(
        m_quickWidget->rootObject(),
        "endLiveResize",
        mediaId,
        sceneX,
        sceneY,
        scale,
        m_sceneStore->sceneUnitScale());
}


bool QuickCanvasController::pushLiveResizeGeometry(const QString& mediaId,
                                                   qreal sceneX,
                                                   qreal sceneY,
                                                   qreal scale) {
    if (!m_quickWidget || !m_quickWidget->rootObject() || mediaId.isEmpty()) {
        return false;
    }

    return GestureCommands::invokeMediaTransformCommand(
        m_quickWidget->rootObject(),
        "applyLiveResizeGeometry",
        mediaId,
        sceneX,
        sceneY,
        scale,
        m_sceneStore->sceneUnitScale());
}

void QuickCanvasController::pushLiveDragSnapPosition(const QString& mediaId, qreal sceneX, qreal sceneY) {
    if (!m_quickWidget || !m_quickWidget->rootObject()) {
        return;
    }
    const qreal unitScale = (m_sceneStore->sceneUnitScale() > 1e-6) ? m_sceneStore->sceneUnitScale() : 1.0;
    QObject* root = m_quickWidget->rootObject();
    // liveSnapDragActive is derived from liveSnapDragMediaId in QML, no need to set it.
    root->setProperty("liveSnapDragMediaId", mediaId);
    root->setProperty("liveSnapDragX",       sceneX * unitScale);
    root->setProperty("liveSnapDragY",       sceneY * unitScale);
    // Record the scene-unit values so handleMediaMoveEnded can commit exactly
    // what was displayed without re-running the snap engine.
    m_lastSnapSceneX     = sceneX;
    m_lastSnapSceneY     = sceneY;
    m_lastSnapWasSnapped = true;
}

void QuickCanvasController::clearLiveDragSnapPosition() {
    if (!m_quickWidget || !m_quickWidget->rootObject()) {
        return;
    }
    QObject* root = m_quickWidget->rootObject();
    // liveSnapDragActive is a derived readonly property in QML (liveSnapDragMediaId !== ""),
    // so clearing liveSnapDragMediaId is sufficient — active updates automatically.
    root->setProperty("liveSnapDragMediaId", QString());
    m_lastSnapSceneX     = 0.0;
    m_lastSnapSceneY     = 0.0;
    m_lastSnapWasSnapped = false;
}

void QuickCanvasController::pushSnapGuidesFromScreenCanvas() {
    if (!m_mediaScene || !m_modelPublisher) {
        return;
    }
    const qreal sceneUnitScale = (m_sceneStore->sceneUnitScale() > 1e-6) ? m_sceneStore->sceneUnitScale() : 1.0;
    ScreenCanvas* screenCanvas = nullptr;
    if (!m_mediaScene->views().isEmpty()) {
        screenCanvas = qobject_cast<ScreenCanvas*>(m_mediaScene->views().first());
    }
    m_modelPublisher->publishSnapGuidesOnly(
        m_viewAdapter,
        SnapGuidePublisher::buildFromScreenCanvas(screenCanvas, sceneUnitScale));
}

void QuickCanvasController::pushSelectionAndSnapModels() {
    if (!m_quickWidget || !m_quickWidget->rootObject()) {
        return;
    }

    // A read-only projection of the scene's selection, never an independent
    // selection store. Rebuilding it also covers widget/backend-initiated changes.
    QVariantList selectionChromeModel;
    QVariantList snapGuidesModel;

    if (m_mediaScene) {
        const qreal sceneUnitScale = (m_sceneStore->sceneUnitScale() > 1e-6) ? m_sceneStore->sceneUnitScale() : 1.0;

        const QList<QGraphicsItem*> sceneItems = m_mediaScene->items();
        for (QGraphicsItem* graphicsItem : sceneItems) {
            auto* media = dynamic_cast<ResizableMediaBase*>(graphicsItem);
            if (!media || !media->isSelected()) {
                continue;
            }

            const QSize baseSize = media->baseSizePx();
            const QPointF scenePos = media->scenePos();
            const qreal mediaScale = std::abs(media->scale()) > 1e-6 ? std::abs(media->scale()) : 1.0;

            qreal baseWidth = static_cast<qreal>(baseSize.width());
            qreal baseHeight = static_cast<qreal>(baseSize.height());
            if (baseWidth <= 0.0 || baseHeight <= 0.0) {
                const QRectF sceneRect = media->sceneBoundingRect().normalized();
                baseWidth = std::max<qreal>(1.0, sceneRect.width());
                baseHeight = std::max<qreal>(1.0, sceneRect.height());
            }

            QVariantMap entry;
            entry.insert(QStringLiteral("mediaId"), media->mediaId());
            entry.insert(QStringLiteral("x"), scenePos.x() * sceneUnitScale);
            entry.insert(QStringLiteral("y"), scenePos.y() * sceneUnitScale);
            entry.insert(QStringLiteral("width"), std::max<qreal>(1.0, baseWidth * mediaScale * sceneUnitScale));
            entry.insert(QStringLiteral("height"), std::max<qreal>(1.0, baseHeight * mediaScale * sceneUnitScale));
            selectionChromeModel.append(entry);
        }

        // Snap guides in Quick Canvas are managed independently via publishSnapGuidesOnly().
        // Do NOT read ScreenCanvas::currentSnapGuideLines() here — those belong to the
        // widget-canvas path and may contain stale lines from prior resize/drag sessions.
        // publishSelectionAndSnapModels always publishes empty guides; the active drag/resize
        // paths push their own guides via publishSnapGuidesOnly() on every tick.
    }

    m_modelPublisher->publishSelectionAndSnapModels(m_viewAdapter, selectionChromeModel, snapGuidesModel);
}

void QuickCanvasController::pushVideoStateModel() {
    if ((m_localDragAccepted || m_localDragCursorHidden) && remoteSceneLocksEdits()) {
        clearLocalDragPreview(false);
    }
    if (!m_quickWidget || !m_quickWidget->rootObject() || !m_mediaScene) {
        return;
    }

    // Publish state for ALL video items present in the scene, not only the selected
    // one.  The result is a QVariantMap (JavaScript object) keyed by mediaId, so each
    // overlay can look up its own state with videoStateModel[mediaId] regardless of
    // which items are selected.
    //
    // Position is read live from QMediaPlayer::position() rather than the cached
    // m_positionMs field, which can be up to 100 ms stale (positionChanged cadence).
    // This halves the worst-case display lag from ~150 ms to ~50 ms (poll interval).
    QVariantMap stateDict;
    const QList<QGraphicsItem*> allItems = m_mediaScene->items();
    for (QGraphicsItem* gi : allItems) {
        auto* v = dynamic_cast<ResizableVideoItem*>(gi);
        if (!v || !v->mediaPlayer()) continue;
        const QString mid = v->mediaId();
        if (mid.isEmpty()) continue;

        const qint64 durationMs = v->mediaPlayer()->duration();
        // During a scrub the player may still be acknowledging an older
        // coalesced seek. Keep the thumb at the newest user target until its
        // native frame has actually arrived.
        const qint64 positionMs = v->isDraggingProgress()
            ? v->currentPositionMs()
            : v->mediaPlayer()->position();
        const qreal progress = (durationMs > 0)
            ? std::clamp<qreal>(static_cast<qreal>(positionMs) / static_cast<qreal>(durationMs), 0.0, 1.0)
            : 0.0;

        QVariantMap state;
        state.insert(QStringLiteral("mediaId"),   mid);
        state.insert(QStringLiteral("isPlaying"),  v->isPlaying());
        state.insert(QStringLiteral("isMuted"),    v->isMuted());
        // Use the direct m_repeatEnabled flag rather than shouldAutoRepeat() so the
        // loop button always reflects the last user toggle, not settings-layer logic.
        state.insert(QStringLiteral("isLooping"),  v->isRepeatEnabled());
        state.insert(QStringLiteral("progress"),   progress);
        state.insert(QStringLiteral("volume"),     v->volume());
        stateDict.insert(mid, state);
    }

    m_quickWidget->rootObject()->setProperty("videoStateModel", stateDict);
}

// ---- Overlay action slots ----

void QuickCanvasController::handleOverlayVisibilityToggle(const QString& mediaId, bool visible) {
    if (remoteSceneLocksEdits()) return;
    ResizableMediaBase* media = mediaItemById(mediaId);
    if (!media) return;
    if (visible) {
        media->setContentVisible(true);
    } else {
        media->setContentVisible(false);
    }
    emit mediaVisibilityToggleRequested(mediaId, visible);
    scheduleMediaModelSync();
}

void QuickCanvasController::handleOverlayBringForward(const QString& mediaId) {
    if (remoteSceneLocksEdits()) return;
    ResizableMediaBase* media = mediaItemById(mediaId);
    if (!media || !m_mediaScene || m_mediaScene->views().isEmpty()) return;
    if (auto* sc = qobject_cast<ScreenCanvas*>(m_mediaScene->views().first())) {
        sc->moveMediaUp(media);
    }
    emit mediaBringForwardRequested(mediaId);
    scheduleMediaModelSync();
}

void QuickCanvasController::handleOverlayBringBackward(const QString& mediaId) {
    if (remoteSceneLocksEdits()) return;
    ResizableMediaBase* media = mediaItemById(mediaId);
    if (!media || !m_mediaScene || m_mediaScene->views().isEmpty()) return;
    if (auto* sc = qobject_cast<ScreenCanvas*>(m_mediaScene->views().first())) {
        sc->moveMediaDown(media);
    }
    emit mediaBringBackwardRequested(mediaId);
    scheduleMediaModelSync();
}

void QuickCanvasController::handleOverlayDelete(const QString& mediaId) {
    if (remoteSceneLocksEdits()) return;
    ResizableMediaBase* media = mediaItemById(mediaId);
    if (!media) return;
    emit mediaDeleteRequested(mediaId);
    if (m_mediaScene && !m_mediaScene->views().isEmpty()) {
        if (auto* sc = qobject_cast<ScreenCanvas*>(m_mediaScene->views().first())) {
            ScreenCanvas::requestMediaDeletion(sc, media);
            return;
        }
    }
    media->prepareForDeletion();
    if (m_mediaScene) m_mediaScene->removeItem(media);
    delete media;
    scheduleMediaModelSync();
}

void QuickCanvasController::handleOverlayPlayPause(const QString& mediaId) {
    if (remoteSceneLocksEdits()) return;
    if (auto* v = dynamic_cast<ResizableVideoItem*>(mediaItemById(mediaId))) {
        v->togglePlayPause();
        emit mediaPlayPauseRequested(mediaId);
    }
}

void QuickCanvasController::handleOverlayStop(const QString& mediaId) {
    if (remoteSceneLocksEdits()) return;
    if (auto* v = dynamic_cast<ResizableVideoItem*>(mediaItemById(mediaId))) {
        v->stopToBeginning();
        emit mediaStopRequested(mediaId);
    }
}

void QuickCanvasController::handleOverlayRepeatToggle(const QString& mediaId) {
    if (remoteSceneLocksEdits()) return;
    if (auto* v = dynamic_cast<ResizableVideoItem*>(mediaItemById(mediaId))) {
        v->toggleRepeat();
        emit mediaRepeatToggleRequested(mediaId);
    }
}

void QuickCanvasController::handleOverlayMuteToggle(const QString& mediaId) {
    if (remoteSceneLocksEdits()) return;
    if (auto* v = dynamic_cast<ResizableVideoItem*>(mediaItemById(mediaId))) {
        v->toggleMute();
        emit mediaMuteToggleRequested(mediaId);
    }
}

void QuickCanvasController::handleOverlayVolumeChange(const QString& mediaId, qreal value) {
    if (remoteSceneLocksEdits()) return;
    if (auto* v = dynamic_cast<ResizableVideoItem*>(mediaItemById(mediaId))) {
        v->setVolume(value);
        emit mediaVolumeChangeRequested(mediaId, value);
    }
}

// ---- Three-phase seek handlers ----
// Phase 1 starts one frame-acknowledged scrub session.
void QuickCanvasController::handleOverlaySeekBegin(const QString& mediaId, qreal ratio) {
    if (remoteSceneLocksEdits()) return;
    if (auto* v = dynamic_cast<ResizableVideoItem*>(mediaItemById(mediaId))) {
        v->beginScrub(std::clamp(ratio, 0.0, 1.0));
    }
}

// Phase 2 retains only the newest pointer position while one seek is in flight.
void QuickCanvasController::handleOverlaySeekUpdate(const QString& mediaId, qreal ratio) {
    if (remoteSceneLocksEdits()) return;
    if (auto* v = dynamic_cast<ResizableVideoItem*>(mediaItemById(mediaId))) {
        v->updateScrub(std::clamp(ratio, 0.0, 1.0));
    }
}

// Phase 3 marks the newest target as final; the video resumes, if necessary,
// only after that target's native frame has arrived.
void QuickCanvasController::handleOverlaySeekEnd(const QString& mediaId, qreal ratio) {
    if (remoteSceneLocksEdits()) {
        if (auto* v = dynamic_cast<ResizableVideoItem*>(mediaItemById(mediaId))) {
            v->endScrub(std::clamp(ratio, 0.0, 1.0));
        }
        return;
    }
    if (auto* v = dynamic_cast<ResizableVideoItem*>(mediaItemById(mediaId))) {
        v->endScrub(std::clamp(ratio, 0.0, 1.0));
        emit mediaSeekRequested(mediaId, ratio);
    }
}

void QuickCanvasController::handleOverlayFitToTextToggle(const QString& mediaId) {
    if (remoteSceneLocksEdits()) return;
    if (auto* t = dynamic_cast<TextMediaItem*>(mediaItemById(mediaId))) {
        t->setFitToTextEnabled(!t->fitToTextEnabled());
        emit mediaFitToTextToggleRequested(mediaId);
        scheduleMediaModelSync();
    }
}

void QuickCanvasController::handleOverlayHorizontalAlign(const QString& mediaId, const QString& alignment) {
    if (remoteSceneLocksEdits()) return;
    auto* t = dynamic_cast<TextMediaItem*>(mediaItemById(mediaId));
    if (!t) return;
    if      (alignment == QStringLiteral("left"))   t->setHorizontalAlignment(TextMediaItem::HorizontalAlignment::Left);
    else if (alignment == QStringLiteral("center")) t->setHorizontalAlignment(TextMediaItem::HorizontalAlignment::Center);
    else if (alignment == QStringLiteral("right"))  t->setHorizontalAlignment(TextMediaItem::HorizontalAlignment::Right);
    emit mediaHorizontalAlignRequested(mediaId, alignment);
    scheduleMediaModelSync();
}

void QuickCanvasController::handleOverlayVerticalAlign(const QString& mediaId, const QString& alignment) {
    if (remoteSceneLocksEdits()) return;
    auto* t = dynamic_cast<TextMediaItem*>(mediaItemById(mediaId));
    if (!t) return;
    if      (alignment == QStringLiteral("top"))    t->setVerticalAlignment(TextMediaItem::VerticalAlignment::Top);
    else if (alignment == QStringLiteral("center")) t->setVerticalAlignment(TextMediaItem::VerticalAlignment::Center);
    else if (alignment == QStringLiteral("bottom")) t->setVerticalAlignment(TextMediaItem::VerticalAlignment::Bottom);
    emit mediaVerticalAlignRequested(mediaId, alignment);
    scheduleMediaModelSync();
}

void QuickCanvasController::handleFadeAnimationTick() {
    if (!m_fadeTickTimer) return;
    if (!m_fadeTickTimer->isActive()) {
        m_fadeTickTimer->start();
    }
}

void QuickCanvasController::handleMediaSettingsChanged(ResizableMediaBase* /*media*/) {
    scheduleMediaModelSync();
}

void QuickCanvasController::pushStaticLayerModels() {
    if (!m_quickWidget || !m_quickWidget->rootObject()) {
        return;
    }

    QVariantList screensModel;
    QVariantList uiZonesModel;

    QList<ScreenInfo> orderedScreens = m_sceneStore->screens();
    std::sort(orderedScreens.begin(), orderedScreens.end(), [](const ScreenInfo& a, const ScreenInfo& b) {
        // Deterministic visual order for labels/delegates:
        // top-to-bottom rows, then left-to-right within each row.
        constexpr int kRowTolerancePx = 80;
        if (std::abs(a.y - b.y) > kRowTolerancePx) {
            return a.y < b.y;
        }
        if (a.x != b.x) {
            return a.x < b.x;
        }
        return a.id < b.id;
    });

    int displayIndex = 1;
    for (const auto& screen : orderedScreens) {
        const QRectF rect = m_sceneStore->sceneScreenRects().value(screen.id);

        QVariantMap screenEntry;
        screenEntry.insert(QStringLiteral("screenId"), screen.id);
        screenEntry.insert(QStringLiteral("displayIndex"), displayIndex++);
        screenEntry.insert(QStringLiteral("x"), rect.x());
        screenEntry.insert(QStringLiteral("y"), rect.y());
        screenEntry.insert(QStringLiteral("width"), rect.width());
        screenEntry.insert(QStringLiteral("height"), rect.height());
        screenEntry.insert(QStringLiteral("primary"), screen.primary);
        // Actual pixel resolution — used for the «Screen N (WxH)» label
        screenEntry.insert(QStringLiteral("pixelWidth"),  screen.width);
        screenEntry.insert(QStringLiteral("pixelHeight"), screen.height);
        screensModel.append(screenEntry);

        for (const auto& zone : screen.uiZones) {
            if (screen.width <= 0 || screen.height <= 0 || rect.width() <= 0.0 || rect.height() <= 0.0) {
                continue;
            }

            const qreal sx = static_cast<qreal>(zone.x) / static_cast<qreal>(screen.width);
            const qreal sy = static_cast<qreal>(zone.y) / static_cast<qreal>(screen.height);
            const qreal sw = static_cast<qreal>(zone.width) / static_cast<qreal>(screen.width);
            const qreal sh = static_cast<qreal>(zone.height) / static_cast<qreal>(screen.height);
            if (sw <= 0.0 || sh <= 0.0) {
                continue;
            }

            QRectF zoneRect(
                rect.x() + sx * rect.width(),
                rect.y() + sy * rect.height(),
                sw * rect.width(),
                sh * rect.height());

            if (zoneRect.height() < 3.0) {
                const qreal delta = 3.0 - zoneRect.height();
                zoneRect.setHeight(3.0);
                if (sy > 0.5) {
                    zoneRect.moveTop(zoneRect.top() - delta);
                }
            }

            QVariantMap zoneEntry;
            zoneEntry.insert(QStringLiteral("screenId"), screen.id);
            zoneEntry.insert(QStringLiteral("type"), zone.type);
            zoneEntry.insert(QStringLiteral("x"), zoneRect.x());
            zoneEntry.insert(QStringLiteral("y"), zoneRect.y());
            zoneEntry.insert(QStringLiteral("width"), zoneRect.width());
            zoneEntry.insert(QStringLiteral("height"), zoneRect.height());

            const QString zoneType = zone.type.toLower();
            const bool systemZone = zoneType == QStringLiteral("taskbar")
                                 || zoneType == QStringLiteral("dock")
                                 || zoneType == QStringLiteral("menu_bar");
            zoneEntry.insert(QStringLiteral("fillColor"), systemZone
                ? QStringLiteral("#50000000")
                : QStringLiteral("#5A808080"));

            uiZonesModel.append(zoneEntry);
        }
    }

    m_quickWidget->rootObject()->setProperty("screensModel", screensModel);
    m_quickWidget->rootObject()->setProperty("uiZonesModel", uiZonesModel);
}

void QuickCanvasController::pushRemoteCursorState() {
    if (!m_quickWidget || !m_quickWidget->rootObject()) {
        return;
    }
    m_quickWidget->rootObject()->setProperty("remoteCursorVisible", m_sceneStore->remoteCursorVisible());
    m_quickWidget->rootObject()->setProperty("remoteCursorX", m_sceneStore->remoteCursorPos().x());
    m_quickWidget->rootObject()->setProperty("remoteCursorY", m_sceneStore->remoteCursorPos().y());
    m_quickWidget->rootObject()->setProperty("remoteCursorDiameter", kRemoteCursorDiameterPx);
    m_quickWidget->rootObject()->setProperty("remoteCursorFill", QStringLiteral("#FFFFFFFF"));
    m_quickWidget->rootObject()->setProperty("remoteCursorBorder", QStringLiteral("#E6000000"));
    m_quickWidget->rootObject()->setProperty("remoteCursorBorderWidth", kRemoteCursorBorderWidthPx);
}

QPointF QuickCanvasController::mapRemoteCursorToQuickScene(int globalX, int globalY, bool* ok) const {
    if (ok) {
        *ok = false;
    }
    if (m_sceneStore->screens().isEmpty() || m_sceneStore->sceneScreenRects().isEmpty()) {
        return {};
    }

    const ScreenInfo* containing = nullptr;
    for (const auto& screen : m_sceneStore->screens()) {
        const QRect remoteRect(screen.x, screen.y, screen.width, screen.height);
        const QRect tolerantRect = remoteRect.adjusted(-1, -1, 1, 1);
        if (tolerantRect.contains(globalX, globalY)) {
            containing = &screen;
            break;
        }
    }
    if (!containing) {
        return {};
    }

    const auto rectIt = m_sceneStore->sceneScreenRects().constFind(containing->id);
    if (rectIt == m_sceneStore->sceneScreenRects().constEnd()) {
        return {};
    }

    const QRect remoteRect(containing->x, containing->y, containing->width, containing->height);
    if (remoteRect.width() <= 0 || remoteRect.height() <= 0) {
        return {};
    }

    const int maxDx = std::max(0, remoteRect.width() - 1);
    const int maxDy = std::max(0, remoteRect.height() - 1);
    const int localX = std::clamp(globalX - remoteRect.x(), 0, maxDx);
    const int localY = std::clamp(globalY - remoteRect.y(), 0, maxDy);
    const qreal relX = (maxDx > 0)
        ? static_cast<qreal>(localX) / static_cast<qreal>(maxDx)
        : 0.0;
    const qreal relY = (maxDy > 0)
        ? static_cast<qreal>(localY) / static_cast<qreal>(maxDy)
        : 0.0;

    if (ok) {
        *ok = true;
    }

    const QRectF sceneRect = rectIt.value();
    return {
        sceneRect.x() + relX * sceneRect.width(),
        sceneRect.y() + relY * sceneRect.height()
    };
}

void QuickCanvasController::rebuildScreenRects() {
    QHash<int, QRectF> newRects;
    if (m_sceneStore->screens().isEmpty()) {
        m_sceneStore->setSceneUnitScale(1.0);
        m_sceneStore->setSceneScreenRects(newRects);
        return;
    }

    m_sceneStore->setSceneUnitScale(currentSceneUnitScale());

    int minX = std::numeric_limits<int>::max();
    int minY = std::numeric_limits<int>::max();
    for (const auto& screen : m_sceneStore->screens()) {
        minX = std::min(minX, screen.x);
        minY = std::min(minY, screen.y);
    }
    if (minX == std::numeric_limits<int>::max()) {
        minX = 0;
    }
    if (minY == std::numeric_limits<int>::max()) {
        minY = 0;
    }

    for (const auto& screen : m_sceneStore->screens()) {
        newRects.insert(
            screen.id,
            QRectF(
                static_cast<qreal>(screen.x - minX) * m_sceneStore->sceneUnitScale(),
                static_cast<qreal>(screen.y - minY) * m_sceneStore->sceneUnitScale(),
                static_cast<qreal>(screen.width) * m_sceneStore->sceneUnitScale(),
                static_cast<qreal>(screen.height) * m_sceneStore->sceneUnitScale()));
    }
    m_sceneStore->setSceneScreenRects(newRects);
}

void QuickCanvasController::refreshSceneUnitScaleIfNeeded(bool force) {
    m_pendingInitialSceneScaleRefresh = false;

    const qreal nextScale = currentSceneUnitScale();
    if (!force && qFuzzyCompare(1.0 + nextScale, 1.0 + m_sceneStore->sceneUnitScale())) {
        return;
    }

    const qreal oldScale = m_sceneStore->sceneUnitScale();
    if (m_sceneStore->screens().isEmpty()) {
        m_sceneStore->setSceneUnitScale(nextScale);
        return;
    }

    rebuildScreenRects();
    pushStaticLayerModels();
    syncMediaModelFromScene();

    if (m_sceneStore->remoteCursorVisible() && oldScale > 0.0 && nextScale > 0.0) {
        const qreal ratio = nextScale / oldScale;
        m_sceneStore->setRemoteCursor(
            m_sceneStore->remoteCursorVisible(),
            m_sceneStore->remoteCursorPos().x() * ratio,
            m_sceneStore->remoteCursorPos().y() * ratio
        );
        pushRemoteCursorState();
    }

    if (!m_initialFitCompleted) {
        scheduleInitialFitIfNeeded(m_initialFitMarginPx);
    } else {
        recenterView();
    }
}

qreal QuickCanvasController::currentSceneUnitScale() const {
    return 1.0;
}

qreal QuickCanvasController::currentViewScale() const {
    if (!m_quickWidget || !m_quickWidget->rootObject()) {
        return 1.0;
    }
    const qreal scale = m_quickWidget->rootObject()->property("viewScale").toReal();
    return scale > 1e-6 ? scale : 1.0;
}

QPointF QuickCanvasController::mapViewPointToScene(const QPointF& viewPoint) const {
    if (!m_quickWidget || !m_quickWidget->rootObject()) {
        return {};
    }

    const QObject* rootObject = m_quickWidget->rootObject();
    const qreal viewScale = std::max<qreal>(1e-6, rootObject->property("viewScale").toReal());
    const qreal panX = rootObject->property("panX").toReal();
    const qreal panY = rootObject->property("panY").toReal();

    const qreal quickSceneX = (viewPoint.x() - panX) / viewScale;
    const qreal quickSceneY = (viewPoint.y() - panY) / viewScale;
    const qreal invSceneUnitScale = (m_sceneStore->sceneUnitScale() > 1e-6) ? (1.0 / m_sceneStore->sceneUnitScale()) : 1.0;

    return { quickSceneX * invSceneUnitScale, quickSceneY * invSceneUnitScale };
}

void QuickCanvasController::scheduleInitialFitIfNeeded(int marginPx) {
    if (m_initialFitCompleted || m_sceneStore->screens().isEmpty()) {
        return;
    }

    m_initialFitMarginPx = marginPx;

    if (tryInitialFitNow(marginPx)) {
        m_initialFitPending = false;
        m_initialFitRetryCount = 0;
        return;
    }

    if (m_initialFitPending) {
        return;
    }

    m_initialFitPending = true;
    m_initialFitRetryCount = 0;
    if (m_initialFitRetryTimer) {
        m_initialFitRetryTimer->start();
    }
}

bool QuickCanvasController::tryInitialFitNow(int marginPx) {
    if (!m_quickWidget || !m_quickWidget->rootObject() || m_sceneStore->screens().isEmpty()) {
        return false;
    }

    const QSize widgetSize = m_quickWidget->size();
    if (widgetSize.width() < 48 || widgetSize.height() < 48) {
        return false;
    }

    QRectF bounds;
    bool hasBounds = false;
    for (auto it = m_sceneStore->sceneScreenRects().constBegin(); it != m_sceneStore->sceneScreenRects().constEnd(); ++it) {
        if (!hasBounds) {
            bounds = it.value();
            hasBounds = true;
        } else {
            bounds = bounds.united(it.value());
        }
    }

    if (!hasBounds || bounds.width() <= 0.0 || bounds.height() <= 0.0) {
        return false;
    }

    QMetaObject::invokeMethod(
        m_quickWidget->rootObject(),
        "fitToBounds",
        Q_ARG(QVariant, bounds.x()),
        Q_ARG(QVariant, bounds.y()),
        Q_ARG(QVariant, bounds.width()),
        Q_ARG(QVariant, bounds.height()),
        Q_ARG(QVariant, static_cast<qreal>(marginPx)));

    m_initialFitCompleted = true;
    return true;
}

bool QuickCanvasController::pushLiveAltResizeGeometry(const QString& mediaId,
                                                      qreal sceneX,
                                                      qreal sceneY,
                                                      qreal width,
                                                      qreal height,
                                                      qreal scale) {
    if (!m_quickWidget || !m_quickWidget->rootObject() || mediaId.isEmpty()) {
        return false;
    }
    return QMetaObject::invokeMethod(
        m_quickWidget->rootObject(),
        "applyLiveAltResizeGeometry",
        Q_ARG(QVariant, mediaId),
        Q_ARG(QVariant, sceneX),
        Q_ARG(QVariant, sceneY),
        Q_ARG(QVariant, width),
        Q_ARG(QVariant, height),
        Q_ARG(QVariant, scale));
}

void QuickCanvasController::stagePendingAltResize(const QString& mediaId,
                                                  const QSize& baseSize,
                                                  const QPointF& scenePos) {
    m_pendingAltResizeMediaId = mediaId;
    m_pendingAltResizeBaseSize = baseSize;
    m_pendingAltResizeScenePos = scenePos;
}

bool QuickCanvasController::commitPendingAltResize(ResizableMediaBase* target) {
    if (m_pendingAltResizeMediaId.isEmpty()) {
        return false;
    }

    if (!target || target->mediaId() != m_pendingAltResizeMediaId
        || target->isBeingDeleted()) {
        clearPendingAltResize();
        return false;
    }

    const QSize finalBaseSize = m_pendingAltResizeBaseSize;
    const QPointF finalScenePos = m_pendingAltResizeScenePos;
    clearPendingAltResize();

    target->setBaseSizePx(finalBaseSize);
    target->suppressNextItemPositionSnap();
    target->setPos(finalScenePos);
    target->notifyInteractiveGeometryChanged();
    return true;
}

void QuickCanvasController::clearPendingAltResize() {
    m_pendingAltResizeMediaId.clear();
    m_pendingAltResizeBaseSize = QSize();
    m_pendingAltResizeScenePos = QPointF();
}

void QuickCanvasController::resetAltResizeState() {
    m_lastResizeWasAlt        = false;
    m_altAxisCaptured         = false;
    m_altCornerCaptured       = false;
    m_altOrigBaseSize         = QSize();
    m_altFixedScenePoint      = QPointF();
    m_altAxisInitialOffset    = 0.0;
    m_altCornerInitialOffsetX = 0.0;
    m_altCornerInitialOffsetY = 0.0;
    clearPendingAltResize();
    m_uniformCornerSnapped    = false;
    m_uniformCornerSnappedPt  = QPointF();
    m_uniformCornerSnapActive = false;
    m_uniformCornerSnapHandle = static_cast<int>(ResizableMediaBase::None);
    m_uniformCornerSnapScale  = 1.0;
}

bool QuickCanvasController::isAxisHandle(int handleValue) {
    return handleValue == static_cast<int>(ResizableMediaBase::LeftMid)
        || handleValue == static_cast<int>(ResizableMediaBase::RightMid)
        || handleValue == static_cast<int>(ResizableMediaBase::TopMid)
        || handleValue == static_cast<int>(ResizableMediaBase::BottomMid);
}

bool QuickCanvasController::isCornerHandle(int handleValue) {
    return handleValue == static_cast<int>(ResizableMediaBase::TopLeft)
        || handleValue == static_cast<int>(ResizableMediaBase::TopRight)
        || handleValue == static_cast<int>(ResizableMediaBase::BottomLeft)
        || handleValue == static_cast<int>(ResizableMediaBase::BottomRight);
}

QPointF QuickCanvasController::computeHandleItemPoint(int handleValue, const QSize& baseSize) {
    const qreal w = static_cast<qreal>(baseSize.width());
    const qreal h = static_cast<qreal>(baseSize.height());
    switch (static_cast<ResizableMediaBase::Handle>(handleValue)) {
        case ResizableMediaBase::TopLeft:     return QPointF(0.0,     0.0);
        case ResizableMediaBase::TopMid:      return QPointF(w * 0.5, 0.0);
        case ResizableMediaBase::TopRight:    return QPointF(w,       0.0);
        case ResizableMediaBase::LeftMid:     return QPointF(0.0,     h * 0.5);
        case ResizableMediaBase::RightMid:    return QPointF(w,       h * 0.5);
        case ResizableMediaBase::BottomLeft:  return QPointF(0.0,     h);
        case ResizableMediaBase::BottomMid:   return QPointF(w * 0.5, h);
        case ResizableMediaBase::BottomRight: return QPointF(w,       h);
        default:                              return QPointF(w * 0.5, h * 0.5);
    }
}
