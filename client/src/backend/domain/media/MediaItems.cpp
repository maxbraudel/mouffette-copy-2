// MediaItems.cpp - Implementation of media item hierarchy extracted from MainWindow.cpp

#include "backend/domain/media/MediaItems.h"
#include "frontend/rendering/canvas/ScreenCanvas.h"
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
#include "frontend/ui/overlays/canvas/CanvasMediaSettingsPanel.h"
#include <cmath>
#include <QVariantAnimation>
#include <QEasingCurve>
#include <QtSvgWidgets/QGraphicsSvgItem>
#include <QtSvg/QSvgRenderer>
#include <QFileInfo>
#include <QMediaPlayer>
#include <QAudioOutput>
#include <QVideoSink>
#include <QMediaMetaData>
#include <QApplication>
#include <QUrl>
#include "frontend/ui/theme/AppColors.h"
#include <QDateTime>

// ---------------- ResizableMediaBase -----------------

int ResizableMediaBase::heightOfMediaOverlays = -1; // default auto
int ResizableMediaBase::cornerRadiusOfMediaOverlays = 6;

double ResizableMediaBase::s_sceneGridUnit = 1.0; // default: 1 scene unit == 1 pixel

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

ResizableMediaBase::~ResizableMediaBase() {
    if (m_lifetimeToken) {
        *m_lifetimeToken = false;
        m_lifetimeToken.reset();
    }
    // Clean up FileManager associations
    if (!m_mediaId.isEmpty() && MediaRuntimeHooks::fileManager()) {
        qDebug() << "MediaItems: Destructing media" << m_mediaId << "with fileId" << m_fileId;
        MediaRuntimeHooks::fileManager()->removeMediaAssociation(m_mediaId);
    }
}

void ResizableMediaBase::notifyUploadChanged() {
    auto notifier = MediaRuntimeHooks::uploadChangedNotifier();
    if (notifier) {
        notifier();
    }
}

void ResizableMediaBase::notifyFileError() {
    auto notifier = MediaRuntimeHooks::fileErrorNotifier();
    if (notifier) {
        notifier(this);
    }
}

void ResizableMediaBase::setBaseSizePx(const QSize& size) {
    if (size == m_baseSize || size.width() <= 0 || size.height() <= 0)
        return;
    prepareGeometryChange();
    m_baseSize = size;
    update();
}

bool ResizableMediaBase::beginAltResizeMode() {
    return onAltResizeModeEngaged();
}

void ResizableMediaBase::notifyInteractiveGeometryChanged() {
    onInteractiveGeometryChanged();
}

void ResizableMediaBase::suppressNextItemPositionSnap() {
    m_suppressNextItemPositionSnap = true;
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

bool ResizableMediaBase::allowAltResize() const {
    return true;
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
    m_fadeAnimation->setEasingCurve(QEasingCurve::Linear);
    QObject::connect(m_fadeAnimation, &QVariantAnimation::valueChanged, [this](const QVariant& v){
        m_contentDisplayOpacity = v.toDouble();
        if (auto tick = MediaRuntimeHooks::mediaOpacityAnimationTickNotifier()) { tick(); }
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
    m_fadeAnimation->setEasingCurve(QEasingCurve::Linear);
    QObject::connect(m_fadeAnimation, &QVariantAnimation::valueChanged, [this](const QVariant& v){
        m_contentDisplayOpacity = v.toDouble();
        if (auto tick = MediaRuntimeHooks::mediaOpacityAnimationTickNotifier()) { tick(); }
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
    onMediaSettingsChanged();
}

void ResizableMediaBase::onMediaSettingsChanged() {
    if (auto notifier = MediaRuntimeHooks::mediaSettingsChangedNotifier()) {
        notifier(this);
    }
}

bool ResizableMediaBase::autoDisplayEnabled() const {
    return m_mediaSettings.displayAutomatically;
}

int ResizableMediaBase::autoDisplayDelayMs() const {
    if (!autoDisplayEnabled() || !m_mediaSettings.displayDelayEnabled) return 0;
    QString text = m_mediaSettings.displayDelayText.trimmed();
    if (text.isEmpty() || text == QStringLiteral("...")) return 0;
    text.replace(',', '.');
    bool ok = false;
    double value = text.toDouble(&ok);
    if (!ok || value < 0.0) return 0;
    return static_cast<int>(std::lround(value * 1000.0));
}

bool ResizableMediaBase::autoUnmuteEnabled() const {
    return m_mediaSettings.unmuteAutomatically;
}

int ResizableMediaBase::autoUnmuteDelayMs() const {
    if (!autoUnmuteEnabled() || !m_mediaSettings.unmuteDelayEnabled) return 0;
    QString text = m_mediaSettings.unmuteDelayText.trimmed();
    if (text.isEmpty() || text == QStringLiteral("...")) return 0;
    text.replace(',', '.');
    bool ok = false;
    double value = text.toDouble(&ok);
    if (!ok || value < 0.0) return 0;
    return static_cast<int>(std::lround(value * 1000.0));
}

bool ResizableMediaBase::autoPlayEnabled() const {
    return m_mediaSettings.playAutomatically;
}

int ResizableMediaBase::autoPlayDelayMs() const {
    if (!autoPlayEnabled() || !m_mediaSettings.playDelayEnabled) return 0;
    QString text = m_mediaSettings.playDelayText.trimmed();
    if (text.isEmpty() || text == QStringLiteral("...")) return 0;
    text.replace(',', '.');
    bool ok = false;
    double value = text.toDouble(&ok);
    if (!ok || value < 0.0) return 0;
    return static_cast<int>(std::lround(value * 1000.0));
}

bool ResizableMediaBase::autoPauseEnabled() const {
    return m_mediaSettings.pauseDelayEnabled;
}

int ResizableMediaBase::autoPauseDelayMs() const {
    if (!autoPauseEnabled()) return 0;
    QString text = m_mediaSettings.pauseDelayText.trimmed();
    if (text.isEmpty() || text == QStringLiteral("...")) return 0;
    text.replace(',', '.');
    bool ok = false;
    double value = text.toDouble(&ok);
    if (!ok || value < 0.0) return 0;
    return static_cast<int>(std::lround(value * 1000.0));
}

bool ResizableMediaBase::autoHideEnabled() const {
    return m_mediaSettings.hideDelayEnabled;
}

int ResizableMediaBase::autoHideDelayMs() const {
    if (!autoHideEnabled()) return 0;
    QString text = m_mediaSettings.hideDelayText.trimmed();
    if (text.isEmpty() || text == QStringLiteral("...")) return 0;
    text.replace(',', '.');
    bool ok = false;
    double value = text.toDouble(&ok);
    if (!ok) return 0;
    return static_cast<int>(std::lround(value * 1000.0));
}

bool ResizableMediaBase::hideWhenVideoEnds() const {
    return m_mediaSettings.hideWhenVideoEnds;
}

bool ResizableMediaBase::autoMuteEnabled() const {
    return m_mediaSettings.muteDelayEnabled;
}

int ResizableMediaBase::autoMuteDelayMs() const {
    if (!autoMuteEnabled()) return 0;
    QString text = m_mediaSettings.muteDelayText.trimmed();
    if (text.isEmpty() || text == QStringLiteral("...")) return 0;
    text.replace(',', '.');
    bool ok = false;
    double value = text.toDouble(&ok);
    if (!ok) return 0;
    return static_cast<int>(std::lround(value * 1000.0));
}

bool ResizableMediaBase::muteWhenVideoEnds() const {
    return m_mediaSettings.muteWhenVideoEnds;
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

double ResizableMediaBase::audioFadeInDurationSeconds() const {
    if (!m_mediaSettings.audioFadeInEnabled) return 0.0;
    QString text = m_mediaSettings.audioFadeInText.trimmed();
    if (text == QStringLiteral("∞") || text.isEmpty() || text == QStringLiteral("...")) return 0.0;
    text.replace(',', '.');
    bool ok = false;
    double value = text.toDouble(&ok);
    if (!ok || value < 0.0) return 0.0;
    return std::clamp(value, 0.0, 3600.0);
}

double ResizableMediaBase::audioFadeOutDurationSeconds() const {
    if (!m_mediaSettings.audioFadeOutEnabled) return 0.0;
    QString text = m_mediaSettings.audioFadeOutText.trimmed();
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
    if (!p.isEmpty() && MediaRuntimeHooks::fileManager()) {
        m_fileId = MediaRuntimeHooks::fileManager()->getOrCreateFileId(p);
        MediaRuntimeHooks::fileManager()->associateMediaWithFile(m_mediaId, m_fileId);
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

        // QuickCanvas computes and applies snapped positions explicitly in controller code.
        // Bypass legacy ScreenCanvas/grid snapping once to avoid double-snapping jitter.
        if (m_suppressNextItemPositionSnap) {
            m_suppressNextItemPositionSnap = false;
            return QVariant(p);
        }
        
        // First apply pixel grid snapping
        p = snapPointToGrid(p);
        
        // Disable movement screen-border snapping (Shift) during ANY active resize (corner or midpoint)
        // to prevent the opposite/fixed corner from being repositioned while scaling.
        bool anyResizeActive = (m_activeHandle != None);
        const auto screenSnap = MediaRuntimeHooks::screenSnapCallback();
        if (screenSnap && !anyResizeActive) {
            const bool shiftPressed = QGuiApplication::keyboardModifiers().testFlag(Qt::ShiftModifier);
            if (shiftPressed) {
                const QRectF mediaBounds(0, 0, m_baseSize.width() * scale(), m_baseSize.height() * scale());
                p = screenSnap(p, mediaBounds, shiftPressed, this);
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
            bool altPressed = QGuiApplication::keyboardModifiers().testFlag(Qt::AltModifier);
            if (!allowAltResize()) {
                altPressed = false;
            }
            const bool wasAltStretching = m_lastAxisAltStretch;
            const qreal currDist = std::hypot(sceneDelta.x(), sceneDelta.y());
            if (!altPressed) {
                // Corner style uniform scaling (original logic simplified)
                if (wasAltStretching) {
                    m_initialScale = scale();
                    m_initialGrabDist = (currDist > 1e-6) ? currDist : 1e-6;
                    m_cornerStretchOrigCaptured = false;
                }
                qreal newScale = m_initialScale * (currDist / (m_initialGrabDist > 0 ? m_initialGrabDist : 1e-6));
                newScale = std::clamp<qreal>(newScale, 0.05, 100.0);
                qreal finalScale = newScale;
                const auto resizeSnap = MediaRuntimeHooks::resizeSnapCallback();
                if (resizeSnap && QGuiApplication::keyboardModifiers().testFlag(Qt::ShiftModifier)) {
                    auto feedback = resizeSnap(newScale, m_fixedScenePoint, m_fixedItemPoint, m_baseSize, true, this);
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
                if (!wasAltStretching && m_cornerStretchOrigCaptured) m_cornerStretchOrigCaptured = false;
            } else {
                // Alt + corner: non-uniform two-axis stretch by directly changing base size (independent width/height)
                // Bake current uniform scale into base size when Alt mode starts
                if (!m_cornerStretchOrigCaptured || !wasAltStretching) {
                    const bool derivedHandlesBaking = onAltResizeModeEngaged();
                    qreal s = scale();
                    
                    // Capture original size - always use current m_baseSize
                    // For derived classes (TextMedia), they handle scale via m_uniformScaleFactor
                    // so we work with baseSize, and the visual size = baseSize * scale
                    QSize originalSize = m_baseSize;
                    
                    if (!derivedHandlesBaking && std::abs(s - 1.0) > 1e-9) {
                        prepareGeometryChange();
                        m_baseSize.setWidth(std::max(1, int(std::round(m_baseSize.width() * s))));
                        m_baseSize.setHeight(std::max(1, int(std::round(m_baseSize.height() * s))));
                        setScale(1.0);
                        // Re-evaluate fixed corner scene point after transform change
                        m_fixedItemPoint = handlePoint(opposite(m_activeHandle));
                        m_fixedScenePoint = mapToScene(m_fixedItemPoint);
                    }
                    // Rebase the interactive scale/grab metrics so switching back to uniform resize
                    // (when Alt is released) computes sensible deltas relative to the current state.
                    m_initialScale = scale();
                    {
                        const qreal d = std::hypot(event->scenePos().x() - m_fixedScenePoint.x(), event->scenePos().y() - m_fixedScenePoint.y());
                        m_initialGrabDist = (d > 1e-6) ? d : 1e-6;
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
                    m_cornerStretchOriginalBaseSize = originalSize;
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
                // For items with scale != 1 that handle their own baking (TextMedia),
                // we need to convert visual dimensions back to base dimensions
                qreal currentScale = scale();
                int newW = std::max(1, static_cast<int>(std::round(desiredW / currentScale)));
                int newH = std::max(1, static_cast<int>(std::round(desiredH / currentScale)));
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
            bool altPressed = QGuiApplication::keyboardModifiers().testFlag(Qt::AltModifier);
            if (!allowAltResize()) {
                altPressed = false;
            }
            const bool wasAltStretching = m_lastAxisAltStretch;
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
                if (!m_axisStretchOrigCaptured || !wasAltStretching) {
                    const bool derivedHandlesBaking = onAltResizeModeEngaged();
                    qreal s = scale();
                    
                    // Capture original size - always use current m_baseSize
                    // For derived classes (TextMedia), they handle scale via m_uniformScaleFactor
                    // so we work with baseSize, and the visual size = baseSize * scale
                    QSize originalSize = m_baseSize;
                    
                    if (!derivedHandlesBaking && std::abs(s - 1.0) > 1e-9) {
                        prepareGeometryChange();
                        m_baseSize.setWidth(std::max(1, int(std::round(m_baseSize.width() * s))));
                        m_baseSize.setHeight(std::max(1, int(std::round(m_baseSize.height() * s))));
                        setScale(1.0); // reset scale so subsequent width math is linear
                        // Re-evaluate fixed corner scene point after transform change
                        m_fixedItemPoint = handlePoint(opposite(m_activeHandle));
                        m_fixedScenePoint = mapToScene(m_fixedItemPoint);
                    }
                    // Update initial scale/grab so exiting Alt (back to uniform) doesn't jump.
                    m_initialScale = scale();
                    {
                        const qreal d = std::hypot(event->scenePos().x() - m_fixedScenePoint.x(), event->scenePos().y() - m_fixedScenePoint.y());
                        m_initialGrabDist = (d > 1e-6) ? d : 1e-6;
                    }
                    // Capture initial cursor offset to preserve relative positioning
                    QPointF currentMovingEdgeScene = mapToScene(handlePoint(m_activeHandle));
                    qreal cursorToEdgeDist = horizontalHandle ? (event->scenePos().x() - currentMovingEdgeScene.x())
                                                              : (event->scenePos().y() - currentMovingEdgeScene.y());
                    // Apply the same direction normalization as deltaScene to ensure consistency
                    if (m_activeHandle == LeftMid || m_activeHandle == TopMid) cursorToEdgeDist = -cursorToEdgeDist;
                    m_axisStretchInitialOffset = cursorToEdgeDist;
                    m_axisStretchOriginalBaseSize = originalSize;
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
                // For items with scale != 1 that handle their own baking (TextMedia),
                // we need to convert visual dimensions back to base dimensions
                qreal currentScale = scale();
                if (horizontalHandle) {
                    int newW = std::max(1, static_cast<int>(std::round(desiredAxisSize / currentScale)));
                    if (newW != newBase.width()) {
                        prepareGeometryChange();
                        newBase.setWidth(newW);
                        m_baseSize = newBase;
                    }
                } else {
                    int newH = std::max(1, static_cast<int>(std::round(desiredAxisSize / currentScale)));
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
    m_topPanel = std::make_unique<OverlayPanel>(OverlayPanel::Top);
    m_topPanel->setStyle(m_overlayStyle);
    
    if (!m_filename.isEmpty()) {
        // Add filename label
        auto filenameElement = std::make_shared<OverlayTextElement>(m_filename, "filename");
        filenameElement->setMaxWidthPx(gOverlayFilenameMaxWidthPx);
        m_topPanel->addElement(filenameElement);
        
        // Insert row break so buttons appear on second row below filename
        m_topPanel->newRow();
        
        // Add standard media overlay buttons using factory
        OverlayPanel::MediaOverlayCallbacks callbacks;
        
        callbacks.onVisibilityToggle = [this](bool visible) {
            // Retrieve fade parameters from per-media settings
            const double fadeInSeconds = fadeInDurationSeconds();
            const double fadeOutSeconds = fadeOutDurationSeconds();
            
            // Always cancel any in-flight fade before starting a new transition
            cancelFade();
            
            if (visible) {
                // Switch to visible using fade in if configured
                if (fadeInSeconds > 0.0) {
                    if (m_contentDisplayOpacity <= 0.0) m_contentDisplayOpacity = 0.0;
                    setContentVisible(true);
                    fadeContentIn(fadeInSeconds);
                } else {
                    setContentVisible(true);
                    m_contentDisplayOpacity = 1.0;
                    update();
                }
            } else {
                // Switch to hidden using fade out if configured
                if (fadeOutSeconds > 0.0) {
                    fadeContentOut(fadeOutSeconds);
                } else {
                    setContentVisible(false);
                    m_contentDisplayOpacity = 0.0;
                    update();
                }
            }
        };
        
        callbacks.onBringForward = [this]() {
            if (scene() && !scene()->views().isEmpty()) {
                if (auto* screenCanvas = qobject_cast<ScreenCanvas*>(scene()->views().first())) {
                    screenCanvas->moveMediaUp(this);
                }
            }
        };
        
        callbacks.onBringBackward = [this]() {
            if (scene() && !scene()->views().isEmpty()) {
                if (auto* screenCanvas = qobject_cast<ScreenCanvas*>(scene()->views().first())) {
                    screenCanvas->moveMediaDown(this);
                }
            }
        };
        
        callbacks.onDelete = [this]() {
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
        };
        
        m_topPanel->addStandardMediaOverlayButtons(callbacks, true);
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
    onOverlayLayoutUpdated();
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

namespace {
qint64 frameTimestampMs(const QVideoFrame& frame) {
    if (!frame.isValid()) {
        return -1;
    }
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
    const qint64 startTimeUs = frame.startTime();
    if (startTimeUs >= 0) {
        return startTimeUs / 1000;
    }
#else
    const qint64 startTimeUs = frame.startTime();
    if (startTimeUs >= 0) {
        return startTimeUs / 1000;
    }
    const QVariant metaTimestamp = frame.metaData(QVideoFrame::StartTime);
    if (metaTimestamp.isValid()) {
        bool ok = false;
        const qint64 micro = metaTimestamp.toLongLong(&ok);
        if (ok) {
            return micro / 1000;
        }
    }
#endif
    return -1;
}
}

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

    const qreal initialVolume = m_audio ? std::clamp<qreal>(m_audio->volume(), 0.0, 1.0) : 1.0;
    m_effectiveMuted = m_audio ? m_audio->isMuted() : false;
    m_userVolumeRatio = initialVolume;
    m_audioFadeTargetVolume = initialVolume;
    m_lastUserVolumeBeforeMute = initialVolume;

    QObject::connect(m_audio, &QAudioOutput::volumeChanged, m_player, [this](qreal v){
        v = std::clamp<qreal>(v, 0.0, 1.0);
        if (!m_volumeChangeFromSettings && !m_volumeChangeFromAudioFade) {
            m_userVolumeRatio = v;
            if (!m_effectiveMuted && v > 0.0) {
                m_lastUserVolumeBeforeMute = v;
            }
            const int percent = std::clamp<int>(static_cast<int>(std::lround(v * 100.0)), 0, 100);
            const QString text = QString::number(percent);
            if (m_mediaSettings.volumeText != text) {
                m_mediaSettings.volumeText = text;
            }
            if (isSelected()) {
                updateControlsLayout();
                update();
            }
        }
    });


    QObject::connect(m_sink, &QVideoSink::videoFrameChanged, m_player, [this](const QVideoFrame& f){
        bool allowVisualUpdate = true;
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
                converted = applyViewportCrop(converted, f);
                m_lastFrameImage = std::move(converted);
                const qint64 ts = frameTimestampMs(f);
                if (ts >= 0) {
                    m_lastFrameTimestampMs = ts;
                } else if (m_player) {
                    m_lastFrameTimestampMs = m_player->position();
                }
                ++m_framesProcessed;
                if (m_warmupActive) {
                    if (!m_warmupFrameCaptured) {
                        m_warmupFrameCaptured = true;
                    } else {
                        allowVisualUpdate = false;
                    }
                }
            }
            logFrameStats();
        }
        if (allowVisualUpdate && shouldRepaint()) {
            m_lastRepaintMs = QDateTime::currentMSecsSinceEpoch();
            update();
        }
    });

    ensureControlsPanel();
    m_controlsLockedUntilReady = true;
    m_controlsDidInitialFade = false;
    if (m_controlsPanel) {
        if (scene() && m_controlsPanel->scene() != scene()) {
            m_controlsPanel->setScene(scene());
        }
        m_controlsPanel->setVisible(false);
        if (auto* root = m_controlsPanel->rootItem()) {
            root->setOpacity(0.0);
        }
    }

    m_progressTimer = new QTimer(); m_progressTimer->setInterval(33);
    QObject::connect(m_progressTimer, &QTimer::timeout, [this]() {
        if (m_player && m_player->playbackState() == QMediaPlayer::PlayingState && !m_draggingProgress && !m_holdLastFrameAtEnd && !m_seeking && m_durationMs > 0) {
            qint64 currentPos = m_player->position(); qreal newRatio = static_cast<qreal>(currentPos) / m_durationMs; m_smoothProgressRatio = std::clamp<qreal>(newRatio, 0.0, 1.0); updateProgressBar(); update();
        }
    });
    m_warmupKeepAliveTimer = new QTimer();
    m_warmupKeepAliveTimer->setInterval(3500);
    m_warmupKeepAliveTimer->setTimerType(Qt::CoarseTimer);
    QObject::connect(m_warmupKeepAliveTimer, &QTimer::timeout, m_player, [this]() {
        handleWarmupKeepAlive();
    });
    QObject::connect(m_player, &QMediaPlayer::mediaStatusChanged, m_player, [this](QMediaPlayer::MediaStatus s){
        if (s == QMediaPlayer::LoadedMedia || s == QMediaPlayer::BufferedMedia) {
            if (!m_adoptedSize) {
                const QMediaMetaData md = m_player->metaData();
                
                // First, try to extract cover/thumbnail images which represent correct display dimensions
                // (especially important for videos with non-square pixels where storage != display dimensions)
                QImage coverImage;
                const QVariant thumbVar = md.value(QMediaMetaData::ThumbnailImage);
                if (thumbVar.isValid()) {
                    if (thumbVar.canConvert<QImage>()) { coverImage = thumbVar.value<QImage>(); }
                    else if (thumbVar.canConvert<QPixmap>()) { coverImage = thumbVar.value<QPixmap>().toImage(); }
                }
                if (coverImage.isNull()) {
                    const QVariant coverVar = md.value(QMediaMetaData::CoverArtImage);
                    if (coverVar.canConvert<QImage>()) { coverImage = coverVar.value<QImage>(); }
                    else if (coverVar.canConvert<QPixmap>()) { coverImage = coverVar.value<QPixmap>().toImage(); }
                }
                
                // If we have a cover image, use its dimensions as the display size (respects PAR/DAR)
                // and lock the display size to prevent frame storage dimensions from overriding it
                if (!coverImage.isNull()) {
                    qDebug() << "ResizableVideoItem: metadata cover image size =" << coverImage.size() << "for" << m_sourcePath;
                    m_posterImage = coverImage;
                    m_posterImageSet = true;
                    m_lastFrameDisplaySize = QSizeF(coverImage.size());
                    m_displaySizeLocked = true; // Lock to prevent frame dimensions from overriding
                    adoptBaseSize(coverImage.size());
                    update();
                } else {
                    // Fallback: use metadata resolution (storage dimensions, may not respect DAR)
                    const QVariant v = md.value(QMediaMetaData::Resolution);
                    const QSize sz = v.toSize();
                    if (!sz.isEmpty()) {
                        qDebug() << "ResizableVideoItem: metadata resolution (storage) =" << sz << "for" << m_sourcePath;
                        m_lastFrameDisplaySize = QSizeF(sz);
                        adoptBaseSize(sz);
                    }
                }
            }
            if (!m_firstFramePrimed && !m_warmupActive) {
                startWarmup();
            }
        }
        if (s == QMediaPlayer::EndOfMedia) {
            bool shouldRepeat = false;
            if (m_repeatEnabled) {
                shouldRepeat = true;
            } else if (consumeAutoRepeatOpportunity()) {
                shouldRepeat = true;
            }

            if (shouldRepeat) {
                m_holdLastFrameAtEnd = false;
                m_seamlessLoopJumpPending = false;
                m_lastSeamlessLoopTriggerMs = 0;
                if (m_progressTimer) m_progressTimer->stop();
                m_smoothProgressRatio = 0.0;
                updateProgressBar();
                if (m_player) {
                    m_player->setPosition(0);
                    m_player->play();
                }
                QTimer::singleShot(10, [this]() {
                    if (m_progressTimer && m_player && m_player->playbackState() == QMediaPlayer::PlayingState) {
                        m_progressTimer->start();
                    }
                });
                m_expectedPlayingState = true;
                updatePlayPauseIconState(true);
                updateControlsLayout();
                update();
            } else {
                m_holdLastFrameAtEnd = true;
                m_seamlessLoopJumpPending = false;
                m_lastSeamlessLoopTriggerMs = 0;
                if (m_durationMs > 0) m_positionMs = m_durationMs;
                m_smoothProgressRatio = 1.0;
                updateProgressBar();
                if (m_progressTimer) m_progressTimer->stop();
                if (m_player) {
                    m_player->pause();
                    m_player->setPosition(0);
                }
                cancelSettingsRepeatSession();
                updateControlsLayout();
                update();
                m_expectedPlayingState = false;
                updatePlayPauseIconState(false);
            }
        }
    });
    QObject::connect(m_player, &QMediaPlayer::durationChanged, m_player, [this](qint64 d){ m_durationMs = d; update(); });
    QObject::connect(m_player, &QMediaPlayer::positionChanged, m_player, [this](qint64 p){
        if (m_warmupActive) {
            const qreal bufferProgress = m_player ? m_player->bufferProgress() : 0.0;
            if (p >= m_warmupTargetPositionMs || bufferProgress >= 0.95) {
                finishWarmup(false);
                // After finishing warmup, position will be reset to 0
                p = m_player ? m_player->position() : 0;
            } else {
                return;
            }
        }
        if (m_holdLastFrameAtEnd) {
            return;
        }

        const bool playing = (m_player && m_player->playbackState() == QMediaPlayer::PlayingState);
        if (playing && !m_seeking && !m_draggingProgress && m_durationMs > 0 && shouldAutoRepeat()) {
            const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
            qint64 leadMargin = std::clamp<qint64>(m_durationMs / 48, 15LL, 120LL); // aim for ~20ms-120ms early seek
            if (leadMargin >= m_durationMs) {
                leadMargin = std::max<qint64>(m_durationMs / 4, 1LL);
                if (leadMargin >= m_durationMs) {
                    leadMargin = std::max<qint64>(m_durationMs - 1, 1LL);
                }
            }

            if (!m_seamlessLoopJumpPending && p >= (m_durationMs - leadMargin)) {
                if (consumeAutoRepeatOpportunity()) {
                    m_seamlessLoopJumpPending = true;
                    m_lastSeamlessLoopTriggerMs = nowMs;
                    m_holdLastFrameAtEnd = false;
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
            }

            if (m_seamlessLoopJumpPending) {
                const qint64 settleMargin = std::clamp<qint64>(leadMargin, 15LL, 200LL);
                if (p <= settleMargin || (nowMs - m_lastSeamlessLoopTriggerMs) > 500) {
                    m_seamlessLoopJumpPending = false;
                }
            }
        } else {
            if (!shouldAutoRepeat()) {
                m_seamlessLoopJumpPending = false;
            }
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
    if (m_warmupKeepAliveTimer) {
        m_warmupKeepAliveTimer->stop();
        delete m_warmupKeepAliveTimer;
        m_warmupKeepAliveTimer = nullptr;
    }
    if (m_player) QObject::disconnect(m_player, nullptr, nullptr, nullptr);
    if (m_sink) QObject::disconnect(m_sink, nullptr, nullptr, nullptr);
    delete m_player; delete m_audio; delete m_sink; delete m_controlsFadeAnim;
}

void ResizableVideoItem::togglePlayPause() {
    if (!m_player) return;
    stopWarmupKeepAlive();
    if (m_warmupActive) {
        finishWarmup(true);
    }
    if (!m_firstFramePrimed) {
        startWarmup();
        finishWarmup(true);
    }
    m_seamlessLoopJumpPending = false;
    m_lastSeamlessLoopTriggerMs = 0;
    bool nowPlaying = false;
    if (m_player->playbackState() == QMediaPlayer::PlayingState) {
        m_player->pause();
        if (m_progressTimer) m_progressTimer->stop();
        nowPlaying = false;
    } else {
        const qint64 startThreshold = nearStartThresholdMs();
        const qint64 playerPos = m_player ? m_player->position() : m_positionMs;
        const qint64 effectivePos = (m_positionMs > 0) ? m_positionMs : playerPos;
        const bool nearStart = (effectivePos <= startThreshold);
        bool nearEnd = false;
        if (m_durationMs > 0) {
            const qint64 endThreshold = std::clamp<qint64>(m_durationMs / 64, 15LL, 250LL);
            if (effectivePos >= (m_durationMs - endThreshold)) {
                nearEnd = true;
            }
        }

        bool resetToBeginning = false;
        if (m_holdLastFrameAtEnd || nearEnd || nearStart) {
            m_holdLastFrameAtEnd = false;
            m_seamlessLoopJumpPending = false;
            m_lastSeamlessLoopTriggerMs = 0;
            m_positionMs = 0;
            // Only seek if we're not already at position 0 (avoid unnecessary seek delay)
            if (effectivePos > 10) {  // Allow 10ms tolerance
                m_player->setPosition(0);
            }
            m_smoothProgressRatio = 0.0;
            updateProgressBar();
            resetToBeginning = true;
        }

        if (resetToBeginning) {
            initializeSettingsRepeatSessionForPlaybackStart();
        }

        m_player->play();
        if (m_progressTimer) m_progressTimer->start();
        nowPlaying = true;
    }
    m_expectedPlayingState = nowPlaying;
    updatePlayPauseIconState(nowPlaying);
    updateControlsLayout(); update();
    if (!nowPlaying) {
        startWarmupKeepAlive();
    }
}

void ResizableVideoItem::toggleRepeat() {
    m_repeatEnabled = !m_repeatEnabled;
    m_seamlessLoopJumpPending = false;
    m_lastSeamlessLoopTriggerMs = 0;
    updateControlsVisualState();
    updateControlsLayout();
    update();
}

void ResizableVideoItem::toggleMute() {
    if (!m_audio) return;
    setMuted(!m_effectiveMuted);
}

void ResizableVideoItem::setVolume(qreal ratio) {
    setVolumeFromControl(std::clamp(ratio, 0.0, 1.0), false);
}

void ResizableVideoItem::setMuted(bool muted, bool skipFade) {
    if (!m_audio) return;

    // Cancel any in-flight fade before applying new state
    stopAudioFadeAnimation(true);

    const bool targetMuted = muted;
    const bool alreadyMuted = (m_effectiveMuted == targetMuted) && !m_audioFadeAnimation;
    if (alreadyMuted) {
        // Ensure hardware mute state matches logical state
        m_audio->setMuted(targetMuted);
        if (targetMuted) {
            const bool prevGuard = m_volumeChangeFromAudioFade;
            m_volumeChangeFromAudioFade = true;
            m_audio->setVolume(0.0);
            m_volumeChangeFromAudioFade = prevGuard;
        }
        updateControlsVisualState();
        updateControlsLayout();
        update();
        return;
    }

    m_effectiveMuted = targetMuted;
    m_savedMuted = targetMuted;

    const double fadeSeconds = skipFade ? 0.0 : (targetMuted ? audioFadeOutDurationSeconds() : audioFadeInDurationSeconds());
    const qreal currentVolume = m_audio ? std::clamp<qreal>(m_audio->volume(), 0.0, 1.0) : 0.0;
    const qreal desiredVolume = targetMuted ? 0.0 : volumeFromSettingsState();

    if (!targetMuted) {
        // Ensure we capture latest intended playback volume
        if (desiredVolume > 0.0) {
            m_lastUserVolumeBeforeMute = desiredVolume;
        }
    }

    if (fadeSeconds <= 0.0) {
        const bool prevGuard = m_volumeChangeFromAudioFade;
        m_volumeChangeFromAudioFade = true;
        m_audio->setMuted(false);
        m_audio->setVolume(desiredVolume);
        m_volumeChangeFromAudioFade = prevGuard;
        m_audio->setMuted(targetMuted);
        if (targetMuted) {
            const bool guard = m_volumeChangeFromAudioFade;
            m_volumeChangeFromAudioFade = true;
            m_audio->setVolume(0.0);
            m_volumeChangeFromAudioFade = guard;
        }
        updateControlsVisualState();
        updateControlsLayout();
        update();
        return;
    }

    const qreal startVolume = targetMuted ? currentVolume : 0.0;
    const qreal endVolume = targetMuted ? 0.0 : desiredVolume;
    if (targetMuted) {
        m_lastUserVolumeBeforeMute = (currentVolume > 0.0) ? currentVolume : desiredVolume;
    }
    startAudioFade(startVolume, endVolume, fadeSeconds, targetMuted);
    updateControlsVisualState();
    updateControlsLayout();
    update();
}

void ResizableVideoItem::stopToBeginning() {
    if (!m_player) return;
    m_seamlessLoopJumpPending = false;
    m_lastSeamlessLoopTriggerMs = 0;
    m_holdLastFrameAtEnd = false;
    m_player->pause(); m_player->setPosition(0);
    m_positionMs = 0; m_smoothProgressRatio = 0.0; updateProgressBar();
    if (m_progressTimer) m_progressTimer->stop();
    cancelSettingsRepeatSession();
    m_expectedPlayingState = false; updatePlayPauseIconState(false);
    updateControlsLayout(); update();
    startWarmupKeepAlive();
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
    cancelSettingsRepeatSession();
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
    cancelSettingsRepeatSession();
    m_expectedPlayingState = false; updatePlayPauseIconState(false);
    updateControlsLayout(); update();
    startWarmupKeepAlive();
}

void ResizableVideoItem::setExternalPosterImage(const QImage& img) {
    if (img.isNull()) {
        return;
    }
    m_posterImage = img;
    m_posterImageSet = true;
    m_lastFrameDisplaySize = QSizeF(img.size());
    m_displaySizeLocked = true; // Lock display size to prevent frame dimensions from overriding
    if (!m_adoptedSize) {
        adoptBaseSize(img.size());
    }
    update();
}

void ResizableVideoItem::setApplicationSuspended(bool suspended) {
    if (m_appSuspended == suspended) return;
    m_appSuspended = suspended;
    m_seamlessLoopJumpPending = false;
    m_lastSeamlessLoopTriggerMs = 0;
    if (m_appSuspended) {
        cancelSettingsRepeatSession();
        stopWarmupKeepAlive();
    }
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
        if (!isPlaying()) {
            startWarmupKeepAlive();
        }
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
        auto targetDisplaySize = [&](const QImage& img) -> QSize {
            if (!m_lastFrameDisplaySize.isEmpty()) {
                return QSize(
                    std::max(1, static_cast<int>(std::lround(m_lastFrameDisplaySize.width()))),
                    std::max(1, static_cast<int>(std::lround(m_lastFrameDisplaySize.height()))));
            }
            return img.size();
        };
        auto drawImg = [&](const QImage& img){
            if (img.isNull() || effective <= 0.0) return;
            const QSize displaySize = targetDisplaySize(img);
            QRectF dst = fitRect(br, displaySize);
            if (effective >= 0.999) {
                painter->drawImage(dst, img);
            } else {
                painter->save();
                painter->setOpacity(effective);
                painter->drawImage(dst, img);
                painter->restore();
            }
        };
        if (!m_lastFrameImage.isNull()) {
            drawImg(m_lastFrameImage);
        } else if (m_posterImageSet && !m_posterImage.isNull()) {
            drawImg(m_posterImage);
        }
    }
    paintSelectionAndLabel(painter);
}


void ResizableVideoItem::maybeAdoptFrameSize(const QVideoFrame& f) {
    if (!f.isValid()) {
        return;
    }

    // If display size is locked (e.g., from external poster with correct aspect ratio),
    // don't let frame storage dimensions override it
    if (m_displaySizeLocked) {
        qDebug() << "ResizableVideoItem: display size locked, ignoring frame storage dimensions" 
                 << f.size() << "keeping display size" << QSize(baseWidth(), baseHeight());
        return;
    }

    const QSizeF displaySizeF = computeFrameDisplaySize(f);
    if (displaySizeF.isEmpty()) {
        return;
    }

    m_lastFrameDisplaySize = displaySizeF;

    const QSize targetSize(
        std::max(1, static_cast<int>(std::lround(displaySizeF.width()))),
        std::max(1, static_cast<int>(std::lround(displaySizeF.height()))));

    if (targetSize.isEmpty()) {
        return;
    }

    bool forceAdopt = !m_adoptedSize;
    if (!forceAdopt && !m_fillContentWithoutAspect) {
        const qreal currentW = baseWidth();
        const qreal currentH = baseHeight();
        if (currentW > 0.0 && currentH > 0.0) {
            const qreal currentAspect = currentW / currentH;
            const qreal newAspect = displaySizeF.width() / displaySizeF.height();
            if (std::abs(currentAspect - newAspect) > 0.01) {
                forceAdopt = true;
            }
        }
    }

    if (forceAdopt) {
        adoptBaseSize(targetSize, true);
    }
}

QSizeF ResizableVideoItem::computeFrameDisplaySize(const QVideoFrame& frame) const {
    if (!frame.isValid()) {
        return QSizeF();
    }

    const QVideoFrameFormat format = frame.surfaceFormat();
    const int width = format.frameWidth();
    const int height = format.frameHeight();

    if (width <= 0 || height <= 0) {
        return QSizeF();
    }

    // Start with storage dimensions
    qreal displayWidth = static_cast<qreal>(width);
    qreal displayHeight = static_cast<qreal>(height);

    // For now, use storage dimensions as display dimensions
    // (pixel aspect ratio and rotation are not reliably available in Qt 6.x)
    return QSizeF(displayWidth, displayHeight);
}

QImage ResizableVideoItem::applyViewportCrop(const QImage& image, const QVideoFrame& frame) const {
    if (image.isNull()) {
        return image;
    }

    const QVideoFrameFormat format = frame.surfaceFormat();
    const QRectF viewport = format.viewport();
    if (viewport.isNull()) {
        return image;
    }

    QRect cropRect = viewport.toAlignedRect();
    if (cropRect.isEmpty()) {
        return image;
    }

    cropRect = cropRect.intersected(image.rect());
    if (cropRect.isEmpty() || cropRect == image.rect()) {
        return image;
    }

    return image.copy(cropRect);
}

void ResizableVideoItem::adoptBaseSize(const QSize& sz, bool force) {
    if (sz.isEmpty()) {
        return;
    }
    if (m_adoptedSize && !force) {
        return;
    }

    const QRectF oldRect(0,0, baseWidth(), baseHeight());
    const QPointF oldCenterScene = mapToScene(oldRect.center());

    prepareGeometryChange();
    m_baseSize = sz;
    m_adoptedSize = true;
    setScale(m_initialScaleFactor);

    const QPointF newTopLeftScene = oldCenterScene - QPointF(sz.width() * m_initialScaleFactor / 2.0,
                                                             sz.height() * m_initialScaleFactor / 2.0);
    setPos(newTopLeftScene);
    update();
}

void ResizableVideoItem::ensureControlsPanel() {
    if (!m_controlsPanel) {
        m_controlsPanel = std::make_unique<OverlayPanel>(OverlayPanel::Bottom);
        OverlayStyle controlsStyle = m_overlayStyle;
        controlsStyle.paddingX = std::max(controlsStyle.paddingX, 8);
        controlsStyle.paddingY = std::max(controlsStyle.paddingY, 6);
        controlsStyle.itemSpacing = std::max(controlsStyle.itemSpacing, 8);
        controlsStyle.maxWidth = 360;
        const int desiredControlHeight = 36;
        controlsStyle.defaultHeight = std::max(controlsStyle.defaultHeight, desiredControlHeight);
        m_controlsPanel->setStyle(controlsStyle);
        m_controlsPanel->setBackgroundVisible(true);

        // Use factory to create all standard video controls in one call
        OverlayPanel::VideoControlCallbacks callbacks;
        callbacks.onPlayPause = [this]() { m_holdLastFrameAtEnd = false; togglePlayPause(); };
        callbacks.onStop = [this]() { stopToBeginning(); };
        callbacks.onRepeat = [this]() { toggleRepeat(); };
        callbacks.onMute = [this]() { toggleMute(); };
        callbacks.onVolumeBegin = [this](qreal ratio) {
            m_draggingVolume = true;
            setVolumeFromControl(ratio, false);
            if (auto slider = m_controlsPanel->getSlider("volume")) slider->setState(OverlayElement::Active);
        };

        callbacks.onVolumeUpdate = [this](qreal ratio) { setVolumeFromControl(ratio, false); };
        callbacks.onVolumeEnd = [this](qreal ratio) {
            setVolumeFromControl(ratio, false);
            m_draggingVolume = false;
            if (auto slider = m_controlsPanel->getSlider("volume")) {
                slider->setState(m_effectiveMuted ? OverlayElement::Disabled : OverlayElement::Normal);
            }
        };
        callbacks.onProgressBegin = [this](qreal ratio) {
            m_draggingProgress = true;
            m_holdLastFrameAtEnd = false;
            seekToRatio(ratio);
            if (auto slider = m_controlsPanel->getSlider("progress")) slider->setState(OverlayElement::Active);
        };
        callbacks.onProgressUpdate = [this](qreal ratio) { m_holdLastFrameAtEnd = false; seekToRatio(ratio); };
        callbacks.onProgressEnd = [this](qreal ratio) {
            seekToRatio(ratio);
            m_draggingProgress = false;
            if (auto slider = m_controlsPanel->getSlider("progress")) slider->setState(OverlayElement::Normal);
        };
        
        m_controlsPanel->addStandardVideoControls(callbacks);
        m_controlsPanel->setVisible(false);
    }

    if (scene() && m_controlsPanel && m_controlsPanel->scene() != scene()) {
        m_controlsPanel->setScene(scene());
    }
}

void ResizableVideoItem::updateControlsVisualState() {
    if (!m_controlsPanel) return;

    const bool playing = isEffectivelyPlayingForControls() || m_expectedPlayingState;
    if (auto playPause = m_controlsPanel->getButton("play-pause")) {
        playPause->setSvgIcon(playing ? ":/icons/icons/pause.svg" : ":/icons/icons/play.svg");
    }

    if (auto repeat = m_controlsPanel->getButton("repeat")) {
        repeat->setState((m_repeatEnabled || m_settingsRepeatEnabled) ? OverlayElement::Toggled : OverlayElement::Normal);
    }

    if (auto mute = m_controlsPanel->getButton("mute")) {
        mute->setSvgIcon(m_effectiveMuted ? ":/icons/icons/volume-off.svg" : ":/icons/icons/volume-on.svg");
        mute->setState(m_effectiveMuted ? OverlayElement::Toggled : OverlayElement::Normal);
    }

    if (auto volumeSlider = m_controlsPanel->getSlider("volume")) {
        if (!m_draggingVolume) {
            volumeSlider->setValue(std::clamp<qreal>(m_effectiveMuted ? 0.0 : m_userVolumeRatio, 0.0, 1.0));
            volumeSlider->setState(m_effectiveMuted ? OverlayElement::Disabled : OverlayElement::Normal);
        }
    }

    if (auto progressSlider = m_controlsPanel->getSlider("progress")) {
        if (!m_draggingProgress) {
            progressSlider->setValue(std::clamp<qreal>(m_smoothProgressRatio, 0.0, 1.0));
            progressSlider->setState(OverlayElement::Normal);
        }
    }
}

void ResizableVideoItem::startWarmup() {
    if (!m_player || m_warmupActive) {
        return;
    }

    stopWarmupKeepAlive();
    m_warmupActive = true;
    m_warmupFrameCaptured = false;
    m_firstFramePrimed = false;
    m_holdLastFrameAtEnd = false;
    m_controlsLockedUntilReady = true;
    m_savedMuted = m_effectiveMuted;
    m_effectiveMuted = true;
    if (m_audio) {
        m_audio->setMuted(true);
    }

    qint64 target = 500; // default warmup duration in ms
    if (m_durationMs > 0) {
        target = std::clamp<qint64>(m_durationMs / 20, 250LL, 800LL);
    }
    m_warmupTargetPositionMs = target;

    if (m_player) {
        m_player->setPlaybackRate(1.0);
        m_player->setPosition(0);
        m_player->play();
    }
}

void ResizableVideoItem::finishWarmup(bool forceImmediate) {
    if (!m_player || !m_warmupActive) {
        return;
    }

    m_warmupActive = false;

    m_player->pause();
    m_player->setPlaybackRate(1.0);
    m_player->setPosition(0);

    m_positionMs = 0;
    m_smoothProgressRatio = 0.0;
    updateProgressBar();

    qDebug() << "ResizableVideoItem: warmup primed for" << m_sourcePath
             << "targetMs=" << m_warmupTargetPositionMs
             << "bufferProgress=" << m_player->bufferProgress();

    if (m_audio) {
        m_audio->setMuted(m_savedMuted);
        if (!m_savedMuted) {
            const bool guardPrev = m_volumeChangeFromSettings;
            m_volumeChangeFromSettings = true;
            m_audio->setVolume(m_userVolumeRatio);
            m_volumeChangeFromSettings = guardPrev;
        }
    }

    m_effectiveMuted = m_savedMuted;
    m_firstFramePrimed = true;
    m_controlsLockedUntilReady = false;
    m_controlsDidInitialFade = false;

    if (isSelected() || forceImmediate) {
        setControlsVisible(true);
        updateControlsLayout();
    }

    m_lastRepaintMs = 0;
    update();
    m_lastWarmupCompletionMs = QDateTime::currentMSecsSinceEpoch();
    startWarmupKeepAlive();
}

void ResizableVideoItem::startWarmupKeepAlive() {
    if (!m_warmupKeepAliveTimer) {
        return;
    }
    if (!m_firstFramePrimed || m_warmupActive || isPlaying() || m_appSuspended) {
        stopWarmupKeepAlive();
        return;
    }
    if (!m_warmupKeepAliveTimer->isActive()) {
        m_warmupKeepAliveTimer->start();
    }
}

void ResizableVideoItem::stopWarmupKeepAlive() {
    if (m_warmupKeepAliveTimer && m_warmupKeepAliveTimer->isActive()) {
        m_warmupKeepAliveTimer->stop();
    }
}

void ResizableVideoItem::handleWarmupKeepAlive() {
    if (!m_player || m_warmupActive || m_keepAlivePulseActive) {
        return;
    }
    if (isPlaying() || m_appSuspended) {
        stopWarmupKeepAlive();
        return;
    }
    if (!m_firstFramePrimed) {
        stopWarmupKeepAlive();
        return;
    }
    if (m_holdLastFrameAtEnd || m_draggingProgress || m_draggingVolume || m_seeking) {
        return;
    }
    if (!isVisibleInAnyView()) {
        return;
    }
    if (m_positionMs > nearStartThresholdMs()) {
        return;
    }
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    if (now - m_lastWarmupCompletionMs < 1500) {
        return;
    }
    performWarmupPulse();
}

void ResizableVideoItem::performWarmupPulse() {
    if (!m_player || m_keepAlivePulseActive) {
        return;
    }

    m_keepAlivePulseActive = true;
    stopWarmupKeepAlive();

    const bool temporarilyMuted = m_audio && !m_effectiveMuted;
    if (temporarilyMuted) {
        m_audio->setMuted(true);
    }

    const qreal previousRate = m_player->playbackRate();
    m_player->setPlaybackRate(2.0);
    m_player->setPosition(0);
    m_player->play();

    const std::weak_ptr<bool> guard = lifetimeGuard();
    QTimer::singleShot(280, [this, guard, temporarilyMuted, previousRate]() {
        if (auto alive = guard.lock(); !alive || !m_player) {
            return;
        }

        m_player->pause();
        m_player->setPlaybackRate(previousRate > 0.0 ? previousRate : 1.0);
        m_player->setPosition(0);

        if (temporarilyMuted && m_audio) {
            m_audio->setMuted(false);
            const bool guardPrev = m_volumeChangeFromSettings;
            m_volumeChangeFromSettings = true;
            m_audio->setVolume(m_userVolumeRatio);
            m_volumeChangeFromSettings = guardPrev;
        }

        m_lastWarmupCompletionMs = QDateTime::currentMSecsSinceEpoch();
        m_keepAlivePulseActive = false;
        startWarmupKeepAlive();
    });
}

QRectF ResizableVideoItem::boundingRect() const {
    return QRectF(0.0, 0.0, baseWidth(), baseHeight());
}

QPainterPath ResizableVideoItem::shape() const {
    QPainterPath path;
    path.addRect(boundingRect());
    return path;
}

QVariant ResizableVideoItem::itemChange(QGraphicsItem::GraphicsItemChange change, const QVariant& value) {
    if (change == ItemSceneChange && m_controlsPanel && m_controlsPanel->scene()) {
        m_controlsPanel->setVisible(false);
        m_controlsPanel->setScene(nullptr);
    }

    QVariant result = ResizableMediaBase::itemChange(change, value);

    if (change == ItemSelectedHasChanged) {
        const bool selected = value.toBool();
        if (selected && !m_controlsLockedUntilReady) {
            setControlsVisible(true);
        } else {
            setControlsVisible(false);
        }
    } else if (change == ItemSceneHasChanged) {
        if (scene()) {
            ensureControlsPanel();
            if (m_controlsPanel) {
                m_controlsPanel->setScene(scene());
                if (isSelected() && !m_controlsLockedUntilReady) {
                    setControlsVisible(true);
                }
            }
        }
    } else if (change == ItemPositionHasChanged || change == ItemTransformHasChanged) {
        updateControlsLayout();
    }

    return result;
}

void ResizableVideoItem::mousePressEvent(QGraphicsSceneMouseEvent* event) {
    if (event->button() == Qt::LeftButton && !m_controlsLockedUntilReady) {
        setControlsVisible(true);
    }
    ResizableMediaBase::mousePressEvent(event);
}

void ResizableVideoItem::mouseMoveEvent(QGraphicsSceneMouseEvent* event) {
    ResizableMediaBase::mouseMoveEvent(event);
    if (event->isAccepted()) {
        updateControlsLayout();
    }
}

void ResizableVideoItem::mouseReleaseEvent(QGraphicsSceneMouseEvent* event) {
    ResizableMediaBase::mouseReleaseEvent(event);
    if (!m_controlsLockedUntilReady) {
        updateControlsLayout();
    }
}

void ResizableVideoItem::mouseDoubleClickEvent(QGraphicsSceneMouseEvent* event) {
    if (event->button() == Qt::LeftButton) {
        togglePlayPause();
        event->accept();
        return;
    }
    QGraphicsItem::mouseDoubleClickEvent(event);
}

void ResizableVideoItem::prepareForDeletion() {
    ResizableMediaBase::prepareForDeletion();
    teardownPlayback();
    if (m_controlsPanel) {
        m_controlsPanel->setVisible(false);
        m_controlsPanel->setScene(nullptr);
    }
}

void ResizableVideoItem::onInteractiveGeometryChanged() {
    ResizableMediaBase::onInteractiveGeometryChanged();
    updateControlsLayout();
}

void ResizableVideoItem::setControlsVisible(bool show) {
    ensureControlsPanel();
    if (!m_controlsPanel) return;

    const bool allow = show && !m_controlsLockedUntilReady;
    if (!allow) {
        if (m_controlsFadeAnim) m_controlsFadeAnim->stop();
        if (auto* root = m_controlsPanel->rootItem()) {
            root->setOpacity(0.0);
        }
        m_controlsPanel->setVisible(false);
        updatePlayPauseIconState(false);
        return;
    }

    m_controlsPanel->setVisible(true);
    updateControlsVisualState();
    const bool playingState = isEffectivelyPlayingForControls() || m_expectedPlayingState;

    if (!m_controlsFadeAnim) {
        m_controlsFadeAnim = new QVariantAnimation();
        m_controlsFadeAnim->setEasingCurve(QEasingCurve::OutCubic);
        QObject::connect(m_controlsFadeAnim, &QVariantAnimation::valueChanged, [this](const QVariant& v){
            if (m_controlsPanel && m_controlsPanel->rootItem()) {
                m_controlsPanel->rootItem()->setOpacity(v.toReal());
            }
        });
    }

    if (!m_controlsDidInitialFade) {
        m_controlsFadeAnim->stop();
        if (m_controlsPanel->rootItem()) {
            m_controlsPanel->rootItem()->setOpacity(0.0);
        }
        m_controlsFadeAnim->setDuration(m_controlsFadeMs);
        m_controlsFadeAnim->setStartValue(0.0);
        m_controlsFadeAnim->setEndValue(1.0);
        m_controlsDidInitialFade = true;
        m_controlsFadeAnim->start();
    } else {
        if (m_controlsFadeAnim) m_controlsFadeAnim->stop();
        if (m_controlsPanel->rootItem()) {
            m_controlsPanel->rootItem()->setOpacity(1.0);
        }
    }

    updatePlayPauseIconState(playingState);
}

void ResizableVideoItem::updateControlsLayout() {
    ensureControlsPanel();
    if (!m_controlsPanel) return;

    if (scene() && m_controlsPanel->scene() != scene()) {
        m_controlsPanel->setScene(scene());
    }

    updateControlsVisualState();

    const bool shouldShow = isSelected() && !m_controlsLockedUntilReady;
    if (!shouldShow) {
        m_controlsPanel->setVisible(false);
        return;
    }

    if (!scene() || scene()->views().isEmpty()) {
        return;
    }

    QGraphicsView* view = scene()->views().first();
    if (!view) {
        return;
    }

    QPointF bottomCenterItem(baseWidth() / 2.0, baseHeight());
    QPointF anchorScene = mapToScene(bottomCenterItem);
    m_controlsPanel->setVisible(true);
    m_controlsPanel->updateLayoutWithAnchor(anchorScene, view);
}

qreal ResizableVideoItem::volumeFromSettingsState() const {
    const MediaSettingsState& state = mediaSettingsState();
    QString text = state.volumeText.trimmed();
    if (text.isEmpty() || text == QStringLiteral("...")) {
        text = QStringLiteral("100");
    }
    bool ok = false;
    int percent = text.toInt(&ok);
    if (!ok) {
        percent = 100;
    }
    percent = std::clamp(percent, 0, 100);
    return static_cast<qreal>(percent) / 100.0;
}

void ResizableVideoItem::applyVolumeRatio(qreal ratio) {
    ratio = std::clamp<qreal>(ratio, 0.0, 1.0);
    m_userVolumeRatio = ratio;
    const bool previousGuard = m_volumeChangeFromSettings;
    m_volumeChangeFromSettings = true;
    if (m_audio) {
        m_audio->setVolume(ratio);
    }
    m_volumeChangeFromSettings = previousGuard;
    if (!m_effectiveMuted && ratio > 0.0) {
        m_lastUserVolumeBeforeMute = ratio;
    }
    if (isSelected()) {
        updateControlsLayout();
        update();
    }
}

void ResizableVideoItem::setVolumeFromControl(qreal ratio, bool fromSettings) {
    ratio = std::clamp<qreal>(ratio, 0.0, 1.0);

    if (fromSettings) {
        applyVolumeRatio(ratio);
        return;
    }

    const int percent = std::clamp<int>(static_cast<int>(std::lround(ratio * 100.0)), 0, 100);
    const QString text = QString::number(percent);

    // Update settings state (slider controls the settings, settings control the volume)
    if (!m_mediaSettings.volumeOverrideEnabled) {
        m_mediaSettings.volumeOverrideEnabled = true;
    }
    if (m_mediaSettings.volumeText != text) {
        m_mediaSettings.volumeText = text;
    }

    // Apply volume from settings state (instead of directly) to ensure settings are the source of truth
    applyVolumeOverrideFromState();
    
    // Notify settings panel to update its display in real-time
    if (scene() && !scene()->views().isEmpty()) {
        if (auto* screenCanvas = qobject_cast<ScreenCanvas*>(scene()->views().first())) {
            screenCanvas->refreshSettingsPanelVolumeDisplay();
        }
    }
}

void ResizableVideoItem::applyVolumeOverrideFromState() {
    const MediaSettingsState& state = mediaSettingsState();
    qreal ratio = 1.0;
    if (state.volumeOverrideEnabled) {
        ratio = volumeFromSettingsState();
    }
    applyVolumeRatio(ratio);
}

void ResizableVideoItem::ensureAudioFadeAnimation() {
    if (m_audioFadeAnimation) {
        QObject::disconnect(m_audioFadeAnimation, nullptr, nullptr, nullptr);
        m_audioFadeAnimation->stop();
        m_audioFadeAnimation->deleteLater();
    }
    m_audioFadeAnimation = new QVariantAnimation();
}

void ResizableVideoItem::stopAudioFadeAnimation(bool resetVolumeGuard) {
    if (!m_audioFadeAnimation) {
        if (resetVolumeGuard) {
            m_volumeChangeFromAudioFade = false;
        }
        return;
    }
    QVariantAnimation* anim = m_audioFadeAnimation;
    m_audioFadeAnimation = nullptr;
    QObject::disconnect(anim, nullptr, nullptr, nullptr);
    anim->stop();
    anim->deleteLater();
    if (resetVolumeGuard) {
        m_volumeChangeFromAudioFade = false;
    }
}

void ResizableVideoItem::startAudioFade(qreal startVolume, qreal endVolume, double durationSeconds, bool targetMuted) {
    if (!m_audio) {
        m_effectiveMuted = targetMuted;
        return;
    }
    startVolume = std::clamp<qreal>(startVolume, 0.0, 1.0);
    endVolume = std::clamp<qreal>(endVolume, 0.0, 1.0);

    stopAudioFadeAnimation(false);
    ensureAudioFadeAnimation();
    if (!m_audioFadeAnimation) {
        m_effectiveMuted = targetMuted;
        m_volumeChangeFromAudioFade = false;
        return;
    }

    m_pendingMuteTarget = targetMuted;
    m_audioFadeStartVolume = startVolume;
    m_audioFadeTargetVolume = endVolume;
    m_volumeChangeFromAudioFade = true;

    m_audio->setMuted(false);
    m_audio->setVolume(startVolume);

    m_audioFadeAnimation->setStartValue(startVolume);
    m_audioFadeAnimation->setEndValue(endVolume);
    m_audioFadeAnimation->setDuration(std::max(1, static_cast<int>(durationSeconds * 1000.0)));
    m_audioFadeAnimation->setEasingCurve(QEasingCurve::Linear);

    QObject::connect(m_audioFadeAnimation, &QVariantAnimation::valueChanged, [this, targetMuted](const QVariant& value) {
        Q_UNUSED(targetMuted);
        if (!m_audio) {
            return;
        }
        const qreal vol = std::clamp<qreal>(value.toDouble(), 0.0, 1.0);
        m_audio->setVolume(vol);
        if (!m_effectiveMuted && vol > 0.0) {
            m_lastUserVolumeBeforeMute = vol;
        }
        if (isSelected()) {
            updateControlsLayout();
        }
        update();
    });

    QObject::connect(m_audioFadeAnimation, &QVariantAnimation::finished, [this]() {
        finalizeAudioFade(m_pendingMuteTarget);
    });

    m_audioFadeAnimation->start();
}

void ResizableVideoItem::finalizeAudioFade(bool targetMuted) {
    QVariantAnimation* anim = m_audioFadeAnimation;
    m_audioFadeAnimation = nullptr;
    if (anim) {
        QObject::disconnect(anim, nullptr, nullptr, nullptr);
        anim->stop();
        anim->deleteLater();
    }

    if (!m_audio) {
        m_volumeChangeFromAudioFade = false;
        return;
    }

    const qreal finalVolume = std::clamp<qreal>(m_audioFadeTargetVolume, 0.0, 1.0);
    m_audio->setMuted(false);
    m_audio->setVolume(finalVolume);
    m_audio->setMuted(targetMuted);
    if (!targetMuted && finalVolume > 0.0) {
        m_lastUserVolumeBeforeMute = finalVolume;
    }
    m_volumeChangeFromAudioFade = false;
    if (isSelected()) {
        updateControlsLayout();
    }
    update();
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
    ensureControlsPanel();
    if (auto playPause = m_controlsPanel->getButton("play-pause")) {
        const QString iconPath = playing ? QStringLiteral(":/icons/icons/pause.svg")
                                         : QStringLiteral(":/icons/icons/play.svg");
        playPause->setSvgIcon(iconPath);
        playPause->setState(playing ? OverlayElement::Active : OverlayElement::Normal);
    }
    updateControlsVisualState();
}

void ResizableVideoItem::onMediaSettingsChanged() {
    ResizableMediaBase::onMediaSettingsChanged();

    const MediaSettingsState& state = mediaSettingsState();
    bool enabled = state.repeatEnabled;
    int parsedLoops = 0;
    if (enabled) {
        QString text = state.repeatCountText.trimmed();
        bool ok = false;
        int value = text.toInt(&ok);
        if (!ok || value <= 0) {
            enabled = false;
        } else {
            parsedLoops = value;
        }
    }

    m_settingsRepeatEnabled = enabled;
    m_settingsRepeatLoopCount = enabled ? parsedLoops : 0;

    if (!m_settingsRepeatEnabled) {
        cancelSettingsRepeatSession();
    } else if (!m_settingsRepeatSessionActive) {
        m_settingsRepeatLoopsRemaining = m_settingsRepeatLoopCount;
    } else {
        m_settingsRepeatLoopsRemaining = std::clamp(m_settingsRepeatLoopsRemaining, 0, m_settingsRepeatLoopCount);
    }

    applyVolumeOverrideFromState();
    updateControlsLayout();
}

void ResizableVideoItem::initializeSettingsRepeatSessionForPlaybackStart() {
    if (!m_settingsRepeatEnabled || m_settingsRepeatLoopCount <= 0) {
        cancelSettingsRepeatSession();
        return;
    }
    m_settingsRepeatSessionActive = true;
    m_settingsRepeatLoopsRemaining = m_settingsRepeatLoopCount;
}

void ResizableVideoItem::cancelSettingsRepeatSession() {
    m_settingsRepeatSessionActive = false;
    m_settingsRepeatLoopsRemaining = 0;
}

bool ResizableVideoItem::settingsRepeatAvailable() const {
    return m_settingsRepeatSessionActive && m_settingsRepeatLoopsRemaining > 0;
}

bool ResizableVideoItem::shouldAutoRepeat() const {
    return m_repeatEnabled || settingsRepeatAvailable();
}

bool ResizableVideoItem::consumeAutoRepeatOpportunity() {
    if (m_repeatEnabled) {
        return true;
    }
    if (settingsRepeatAvailable()) {
        --m_settingsRepeatLoopsRemaining;
        return true;
    }
    return false;
}

qint64 ResizableVideoItem::nearStartThresholdMs() const {
    if (m_durationMs <= 0) {
        return 250;
    }
    qint64 threshold = m_durationMs / 60;
    threshold = std::clamp<qint64>(threshold, 80, 350);
    return threshold;
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

void ResizableVideoItem::updateProgressBar() {
    // Progress is now managed by the overlay slider element
    if (auto progressSlider = m_controlsPanel->getSlider("progress")) {
        if (!m_draggingProgress) {
            progressSlider->setValue(std::clamp<qreal>(m_smoothProgressRatio, 0.0, 1.0));
        }
    }
}

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
    m_warmupActive = false;
    m_warmupFrameCaptured = false;
    m_holdLastFrameAtEnd = false;
    m_lastFrameImage = QImage();
    m_lastFrameDisplaySize = QSizeF();
    m_lastFrameTimestampMs = -1;
    m_smoothProgressRatio = 0.0;
    m_positionMs = 0;
    cancelSettingsRepeatSession();
    if (m_progressTimer) {
        m_progressTimer->stop();
    }
    m_controlsLockedUntilReady = true;
    m_controlsDidInitialFade = false;
    startWarmup();
}

void ResizableVideoItem::teardownPlayback() {
    if (m_playbackTornDown) {
        return;
    }
    m_playbackTornDown = true;
    cancelSettingsRepeatSession();
    stopWarmupKeepAlive();

    m_warmupActive = false;
    m_warmupFrameCaptured = false;

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

    m_lastFrameTimestampMs = -1;
    m_lastFrameDisplaySize = QSizeF();

    if (m_audio) {
        QObject::disconnect(m_audio, nullptr, nullptr, nullptr);
        m_audio->setMuted(true);
    }
}
