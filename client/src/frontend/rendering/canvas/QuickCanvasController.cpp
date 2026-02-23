#include "frontend/rendering/canvas/QuickCanvasController.h"
#include "frontend/rendering/canvas/CanvasSceneStore.h"
#include "frontend/rendering/canvas/GestureCommands.h"
#include "frontend/rendering/canvas/ModelPublisher.h"
#include "frontend/rendering/canvas/PointerSession.h"
#include "frontend/rendering/canvas/QuickCanvasViewAdapter.h"
#include "frontend/rendering/canvas/SelectionStore.h"
#include "frontend/rendering/canvas/SnapEngine.h"
#include "frontend/rendering/canvas/SnapStore.h"

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
#include <QMouseEvent>
#include <QQuickItem>
#include <QQuickWidget>
#include <QScreen>
#include <QLineF>
#include <QGuiApplication>
#include <QTimer>
#include <QVariantList>
#include <QWindow>
#include <QtMath>

#include "backend/domain/media/MediaItems.h"
#include "backend/domain/media/TextMediaItem.h"
#include "frontend/rendering/canvas/ScreenCanvas.h"

namespace {
constexpr int kRemoteCursorDiameterPx = 30;
constexpr qreal kRemoteCursorBorderWidthPx = 2.0;

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

QString textHorizontalAlignmentToString(TextMediaItem::HorizontalAlignment alignment) {
    switch (alignment) {
    case TextMediaItem::HorizontalAlignment::Left:
        return QStringLiteral("left");
    case TextMediaItem::HorizontalAlignment::Center:
        return QStringLiteral("center");
    case TextMediaItem::HorizontalAlignment::Right:
        return QStringLiteral("right");
    }
    return QStringLiteral("center");
}

QString textVerticalAlignmentToString(TextMediaItem::VerticalAlignment alignment) {
    switch (alignment) {
    case TextMediaItem::VerticalAlignment::Top:
        return QStringLiteral("top");
    case TextMediaItem::VerticalAlignment::Center:
        return QStringLiteral("center");
    case TextMediaItem::VerticalAlignment::Bottom:
        return QStringLiteral("bottom");
    }
    return QStringLiteral("center");
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
}

QuickCanvasController::QuickCanvasController(QObject* parent)
    : QObject(parent)
{
    m_sceneStore = new CanvasSceneStore(this);
    m_pointerSession = new PointerSession();
    m_selectionStore = new SelectionStore();
    m_modelPublisher = new ModelPublisher();
    m_snapStore = new SnapStore();

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

        m_hasQueuedResize = false;
        m_executingQueuedResize = true;
        handleMediaResizeRequested(mediaId, handleId, sceneX, sceneY, snap);
        m_executingQueuedResize = false;

        if (m_hasQueuedResize) {
            m_resizeDispatchTimer->start();
        }
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
    delete m_modelPublisher;
    m_modelPublisher = nullptr;
    delete m_pointerSession;
    m_pointerSession = nullptr;
    delete m_selectionStore;
    m_selectionStore = nullptr;
    delete m_snapStore;
    m_snapStore = nullptr;
}

bool QuickCanvasController::initialize(QWidget* parentWidget, QString* errorMessage) {
    if (m_quickWidget) {
        return true;
    }

    m_quickWidget = new QQuickWidget(parentWidget);
    m_quickWidget->installEventFilter(this);
    m_quickWidget->setAcceptDrops(true);
    m_quickWidget->setResizeMode(QQuickWidget::SizeRootObjectToView);
    m_quickWidget->setSource(QUrl(QStringLiteral("qrc:/qml/CanvasRoot.qml")));

    if (m_quickWidget->status() == QQuickWidget::Error) {
        if (errorMessage) {
            QStringList messages;
            const auto qmlErrors = m_quickWidget->errors();
            for (const auto& err : qmlErrors) {
                messages.append(err.toString());
            }
            *errorMessage = messages.join(QStringLiteral(" | "));
        }
        delete m_quickWidget;
        m_quickWidget = nullptr;
        return false;
    }

    m_viewAdapter = new QuickCanvasViewAdapter(m_quickWidget, this);

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
        m_quickWidget->rootObject(), SIGNAL(mediaMoveStarted(QString,double,double)),
        this, SLOT(handleMediaMoveStarted(QString,double,double)));
    QObject::connect(
        m_quickWidget->rootObject(), SIGNAL(mediaMoveUpdated(QString,double,double)),
        this, SLOT(handleMediaMoveUpdated(QString,double,double)));
    QObject::connect(
        m_quickWidget->rootObject(), SIGNAL(mediaMoveEnded(QString,double,double)),
        this, SLOT(handleMediaMoveEnded(QString,double,double)));
    QObject::connect(
        m_quickWidget->rootObject(), SIGNAL(mediaResizeRequested(QString,QString,double,double,bool)),
        this, SLOT(handleMediaResizeRequested(QString,QString,double,double,bool)));
    QObject::connect(
        m_quickWidget->rootObject(), SIGNAL(mediaResizeEnded(QString)),
        this, SLOT(handleMediaResizeEnded(QString)));
    QObject::connect(
        m_quickWidget->rootObject(), SIGNAL(textCommitRequested(QString,QString)),
        this, SLOT(handleTextCommitRequested(QString,QString)));
    QObject::connect(
        m_quickWidget->rootObject(), SIGNAL(textCreateRequested(double,double)),
        this, SLOT(handleTextCreateRequested(double,double)));

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
            if (dragEnter && dragEnter->mimeData() && dragEnter->mimeData()->hasUrls()) {
                dragEnter->acceptProposedAction();
                return true;
            }
            break;
        }
        case QEvent::DragMove: {
            auto* dragMove = static_cast<QDragMoveEvent*>(event);
            if (dragMove && dragMove->mimeData() && dragMove->mimeData()->hasUrls()) {
                dragMove->acceptProposedAction();
                return true;
            }
            break;
        }
        case QEvent::Drop: {
            auto* dropEvent = static_cast<QDropEvent*>(event);
            if (dropEvent && dropEvent->mimeData() && dropEvent->mimeData()->hasUrls()) {
                QStringList localPaths;
                const QList<QUrl> urls = dropEvent->mimeData()->urls();
                for (const QUrl& url : urls) {
                    if (url.isLocalFile()) {
                        const QString localPath = url.toLocalFile();
                        if (!localPath.isEmpty()) {
                            localPaths.append(localPath);
                        }
                    }
                }

                if (!localPaths.isEmpty()) {
                    emit localFilesDropRequested(localPaths, mapViewPointToScene(dropEvent->position()));
                    dropEvent->acceptProposedAction();
                    return true;
                }
            }
            break;
        }
        default:
            break;
        }
    }

    return QObject::eventFilter(watched, event);
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

    if (m_mediaScene) {
        disconnect(m_mediaScene, nullptr, this, nullptr);
    }

    m_mediaScene = scene;
    m_mediaItemsById.clear();

    if (m_mediaScene) {
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
        if (!media) {
            continue;
        }
        const QString mediaId = media->mediaId();
        if (mediaId.isEmpty()) {
            continue;
        }
        m_mediaItemsById.insert(mediaId, media);
    }
}

ResizableMediaBase* QuickCanvasController::mediaItemById(const QString& mediaId) {
    if (!m_mediaScene || mediaId.isEmpty()) {
        return nullptr;
    }

    auto it = m_mediaItemsById.constFind(mediaId);
    if (it != m_mediaItemsById.constEnd()) {
        ResizableMediaBase* candidate = it.value();
        if (candidate && candidate->scene() == m_mediaScene) {
            return candidate;
        }
    }

    rebuildMediaItemIndex();
    return m_mediaItemsById.value(mediaId, nullptr);
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

    // Suppress both scene::changed and scene::selectionChanged during the mutation
    // so neither triggers a full media-model republish that would destroy QML delegates.
    m_selectionMutationInProgress = true;
    if (!additive) {
        m_mediaScene->clearSelection();
    }
    target->setSelected(true);
    m_selectionMutationInProgress = false;

    // Cancel any pending media sync that was scheduled before this call
    // (e.g. from a scene::changed fired by a prior selection repaint).
    if (m_mediaSyncTimer) {
        m_mediaSyncTimer->stop();
    }
    m_mediaSyncPending = false;

    m_selectionStore->setSelectedMediaId(mediaId);
    pushSelectionAndSnapModels();
}

// Move drag is now fully QML-native (DragHandler on each mediaDelegate inside contentRoot).
// C++ is only notified once at drag-end to commit the final position.
void QuickCanvasController::handleMediaMoveEnded(const QString& mediaId, qreal sceneX, qreal sceneY) {
    if (!m_mediaScene || mediaId.isEmpty()) {
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
        movedTarget->setPos(QPointF(sceneX, sceneY));
    }
    m_pointerSession->setDraggingMedia(false);

    if (m_mediaSyncTimer) {
        m_mediaSyncTimer->stop();
    }
    m_mediaSyncPending = false;
    if (movedTarget) {
        const qreal currentScale = std::abs(movedTarget->scale()) > 1e-6
            ? std::abs(movedTarget->scale())
            : 1.0;
        if (!commitMediaTransform(mediaId, sceneX, sceneY, currentScale)) {
            pushMediaModelOnly();
        }
    } else {
        pushMediaModelOnly();
    }
    pushSelectionAndSnapModels();
}

void QuickCanvasController::handleMediaMoveStarted(const QString& mediaId, qreal sceneX, qreal sceneY) {
    Q_UNUSED(sceneX)
    Q_UNUSED(sceneY)
    if (!m_mediaScene || mediaId.isEmpty()) {
        return;
    }
    mediaItemById(mediaId);
}

void QuickCanvasController::handleMediaMoveUpdated(const QString& mediaId, qreal sceneX, qreal sceneY) {
    Q_UNUSED(sceneX)
    Q_UNUSED(sceneY)
    if (!m_mediaScene || mediaId.isEmpty()) {
        return;
    }
    mediaItemById(mediaId);
}

void QuickCanvasController::handleMediaResizeRequested(const QString& mediaId,
                                                       const QString& handleId,
                                                       qreal sceneX,
                                                       qreal sceneY,
                                                       bool snap) {
    if (!m_executingQueuedResize) {
        m_queuedResizeMediaId = mediaId;
        m_queuedResizeHandleId = handleId;
        m_queuedResizeSceneX = sceneX;
        m_queuedResizeSceneY = sceneY;
        m_queuedResizeSnap = snap;
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
        m_snapStore->clear();
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
        return;
    }

    if (m_resizeBaseSize.isEmpty()) {
        m_resizeBaseSize = baseSizeNow;
        m_resizeFixedItemPoint = fixedItemPoint;
        const qreal currentScaleAtStart = std::abs(target->scale()) > 1e-6 ? std::abs(target->scale()) : 1.0;
        m_resizeFixedScenePoint = target->scenePos() + m_resizeFixedItemPoint * currentScaleAtStart;
        m_resizeLastSceneX = target->scenePos().x();
        m_resizeLastSceneY = target->scenePos().y();
        m_resizeLastScale = currentScaleAtStart;
        beginLiveResizeSession(mediaId);
        buildResizeSnapCaches(target);
    }

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

    if (snap && !m_mediaScene->views().isEmpty()) {
        if (auto* screenCanvas = qobject_cast<ScreenCanvas*>(m_mediaScene->views().first())) {
            if (activeHandle == ResizableMediaBase::LeftMid || activeHandle == ResizableMediaBase::RightMid
                || activeHandle == ResizableMediaBase::TopMid || activeHandle == ResizableMediaBase::BottomMid) {
                proposedScale = applyAxisSnapWithCachedTargets(
                    target,
                    proposedScale,
                    fixedScenePoint,
                    baseSize,
                    activeHandle,
                    screenCanvas);
            } else {
                const qreal proposedW = proposedScale * baseSize.width();
                const qreal proposedH = proposedScale * baseSize.height();
                qreal snappedW = 0.0;
                qreal snappedH = 0.0;
                QPointF snappedCorner;
                if (applyCornerSnapWithCachedTargets(
                        activeHandle,
                        fixedScenePoint,
                        proposedW,
                        proposedH,
                        snappedW,
                        snappedH,
                        snappedCorner,
                        screenCanvas)) {
                    const qreal snappedScaleW = snappedW / std::max<qreal>(1.0, baseSize.width());
                    const qreal snappedScaleH = snappedH / std::max<qreal>(1.0, baseSize.height());
                    proposedScale = std::max<qreal>(0.05, std::max(snappedScaleW, snappedScaleH));
                }
            }
        }
    }

    proposedScale = std::clamp<qreal>(proposedScale, 0.05, 100.0);
    target->setScale(proposedScale);
    const QPointF snappedPos = fixedScenePoint - fixedItemPoint * proposedScale;
    target->setPos(snappedPos);
    m_resizeLastSceneX = snappedPos.x();
    m_resizeLastSceneY = snappedPos.y();
    m_resizeLastScale = proposedScale;
    // Lightweight live update for the active item only. Avoid rebuilding the
    // full model every pointer tick: it is expensive and can starve drag events.
    if (!pushLiveResizeGeometry(mediaId, snappedPos.x(), snappedPos.y(), proposedScale)) {
        // Fallback path if QML function is unavailable for any reason.
        pushMediaModelOnly();
    }
}

void QuickCanvasController::handleMediaResizeEnded(const QString& mediaId) {
    m_hasQueuedResize = false;
    if (m_resizeDispatchTimer) {
        m_resizeDispatchTimer->stop();
    }

    if (m_pointerSession->resizeActive() && !mediaId.isEmpty() && mediaId != m_pointerSession->resizeMediaId()) {
        return;
    }

    const QString finalMediaId = !mediaId.isEmpty() ? mediaId : m_pointerSession->resizeMediaId();
    qreal finalX = m_resizeLastSceneX;
    qreal finalY = m_resizeLastSceneY;
    qreal finalScale = m_resizeLastScale;
    bool haveFinal = !finalMediaId.isEmpty();
    if (m_mediaScene && !finalMediaId.isEmpty()) {
        const ResizableMediaBase* media = mediaItemById(finalMediaId);
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
    if (m_mediaSyncTimer) {
        m_mediaSyncTimer->stop();
    }
    m_mediaSyncPending = false;
    if (!endLiveResizeSession(finalMediaId, finalX, finalY, finalScale)) {
        if (haveFinal) {
            pushMediaModelOnly();
        }
    }
    // Media geometry is committed by endLiveResizeSession; refresh chrome/guides.
    pushSelectionAndSnapModels();
}

void QuickCanvasController::handleTextCommitRequested(const QString& mediaId, const QString& text) {
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

    textMedia->setText(text);
    textMedia->setSelected(true);
    scheduleMediaModelSync();
}

void QuickCanvasController::handleTextCreateRequested(qreal viewX, qreal viewY) {
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
        for (QGraphicsItem* graphicsItem : sceneItems) {
            auto* media = dynamic_cast<ResizableMediaBase*>(graphicsItem);
            if (!media) {
                continue;
            }

            const QString mediaId = media->mediaId();
            if (!mediaId.isEmpty()) {
                m_mediaItemsById.insert(mediaId, media);
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
            // NOTE: 'selected' is intentionally NOT included here.
            // Selection state is published separately via selectionChromeModel.
            // Including it here would cause the entire mediaModel to be replaced
            // on every selection change, destroying all QML delegates mid-gesture.
            mediaEntry.insert(QStringLiteral("sourcePath"), media->sourcePath());
            mediaEntry.insert(QStringLiteral("uploadState"), uploadStateToString(media->uploadState()));
            mediaEntry.insert(QStringLiteral("displayName"), media->displayName());

            if (auto* textMedia = dynamic_cast<TextMediaItem*>(media)) {
                const QFont textFont = textMedia->font();
                mediaEntry.insert(QStringLiteral("textContent"), textMedia->text());
                mediaEntry.insert(QStringLiteral("textHorizontalAlignment"), textHorizontalAlignmentToString(textMedia->horizontalAlignment()));
                mediaEntry.insert(QStringLiteral("textVerticalAlignment"), textVerticalAlignmentToString(textMedia->verticalAlignment()));
                mediaEntry.insert(QStringLiteral("fitToTextEnabled"), textMedia->fitToTextEnabled());
                mediaEntry.insert(QStringLiteral("textFontFamily"), textFont.family());
                mediaEntry.insert(QStringLiteral("textFontPixelSize"), textFont.pixelSize() > 0 ? textFont.pixelSize() : textFont.pointSize());
                mediaEntry.insert(QStringLiteral("textFontWeight"), textMedia->textFontWeightValue());
                mediaEntry.insert(QStringLiteral("textItalic"), textMedia->italicEnabled());
                mediaEntry.insert(QStringLiteral("textUnderline"), textMedia->underlineEnabled());
                mediaEntry.insert(QStringLiteral("textUppercase"), textMedia->uppercaseEnabled());
                mediaEntry.insert(QStringLiteral("textColor"), textMedia->textColor().name(QColor::HexArgb));
                mediaEntry.insert(QStringLiteral("textOutlineWidthPercent"), textMedia->textBorderWidth());
                mediaEntry.insert(QStringLiteral("textOutlineColor"), textMedia->textBorderColor().name(QColor::HexArgb));
                mediaEntry.insert(QStringLiteral("textHighlightEnabled"), textMedia->highlightEnabled());
                mediaEntry.insert(QStringLiteral("textHighlightColor"), textMedia->highlightColor().name(QColor::HexArgb));
                mediaEntry.insert(QStringLiteral("textEditable"), textMedia->isEditing());
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

qreal QuickCanvasController::applyAxisSnapWithCachedTargets(ResizableMediaBase* target,
                                                            qreal proposedScale,
                                                            const QPointF& fixedScenePoint,
                                                            const QSize& baseSize,
                                                            int activeHandleValue,
                                                            ScreenCanvas* screenCanvas) const {
    return SnapEngine::applyAxisSnapWithTargets(
        target,
        proposedScale,
        fixedScenePoint,
        baseSize,
        activeHandleValue,
        screenCanvas,
        *m_snapStore);
}

bool QuickCanvasController::applyCornerSnapWithCachedTargets(int activeHandleValue,
                                                              const QPointF& fixedScenePoint,
                                                              qreal proposedW,
                                                              qreal proposedH,
                                                              qreal& snappedW,
                                                              qreal& snappedH,
                                                              QPointF& snappedCorner,
                                                              ScreenCanvas* screenCanvas) const {
    return SnapEngine::applyCornerSnapWithTargets(
        activeHandleValue,
        fixedScenePoint,
        proposedW,
        proposedH,
        snappedW,
        snappedH,
        snappedCorner,
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

bool QuickCanvasController::commitMediaTransform(const QString& mediaId,
                                                 qreal sceneX,
                                                 qreal sceneY,
                                                 qreal scale) {
    if (!m_quickWidget || !m_quickWidget->rootObject() || mediaId.isEmpty()) {
        return false;
    }

    return GestureCommands::invokeMediaTransformCommand(
        m_quickWidget->rootObject(),
        "commitMediaTransform",
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

void QuickCanvasController::pushSelectionAndSnapModels() {
    if (!m_quickWidget || !m_quickWidget->rootObject()) {
        return;
    }

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

        if (!m_mediaScene->views().isEmpty()) {
            if (auto* screenCanvas = qobject_cast<ScreenCanvas*>(m_mediaScene->views().first())) {
                const QVector<QLineF> lines = screenCanvas->currentSnapGuideLines();
                for (const QLineF& line : lines) {
                    QVariantMap guideEntry;
                    guideEntry.insert(QStringLiteral("x1"), line.x1() * sceneUnitScale);
                    guideEntry.insert(QStringLiteral("y1"), line.y1() * sceneUnitScale);
                    guideEntry.insert(QStringLiteral("x2"), line.x2() * sceneUnitScale);
                    guideEntry.insert(QStringLiteral("y2"), line.y2() * sceneUnitScale);
                    snapGuidesModel.append(guideEntry);
                }
            }
        }
    }

    m_modelPublisher->publishSelectionAndSnapModels(m_viewAdapter, selectionChromeModel, snapGuidesModel);
}

void QuickCanvasController::pushStaticLayerModels() {
    if (!m_quickWidget || !m_quickWidget->rootObject()) {
        return;
    }

    QVariantList screensModel;
    QVariantList uiZonesModel;

    for (const auto& screen : m_sceneStore->screens()) {
        const QRectF rect = m_sceneStore->sceneScreenRects().value(screen.id);

        QVariantMap screenEntry;
        screenEntry.insert(QStringLiteral("screenId"), screen.id);
        screenEntry.insert(QStringLiteral("x"), rect.x());
        screenEntry.insert(QStringLiteral("y"), rect.y());
        screenEntry.insert(QStringLiteral("width"), rect.width());
        screenEntry.insert(QStringLiteral("height"), rect.height());
        screenEntry.insert(QStringLiteral("primary"), screen.primary);
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
