#include "frontend/rendering/canvas/QuickCanvasController.h"
#include "frontend/rendering/canvas/CanvasSceneStore.h"

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

bool quickCanvasInteractionDebugEnabled() {
    static const bool enabled = qEnvironmentVariableIntValue("MOUFFETTE_CURSOR_DEBUG") > 0;
    return enabled;
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

    m_mediaSyncTimer = new QTimer(this);
    m_mediaSyncTimer->setSingleShot(true);
    m_mediaSyncTimer->setInterval(16);
    connect(m_mediaSyncTimer, &QTimer::timeout, this, [this]() {
        syncMediaModelFromScene();
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

    setScreenCount(0);
    setShellActive(false);
    setTextToolActive(false);
    setScreens({});
    m_quickWidget->rootObject()->setProperty("mediaModel", QVariantList{});
    m_quickWidget->rootObject()->setProperty("selectionChromeModel", QVariantList{});
    m_quickWidget->rootObject()->setProperty("snapGuidesModel", QVariantList{});

    QObject::connect(
        m_quickWidget->rootObject(), SIGNAL(mediaSelectRequested(QString,bool)),
        this, SLOT(handleMediaSelectRequested(QString,bool)));
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

    if (m_mediaScene) {
        connect(m_mediaScene, &QGraphicsScene::changed, this, [this](const QList<QRectF>&) {
            if (!m_draggingMedia) scheduleMediaModelSync();
        });
        connect(m_mediaScene, &QGraphicsScene::selectionChanged, this, [this]() {
            if (!m_draggingMedia) scheduleMediaModelSync();
        });
    }

    scheduleMediaModelSync();
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

    ResizableMediaBase* target = nullptr;
    const QList<QGraphicsItem*> sceneItems = m_mediaScene->items();
    for (QGraphicsItem* graphicsItem : sceneItems) {
        auto* media = dynamic_cast<ResizableMediaBase*>(graphicsItem);
        if (!media) {
            continue;
        }
        if (media->mediaId() == mediaId) {
            target = media;
            break;
        }
    }

    if (!target) {
        return;
    }

    const bool alreadyOnlySelected = target->isSelected()
        && m_mediaScene->selectedItems().size() == 1;
    if (!additive && alreadyOnlySelected) {
        return;
    }

    if (additive && target->isSelected()) {
        return;
    }

    if (!additive) {
        m_mediaScene->clearSelection();
    }
    target->setSelected(true);
    scheduleMediaModelSync();
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

    m_draggingMedia = true;
    ResizableMediaBase* movedTarget = nullptr;
    const QList<QGraphicsItem*> sceneItems = m_mediaScene->items();
    for (QGraphicsItem* graphicsItem : sceneItems) {
        auto* media = dynamic_cast<ResizableMediaBase*>(graphicsItem);
        if (!media || media->mediaId() != mediaId) {
            continue;
        }
        movedTarget = media;
        media->setPos(QPointF(sceneX, sceneY));
        break;
    }
    m_draggingMedia = false;

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

void QuickCanvasController::handleMediaResizeRequested(const QString& mediaId,
                                                       const QString& handleId,
                                                       qreal sceneX,
                                                       qreal sceneY,
                                                       bool snap) {
    if (!m_mediaScene || mediaId.isEmpty() || handleId.isEmpty()) {
        m_draggingMedia = false;
        return;
    }

    if (m_resizeActive) {
        if (mediaId != m_resizeMediaId || handleId != m_resizeHandleId) {
            if (quickCanvasInteractionDebugEnabled()) {
                qInfo() << "[QuickCanvas][Resize] Ignored mismatched request"
                        << "active" << m_resizeMediaId << m_resizeHandleId
                        << "incoming" << mediaId << handleId;
            }
            return;
        }
    } else {
        m_resizeActive = true;
        m_resizeMediaId = mediaId;
        m_resizeHandleId = handleId;
        m_resizeBaseSize = QSize();
        m_resizeFixedItemPoint = QPointF();
        m_resizeFixedScenePoint = QPointF();
        m_resizeSnapCacheReady = false;
        m_resizeSnapEdgesX.clear();
        m_resizeSnapEdgesY.clear();
        m_resizeSnapCorners.clear();
        if (m_mediaSyncTimer) {
            m_mediaSyncTimer->stop();
        }
        m_mediaSyncPending = false;
    }

    m_draggingMedia = true;

    ResizableMediaBase* target = nullptr;
    const QList<QGraphicsItem*> sceneItems = m_mediaScene->items();
    for (QGraphicsItem* graphicsItem : sceneItems) {
        auto* media = dynamic_cast<ResizableMediaBase*>(graphicsItem);
        if (!media || media->mediaId() != mediaId) {
            continue;
        }
        target = media;
        break;
    }
    if (!target) {
        m_draggingMedia = false;
        endLiveResizeSession(m_resizeMediaId, m_resizeLastSceneX, m_resizeLastSceneY, m_resizeLastScale);
        m_resizeActive = false;
        m_resizeMediaId.clear();
        m_resizeHandleId.clear();
        m_resizeBaseSize = QSize();
        m_resizeFixedItemPoint = QPointF();
        m_resizeFixedScenePoint = QPointF();
        return;
    }

    const QSize baseSizeNow = target->baseSizePx();
    if (baseSizeNow.width() <= 0 || baseSizeNow.height() <= 0) {
        m_draggingMedia = false;
        endLiveResizeSession(m_resizeMediaId, m_resizeLastSceneX, m_resizeLastSceneY, m_resizeLastScale);
        m_resizeActive = false;
        m_resizeMediaId.clear();
        m_resizeHandleId.clear();
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
        m_draggingMedia = false;
        endLiveResizeSession(m_resizeMediaId, m_resizeLastSceneX, m_resizeLastSceneY, m_resizeLastScale);
        m_resizeActive = false;
        m_resizeMediaId.clear();
        m_resizeHandleId.clear();
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
        if (quickCanvasInteractionDebugEnabled()) {
            qInfo() << "[QuickCanvas][Resize] Begin session"
                    << mediaId << handleId
                    << "base" << m_resizeBaseSize
                    << "fixedScene" << m_resizeFixedScenePoint;
        }
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
    if (m_resizeActive && !mediaId.isEmpty() && mediaId != m_resizeMediaId) {
        return;
    }

    const QString finalMediaId = !mediaId.isEmpty() ? mediaId : m_resizeMediaId;
    qreal finalX = m_resizeLastSceneX;
    qreal finalY = m_resizeLastSceneY;
    qreal finalScale = m_resizeLastScale;
    bool haveFinal = !finalMediaId.isEmpty();
    if (m_mediaScene && !finalMediaId.isEmpty()) {
        const QList<QGraphicsItem*> sceneItems = m_mediaScene->items();
        for (QGraphicsItem* graphicsItem : sceneItems) {
            auto* media = dynamic_cast<ResizableMediaBase*>(graphicsItem);
            if (!media || media->mediaId() != finalMediaId) {
                continue;
            }
            finalX = media->scenePos().x();
            finalY = media->scenePos().y();
            finalScale = std::abs(media->scale()) > 1e-6 ? std::abs(media->scale()) : 1.0;
            haveFinal = true;
            break;
        }
    }

    m_draggingMedia = false;
    m_resizeActive = false;
    m_resizeMediaId.clear();
    m_resizeHandleId.clear();
    m_resizeBaseSize = QSize();
    m_resizeFixedItemPoint = QPointF();
    m_resizeFixedScenePoint = QPointF();
    m_resizeSnapCacheReady = false;
    m_resizeSnapEdgesX.clear();
    m_resizeSnapEdgesY.clear();
    m_resizeSnapCorners.clear();
    if (m_mediaSyncTimer) {
        m_mediaSyncTimer->stop();
    }
    m_mediaSyncPending = false;
    if (!endLiveResizeSession(finalMediaId, finalX, finalY, finalScale)) {
        if (haveFinal) {
            pushMediaModelOnly();
        }
    }
    if (quickCanvasInteractionDebugEnabled()) {
        qInfo() << "[QuickCanvas][Resize] End session"
                << finalMediaId
                << "final" << finalX << finalY << finalScale;
    }
    // Media geometry is committed by endLiveResizeSession; refresh chrome/guides.
    pushSelectionAndSnapModels();
}

void QuickCanvasController::handleTextCommitRequested(const QString& mediaId, const QString& text) {
    if (!m_mediaScene || mediaId.isEmpty()) {
        return;
    }

    const QList<QGraphicsItem*> sceneItems = m_mediaScene->items();
    for (QGraphicsItem* graphicsItem : sceneItems) {
        auto* media = dynamic_cast<ResizableMediaBase*>(graphicsItem);
        if (!media || media->mediaId() != mediaId) {
            continue;
        }

        auto* textMedia = dynamic_cast<TextMediaItem*>(media);
        if (!textMedia) {
            return;
        }

        textMedia->setText(text);
        textMedia->setSelected(true);
        scheduleMediaModelSync();
        return;
    }
}

void QuickCanvasController::handleTextCreateRequested(qreal viewX, qreal viewY) {
    emit textMediaCreateRequested(mapViewPointToScene(QPointF(viewX, viewY)));
}

void QuickCanvasController::syncMediaModelFromScene() {
    if (m_draggingMedia || m_resizeActive) {
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

    QVariantList mediaModel;
    if (m_mediaScene) {
        const QList<QGraphicsItem*> sceneItems = m_mediaScene->items();
        for (QGraphicsItem* graphicsItem : sceneItems) {
            auto* media = dynamic_cast<ResizableMediaBase*>(graphicsItem);
            if (!media) {
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

            const qreal sceneUnitScale = (m_sceneStore->sceneUnitScale() > 1e-6) ? m_sceneStore->sceneUnitScale() : 1.0;

            QVariantMap mediaEntry;
            mediaEntry.insert(QStringLiteral("mediaId"), media->mediaId());
            mediaEntry.insert(QStringLiteral("mediaType"), media->isTextMedia()
                ? QStringLiteral("text")
                : (media->isVideoMedia() ? QStringLiteral("video") : QStringLiteral("image")));
            mediaEntry.insert(QStringLiteral("x"), scenePos.x() * sceneUnitScale);
            mediaEntry.insert(QStringLiteral("y"), scenePos.y() * sceneUnitScale);
            mediaEntry.insert(QStringLiteral("width"), std::max<qreal>(1.0, baseWidth * sceneUnitScale));
            mediaEntry.insert(QStringLiteral("height"), std::max<qreal>(1.0, baseHeight * sceneUnitScale));
            mediaEntry.insert(QStringLiteral("scale"), mediaScale);
            mediaEntry.insert(QStringLiteral("z"), media->zValue());
            mediaEntry.insert(QStringLiteral("selected"), media->isSelected());
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

    m_quickWidget->rootObject()->setProperty("mediaModel", mediaModel);
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
    m_resizeSnapCacheReady = false;
    m_resizeSnapEdgesX.clear();
    m_resizeSnapEdgesY.clear();
    m_resizeSnapCorners.clear();

    if (!resizingItem) {
        return;
    }

    for (auto it = m_sceneStore->sceneScreenRects().constBegin(); it != m_sceneStore->sceneScreenRects().constEnd(); ++it) {
        const QRectF& sr = it.value();
        m_resizeSnapEdgesX.append(sr.left());
        m_resizeSnapEdgesX.append(sr.right());
        m_resizeSnapEdgesY.append(sr.top());
        m_resizeSnapEdgesY.append(sr.bottom());
        m_resizeSnapCorners.append(sr.topLeft());
        m_resizeSnapCorners.append(sr.topRight());
        m_resizeSnapCorners.append(sr.bottomLeft());
        m_resizeSnapCorners.append(sr.bottomRight());
    }

    if (m_mediaScene) {
        const QList<QGraphicsItem*> sceneItems = m_mediaScene->items();
        for (QGraphicsItem* graphicsItem : sceneItems) {
            auto* media = dynamic_cast<ResizableMediaBase*>(graphicsItem);
            if (!media || media == resizingItem) {
                continue;
            }
            const QRectF r = media->sceneBoundingRect();
            m_resizeSnapEdgesX.append(r.left());
            m_resizeSnapEdgesX.append(r.right());
            m_resizeSnapEdgesY.append(r.top());
            m_resizeSnapEdgesY.append(r.bottom());
            m_resizeSnapCorners.append(r.topLeft());
            m_resizeSnapCorners.append(r.topRight());
            m_resizeSnapCorners.append(r.bottomLeft());
            m_resizeSnapCorners.append(r.bottomRight());
        }
    }

    sortAndDeduplicate<qreal>(m_resizeSnapEdgesX, [](qreal a, qreal b) { return std::abs(a - b) < 1e-6; });
    sortAndDeduplicate<qreal>(m_resizeSnapEdgesY, [](qreal a, qreal b) { return std::abs(a - b) < 1e-6; });
    std::sort(m_resizeSnapCorners.begin(), m_resizeSnapCorners.end(), [](const QPointF& lhs, const QPointF& rhs) {
        if (std::abs(lhs.x() - rhs.x()) > 1e-6) {
            return lhs.x() < rhs.x();
        }
        return lhs.y() < rhs.y();
    });
    m_resizeSnapCorners.erase(std::unique(m_resizeSnapCorners.begin(), m_resizeSnapCorners.end(), [](const QPointF& a, const QPointF& b) {
        return std::abs(a.x() - b.x()) < 1e-6 && std::abs(a.y() - b.y()) < 1e-6;
    }), m_resizeSnapCorners.end());

    m_resizeSnapCacheReady = true;
}

qreal QuickCanvasController::applyAxisSnapWithCachedTargets(ResizableMediaBase* target,
                                                            qreal proposedScale,
                                                            const QPointF& fixedScenePoint,
                                                            const QSize& baseSize,
                                                            int activeHandleValue,
                                                            ScreenCanvas* screenCanvas) const {
    if (!target || !screenCanvas || !m_resizeSnapCacheReady) {
        return proposedScale;
    }

    using H = ResizableMediaBase::Handle;
    const H activeHandle = static_cast<H>(activeHandleValue);
    const bool isHorizontal = (activeHandle == H::LeftMid || activeHandle == H::RightMid);
    const bool isVertical = (activeHandle == H::TopMid || activeHandle == H::BottomMid);
    if (!isHorizontal && !isVertical) {
        return proposedScale;
    }
    if (!QGuiApplication::keyboardModifiers().testFlag(Qt::ShiftModifier)) {
        return proposedScale;
    }

    const QVector<qreal>& axisTargets = isHorizontal ? m_resizeSnapEdgesX : m_resizeSnapEdgesY;
    if (axisTargets.isEmpty()) {
        return proposedScale;
    }

    const QTransform t = screenCanvas->transform();
    const qreal snapDistanceScene = screenCanvas->snapDistancePx() / (t.m11() > 1e-6 ? t.m11() : 1.0);
    constexpr qreal releaseFactor = 1.4;
    const qreal releaseDist = snapDistanceScene * releaseFactor;

    const qreal halfW = (baseSize.width() * proposedScale) / 2.0;
    const qreal halfH = (baseSize.height() * proposedScale) / 2.0;
    const qreal movingEdgePos = [&]() {
        switch (activeHandle) {
        case H::LeftMid: return fixedScenePoint.x() - 2 * halfW;
        case H::RightMid: return fixedScenePoint.x() + 2 * halfW;
        case H::TopMid: return fixedScenePoint.y() - 2 * halfH;
        case H::BottomMid: return fixedScenePoint.y() + 2 * halfH;
        default: return 0.0;
        }
    }();

    auto computeScaleFor = [&](qreal edgeScenePos) {
        if (isHorizontal) {
            const qreal desiredHalfWidth = (activeHandle == H::LeftMid)
                ? (fixedScenePoint.x() - edgeScenePos) / 2.0
                : (edgeScenePos - fixedScenePoint.x()) / 2.0;
            if (desiredHalfWidth <= 0) return proposedScale;
            return (desiredHalfWidth * 2.0) / baseSize.width();
        }
        const qreal desiredHalfHeight = (activeHandle == H::TopMid)
            ? (fixedScenePoint.y() - edgeScenePos) / 2.0
            : (edgeScenePos - fixedScenePoint.y()) / 2.0;
        if (desiredHalfHeight <= 0) return proposedScale;
        return (desiredHalfHeight * 2.0) / baseSize.height();
    };

    bool snapActive = target->isAxisSnapActive();
    const H snapHandle = target->axisSnapHandle();
    const qreal snapTargetScale = target->axisSnapTargetScale();
    if (snapActive && snapHandle == activeHandle) {
        auto snappedEdgePosForScale = [&](qreal s) {
            const qreal hw = (baseSize.width() * s) / 2.0;
            const qreal hh = (baseSize.height() * s) / 2.0;
            switch (activeHandle) {
            case H::LeftMid: return fixedScenePoint.x() - 2 * hw;
            case H::RightMid: return fixedScenePoint.x() + 2 * hw;
            case H::TopMid: return fixedScenePoint.y() - 2 * hh;
            case H::BottomMid: return fixedScenePoint.y() + 2 * hh;
            default: return 0.0;
            }
        };
        const qreal snappedEdgePos = snappedEdgePosForScale(snapTargetScale);
        const qreal distToLocked = std::abs(movingEdgePos - snappedEdgePos);
        if (distToLocked <= releaseDist) {
            return snapTargetScale;
        }
        target->setAxisSnapActive(false, H::None, 0.0);
        snapActive = false;
    }

    qreal bestDist = snapDistanceScene;
    qreal bestScale = proposedScale;
    for (qreal edge : axisTargets) {
        const qreal dist = std::abs(movingEdgePos - edge);
        if (dist < bestDist) {
            const qreal candidateScale = computeScaleFor(edge);
            if (candidateScale > 0.0) {
                bestDist = dist;
                bestScale = candidateScale;
            }
        }
    }

    if (!snapActive && bestScale != proposedScale && bestDist < snapDistanceScene) {
        target->setAxisSnapActive(true, activeHandle, bestScale);
    }

    return bestScale;
}

bool QuickCanvasController::applyCornerSnapWithCachedTargets(int activeHandleValue,
                                                              const QPointF& fixedScenePoint,
                                                              qreal proposedW,
                                                              qreal proposedH,
                                                              qreal& snappedW,
                                                              qreal& snappedH,
                                                              QPointF& snappedCorner,
                                                              ScreenCanvas* screenCanvas) const {
    const ResizableMediaBase::Handle activeHandleEnum = static_cast<ResizableMediaBase::Handle>(activeHandleValue);
    if (!screenCanvas || !m_resizeSnapCacheReady || m_resizeSnapCorners.isEmpty()) {
        return false;
    }

    using H = ResizableMediaBase::Handle;
    const bool isCorner = (activeHandleEnum == H::TopLeft || activeHandleEnum == H::TopRight
        || activeHandleEnum == H::BottomLeft || activeHandleEnum == H::BottomRight);
    if (!isCorner) {
        return false;
    }
    if (!QGuiApplication::keyboardModifiers().testFlag(Qt::ShiftModifier)) {
        return false;
    }

    const QTransform t = screenCanvas->transform();
    const qreal cornerZone = screenCanvas->cornerSnapDistancePx() / (t.m11() > 1e-6 ? t.m11() : 1.0);

    auto movingCornerPoint = [&](qreal w, qreal h) {
        QPointF tl;
        if (activeHandleEnum == H::TopLeft) {
            tl = QPointF(fixedScenePoint.x() - w, fixedScenePoint.y() - h);
            return tl;
        }
        if (activeHandleEnum == H::TopRight) {
            tl = QPointF(fixedScenePoint.x(), fixedScenePoint.y() - h);
            return QPointF(tl.x() + w, tl.y());
        }
        if (activeHandleEnum == H::BottomLeft) {
            tl = QPointF(fixedScenePoint.x() - w, fixedScenePoint.y());
            return QPointF(tl.x(), tl.y() + h);
        }
        tl = QPointF(fixedScenePoint.x(), fixedScenePoint.y());
        return QPointF(tl.x() + w, tl.y() + h);
    };

    const QPointF candidate = movingCornerPoint(proposedW, proposedH);
    qreal bestErr = std::numeric_limits<qreal>::max();
    QPointF bestTarget;
    for (const QPointF& targetCorner : m_resizeSnapCorners) {
        const qreal dx = std::abs(candidate.x() - targetCorner.x());
        const qreal dy = std::abs(candidate.y() - targetCorner.y());
        if (dx > cornerZone || dy > cornerZone) {
            continue;
        }
        const qreal err = std::hypot(dx, dy);
        if (err < bestErr) {
            bestErr = err;
            bestTarget = targetCorner;
        }
    }

    if (bestErr == std::numeric_limits<qreal>::max()) {
        return false;
    }

    qreal outW = proposedW;
    qreal outH = proposedH;
    if (activeHandleEnum == H::TopLeft) {
        outW = fixedScenePoint.x() - bestTarget.x();
        outH = fixedScenePoint.y() - bestTarget.y();
    } else if (activeHandleEnum == H::TopRight) {
        outW = bestTarget.x() - fixedScenePoint.x();
        outH = fixedScenePoint.y() - bestTarget.y();
    } else if (activeHandleEnum == H::BottomLeft) {
        outW = fixedScenePoint.x() - bestTarget.x();
        outH = bestTarget.y() - fixedScenePoint.y();
    } else {
        outW = bestTarget.x() - fixedScenePoint.x();
        outH = bestTarget.y() - fixedScenePoint.y();
    }

    if (outW <= 0.0 || outH <= 0.0) {
        return false;
    }

    snappedW = outW;
    snappedH = outH;
    snappedCorner = bestTarget;
    return true;
}

bool QuickCanvasController::endLiveResizeSession(const QString& mediaId,
                                                 qreal sceneX,
                                                 qreal sceneY,
                                                 qreal scale) {
    if (!m_quickWidget || !m_quickWidget->rootObject() || mediaId.isEmpty()) {
        return false;
    }

    const qreal sceneUnitScale = (m_sceneStore->sceneUnitScale() > 1e-6) ? m_sceneStore->sceneUnitScale() : 1.0;
    return QMetaObject::invokeMethod(
        m_quickWidget->rootObject(),
        "endLiveResize",
        Q_ARG(QVariant, mediaId),
        Q_ARG(QVariant, sceneX * sceneUnitScale),
        Q_ARG(QVariant, sceneY * sceneUnitScale),
        Q_ARG(QVariant, scale));
}

bool QuickCanvasController::commitMediaTransform(const QString& mediaId,
                                                 qreal sceneX,
                                                 qreal sceneY,
                                                 qreal scale) {
    if (!m_quickWidget || !m_quickWidget->rootObject() || mediaId.isEmpty()) {
        return false;
    }

    const qreal sceneUnitScale = (m_sceneStore->sceneUnitScale() > 1e-6) ? m_sceneStore->sceneUnitScale() : 1.0;
    return QMetaObject::invokeMethod(
        m_quickWidget->rootObject(),
        "commitMediaTransform",
        Q_ARG(QVariant, mediaId),
        Q_ARG(QVariant, sceneX * sceneUnitScale),
        Q_ARG(QVariant, sceneY * sceneUnitScale),
        Q_ARG(QVariant, scale));
}

bool QuickCanvasController::pushLiveResizeGeometry(const QString& mediaId,
                                                   qreal sceneX,
                                                   qreal sceneY,
                                                   qreal scale) {
    if (!m_quickWidget || !m_quickWidget->rootObject() || mediaId.isEmpty()) {
        return false;
    }

    const qreal sceneUnitScale = (m_sceneStore->sceneUnitScale() > 1e-6) ? m_sceneStore->sceneUnitScale() : 1.0;
    return QMetaObject::invokeMethod(
        m_quickWidget->rootObject(),
        "applyLiveResizeGeometry",
        Q_ARG(QVariant, mediaId),
        Q_ARG(QVariant, sceneX * sceneUnitScale),
        Q_ARG(QVariant, sceneY * sceneUnitScale),
        Q_ARG(QVariant, scale));
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

    m_quickWidget->rootObject()->setProperty("selectionChromeModel", selectionChromeModel);
    m_quickWidget->rootObject()->setProperty("snapGuidesModel", snapGuidesModel);
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
