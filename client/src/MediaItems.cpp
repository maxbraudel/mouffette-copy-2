// MediaItems.cpp - Implementation of media item hierarchy extracted from MainWindow.cpp

#include "MediaItems.h"
#include "ScreenCanvas.h"
#include <numeric>
#include <QPainter>
#include <QGraphicsView>
#include <QGraphicsSceneMouseEvent>
#include <QGraphicsSceneHoverEvent>
#include <QGuiApplication>
#include <QStyleOptionGraphicsItem>
#include <QWidget>
#include <QTimer>
#include <QVideoFrame>
#include <QVideoFrameFormat>
#include <algorithm>
#include <QDebug>
#include "MediaSettingsPanel.h"
#include <cmath>
#include <QVariantAnimation>
#include <QtSvgWidgets/QGraphicsSvgItem>
#include <QtSvg/QSvgRenderer>
#include <QFileInfo>
#include <QMediaPlayer>
#include <QAudioOutput>
#include <QVideoSink>
#include <QMediaMetaData>
#include <QApplication>
#include <QUrl>
#include "AppColors.h"
#include <QDateTime>

// ---------------- ResizableMediaBase -----------------

int ResizableMediaBase::heightOfMediaOverlays = -1; // default auto
int ResizableMediaBase::cornerRadiusOfMediaOverlays = 6;

double ResizableMediaBase::s_sceneGridUnit = 1.0; // default: 1 scene unit == 1 pixel
std::function<QPointF(const QPointF&, const QRectF&, bool, ResizableMediaBase*)> ResizableMediaBase::s_screenSnapCallback;
std::function<ResizableMediaBase::ResizeSnapFeedback(qreal, const QPointF&, const QPointF&, const QSize&, bool, ResizableMediaBase*)> ResizableMediaBase::s_resizeSnapCallback;
std::function<void()> ResizableMediaBase::s_uploadChangedNotifier = nullptr;
std::function<void(ResizableMediaBase*)> ResizableMediaBase::s_fileErrorNotifier = nullptr;

QString ResizableMediaBase::displayName() const {
    if (!m_filename.isEmpty()) return m_filename;
    if (!m_sourcePath.isEmpty()) {
        QFileInfo fi(m_sourcePath);
        const QString base = fi.fileName();
        if (!base.isEmpty()) return base;
    }
    return QStringLiteral("Media");
}

void ResizableMediaBase::setSceneGridUnit(double u) { s_sceneGridUnit = (u > 1e-9 ? u : 1.0); }
double ResizableMediaBase::sceneGridUnit() { return s_sceneGridUnit; }

void ResizableMediaBase::setScreenSnapCallback(std::function<QPointF(const QPointF&, const QRectF&, bool, ResizableMediaBase*)> callback) {
    s_screenSnapCallback = callback;
}

std::function<QPointF(const QPointF&, const QRectF&, bool, ResizableMediaBase*)> ResizableMediaBase::screenSnapCallback() {
    return s_screenSnapCallback;
}

void ResizableMediaBase::setResizeSnapCallback(std::function<ResizeSnapFeedback(qreal, const QPointF&, const QPointF&, const QSize&, bool, ResizableMediaBase*)> callback) {
    s_resizeSnapCallback = callback;
}

std::function<ResizableMediaBase::ResizeSnapFeedback(qreal, const QPointF&, const QPointF&, const QSize&, bool, ResizableMediaBase*)> ResizableMediaBase::resizeSnapCallback() {
    return s_resizeSnapCallback;
}

ResizableMediaBase::~ResizableMediaBase() {
    if (m_lifetimeToken) {
        *m_lifetimeToken = false;
        m_lifetimeToken.reset();
    }
    // Clean up FileManager associations
    if (!m_mediaId.isEmpty()) {
        qDebug() << "MediaItems: Destructing media" << m_mediaId << "with fileId" << m_fileId;
        FileManager::instance().removeMediaAssociation(m_mediaId);
    }
}

ResizableMediaBase::ResizableMediaBase(const QSize& baseSizePx, int visualSizePx, int selectionSizePx, const QString& filename)
{
    m_lifetimeToken = std::make_shared<bool>(true);
    m_visualSize = qMax(4, visualSizePx);
    m_selectionSize = qMax(m_visualSize, selectionSizePx);
    setFlags(QGraphicsItem::ItemIsMovable | QGraphicsItem::ItemIsSelectable | QGraphicsItem::ItemSendsGeometryChanges);
    setAcceptHoverEvents(true);
    m_baseSize = baseSizePx;
    setScale(1.0);
    setZValue(1.0);
    m_filename = filename;
    // Generate a stable unique identifier at creation time (used to disambiguate duplicates)
    m_mediaId = QUuid::createUuid().toString(QUuid::WithoutBraces);
    initializeOverlays();
}

std::weak_ptr<bool> ResizableMediaBase::lifetimeGuard() const {
    return m_lifetimeToken;
}

void ResizableMediaBase::cancelFade() {
    if (m_fadeAnimation) {
        m_fadeAnimation->stop();
        delete m_fadeAnimation;
        m_fadeAnimation = nullptr;
    }
}

void ResizableMediaBase::fadeContentIn(double seconds) {
    cancelFade();
    // Ensure visible state and starting opacity
    m_contentVisible = true;
    if (seconds <= 0.0) { m_contentDisplayOpacity = 1.0; update(); return; }
    if (m_contentDisplayOpacity <= 0.0 || m_contentDisplayOpacity > 1.0) m_contentDisplayOpacity = 0.0;
    m_fadeAnimation = new QVariantAnimation();
    m_fadeAnimation->setStartValue(m_contentDisplayOpacity);
    m_fadeAnimation->setEndValue(1.0);
    int durationMs = static_cast<int>(seconds * 1000.0);
    m_fadeAnimation->setDuration(std::max(1, durationMs));
    QObject::connect(m_fadeAnimation, &QVariantAnimation::valueChanged, [this](const QVariant& v){
        m_contentDisplayOpacity = v.toDouble();
        update();
    });
    QObject::connect(m_fadeAnimation, &QVariantAnimation::finished, [this](){
        // clamp and cleanup
        m_contentDisplayOpacity = 1.0;
        if (m_fadeAnimation) { m_fadeAnimation->deleteLater(); m_fadeAnimation = nullptr; }
        update();
    });
    m_fadeAnimation->start();
}

void ResizableMediaBase::fadeContentOut(double seconds) {
    cancelFade();
    // Keep visible during fade so we can animate down; we flip flag at end
    if (seconds <= 0.0) { m_contentDisplayOpacity = 0.0; m_contentVisible = false; update(); return; }
    if (m_contentDisplayOpacity < 0.0 || m_contentDisplayOpacity > 1.0) m_contentDisplayOpacity = 1.0;
    m_contentVisible = true;
    m_fadeAnimation = new QVariantAnimation();
    m_fadeAnimation->setStartValue(m_contentDisplayOpacity);
    m_fadeAnimation->setEndValue(0.0);
    int durationMs = static_cast<int>(seconds * 1000.0);
    m_fadeAnimation->setDuration(std::max(1, durationMs));
    QObject::connect(m_fadeAnimation, &QVariantAnimation::valueChanged, [this](const QVariant& v){
        m_contentDisplayOpacity = v.toDouble();
        update();
    });
    QObject::connect(m_fadeAnimation, &QVariantAnimation::finished, [this](){
        m_contentDisplayOpacity = 0.0;
        m_contentVisible = false; // finalize hidden state
        if (m_fadeAnimation) { m_fadeAnimation->deleteLater(); m_fadeAnimation = nullptr; }
        update();
    });
    m_fadeAnimation->start();
}

void ResizableMediaBase::setMediaSettingsState(const MediaSettingsState& state) {
    m_mediaSettings = state;
    qreal finalOpacity = 1.0;
    if (m_mediaSettings.opacityOverrideEnabled) {
        bool ok = false;
        int val = m_mediaSettings.opacityText.trimmed().toInt(&ok);
        if (!ok) val = 100;
        val = std::clamp(val, 0, 100);
        finalOpacity = static_cast<qreal>(val) / 100.0;
    }
    setContentOpacity(finalOpacity);
}

bool ResizableMediaBase::autoDisplayEnabled() const {
    return m_mediaSettings.displayAutomatically;
}

int ResizableMediaBase::autoDisplayDelayMs() const {
    if (!autoDisplayEnabled() || !m_mediaSettings.displayDelayEnabled) return 0;
    bool ok = false;
    int value = m_mediaSettings.displayDelayText.trimmed().toInt(&ok);
    if (!ok || value < 0) return 0;
    return value * 1000;
}

bool ResizableMediaBase::autoPlayEnabled() const {
    return m_mediaSettings.playAutomatically;
}

int ResizableMediaBase::autoPlayDelayMs() const {
    if (!autoPlayEnabled() || !m_mediaSettings.playDelayEnabled) return 0;
    bool ok = false;
    int value = m_mediaSettings.playDelayText.trimmed().toInt(&ok);
    if (!ok || value < 0) return 0;
    return value * 1000;
}

double ResizableMediaBase::fadeInDurationSeconds() const {
    if (!m_mediaSettings.fadeInEnabled) return 0.0;
    QString text = m_mediaSettings.fadeInText.trimmed();
    if (text == QStringLiteral("∞") || text.isEmpty() || text == QStringLiteral("...")) return 0.0;
    text.replace(',', '.');
    bool ok = false;
    double value = text.toDouble(&ok);
    if (!ok || value < 0.0) return 0.0;
    return std::clamp(value, 0.0, 3600.0);
}

double ResizableMediaBase::fadeOutDurationSeconds() const {
    if (!m_mediaSettings.fadeOutEnabled) return 0.0;
    QString text = m_mediaSettings.fadeOutText.trimmed();
    if (text == QStringLiteral("∞") || text.isEmpty() || text == QStringLiteral("...")) return 0.0;
    text.replace(',', '.');
    bool ok = false;
    double value = text.toDouble(&ok);
    if (!ok || value < 0.0) return 0.0;
    return std::clamp(value, 0.0, 3600.0);
}

bool ResizableMediaBase::opacityOverrideEnabled() const {
    return m_mediaSettings.opacityOverrideEnabled;
}

int ResizableMediaBase::opacityPercent() const {
    bool ok = false;
    int value = m_mediaSettings.opacityText.trimmed().toInt(&ok);
    if (!ok) value = 100;
    return std::clamp(value, 0, 100);
}

void ResizableMediaBase::setSourcePath(const QString& p) {
    m_sourcePath = p;
    
    // If we have a valid file path, register it with FileManager
    if (!p.isEmpty()) {
        m_fileId = FileManager::instance().getOrCreateFileId(p);
        FileManager::instance().associateMediaWithFile(m_mediaId, m_fileId);
    }
}

void ResizableMediaBase::setHeightOfMediaOverlaysPx(int px) { heightOfMediaOverlays = px; }
int  ResizableMediaBase::getHeightOfMediaOverlaysPx() { return heightOfMediaOverlays; }
void ResizableMediaBase::setCornerRadiusOfMediaOverlaysPx(int px) { cornerRadiusOfMediaOverlays = std::max(0, px); }
int  ResizableMediaBase::getCornerRadiusOfMediaOverlaysPx() { return cornerRadiusOfMediaOverlays; }

void ResizableMediaBase::requestLabelRelayout() { updateOverlayLayout(); }

bool ResizableMediaBase::isOnHandleAtItemPos(const QPointF& itemPos) const { return hitTestHandle(itemPos) != None; }

bool ResizableMediaBase::beginResizeAtScenePos(const QPointF& scenePos) {
    Handle h = hitTestHandle(mapFromScene(scenePos));
    if (h == None) return false;
    m_activeHandle = h;
    m_fixedItemPoint = handlePoint(opposite(m_activeHandle));
    m_fixedScenePoint = mapToScene(m_fixedItemPoint);
    m_initialScale = scale();
    const qreal d = std::hypot(scenePos.x() - m_fixedScenePoint.x(), scenePos.y() - m_fixedScenePoint.y());
    m_initialGrabDist = (d > 1e-6) ? d : 1e-6;
    grabMouse();
    return true;
}

Qt::CursorShape ResizableMediaBase::cursorForScenePos(const QPointF& scenePos) const {
    switch (hitTestHandle(mapFromScene(scenePos))) {
        case TopLeft:
        case BottomRight:
            return Qt::SizeFDiagCursor;
        case TopRight:
        case BottomLeft:
            return Qt::SizeBDiagCursor;
        case LeftMid:
        case RightMid:
            return Qt::SizeHorCursor;
        case TopMid:
        case BottomMid:
            return Qt::SizeVerCursor;
        default:
            return Qt::ArrowCursor;
    }
}

void ResizableMediaBase::setHandleVisualSize(int px) {
    int newVisual = qMax(4, px);
    int newSelection = qMax(m_selectionSize, newVisual);
    if (newSelection != m_selectionSize) {
        prepareGeometryChange();
        m_selectionSize = newSelection;
    }
    m_visualSize = newVisual;
    update();
}

void ResizableMediaBase::setHandleSelectionSize(int px) {
    int newSel = qMax(4, px);
    if (newSel != m_selectionSize) {
        prepareGeometryChange();
        m_selectionSize = newSel;
        update();
    }
}

QRectF ResizableMediaBase::boundingRect() const {
    // Always inflate by handle size for consistent selection area
        return QRectF(0, 0, m_baseSize.width(), m_baseSize.height());
}

QPainterPath ResizableMediaBase::shape() const {
    QPainterPath path; 
    QRectF mediaRect(0,0, m_baseSize.width(), m_baseSize.height());
    // Always include the inflated outer rect so near-edge clicks (future handle zones) select.
    // This single expanded rectangle covers all handle areas without overlap issues.
    qreal pad = toItemLengthFromPixels(m_selectionSize) / 2.0;
    path.addRect(mediaRect.adjusted(-pad, -pad, pad, pad));
    return path;
}

QVariant ResizableMediaBase::itemChange(GraphicsItemChange change, const QVariant &value) {
    // Snap requested position changes to the scene pixel grid
    if (change == ItemPositionChange && value.canConvert<QPointF>()) {
        QPointF p = value.toPointF();
        
        // First apply pixel grid snapping
        p = snapPointToGrid(p);
        
        // Disable movement screen-border snapping (Shift) during ANY active resize (corner or midpoint)
        // to prevent the opposite/fixed corner from being repositioned while scaling.
        bool anyResizeActive = (m_activeHandle != None);
        if (s_screenSnapCallback && !anyResizeActive) {
            const bool shiftPressed = QGuiApplication::keyboardModifiers().testFlag(Qt::ShiftModifier);
            if (shiftPressed) {
                const QRectF mediaBounds(0, 0, m_baseSize.width() * scale(), m_baseSize.height() * scale());
                p = s_screenSnapCallback(p, mediaBounds, shiftPressed, this);
            }
        }
        
        return QVariant(p);
    }
    if (change == ItemSelectedChange) {
        prepareGeometryChange();
    }
    if (change == ItemSelectedHasChanged) {
        updateOverlayLayout();
        updateOverlayVisibility();
    }
    if (change == ItemTransformHasChanged || change == ItemPositionHasChanged) {
        updateOverlayLayout();
    }
    return QGraphicsItem::itemChange(change, value);
}

void ResizableMediaBase::mousePressEvent(QGraphicsSceneMouseEvent* event) {
    // When not selected, clicking anywhere inside the item's bounding rect (including where handles
    // would appear) should select the item. Only treat handle hits as resize starts if already selected.
    if (isSelected()) {
        m_activeHandle = hitTestHandle(event->pos());
        if (m_activeHandle != None) {
            m_fixedItemPoint = handlePoint(opposite(m_activeHandle));
            m_fixedScenePoint = mapToScene(m_fixedItemPoint);
            m_initialScale = scale();
            const qreal d = std::hypot(event->scenePos().x() - m_fixedScenePoint.x(), event->scenePos().y() - m_fixedScenePoint.y());
            m_initialGrabDist = (d > 1e-6) ? d : 1e-6;
            event->accept();
            return;
        }
    } else {
        // If not selected, ensure this press is treated as a normal selection click (no handle pre-emption)
        m_activeHandle = None;
    }
    QGraphicsItem::mousePressEvent(event);
}

void ResizableMediaBase::mouseMoveEvent(QGraphicsSceneMouseEvent* event) {
    if (m_activeHandle != None) {
        const QPointF sceneDelta = event->scenePos() - m_fixedScenePoint;
        const QPointF movingItemPoint = handlePoint(m_activeHandle);
        const QPointF itemVec = movingItemPoint - m_fixedItemPoint; // in item coords
        if (std::abs(itemVec.x()) < 1e-12 && std::abs(itemVec.y()) < 1e-12) { QGraphicsItem::mouseMoveEvent(event); return; }

        qreal targetScale = scale();
        bool axisLocked = false;
        if (m_activeHandle == LeftMid || m_activeHandle == RightMid) axisLocked = true; // horizontal only
        if (m_activeHandle == TopMid || m_activeHandle == BottomMid) axisLocked = true; // vertical only

    QPointF desiredMovingCornerScene; // carry outside to translation phase
    bool cornerSnapped = false;
    if (!axisLocked) {
            const bool altPressed = QGuiApplication::keyboardModifiers().testFlag(Qt::AltModifier);
            if (!altPressed) {
                // Corner style uniform scaling (original logic simplified)
                const qreal currDist = std::hypot(sceneDelta.x(), sceneDelta.y());
                qreal newScale = m_initialScale * (currDist / (m_initialGrabDist > 0 ? m_initialGrabDist : 1e-6));
                newScale = std::clamp<qreal>(newScale, 0.05, 100.0);
                qreal finalScale = newScale;
                if (s_resizeSnapCallback && QGuiApplication::keyboardModifiers().testFlag(Qt::ShiftModifier)) {
                    auto feedback = s_resizeSnapCallback(newScale, m_fixedScenePoint, m_fixedItemPoint, m_baseSize, true, this);
                    finalScale = feedback.scale;
                    cornerSnapped = feedback.cornerSnapped;
                    desiredMovingCornerScene = feedback.snappedMovingCornerScene;
                }
                // Pixel snap uniform
                const qreal desiredW = finalScale * static_cast<qreal>(m_baseSize.width());
                const qreal desiredH = finalScale * static_cast<qreal>(m_baseSize.height());
                const qreal snappedW = std::round(desiredW);
                const qreal snappedH = std::round(desiredH);
                const qreal sFromW = (m_baseSize.width()  > 0) ? (snappedW / static_cast<qreal>(m_baseSize.width()))  : finalScale;
                const qreal sFromH = (m_baseSize.height() > 0) ? (snappedH / static_cast<qreal>(m_baseSize.height())) : finalScale;
                targetScale = (std::abs(sFromW - finalScale) <= std::abs(sFromH - finalScale)) ? sFromW : sFromH;
                targetScale = std::clamp<qreal>(targetScale, 0.05, 100.0);
                m_lastAxisAltStretch = false;
                // Reset corner stretch state if previously used
                if (m_cornerStretchOrigCaptured) m_cornerStretchOrigCaptured = false;
            } else {
                // Alt + corner: non-uniform two-axis stretch by directly changing base size (independent width/height)
                // Bake current uniform scale into base size on first Alt use for this interaction
                if (!m_cornerStretchOrigCaptured) {
                    qreal s = scale();
                    if (std::abs(s - 1.0) > 1e-9) {
                        prepareGeometryChange();
                        m_baseSize.setWidth(std::max(1, int(std::round(m_baseSize.width() * s))));
                        m_baseSize.setHeight(std::max(1, int(std::round(m_baseSize.height() * s))));
                        setScale(1.0);
                        // Re-evaluate fixed corner scene point after transform change
                        m_fixedItemPoint = handlePoint(opposite(m_activeHandle));
                        m_fixedScenePoint = mapToScene(m_fixedItemPoint);
                    }
                    // Capture initial cursor offsets along X/Y relative to moving corner edge endpoints.
                    QPointF movingCornerScene = mapToScene(handlePoint(m_activeHandle));
                    QPointF cursor = event->scenePos();
                    // Normalize deltas so outward drag increases positive dimension similarly for all corners.
                    qreal dx = cursor.x() - movingCornerScene.x();
                    qreal dy = cursor.y() - movingCornerScene.y();
                    if (m_activeHandle == TopLeft || m_activeHandle == BottomLeft) dx = -dx; // outward = to left for left corners
                    if (m_activeHandle == TopLeft || m_activeHandle == TopRight)  dy = -dy; // outward = up for top corners
                    m_cornerStretchInitialOffsetX = dx;
                    m_cornerStretchInitialOffsetY = dy;
                    m_cornerStretchOriginalBaseSize = m_baseSize;
                    m_cornerStretchOrigCaptured = true;
                }
                // Compute current outward deltas
                QPointF cursor = event->scenePos();
                QPointF fixedCornerScene = m_fixedScenePoint; // preserved anchor
                // Determine extent along X: distance from fixed corner to cursor, normalized by corner orientation
                QPointF movingCornerItem = handlePoint(m_activeHandle);
                QPointF movingCornerScene = mapToScene(movingCornerItem);
                // Outward direction normalization like above
                qreal dxRaw = cursor.x() - movingCornerScene.x();
                qreal dyRaw = cursor.y() - movingCornerScene.y();
                if (m_activeHandle == TopLeft || m_activeHandle == BottomLeft) dxRaw = -dxRaw;
                if (m_activeHandle == TopLeft || m_activeHandle == TopRight)  dyRaw = -dyRaw;
                qreal desiredW = std::abs((movingCornerScene.x() - fixedCornerScene.x())) + dxRaw - m_cornerStretchInitialOffsetX;
                qreal desiredH = std::abs((movingCornerScene.y() - fixedCornerScene.y())) + dyRaw - m_cornerStretchInitialOffsetY;
                desiredW = std::max<qreal>(1.0, desiredW);
                desiredH = std::max<qreal>(1.0, desiredH);
                // Shift+Alt snapping: first attempt corner snap (intersection). If that fails, fall back to per-axis.
                const bool shiftPressed = QGuiApplication::keyboardModifiers().testFlag(Qt::ShiftModifier);
                if (shiftPressed && scene() && !scene()->views().isEmpty()) {
                    if (auto* scView = qobject_cast<ScreenCanvas*>(scene()->views().first())) {
                        auto cornerRes = scView->applyCornerAltSnapWithHysteresis(this, m_activeHandle, m_fixedScenePoint, m_cornerStretchOriginalBaseSize, desiredW, desiredH);
                        if (cornerRes.cornerSnapped) {
                            desiredW = cornerRes.snappedW;
                            desiredH = cornerRes.snappedH;
                        } else {
                            // Fallback to per-axis snapping
                            Handle hForX = (m_activeHandle == TopLeft || m_activeHandle == BottomLeft) ? LeftMid : RightMid;
                            qreal eqScaleX = (m_cornerStretchOriginalBaseSize.width() > 0) ? (desiredW / m_cornerStretchOriginalBaseSize.width()) : 1.0;
                            eqScaleX = std::clamp<qreal>(eqScaleX, 0.05, 100.0);
                            qreal snappedScaleX = scView->applyAxisSnapWithHysteresis(this, eqScaleX, m_fixedScenePoint, m_cornerStretchOriginalBaseSize, hForX);
                            desiredW = snappedScaleX * m_cornerStretchOriginalBaseSize.width();
                            Handle hForY = (m_activeHandle == TopLeft || m_activeHandle == TopRight) ? TopMid : BottomMid;
                            qreal eqScaleY = (m_cornerStretchOriginalBaseSize.height() > 0) ? (desiredH / m_cornerStretchOriginalBaseSize.height()) : 1.0;
                            eqScaleY = std::clamp<qreal>(eqScaleY, 0.05, 100.0);
                            qreal snappedScaleY = scView->applyAxisSnapWithHysteresis(this, eqScaleY, m_fixedScenePoint, m_cornerStretchOriginalBaseSize, hForY);
                            desiredH = snappedScaleY * m_cornerStretchOriginalBaseSize.height();
                        }
                    }
                } else if (m_axisSnapActive) {
                    // If previously snapping and Shift released, clear state
                    m_axisSnapActive = false; m_axisSnapHandle = None; m_axisSnapTargetScale = 1.0;
                }
                // Apply new base size (non-uniform)
                QSize newBase = m_baseSize;
                int newW = std::max(1, static_cast<int>(std::round(desiredW)));
                int newH = std::max(1, static_cast<int>(std::round(desiredH)));
                if (newW != newBase.width() || newH != newBase.height()) {
                    prepareGeometryChange();
                    newBase.setWidth(newW); newBase.setHeight(newH); m_baseSize = newBase;
                    // Update fixed item point (opposite corner) but keep scene anchor stable
                    m_fixedItemPoint = handlePoint(opposite(m_activeHandle));
                }
                targetScale = scale(); // keep overall scale (should be 1 after bake)
                m_lastAxisAltStretch = true; // reuse flag to denote custom base mutation
                m_fillContentWithoutAspect = true; // persist fill behavior after non-uniform corner stretch
            }
        } else {
            // Axis-only resize (side midpoint handles). Supports two modes:
            //  1) Default (no Alt): uniform scaling along both axes derived from movement on one axis.
            //  2) Alt/Option held: non-uniform stretch along ONLY the active axis (width OR height).
            const bool horizontalHandle = (m_activeHandle == LeftMid || m_activeHandle == RightMid);
            const bool altPressed = QGuiApplication::keyboardModifiers().testFlag(Qt::AltModifier);
            qreal baseLenAxis = horizontalHandle ? m_baseSize.width() : m_baseSize.height();
            qreal deltaScene = horizontalHandle ? (event->scenePos().x() - m_fixedScenePoint.x())
                                                : (event->scenePos().y() - m_fixedScenePoint.y());
            if (m_activeHandle == LeftMid || m_activeHandle == TopMid) deltaScene = -deltaScene; // normalize outward growth
            qreal extent = std::abs(deltaScene);
            qreal newScaleAxis = extent / (baseLenAxis > 0 ? baseLenAxis : 1.0);
            newScaleAxis = std::clamp<qreal>(newScaleAxis, 0.05, 100.0);
            if (!altPressed) {
                // Legacy behavior: uniform scale from axis drag
                targetScale = newScaleAxis;
                const bool shiftPressed = QGuiApplication::keyboardModifiers().testFlag(Qt::ShiftModifier);
                if (shiftPressed) {
                    if (scene() && !scene()->views().isEmpty()) {
                        if (auto* scView = qobject_cast<ScreenCanvas*>(scene()->views().first())) {
                            targetScale = scView->applyAxisSnapWithHysteresis(this, targetScale, m_fixedScenePoint, m_baseSize, m_activeHandle);
                        }
                    }
                } else if (m_axisSnapActive) {
                    // User released Shift mid-resize: drop snap state
                    m_axisSnapActive = false; m_axisSnapHandle = None; m_axisSnapTargetScale = 1.0;
                }
                m_lastAxisAltStretch = false;
                // Do not reset m_fillContentWithoutAspect here; if user previously non-uniform stretched, keep it
            } else {
                // Alt stretch: modify only one dimension by changing base size horizontally or vertically.
                // We keep overall QGraphicsItem scale() unchanged (targetScale stays current scale) and
                // update m_baseSize dimension directly for instantaneous non-uniform scaling.
                m_lastAxisAltStretch = true;
                m_fillContentWithoutAspect = true; // persist fill behavior after non-uniform axis stretch
                if (!m_axisStretchOrigCaptured) {
                    // First Alt movement in this interaction: bake current uniform scale into base size
                    qreal s = scale();
                    if (std::abs(s - 1.0) > 1e-9) {
                        prepareGeometryChange();
                        m_baseSize.setWidth(std::max(1, int(std::round(m_baseSize.width() * s))));
                        m_baseSize.setHeight(std::max(1, int(std::round(m_baseSize.height() * s))));
                        setScale(1.0); // reset scale so subsequent width math is linear
                        // Re-evaluate fixed corner scene point after transform change
                        m_fixedItemPoint = handlePoint(opposite(m_activeHandle));
                        m_fixedScenePoint = mapToScene(m_fixedItemPoint);
                    }
                    // Capture initial cursor offset to preserve relative positioning
                    QPointF currentMovingEdgeScene = mapToScene(handlePoint(m_activeHandle));
                    qreal cursorToEdgeDist = horizontalHandle ? (event->scenePos().x() - currentMovingEdgeScene.x())
                                                              : (event->scenePos().y() - currentMovingEdgeScene.y());
                    // Apply the same direction normalization as deltaScene to ensure consistency
                    if (m_activeHandle == LeftMid || m_activeHandle == TopMid) cursorToEdgeDist = -cursorToEdgeDist;
                    m_axisStretchInitialOffset = cursorToEdgeDist;
                    m_axisStretchOriginalBaseSize = m_baseSize;
                    m_axisStretchOrigCaptured = true;
                }
                // Check for Shift+Alt snapping before applying size changes
                const bool shiftPressed = QGuiApplication::keyboardModifiers().testFlag(Qt::ShiftModifier);
                qreal desiredAxisSize = extent - m_axisStretchInitialOffset;
                
                if (shiftPressed) {
                    // Apply axis snapping for Alt stretch mode
                    // Convert the desired axis size to an equivalent scale for snapping logic
                    qreal origAxisSize = horizontalHandle ? m_axisStretchOriginalBaseSize.width() : m_axisStretchOriginalBaseSize.height();
                    qreal equivalentScale = (origAxisSize > 0) ? (desiredAxisSize / origAxisSize) : 1.0;
                    equivalentScale = std::clamp<qreal>(equivalentScale, 0.05, 100.0);
                    
                    if (scene() && !scene()->views().isEmpty()) {
                        if (auto* scView = qobject_cast<ScreenCanvas*>(scene()->views().first())) {
                            qreal snappedScale = scView->applyAxisSnapWithHysteresis(this, equivalentScale, m_fixedScenePoint, m_axisStretchOriginalBaseSize, m_activeHandle);
                            // Convert snapped scale back to desired size
                            desiredAxisSize = snappedScale * origAxisSize;
                        }
                    }
                } else if (m_axisSnapActive) {
                    // User released Shift mid-resize: drop snap state
                    m_axisSnapActive = false; m_axisSnapHandle = None; m_axisSnapTargetScale = 1.0;
                }

                QSize newBase = m_baseSize;
                if (horizontalHandle) {
                    int newW = std::max(1, static_cast<int>(std::round(desiredAxisSize)));
                    if (newW != newBase.width()) {
                        prepareGeometryChange();
                        newBase.setWidth(newW);
                        m_baseSize = newBase;
                    }
                } else {
                    int newH = std::max(1, static_cast<int>(std::round(desiredAxisSize)));
                    if (newH != newBase.height()) {
                        prepareGeometryChange();
                        newBase.setHeight(newH);
                        m_baseSize = newBase;
                    }
                }
                // Keep scale unchanged; anchor fixed side. We ONLY update the item-space point for the fixed side
                // (its coordinates change when base size changes) but we DO NOT overwrite the stored scene-space
                // fixed point. Overwriting m_fixedScenePoint caused the apparent growth to occur in the opposite
                // direction for left & bottom handles because the anchor drifted. By preserving the original
                // scene anchor captured at press (or after the initial scale bake), the resizing now correctly
                // expands/contracts toward the dragged midpoint handle.
                m_fixedItemPoint = handlePoint(opposite(m_activeHandle));
                // m_fixedScenePoint intentionally left unchanged here.
                targetScale = scale();
            }
        }

        QPointF snappedPos = m_fixedScenePoint - targetScale * m_fixedItemPoint;
        if (axisLocked && m_lastAxisAltStretch) {
            // During Alt axis stretch we changed base size: ensure the fixed midpoint side stays anchored.
            // Recompute position so that the fixed item point maps exactly to fixedScenePoint.
            snappedPos = m_fixedScenePoint - targetScale * m_fixedItemPoint;
        }
        if (!axisLocked) {
            // If corner snapped we may need to translate so moving corner matches target exactly
            if (cornerSnapped && m_activeHandle != None) {
                QPointF newMovingCornerItem = handlePoint(m_activeHandle); // moving handle item point
                QPointF newMovingCornerScene = snappedPos + targetScale * newMovingCornerItem;
                if (!desiredMovingCornerScene.isNull()) {
                    QPointF delta = desiredMovingCornerScene - newMovingCornerScene;
                    snappedPos += delta; // translate whole item so moving corner aligns
                }
            }
        }
    setScale(targetScale);
    setPos(snappedPos);
        // Explicitly update overlay layout during corner resize so the top anchor tracks
        // width/height changes in real time. This fixes the observed drift where the
        // top panel (filename + buttons) lags behind when resizing from the bottom-right.
        updateOverlayLayout();
        onInteractiveGeometryChanged();
        event->accept();
        return;
    }
    QGraphicsItem::mouseMoveEvent(event);
}

void ResizableMediaBase::mouseReleaseEvent(QGraphicsSceneMouseEvent* event) {
    if (m_activeHandle != None) {
        // For side midpoint handles, perform a one-time pixel snap to avoid accumulated subpixel drift
        if (m_activeHandle == LeftMid || m_activeHandle == RightMid || m_activeHandle == TopMid || m_activeHandle == BottomMid) {
            const bool horizontal = (m_activeHandle == LeftMid || m_activeHandle == RightMid);
            qreal baseLen = horizontal ? m_baseSize.width() : m_baseSize.height();
            // Compute current world length along axis
            qreal currLen = baseLen * scale();
            qreal snappedLen = std::round(currLen);
            if (baseLen > 0) {
                qreal snappedScale = std::clamp<qreal>(snappedLen / baseLen, 0.05, 100.0);
                // Adjust position so fixed side remains anchored
                QPointF fixedScene = m_fixedScenePoint; // was captured at press
                setScale(snappedScale);
                setPos(fixedScene - snappedScale * m_fixedItemPoint);
            }
        }
    m_activeHandle = None;
    m_axisStretchOrigCaptured = false; // reset for next interaction
        m_cornerStretchOrigCaptured = false; // reset corner stretch state
        // One final layout sync after any resize completes to guarantee centering.
        updateOverlayLayout();
        // Clear axis snap hysteresis state after resize interaction ends
        if (m_axisSnapActive) {
            m_axisSnapActive = false;
            m_axisSnapHandle = None;
            m_axisSnapTargetScale = 1.0;
        }
        ungrabMouse();
        onInteractiveGeometryChanged();
        event->accept();
        return;
    }
    QGraphicsItem::mouseReleaseEvent(event);
}

void ResizableMediaBase::prepareForDeletion() {
    if (m_beingDeleted) return;
    m_beingDeleted = true;
    // Cancel any active resize interaction.
    if (m_activeHandle != None) { m_activeHandle = None; ungrabMouse(); }
    cancelFade();
    setContentVisible(false);
    m_contentDisplayOpacity = 0.0;
    setVisible(false);
    update();
    // Fully detach overlay panels: remove their background graphics items from the scene so no further relayout occurs.
    auto detachPanel = [&](std::unique_ptr<OverlayPanel>& panel){
        if (!panel) return;
        panel->setVisible(false);
        // We rely on panel destructor later; clear elements now to drop graphics.
        panel->clearElements();
    };
    detachPanel(m_topPanel);
    // Settings panel removed - now managed globally by ScreenCanvas
}

void ResizableMediaBase::showWithConfiguredFade() {
    // Mirror logic from visibility toggle (show branch)
    double fadeInSeconds = fadeInDurationSeconds();
    cancelFade();
    if (fadeInSeconds > 0.0) {
        if (m_contentDisplayOpacity <= 0.0) m_contentDisplayOpacity = 0.0;
        setContentVisible(true);
        fadeContentIn(fadeInSeconds);
    } else {
        setContentVisible(true);
        m_contentDisplayOpacity = 1.0;
        update();
    }
    // Update overlay visibility toggle button icon/state if present
    if (m_topPanel) {
        if (auto el = m_topPanel->findElement("visibility_toggle")) {
            el->setState(OverlayElement::Toggled);
            if (auto* btnEl = dynamic_cast<OverlayButtonElement*>(el.get())) {
                btnEl->setSvgIcon(":/icons/icons/visibility-on.svg");
            }
        }
    }
}

void ResizableMediaBase::hideWithConfiguredFade() {
    double fadeOutSeconds = fadeOutDurationSeconds();
    cancelFade();
    if (fadeOutSeconds > 0.0) {
        fadeContentOut(fadeOutSeconds);
    } else {
        setContentVisible(false);
        m_contentDisplayOpacity = 0.0;
        update();
    }
    if (m_topPanel) {
        if (auto el = m_topPanel->findElement("visibility_toggle")) {
            el->setState(OverlayElement::Normal);
            if (auto* btnEl = dynamic_cast<OverlayButtonElement*>(el.get())) {
                btnEl->setSvgIcon(":/icons/icons/visibility-off.svg");
            }
        }
    }
}

void ResizableMediaBase::showImmediateNoFade() {
    cancelFade();
    setContentVisible(true);
    m_contentDisplayOpacity = 1.0;
    update();
    if (m_topPanel) {
        if (auto el = m_topPanel->findElement("visibility_toggle")) {
            el->setState(OverlayElement::Toggled);
            if (auto* btnEl = dynamic_cast<OverlayButtonElement*>(el.get())) {
                btnEl->setSvgIcon(":/icons/icons/visibility-on.svg");
            }
        }
    }
}

void ResizableMediaBase::hideImmediateNoFade() {
    cancelFade();
    setContentVisible(false);
    m_contentDisplayOpacity = 0.0;
    update();
    if (m_topPanel) {
        if (auto el = m_topPanel->findElement("visibility_toggle")) {
            el->setState(OverlayElement::Normal);
            if (auto* btnEl = dynamic_cast<OverlayButtonElement*>(el.get())) {
                btnEl->setSvgIcon(":/icons/icons/visibility-off.svg");
            }
        }
    }
}

void ResizableMediaBase::hoverMoveEvent(QGraphicsSceneHoverEvent* event) { QGraphicsItem::hoverMoveEvent(event); }
void ResizableMediaBase::hoverLeaveEvent(QGraphicsSceneHoverEvent* event) { QGraphicsItem::hoverLeaveEvent(event); }

void ResizableMediaBase::paintSelectionAndLabel(QPainter* painter) {
    Q_UNUSED(painter);
    // Selection chrome (borders + handles) is now drawn by ScreenCanvas as high-z scene items
    // to ensure it appears above all media regardless of Z order. We intentionally no-op here
    // to avoid duplicate visuals.
}

ResizableMediaBase::Handle ResizableMediaBase::hitTestHandle(const QPointF& p) const {
    if (!isSelected()) return None;
    const qreal s = toItemLengthFromPixels(m_selectionSize);
    QRectF br(0, 0, m_baseSize.width(), m_baseSize.height());
    auto contains = [&](const QPointF& center){ return QRectF(center - QPointF(s/2,s/2), QSizeF(s,s)).contains(p); };
    if (contains(br.topLeft())) return TopLeft;
    if (contains(QPointF(br.right(), br.top()))) return TopRight;
    if (contains(QPointF(br.left(), br.bottom()))) return BottomLeft;
    if (contains(br.bottomRight())) return BottomRight;
    // Midpoints
    if (contains(QPointF(br.center().x(), br.top()))) return TopMid;
    if (contains(QPointF(br.center().x(), br.bottom()))) return BottomMid;
    if (contains(QPointF(br.left(), br.center().y()))) return LeftMid;
    if (contains(QPointF(br.right(), br.center().y()))) return RightMid;
    return None;
}

ResizableMediaBase::Handle ResizableMediaBase::opposite(Handle h) const {
    switch (h) {
        case TopLeft: return BottomRight;
        case TopRight: return BottomLeft;
        case BottomLeft: return TopRight;
        case BottomRight: return TopLeft;
        case LeftMid: return RightMid;
        case RightMid: return LeftMid;
        case TopMid: return BottomMid;
        case BottomMid: return TopMid;
        default: return None;
    }
}

QPointF ResizableMediaBase::handlePoint(Handle h) const {
    switch (h) {
        case TopLeft: return QPointF(0,0);
        case TopRight: return QPointF(m_baseSize.width(),0);
        case BottomLeft: return QPointF(0,m_baseSize.height());
        case BottomRight: return QPointF(m_baseSize.width(), m_baseSize.height());
        case LeftMid: return QPointF(0, m_baseSize.height()/2.0);
        case RightMid: return QPointF(m_baseSize.width(), m_baseSize.height()/2.0);
        case TopMid: return QPointF(m_baseSize.width()/2.0, 0);
        case BottomMid: return QPointF(m_baseSize.width()/2.0, m_baseSize.height());
        default: return QPointF(0,0);
    }
}

qreal ResizableMediaBase::toItemLengthFromPixels(int px) const {
    if (!scene() || scene()->views().isEmpty()) return px;
    QGraphicsView* v = scene()->views().first();
    QTransform itemToViewport = v->viewportTransform() * sceneTransform();
    qreal sx = std::hypot(itemToViewport.m11(), itemToViewport.m21());
    if (sx <= 1e-6) return px;
    return px / sx;
}

void ResizableMediaBase::initializeOverlays() {
    m_overlayStyle.cornerRadius = getCornerRadiusOfMediaOverlaysPx();
    if (getHeightOfMediaOverlaysPx() > 0) m_overlayStyle.defaultHeight = getHeightOfMediaOverlaysPx();
    m_topPanel = std::make_unique<OverlayPanel>(OverlayPanel::Top); m_topPanel->setStyle(m_overlayStyle);
    if (!m_filename.isEmpty()) {
    auto filenameElement = std::make_shared<OverlayTextElement>(m_filename, "filename");
    filenameElement->setMaxWidthPx(gOverlayFilenameMaxWidthPx);
        m_topPanel->addElement(filenameElement);
        // Insert an explicit row break so buttons are laid out on a second row below the filename.
        // Subsequent layout logic in OverlayPanel ensures the first row (filename) is widened
        // to match the exact width of the buttons row for a clean vertical stack.
        m_topPanel->newRow();
        
        // Add visibility toggle button (content visibility only)
        auto visibilityBtn = std::make_shared<OverlayButtonElement>(QString(), "visibility_toggle");
        visibilityBtn->setSvgIcon(":/icons/icons/visibility-on.svg"); // start visible
        visibilityBtn->setToggleOnly(true);
        visibilityBtn->setState(OverlayElement::Toggled); // initial: visible ON
        visibilityBtn->setOnClicked([this, btnRef=visibilityBtn]() {
            if (!btnRef) return;
            const bool currentlyVisible = (btnRef->state() == OverlayElement::Toggled);
            // Retrieve fade parameters from per-media settings
            const double fadeInSeconds = fadeInDurationSeconds();
            const double fadeOutSeconds = fadeOutDurationSeconds();
            // Always cancel any in-flight fade before starting a new transition or applying an instant change
            cancelFade();
            if (currentlyVisible) {
                // Switch to hidden using fade out if configured
                if (fadeOutSeconds > 0.0) {
                    fadeContentOut(fadeOutSeconds);
                } else {
                    setContentVisible(false);
                    m_contentDisplayOpacity = 0.0;
                    update();
                }
                btnRef->setState(OverlayElement::Normal);
                btnRef->setSvgIcon(":/icons/icons/visibility-off.svg");
            } else {
                // Switch to visible using fade in if configured
                if (fadeInSeconds > 0.0) {
                    // Start from 0 for a consistent fade-in if currently fully hidden
                    if (m_contentDisplayOpacity <= 0.0) m_contentDisplayOpacity = 0.0;
                    setContentVisible(true);
                    fadeContentIn(fadeInSeconds);
                } else {
                    setContentVisible(true);
                    m_contentDisplayOpacity = 1.0;
                    update();
                }
                btnRef->setState(OverlayElement::Toggled);
                btnRef->setSvgIcon(":/icons/icons/visibility-on.svg");
            }
        });
        m_topPanel->addElement(visibilityBtn);
        
        // Add bring forward button (Z-order up)
        auto bringForwardBtn = std::make_shared<OverlayButtonElement>(QString(), "bring_forward");
        bringForwardBtn->setSvgIcon(":/icons/icons/arrow-up.svg");
        bringForwardBtn->setToggleOnly(false); // Simple button, not a toggle
        bringForwardBtn->setState(OverlayElement::Normal);
        bringForwardBtn->setOnClicked([this]() {
            if (scene() && !scene()->views().isEmpty()) {
                if (auto* screenCanvas = qobject_cast<ScreenCanvas*>(scene()->views().first())) {
                    screenCanvas->moveMediaUp(this);
                }
            }
        });
        m_topPanel->addElement(bringForwardBtn);
        
        // Add bring backward button (Z-order down)
        auto bringBackwardBtn = std::make_shared<OverlayButtonElement>(QString(), "bring_backward");
        bringBackwardBtn->setSvgIcon(":/icons/icons/arrow-down.svg");
        bringBackwardBtn->setToggleOnly(false); // Simple button, not a toggle
        bringBackwardBtn->setState(OverlayElement::Normal);
        bringBackwardBtn->setOnClicked([this]() {
            if (scene() && !scene()->views().isEmpty()) {
                if (auto* screenCanvas = qobject_cast<ScreenCanvas*>(scene()->views().first())) {
                    screenCanvas->moveMediaDown(this);
                }
            }
        });
        m_topPanel->addElement(bringBackwardBtn);

        // Add delete button (remove media)
        auto deleteBtn = std::make_shared<OverlayButtonElement>(QString(), "delete_media");
        deleteBtn->setSvgIcon(":/icons/icons/delete.svg");
        deleteBtn->setToggleOnly(false);
        deleteBtn->setState(OverlayElement::Normal);
        deleteBtn->setOnClicked([this]() {
            if (scene() && !scene()->views().isEmpty()) {
                if (auto* screenCanvas = qobject_cast<ScreenCanvas*>(scene()->views().first())) {
                    ScreenCanvas::requestMediaDeletion(screenCanvas, this);
                    return;
                }
            }
            setVisible(false);
            setEnabled(false);
            prepareForDeletion();
            if (scene()) scene()->removeItem(this);
            delete this;
        });
        m_topPanel->addElement(deleteBtn);
    }
}

void ResizableMediaBase::updateOverlayVisibility() {
    // Show top overlay (filename + settings button) only when the item is selected,
    // matching bottom overlay behavior.
    bool shouldShowTop = isSelected() && !m_filename.isEmpty();
    if (m_topPanel) m_topPanel->setVisible(shouldShowTop);
    // Settings panel now managed globally by ScreenCanvas - no per-media panel logic needed
}

void ResizableMediaBase::updateOverlayLayout() {
    if (!scene() || scene()->views().isEmpty()) return;
    QGraphicsView* view = scene()->views().first();
    if (m_topPanel && !m_topPanel->scene()) m_topPanel->setScene(scene());
    QRectF itemRect(0,0,m_baseSize.width(), m_baseSize.height());
    QPointF topAnchorScene = mapToScene(QPointF(itemRect.center().x(), itemRect.top()));
    if (m_topPanel) m_topPanel->updateLayoutWithAnchor(topAnchorScene, view);
    // Settings panel now managed globally by ScreenCanvas
}

// ---------------- ResizablePixmapItem -----------------

ResizablePixmapItem::ResizablePixmapItem(const QPixmap& pm, int visualSizePx, int selectionSizePx, const QString& filename)
    : ResizableMediaBase(pm.size(), visualSizePx, selectionSizePx, filename), m_pix(pm) {}

void ResizablePixmapItem::paint(QPainter* painter, const QStyleOptionGraphicsItem* option, QWidget* widget) {
    Q_UNUSED(option); Q_UNUSED(widget);
    if (isContentVisible() || m_contentDisplayOpacity > 0.0) {
        qreal effective = contentOpacity() * m_contentDisplayOpacity;
        if (!m_pix.isNull() && effective > 0.0) {
            QSize target(baseSizePx());
            QPixmap toDraw = (m_pix.size() == target) ? m_pix : m_pix.scaled(target, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
            if (effective >= 0.999) {
                painter->drawPixmap(QPointF(0,0), toDraw);
            } else {
                painter->save();
                painter->setOpacity(effective);
                painter->drawPixmap(QPointF(0,0), toDraw);
                painter->restore();
            }
        }
    }
    paintSelectionAndLabel(painter);
}

// ---------------- ResizableVideoItem -----------------

ResizableVideoItem::ResizableVideoItem(const QString& filePath, int visualSizePx, int selectionSizePx, const QString& filename, int controlsFadeMs)
    : ResizableMediaBase(QSize(640,360), visualSizePx, selectionSizePx, filename)
{
    m_controlsFadeMs = std::max(0, controlsFadeMs);
    m_player = new QMediaPlayer();
    m_audio = new QAudioOutput();
    m_sink = new QVideoSink();
    m_player->setAudioOutput(m_audio);
    m_player->setVideoSink(m_sink);
    m_player->setSource(QUrl::fromLocalFile(filePath));


    QObject::connect(m_sink, &QVideoSink::videoFrameChanged, m_player, [this](const QVideoFrame& f){
        ++m_framesReceived;
        if (m_appSuspended) {
            ++m_framesSkipped;
            return;
        }
        if (!m_holdLastFrameAtEnd && f.isValid()) {
            if (!isVisibleInAnyView()) {
                ++m_framesSkipped;
                logFrameStats();
                return;
            }

            QImage converted = convertFrameToImage(f);
            if (converted.isNull()) {
                ++m_framesDropped;
                ++m_conversionFailures;
                if (m_conversionFailures <= 5 || (m_conversionFailures % 25) == 0) {
                    qWarning() << "ResizableVideoItem: frame conversion failed"
                               << "handleType=" << f.handleType()
                               << "pixelFormat=" << f.pixelFormat()
                               << "surface=" << f.surfaceFormat().pixelFormat();
                }
            } else {
                m_conversionFailures = 0;
                maybeAdoptFrameSize(f);
                m_lastFrameImage = std::move(converted);
                ++m_framesProcessed;
                if (m_primingFirstFrame && !m_firstFramePrimed) {
                    m_firstFramePrimed = true;
                    m_primingFirstFrame = false;
                    if (m_player) {
                        m_player->pause();
                        m_player->setPosition(0);
                    }
                    if (m_audio) m_audio->setMuted(m_savedMuted);
                    m_controlsLockedUntilReady = false;
                    m_controlsDidInitialFade = false;
                    if (isSelected()) {
                        setControlsVisible(true);
                        updateControlsLayout();
                        m_lastRepaintMs = 0;
                        update();
                        return;
                    }
                }
            }
            logFrameStats();
        }
        if (shouldRepaint()) {
            m_lastRepaintMs = QDateTime::currentMSecsSinceEpoch();
            update();
        }
    });

    // Floating controls (absolute px background + children) created after sink connection
    m_controlsBg = new QGraphicsRectItem();
    m_controlsBg->setPen(Qt::NoPen); m_controlsBg->setBrush(Qt::NoBrush); m_controlsBg->setZValue(12000.0);
    m_controlsBg->setFlag(QGraphicsItem::ItemIgnoresTransformations, true);
    m_controlsBg->setCacheMode(QGraphicsItem::DeviceCoordinateCache);
    m_controlsBg->setAcceptedMouseButtons(Qt::NoButton); m_controlsBg->setOpacity(0.0);
    if (scene()) scene()->addItem(m_controlsBg);

    // Helper function to apply 1px border to video control elements
    auto applyVideoBorder = [](QAbstractGraphicsShapeItem* item) {
        if (item) {
            QPen borderPen(AppColors::gOverlayBorderColor, 1);
            item->setPen(borderPen);
        }
    };
    
    auto makeSvg = [&](const char* path, QGraphicsItem* parent){ auto* svg = new QGraphicsSvgItem(path, parent); svg->setCacheMode(QGraphicsItem::DeviceCoordinateCache); svg->setZValue(12002.0); svg->setFlag(QGraphicsItem::ItemIgnoresTransformations, true); svg->setAcceptedMouseButtons(Qt::NoButton); return svg; };
    m_playBtnRectItem = new RoundedRectItem(m_controlsBg); m_playBtnRectItem->setBrush(m_overlayStyle.backgroundBrush()); m_playBtnRectItem->setZValue(12001.0); m_playBtnRectItem->setFlag(QGraphicsItem::ItemIgnoresTransformations, true); m_playBtnRectItem->setAcceptedMouseButtons(Qt::NoButton); applyVideoBorder(m_playBtnRectItem);
    m_playIcon = makeSvg(":/icons/icons/play.svg", m_playBtnRectItem);
    m_pauseIcon = makeSvg(":/icons/icons/pause.svg", m_playBtnRectItem); m_pauseIcon->setVisible(false);
    m_stopBtnRectItem = new RoundedRectItem(m_controlsBg); m_stopBtnRectItem->setBrush(m_overlayStyle.backgroundBrush()); m_stopBtnRectItem->setZValue(12001.0); m_stopBtnRectItem->setFlag(QGraphicsItem::ItemIgnoresTransformations, true); m_stopBtnRectItem->setAcceptedMouseButtons(Qt::NoButton); applyVideoBorder(m_stopBtnRectItem);
    m_stopIcon = makeSvg(":/icons/icons/stop.svg", m_stopBtnRectItem);
    m_repeatBtnRectItem = new RoundedRectItem(m_controlsBg); m_repeatBtnRectItem->setBrush(m_overlayStyle.backgroundBrush()); m_repeatBtnRectItem->setZValue(12001.0); m_repeatBtnRectItem->setFlag(QGraphicsItem::ItemIgnoresTransformations, true); m_repeatBtnRectItem->setAcceptedMouseButtons(Qt::NoButton); applyVideoBorder(m_repeatBtnRectItem);
    m_repeatIcon = makeSvg(":/icons/icons/loop.svg", m_repeatBtnRectItem);
    m_muteBtnRectItem = new RoundedRectItem(m_controlsBg); m_muteBtnRectItem->setBrush(m_overlayStyle.backgroundBrush()); m_muteBtnRectItem->setZValue(12001.0); m_muteBtnRectItem->setFlag(QGraphicsItem::ItemIgnoresTransformations, true); m_muteBtnRectItem->setAcceptedMouseButtons(Qt::NoButton); applyVideoBorder(m_muteBtnRectItem);
    m_muteIcon = makeSvg(":/icons/icons/volume-on.svg", m_muteBtnRectItem);
    m_muteSlashIcon = makeSvg(":/icons/icons/volume-off.svg", m_muteBtnRectItem); m_muteSlashIcon->setVisible(false);
    m_volumeBgRectItem = new QGraphicsRectItem(m_controlsBg); m_volumeBgRectItem->setPen(Qt::NoPen); m_volumeBgRectItem->setBrush(m_overlayStyle.backgroundBrush()); m_volumeBgRectItem->setZValue(12001.0); m_volumeBgRectItem->setFlag(QGraphicsItem::ItemIgnoresTransformations, true); m_volumeBgRectItem->setAcceptedMouseButtons(Qt::NoButton);
    m_volumeFillRectItem = new QGraphicsRectItem(m_volumeBgRectItem); m_volumeFillRectItem->setPen(Qt::NoPen); m_volumeFillRectItem->setBrush(QColor(74,144,226)); m_volumeFillRectItem->setZValue(12002.0); m_volumeFillRectItem->setFlag(QGraphicsItem::ItemIgnoresTransformations, true); m_volumeFillRectItem->setAcceptedMouseButtons(Qt::NoButton);
    m_progressBgRectItem = new QGraphicsRectItem(m_controlsBg); m_progressBgRectItem->setPen(Qt::NoPen); m_progressBgRectItem->setBrush(m_overlayStyle.backgroundBrush()); m_progressBgRectItem->setZValue(12001.0); m_progressBgRectItem->setFlag(QGraphicsItem::ItemIgnoresTransformations, true); m_progressBgRectItem->setAcceptedMouseButtons(Qt::NoButton);
    m_progressFillRectItem = new QGraphicsRectItem(m_progressBgRectItem); m_progressFillRectItem->setPen(Qt::NoPen); m_progressFillRectItem->setBrush(QColor(74,144,226)); m_progressFillRectItem->setZValue(12002.0); m_progressFillRectItem->setFlag(QGraphicsItem::ItemIgnoresTransformations, true); m_progressFillRectItem->setAcceptedMouseButtons(Qt::NoButton);

    m_progressTimer = new QTimer(); m_progressTimer->setInterval(33);
    QObject::connect(m_progressTimer, &QTimer::timeout, [this]() {
        if (m_player && m_player->playbackState() == QMediaPlayer::PlayingState && !m_draggingProgress && !m_holdLastFrameAtEnd && !m_seeking && m_durationMs > 0) {
            qint64 currentPos = m_player->position(); qreal newRatio = static_cast<qreal>(currentPos) / m_durationMs; m_smoothProgressRatio = std::clamp<qreal>(newRatio, 0.0, 1.0); updateProgressBar(); update();
        }
    });
    m_controlsLockedUntilReady = true; m_controlsDidInitialFade = false;
    auto hide = [&](QGraphicsItem* it){ if (it) it->setVisible(false); };
    hide(m_controlsBg); hide(m_playBtnRectItem); hide(m_playIcon); hide(m_pauseIcon); hide(m_stopBtnRectItem); hide(m_stopIcon); hide(m_repeatBtnRectItem); hide(m_repeatIcon); hide(m_muteBtnRectItem); hide(m_muteIcon); hide(m_muteSlashIcon); hide(m_volumeBgRectItem); hide(m_volumeFillRectItem); hide(m_progressBgRectItem); hide(m_progressFillRectItem);

    QObject::connect(m_player, &QMediaPlayer::mediaStatusChanged, m_player, [this](QMediaPlayer::MediaStatus s){
        if (s == QMediaPlayer::LoadedMedia || s == QMediaPlayer::BufferedMedia) {
            if (!m_adoptedSize) {
                const QMediaMetaData md = m_player->metaData();
                const QVariant v = md.value(QMediaMetaData::Resolution);
                const QSize sz = v.toSize(); if (!sz.isEmpty()) adoptBaseSize(sz);
                const QVariant thumbVar = md.value(QMediaMetaData::ThumbnailImage);
                if (!m_posterImageSet && thumbVar.isValid()) {
                    if (thumbVar.canConvert<QImage>()) { m_posterImage = thumbVar.value<QImage>(); m_posterImageSet = !m_posterImage.isNull(); }
                    else if (thumbVar.canConvert<QPixmap>()) { m_posterImage = thumbVar.value<QPixmap>().toImage(); m_posterImageSet = !m_posterImage.isNull(); }
                    if (!m_posterImageSet) {
                        const QVariant coverVar = md.value(QMediaMetaData::CoverArtImage);
                        if (coverVar.canConvert<QImage>()) { m_posterImage = coverVar.value<QImage>(); m_posterImageSet = !m_posterImage.isNull(); }
                        else if (coverVar.canConvert<QPixmap>()) { m_posterImage = coverVar.value<QPixmap>().toImage(); m_posterImageSet = !m_posterImage.isNull(); }
                    }
                    if (m_posterImageSet) update();
                }
            }
            if (!m_firstFramePrimed && !m_primingFirstFrame) {
                m_holdLastFrameAtEnd = false; m_savedMuted = m_audio ? m_audio->isMuted() : false; if (m_audio) m_audio->setMuted(true); m_primingFirstFrame = true; if (m_player) m_player->play();
            }
        }
        if (s == QMediaPlayer::EndOfMedia) {
            if (m_repeatEnabled) {
                if (m_progressTimer) m_progressTimer->stop();
                m_smoothProgressRatio = 0.0; updateProgressBar(); m_player->setPosition(0); m_player->play();
                QTimer::singleShot(10, [this]() { if (m_progressTimer && m_player && m_player->playbackState() == QMediaPlayer::PlayingState) m_progressTimer->start(); });
                m_expectedPlayingState = true;
                updatePlayPauseIconState(true);
            } else {
                m_holdLastFrameAtEnd = true; if (m_durationMs > 0) m_positionMs = m_durationMs; m_smoothProgressRatio = 1.0; updateProgressBar(); if (m_progressTimer) m_progressTimer->stop(); updateControlsLayout(); update(); if (m_player) m_player->pause();
                m_expectedPlayingState = false;
                updatePlayPauseIconState(false);
            }
        }
    });
    QObject::connect(m_player, &QMediaPlayer::durationChanged, m_player, [this](qint64 d){ m_durationMs = d; update(); });
    QObject::connect(m_player, &QMediaPlayer::positionChanged, m_player, [this](qint64 p){
        if (m_holdLastFrameAtEnd) {
            return;
        }

        const bool playing = (m_player && m_player->playbackState() == QMediaPlayer::PlayingState);
        if (m_repeatEnabled && playing && !m_seeking && !m_draggingProgress && m_durationMs > 0) {
            const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
            qint64 leadMargin = std::clamp<qint64>(m_durationMs / 48, 15LL, 120LL); // aim for ~20ms-120ms early seek
            if (leadMargin >= m_durationMs) {
                leadMargin = std::max<qint64>(m_durationMs / 4, 1LL);
                if (leadMargin >= m_durationMs) {
                    leadMargin = std::max<qint64>(m_durationMs - 1, 1LL);
                }
            }

            if (!m_seamlessLoopJumpPending && p >= (m_durationMs - leadMargin)) {
                m_seamlessLoopJumpPending = true;
                m_lastSeamlessLoopTriggerMs = nowMs;
                m_positionMs = 0;
                m_smoothProgressRatio = 0.0;
                updateProgressBar();
                if (m_progressTimer && !m_progressTimer->isActive()) {
                    m_progressTimer->start();
                }
                if (m_player) {
                    m_player->setPosition(0);
                    if (m_player->playbackState() != QMediaPlayer::PlayingState) {
                        m_player->play();
                    }
                }
                m_expectedPlayingState = true;
                updatePlayPauseIconState(true);
                updateControlsLayout();
                update();
                return;
            }

            if (m_seamlessLoopJumpPending) {
                const qint64 settleMargin = std::clamp<qint64>(leadMargin, 15LL, 200LL);
                if (p <= settleMargin || (nowMs - m_lastSeamlessLoopTriggerMs) > 500) {
                    m_seamlessLoopJumpPending = false;
                }
            }
        } else {
            m_seamlessLoopJumpPending = false;
        }

        m_positionMs = p;
    });
    
    // Connect to error signals to detect missing/corrupted source files
    QObject::connect(m_player, &QMediaPlayer::errorOccurred, m_player, [this](QMediaPlayer::Error error, const QString& errorString) {
        qDebug() << "ResizableVideoItem: Media player error occurred for" << sourcePath() 
                 << "- Error:" << error << "Message:" << errorString;
        
        // Check if the error indicates the file is missing or corrupted
        if (error == QMediaPlayer::ResourceError || error == QMediaPlayer::FormatError) {
            qDebug() << "ResizableVideoItem: File appears to be missing or corrupted, requesting removal";
            notifyFileError();
        }
    });
}

ResizableVideoItem::~ResizableVideoItem() {
    teardownPlayback();
    if (m_player) QObject::disconnect(m_player, nullptr, nullptr, nullptr);
    if (m_sink) QObject::disconnect(m_sink, nullptr, nullptr, nullptr);
    delete m_player; delete m_audio; delete m_sink; delete m_controlsFadeAnim;
    if (m_controlsBg && m_controlsBg->parentItem() == nullptr) delete m_controlsBg;
}

void ResizableVideoItem::togglePlayPause() {
    if (!m_player) return;
    m_seamlessLoopJumpPending = false;
    m_lastSeamlessLoopTriggerMs = 0;
    bool nowPlaying = false;
    if (m_player->playbackState() == QMediaPlayer::PlayingState) {
        m_player->pause();
        if (m_progressTimer) m_progressTimer->stop();
        nowPlaying = false;
    } else {
        if (m_holdLastFrameAtEnd) {
            m_holdLastFrameAtEnd = false;
            m_positionMs = 0;
            m_player->setPosition(0);
            m_smoothProgressRatio = 0.0;
            updateProgressBar();
        }
        m_player->play();
        if (m_progressTimer) m_progressTimer->start();
        nowPlaying = true;
    }
    m_expectedPlayingState = nowPlaying;
    updatePlayPauseIconState(nowPlaying);
    updateControlsLayout(); update();
}

void ResizableVideoItem::toggleRepeat() {
    m_repeatEnabled = !m_repeatEnabled;
    m_seamlessLoopJumpPending = false;
    m_lastSeamlessLoopTriggerMs = 0;
    updateControlsLayout();
    update();
}

void ResizableVideoItem::toggleMute() { if (!m_audio) return; m_audio->setMuted(!m_audio->isMuted()); bool muted = m_audio->isMuted(); updateControlsLayout(); update(); }

void ResizableVideoItem::stopToBeginning() {
    if (!m_player) return;
    m_seamlessLoopJumpPending = false;
    m_lastSeamlessLoopTriggerMs = 0;
    m_holdLastFrameAtEnd = false;
    m_player->pause(); m_player->setPosition(0);
    m_positionMs = 0; m_smoothProgressRatio = 0.0; updateProgressBar();
    if (m_progressTimer) m_progressTimer->stop();
    m_expectedPlayingState = false; updatePlayPauseIconState(false);
    updateControlsLayout(); update();
}

void ResizableVideoItem::seekToRatio(qreal r) {
    if (!m_player || m_durationMs <= 0) return;
    m_seamlessLoopJumpPending = false;
    m_lastSeamlessLoopTriggerMs = 0;
    r = std::clamp<qreal>(r, 0.0, 1.0);
    m_holdLastFrameAtEnd = false;
    m_seeking = true;
    if (m_progressTimer) m_progressTimer->stop();
    m_smoothProgressRatio = r;
    m_positionMs = static_cast<qint64>(r * m_durationMs);
    updateProgressBar(); updateControlsLayout(); update();
    const qint64 pos = m_positionMs; m_player->setPosition(pos);
    QTimer::singleShot(30, [this]() {
        m_seeking = false;
        if (m_progressTimer && m_player && m_player->playbackState() == QMediaPlayer::PlayingState) m_progressTimer->start();
    });
}

void ResizableVideoItem::pauseAndSetPosition(qint64 posMs) {
    if (!m_player) return;
    if (posMs < 0) posMs = 0;
    if (m_durationMs > 0 && posMs > m_durationMs) posMs = m_durationMs;
    m_seamlessLoopJumpPending = false;
    m_lastSeamlessLoopTriggerMs = 0;
    m_holdLastFrameAtEnd = false;
    m_player->pause();
    if (m_progressTimer) m_progressTimer->stop();
    m_player->setPosition(posMs);
    m_positionMs = posMs;
    m_smoothProgressRatio = (m_durationMs > 0 ? double(posMs)/double(m_durationMs) : 0.0);
    updateProgressBar();
    m_expectedPlayingState = false; updatePlayPauseIconState(false);
    updateControlsLayout(); update();
}

void ResizableVideoItem::setExternalPosterImage(const QImage& img) { if (!img.isNull()) { m_posterImage = img; m_posterImageSet = true; if (!m_adoptedSize) adoptBaseSize(img.size()); update(); } }

void ResizableVideoItem::updateDragWithScenePos(const QPointF& scenePos) {
    QPointF p = mapFromScene(scenePos);
    if (m_draggingProgress) { qreal r = (p.x() - m_progRectItemCoords.left()) / m_progRectItemCoords.width(); r = std::clamp<qreal>(r, 0.0, 1.0); m_holdLastFrameAtEnd = false; seekToRatio(r); if (m_durationMs > 0) m_positionMs = static_cast<qint64>(r * m_durationMs); updateControlsLayout(); update(); }
    else if (m_draggingVolume) { qreal r = (p.x() - m_volumeRectItemCoords.left()) / m_volumeRectItemCoords.width(); r = std::clamp<qreal>(r, 0.0, 1.0); if (m_audio) m_audio->setVolume(r); updateControlsLayout(); update(); }
}

void ResizableVideoItem::endDrag() { if (m_draggingProgress || m_draggingVolume) { m_draggingProgress = false; m_draggingVolume = false; ungrabMouse(); updateControlsLayout(); update(); } }

void ResizableVideoItem::setApplicationSuspended(bool suspended) {
    if (m_appSuspended == suspended) return;
    m_appSuspended = suspended;
    m_seamlessLoopJumpPending = false;
    m_lastSeamlessLoopTriggerMs = 0;
    if (m_appSuspended) {
        m_wasPlayingBeforeSuspend = m_player && m_player->playbackState() == QMediaPlayer::PlayingState;
        m_resumePositionMs = m_player ? m_player->position() : m_positionMs;
        m_needsReprimeAfterResume = !m_firstFramePrimed;
        if (m_player) {
            if (m_wasPlayingBeforeSuspend) {
                m_player->pause();
            }
            if (!m_sinkDetached) {
                m_player->setVideoSink(nullptr);
                m_sinkDetached = true;
            }
        }
        if (m_progressTimer) {
            m_progressTimer->stop();
        }
    } else {
        if (m_player) {
            if (m_sinkDetached) {
                m_player->setVideoSink(m_sink);
                m_sinkDetached = false;
            }
            if (m_needsReprimeAfterResume) {
                restartPrimingSequence();
            } else {
                if (m_resumePositionMs > 0) {
                    m_player->setPosition(m_resumePositionMs);
                    m_positionMs = m_resumePositionMs;
                    m_smoothProgressRatio = (m_durationMs > 0) ? std::clamp<qreal>(qreal(m_positionMs) / qreal(m_durationMs), 0.0, 1.0) : 0.0;
                    updateProgressBar();
                }
                if (m_wasPlayingBeforeSuspend) {
                    m_player->play();
                }
            }
        }
        if (m_progressTimer && m_player && m_player->playbackState() == QMediaPlayer::PlayingState) {
            m_progressTimer->start();
        }
        m_wasPlayingBeforeSuspend = false;
        m_needsReprimeAfterResume = false;
        m_resumePositionMs = 0;
        update();
    }
}

void ResizableVideoItem::getFrameStats(int& received, int& processed, int& skipped) const { received = m_framesReceived; processed = m_framesProcessed; skipped = m_framesSkipped; }

void ResizableVideoItem::getFrameStatsExtended(int& received, int& processed, int& skipped, int& dropped, int& conversionFailures) const {
    received = m_framesReceived;
    processed = m_framesProcessed;
    skipped = m_framesSkipped;
    dropped = m_framesDropped;
    conversionFailures = m_framesDropped;
}

void ResizableVideoItem::resetFrameStats() {
    m_framesReceived = 0;
    m_framesProcessed = 0;
    m_framesSkipped = 0;
    m_framesDropped = 0;
    m_conversionFailures = 0;
}

bool ResizableVideoItem::handleControlsPressAtItemPos(const QPointF& itemPos) {
    if (!isSelected() || m_controlsLockedUntilReady) return false;
    if (m_playBtnRectItemCoords.contains(itemPos)) { m_holdLastFrameAtEnd = false; togglePlayPause(); return true; }
    if (m_stopBtnRectItemCoords.contains(itemPos)) { m_holdLastFrameAtEnd = false; stopToBeginning(); return true; }
    if (m_repeatBtnRectItemCoords.contains(itemPos)) { toggleRepeat(); return true; }
    if (m_muteBtnRectItemCoords.contains(itemPos)) { toggleMute(); return true; }
    if (m_progRectItemCoords.contains(itemPos)) { qreal r = (itemPos.x() - m_progRectItemCoords.left()) / m_progRectItemCoords.width(); m_holdLastFrameAtEnd = false; seekToRatio(r); m_draggingProgress = true; grabMouse(); return true; }
    if (m_volumeRectItemCoords.contains(itemPos)) { qreal r = (itemPos.x() - m_volumeRectItemCoords.left()) / m_volumeRectItemCoords.width(); r = std::clamp<qreal>(r, 0.0, 1.0); if (m_audio) m_audio->setVolume(r); updateControlsLayout(); m_draggingVolume = true; grabMouse(); return true; }
    return false;
}

void ResizableVideoItem::paint(QPainter* painter, const QStyleOptionGraphicsItem* option, QWidget* widget) {
    Q_UNUSED(option); Q_UNUSED(widget);
    QRectF br(0,0, baseWidth(), baseHeight());
    auto fitRect = [&](const QRectF& bounds, const QSize& imgSz) -> QRectF {
        if (m_fillContentWithoutAspect) {
            // Non-uniform stretch requested: fill full bounds ignoring aspect
            return bounds;
        }
        if (bounds.isEmpty() || imgSz.isEmpty()) return bounds; qreal brW = bounds.width(); qreal brH = bounds.height(); qreal imgW = imgSz.width(); qreal imgH = imgSz.height(); if (imgW <= 0 || imgH <= 0) return bounds; qreal brAR = brW / brH; qreal imgAR = imgW / imgH; if (imgAR > brAR) { qreal h = brW / imgAR; return QRectF(bounds.left(), bounds.top() + (brH - h)/2.0, brW, h);} else { qreal w = brH * imgAR; return QRectF(bounds.left() + (brW - w)/2.0, bounds.top(), w, brH);} };
    if (isContentVisible() || m_contentDisplayOpacity > 0.0) {
        qreal effective = contentOpacity() * m_contentDisplayOpacity;
        auto drawImg = [&](const QImage& img){ if (img.isNull() || effective <= 0.0) return; QRectF dst = fitRect(br, img.size()); if (effective >= 0.999) { painter->drawImage(dst, img); } else { painter->save(); painter->setOpacity(effective); painter->drawImage(dst, img); painter->restore(); } };
    if (!m_lastFrameImage.isNull()) { drawImg(m_lastFrameImage); }
    else if (m_posterImageSet && !m_posterImage.isNull()) { drawImg(m_posterImage); }
    }
    paintSelectionAndLabel(painter);
}

QRectF ResizableVideoItem::boundingRect() const {
    QRectF br(0,0, baseWidth(), baseHeight());
    if (isSelected() && !m_controlsLockedUntilReady) {
        const int overrideH = ResizableMediaBase::getHeightOfMediaOverlaysPx();
        const int fallbackH = m_overlayStyle.defaultHeight > 0 ? m_overlayStyle.defaultHeight : 24;
        const int rowH = (overrideH > 0) ? overrideH : fallbackH; const int gapPx = 8;
        qreal extra = toItemLengthFromPixels(rowH * 2 + 2 * gapPx); br.setHeight(br.height() + extra);
    }
   

    
    if (isSelected()) { qreal pad = toItemLengthFromPixels(m_selectionSize) / 2.0; br = br.adjusted(-pad, -pad, pad, pad); }
    return br;
}

QPainterPath ResizableVideoItem::shape() const {
    QPainterPath p; p.addRect(QRectF(0,0, baseWidth(), baseHeight()));
    if (isSelected() && !m_controlsLockedUntilReady) {
        if (!m_playBtnRectItemCoords.isNull()) p.addRect(m_playBtnRectItemCoords);
        if (!m_stopBtnRectItemCoords.isNull()) p.addRect(m_stopBtnRectItemCoords);
        if (!m_repeatBtnRectItemCoords.isNull()) p.addRect(m_repeatBtnRectItemCoords);
        if (!m_muteBtnRectItemCoords.isNull()) p.addRect(m_muteBtnRectItemCoords);
        if (!m_progRectItemCoords.isNull()) p.addRect(m_progRectItemCoords);
        if (!m_volumeRectItemCoords.isNull()) p.addRect(m_volumeRectItemCoords);
    }
    if (isSelected()) { const qreal s = toItemLengthFromPixels(m_selectionSize); QRectF br(0,0, baseWidth(), baseHeight()); p.addRect(QRectF(br.topLeft() - QPointF(s/2,s/2), QSizeF(s,s))); p.addRect(QRectF(QPointF(br.right(), br.top()) - QPointF(s/2,s/2), QSizeF(s,s))); p.addRect(QRectF(QPointF(br.left(), br.bottom()) - QPointF(s/2,s/2), QSizeF(s,s))); p.addRect(QRectF(br.bottomRight() - QPointF(s/2,s/2), QSizeF(s,s))); }
    return p;
}

void ResizableVideoItem::mousePressEvent(QGraphicsSceneMouseEvent* event) {
    m_activeHandle = hitTestHandle(event->pos());
    if (m_activeHandle != None) {
        m_fixedItemPoint = handlePoint(opposite(m_activeHandle)); m_fixedScenePoint = mapToScene(m_fixedItemPoint); m_initialScale = scale(); const qreal d = std::hypot(event->scenePos().x() - m_fixedScenePoint.x(), event->scenePos().y() - m_fixedScenePoint.y()); m_initialGrabDist = (d > 1e-6) ? d : 1e-6; event->accept(); return; }
    if (!m_controlsLockedUntilReady && isSelected() && m_playBtnRectItemCoords.contains(event->pos())) { togglePlayPause(); event->accept(); return; }
    if (!m_controlsLockedUntilReady && isSelected() && m_stopBtnRectItemCoords.contains(event->pos())) { stopToBeginning(); event->accept(); return; }
    if (!m_controlsLockedUntilReady && isSelected() && m_repeatBtnRectItemCoords.contains(event->pos())) { toggleRepeat(); event->accept(); return; }
    if (!m_controlsLockedUntilReady && isSelected() && m_muteBtnRectItemCoords.contains(event->pos())) { toggleMute(); event->accept(); return; }
    if (!m_controlsLockedUntilReady && isSelected() && m_progRectItemCoords.contains(event->pos())) { qreal r = (event->pos().x() - m_progRectItemCoords.left()) / m_progRectItemCoords.width(); m_seeking = true; if (m_progressTimer) m_progressTimer->stop(); seekToRatio(r); m_draggingProgress = true; grabMouse(); event->accept(); return; }
    if (!m_controlsLockedUntilReady && isSelected() && m_volumeRectItemCoords.contains(event->pos())) { qreal r = (event->pos().x() - m_volumeRectItemCoords.left()) / m_volumeRectItemCoords.width(); r = std::clamp<qreal>(r, 0.0, 1.0); if (m_audio) m_audio->setVolume(r); updateControlsLayout(); m_draggingVolume = true; grabMouse(); event->accept(); return; }
    ResizableMediaBase::mousePressEvent(event);
}

void ResizableVideoItem::mouseDoubleClickEvent(QGraphicsSceneMouseEvent* event) {
    if (isSelected() && !m_controlsLockedUntilReady) {
        if (m_playBtnRectItemCoords.contains(event->pos())) { togglePlayPause(); event->accept(); return; }
        if (m_stopBtnRectItemCoords.contains(event->pos())) { stopToBeginning(); event->accept(); return; }
        if (m_repeatBtnRectItemCoords.contains(event->pos())) { toggleRepeat(); event->accept(); return; }
        if (m_muteBtnRectItemCoords.contains(event->pos())) { toggleMute(); event->accept(); return; }
        if (m_progRectItemCoords.contains(event->pos())) { qreal r = (event->pos().x() - m_progRectItemCoords.left()) / m_progRectItemCoords.width(); seekToRatio(r); event->accept(); return; }
        if (m_volumeRectItemCoords.contains(event->pos())) { qreal r = (event->pos().x() - m_volumeRectItemCoords.left()) / m_volumeRectItemCoords.width(); r = std::clamp<qreal>(r, 0.0, 1.0); if (m_audio) m_audio->setVolume(r); event->accept(); return; }
    }
    ResizableMediaBase::mouseDoubleClickEvent(event);
}

QVariant ResizableVideoItem::itemChange(GraphicsItemChange change, const QVariant &value) {
    if (change == ItemSceneChange) { if (value.value<QGraphicsScene*>() == nullptr) { if (m_controlsBg) m_controlsBg->setVisible(false); } }
    if (change == ItemSceneHasChanged) { if (scene() && m_controlsBg && m_controlsBg->scene() != scene()) { if (m_controlsBg->scene()) m_controlsBg->scene()->removeItem(m_controlsBg); scene()->addItem(m_controlsBg); } }
    if (change == ItemSelectedChange) { const bool willBeSelected = value.toBool(); prepareGeometryChange(); setControlsVisible(willBeSelected); }
    if (change == ItemSelectedHasChanged) { updateControlsLayout(); }
    if (change == ItemPositionHasChanged || change == ItemTransformHasChanged) { updateControlsLayout(); }
    return ResizableMediaBase::itemChange(change, value);
}

void ResizableVideoItem::onInteractiveGeometryChanged() { updateControlsLayout(); updateOverlayLayout(); update(); }

void ResizableVideoItem::mouseMoveEvent(QGraphicsSceneMouseEvent* event) {
    if (m_activeHandle != None) { ResizableMediaBase::mouseMoveEvent(event); return; }
    if (event->buttons() & Qt::LeftButton) {
        if (m_draggingProgress) { qreal r = (event->pos().x() - m_progRectItemCoords.left()) / m_progRectItemCoords.width(); r = std::clamp<qreal>(r, 0.0, 1.0); seekToRatio(r); updateControlsLayout(); update(); event->accept(); return; }
        if (m_draggingVolume) { qreal r = (event->pos().x() - m_volumeRectItemCoords.left()) / m_volumeRectItemCoords.width(); r = std::clamp<qreal>(r, 0.0, 1.0); if (m_audio) m_audio->setVolume(r); updateControlsLayout(); update(); event->accept(); return; }
    }
    ResizableMediaBase::mouseMoveEvent(event);
}

void ResizableVideoItem::mouseReleaseEvent(QGraphicsSceneMouseEvent* event) {
    if (m_draggingProgress || m_draggingVolume) { m_draggingProgress = false; m_draggingVolume = false; ungrabMouse(); QTimer::singleShot(30, [this]() { m_seeking = false; if (m_progressTimer && m_player && m_player->playbackState() == QMediaPlayer::PlayingState) m_progressTimer->start(); }); event->accept(); return; }
    ResizableMediaBase::mouseReleaseEvent(event);
}

void ResizableVideoItem::prepareForDeletion() {
    ResizableMediaBase::prepareForDeletion();
    // Stop timers / animations immediately.
    if (m_progressTimer) m_progressTimer->stop();
    if (m_controlsFadeAnim) m_controlsFadeAnim->stop();
    m_draggingProgress = m_draggingVolume = false; m_seeking = false;
    teardownPlayback();
    // Hide controls background & children (safer than deleting here; destructor will clean up if needed).
    if (m_controlsBg) {
        if (m_controlsBg->scene()) m_controlsBg->scene()->removeItem(m_controlsBg);
        m_controlsBg->setVisible(false);
    }
}

void ResizableVideoItem::maybeAdoptFrameSize(const QVideoFrame& f) {
    if (m_adoptedSize) return;
    if (!f.isValid()) return;
    const int w = f.width();
    const int h = f.height();
    if (w <= 0 || h <= 0) return;
    adoptBaseSize(QSize(w, h));
}

void ResizableVideoItem::adoptBaseSize(const QSize& sz) { if (m_adoptedSize) return; if (sz.isEmpty()) return; m_adoptedSize = true; const QRectF oldRect(0,0, baseWidth(), baseHeight()); const QPointF oldCenterScene = mapToScene(oldRect.center()); prepareGeometryChange(); m_baseSize = sz; setScale(m_initialScaleFactor); const QPointF newTopLeftScene = oldCenterScene - QPointF(sz.width()*m_initialScaleFactor/2.0, sz.height()*m_initialScaleFactor/2.0); setPos(newTopLeftScene); update(); }

void ResizableVideoItem::setControlsVisible(bool show) {
     if (!m_controlsBg) return; bool allow = show && !m_controlsLockedUntilReady;
    auto setChildrenVisible = [&](bool vis){ if (m_playBtnRectItem) m_playBtnRectItem->setVisible(vis); if (m_stopBtnRectItem) m_stopBtnRectItem->setVisible(vis); if (m_stopIcon) m_stopIcon->setVisible(vis); if (m_repeatBtnRectItem) m_repeatBtnRectItem->setVisible(vis); if (m_repeatIcon) m_repeatIcon->setVisible(vis); if (m_muteBtnRectItem) m_muteBtnRectItem->setVisible(vis); if (m_muteIcon) m_muteIcon->setVisible(vis); if (m_muteSlashIcon) m_muteSlashIcon->setVisible(vis && m_audio && m_audio->isMuted()); if (m_volumeBgRectItem) m_volumeBgRectItem->setVisible(vis); if (m_volumeFillRectItem) m_volumeFillRectItem->setVisible(vis); if (m_progressBgRectItem) m_progressBgRectItem->setVisible(vis); if (m_progressFillRectItem) m_progressFillRectItem->setVisible(vis); };
     if (allow) { m_controlsBg->setVisible(true); m_controlsBg->setZValue(12000.0); setChildrenVisible(true); if (!m_controlsDidInitialFade) { if (!m_controlsFadeAnim) { m_controlsFadeAnim = new QVariantAnimation(); m_controlsFadeAnim->setEasingCurve(QEasingCurve::OutCubic); QObject::connect(m_controlsFadeAnim, &QVariantAnimation::valueChanged, [this](const QVariant& v){ if (m_controlsBg) m_controlsBg->setOpacity(v.toReal()); }); } m_controlsFadeAnim->stop(); m_controlsFadeAnim->setDuration(m_controlsFadeMs); m_controlsFadeAnim->setStartValue(0.0); m_controlsFadeAnim->setEndValue(1.0); m_controlsDidInitialFade = true; m_controlsFadeAnim->start(); } else { if (m_controlsFadeAnim) m_controlsFadeAnim->stop(); m_controlsBg->setOpacity(1.0); } }
    else { if (m_controlsFadeAnim) m_controlsFadeAnim->stop(); m_controlsBg->setOpacity(0.0); m_controlsBg->setVisible(false); setChildrenVisible(false); }
    if (allow) {
        updatePlayPauseIconState(isEffectivelyPlayingForControls() || m_expectedPlayingState);
    } else {
        updatePlayPauseIconState(false);
    }
 }

void ResizableVideoItem::updateControlsLayout() {
    if (!scene() || scene()->views().isEmpty()) return; if (!isSelected()) return; if (m_controlsLockedUntilReady) return; QGraphicsView* v = scene()->views().first();
    const int padYpx = 8; const int gapPx = 8; const int overrideH = ResizableMediaBase::getHeightOfMediaOverlaysPx(); const int fallbackH = m_overlayStyle.defaultHeight > 0 ? m_overlayStyle.defaultHeight : 36; const int rowHpx = (overrideH > 0) ? overrideH : fallbackH; const int totalWpx = 320; const int playWpx = rowHpx; const int stopWpx = rowHpx; const int repeatWpx = rowHpx; const int muteWpx = rowHpx; const int buttonGapPx = gapPx; const int volumeWpx = std::max(0, totalWpx - (playWpx + stopWpx + repeatWpx + muteWpx) - buttonGapPx * 4); const int progWpx = totalWpx;
    auto baseBrush = m_overlayStyle.backgroundBrush();
    QBrush activeBrush(AppColors::gOverlayActiveBackgroundColor);
    QPointF bottomCenterItem(baseWidth()/2.0, baseHeight()); QPointF bottomCenterScene = mapToScene(bottomCenterItem); QPointF bottomCenterView = v->viewportTransform().map(bottomCenterScene); QPointF ctrlTopLeftView = bottomCenterView + QPointF(-totalWpx/2.0, gapPx); QPointF ctrlTopLeftScene = v->viewportTransform().inverted().map(ctrlTopLeftView); QPointF ctrlTopLeftItem = mapFromScene(ctrlTopLeftScene);
    if (m_controlsBg) { m_controlsBg->setRect(0,0,totalWpx, rowHpx*2 + gapPx); m_controlsBg->setPos(ctrlTopLeftScene); }
    const qreal x0=0; const qreal x1=x0+playWpx+buttonGapPx; const qreal x2=x1+stopWpx+buttonGapPx; const qreal x3=x2+repeatWpx+buttonGapPx; const qreal x4=x3+muteWpx+buttonGapPx; auto appliedCornerRadius = m_overlayStyle.cornerRadius;
    if (m_playBtnRectItem) { m_playBtnRectItem->setRect(0,0,playWpx,rowHpx); m_playBtnRectItem->setPos(x0,0); m_playBtnRectItem->setRadius(appliedCornerRadius); m_playBtnRectItem->setBrush(baseBrush);} if (m_stopBtnRectItem) { m_stopBtnRectItem->setRect(0,0,stopWpx,rowHpx); m_stopBtnRectItem->setPos(x1,0); m_stopBtnRectItem->setRadius(appliedCornerRadius); m_stopBtnRectItem->setBrush(baseBrush);} if (m_repeatBtnRectItem) { m_repeatBtnRectItem->setRect(0,0,repeatWpx,rowHpx); m_repeatBtnRectItem->setPos(x2,0); m_repeatBtnRectItem->setRadius(appliedCornerRadius); m_repeatBtnRectItem->setBrush(m_repeatEnabled ? activeBrush : baseBrush);} if (m_muteBtnRectItem) { m_muteBtnRectItem->setRect(0,0,muteWpx,rowHpx); m_muteBtnRectItem->setPos(x3,0); m_muteBtnRectItem->setRadius(appliedCornerRadius); bool muted = m_audio && m_audio->isMuted(); m_muteBtnRectItem->setBrush(muted ? activeBrush : baseBrush);} if (m_volumeBgRectItem) { m_volumeBgRectItem->setRect(0,0,volumeWpx,rowHpx); m_volumeBgRectItem->setPos(x4,0);} if (m_volumeFillRectItem) { const qreal margin=1.0; qreal vol = m_audio ? std::clamp<qreal>(m_audio->volume(),0.0,1.0):0.0; const qreal innerW = std::max<qreal>(0.0, volumeWpx - 2*margin); m_volumeFillRectItem->setRect(margin,margin, innerW*vol, rowHpx - 2*margin);} if (m_progressBgRectItem) { m_progressBgRectItem->setRect(0,0,progWpx,rowHpx); m_progressBgRectItem->setPos(0,rowHpx+gapPx);} if (m_progressFillRectItem) { if (!m_draggingProgress) { updateProgressBar(); } else { qreal ratio = (m_durationMs>0) ? (qreal(m_positionMs)/m_durationMs) : 0.0; ratio = std::clamp<qreal>(ratio,0.0,1.0); const qreal margin=1.0; m_progressFillRectItem->setRect(margin,margin,(progWpx-2*margin)*ratio,rowHpx-2*margin);} }
    auto placeSvg = [](QGraphicsSvgItem* svg, qreal targetW, qreal targetH){ if (!svg) return; QSizeF nat = svg->renderer() ? svg->renderer()->defaultSize() : QSizeF(24,24); if (nat.width()<=0||nat.height()<=0) nat = svg->boundingRect().size(); if (nat.width()<=0||nat.height()<=0) nat = QSizeF(24,24); qreal scale = std::min(targetW/nat.width(), targetH/nat.height())*0.6; svg->setScale(scale); qreal x = (targetW - nat.width()*scale)/2.0; qreal y=(targetH - nat.height()*scale)/2.0; svg->setPos(x,y); };
    bool isPlaying = isEffectivelyPlayingForControls();
    if (!isPlaying && m_expectedPlayingState) {
        isPlaying = true;
    }
    updatePlayPauseIconState(isPlaying);
    if (m_playIcon) placeSvg(m_playIcon, playWpx, rowHpx);
    if (m_pauseIcon) placeSvg(m_pauseIcon, playWpx, rowHpx);
    placeSvg(m_stopIcon, stopWpx, rowHpx);
    placeSvg(m_repeatIcon, repeatWpx, rowHpx);
    bool muted = m_audio && m_audio->isMuted(); if (m_muteIcon && m_muteSlashIcon) { m_muteIcon->setVisible(!muted); m_muteSlashIcon->setVisible(muted); placeSvg(m_muteIcon, muteWpx, rowHpx); placeSvg(m_muteSlashIcon, muteWpx, rowHpx);} const qreal rowHItem = toItemLengthFromPixels(rowHpx); const qreal playWItem = toItemLengthFromPixels(playWpx); const qreal stopWItem = toItemLengthFromPixels(stopWpx); const qreal repeatWItem = toItemLengthFromPixels(repeatWpx); const qreal muteWItem = toItemLengthFromPixels(muteWpx); const qreal volumeWItem = toItemLengthFromPixels(volumeWpx); const qreal progWItem = toItemLengthFromPixels(progWpx); const qreal gapItem = toItemLengthFromPixels(gapPx); const qreal x0Item = toItemLengthFromPixels(int(std::round(x0))); const qreal x1Item = toItemLengthFromPixels(int(std::round(x1))); const qreal x2Item = toItemLengthFromPixels(int(std::round(x2))); const qreal x3Item = toItemLengthFromPixels(int(std::round(x3))); const qreal x4Item = toItemLengthFromPixels(int(std::round(x4))); m_playBtnRectItemCoords = QRectF(ctrlTopLeftItem.x()+x0Item, ctrlTopLeftItem.y()+0, playWItem,rowHItem); m_stopBtnRectItemCoords = QRectF(ctrlTopLeftItem.x()+x1Item, ctrlTopLeftItem.y()+0, stopWItem,rowHItem); m_repeatBtnRectItemCoords = QRectF(ctrlTopLeftItem.x()+x2Item, ctrlTopLeftItem.y()+0, repeatWItem,rowHItem); m_muteBtnRectItemCoords = QRectF(ctrlTopLeftItem.x()+x3Item, ctrlTopLeftItem.y()+0, muteWItem,rowHItem); m_volumeRectItemCoords = QRectF(ctrlTopLeftItem.x()+x4Item, ctrlTopLeftItem.y()+0, volumeWItem,rowHItem); m_progRectItemCoords = QRectF(ctrlTopLeftItem.x()+0, ctrlTopLeftItem.y()+rowHItem+gapItem, progWItem,rowHItem);
}

bool ResizableVideoItem::isVisibleInAnyView() const { if (!scene() || scene()->views().isEmpty()) return false; auto *view = scene()->views().first(); if (!view || !view->viewport()) return false; QRectF viewportRect = view->viewport()->rect(); QRectF sceneRect = view->mapToScene(viewportRect.toRect()).boundingRect(); QRectF itemSceneRect = mapToScene(boundingRect()).boundingRect(); return sceneRect.intersects(itemSceneRect); }
bool ResizableVideoItem::shouldRepaint() const { const qint64 now = QDateTime::currentMSecsSinceEpoch(); return (now - m_lastRepaintMs) >= m_repaintBudgetMs; }
void ResizableVideoItem::logFrameStats() const {
    if (m_framesReceived > 0 && m_framesReceived % 120 == 0) {
        const float processRatio = float(m_framesProcessed) / float(m_framesReceived);
        const float skipRatio = float(m_framesSkipped) / float(m_framesReceived);
        const float dropRatio = float(m_framesDropped) / float(m_framesReceived);
        const float failureRatio = float(m_conversionFailures) / float(m_framesReceived);
        qDebug() << "VideoItem frame stats: received=" << m_framesReceived
                 << "processed=" << m_framesProcessed << "(" << (processRatio * 100.0f) << "% )"
                 << "skipped=" << m_framesSkipped << "(" << (skipRatio * 100.0f) << "% )"
                 << "dropped=" << m_framesDropped << "(" << (dropRatio * 100.0f) << "% )"
                 << "conversionFailures=" << m_framesDropped << "(" << (failureRatio * 100.0f) << "% )";
    }
}

void ResizableVideoItem::updatePlayPauseIconState(bool playing) {
    if (!m_playIcon || !m_pauseIcon) {
        return;
    }
    const bool buttonVisible = m_playBtnRectItem && m_playBtnRectItem->isVisible();
    m_playIcon->setVisible(buttonVisible && !playing);
    m_pauseIcon->setVisible(buttonVisible && playing);
}

bool ResizableVideoItem::isEffectivelyPlayingForControls() const {
    if (m_progressTimer && m_progressTimer->isActive()) {
        return true;
    }
    if (!m_player) {
        return false;
    }
    if (m_holdLastFrameAtEnd) {
        return false;
    }
    const bool playerPlaying = (m_player->playbackState() == QMediaPlayer::PlayingState);
    if (!m_repeatEnabled && m_durationMs > 0 && (m_positionMs + 30 >= m_durationMs)) {
        if (!playerPlaying && !(m_progressTimer && m_progressTimer->isActive())) {
            return false;
        }
    }
    return playerPlaying;
}

void ResizableVideoItem::updateProgressBar() { if (!m_progressFillRectItem) return; const qreal margin = 1.0; const QRectF bgRect = m_progressBgRectItem ? m_progressBgRectItem->rect() : QRectF(); const qreal progWpx = bgRect.width(); const qreal rowH = bgRect.height(); m_progressFillRectItem->setRect(margin, margin, (progWpx - 2*margin) * m_smoothProgressRatio, rowH - 2*margin); }

QImage ResizableVideoItem::convertFrameToImage(const QVideoFrame& frame) const {
    if (!frame.isValid()) {
        return {};
    }

    QImage image = frame.toImage();
    if (!image.isNull()) {
        if (image.format() != QImage::Format_RGBA8888 && image.format() != QImage::Format_ARGB32_Premultiplied) {
            image = image.convertToFormat(QImage::Format_RGBA8888);
        }
        return image;
    }

    QVideoFrame copy(frame);
    if (!copy.isValid()) {
        return {};
    }

    if (!copy.map(QVideoFrame::ReadOnly)) {
        return {};
    }

    QImage mapped;
    const QVideoFrameFormat format = copy.surfaceFormat();
    const int width = format.frameWidth();
    const int height = format.frameHeight();
    const int stride = copy.bytesPerLine(0);
    const QImage::Format imgFormat = QVideoFrameFormat::imageFormatFromPixelFormat(format.pixelFormat());
    if (imgFormat != QImage::Format_Invalid && width > 0 && height > 0 && stride > 0) {
        mapped = QImage(copy.bits(0), width, height, stride, imgFormat).copy();
    }

    copy.unmap();

    if (!mapped.isNull() && mapped.format() != QImage::Format_RGBA8888 && mapped.format() != QImage::Format_ARGB32_Premultiplied) {
        mapped = mapped.convertToFormat(QImage::Format_RGBA8888);
    }

    return mapped;
}

void ResizableVideoItem::restartPrimingSequence() {
    if (!m_player) {
        return;
    }

    m_firstFramePrimed = false;
    m_primingFirstFrame = false;
    m_holdLastFrameAtEnd = false;
    m_lastFrameImage = QImage();
    m_smoothProgressRatio = 0.0;
    m_positionMs = 0;
    if (m_progressTimer) {
        m_progressTimer->stop();
    }
    m_controlsLockedUntilReady = true;
    m_controlsDidInitialFade = false;
    m_savedMuted = m_audio ? m_audio->isMuted() : false;
    if (m_audio) {
        m_audio->setMuted(true);
    }
    m_primingFirstFrame = true;
    m_player->setPosition(0);
    m_player->play();
}

void ResizableVideoItem::teardownPlayback() {
    if (m_playbackTornDown) {
        return;
    }
    m_playbackTornDown = true;

    if (m_progressTimer) {
        m_progressTimer->stop();
    }

    if (m_player) {
        // Stop playback synchronously and detach sinks/audio so Media Foundation tears down immediately
        m_player->pause();
        m_player->stop();
        m_player->setSource(QUrl());
        if (m_audio) {
            m_player->setAudioOutput(nullptr);
        }
        if (!m_sinkDetached) {
            m_player->setVideoSink(nullptr);
            m_sinkDetached = true;
        }
    }

    if (m_sink) {
        QObject::disconnect(m_sink, nullptr, nullptr, nullptr);
        m_sink->setVideoFrame(QVideoFrame());
    }

    if (m_audio) {
        QObject::disconnect(m_audio, nullptr, nullptr, nullptr);
        m_audio->setMuted(true);
    }
}
