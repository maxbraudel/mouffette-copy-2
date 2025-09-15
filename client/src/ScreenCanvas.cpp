#include "ScreenCanvas.h"

#include <QGraphicsScene>
#include <QMimeData>
#include <QDragEnterEvent>
#include <QDragMoveEvent>
#include <QDragLeaveEvent>
#include <QDropEvent>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QWheelEvent>
#include <QScrollBar>
#include <QGestureEvent>
#include <QPinchGesture>
#include <QNativeGestureEvent>
#include <QFileInfo>
#include <QVariantAnimation>
#include <QVideoSink>
#include <QMediaPlayer>
#include <QAudioOutput>
#include <QGuiApplication>
#include <QScreen>
#include <QStyleHints>
#include <QCursor>
#include <QGraphicsPixmapItem>
#include <QtMath>
#include <QRandomGenerator>
#include <QJsonObject>
#include <QPainter>
#include <QBuffer>

ScreenCanvas::ScreenCanvas(QWidget* parent) : QGraphicsView(parent) {
    setAcceptDrops(true);
    setDragMode(QGraphicsView::NoDrag); // manual panning / selection logic
    m_scene = new QGraphicsScene(this);
    setScene(m_scene);
    // Expand virtual scene rect so user can pan into empty space (design tool feel)
    m_scene->setSceneRect(-50000, -50000, 100000, 100000);
    setRenderHint(QPainter::Antialiasing, true);
    // Figma-like: no scrollbars, we manually pan/zoom via transform.
    setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    // Remove frame and make background transparent so only content shows.
    setFrameStyle(QFrame::NoFrame);
    setBackgroundBrush(Qt::NoBrush);
    setAttribute(Qt::WA_AcceptTouchEvents, true);
    setTransformationAnchor(QGraphicsView::NoAnchor); // we'll anchor manually
    if (viewport()) {
        viewport()->setAutoFillBackground(false);
        viewport()->setAttribute(Qt::WA_TranslucentBackground, true);
    }
    // FullViewportUpdate avoids artifacts when we translate/scale manually.
    setViewportUpdateMode(QGraphicsView::FullViewportUpdate);
    viewport()->setMouseTracking(true);
    grabGesture(Qt::PinchGesture); // for non-macOS platforms
    m_nativePinchGuardTimer = new QTimer(this);
    m_nativePinchGuardTimer->setInterval(180); // short guard after last native pinch
    m_nativePinchGuardTimer->setSingleShot(true);
    connect(m_nativePinchGuardTimer, &QTimer::timeout, this, [this]() { m_nativePinchActive = false; });
}

void ScreenCanvas::setScreens(const QList<ScreenInfo>& screens) {
    m_screens = screens;
    createScreenItems();
}

void ScreenCanvas::clearScreens() {
    for (auto* r : m_screenItems) {
        if (r) m_scene->removeItem(r);
        delete r;
    }
    m_screenItems.clear();
}

void ScreenCanvas::recenterWithMargin(int marginPx) {
    QRectF bounds = screensBoundingRect();
    if (bounds.isNull() || !bounds.isValid()) return;
    const QSize vp = viewport() ? viewport()->size() : size();
    const qreal availW = static_cast<qreal>(vp.width())  - 2.0 * marginPx;
    const qreal availH = static_cast<qreal>(vp.height()) - 2.0 * marginPx;
    if (availW <= 1 || availH <= 1 || bounds.width() <= 0 || bounds.height() <= 0) {
        fitInView(bounds, Qt::KeepAspectRatio);
        centerOn(bounds.center());
        return;
    }
    const qreal sx = availW / bounds.width();
    const qreal sy = availH / bounds.height();
    const qreal s = std::min(sx, sy);
    QTransform t; t.scale(s, s); setTransform(t); centerOn(bounds.center());
    if (m_scene) {
        const QList<QGraphicsItem*> sel = m_scene->selectedItems();
        for (QGraphicsItem* it : sel) {
            if (auto* v = dynamic_cast<ResizableVideoItem*>(it)) v->requestOverlayRelayout();
            if (auto* b = dynamic_cast<ResizableMediaBase*>(it)) b->requestLabelRelayout();
        }
    }
    m_ignorePanMomentum = true; m_momentumPrimed = false; m_lastMomentumMag = 0.0; m_lastMomentumDelta = QPoint(0,0); m_momentumTimer.restart();
}

void ScreenCanvas::updateRemoteCursor(int globalX, int globalY) {
    QPointF scenePos = mapToScene(mapFromGlobal(QPoint(globalX, globalY)));
    if (!m_remoteCursorDot) {
        m_remoteCursorDot = new QGraphicsEllipseItem(QRectF(-6,-6,12,12));
        m_remoteCursorDot->setBrush(QBrush(QColor(255,64,64,200)));
        m_remoteCursorDot->setPen(Qt::NoPen);
        m_remoteCursorDot->setZValue(4000.0);
        if (m_scene) m_scene->addItem(m_remoteCursorDot);
    }
    if (m_remoteCursorDot) m_remoteCursorDot->setPos(scenePos);
}

void ScreenCanvas::hideRemoteCursor() {
    if (m_remoteCursorDot) m_remoteCursorDot->hide();
}

void ScreenCanvas::setMediaHandleSelectionSizePx(int px) { m_mediaHandleSelectionSizePx = qMax(1, px); }
void ScreenCanvas::setMediaHandleVisualSizePx(int px) { m_mediaHandleVisualSizePx = qMax(1, px); }
void ScreenCanvas::setMediaHandleSizePx(int px) { setMediaHandleSelectionSizePx(px); setMediaHandleVisualSizePx(px); }

void ScreenCanvas::setScreenBorderWidthPx(int px) {
    m_screenBorderWidthPx = qMax(0, px);
    if (!m_scene) return;
    for (int i = 0; i < m_screenItems.size() && i < m_screens.size(); ++i) {
        QGraphicsRectItem* item = m_screenItems[i]; if (!item) continue;
        int penW = m_screenBorderWidthPx;
        int oldPenW = static_cast<int>(item->pen().widthF());
        QRectF currentInner = item->rect();
        QRectF outer = currentInner.adjusted(-(oldPenW/2.0), -(oldPenW/2.0), (oldPenW/2.0), (oldPenW/2.0));
        QRectF newInner = outer.adjusted(penW/2.0, penW/2.0, -penW/2.0, -penW/2.0);
        item->setRect(newInner); QPen p = item->pen(); p.setWidthF(penW); item->setPen(p);
    }
}

bool ScreenCanvas::event(QEvent* event) {
    if (event->type() == QEvent::Gesture) {
    return gestureEvent(static_cast<QGestureEvent*>(event));
    }
    if (event->type() == QEvent::NativeGesture) {
        auto* ng = static_cast<QNativeGestureEvent*>(event);
        if (ng->gestureType() == Qt::ZoomNativeGesture) {
            m_nativePinchActive = true; m_nativePinchGuardTimer->start();
            const qreal factor = std::pow(2.0, ng->value());
            QPoint vpPos = viewport()->mapFromGlobal(QCursor::pos());
            if (!viewport()->rect().contains(vpPos)) {
                QPoint viewPos = ng->position().toPoint();
                vpPos = viewport()->mapFrom(this, viewPos);
                if (!viewport()->rect().contains(vpPos)) vpPos = m_lastMousePos.isNull() ? viewport()->rect().center() : m_lastMousePos;
            }
            m_lastMousePos = vpPos; // remember anchor
            zoomAroundViewportPos(vpPos, factor); event->accept(); return true;
        }
    }
    return QGraphicsView::event(event);
}

bool ScreenCanvas::viewportEvent(QEvent* event) {
#ifdef Q_OS_MACOS
    // Some trackpad gestures may arrive directly to the viewport; handle zoom similarly
    if (event->type() == QEvent::NativeGesture) {
        auto* ng = static_cast<QNativeGestureEvent*>(event);
        if (ng->gestureType() == Qt::ZoomNativeGesture) {
            m_nativePinchActive = true; m_nativePinchGuardTimer->start();
            const qreal factor = std::pow(2.0, ng->value());
            QPoint vpPos = viewport()->mapFrom(this, ng->position().toPoint());
            if (!viewport()->rect().contains(vpPos)) vpPos = viewport()->rect().center();
            m_lastMousePos = vpPos; // remember anchor
            zoomAroundViewportPos(vpPos, factor);
            event->accept();
            return true;
        }
    }
#endif
    return QGraphicsView::viewportEvent(event);
}

bool ScreenCanvas::gestureEvent(QGestureEvent* event) {
    if (QGesture* g = event->gesture(Qt::PinchGesture)) {
        auto* pinch = static_cast<QPinchGesture*>(g);
        if (pinch->changeFlags() & QPinchGesture::ScaleFactorChanged) {
            // centerPoint() is already in the receiving widget's coordinate system (our view)
            QPoint vpPos = pinch->centerPoint().toPoint();
            if (!viewport()->rect().contains(vpPos)) {
                // Fallbacks: cursor inside viewport, then last mouse pos, then center
                QPoint cursorVp = viewport()->mapFromGlobal(QCursor::pos());
                if (viewport()->rect().contains(cursorVp)) vpPos = cursorVp;
                else vpPos = m_lastMousePos.isNull() ? viewport()->rect().center() : m_lastMousePos;
            }
            m_lastMousePos = vpPos; // keep coherent for subsequent wheel/native zooms
            const qreal factor = pinch->scaleFactor();
            zoomAroundViewportPos(vpPos, factor);
        }
        event->accept();
        return true;
    }
    return QGraphicsView::event(event);
}

void ScreenCanvas::keyPressEvent(QKeyEvent* event) {
    if (event->key() == Qt::Key_Delete || event->key() == Qt::Key_Backspace) {
        if (m_scene) {
            const QList<QGraphicsItem*> sel = m_scene->selectedItems();
            for (QGraphicsItem* it : sel) if (auto* base = dynamic_cast<ResizableMediaBase*>(it)) { base->ungrabMouse(); m_scene->removeItem(base); delete base; }
        }
        event->accept(); return;
    }
    if (event->key() == Qt::Key_Space) { recenterWithMargin(53); event->accept(); return; }
    QGraphicsView::keyPressEvent(event);
}

void ScreenCanvas::mousePressEvent(QMouseEvent* event) {
    // (Space-to-pan currently disabled pending dedicated key tracking)
    const bool spaceHeld = false;
    if (event->button() == Qt::LeftButton) {
        if (m_scene) {
            const QPointF scenePosEarly = mapToScene(event->pos());
            const QList<QGraphicsItem*> selEarly = m_scene->selectedItems();
            for (QGraphicsItem* it : selEarly) if (auto* v = dynamic_cast<ResizableVideoItem*>(it)) { if (v->handleControlsPressAtItemPos(v->mapFromScene(scenePosEarly))) { m_overlayMouseDown = true; event->accept(); return; } }
        }
        // Space+drag always pans
        if (spaceHeld) { m_panning = true; m_lastPanPoint = event->pos(); event->accept(); return; }
        const QPointF scenePos = mapToScene(event->pos());
        ResizableMediaBase* topHandleItem = nullptr; qreal topZ = -std::numeric_limits<qreal>::infinity();
        const QList<QGraphicsItem*> sel = m_scene ? m_scene->selectedItems() : QList<QGraphicsItem*>();
        for (QGraphicsItem* it : sel) if (auto* rp = dynamic_cast<ResizableMediaBase*>(it)) if (rp->isSelected() && rp->isOnHandleAtItemPos(rp->mapFromScene(scenePos))) { if (rp->zValue() > topZ) { topZ = rp->zValue(); topHandleItem = rp; } }
        if (topHandleItem) { if (topHandleItem->beginResizeAtScenePos(scenePos)) { viewport()->setCursor(topHandleItem->cursorForScenePos(scenePos)); event->accept(); return; } }
        const QList<QGraphicsItem*> hitItems = items(event->pos()); bool clickedOverlayOnly = false; bool anyMedia = false;
        for (QGraphicsItem* hi : hitItems) {
            if (hi->data(0).toString() == QLatin1String("overlay")) clickedOverlayOnly = true;
            QGraphicsItem* scan = hi; while (scan) { if (dynamic_cast<ResizableMediaBase*>(scan)) { anyMedia = true; break; } scan = scan->parentItem(); }
        }
        if (clickedOverlayOnly && !anyMedia) { event->accept(); return; }
        auto toMedia = [](QGraphicsItem* x)->ResizableMediaBase* { while (x) { if (auto* m = dynamic_cast<ResizableMediaBase*>(x)) return m; x = x->parentItem(); } return nullptr; };
        ResizableMediaBase* mediaHit = nullptr; for (QGraphicsItem* it : hitItems) { if ((mediaHit = toMedia(it))) break; }
        if (mediaHit) {
            if (m_scene) m_scene->clearSelection(); if (!mediaHit->isSelected()) mediaHit->setSelected(true);
            if (auto* v = dynamic_cast<ResizableVideoItem*>(mediaHit)) { const QPointF itemPos = v->mapFromScene(mapToScene(event->pos())); if (v->handleControlsPressAtItemPos(itemPos)) { event->accept(); return; } }
            QMouseEvent synthetic(event->type(), event->position(), event->scenePosition(), event->globalPosition(), event->button(), event->buttons(), Qt::NoModifier);
            QGraphicsView::mousePressEvent(&synthetic);
            if (m_scene) m_scene->clearSelection(); mediaHit->setSelected(true); return;
        }
        for (QGraphicsItem* it : scene()->selectedItems()) if (auto* v = dynamic_cast<ResizableVideoItem*>(it)) { const QPointF itemPos = v->mapFromScene(mapToScene(event->pos())); if (v->handleControlsPressAtItemPos(itemPos)) { event->accept(); return; } }
        if (m_scene) m_scene->clearSelection();
        // Start panning when clicking empty space
        m_panning = true; m_lastPanPoint = event->pos(); event->accept(); return;
    }
    QGraphicsView::mousePressEvent(event);
}

void ScreenCanvas::mouseDoubleClickEvent(QMouseEvent* event) {
    if (event->button() == Qt::LeftButton) {
        if (m_scene) {
            const QPointF scenePosSel = mapToScene(event->pos());
            const QList<QGraphicsItem*> sel = m_scene->selectedItems();
            for (QGraphicsItem* it : sel) if (auto* v = dynamic_cast<ResizableVideoItem*>(it)) if (v->handleControlsPressAtItemPos(v->mapFromScene(scenePosSel))) { m_overlayMouseDown = true; event->accept(); return; }
        }
        const QList<QGraphicsItem*> hitItems = items(event->pos());
        auto toMedia = [](QGraphicsItem* x)->ResizableMediaBase* { while (x) { if (auto* m = dynamic_cast<ResizableMediaBase*>(x)) return m; x = x->parentItem(); } return nullptr; };
        ResizableMediaBase* mediaHit = nullptr; for (QGraphicsItem* it : hitItems) { if ((mediaHit = toMedia(it))) break; }
        if (mediaHit) { if (scene()) scene()->clearSelection(); if (!mediaHit->isSelected()) mediaHit->setSelected(true); if (auto* v = dynamic_cast<ResizableVideoItem*>(mediaHit)) { const QPointF itemPos = v->mapFromScene(mapToScene(event->pos())); if (v->handleControlsPressAtItemPos(itemPos)) { event->accept(); return; } } QGraphicsView::mouseDoubleClickEvent(event); if (scene()) scene()->clearSelection(); mediaHit->setSelected(true); return; }
    }
    QGraphicsView::mouseDoubleClickEvent(event);
}

void ScreenCanvas::mouseMoveEvent(QMouseEvent* event) {
    if (m_overlayMouseDown) {
        if (m_scene) { const QList<QGraphicsItem*> sel = m_scene->selectedItems(); for (QGraphicsItem* it : sel) if (auto* v = dynamic_cast<ResizableVideoItem*>(it)) { if (v->isDraggingProgress() || v->isDraggingVolume()) { v->updateDragWithScenePos(mapToScene(event->pos())); event->accept(); return; } } }
        event->accept(); return;
    }
    m_lastMousePos = event->pos();
    const QPointF scenePos = mapToScene(event->pos());
    Qt::CursorShape resizeCursor = Qt::ArrowCursor; bool onResizeHandle = false; qreal topZ = -std::numeric_limits<qreal>::infinity();
    const QList<QGraphicsItem*> sel = m_scene ? m_scene->selectedItems() : QList<QGraphicsItem*>();
    for (QGraphicsItem* it : sel) if (auto* rp = dynamic_cast<ResizableMediaBase*>(it)) if (rp->isSelected() && rp->zValue() >= topZ) { Qt::CursorShape itemCursor = rp->cursorForScenePos(scenePos); if (itemCursor != Qt::ArrowCursor) { resizeCursor = itemCursor; onResizeHandle = true; topZ = rp->zValue(); } }
    if (onResizeHandle) viewport()->setCursor(resizeCursor); else viewport()->unsetCursor();
    if (event->buttons() & Qt::LeftButton) {
        for (QGraphicsItem* it : sel) if (auto* v = dynamic_cast<ResizableVideoItem*>(it)) if (v->isSelected() && (v->isDraggingProgress() || v->isDraggingVolume())) { v->updateDragWithScenePos(mapToScene(event->pos())); event->accept(); return; }
        const QList<QGraphicsItem*> hitItems = items(event->pos()); auto toMedia = [](QGraphicsItem* x)->ResizableMediaBase* { while (x) { if (auto* m = dynamic_cast<ResizableMediaBase*>(x)) return m; x = x->parentItem(); } return nullptr; }; bool hitMedia = false; for (QGraphicsItem* it : hitItems) if (toMedia(it)) { hitMedia = true; break; } if (hitMedia) { QGraphicsView::mouseMoveEvent(event); return; }
    }
    if (m_panning) {
        QPoint delta = event->pos() - m_lastPanPoint;
        if (!delta.isNull()) {
            QTransform t = transform();
            // Translate by raw delta scaled to current transform (so pixel drag equals screen pan)
            t.translate(delta.x() * -1.0 / t.m11(), delta.y() * -1.0 / t.m22());
            setTransform(t);
            m_lastPanPoint = event->pos();
        }
        event->accept();
        return;
    }
    QGraphicsView::mouseMoveEvent(event);
}

void ScreenCanvas::mouseReleaseEvent(QMouseEvent* event) {
    if (event->button() == Qt::LeftButton) {
        if (m_overlayMouseDown) {
            if (m_scene) { const QList<QGraphicsItem*> sel = m_scene->selectedItems(); for (QGraphicsItem* it : sel) if (auto* v = dynamic_cast<ResizableVideoItem*>(it)) if (v->isDraggingProgress() || v->isDraggingVolume()) v->endDrag(); }
            m_overlayMouseDown = false; event->accept(); return;
        }
        for (QGraphicsItem* it : m_scene->items()) if (auto* v = dynamic_cast<ResizableVideoItem*>(it)) if (v->isSelected() && (v->isDraggingProgress() || v->isDraggingVolume())) { v->endDrag(); event->accept(); return; }
    if (m_panning) { m_panning = false; event->accept(); return; }
        bool wasResizing = false; for (QGraphicsItem* it : m_scene->items()) if (auto* rp = dynamic_cast<ResizableMediaBase*>(it)) if (rp->isActivelyResizing()) { wasResizing = true; break; }
        if (wasResizing) viewport()->unsetCursor();
        QMouseEvent synthetic(event->type(), event->position(), event->scenePosition(), event->globalPosition(), event->button(), event->buttons(), Qt::NoModifier);
        QGraphicsView::mouseReleaseEvent(&synthetic);
        if (m_scene) { const QList<QGraphicsItem*> sel = m_scene->selectedItems(); if (!sel.isEmpty()) { const QList<QGraphicsItem*> hitItems = items(event->pos()); auto toMedia = [](QGraphicsItem* x)->ResizableMediaBase* { while (x) { if (auto* m = dynamic_cast<ResizableMediaBase*>(x)) return m; x = x->parentItem(); } return nullptr; }; ResizableMediaBase* keep = nullptr; for (QGraphicsItem* it : hitItems) if ((keep = toMedia(it))) break; if (!keep) for (QGraphicsItem* it : sel) if ((keep = dynamic_cast<ResizableMediaBase*>(it))) break; m_scene->clearSelection(); if (keep) keep->setSelected(true); } }
        return;
    }
    QGraphicsView::mouseReleaseEvent(event);
}

void ScreenCanvas::wheelEvent(QWheelEvent* event) {
#ifdef Q_OS_MACOS
    if (m_nativePinchActive) { event->ignore(); return; }
#endif
#ifdef Q_OS_MACOS
    const bool zoomModifier = event->modifiers().testFlag(Qt::MetaModifier);
#else
    const bool zoomModifier = event->modifiers().testFlag(Qt::ControlModifier);
#endif
    if (zoomModifier) {
        qreal deltaY = 0.0;
        if (!event->pixelDelta().isNull()) deltaY = event->pixelDelta().y();
        else if (!event->angleDelta().isNull()) deltaY = event->angleDelta().y() / 8.0;
        if (deltaY != 0.0) { const qreal factor = std::pow(1.0015, deltaY); zoomAroundViewportPos(event->position(), factor); event->accept(); return; }
    }
    QPoint delta; if (!event->pixelDelta().isNull()) delta = event->pixelDelta(); else if (!event->angleDelta().isNull()) delta = event->angleDelta() / 8;
    if (!delta.isNull()) {
        if (m_ignorePanMomentum) {
#if QT_VERSION >= QT_VERSION_CHECK(6, 3, 0)
            if (event->phase() == Qt::ScrollBegin) { m_ignorePanMomentum = false; }
            if (!m_ignorePanMomentum) {}
#endif
            if (m_ignorePanMomentum) {
                const double noiseGate = 1.0; const double boostRatio = 1.25; const qint64 graceMs = 180; const qint64 elapsed = m_momentumTimer.isValid() ? m_momentumTimer.elapsed() : LLONG_MAX;
                const double mag = std::abs(delta.x()) + std::abs(delta.y());
                if (elapsed < graceMs) { event->accept(); return; }
                if (mag <= noiseGate) { event->accept(); return; }
                if (!m_momentumPrimed) { m_momentumPrimed = true; m_lastMomentumMag = mag; m_lastMomentumDelta = delta; event->accept(); return; }
                const bool sameDir = (m_lastMomentumDelta.x()==0 || delta.x()==0 || (std::signbit(m_lastMomentumDelta.x())==std::signbit(delta.x()))) && (m_lastMomentumDelta.y()==0 || delta.y()==0 || (std::signbit(m_lastMomentumDelta.y())==std::signbit(delta.y())));
                if (sameDir) {
                    if (mag <= m_lastMomentumMag * boostRatio) { m_lastMomentumMag = mag; m_lastMomentumDelta = delta; event->accept(); return; }
                    m_ignorePanMomentum = false;
                } else { m_ignorePanMomentum = false; }
            }
        }
        horizontalScrollBar()->setValue(horizontalScrollBar()->value() - delta.x());
        verticalScrollBar()->setValue(verticalScrollBar()->value() - delta.y());
        event->accept(); return;
    }
    QGraphicsView::wheelEvent(event);
}

void ScreenCanvas::dragEnterEvent(QDragEnterEvent* event) {
    if (!event->mimeData()) { event->ignore(); return; }
    const QMimeData* mime = event->mimeData();
    if (mime->hasUrls()) { event->acceptProposedAction(); ensureDragPreview(mime); }
    else if (mime->hasImage()) { event->acceptProposedAction(); ensureDragPreview(mime); }
    else event->ignore();
}

void ScreenCanvas::dragMoveEvent(QDragMoveEvent* event) {
    const QMimeData* mime = event->mimeData(); if (!mime) { event->ignore(); return; }
    if (!m_dragPreviewItem) ensureDragPreview(mime);
    QPointF scenePos = mapToScene(event->position().toPoint()); m_dragPreviewLastScenePos = scenePos; updateDragPreviewPos(scenePos);
    if (!m_dragCursorHidden) { viewport()->setCursor(Qt::BlankCursor); m_dragCursorHidden = true; }
    event->acceptProposedAction();
}

void ScreenCanvas::dragLeaveEvent(QDragLeaveEvent* event) {
    clearDragPreview(); if (m_dragCursorHidden) { viewport()->unsetCursor(); m_dragCursorHidden = false; } event->accept();
}

void ScreenCanvas::dropEvent(QDropEvent* event) {
    const QMimeData* mime = event->mimeData(); if (!mime) { event->ignore(); return; }
    QPointF scenePos = mapToScene(event->position().toPoint());
    if (mime->hasUrls()) {
        const QList<QUrl> urls = mime->urls();
        for (const QUrl& url : urls) {
            if (url.isLocalFile()) {
                QString localPath = url.toLocalFile();
                if (localPath.isEmpty()) continue;
                QFileInfo fi(localPath);
                const QString suffix = fi.suffix().toLower();
                bool isVideo = suffix == "mp4" || suffix == "mov" || suffix == "m4v" || suffix == "avi" || suffix == "mkv" || suffix == "webm";
                if (isVideo) {
                    // Use default handle sizes similar to previous inline defaults (visual 12, selection 30)
                    auto* v = new ResizableVideoItem(localPath, 12, 30, fi.fileName(), m_videoControlsFadeMs);
                    // Preserve global canvas media scale (so video size matches screens & images 1:1)
                    v->setInitialScaleFactor(m_scaleFactor);
                    // Use current placeholder base size (constructor default 640x360) to center on drop point
                    const qreal phW = 640.0 * m_scaleFactor;
                    const qreal phH = 360.0 * m_scaleFactor;
                    v->setPos(scenePos - QPointF(phW/2.0, phH/2.0));
                    v->setScale(m_scaleFactor); // adoptBaseSize will keep center when real size arrives
                    // If a drag preview frame was captured for this video, use it immediately as a poster to avoid flicker gap
                    if (m_dragPreviewIsVideo && m_dragPreviewGotFrame && !m_dragPreviewPixmap.isNull()) {
                        QImage poster = m_dragPreviewPixmap.toImage();
                        if (!poster.isNull()) v->setExternalPosterImage(poster);
                    }
                    m_scene->addItem(v);
                    v->setSelected(true);
                } else {
                    QPixmap pm(localPath);
                    if (!pm.isNull()) {
                        auto* p = new ResizablePixmapItem(pm, 12, 30, fi.fileName());
                        p->setPos(scenePos - QPointF(pm.width()/2.0 * m_scaleFactor, pm.height()/2.0 * m_scaleFactor));
                        p->setScale(m_scaleFactor);
                        m_scene->addItem(p);
                        p->setSelected(true);
                    }
                }
            }
        }
    } else if (mime->hasImage()) {
        QImage img = qvariant_cast<QImage>(mime->imageData());
        if (!img.isNull()) {
            QPixmap pm = QPixmap::fromImage(img);
            if (!pm.isNull()) {
                auto* p = new ResizablePixmapItem(pm, 12, 30, QString());
                p->setPos(scenePos - QPointF(pm.width()/2.0 * m_scaleFactor, pm.height()/2.0 * m_scaleFactor));
                p->setScale(m_scaleFactor);
                m_scene->addItem(p);
                p->setSelected(true);
            }
        }
    }
    clearDragPreview(); if (m_dragCursorHidden) { viewport()->unsetCursor(); m_dragCursorHidden = false; }
    event->acceptProposedAction();
}

void ScreenCanvas::ensureDragPreview(const QMimeData* mime) {
    if (!mime) return; if (m_dragPreviewItem) return; m_dragPreviewGotFrame = false; m_dragPreviewIsVideo = false;
    if (mime->hasUrls()) {
        QList<QUrl> urls = mime->urls(); if (!urls.isEmpty() && urls.first().isLocalFile()) {
            QFileInfo fi(urls.first().toLocalFile()); QString suffix = fi.suffix().toLower();
            bool isVideo = suffix == "mp4" || suffix == "mov" || suffix == "m4v" || suffix == "avi" || suffix == "mkv" || suffix == "webm";
            if (isVideo) { m_dragPreviewIsVideo = true; startVideoPreviewProbe(fi.absoluteFilePath()); return; }
            QPixmap pm(fi.absoluteFilePath()); if (!pm.isNull()) { m_dragPreviewPixmap = pm; m_dragPreviewBaseSize = pm.size(); }
        }
    } else if (mime->hasImage()) {
        QImage img = qvariant_cast<QImage>(mime->imageData()); if (!img.isNull()) { m_dragPreviewPixmap = QPixmap::fromImage(img); m_dragPreviewBaseSize = m_dragPreviewPixmap.size(); }
    }
    if (!m_dragPreviewPixmap.isNull()) {
        auto* pmItem = new QGraphicsPixmapItem(m_dragPreviewPixmap); pmItem->setOpacity(0.0); pmItem->setZValue(5000.0); pmItem->setScale(m_scaleFactor); m_scene->addItem(pmItem); m_dragPreviewItem = pmItem; startDragPreviewFadeIn();
    }
}

void ScreenCanvas::updateDragPreviewPos(const QPointF& scenePos) {
    if (!m_dragPreviewItem) return; QSize baseSize = m_dragPreviewBaseSize; if (baseSize.isEmpty()) baseSize = QSize(400, 240);
    QPointF topLeft = scenePos - QPointF(baseSize.width()/2.0 * m_scaleFactor, baseSize.height()/2.0 * m_scaleFactor);
    m_dragPreviewItem->setPos(topLeft);
}

void ScreenCanvas::clearDragPreview() {
    stopVideoPreviewProbe(); stopDragPreviewFade(); if (m_dragPreviewItem) { m_scene->removeItem(m_dragPreviewItem); delete m_dragPreviewItem; m_dragPreviewItem = nullptr; }
    m_dragPreviewPixmap = QPixmap(); m_dragPreviewGotFrame = false; m_dragPreviewIsVideo = false;
}

QPixmap ScreenCanvas::makeVideoPlaceholderPixmap(const QSize& pxSize) {
    QPixmap pm(pxSize); pm.fill(Qt::transparent); QPainter p(&pm); p.setRenderHint(QPainter::Antialiasing, true); QRect r(0,0,pxSize.width()-1, pxSize.height()-1); p.setBrush(QColor(40,40,40,220)); p.setPen(Qt::NoPen); p.drawRoundedRect(r, 16,16); QPolygon play; play << QPoint(pxSize.width()/2 - 18, pxSize.height()/2 - 24) << QPoint(pxSize.width()/2 - 18, pxSize.height()/2 + 24) << QPoint(pxSize.width()/2 + 26, pxSize.height()/2); p.setBrush(QColor(255,255,255,200)); p.drawPolygon(play); return pm;
}

void ScreenCanvas::startVideoPreviewProbe(const QString& localFilePath) {
#ifdef Q_OS_MACOS
    // Fast macOS thumbnail path (if available) could be wired here later; currently fallback.
#endif
    startVideoPreviewProbeFallback(localFilePath);
}

void ScreenCanvas::startVideoPreviewProbeFallback(const QString& localFilePath) {
    if (m_dragPreviewPlayer) return; m_dragPreviewPlayer = new QMediaPlayer(this); m_dragPreviewAudio = new QAudioOutput(this); m_dragPreviewAudio->setMuted(true); m_dragPreviewPlayer->setAudioOutput(m_dragPreviewAudio); m_dragPreviewSink = new QVideoSink(this); m_dragPreviewPlayer->setVideoSink(m_dragPreviewSink); m_dragPreviewPlayer->setSource(QUrl::fromLocalFile(localFilePath));
    connect(m_dragPreviewSink, &QVideoSink::videoFrameChanged, this, [this](const QVideoFrame& f){ if (m_dragPreviewGotFrame || !f.isValid()) return; QImage img = f.toImage(); if (img.isNull()) return; m_dragPreviewGotFrame = true; QPixmap newPm = QPixmap::fromImage(img); if (newPm.isNull()) return; m_dragPreviewPixmap = newPm; m_dragPreviewBaseSize = newPm.size(); if (!m_dragPreviewItem) { auto* pmItem = new QGraphicsPixmapItem(m_dragPreviewPixmap); pmItem->setOpacity(0.0); pmItem->setZValue(5000.0); pmItem->setScale(m_scaleFactor); m_scene->addItem(pmItem); m_dragPreviewItem = pmItem; updateDragPreviewPos(m_dragPreviewLastScenePos); startDragPreviewFadeIn(); } else if (auto* pmItem = qgraphicsitem_cast<QGraphicsPixmapItem*>(m_dragPreviewItem)) { pmItem->setPixmap(m_dragPreviewPixmap); updateDragPreviewPos(m_dragPreviewLastScenePos); } if (m_dragPreviewPlayer) m_dragPreviewPlayer->pause(); if (m_dragPreviewFallbackTimer) { m_dragPreviewFallbackTimer->stop(); m_dragPreviewFallbackTimer->deleteLater(); m_dragPreviewFallbackTimer = nullptr; } });
    m_dragPreviewPlayer->play();
}

void ScreenCanvas::stopVideoPreviewProbe() {
    if (m_dragPreviewFallbackTimer) { m_dragPreviewFallbackTimer->stop(); m_dragPreviewFallbackTimer->deleteLater(); m_dragPreviewFallbackTimer = nullptr; }
    if (m_dragPreviewPlayer) { m_dragPreviewPlayer->stop(); m_dragPreviewPlayer->deleteLater(); m_dragPreviewPlayer = nullptr; }
    if (m_dragPreviewSink) { m_dragPreviewSink->deleteLater(); m_dragPreviewSink = nullptr; }
    if (m_dragPreviewAudio) { m_dragPreviewAudio->deleteLater(); m_dragPreviewAudio = nullptr; }
}

void ScreenCanvas::startDragPreviewFadeIn() {
    stopDragPreviewFade(); if (!m_dragPreviewItem) return; const qreal target = m_dragPreviewTargetOpacity; if (m_dragPreviewItem->opacity() >= target - 0.001) return; auto* anim = new QVariantAnimation(this); m_dragPreviewFadeAnim = anim; anim->setStartValue(0.0); anim->setEndValue(target); anim->setDuration(m_dragPreviewFadeMs); anim->setEasingCurve(QEasingCurve::OutCubic); connect(anim, &QVariantAnimation::valueChanged, this, [this](const QVariant& v){ if (m_dragPreviewItem) m_dragPreviewItem->setOpacity(v.toReal()); }); connect(anim, &QVariantAnimation::finished, this, [this](){ m_dragPreviewFadeAnim = nullptr; }); anim->start(QAbstractAnimation::DeleteWhenStopped);
}

void ScreenCanvas::stopDragPreviewFade() { if (m_dragPreviewFadeAnim) { m_dragPreviewFadeAnim->stop(); m_dragPreviewFadeAnim = nullptr; } }

void ScreenCanvas::onFastVideoThumbnailReady(const QImage& img) {
    if (img.isNull()) return; if (m_dragPreviewGotFrame) return; m_dragPreviewGotFrame = true; QPixmap pm = QPixmap::fromImage(img); if (pm.isNull()) return; m_dragPreviewPixmap = pm; m_dragPreviewBaseSize = pm.size(); if (!m_dragPreviewItem) { auto* pmItem = new QGraphicsPixmapItem(m_dragPreviewPixmap); pmItem->setOpacity(0.0); pmItem->setZValue(5000.0); pmItem->setScale(m_scaleFactor); if (m_scene) m_scene->addItem(pmItem); m_dragPreviewItem = pmItem; updateDragPreviewPos(m_dragPreviewLastScenePos); startDragPreviewFadeIn(); } else if (auto* pix = qgraphicsitem_cast<QGraphicsPixmapItem*>(m_dragPreviewItem)) { pix->setPixmap(m_dragPreviewPixmap); updateDragPreviewPos(m_dragPreviewLastScenePos); }
    if (m_dragPreviewFallbackTimer) { m_dragPreviewFallbackTimer->stop(); m_dragPreviewFallbackTimer->deleteLater(); m_dragPreviewFallbackTimer = nullptr; }
    if (m_dragPreviewPlayer) { m_dragPreviewPlayer->stop(); m_dragPreviewPlayer->deleteLater(); m_dragPreviewPlayer = nullptr; }
    if (m_dragPreviewSink) { m_dragPreviewSink->deleteLater(); m_dragPreviewSink = nullptr; }
    if (m_dragPreviewAudio) { m_dragPreviewAudio->deleteLater(); m_dragPreviewAudio = nullptr; }
}

void ScreenCanvas::createScreenItems() {
    clearScreens();
    if (!m_scene) return;
    const double spacing = static_cast<double>(m_screenSpacingPx);
    QMap<int, QRectF> compactPositions = calculateCompactPositions(1.0, spacing, spacing);
    for (int i = 0; i < m_screens.size(); ++i) {
        const ScreenInfo& s = m_screens[i]; QRectF pos = compactPositions.value(i); auto* rect = createScreenItem(s, i, pos); rect->setZValue(-1000.0); m_scene->addItem(rect); m_screenItems << rect; }
    ensureZOrder();
}

QGraphicsRectItem* ScreenCanvas::createScreenItem(const ScreenInfo& screen, int index, const QRectF& position) {
    const int penWidth = m_screenBorderWidthPx;
    QRectF inner = position.adjusted(penWidth/2.0, penWidth/2.0, -penWidth/2.0, -penWidth/2.0);
    QGraphicsRectItem* item = new QGraphicsRectItem(inner);
    if (screen.primary) { item->setBrush(QBrush(QColor(74,144,226,180))); item->setPen(QPen(QColor(74,144,226), penWidth)); }
    else { item->setBrush(QBrush(QColor(80,80,80,180))); item->setPen(QPen(QColor(160,160,160), penWidth)); }
    item->setData(0, index);
    QGraphicsTextItem* label = new QGraphicsTextItem(QString("Screen %1\n%2×%3").arg(index+1).arg(screen.width).arg(screen.height));
    label->setDefaultTextColor(Qt::white);
    QFont f("Arial", m_screenLabelFontPt, QFont::Bold);
    label->setFont(f);
    QRectF labelRect = label->boundingRect(); QRectF screenRect = item->rect(); label->setPos(screenRect.center() - labelRect.center()); label->setParentItem(item);
    return item;
}

QMap<int, QRectF> ScreenCanvas::calculateCompactPositions(double scaleFactor, double hSpacing, double vSpacing) const {
    QMap<int, QRectF> positions; if (m_screens.isEmpty()) return positions; QList<QPair<int, ScreenInfo>> screenPairs; for (int i=0;i<m_screens.size();++i) screenPairs.append(qMakePair(i, m_screens[i]));
    std::sort(screenPairs.begin(), screenPairs.end(), [](const QPair<int, ScreenInfo>& a, const QPair<int, ScreenInfo>& b){ if (qAbs(a.second.y - b.second.y) < 100) return a.second.x < b.second.x; return a.second.y < b.second.y; });
    double currentX = 0, currentY = 0, rowHeight = 0; int lastY = INT_MIN;
    for (const auto& pair : screenPairs) {
        int index = pair.first; const ScreenInfo& screen = pair.second;
        double screenWidth = screen.width * scaleFactor;
        double screenHeight = screen.height * scaleFactor;
        if (lastY != INT_MIN && qAbs(screen.y - lastY) > 100) {
            currentX = 0; currentY += rowHeight + vSpacing; rowHeight = 0;
        }
        QRectF rect(currentX, currentY, screenWidth, screenHeight);
        positions[index] = rect;
        currentX += screenWidth + hSpacing;
        rowHeight = qMax(rowHeight, screenHeight);
        lastY = screen.y;
    }
    return positions; }

QRectF ScreenCanvas::screensBoundingRect() const { QRectF bounds; bool first = true; for (auto* item : m_screenItems) { if (!item) continue; QRectF r = item->sceneBoundingRect(); if (first) { bounds = r; first = false; } else { bounds = bounds.united(r); } } return bounds; }

void ScreenCanvas::zoomAroundViewportPos(const QPointF& vpPosF, qreal factor) {
    QPoint vpPos = vpPosF.toPoint(); if (!viewport()->rect().contains(vpPos)) vpPos = viewport()->rect().center(); const QPointF sceneAnchor = mapToScene(vpPos);
    QTransform t = transform(); t.translate(sceneAnchor.x(), sceneAnchor.y()); t.scale(factor, factor); t.translate(-sceneAnchor.x(), -sceneAnchor.y()); setTransform(t);
    if (m_scene) { const QList<QGraphicsItem*> sel = m_scene->selectedItems(); for (QGraphicsItem* it : sel) { if (auto* v = dynamic_cast<ResizableVideoItem*>(it)) v->requestOverlayRelayout(); if (auto* b = dynamic_cast<ResizableMediaBase*>(it)) b->requestLabelRelayout(); } }
}

void ScreenCanvas::ensureZOrder() {
    // Ensures overlays or future interactive layers can sit above screens; currently screens use -1000.
}

void ScreenCanvas::debugLogScreenSizes() const {
    if (m_screenItems.size() != m_screens.size()) {
        qDebug() << "Screen/item count mismatch" << m_screenItems.size() << m_screens.size();
    }
    for (int i = 0; i < m_screenItems.size() && i < m_screens.size(); ++i) {
        auto* item = m_screenItems[i]; if (!item) continue;
        const ScreenInfo& si = m_screens[i];
        QRectF r = item->rect(); // local (inner) rect already adjusted for border
        qDebug() << "Screen" << i << "expected" << si.width << "x" << si.height
                 << "scaleFactor" << m_scaleFactor
                 << "itemRect" << r.width() << "x" << r.height()
                 << "sceneBounding" << item->sceneBoundingRect().size();
    }
}
