#include "frontend/rendering/canvas/QuickCanvasController.h"

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
}

QuickCanvasController::QuickCanvasController(QObject* parent)
    : QObject(parent)
{
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
        case QEvent::MouseButtonPress: {
            auto* mouseEvent = static_cast<QMouseEvent*>(event);
            if (m_textToolActive && mouseEvent && mouseEvent->button() == Qt::LeftButton) {
                emit textMediaCreateRequested(mapViewPointToScene(mouseEvent->position()));
                mouseEvent->accept();
                return true;
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
    if (screenListsEquivalent(m_screens, screens)) {
        return;
    }

    m_screens = screens;
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
    m_remoteCursorVisible = true;
    m_remoteCursorX = mapped.x();
    m_remoteCursorY = mapped.y();
    pushRemoteCursorState();
}

void QuickCanvasController::hideRemoteCursor() {
    m_remoteCursorVisible = false;
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
    for (auto it = m_sceneScreenRects.constBegin(); it != m_sceneScreenRects.constEnd(); ++it) {
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
    const QList<QGraphicsItem*> sceneItems = m_mediaScene->items();
    for (QGraphicsItem* graphicsItem : sceneItems) {
        auto* media = dynamic_cast<ResizableMediaBase*>(graphicsItem);
        if (!media || media->mediaId() != mediaId) {
            continue;
        }
        media->setPos(QPointF(sceneX, sceneY));
        break;
    }
    m_draggingMedia = false;

    if (m_mediaSyncTimer) {
        m_mediaSyncTimer->stop();
    }
    m_mediaSyncPending = false;
    syncMediaModelFromScene();
}

void QuickCanvasController::handleMediaResizeRequested(const QString& mediaId,
                                                       const QString& handleId,
                                                       qreal sceneX,
                                                       qreal sceneY,
                                                       bool snap) {
    if (!m_mediaScene || mediaId.isEmpty() || handleId.isEmpty()) {
        return;
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
        return;
    }

    const QSize baseSize = target->baseSizePx();
    if (baseSize.width() <= 0 || baseSize.height() <= 0) {
        return;
    }

    ResizableMediaBase::Handle activeHandle = ResizableMediaBase::None;
    QPointF fixedItemPoint;

    if (handleId == QStringLiteral("top-left")) {
        activeHandle = ResizableMediaBase::TopLeft;
        fixedItemPoint = QPointF(baseSize.width(), baseSize.height());
    } else if (handleId == QStringLiteral("top-mid")) {
        activeHandle = ResizableMediaBase::TopMid;
        fixedItemPoint = QPointF(baseSize.width() * 0.5, baseSize.height());
    } else if (handleId == QStringLiteral("top-right")) {
        activeHandle = ResizableMediaBase::TopRight;
        fixedItemPoint = QPointF(0.0, baseSize.height());
    } else if (handleId == QStringLiteral("left-mid")) {
        activeHandle = ResizableMediaBase::LeftMid;
        fixedItemPoint = QPointF(baseSize.width(), baseSize.height() * 0.5);
    } else if (handleId == QStringLiteral("right-mid")) {
        activeHandle = ResizableMediaBase::RightMid;
        fixedItemPoint = QPointF(0.0, baseSize.height() * 0.5);
    } else if (handleId == QStringLiteral("bottom-left")) {
        activeHandle = ResizableMediaBase::BottomLeft;
        fixedItemPoint = QPointF(baseSize.width(), 0.0);
    } else if (handleId == QStringLiteral("bottom-mid")) {
        activeHandle = ResizableMediaBase::BottomMid;
        fixedItemPoint = QPointF(baseSize.width() * 0.5, 0.0);
    } else if (handleId == QStringLiteral("bottom-right")) {
        activeHandle = ResizableMediaBase::BottomRight;
        fixedItemPoint = QPointF(0.0, 0.0);
    } else {
        return;
    }

    const qreal currentScale = std::abs(target->scale()) > 1e-6 ? std::abs(target->scale()) : 1.0;
    const QPointF fixedScenePoint = target->scenePos() + fixedItemPoint * currentScale;
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
                proposedScale = screenCanvas->applyAxisSnapWithHysteresis(
                    target,
                    proposedScale,
                    fixedScenePoint,
                    baseSize,
                    activeHandle);
            } else {
                const qreal proposedW = proposedScale * baseSize.width();
                const qreal proposedH = proposedScale * baseSize.height();
                const auto cornerSnap = screenCanvas->applyCornerAltSnapWithHysteresis(
                    target,
                    activeHandle,
                    fixedScenePoint,
                    baseSize,
                    proposedW,
                    proposedH);
                if (cornerSnap.cornerSnapped) {
                    const qreal snappedScaleW = cornerSnap.snappedW / std::max<qreal>(1.0, baseSize.width());
                    const qreal snappedScaleH = cornerSnap.snappedH / std::max<qreal>(1.0, baseSize.height());
                    proposedScale = std::max<qreal>(0.05, std::max(snappedScaleW, snappedScaleH));
                }
            }
        }
    }

    proposedScale = std::clamp<qreal>(proposedScale, 0.05, 100.0);
    target->setScale(proposedScale);
    const QPointF snappedPos = fixedScenePoint - fixedItemPoint * proposedScale;
    target->setPos(snappedPos);
    // Push only media positions — same reason as handleMediaMoveRequested.
    pushMediaModelOnly();
}

void QuickCanvasController::handleMediaResizeEnded(const QString& mediaId) {
    Q_UNUSED(mediaId)
    m_draggingMedia = false;
    scheduleMediaModelSync();
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

            const qreal sceneUnitScale = (m_sceneUnitScale > 1e-6) ? m_sceneUnitScale : 1.0;

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

void QuickCanvasController::pushSelectionAndSnapModels() {
    if (!m_quickWidget || !m_quickWidget->rootObject()) {
        return;
    }

    QVariantList selectionChromeModel;
    QVariantList snapGuidesModel;

    if (m_mediaScene) {
        const qreal sceneUnitScale = (m_sceneUnitScale > 1e-6) ? m_sceneUnitScale : 1.0;

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

    for (const auto& screen : m_screens) {
        const QRectF rect = m_sceneScreenRects.value(screen.id);

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
    m_quickWidget->rootObject()->setProperty("remoteCursorVisible", m_remoteCursorVisible);
    m_quickWidget->rootObject()->setProperty("remoteCursorX", m_remoteCursorX);
    m_quickWidget->rootObject()->setProperty("remoteCursorY", m_remoteCursorY);
    m_quickWidget->rootObject()->setProperty("remoteCursorDiameter", kRemoteCursorDiameterPx);
    m_quickWidget->rootObject()->setProperty("remoteCursorFill", QStringLiteral("#FFFFFFFF"));
    m_quickWidget->rootObject()->setProperty("remoteCursorBorder", QStringLiteral("#E6000000"));
    m_quickWidget->rootObject()->setProperty("remoteCursorBorderWidth", kRemoteCursorBorderWidthPx);
}

QPointF QuickCanvasController::mapRemoteCursorToQuickScene(int globalX, int globalY, bool* ok) const {
    if (ok) {
        *ok = false;
    }
    if (m_screens.isEmpty() || m_sceneScreenRects.isEmpty()) {
        return {};
    }

    const ScreenInfo* containing = nullptr;
    for (const auto& screen : m_screens) {
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

    const auto rectIt = m_sceneScreenRects.constFind(containing->id);
    if (rectIt == m_sceneScreenRects.constEnd()) {
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
    m_sceneScreenRects.clear();
    if (m_screens.isEmpty()) {
        m_sceneUnitScale = 1.0;
        return;
    }

    m_sceneUnitScale = currentSceneUnitScale();

    int minX = std::numeric_limits<int>::max();
    int minY = std::numeric_limits<int>::max();
    for (const auto& screen : m_screens) {
        minX = std::min(minX, screen.x);
        minY = std::min(minY, screen.y);
    }
    if (minX == std::numeric_limits<int>::max()) {
        minX = 0;
    }
    if (minY == std::numeric_limits<int>::max()) {
        minY = 0;
    }

    for (const auto& screen : m_screens) {
        m_sceneScreenRects.insert(
            screen.id,
            QRectF(
                static_cast<qreal>(screen.x - minX) * m_sceneUnitScale,
                static_cast<qreal>(screen.y - minY) * m_sceneUnitScale,
                static_cast<qreal>(screen.width) * m_sceneUnitScale,
                static_cast<qreal>(screen.height) * m_sceneUnitScale));
    }
}

void QuickCanvasController::refreshSceneUnitScaleIfNeeded(bool force) {
    m_pendingInitialSceneScaleRefresh = false;

    const qreal nextScale = currentSceneUnitScale();
    if (!force && qFuzzyCompare(1.0 + nextScale, 1.0 + m_sceneUnitScale)) {
        return;
    }

    const qreal oldScale = m_sceneUnitScale;
    if (m_screens.isEmpty()) {
        m_sceneUnitScale = nextScale;
        return;
    }

    rebuildScreenRects();
    pushStaticLayerModels();
    syncMediaModelFromScene();

    if (m_remoteCursorVisible && oldScale > 0.0 && nextScale > 0.0) {
        const qreal ratio = nextScale / oldScale;
        m_remoteCursorX *= ratio;
        m_remoteCursorY *= ratio;
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

QRectF QuickCanvasController::scaleSceneRect(const QRectF& rect) const {
    if (m_sceneUnitScale == 1.0) {
        return rect;
    }

    return QRectF(
        rect.x() * m_sceneUnitScale,
        rect.y() * m_sceneUnitScale,
        rect.width() * m_sceneUnitScale,
        rect.height() * m_sceneUnitScale);
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
    const qreal invSceneUnitScale = (m_sceneUnitScale > 1e-6) ? (1.0 / m_sceneUnitScale) : 1.0;

    return { quickSceneX * invSceneUnitScale, quickSceneY * invSceneUnitScale };
}

void QuickCanvasController::scheduleInitialFitIfNeeded(int marginPx) {
    if (m_initialFitCompleted || m_screens.isEmpty()) {
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
    if (!m_quickWidget || !m_quickWidget->rootObject() || m_screens.isEmpty()) {
        return false;
    }

    const QSize widgetSize = m_quickWidget->size();
    if (widgetSize.width() < 48 || widgetSize.height() < 48) {
        return false;
    }

    QRectF bounds;
    bool hasBounds = false;
    for (auto it = m_sceneScreenRects.constBegin(); it != m_sceneScreenRects.constEnd(); ++it) {
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
