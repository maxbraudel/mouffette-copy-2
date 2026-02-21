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
#include <QTimer>
#include <QVariantList>
#include <QWindow>
#include <QtMath>

#include "backend/domain/media/MediaItems.h"
#include "backend/domain/media/TextMediaItem.h"

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

    QObject::connect(
        m_quickWidget->rootObject(), SIGNAL(mediaSelectRequested(QString,bool)),
        this, SLOT(handleMediaSelectRequested(QString,bool)));
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
    return true;
}

bool QuickCanvasController::eventFilter(QObject* watched, QEvent* event) {
    if (watched == m_quickWidget && event) {
        switch (event->type()) {
        case QEvent::Show:
        case QEvent::Polish:
        case QEvent::PolishRequest:
        case QEvent::ScreenChangeInternal:
            refreshSceneUnitScaleIfNeeded();
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
    m_screens = screens;
    setScreenCount(screens.size());
    rebuildScreenRects();
    pushStaticLayerModels();
    if (screens.isEmpty()) {
        hideRemoteCursor();
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
            scheduleMediaModelSync();
        });
        connect(m_mediaScene, &QGraphicsScene::selectionChanged, this, [this]() {
            scheduleMediaModelSync();
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

    if (!additive) {
        m_mediaScene->clearSelection();
    }
    target->setSelected(true);
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

    QVariantList mediaModel;
    if (m_mediaScene) {
        const QList<QGraphicsItem*> sceneItems = m_mediaScene->items();
        for (QGraphicsItem* graphicsItem : sceneItems) {
            auto* media = dynamic_cast<ResizableMediaBase*>(graphicsItem);
            if (!media) {
                continue;
            }

            QRectF sceneRect;
            const QSize baseSize = media->baseSizePx();
            if (baseSize.width() > 0 && baseSize.height() > 0) {
                sceneRect = media->mapRectToScene(QRectF(QPointF(0.0, 0.0), QSizeF(baseSize)));
            } else {
                sceneRect = media->sceneBoundingRect();
            }
            sceneRect = scaleSceneRect(sceneRect.normalized());

            QVariantMap mediaEntry;
            mediaEntry.insert(QStringLiteral("mediaId"), media->mediaId());
            mediaEntry.insert(QStringLiteral("mediaType"), media->isTextMedia()
                ? QStringLiteral("text")
                : (media->isVideoMedia() ? QStringLiteral("video") : QStringLiteral("image")));
            mediaEntry.insert(QStringLiteral("x"), sceneRect.x());
            mediaEntry.insert(QStringLiteral("y"), sceneRect.y());
            mediaEntry.insert(QStringLiteral("width"), sceneRect.width());
            mediaEntry.insert(QStringLiteral("height"), sceneRect.height());
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

    recenterView();
}

qreal QuickCanvasController::currentSceneUnitScale() const {
    return 1.0;
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
