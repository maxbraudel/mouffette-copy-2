#include "MainWindow.h"
#include <QMenuBar>
#include <QMessageBox>
#include <QHostInfo>
#include <QGuiApplication>
#include <QDebug>
#include <QCloseEvent>
#include <QResizeEvent>
#include <QKeyEvent>
#include <QStyleOption>
#include <algorithm>
#include <cmath>
#include <QDialog>
#include <QLineEdit>
#include <QNativeGestureEvent>
#include <QCursor>
#include <QRandomGenerator>
#include <QPainter>
#include <QPaintEvent>
#include <QGraphicsOpacityEffect>
#include <QPropertyAnimation>
#include <QVariantAnimation>
#include <QEasingCurve>
#include <QTimer>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QDragMoveEvent>
#include <QMimeData>
#include <QGraphicsPixmapItem>
#include <QGraphicsSceneMouseEvent>
#include <QPen>
#include <QBrush>
#include <QUrl>
#include <QImage>
#include <QPixmap>
#include <QGraphicsTextItem>
#include <QGraphicsRectItem>
#include <QGraphicsPathItem>
#include <QtSvgWidgets/QGraphicsSvgItem>
#include <QtSvg/QSvgRenderer>
#include <QPainterPathStroker>
#include <QFileInfo>
#include <climits>
#ifdef Q_OS_MACOS
#include "MacCursorHider.h"
#include "MacVideoThumbnailer.h"
#endif
#include <QGraphicsItem>
#include <QSet>
#include <QMediaPlayer>
#include <QAudioOutput>
#include <QVideoSink>
#include <QMediaMetaData>
#include <QVideoFrame>
#include <QElapsedTimer>
#include <QDateTime>
#include <QThreadPool>
#include <QRunnable>
#include <QMutex>
#include <QMutexLocker>
#include <QThread>
#include <QObject>
#include <atomic>
#include <thread>
#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <objbase.h>
#include <combaseapi.h>
// Ensure GUIDs (IIDs/CLSIDs) are defined in this translation unit for MinGW linkers
#ifndef INITGUID
#define INITGUID
#endif
#include <initguid.h>
#include <mmdeviceapi.h>
#include <endpointvolume.h>
#endif
#ifdef Q_OS_MACOS
#include <QProcess>
#endif

const QString MainWindow::DEFAULT_SERVER_URL = "ws://192.168.0.188:8080";

// Z-ordering constants used throughout the scene
namespace {
constexpr qreal Z_SCREENS = -1000.0;
constexpr qreal Z_MEDIA_BASE = 1.0;
constexpr qreal Z_REMOTE_CURSOR = 10000.0;
constexpr qreal Z_SCENE_OVERLAY = 12000.0; // above all scene content
}

// Ensure all fade animations respect the configured duration
void MainWindow::applyAnimationDurations() {
    if (m_spinnerFade) m_spinnerFade->setDuration(m_loaderFadeDurationMs);
    if (m_canvasFade) m_canvasFade->setDuration(m_fadeDurationMs);
    if (m_volumeFade) m_volumeFade->setDuration(m_fadeDurationMs);
}

// Simple modern circular line spinner (no Q_OBJECT needed)
class SpinnerWidget : public QWidget {
public:
    explicit SpinnerWidget(QWidget* parent = nullptr)
        : QWidget(parent)
        , m_angle(0)
        , m_radiusPx(24)      // default visual radius in pixels
        , m_lineWidthPx(6)    // default line width
        , m_color("#4a90e2") // default color matches Send button
    {
        setAttribute(Qt::WA_TransparentForMouseEvents, true);
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
        m_timer = new QTimer(this);
        connect(m_timer, &QTimer::timeout, this, [this]() {
            m_angle = (m_angle + 6) % 360; // smooth rotation
            update();
        });
        m_timer->setInterval(16); // ~60 FPS
    }

    void start() { if (!m_timer->isActive()) m_timer->start(); }
    void stop() { if (m_timer->isActive()) m_timer->stop(); }

    // Customization knobs
    void setRadius(int radiusPx) {
        m_radiusPx = qMax(8, radiusPx);
        updateGeometry();
        update();
    }
    void setLineWidth(int px) {
        m_lineWidthPx = qMax(1, px);
        update();
    }
    void setColor(const QColor& c) {
        m_color = c;
        update();
    }
    int radius() const { return m_radiusPx; }
    int lineWidth() const { return m_lineWidthPx; }
    QColor color() const { return m_color; }

protected:
    void paintEvent(QPaintEvent* event) override {
        Q_UNUSED(event);
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing, true);

        // Palette-aware background
        QStyleOption opt; opt.initFrom(this);
        style()->drawPrimitive(QStyle::PE_Widget, &opt, &p, this);

    const int s = qMin(width(), height());
    // Use configured radius but ensure it fits inside current widget size with padding
    int outer = 2 * m_radiusPx;        // desired diameter from config
    const int maxOuter = qMax(16, s - 12); // keep some padding to avoid clipping
    outer = qMin(outer, maxOuter);
    const int thickness = qMin(m_lineWidthPx, qMax(1, outer / 2)); // ensure sane bounds

        QPointF center(width() / 2.0, height() / 2.0);
        QRectF rect(center.x() - outer/2.0, center.y() - outer/2.0, outer, outer);
        
    QColor arc = m_color;
    arc.setAlpha(230);

    // Flat caps (no rounded ends) as requested
    p.setPen(QPen(arc, thickness, Qt::SolidLine, Qt::FlatCap));
        int span = 16 * 300; // 300-degree arc for modern look
        p.save();
        p.translate(center);
        p.rotate(m_angle);
        p.translate(-center);
        p.drawArc(rect, 0, span);
        p.restore();
    }

    QSize minimumSizeHint() const override { return QSize(48, 48); }

private:
    QTimer* m_timer;
    int m_angle;
    int m_radiusPx;
    int m_lineWidthPx;
    QColor m_color;
};

// Simple rounded-rectangle graphics item with a settable rect and radius
class RoundedRectItem : public QGraphicsPathItem {
public:
    explicit RoundedRectItem(QGraphicsItem* parent = nullptr)
        : QGraphicsPathItem(parent) {}
    void setRect(const QRectF& r) { m_rect = r; updatePath(); }
    void setRect(qreal x, qreal y, qreal w, qreal h) { setRect(QRectF(x, y, w, h)); }
    QRectF rect() const { return m_rect; }
    void setRadius(qreal radiusPx) { m_radius = std::max<qreal>(0.0, radiusPx); updatePath(); }
    qreal radius() const { return m_radius; }
private:
    void updatePath() {
        QPainterPath p;
        if (m_rect.isNull()) { setPath(p); return; }
        // Clamp radius so it never exceeds half of width/height
        const qreal r = std::min({ m_radius, m_rect.width() * 0.5, m_rect.height() * 0.5 });
        if (r > 0.0)
            p.addRoundedRect(m_rect, r, r);
        else
            p.addRect(m_rect);
        setPath(p);
    }
    QRectF m_rect;
    qreal  m_radius = 0.0;
};

// Resizable, movable pixmap item with corner handles; keeps aspect ratio
class ResizableMediaBase : public QGraphicsItem {
public:
    virtual ~ResizableMediaBase() override {
        // If label overlay was reparented to the scene, delete it explicitly
        if (m_labelBg && m_labelBg->parentItem() == nullptr) {
            delete m_labelBg; // also deletes m_labelText child
            m_labelBg = nullptr;
            m_labelText = nullptr;
        }
    }
    explicit ResizableMediaBase(const QSize& baseSizePx, int visualSizePx, int selectionSizePx, const QString& filename = QString())
    {
        m_visualSize = qMax(4, visualSizePx);
        m_selectionSize = qMax(m_visualSize, selectionSizePx);
        setFlags(QGraphicsItem::ItemIsMovable | QGraphicsItem::ItemIsSelectable | QGraphicsItem::ItemSendsGeometryChanges);
    setAcceptHoverEvents(true);
        m_baseSize = baseSizePx;
        setScale(1.0);
        setZValue(1.0);
        // Filename label setup (zoom-independent via ItemIgnoresTransformations)
        m_filename = filename;
    m_labelBg = new RoundedRectItem(this);
    m_labelBg->setPen(Qt::NoPen);
    // Unified translucent dark background used by label and video controls
    m_labelBg->setBrush(QColor(0, 0, 0, 160));
    m_labelBg->setZValue(Z_SCENE_OVERLAY); // keep overlays above media
        m_labelBg->setCacheMode(QGraphicsItem::DeviceCoordinateCache);
        m_labelBg->setFlag(QGraphicsItem::ItemIgnoresTransformations, true);
        m_labelBg->setAcceptedMouseButtons(Qt::NoButton);
        m_labelText = new QGraphicsTextItem(m_filename, m_labelBg);
        m_labelText->setDefaultTextColor(Qt::white);
    m_labelText->setZValue(Z_SCENE_OVERLAY + 1);
        m_labelText->setCacheMode(QGraphicsItem::DeviceCoordinateCache);
        m_labelText->setFlag(QGraphicsItem::ItemIgnoresTransformations, true);
        m_labelText->setAcceptedMouseButtons(Qt::NoButton);
        updateLabelLayout();
    }
    // Global override (in pixels) for the height of media overlays (e.g., video controls).
    // -1 means "auto" (use filename label background height).
    static void setHeightOfMediaOverlaysPx(int px) { heightOfMediaOverlays = px; }
    static int getHeightOfMediaOverlaysPx() { return heightOfMediaOverlays; }
    // Global override for corner radius (in pixels) for overlay buttons and filename label background
    // Applies to: filename background, play/stop/repeat/mute buttons. Excludes: progress & volume bars.
    static void setCornerRadiusOfMediaOverlaysPx(int px) { cornerRadiusOfMediaOverlays = std::max(0, px); }
    static int getCornerRadiusOfMediaOverlaysPx() { return cornerRadiusOfMediaOverlays; }
    // Public hook for view to relayout the filename label after pan/zoom
    void requestLabelRelayout() { updateLabelLayout(); }
    // Utility for view: tell if a given item-space pos is on a resize handle
    bool isOnHandleAtItemPos(const QPointF& itemPos) const {
        return hitTestHandle(itemPos) != None;
    }

    // Start a resize operation given a scene position; returns true if a handle was engaged
    bool beginResizeAtScenePos(const QPointF& scenePos) {
        Handle h = hitTestHandle(mapFromScene(scenePos));
        if (h == None) return false;
        m_activeHandle = h;
        m_fixedItemPoint = handlePoint(opposite(m_activeHandle));
        m_fixedScenePoint = mapToScene(m_fixedItemPoint);
    m_initialScale = scale();
    const qreal d = std::hypot(scenePos.x() - m_fixedScenePoint.x(),
                   scenePos.y() - m_fixedScenePoint.y());
    m_initialGrabDist = (d > 1e-6) ? d : 1e-6;
        grabMouse();
        return true;
    }

    // For view-level hover support: suggest a cursor based on a scene position
    Qt::CursorShape cursorForScenePos(const QPointF& scenePos) const {
        switch (hitTestHandle(mapFromScene(scenePos))) {
            case TopLeft:
            case BottomRight:
                return Qt::SizeFDiagCursor;
            case TopRight:
            case BottomLeft:
                return Qt::SizeBDiagCursor;
            default:
                return Qt::ArrowCursor;
        }
    }

    // Check if this item is currently being resized
    bool isActivelyResizing() const {
        return m_activeHandle != None;
    }

    void setHandleVisualSize(int px) {
        int newVisual = qMax(4, px);
        int newSelection = qMax(m_selectionSize, newVisual);
        if (newSelection != m_selectionSize) {
            prepareGeometryChange();
            m_selectionSize = newSelection;
        }
        m_visualSize = newVisual;
        update();
    }
    void setHandleSelectionSize(int px) {
        int newSel = qMax(4, px);
        if (newSel != m_selectionSize) {
            prepareGeometryChange();
            m_selectionSize = newSel;
            update();
        }
    }

    QRectF boundingRect() const override {
        QRectF br(0, 0, m_baseSize.width(), m_baseSize.height());
        // Only extend bounding rect for handle hit zones when selected
        if (isSelected()) {
            qreal pad = toItemLengthFromPixels(m_selectionSize) / 2.0;
            return br.adjusted(-pad, -pad, pad, pad);
        }
        return br;
    }

    QPainterPath shape() const override {
        QPainterPath path;
        QRectF br(0, 0, m_baseSize.width(), m_baseSize.height());
        path.addRect(br);
        // Only add handle hit zones when selected
        if (isSelected()) {
            const qreal s = toItemLengthFromPixels(m_selectionSize);
            path.addRect(QRectF(br.topLeft() - QPointF(s/2,s/2), QSizeF(s,s)));
            path.addRect(QRectF(QPointF(br.right(), br.top()) - QPointF(s/2,s/2), QSizeF(s,s)));
            path.addRect(QRectF(QPointF(br.left(), br.bottom()) - QPointF(s/2,s/2), QSizeF(s,s)));
            path.addRect(QRectF(br.bottomRight() - QPointF(s/2,s/2), QSizeF(s,s)));
        }
        return path;
    }

protected:
    // Called during interactive geometry changes (resize/drag) so subclasses can re-layout overlays.
    virtual void onInteractiveGeometryChanged() {}
    // Draw selection chrome only; relayout happens on transform/position changes
    void paintSelectionAndLabel(QPainter* painter) {
        if (!isSelected()) return;
        QRectF br(0, 0, m_baseSize.width(), m_baseSize.height());
        painter->save();
        painter->setBrush(Qt::NoBrush);
        QPen whitePen(QColor(255,255,255));
        whitePen.setCosmetic(true);
        whitePen.setWidth(1);
        whitePen.setStyle(Qt::DashLine);
        whitePen.setDashPattern(QVector<qreal>({4, 4}));
        whitePen.setCapStyle(Qt::FlatCap);
        whitePen.setJoinStyle(Qt::MiterJoin);
        painter->setPen(whitePen);
        painter->drawRect(br);
        QPen bluePen(QColor(74,144,226));
        bluePen.setCosmetic(true);
        bluePen.setWidth(1);
        bluePen.setStyle(Qt::DashLine);
        bluePen.setDashPattern(QVector<qreal>({4, 4}));
        bluePen.setDashOffset(4);
        bluePen.setCapStyle(Qt::FlatCap);
        bluePen.setJoinStyle(Qt::MiterJoin);
        painter->setPen(bluePen);
        painter->drawRect(br);
        painter->restore();
        const qreal s = toItemLengthFromPixels(m_visualSize);
        painter->setPen(QPen(QColor(74,144,226), 0));
        painter->setBrush(QBrush(Qt::white));
        painter->drawRect(QRectF(br.topLeft() - QPointF(s/2,s/2), QSizeF(s,s)));
        painter->drawRect(QRectF(QPointF(br.right(), br.top()) - QPointF(s/2,s/2), QSizeF(s,s)));
        painter->drawRect(QRectF(QPointF(br.left(), br.bottom()) - QPointF(s/2,s/2), QSizeF(s,s)));
        painter->drawRect(QRectF(br.bottomRight() - QPointF(s/2,s/2), QSizeF(s,s)));
    }

    QVariant itemChange(GraphicsItemChange change, const QVariant &value) override {
        if (change == ItemSelectedChange) {
            // Include/exclude handle zones (affects bounding/shape)
            prepareGeometryChange();
            // Pre-toggle visibility to match new state
            if (m_labelBg && m_labelText) {
                const bool willBeSelected = value.toBool();
                const bool show = willBeSelected && !m_filename.isEmpty();
                m_labelBg->setVisible(show);
                m_labelText->setVisible(show);
            }
        }
        if (change == ItemSelectedHasChanged) {
            // Keep label properly positioned after selection changes
            updateLabelLayout();
        }
        if (change == ItemTransformHasChanged || change == ItemPositionHasChanged) {
            // Keep the filename overlay glued during resize/move
            updateLabelLayout();
        }
    return QGraphicsItem::itemChange(change, value);
    }

    void mousePressEvent(QGraphicsSceneMouseEvent* event) override {
        m_activeHandle = hitTestHandle(event->pos());
        if (m_activeHandle != None) {
            m_fixedItemPoint = handlePoint(opposite(m_activeHandle));
            m_fixedScenePoint = mapToScene(m_fixedItemPoint);
            // Capture initial state so scaling starts from current cursor distance (no jump)
            m_initialScale = scale();
            const qreal d = std::hypot(event->scenePos().x() - m_fixedScenePoint.x(),
                                       event->scenePos().y() - m_fixedScenePoint.y());
            m_initialGrabDist = (d > 1e-6) ? d : 1e-6;
            event->accept();
            return;
        }
    QGraphicsItem::mousePressEvent(event);
    }

    void mouseMoveEvent(QGraphicsSceneMouseEvent* event) override {
        if (m_activeHandle != None) {
            const QPointF v = event->scenePos() - m_fixedScenePoint;
            const qreal currDist = std::hypot(v.x(), v.y());
            qreal newScale = m_initialScale * (currDist / (m_initialGrabDist > 0 ? m_initialGrabDist : 1e-6));
            newScale = std::clamp<qreal>(newScale, 0.05, 100.0);
            setScale(newScale);
            setPos(m_fixedScenePoint - newScale * m_fixedItemPoint);
            // Notify subclass so overlays (e.g., controls) can follow during resize
            onInteractiveGeometryChanged();
            event->accept();
            return;
        }
        // Do not manage cursor here - let the view handle it globally
    QGraphicsItem::mouseMoveEvent(event);
    }

    void mouseReleaseEvent(QGraphicsSceneMouseEvent* event) override {
        if (m_activeHandle != None) {
            m_activeHandle = None;
            ungrabMouse();
            // Final sync after interactive resize ends
            onInteractiveGeometryChanged();
            event->accept();
            return;
        }
    QGraphicsItem::mouseReleaseEvent(event);
    }

    void hoverMoveEvent(QGraphicsSceneHoverEvent* event) override {
        // Do not manage cursor here - let the view handle it globally
    QGraphicsItem::hoverMoveEvent(event);
    }

    void hoverLeaveEvent(QGraphicsSceneHoverEvent* event) override {
        // Do not manage cursor here - let the view handle it globally
    QGraphicsItem::hoverLeaveEvent(event);
    }

protected:
    enum Handle { None, TopLeft, TopRight, BottomLeft, BottomRight };
    QSize m_baseSize;
    Handle m_activeHandle = None;
    QPointF m_fixedItemPoint;  // item coords
    QPointF m_fixedScenePoint; // scene coords
    qreal m_initialScale = 1.0;
    qreal m_initialGrabDist = 1.0;
    // Desired sizes in device pixels (screen space)
    int m_visualSize = 8;
    int m_selectionSize = 12;
    Handle m_lastHoverHandle = None;
    // Filename label elements
    QString m_filename;
    RoundedRectItem* m_labelBg = nullptr;
    QGraphicsTextItem* m_labelText = nullptr;
    // Shared overlay height config
    static int heightOfMediaOverlays;
    static int cornerRadiusOfMediaOverlays;

    Handle hitTestHandle(const QPointF& p) const {
        // Only allow handle interaction when selected
        if (!isSelected()) return None;
        
        const qreal s = toItemLengthFromPixels(m_selectionSize); // selection square centered on corners
        QRectF br(0, 0, m_baseSize.width(), m_baseSize.height());
        if (QRectF(br.topLeft() - QPointF(s/2,s/2), QSizeF(s,s)).contains(p)) return TopLeft;
        if (QRectF(QPointF(br.right(), br.top()) - QPointF(s/2,s/2), QSizeF(s,s)).contains(p)) return TopRight;
        if (QRectF(QPointF(br.left(), br.bottom()) - QPointF(s/2,s/2), QSizeF(s,s)).contains(p)) return BottomLeft;
        if (QRectF(br.bottomRight() - QPointF(s/2,s/2), QSizeF(s,s)).contains(p)) return BottomRight;
        return None;
    }
    // No global override cursor helpers needed with per-item cursors
    Handle opposite(Handle h) const {
        switch (h) {
            case TopLeft: return BottomRight;
            case TopRight: return BottomLeft;
            case BottomLeft: return TopRight;
            case BottomRight: return TopLeft;
            default: return None;
        }
    }
    QPointF handlePoint(Handle h) const {
        switch (h) {
            case TopLeft: return QPointF(0,0);
            case TopRight: return QPointF(m_baseSize.width(), 0);
            case BottomLeft: return QPointF(0, m_baseSize.height());
            case BottomRight: return QPointF(m_baseSize.width(), m_baseSize.height());
            default: return QPointF(0,0);
        }
    }

    qreal toItemLengthFromPixels(int px) const {
        if (!scene() || scene()->views().isEmpty()) return px; // fallback
        QGraphicsView* v = scene()->views().first();
        QTransform itemToViewport = v->viewportTransform() * sceneTransform();
        // Effective scale along X (uniform expected)
        qreal sx = std::hypot(itemToViewport.m11(), itemToViewport.m21());
        if (sx <= 1e-6) return px;
        // Convert device pixels to item units
        return px / sx;
    }

    void updateLabelLayout() {
        if (!m_labelBg || !m_labelText) return;
        const bool show = !m_filename.isEmpty() && isSelected();
        m_labelBg->setVisible(show);
        m_labelText->setVisible(show);
        if (!show) return;

        // Padding and vertical gap in pixels
        const int padXpx = 8;
        const int padYpx = 4;
        const int gapPx  = 8;
        // Update text and background sizes (in px, unaffected by zoom)
        m_labelText->setPlainText(m_filename);
        QRectF tr = m_labelText->boundingRect();
        const qreal bgW = tr.width() + 2*padXpx;
        const qreal bgH = tr.height() + 2*padYpx;
    m_labelBg->setRect(0, 0, bgW, bgH);
    // Apply configured corner radius
    m_labelBg->setRadius(ResizableMediaBase::getCornerRadiusOfMediaOverlaysPx());
        m_labelText->setPos(padXpx, padYpx);

    // Position: center above image top edge
        if (!scene() || scene()->views().isEmpty()) {
            // Fallback using item-units conversion
            const qreal wItem = toItemLengthFromPixels(static_cast<int>(std::round(bgW)));
            const qreal hItem = toItemLengthFromPixels(static_cast<int>(std::round(bgH)));
            const qreal gapItem = toItemLengthFromPixels(gapPx);
            const qreal xItem = (m_baseSize.width() - wItem) / 2.0;
            const qreal yItem = - (hItem + gapItem);
            m_labelBg->setPos(xItem, yItem);
            return;
        }
    QGraphicsView* v = scene()->views().first();
        // Ensure the overlay is a top-level scene item so its Z-order is global
        if (m_labelBg->parentItem() != nullptr) {
            m_labelBg->setParentItem(nullptr);
            if (scene() && !m_labelBg->scene()) scene()->addItem(m_labelBg);
        }
        // Raise above everything when visible
    m_labelBg->setZValue(Z_SCENE_OVERLAY);
        // Parent top-center in viewport coords
        QPointF topCenterItem(m_baseSize.width()/2.0, 0.0);
        QPointF topCenterScene = mapToScene(topCenterItem);
    // Use high-precision mapping to avoid rounding drift when zoom changes
    QPointF topCenterView = v->viewportTransform().map(topCenterScene);
        // Desired top-left of label in viewport (pixels)
        QPointF labelTopLeftView = topCenterView - QPointF(bgW/2.0, gapPx + bgH);
        // Map back to item coords
    // Map back to scene coords using inverse viewport transform to preserve precision
    QTransform inv = v->viewportTransform().inverted();
    QPointF labelTopLeftScene = inv.map(labelTopLeftView);
        QPointF labelTopLeftItem = mapFromScene(labelTopLeftScene);
    if (m_labelBg && m_labelBg->parentItem() == nullptr) {
        m_labelBg->setPos(labelTopLeftScene);
    } else {
        m_labelBg->setPos(labelTopLeftItem);
    }
    }
};

// Default: auto (match filename label background height)
int ResizableMediaBase::heightOfMediaOverlays = -1;
int ResizableMediaBase::cornerRadiusOfMediaOverlays = 6;

// Image media implementation using the shared base
class ResizablePixmapItem : public ResizableMediaBase {
public:
    explicit ResizablePixmapItem(const QPixmap& pm, int visualSizePx, int selectionSizePx, const QString& filename = QString())
        : ResizableMediaBase(pm.size(), visualSizePx, selectionSizePx, filename), m_pix(pm)
    {}
    void paint(QPainter* painter, const QStyleOptionGraphicsItem* option, QWidget* widget) override {
        Q_UNUSED(option); Q_UNUSED(widget);
        if (!m_pix.isNull()) {
            painter->drawPixmap(QPointF(0,0), m_pix);
        }
        paintSelectionAndLabel(painter);
    }
private:
    QPixmap m_pix;
};

// Forward declaration for async frame processing
class ResizableVideoItem;

// Phase 2: Frame Conversion Worker for async processing
class FrameConversionWorker : public QRunnable {
public:
    FrameConversionWorker(ResizableVideoItem* item, QVideoFrame frame, quint64 serial)
        : m_itemPtr(reinterpret_cast<uintptr_t>(item)), m_frame(frame), m_serial(serial)
    {
        setAutoDelete(true);
    }
    
    void run() override; // Implementation moved after ResizableVideoItem definition
    
    // Global registry to validate item lifetime
    static QSet<uintptr_t> s_activeItems;
    static void registerItem(ResizableVideoItem* item) {
        s_activeItems.insert(reinterpret_cast<uintptr_t>(item));
    }
    static void unregisterItem(ResizableVideoItem* item) {
        s_activeItems.remove(reinterpret_cast<uintptr_t>(item));
    }
    
private:
    uintptr_t m_itemPtr;
    QVideoFrame m_frame;
    quint64 m_serial;
};

// Video media implementation: renders current frame and overlays controls
class ResizableVideoItem : public ResizableMediaBase {
public:
    explicit ResizableVideoItem(const QString& filePath, int visualSizePx, int selectionSizePx, const QString& filename = QString(), int controlsFadeMs = 140)
        : ResizableMediaBase(QSize(640,360), visualSizePx, selectionSizePx, filename)
    {
        // Phase 2: Register for async frame conversion
        FrameConversionWorker::registerItem(this);
        
        m_controlsFadeMs = std::max(0, controlsFadeMs);
        
        // Keep media player on main thread - Phase 2 async conversion is sufficient
        m_player = new QMediaPlayer();
        m_audio = new QAudioOutput();
        m_sink = new QVideoSink();
        m_player->setAudioOutput(m_audio);
        m_player->setVideoSink(m_sink);
        m_player->setSource(QUrl::fromLocalFile(filePath));
        
        // Connect signals directly (no worker needed)
        QObject::connect(m_sink, &QVideoSink::videoFrameChanged, m_player, [this](const QVideoFrame& f){
            ++m_framesReceived;
            
            // Only replace the cached frame when valid and not holding the last frame at end-of-media
            if (!m_holdLastFrameAtEnd && f.isValid()) {
                // Phase 2: Early visibility check - skip processing if not visible
                if (!isVisibleInAnyView()) {
                    ++m_framesSkipped;
                    logFrameStats();
                    return;
                }
                
                // Phase 2: Async frame conversion with dropping
                {
                    QMutexLocker locker(&m_frameMutex);
                    m_pendingFrame = f; // Always store latest frame
                }
                
                // Only start conversion if not already busy
                if (!m_conversionBusy.exchange(true)) {
                    quint64 serial = ++m_frameSerial;
                    auto* worker = new FrameConversionWorker(this, f, serial);
                    QThreadPool::globalInstance()->start(worker);
                    ++m_conversionsStarted;
                    ++m_framesProcessed;
                } else {
                    ++m_framesSkipped; // Conversion busy, frame will be picked up later
                }
                
                // Always store the latest frame for size adoption (lightweight)
                m_lastFrame = f;
                maybeAdoptFrameSize(f);
                
                // If we primed playback to grab the first frame, pause immediately and restore audio state
                if (m_primingFirstFrame && !m_firstFramePrimed) {
                    m_firstFramePrimed = true;
                    m_primingFirstFrame = false;
                    if (m_player) {
                        m_player->pause();
                        m_player->setPosition(0);
                    }
                    if (m_audio) m_audio->setMuted(m_savedMuted);
                    // Unlock controls now that the video is ready
                    m_controlsLockedUntilReady = false;
                    m_controlsDidInitialFade = false; // next show should fade once
                    if (isSelected()) {
                        setControlsVisible(true);
                        updateControlsLayout();
                        // Force repaint for first frame unlock
                        m_lastRepaintMs = 0;
                        update();
                        return;
                    }
                }
                
                logFrameStats();
            }
            
            // Throttled repaint (will paint existing cached image or fallback)
            if (shouldRepaint()) {
                m_lastRepaintMs = QDateTime::currentMSecsSinceEpoch();
                this->update();
            }
        });
        
    // Controls overlays (ignore transforms so they stay in absolute pixels)
    // Create as child first, then reparent to scene for scale-invariant positioning
    m_controlsBg = new QGraphicsRectItem();
    m_controlsBg->setPen(Qt::NoPen);
    // Keep container transparent; draw backgrounds on child rects so play is a square, progress is full-width
    m_controlsBg->setBrush(Qt::NoBrush);
    m_controlsBg->setZValue(Z_SCENE_OVERLAY);
    m_controlsBg->setFlag(QGraphicsItem::ItemIgnoresTransformations, true);
    m_controlsBg->setCacheMode(QGraphicsItem::DeviceCoordinateCache);
    m_controlsBg->setAcceptedMouseButtons(Qt::NoButton);
    m_controlsBg->setOpacity(0.0);
    // Keep as child of the video item so positioning uses item coordinates
    if (scene()) scene()->addItem(m_controlsBg);

    m_playBtnRectItem = new RoundedRectItem(m_controlsBg);
    m_playBtnRectItem->setCacheMode(QGraphicsItem::DeviceCoordinateCache);
    m_playBtnRectItem->setPen(Qt::NoPen);
    // Square background for the play button, same as filename label
    m_playBtnRectItem->setBrush(m_labelBg ? m_labelBg->brush() : QBrush(QColor(0,0,0,160)));
    m_playBtnRectItem->setZValue(Z_SCENE_OVERLAY + 1);
    m_playBtnRectItem->setFlag(QGraphicsItem::ItemIgnoresTransformations, true);
    m_playBtnRectItem->setAcceptedMouseButtons(Qt::NoButton);

    m_playIcon = new QGraphicsSvgItem(":/icons/icons/play.svg", m_playBtnRectItem);
    m_playIcon->setCacheMode(QGraphicsItem::DeviceCoordinateCache);
    m_playIcon->setZValue(Z_SCENE_OVERLAY + 2);
    m_playIcon->setFlag(QGraphicsItem::ItemIgnoresTransformations, true);
    m_playIcon->setAcceptedMouseButtons(Qt::NoButton);
    m_pauseIcon = new QGraphicsSvgItem(":/icons/icons/pause.svg", m_playBtnRectItem);
    m_pauseIcon->setCacheMode(QGraphicsItem::DeviceCoordinateCache);
    m_pauseIcon->setZValue(Z_SCENE_OVERLAY + 2);
    m_pauseIcon->setFlag(QGraphicsItem::ItemIgnoresTransformations, true);
    m_pauseIcon->setAcceptedMouseButtons(Qt::NoButton);
    m_pauseIcon->setVisible(false);

    // Stop button (top row, square)
    m_stopBtnRectItem = new RoundedRectItem(m_controlsBg);
    m_stopBtnRectItem->setCacheMode(QGraphicsItem::DeviceCoordinateCache);
    m_stopBtnRectItem->setPen(Qt::NoPen);
    m_stopBtnRectItem->setBrush(m_labelBg ? m_labelBg->brush() : QBrush(QColor(0,0,0,160)));
    m_stopBtnRectItem->setZValue(Z_SCENE_OVERLAY + 1);
    m_stopBtnRectItem->setFlag(QGraphicsItem::ItemIgnoresTransformations, true);
    m_stopBtnRectItem->setAcceptedMouseButtons(Qt::NoButton);
    m_stopIcon = new QGraphicsSvgItem(":/icons/icons/stop.svg", m_stopBtnRectItem);
    m_stopIcon->setCacheMode(QGraphicsItem::DeviceCoordinateCache);
    m_stopIcon->setZValue(Z_SCENE_OVERLAY + 2);
    m_stopIcon->setFlag(QGraphicsItem::ItemIgnoresTransformations, true);
    m_stopIcon->setAcceptedMouseButtons(Qt::NoButton);

    // Repeat toggle button (top row, square)
    m_repeatBtnRectItem = new RoundedRectItem(m_controlsBg);
    m_repeatBtnRectItem->setCacheMode(QGraphicsItem::DeviceCoordinateCache);
    m_repeatBtnRectItem->setPen(Qt::NoPen);
    m_repeatBtnRectItem->setBrush(m_labelBg ? m_labelBg->brush() : QBrush(QColor(0,0,0,160)));
    m_repeatBtnRectItem->setZValue(Z_SCENE_OVERLAY + 1);
    m_repeatBtnRectItem->setFlag(QGraphicsItem::ItemIgnoresTransformations, true);
    m_repeatBtnRectItem->setAcceptedMouseButtons(Qt::NoButton);
    m_repeatIcon = new QGraphicsSvgItem(":/icons/icons/loop.svg", m_repeatBtnRectItem);
    m_repeatIcon->setCacheMode(QGraphicsItem::DeviceCoordinateCache);
    m_repeatIcon->setZValue(Z_SCENE_OVERLAY + 2);
    m_repeatIcon->setFlag(QGraphicsItem::ItemIgnoresTransformations, true);
    m_repeatIcon->setAcceptedMouseButtons(Qt::NoButton);

    // Mute toggle button (top row, square)
    m_muteBtnRectItem = new RoundedRectItem(m_controlsBg);
    m_muteBtnRectItem->setCacheMode(QGraphicsItem::DeviceCoordinateCache);
    m_muteBtnRectItem->setPen(Qt::NoPen);
    m_muteBtnRectItem->setBrush(m_labelBg ? m_labelBg->brush() : QBrush(QColor(0,0,0,160)));
    m_muteBtnRectItem->setZValue(Z_SCENE_OVERLAY + 1);
    m_muteBtnRectItem->setFlag(QGraphicsItem::ItemIgnoresTransformations, true);
    m_muteBtnRectItem->setAcceptedMouseButtons(Qt::NoButton);
    m_muteIcon = new QGraphicsSvgItem(":/icons/icons/volume-on.svg", m_muteBtnRectItem);
    m_muteIcon->setCacheMode(QGraphicsItem::DeviceCoordinateCache);
    m_muteIcon->setZValue(Z_SCENE_OVERLAY + 2);
    m_muteIcon->setFlag(QGraphicsItem::ItemIgnoresTransformations, true);
    m_muteIcon->setAcceptedMouseButtons(Qt::NoButton);
    // volume-off asset shown when muted
    m_muteSlashIcon = new QGraphicsSvgItem(":/icons/icons/volume-off.svg", m_muteBtnRectItem);
    m_muteSlashIcon->setCacheMode(QGraphicsItem::DeviceCoordinateCache);
    m_muteSlashIcon->setZValue(Z_SCENE_OVERLAY + 2);
    m_muteSlashIcon->setFlag(QGraphicsItem::ItemIgnoresTransformations, true);
    m_muteSlashIcon->setAcceptedMouseButtons(Qt::NoButton);

    // Volume slider (top row, remaining width)
    m_volumeBgRectItem = new QGraphicsRectItem(m_controlsBg);
    m_volumeBgRectItem->setCacheMode(QGraphicsItem::DeviceCoordinateCache);
    m_volumeBgRectItem->setPen(Qt::NoPen);
    m_volumeBgRectItem->setBrush(m_labelBg ? m_labelBg->brush() : QBrush(QColor(0,0,0,160)));
    m_volumeBgRectItem->setZValue(Z_SCENE_OVERLAY + 1);
    m_volumeBgRectItem->setFlag(QGraphicsItem::ItemIgnoresTransformations, true);
    m_volumeBgRectItem->setAcceptedMouseButtons(Qt::NoButton);
    m_volumeFillRectItem = new QGraphicsRectItem(m_volumeBgRectItem);
    m_volumeFillRectItem->setPen(Qt::NoPen);
    m_volumeFillRectItem->setBrush(QColor(74,144,226));
    m_volumeFillRectItem->setZValue(Z_SCENE_OVERLAY + 2);
    m_volumeFillRectItem->setFlag(QGraphicsItem::ItemIgnoresTransformations, true);
    m_volumeFillRectItem->setAcceptedMouseButtons(Qt::NoButton);

    m_progressBgRectItem = new QGraphicsRectItem(m_controlsBg);
    m_progressBgRectItem->setCacheMode(QGraphicsItem::DeviceCoordinateCache);
    m_progressBgRectItem->setPen(Qt::NoPen);
    // Full-width background for the progress row, same as filename label
    m_progressBgRectItem->setBrush(m_labelBg ? m_labelBg->brush() : QBrush(QColor(0,0,0,160)));
    m_progressBgRectItem->setZValue(Z_SCENE_OVERLAY + 1);
    m_progressBgRectItem->setFlag(QGraphicsItem::ItemIgnoresTransformations, true);
    m_progressBgRectItem->setAcceptedMouseButtons(Qt::NoButton);

    m_progressFillRectItem = new QGraphicsRectItem(m_progressBgRectItem);
    m_progressFillRectItem->setPen(Qt::NoPen);
    // Progress fill uses the primary accent color; keep solid for readability over translucent bg
    m_progressFillRectItem->setBrush(QColor(74,144,226));
    m_progressFillRectItem->setZValue(Z_SCENE_OVERLAY + 2);
    m_progressFillRectItem->setFlag(QGraphicsItem::ItemIgnoresTransformations, true);
    m_progressFillRectItem->setAcceptedMouseButtons(Qt::NoButton);
    
    // Initialize smooth progress timer for real-time updates
    m_progressTimer = new QTimer();
    m_progressTimer->setInterval(33); // ~30fps updates for smooth progress (more efficient than 60fps)
    
    // Connect timer to update progress smoothly during playback
    QObject::connect(m_progressTimer, &QTimer::timeout, [this]() {
        if (m_player && m_player->playbackState() == QMediaPlayer::PlayingState && 
            !m_draggingProgress && !m_holdLastFrameAtEnd && !m_seeking && m_durationMs > 0) {
            // Calculate smooth progress based on current player position
            qint64 currentPos = m_player->position();
            qreal newRatio = static_cast<qreal>(currentPos) / m_durationMs;
            m_smoothProgressRatio = std::clamp<qreal>(newRatio, 0.0, 1.0);
            updateProgressBar();
            this->update();
        }
    });
    // Controls are hidden by default (only visible when the item is selected)
    // Lock controls until video is ready (first frame primed)
    m_controlsLockedUntilReady = true;
    m_controlsDidInitialFade = false;
    if (m_controlsBg) m_controlsBg->setVisible(false);
    if (m_playBtnRectItem) m_playBtnRectItem->setVisible(false);
    if (m_playIcon) m_playIcon->setVisible(false);
    if (m_pauseIcon) m_pauseIcon->setVisible(false);
    if (m_stopBtnRectItem) m_stopBtnRectItem->setVisible(false);
    if (m_stopIcon) m_stopIcon->setVisible(false);
    if (m_repeatBtnRectItem) m_repeatBtnRectItem->setVisible(false);
    if (m_repeatIcon) m_repeatIcon->setVisible(false);
    if (m_muteBtnRectItem) m_muteBtnRectItem->setVisible(false);
    if (m_muteIcon) m_muteIcon->setVisible(false);
    if (m_muteSlashIcon) m_muteSlashIcon->setVisible(false);
    if (m_volumeBgRectItem) m_volumeBgRectItem->setVisible(false);
    if (m_volumeFillRectItem) m_volumeFillRectItem->setVisible(false);
    if (m_progressBgRectItem) m_progressBgRectItem->setVisible(false);
    if (m_progressFillRectItem) m_progressFillRectItem->setVisible(false);
    
    // Connect media player signals directly (main thread)
    QObject::connect(m_player, &QMediaPlayer::mediaStatusChanged, m_player, [this](QMediaPlayer::MediaStatus s){
        if (s == QMediaPlayer::LoadedMedia || s == QMediaPlayer::BufferedMedia) {
            if (!m_adoptedSize) {
                const QMediaMetaData md = m_player->metaData();
                const QVariant v = md.value(QMediaMetaData::Resolution);
                const QSize sz = v.toSize();
                if (!sz.isEmpty()) adoptBaseSize(sz);
                // Try to use a poster/thumbnail image while waiting for the first frame
                const QVariant thumbVar = md.value(QMediaMetaData::ThumbnailImage);
                if (!m_posterImageSet && thumbVar.isValid()) {
                    if (thumbVar.canConvert<QImage>()) {
                        m_posterImage = thumbVar.value<QImage>();
                        m_posterImageSet = !m_posterImage.isNull();
                    } else if (thumbVar.canConvert<QPixmap>()) {
                        m_posterImage = thumbVar.value<QPixmap>().toImage();
                        m_posterImageSet = !m_posterImage.isNull();
                    }
                    if (!m_posterImageSet) {
                        const QVariant coverVar = md.value(QMediaMetaData::CoverArtImage);
                        if (coverVar.canConvert<QImage>()) {
                            m_posterImage = coverVar.value<QImage>();
                            m_posterImageSet = !m_posterImage.isNull();
                        } else if (coverVar.canConvert<QPixmap>()) {
                            m_posterImage = coverVar.value<QPixmap>().toImage();
                            m_posterImageSet = !m_posterImage.isNull();
                        }
                    }
                    if (m_posterImageSet) this->update();
                }
            }
            if (!m_firstFramePrimed && !m_primingFirstFrame) {
                m_holdLastFrameAtEnd = false; // new load, don't hold
                m_savedMuted = m_audio ? m_audio->isMuted() : false;
                if (m_audio) m_audio->setMuted(true);
                m_primingFirstFrame = true;
                if (m_player) m_player->play();
            }
        }
        if (s == QMediaPlayer::EndOfMedia) {
            if (m_repeatEnabled) {
                // Stop timer temporarily to prevent flickering during reset
                if (m_progressTimer) m_progressTimer->stop();
                // Reset progress instantly when repeating
                m_smoothProgressRatio = 0.0;
                updateProgressBar();
                m_player->setPosition(0);
                m_player->play();
                // Restart timer after a brief delay to ensure position is set
                QTimer::singleShot(10, [this]() {
                    if (m_progressTimer && m_player && m_player->playbackState() == QMediaPlayer::PlayingState) {
                        m_progressTimer->start();
                    }
                });
            } else {
                // Keep showing the last frame and pin progress to 100%
                m_holdLastFrameAtEnd = true;
                if (m_durationMs > 0) m_positionMs = m_durationMs;
                // Set progress to 100% instantly
                m_smoothProgressRatio = 1.0;
                updateProgressBar();
                // Stop smooth progress timer when video ends
                if (m_progressTimer) m_progressTimer->stop();
                // Update controls immediately to reflect Play state without waiting for further signals
                updateControlsLayout();
                update();
                if (m_player) m_player->pause();
            }
        }
    });
    
    QObject::connect(m_player, &QMediaPlayer::durationChanged, m_player, [this](qint64 d){ 
        m_durationMs = d; 
        this->update(); 
    });
    
    QObject::connect(m_player, &QMediaPlayer::positionChanged, m_player, [this](qint64 p){
        if (m_holdLastFrameAtEnd) return; // keep progress pinned at end until user action
        m_positionMs = p; 
        // Progress updates are handled by the timer for smooth real-time movement
    });
    }
    ~ResizableVideoItem() override {
    // Phase 2: Unregister from async frame conversion
    FrameConversionWorker::unregisterItem(this);
    
    // Clean up media player (main thread)
    if (m_player) QObject::disconnect(m_player, nullptr, nullptr, nullptr);
    if (m_sink) QObject::disconnect(m_sink, nullptr, nullptr, nullptr);
    delete m_player;
    delete m_audio;
    delete m_sink;
    
    delete m_controlsFadeAnim;
    // Clean up controls overlay if top-level scene item
    if (m_controlsBg && m_controlsBg->parentItem() == nullptr) {
        delete m_controlsBg;
        m_controlsBg = nullptr;
        m_playBtnRectItem = nullptr;
        m_stopBtnRectItem = nullptr;
        m_repeatBtnRectItem = nullptr;
        m_muteBtnRectItem = nullptr;
        m_volumeBgRectItem = nullptr;
        m_volumeFillRectItem = nullptr;
        m_progressBgRectItem = nullptr;
        m_progressFillRectItem = nullptr;
        m_playIcon = nullptr;
        m_pauseIcon = nullptr;
        m_stopIcon = nullptr;
        m_repeatIcon = nullptr;
        m_muteIcon = nullptr;
        m_muteSlashIcon = nullptr;
    }
    }
    void togglePlayPause() {
        if (!m_player) return;
        if (m_player->playbackState() == QMediaPlayer::PlayingState) {
            m_player->pause();
            // Stop smooth progress timer when paused
            if (m_progressTimer) m_progressTimer->stop();
        } else {
            // If we were holding the last frame, restart cleanly from the beginning
            if (m_holdLastFrameAtEnd) {
                m_holdLastFrameAtEnd = false;
                m_positionMs = 0;
                m_player->setPosition(0);
                // Reset progress to 0
                m_smoothProgressRatio = 0.0;
                updateProgressBar();
            }
            m_player->play();
            // Start smooth progress timer when playing
            if (m_progressTimer) m_progressTimer->start();
        }
        updateControlsLayout();
        update();
    }
    void stopToBeginning() {
        if (!m_player) return;
    m_holdLastFrameAtEnd = false;
        m_player->pause();
        m_player->setPosition(0);
    m_positionMs = 0;
    // Reset progress and stop timer
    m_smoothProgressRatio = 0.0;
    updateProgressBar();
    if (m_progressTimer) m_progressTimer->stop();
    updateControlsLayout();
    update();
    }
    void toggleRepeat() {
    m_repeatEnabled = !m_repeatEnabled;
    // Refresh to update button background tint
    updateControlsLayout();
    update();
    }
    void toggleMute() {
        if (!m_audio) return;
    m_audio->setMuted(!m_audio->isMuted());
    // Ensure the icon updates immediately
    updateControlsLayout();
    update();
    }
    void seekToRatio(qreal r) {
        if (!m_player || m_durationMs <= 0) return;
        r = std::clamp<qreal>(r, 0.0, 1.0);
        m_holdLastFrameAtEnd = false;
        // Suppress timer updates briefly to avoid flicker back to old position
        m_seeking = true;
        if (m_progressTimer) m_progressTimer->stop();
        // Update local UI state immediately
        m_smoothProgressRatio = r;
        m_positionMs = static_cast<qint64>(r * m_durationMs);
        updateProgressBar();
        updateControlsLayout();
        update();
        // Perform the actual seek
        const qint64 pos = m_positionMs;
        m_player->setPosition(pos);
        // Re-enable timer after a short delay to allow backend to settle
        QTimer::singleShot(30, [this]() {
            m_seeking = false;
            if (m_progressTimer && m_player && m_player->playbackState() == QMediaPlayer::PlayingState)
                m_progressTimer->start();
        });
    }
    void setInitialScaleFactor(qreal f) { m_initialScaleFactor = f; }
    void setExternalPosterImage(const QImage& img) {
        if (!img.isNull()) {
            m_posterImage = img;
            m_posterImageSet = true;
            // Adopt the natural size of the poster immediately to avoid any temporary distortion
            if (!m_adoptedSize) {
                adoptBaseSize(img.size());
            }
            update();
        }
    }
    // Drag helpers for view-level coordination
    bool isDraggingProgress() const { return m_draggingProgress; }
    bool isDraggingVolume() const { return m_draggingVolume; }
    void updateDragWithScenePos(const QPointF& scenePos) {
        QPointF p = mapFromScene(scenePos);
        if (m_draggingProgress) {
            qreal r = (p.x() - m_progRectItemCoords.left()) / m_progRectItemCoords.width();
            r = std::clamp<qreal>(r, 0.0, 1.0);
            m_holdLastFrameAtEnd = false;
            seekToRatio(r);
            if (m_durationMs > 0) m_positionMs = static_cast<qint64>(r * m_durationMs);
            updateControlsLayout();
            update();
        } else if (m_draggingVolume) {
            qreal r = (p.x() - m_volumeRectItemCoords.left()) / m_volumeRectItemCoords.width();
            r = std::clamp<qreal>(r, 0.0, 1.0);
            if (m_audio) m_audio->setVolume(r);
            updateControlsLayout();
            update();
        }
    }
    void endDrag() {
        if (m_draggingProgress || m_draggingVolume) {
            m_draggingProgress = false;
            m_draggingVolume = false;
            ungrabMouse();
            updateControlsLayout();
            update();
        }
    }
    // Public helper to refresh overlay positions when the view transform changes
    void requestOverlayRelayout() { updateControlsLayout(); }
    
    // Phase 1: Performance tuning methods
    void setFrameProcessingBudget(int ms) { m_frameProcessBudgetMs = std::max(1, ms); }
    void setRepaintBudget(int ms) { m_repaintBudgetMs = std::max(1, ms); }
    void getFrameStats(int& received, int& processed, int& skipped) const {
        received = m_framesReceived;
        processed = m_framesProcessed;
        skipped = m_framesSkipped;
    }
    void getFrameStatsExtended(int& received, int& processed, int& skipped, int& dropped, int& conversionsStarted, int& conversionsCompleted) const {
        received = m_framesReceived;
        processed = m_framesProcessed;
        skipped = m_framesSkipped;
        dropped = m_framesDropped;
        conversionsStarted = m_conversionsStarted;
        conversionsCompleted = m_conversionsCompleted;
    }
    void resetFrameStats() { 
        m_framesReceived = m_framesProcessed = m_framesSkipped = 0; 
        m_framesDropped = m_conversionsStarted = m_conversionsCompleted = 0;
        m_frameSerial = m_lastProcessedSerial = 0;
    }
    
    // Phase 2: Async frame conversion callback
    void onFrameConversionComplete(QImage convertedImage, quint64 serial) {
        if (serial <= m_lastProcessedSerial) {
            ++m_framesDropped;
            return; // Older frame, discard
        }
        
        m_lastProcessedSerial = serial;
        m_lastFrameImage = std::move(convertedImage);
        ++m_conversionsCompleted;
        m_conversionBusy.store(false);
        
        // Debug: Log successful conversion
        if (m_conversionsCompleted % 30 == 0) {
            qDebug() << "Frame conversion completed, serial:" << serial << "thread:" << QThread::currentThread();
        }
        
        // Check if there's a newer pending frame to process
        {
            QMutexLocker locker(&m_frameMutex);
            if (m_pendingFrame.isValid() && !m_conversionBusy.exchange(true)) {
                quint64 newSerial = ++m_frameSerial;
                auto* worker = new FrameConversionWorker(this, m_pendingFrame, newSerial);
                QThreadPool::globalInstance()->start(worker);
                ++m_conversionsStarted;
                m_pendingFrame = QVideoFrame(); // Clear pending
            }
        }
        
        // Throttled repaint - direct update (main thread)
        if (shouldRepaint()) {
            m_lastRepaintMs = QDateTime::currentMSecsSinceEpoch();
            update();
        }
    }
    
    // Expose a helper for view-level control handling
    bool handleControlsPressAtItemPos(const QPointF& itemPos) {
        if (!isSelected()) return false;
        if (m_controlsLockedUntilReady) return false;
        // Play button
    if (m_playBtnRectItemCoords.contains(itemPos)) { m_holdLastFrameAtEnd = false; togglePlayPause(); return true; }
        // Stop
    if (m_stopBtnRectItemCoords.contains(itemPos)) { m_holdLastFrameAtEnd = false; stopToBeginning(); return true; }
        // Repeat
        if (m_repeatBtnRectItemCoords.contains(itemPos)) { toggleRepeat(); return true; }
        // Mute
        if (m_muteBtnRectItemCoords.contains(itemPos)) { toggleMute(); return true; }
        // Progress bar
        if (m_progRectItemCoords.contains(itemPos)) {
            qreal r = (itemPos.x() - m_progRectItemCoords.left()) / m_progRectItemCoords.width();
            m_holdLastFrameAtEnd = false;
            seekToRatio(r);
            // Begin drag-to-seek
            m_draggingProgress = true;
            grabMouse();
            return true;
        }
        // Volume slider
        if (m_volumeRectItemCoords.contains(itemPos)) {
            qreal r = (itemPos.x() - m_volumeRectItemCoords.left()) / m_volumeRectItemCoords.width();
            r = std::clamp<qreal>(r, 0.0, 1.0);
            if (m_audio) m_audio->setVolume(r);
            updateControlsLayout();
            // Begin drag-to-volume
            m_draggingVolume = true;
            grabMouse();
            return true;
        }
        return false;
    }
    void paint(QPainter* painter, const QStyleOptionGraphicsItem* option, QWidget* widget) override {
        Q_UNUSED(option); Q_UNUSED(widget);
    // Painting should not mutate other items; layout is updated on relevant events
        // Draw current frame; otherwise if available, draw poster image; avoid black placeholder to prevent flicker
        QRectF br(0,0, baseWidth(), baseHeight());
        auto fitRect = [](const QRectF& bounds, const QSize& imgSz) -> QRectF {
            if (bounds.isEmpty() || imgSz.isEmpty()) return bounds;
            const qreal brW = bounds.width();
            const qreal brH = bounds.height();
            const qreal imgW = imgSz.width();
            const qreal imgH = imgSz.height();
            if (imgW <= 0 || imgH <= 0) return bounds;
            const qreal brAR = brW / brH;
            const qreal imgAR = imgW / imgH;
            if (imgAR > brAR) {
                // image is wider; fit width
                const qreal h = brW / imgAR;
                return QRectF(bounds.left(), bounds.top() + (brH - h)/2.0, brW, h);
            } else {
                // image is taller; fit height
                const qreal w = brH * imgAR;
                return QRectF(bounds.left() + (brW - w)/2.0, bounds.top(), w, brH);
            }
        };
        if (!m_lastFrameImage.isNull()) {
            QRectF dst = fitRect(br, m_lastFrameImage.size());
            painter->drawImage(dst, m_lastFrameImage);
        } else if (m_lastFrame.isValid()) {
            // One-time fallback if cache not yet populated
            QImage img = m_lastFrame.toImage();
            if (!img.isNull()) {
                QRectF dst = fitRect(br, img.size());
                painter->drawImage(dst, img);
            } else if (m_posterImageSet && !m_posterImage.isNull()) {
                QRectF dst = fitRect(br, m_posterImage.size());
                painter->drawImage(dst, m_posterImage);
            }
        } else if (m_posterImageSet && !m_posterImage.isNull()) {
            QRectF dst = fitRect(br, m_posterImage.size());
            painter->drawImage(dst, m_posterImage);
        }
    // Pause icon visibility is controlled elsewhere; avoid layout work during paint
        // Selection/handles and label
        paintSelectionAndLabel(painter);
    }
    QRectF boundingRect() const override {
        QRectF br(0,0, baseWidth(), baseHeight());
        // Only extend for controls when selected and unlocked
        if (isSelected() && !m_controlsLockedUntilReady) {
            const int overrideH = ResizableMediaBase::getHeightOfMediaOverlaysPx();
            // Match label background height exactly when auto
            const qreal labelBgH = (m_labelBg ? m_labelBg->rect().height() : 0.0);
            const int padYpx = 4;
            const int fallbackH = (m_labelText ? static_cast<int>(std::round(m_labelText->boundingRect().height())) + 2*padYpx : 24);
            const int rowH = (overrideH > 0) ? overrideH : (labelBgH > 0 ? static_cast<int>(std::round(labelBgH)) : fallbackH);
            const int gapPx = 8; // outer gap (media-to-controls) and inner gap (between rows)
            qreal extra = toItemLengthFromPixels(rowH * 2 + 2 * gapPx); // two rows + outer+inner gap
            br.setHeight(br.height() + extra);
        }
        if (isSelected()) {
            qreal pad = toItemLengthFromPixels(m_selectionSize) / 2.0;
            br = br.adjusted(-pad, -pad, pad, pad);
        }
        return br;
    }
    QPainterPath shape() const override {
        QPainterPath p; p.addRect(QRectF(0,0, baseWidth(), baseHeight()));
        // controls clickable areas only when selected and unlocked
        if (isSelected() && !m_controlsLockedUntilReady) {
            if (!m_playBtnRectItemCoords.isNull()) p.addRect(m_playBtnRectItemCoords);
            if (!m_stopBtnRectItemCoords.isNull()) p.addRect(m_stopBtnRectItemCoords);
            if (!m_repeatBtnRectItemCoords.isNull()) p.addRect(m_repeatBtnRectItemCoords);
            if (!m_muteBtnRectItemCoords.isNull()) p.addRect(m_muteBtnRectItemCoords);
            if (!m_progRectItemCoords.isNull()) p.addRect(m_progRectItemCoords);
            if (!m_volumeRectItemCoords.isNull()) p.addRect(m_volumeRectItemCoords);
        }
        if (isSelected()) {
            const qreal s = toItemLengthFromPixels(m_selectionSize);
            QRectF br(0,0, baseWidth(), baseHeight());
            p.addRect(QRectF(br.topLeft() - QPointF(s/2,s/2), QSizeF(s,s)));
            p.addRect(QRectF(QPointF(br.right(), br.top()) - QPointF(s/2,s/2), QSizeF(s,s)));
            p.addRect(QRectF(QPointF(br.left(), br.bottom()) - QPointF(s/2,s/2), QSizeF(s,s)));
            p.addRect(QRectF(br.bottomRight() - QPointF(s/2,s/2), QSizeF(s,s)));
        }
        return p;
    }
    void mousePressEvent(QGraphicsSceneMouseEvent* event) override {
        // Translate to base for resize first
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
    // Controls interactions using last computed rects (only when selected and unlocked)
    if (!m_controlsLockedUntilReady && isSelected() && m_playBtnRectItemCoords.contains(event->pos())) {
            togglePlayPause();
            event->accept();
            return;
        }
    if (!m_controlsLockedUntilReady && isSelected() && m_stopBtnRectItemCoords.contains(event->pos())) { stopToBeginning(); event->accept(); return; }
    if (!m_controlsLockedUntilReady && isSelected() && m_repeatBtnRectItemCoords.contains(event->pos())) { toggleRepeat(); event->accept(); return; }
    if (!m_controlsLockedUntilReady && isSelected() && m_muteBtnRectItemCoords.contains(event->pos())) { toggleMute(); event->accept(); return; }
    if (!m_controlsLockedUntilReady && isSelected() && m_progRectItemCoords.contains(event->pos())) {
            qreal r = (event->pos().x() - m_progRectItemCoords.left()) / m_progRectItemCoords.width();
            // Prevent timer from fighting initial press update
            m_seeking = true;
            if (m_progressTimer) m_progressTimer->stop();
            seekToRatio(r);
            m_draggingProgress = true;
            grabMouse();
            event->accept();
            return;
        }
    if (!m_controlsLockedUntilReady && isSelected() && m_volumeRectItemCoords.contains(event->pos())) {
            qreal r = (event->pos().x() - m_volumeRectItemCoords.left()) / m_volumeRectItemCoords.width();
            r = std::clamp<qreal>(r, 0.0, 1.0);
            if (m_audio) m_audio->setVolume(r);
            updateControlsLayout();
            m_draggingVolume = true;
            grabMouse();
            event->accept();
            return;
        }
        ResizableMediaBase::mousePressEvent(event);
    }
    void mouseDoubleClickEvent(QGraphicsSceneMouseEvent* event) override {
        // Treat double-clicks on controls the same as single clicks; keep selection
    if (isSelected() && !m_controlsLockedUntilReady) {
            if (m_playBtnRectItemCoords.contains(event->pos())) {
                togglePlayPause();
                event->accept();
                return;
            }
            if (m_stopBtnRectItemCoords.contains(event->pos())) {
                stopToBeginning();
                event->accept();
                return;
            }
            if (m_repeatBtnRectItemCoords.contains(event->pos())) {
                toggleRepeat();
                event->accept();
                return;
            }
            if (m_muteBtnRectItemCoords.contains(event->pos())) {
                toggleMute();
                event->accept();
                return;
            }
            if (m_progRectItemCoords.contains(event->pos())) {
                qreal r = (event->pos().x() - m_progRectItemCoords.left()) / m_progRectItemCoords.width();
                seekToRatio(r);
                event->accept();
                return;
            }
            if (m_volumeRectItemCoords.contains(event->pos())) {
                qreal r = (event->pos().x() - m_volumeRectItemCoords.left()) / m_volumeRectItemCoords.width();
                r = std::clamp<qreal>(r, 0.0, 1.0);
                if (m_audio) m_audio->setVolume(r);
                event->accept();
                return;
            }
        }
        ResizableMediaBase::mouseDoubleClickEvent(event);
    }
protected:
    QVariant itemChange(GraphicsItemChange change, const QVariant &value) override {
        if (change == ItemSceneChange) {
            // When removed from a scene, temporarily hide overlay to avoid dangling in old scene
            if (value.value<QGraphicsScene*>() == nullptr) {
                if (m_controlsBg) m_controlsBg->setVisible(false);
            }
        }
        if (change == ItemSceneHasChanged) {
            // Ensure controls overlay belongs to the new scene
            if (scene() && m_controlsBg && m_controlsBg->scene() != scene()) {
                if (m_controlsBg->scene()) m_controlsBg->scene()->removeItem(m_controlsBg);
                scene()->addItem(m_controlsBg);
            }
        }
        if (change == ItemSelectedChange) {
            const bool willBeSelected = value.toBool();
            // Prepare for geometry change since bounding rect depends on selection
            prepareGeometryChange();
            setControlsVisible(willBeSelected);
        }
        if (change == ItemSelectedHasChanged) {
            // Recompute layout after selection toggles
            updateControlsLayout();
        }
        if (change == ItemPositionHasChanged || change == ItemTransformHasChanged) {
            // Keep overlay glued while moving/resizing from any handle
            updateControlsLayout();
        }
        return ResizableMediaBase::itemChange(change, value);
    }
    void onInteractiveGeometryChanged() override {
        // Keep both label and controls overlays glued during interactive resize
        updateLabelLayout();
        updateControlsLayout();
        update();
    }

    void mouseMoveEvent(QGraphicsSceneMouseEvent* event) override {
        // Allow base to manage resize drag when active
        if (m_activeHandle != None) { ResizableMediaBase::mouseMoveEvent(event); return; }
        if (event->buttons() & Qt::LeftButton) {
            if (m_draggingProgress) {
                qreal r = (event->pos().x() - m_progRectItemCoords.left()) / m_progRectItemCoords.width();
                // Clamp and update both the player and local UI state for immediate feedback
                r = std::clamp<qreal>(r, 0.0, 1.0);
                seekToRatio(r);
                updateControlsLayout();
                update();
                event->accept();
                return;
            }
            if (m_draggingVolume) {
                qreal r = (event->pos().x() - m_volumeRectItemCoords.left()) / m_volumeRectItemCoords.width();
                r = std::clamp<qreal>(r, 0.0, 1.0);
                if (m_audio) m_audio->setVolume(r);
                updateControlsLayout();
                update();
                event->accept();
                return;
            }
        }
        ResizableMediaBase::mouseMoveEvent(event);
    }

    void mouseReleaseEvent(QGraphicsSceneMouseEvent* event) override {
        if (m_draggingProgress || m_draggingVolume) {
            m_draggingProgress = false;
            m_draggingVolume = false;
            ungrabMouse();
            // Allow timer to resume after a beat so backend position has caught up
            QTimer::singleShot(30, [this]() {
                m_seeking = false;
                if (m_progressTimer && m_player && m_player->playbackState() == QMediaPlayer::PlayingState)
                    m_progressTimer->start();
            });
            event->accept();
            return;
        }
        ResizableMediaBase::mouseReleaseEvent(event);
    }
private:
    void maybeAdoptFrameSize(const QVideoFrame& f) {
        if (m_adoptedSize) return;
        if (!f.isValid()) return;
        QImage img = f.toImage();
        if (img.isNull()) return;
        const QSize sz = img.size();
        if (sz.isEmpty()) return;
        adoptBaseSize(sz);
    }
    void adoptBaseSize(const QSize& sz) {
        if (m_adoptedSize) return;
        if (sz.isEmpty()) return;
        m_adoptedSize = true;
        // Preserve center in scene while changing base size and scale
        const QRectF oldRect(0,0, baseWidth(), baseHeight());
        const QPointF oldCenterScene = mapToScene(oldRect.center());
        prepareGeometryChange();
    m_baseSize = sz;
        setScale(m_initialScaleFactor);
        const QPointF newTopLeftScene = oldCenterScene - QPointF(sz.width() * m_initialScaleFactor / 2.0,
                                                                 sz.height() * m_initialScaleFactor / 2.0);
        setPos(newTopLeftScene);
        update();
    }
    void setControlsVisible(bool show) {
        const bool allow = show && !m_controlsLockedUntilReady;
        auto setChildrenVisible = [this](bool vis){
            if (m_playBtnRectItem) m_playBtnRectItem->setVisible(vis);
            if (m_playIcon) m_playIcon->setVisible(vis && !(m_player && m_player->playbackState() == QMediaPlayer::PlayingState));
            if (m_pauseIcon) m_pauseIcon->setVisible(vis &&  (m_player && m_player->playbackState() == QMediaPlayer::PlayingState));
            if (m_stopBtnRectItem) m_stopBtnRectItem->setVisible(vis);
            if (m_stopIcon) m_stopIcon->setVisible(vis);
            if (m_repeatBtnRectItem) m_repeatBtnRectItem->setVisible(vis);
            if (m_repeatIcon) m_repeatIcon->setVisible(vis);
            if (m_muteBtnRectItem) m_muteBtnRectItem->setVisible(vis);
            if (m_muteIcon) m_muteIcon->setVisible(vis);
            if (m_muteSlashIcon) m_muteSlashIcon->setVisible(vis && m_audio && m_audio->isMuted());
            if (m_volumeBgRectItem) m_volumeBgRectItem->setVisible(vis);
            if (m_volumeFillRectItem) m_volumeFillRectItem->setVisible(vis);
            if (m_progressBgRectItem) m_progressBgRectItem->setVisible(vis);
            if (m_progressFillRectItem) m_progressFillRectItem->setVisible(vis);
        };
    if (!m_controlsBg) return;
        if (allow) {
            m_controlsBg->setVisible(true);
            m_controlsBg->setZValue(12000.0); // above everything
            setChildrenVisible(true);
            // Fade only the first time the controls become visible after unlock
            if (!m_controlsDidInitialFade) {
                if (!m_controlsFadeAnim) {
                    m_controlsFadeAnim = new QVariantAnimation();
                    m_controlsFadeAnim->setEasingCurve(QEasingCurve::OutCubic);
                    QObject::connect(m_controlsFadeAnim, &QVariantAnimation::valueChanged, [this](const QVariant& v){
                        if (m_controlsBg) m_controlsBg->setOpacity(v.toReal());
                    });
                }
                m_controlsFadeAnim->stop();
                m_controlsFadeAnim->setDuration(m_controlsFadeMs);
                m_controlsFadeAnim->setStartValue(0.0);
                m_controlsFadeAnim->setEndValue(1.0);
                m_controlsDidInitialFade = true; // ensure subsequent shows are instant
                m_controlsFadeAnim->start();
            } else {
                if (m_controlsFadeAnim) m_controlsFadeAnim->stop();
                m_controlsBg->setOpacity(1.0);
            }
        } else {
            if (m_controlsFadeAnim) m_controlsFadeAnim->stop();
            m_controlsBg->setOpacity(0.0);
            m_controlsBg->setVisible(false);
            setChildrenVisible(false);
            // Keep m_controlsDidInitialFade as-is so next selection show is instant
        }
    }
    void updateControlsLayout() {
        if (!scene() || scene()->views().isEmpty()) return;
        if (!isSelected()) return; // layout only needed when visible
        if (m_controlsLockedUntilReady) return; // defer until video is ready
        QGraphicsView* v = scene()->views().first();
        // Heights based on filename label height, or explicit override
        const int padYpx = 4;
        const int gapPx = 8;
        const int overrideH = ResizableMediaBase::getHeightOfMediaOverlaysPx();
        const qreal labelBgH = (m_labelBg ? m_labelBg->rect().height() : 0.0);
        const int fallbackH = (m_labelText ? static_cast<int>(std::round(m_labelText->boundingRect().height())) + 2*padYpx : 24);
        const int rowHpx = (overrideH > 0) ? overrideH : (labelBgH > 0 ? static_cast<int>(std::round(labelBgH)) : fallbackH);
        const int totalWpx = 260; // absolute width for controls overlay
        const int playWpx = rowHpx; // square button width equals height
        // Top row: play, stop, repeat, mute, then volume slider filling the rest, with gaps between buttons
        const int stopWpx = rowHpx;
        const int repeatWpx = rowHpx;
        const int muteWpx = rowHpx;
        const int buttonGapPx = gapPx; // horizontal gap between buttons equals outer/inner gap
        const int volumeWpx = std::max(0, totalWpx - (playWpx + stopWpx + repeatWpx + muteWpx) - buttonGapPx * 4);
        const int progWpx = totalWpx; // progress spans full width on second row

        // Active-state tinting: slightly blue-tinted version of label background
        auto baseBrush = (m_labelBg ? m_labelBg->brush() : QBrush(QColor(0,0,0,160)));
        auto blendColor = [](const QColor& a, const QColor& b, qreal t){
            auto clamp255 = [](int v){ return std::max(0, std::min(255, v)); };
            int r = clamp255(static_cast<int>(std::round(a.red()   * (1.0 - t) + b.red()   * t)));
            int g = clamp255(static_cast<int>(std::round(a.green() * (1.0 - t) + b.green() * t)));
            int bch = clamp255(static_cast<int>(std::round(a.blue()  * (1.0 - t) + b.blue()  * t)));
            return QColor(r, g, bch, a.alpha());
        };
        const QColor accentBlue(74, 144, 226, 255);
        const qreal tintStrength = 0.33; // subtle blue
        QColor baseColor = baseBrush.color().isValid() ? baseBrush.color() : QColor(0,0,0,160);
        QBrush activeBrush(blendColor(baseColor, accentBlue, tintStrength));

        // Anchor: compute top-left of controls in item coordinates by using view-space mapping
        // Use the same approach as filename label for consistent behavior
        QPointF bottomCenterItem(baseWidth() / 2.0, baseHeight());
    QPointF bottomCenterScene = mapToScene(bottomCenterItem);
    // Map scene->viewport with full precision (avoid QPoint rounding)
    QPointF bottomCenterView = v->viewportTransform().map(bottomCenterScene);
    // Desired top-left of controls in viewport (pixels) - position below video with gap
    QPointF ctrlTopLeftView = bottomCenterView + QPointF(-totalWpx / 2.0, gapPx);
    // Map back to item coords using inverse viewport transform (preserve sub-pixel precision)
    QPointF ctrlTopLeftScene = v->viewportTransform().inverted().map(ctrlTopLeftView);
        QPointF ctrlTopLeftItem = mapFromScene(ctrlTopLeftScene);

        // Background tall enough to hold two rows (play above, progress below) with inner gap
        if (m_controlsBg) {
            m_controlsBg->setRect(0, 0, totalWpx, rowHpx * 2 + gapPx);
            // Controls are a top-level scene item; place in scene coords
            m_controlsBg->setPos(ctrlTopLeftScene);
        }

        // Compute x-positions with uniform gaps (in the local px space of m_controlsBg)
        const qreal x0 = 0;
        const qreal x1 = x0 + playWpx + buttonGapPx;
        const qreal x2 = x1 + stopWpx + buttonGapPx;
        const qreal x3 = x2 + repeatWpx + buttonGapPx;
        const qreal x4 = x3 + muteWpx + buttonGapPx;

        if (m_playBtnRectItem) {
            m_playBtnRectItem->setRect(0, 0, playWpx, rowHpx); m_playBtnRectItem->setPos(x0, 0);
            m_playBtnRectItem->setRadius(ResizableMediaBase::getCornerRadiusOfMediaOverlaysPx());
            m_playBtnRectItem->setBrush(baseBrush);
        }
        if (m_stopBtnRectItem) {
            m_stopBtnRectItem->setRect(0, 0, stopWpx, rowHpx); m_stopBtnRectItem->setPos(x1, 0);
            m_stopBtnRectItem->setRadius(ResizableMediaBase::getCornerRadiusOfMediaOverlaysPx());
            m_stopBtnRectItem->setBrush(baseBrush);
        }
        if (m_repeatBtnRectItem) {
            m_repeatBtnRectItem->setRect(0, 0, repeatWpx, rowHpx); m_repeatBtnRectItem->setPos(x2, 0);
            m_repeatBtnRectItem->setRadius(ResizableMediaBase::getCornerRadiusOfMediaOverlaysPx());
            m_repeatBtnRectItem->setBrush(m_repeatEnabled ? activeBrush : baseBrush);
        }
        if (m_muteBtnRectItem) {
            m_muteBtnRectItem->setRect(0, 0, muteWpx, rowHpx); m_muteBtnRectItem->setPos(x3, 0);
            m_muteBtnRectItem->setRadius(ResizableMediaBase::getCornerRadiusOfMediaOverlaysPx());
            bool muted = m_audio && m_audio->isMuted();
            m_muteBtnRectItem->setBrush(muted ? activeBrush : baseBrush);
        }
        if (m_volumeBgRectItem) { m_volumeBgRectItem->setRect(0, 0, volumeWpx, rowHpx); m_volumeBgRectItem->setPos(x4, 0); }
        if (m_volumeFillRectItem) {
            const qreal margin = 2.0;
            qreal vol = m_audio ? std::clamp<qreal>(m_audio->volume(), 0.0, 1.0) : 0.0;
            const qreal innerW = std::max<qreal>(0.0, volumeWpx - 2*margin);
            m_volumeFillRectItem->setRect(margin, margin, innerW * vol, rowHpx - 2*margin);
        }
        if (m_progressBgRectItem) {
            // Second row, spans left to right under play button, with a gap
            m_progressBgRectItem->setRect(0, 0, progWpx, rowHpx);
            m_progressBgRectItem->setPos(0, rowHpx + gapPx);
        }
        if (m_progressFillRectItem) {
            // Use the smoothly animated ratio instead of direct calculation
            if (!m_draggingProgress) {
                updateProgressBar(); // This uses m_smoothProgressRatio
            } else {
                qreal ratio = (m_durationMs > 0) ? (static_cast<qreal>(m_positionMs) / m_durationMs) : 0.0;
                ratio = std::clamp<qreal>(ratio, 0.0, 1.0);
                const qreal margin = 2.0;
                m_progressFillRectItem->setRect(margin, margin, (progWpx - 2*margin) * ratio, rowHpx - 2*margin);
            }
        }

        // Update SVG icon placement, toggle variants (target sizes are in local px of bg/buttons)
        auto placeSvg = [](QGraphicsSvgItem* svg, qreal targetW, qreal targetH) {
            if (!svg) return;
            QSizeF nat = svg->renderer() ? svg->renderer()->defaultSize() : QSizeF(24,24);
            if (nat.width() <= 0 || nat.height() <= 0) nat = svg->boundingRect().size();
            if (nat.width() <= 0 || nat.height() <= 0) nat = QSizeF(24,24);
            const qreal scale = std::min(targetW / nat.width(), targetH / nat.height()) * 0.6; // 60% of button area
            svg->setScale(scale);
            const qreal x = (targetW - nat.width()*scale) / 2.0;
            const qreal y = (targetH - nat.height()*scale) / 2.0;
            svg->setPos(x, y);
        };
        bool isPlaying = (m_player && m_player->playbackState() == QMediaPlayer::PlayingState);
        if (m_holdLastFrameAtEnd) isPlaying = false;
        if (!m_repeatEnabled && m_durationMs > 0 && (m_positionMs + 30 >= m_durationMs)) isPlaying = false;
        if (m_playIcon && m_pauseIcon) {
            m_playIcon->setVisible(!isPlaying);
            m_pauseIcon->setVisible(isPlaying);
            placeSvg(m_playIcon, playWpx, rowHpx);
            placeSvg(m_pauseIcon, playWpx, rowHpx);
        }
        placeSvg(m_stopIcon, stopWpx, rowHpx);
        placeSvg(m_repeatIcon, repeatWpx, rowHpx);
        bool muted = m_audio && m_audio->isMuted();
        if (m_muteIcon && m_muteSlashIcon) {
            m_muteIcon->setVisible(!muted);
            m_muteSlashIcon->setVisible(muted);
            placeSvg(m_muteIcon, muteWpx, rowHpx);
            placeSvg(m_muteSlashIcon, muteWpx, rowHpx);
        }

        // Store item-space rects for hit testing (convert px extents to item units)
        const qreal rowHItem = toItemLengthFromPixels(rowHpx);
        const qreal playWItem = toItemLengthFromPixels(playWpx);
        const qreal stopWItem = toItemLengthFromPixels(stopWpx);
        const qreal repeatWItem = toItemLengthFromPixels(repeatWpx);
        const qreal muteWItem = toItemLengthFromPixels(muteWpx);
        const qreal volumeWItem = toItemLengthFromPixels(volumeWpx);
        const qreal progWItem = toItemLengthFromPixels(progWpx);
        const qreal gapItem = toItemLengthFromPixels(gapPx);
        const qreal x0Item = toItemLengthFromPixels(static_cast<int>(std::round(x0)));
        const qreal x1Item = toItemLengthFromPixels(static_cast<int>(std::round(x1)));
        const qreal x2Item = toItemLengthFromPixels(static_cast<int>(std::round(x2)));
        const qreal x3Item = toItemLengthFromPixels(static_cast<int>(std::round(x3)));
        const qreal x4Item = toItemLengthFromPixels(static_cast<int>(std::round(x4)));
        m_playBtnRectItemCoords   = QRectF(ctrlTopLeftItem.x() + x0Item, ctrlTopLeftItem.y() + 0,       playWItem,   rowHItem);
        m_stopBtnRectItemCoords   = QRectF(ctrlTopLeftItem.x() + x1Item, ctrlTopLeftItem.y() + 0,       stopWItem,   rowHItem);
        m_repeatBtnRectItemCoords = QRectF(ctrlTopLeftItem.x() + x2Item, ctrlTopLeftItem.y() + 0,       repeatWItem, rowHItem);
        m_muteBtnRectItemCoords   = QRectF(ctrlTopLeftItem.x() + x3Item, ctrlTopLeftItem.y() + 0,       muteWItem,   rowHItem);
        m_volumeRectItemCoords    = QRectF(ctrlTopLeftItem.x() + x4Item, ctrlTopLeftItem.y() + 0,       volumeWItem, rowHItem);
        m_progRectItemCoords      = QRectF(ctrlTopLeftItem.x() + 0,      ctrlTopLeftItem.y() + rowHItem + gapItem,
                                           progWItem, rowHItem);
    }
    qreal baseWidth() const { return static_cast<qreal>(m_baseSize.width()); }
    qreal baseHeight() const { return static_cast<qreal>(m_baseSize.height()); }
    QMediaPlayer* m_player = nullptr;
    QAudioOutput* m_audio = nullptr;
    QVideoSink* m_sink = nullptr;
    QVideoFrame m_lastFrame;
    QImage m_lastFrameImage;
    qint64 m_durationMs = 0;
    qint64 m_positionMs = 0;
    // First-frame priming state
    bool m_primingFirstFrame = false;
    bool m_firstFramePrimed = false;
    bool m_savedMuted = false;
    // Optional poster image from metadata to avoid initial black frame
    QImage m_posterImage;
    bool m_posterImageSet = false;
    // Floating controls (absolute px)
    QGraphicsRectItem* m_controlsBg = nullptr;
    RoundedRectItem* m_playBtnRectItem = nullptr;
    QGraphicsSvgItem* m_playIcon = nullptr; // play triangle
    QGraphicsSvgItem* m_pauseIcon = nullptr; // pause bars
    RoundedRectItem* m_stopBtnRectItem = nullptr;
    QGraphicsSvgItem* m_stopIcon = nullptr;
    RoundedRectItem* m_repeatBtnRectItem = nullptr;
    QGraphicsSvgItem* m_repeatIcon = nullptr; // currently unused visual, keep for symmetry
    RoundedRectItem* m_muteBtnRectItem = nullptr;
    QGraphicsSvgItem* m_muteIcon = nullptr; // volume on
    QGraphicsSvgItem* m_muteSlashIcon = nullptr; // volume off
    QGraphicsRectItem* m_volumeBgRectItem = nullptr;
    QGraphicsRectItem* m_volumeFillRectItem = nullptr;
    QGraphicsRectItem* m_progressBgRectItem = nullptr;
    QGraphicsRectItem* m_progressFillRectItem = nullptr;
    bool m_adoptedSize = false;
    qreal m_initialScaleFactor = 1.0;
    // Cached item-space rects for hit-testing
    QRectF m_playBtnRectItemCoords;
    QRectF m_stopBtnRectItemCoords;
    QRectF m_repeatBtnRectItemCoords;
    QRectF m_muteBtnRectItemCoords;
    QRectF m_volumeRectItemCoords;
    QRectF m_progRectItemCoords;
    bool m_repeatEnabled = false;
    // Drag state for sliders
    bool m_draggingProgress = false;
    bool m_draggingVolume = false;
    // Hold last frame after EndOfMedia until next user action
    bool m_holdLastFrameAtEnd = false;
    // Smooth progress updates
    QTimer* m_progressTimer = nullptr;
    qreal m_smoothProgressRatio = 0.0;
    // Guard to suppress timer updates while a seek is settling
    bool m_seeking = false;
    // Hide/disable controls until first frame is primed
    bool m_controlsLockedUntilReady = true;
    // Controls fade animation config/state
    int m_controlsFadeMs = 140;
    QVariantAnimation* m_controlsFadeAnim = nullptr;
    bool m_controlsDidInitialFade = false;
    
    // Phase 1: Frame processing throttling and instrumentation
    qint64 m_lastFrameProcessMs = 0;
    qint64 m_lastRepaintMs = 0;
    int m_frameProcessBudgetMs = 16; // ~60fps max processing rate
    int m_repaintBudgetMs = 16; // ~60fps max repaint rate
    // Debug counters
    mutable int m_framesReceived = 0;
    mutable int m_framesProcessed = 0;
    mutable int m_framesSkipped = 0;
    
    // Phase 2: Async frame conversion with frame dropping
    std::atomic<bool> m_conversionBusy{false};
    QVideoFrame m_pendingFrame;
    QMutex m_frameMutex;
    std::atomic<quint64> m_frameSerial{0};
    quint64 m_lastProcessedSerial = 0;
    mutable int m_framesDropped = 0;
    mutable int m_conversionsStarted = 0;
    mutable int m_conversionsCompleted = 0;

private:
    // Phase 1: Visibility and frame processing helpers
    bool isVisibleInAnyView() const {
        if (!scene() || scene()->views().isEmpty()) return false;
        auto *view = scene()->views().first();
        if (!view || !view->viewport()) return false;
        
        QRectF viewportRect = view->viewport()->rect();
        QRectF sceneRect = view->mapToScene(viewportRect.toRect()).boundingRect();
        QRectF itemSceneRect = mapToScene(boundingRect()).boundingRect();
        
        return sceneRect.intersects(itemSceneRect);
    }
    
    bool shouldProcessFrame() const {
        const qint64 now = QDateTime::currentMSecsSinceEpoch();
        return (now - m_lastFrameProcessMs) >= m_frameProcessBudgetMs;
    }
    
    bool shouldRepaint() const {
        const qint64 now = QDateTime::currentMSecsSinceEpoch();
        return (now - m_lastRepaintMs) >= m_repaintBudgetMs;
    }
    
    void logFrameStats() const {
        // Log stats every 120 frames when multiple videos might be playing
        if (m_framesReceived > 0 && m_framesReceived % 120 == 0) {
            const float processRatio = float(m_framesProcessed) / float(m_framesReceived);
            const float skipRatio = float(m_framesSkipped) / float(m_framesReceived);
            const float dropRatio = float(m_framesDropped) / float(m_framesReceived);
            const float conversionEfficiency = (m_conversionsStarted > 0) ? 
                float(m_conversionsCompleted) / float(m_conversionsStarted) : 0.0f;
            
            qDebug() << "VideoItem frame stats: received=" << m_framesReceived 
                     << "processed=" << m_framesProcessed << "(" << (processRatio * 100.0f) << "%)"
                     << "skipped=" << m_framesSkipped << "(" << (skipRatio * 100.0f) << "%)"
                     << "dropped=" << m_framesDropped << "(" << (dropRatio * 100.0f) << "%)"
                     << "conversions=" << m_conversionsStarted << "/" << m_conversionsCompleted 
                     << "(" << (conversionEfficiency * 100.0f) << "% efficiency)";
        }
    }
    
    void updateProgressBar() {
        if (!m_progressFillRectItem) return;
        // Use animated ratio instead of direct position calculation
        const qreal margin = 2.0;
        const QRectF bgRect = m_progressBgRectItem ? m_progressBgRectItem->rect() : QRectF();
        const qreal progWpx = bgRect.width();
        const qreal rowH = bgRect.height();
        m_progressFillRectItem->setRect(margin, margin, (progWpx - 2*margin) * m_smoothProgressRatio, rowH - 2*margin);
    }
};

QSet<uintptr_t> FrameConversionWorker::s_activeItems;

// Phase 2: FrameConversionWorker implementation (after ResizableVideoItem is fully defined)
void FrameConversionWorker::run() {
    if (!m_frame.isValid()) return;
    
    // Do the expensive conversion off the main thread
    QImage img = m_frame.toImage();
    if (img.isNull()) return;
    
    QImage converted = img.convertToFormat(QImage::Format_ARGB32_Premultiplied);
    if (converted.isNull()) return;
    
    // Capture by value for the callback to avoid lifetime issues
    uintptr_t itemPtr = m_itemPtr;
    quint64 serial = m_serial;
    
    // Post result back to main thread via queued connection
    QMetaObject::invokeMethod(qApp, [itemPtr, converted = std::move(converted), serial]() mutable {
        // Validate that the item still exists via static registry check
        if (s_activeItems.contains(itemPtr)) {
            auto* item = reinterpret_cast<ResizableVideoItem*>(itemPtr);
            item->onFrameConversionComplete(std::move(converted), serial);
        }
    }, Qt::QueuedConnection);
}

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent)
    , m_centralWidget(nullptr)
    , m_webSocketClient(new WebSocketClient(this)) // Initialize WebSocket client
    , m_statusUpdateTimer(new QTimer(this))
    , m_trayIcon(nullptr)
    , m_screenViewWidget(nullptr)
    , m_screenCanvas(nullptr)
    , m_ignoreSelectionChange(false)
    , m_displaySyncTimer(new QTimer(this))
    , m_canvasStack(new QStackedWidget(this)) // Initialize m_canvasStack
{
    // Check if system tray is available
    if (!QSystemTrayIcon::isSystemTrayAvailable()) {
        QMessageBox::critical(this, "System Tray",
                             "System tray is not available on this system.");
    }
    
    setupUI();
    setupMenuBar();
    setupSystemTray();
    setupVolumeMonitoring();
    
    // Connect WebSocket signals
    connect(m_webSocketClient, &WebSocketClient::connected, this, &MainWindow::onConnected);
    connect(m_webSocketClient, &WebSocketClient::disconnected, this, &MainWindow::onDisconnected);
    connect(m_webSocketClient, &WebSocketClient::clientListReceived, this, &MainWindow::onClientListReceived);
    connect(m_webSocketClient, &WebSocketClient::registrationConfirmed, this, &MainWindow::onRegistrationConfirmed);
    connect(m_webSocketClient, &WebSocketClient::screensInfoReceived, this, &MainWindow::onScreensInfoReceived);
    connect(m_webSocketClient, &WebSocketClient::watchStatusChanged, this, &MainWindow::onWatchStatusChanged);
    connect(m_webSocketClient, &WebSocketClient::dataRequestReceived, this, [this]() {
        // Server requested immediate state (on first watch or refresh)
        m_webSocketClient->sendStateSnapshot(getLocalScreenInfo(), getSystemVolumePercent());
    });
    
    // Setup status update timer
    m_statusUpdateTimer->setInterval(1000); // Update every second
    connect(m_statusUpdateTimer, &QTimer::timeout, this, &MainWindow::updateConnectionStatus);
    m_statusUpdateTimer->start();

    // Debounced display sync timer
    m_displaySyncTimer->setSingleShot(true);
    m_displaySyncTimer->setInterval(300); // debounce bursts of screen change signals
    connect(m_displaySyncTimer, &QTimer::timeout, this, [this]() {
        if (m_webSocketClient->isConnected() && m_isWatched) syncRegistration();
    });

    // Listen to display changes to keep server-side screen info up-to-date
    auto connectScreenSignals = [this](QScreen* s) {
        connect(s, &QScreen::geometryChanged, this, [this]() { m_displaySyncTimer->start(); });
        connect(s, &QScreen::availableGeometryChanged, this, [this]() { m_displaySyncTimer->start(); });
        connect(s, &QScreen::physicalDotsPerInchChanged, this, [this]() { m_displaySyncTimer->start(); });
        connect(s, &QScreen::primaryOrientationChanged, this, [this]() { m_displaySyncTimer->start(); });
    };
    for (QScreen* s : QGuiApplication::screens()) connectScreenSignals(s);
    connect(qApp, &QGuiApplication::screenAdded, this, [this, connectScreenSignals](QScreen* s) {
        connectScreenSignals(s);
        m_displaySyncTimer->start();
    });
    connect(qApp, &QGuiApplication::screenRemoved, this, [this](QScreen*) {
        m_displaySyncTimer->start();
    });

    // Smart reconnection system with exponential backoff
    m_reconnectTimer = new QTimer(this);
    m_reconnectTimer->setSingleShot(true);
    m_reconnectAttempts = 0;
    m_maxReconnectDelay = 60000; // Max 60 seconds between attempts
    connect(m_reconnectTimer, &QTimer::timeout, this, &MainWindow::attemptReconnect);

        // (cleaned) avoid duplicate hookups
#ifdef Q_OS_MACOS
    // Use a normal window on macOS so the title bar and traffic lights have standard size.
    // Avoid Qt::Tool/WA_MacAlwaysShowToolWindow which force a tiny utility-chrome window.
#endif
    
    // Initially disable UI until connected
    setUIEnabled(false);
    
    // Start minimized to tray and auto-connect
    hide();
    connectToServer();
}

void MainWindow::showScreenView(const ClientInfo& client) {
    qDebug() << "showScreenView called for client:" << client.getMachineName();
    // Update client name
    m_clientNameLabel->setText(QString("%1 (%2)").arg(client.getMachineName()).arg(client.getPlatform()));
    
    // Reset to spinner state but delay showing the spinner to avoid flicker
    if (m_canvasStack) m_canvasStack->setCurrentIndex(0);
    if (m_loadingSpinner) {
        m_loadingSpinner->stop();
        m_spinnerFade->stop();
        m_spinnerOpacity->setOpacity(0.0);
    }
    if (m_volumeIndicator) {
        m_volumeIndicator->hide();
        m_volumeFade->stop();
        m_volumeOpacity->setOpacity(0.0);
    }
    if (m_canvasFade) m_canvasFade->stop();
    if (m_canvasOpacity) m_canvasOpacity->setOpacity(0.0);
    
    if (m_loaderDelayTimer) {
        m_loaderDelayTimer->stop();
        // Prevent multiple connections across navigations
        QObject::disconnect(m_loaderDelayTimer, nullptr, nullptr, nullptr);
        connect(m_loaderDelayTimer, &QTimer::timeout, this, [this]() {
            if (!m_canvasStack || !m_loadingSpinner || !m_spinnerFade || !m_spinnerOpacity) return;
            m_canvasStack->setCurrentIndex(0);
            m_spinnerFade->stop();
            m_spinnerOpacity->setOpacity(0.0);
            m_loadingSpinner->start();
            // Spinner uses its own fade duration
            if (m_spinnerFade) m_spinnerFade->setDuration(m_loaderFadeDurationMs);
            m_spinnerFade->setStartValue(0.0);
            m_spinnerFade->setEndValue(1.0);
            m_spinnerFade->start();
        });
        m_loaderDelayTimer->start(m_loaderDelayMs);
    }
    if (m_screenCanvas) {
        m_screenCanvas->clearScreens();
    }
    
    // Request fresh screen info on demand
    if (!client.getId().isEmpty()) {
        m_webSocketClient->requestScreens(client.getId());
    }
    
    // Switch to screen view page
    m_stackedWidget->setCurrentWidget(m_screenViewWidget);
    // Show back button when on screen view
    if (m_backButton) m_backButton->show();
    
    // Start watching this client's screens for real-time updates
    if (m_webSocketClient && m_webSocketClient->isConnected()) {
        if (!m_watchedClientId.isEmpty() && m_watchedClientId != client.getId()) {
            m_webSocketClient->unwatchScreens(m_watchedClientId);
        }
        if (!client.getId().isEmpty()) {
            m_webSocketClient->watchScreens(client.getId());
            m_watchedClientId = client.getId();
        }
    }
    qDebug() << "Screen view now showing. Current widget index:" << m_stackedWidget->currentIndex();
}

void MainWindow::showClientListView() {
    qDebug() << "showClientListView called. Current widget index before:" << m_stackedWidget->currentIndex();
    
    // Switch to client list page
    m_stackedWidget->setCurrentWidget(m_clientListPage);
    // Hide back button when on client list
    if (m_backButton) m_backButton->hide();
    
    qDebug() << "Client list view now showing. Current widget index:" << m_stackedWidget->currentIndex();
    
    // Stop watching when leaving screen view
    if (m_webSocketClient && m_webSocketClient->isConnected() && !m_watchedClientId.isEmpty()) {
        m_webSocketClient->unwatchScreens(m_watchedClientId);
        m_watchedClientId.clear();
    }
    if (m_screenCanvas) m_screenCanvas->hideRemoteCursor();
    // Clear selection without triggering selection change event
    m_ignoreSelectionChange = true;
    m_clientListWidget->clearSelection();
    m_ignoreSelectionChange = false;
}

void MainWindow::onClientItemClicked(QListWidgetItem* item) {
    if (!item) return;
    int index = m_clientListWidget->row(item);
    if (index >= 0 && index < m_availableClients.size()) {
        const ClientInfo& client = m_availableClients[index];
        m_selectedClient = client;
        // Switch to screen view first with any cached info for immediate feedback
        showScreenView(client);
        // Then request fresh screens info on-demand
        if (m_webSocketClient && m_webSocketClient->isConnected()) {
            m_webSocketClient->requestScreens(client.getId());
        }
    }
}

void MainWindow::updateVolumeIndicator() {
    int vol = m_selectedClient.getVolumePercent();
    QString text;
    if (vol >= 0) {
        QString icon = (vol == 0) ? "🔇" : (vol < 34 ? "🔈" : (vol < 67 ? "🔉" : "🔊"));
        text = QString("%1 %2%").arg(icon).arg(vol);
    } else {
        text = QString("🔈 --");
    }
    m_volumeIndicator->setText(text);
}

void MainWindow::onBackToClientListClicked() {
    showClientListView();
}

void MainWindow::onSendMediaClicked() {
    // Placeholder implementation
    QMessageBox::information(this, "Send Media", 
        QString("Sending media to %1's screens...\n\nThis feature will be implemented in the next phase.")
        .arg(m_selectedClient.getMachineName()));
}



bool MainWindow::eventFilter(QObject* obj, QEvent* event) {
    // Swallow spacebar outside of the canvas to prevent accidental activations,
    // but let it reach the canvas for the recenter shortcut.
    if (event->type() == QEvent::KeyPress) {
        auto* keyEvent = static_cast<QKeyEvent*>(event);
        if (keyEvent->key() == Qt::Key_Space) {
            QWidget* w = qobject_cast<QWidget*>(obj);
            bool onCanvas = false;
            if (m_screenCanvas) {
                QWidget* canvasWidget = m_screenCanvas;
                onCanvas = (w == canvasWidget) || (w && canvasWidget->isAncestorOf(w));
            }
            if (!onCanvas) {
                return true; // consume outside the canvas
            } else {
                // Recenter shortcut on canvas regardless of exact focus widget
                m_screenCanvas->recenterWithMargin(33);
                return true; // consume
            }
        }
    }
    // Apply rounded clipping to canvas widgets so border radius is visible
    if (obj == m_canvasContainer || obj == m_canvasStack || (m_screenCanvas && obj == m_screenCanvas->viewport())) {
        if (event->type() == QEvent::Resize || event->type() == QEvent::Show) {
            if (QWidget* w = qobject_cast<QWidget*>(obj)) {
                const int r = 5; // match clients list radius
                QPainterPath path;
                path.addRoundedRect(w->rect(), r, r);
                QRegion mask(path.toFillPolygon().toPolygon());
                w->setMask(mask);
            }
        }
    }
    return QMainWindow::eventFilter(obj, event);
}

// ScreenCanvas implementation
ScreenCanvas::ScreenCanvas(QWidget* parent)
    : QGraphicsView(parent)
    , m_scene(new QGraphicsScene(this))
    , m_panning(false)
{
    setScene(m_scene);
    setDragMode(QGraphicsView::NoDrag);
    setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    // Use the application palette for consistent theming with the client list container
    const QBrush bgBrush = palette().brush(QPalette::Base);
    setBackgroundBrush(bgBrush);        // outside the scene rect
    if (m_scene) m_scene->setBackgroundBrush(bgBrush); // inside the scene rect
    setFrameShape(QFrame::NoFrame);
    setRenderHint(QPainter::Antialiasing);
    // Use manual anchoring logic for consistent behavior across platforms
    setTransformationAnchor(QGraphicsView::NoAnchor);
    // When the view is resized, keep the view-centered anchor (we also recenter explicitly on window resize)
    setResizeAnchor(QGraphicsView::AnchorViewCenter);
    // Enable drag & drop
    setAcceptDrops(true);
    
    // Enable mouse tracking for panning
    setMouseTracking(true);
    m_lastMousePos = QPoint(0, 0);
    // Pinch guard timer prevents two-finger scroll handling during active pinch
    m_nativePinchGuardTimer = new QTimer(this);
    m_nativePinchGuardTimer->setSingleShot(true);
    m_nativePinchGuardTimer->setInterval(60); // short grace period after last pinch delta
    connect(m_nativePinchGuardTimer, &QTimer::timeout, this, [this]() {
        m_nativePinchActive = false;
    });
    
    // Enable pinch gesture (except on macOS where we'll rely on NativeGesture to avoid double handling)
#ifndef Q_OS_MACOS
    grabGesture(Qt::PinchGesture);
#endif
    // Remote cursor overlay (hidden by default)
    m_remoteCursorDot = m_scene->addEllipse(0, 0, 10, 10, QPen(QColor(74, 144, 226)), QBrush(Qt::white));
    if (m_remoteCursorDot) {
        m_remoteCursorDot->setZValue(10000);
        m_remoteCursorDot->setVisible(false);
    }
}

void ScreenCanvas::setScreens(const QList<ScreenInfo>& screens) {
    m_screens = screens;
    clearScreens();
    createScreenItems();
    
    // Keep a large scene for free movement and center on screens
    const double LARGE_SCENE_SIZE = 100000.0;
    QRectF sceneRect(-LARGE_SCENE_SIZE/2, -LARGE_SCENE_SIZE/2, LARGE_SCENE_SIZE, LARGE_SCENE_SIZE);
    m_scene->setSceneRect(sceneRect);
    if (!m_screens.isEmpty()) {
        recenterWithMargin(53);
    }
}

void ScreenCanvas::clearScreens() {
    // Clear existing screen items
    for (QGraphicsRectItem* item : m_screenItems) {
        m_scene->removeItem(item);
        delete item;
    }
    m_screenItems.clear();
}

void ScreenCanvas::createScreenItems() {
    const double SCALE_FACTOR = m_scaleFactor; // Use member scale factor so drops match
    const double H_SPACING = 0.0;  // No horizontal gap between adjacent screens
    const double V_SPACING = 5.0;  // Keep a small vertical gap between rows
    
    // Calculate compact positioning
    QMap<int, QRectF> compactPositions = calculateCompactPositions(SCALE_FACTOR, H_SPACING, V_SPACING);
    
    for (int i = 0; i < m_screens.size(); ++i) {
        const ScreenInfo& screen = m_screens[i];
        QGraphicsRectItem* screenItem = createScreenItem(screen, i, compactPositions[i]);
        m_screenItems.append(screenItem);
        m_scene->addItem(screenItem);
    }
    ensureZOrder();
}

void ScreenCanvas::updateRemoteCursor(int globalX, int globalY) {
    if (!m_remoteCursorDot || m_screens.isEmpty() || m_screenItems.size() != m_screens.size()) {
        if (m_remoteCursorDot) m_remoteCursorDot->setVisible(false);
        return;
    }
    int idx = -1;
    int localX = 0, localY = 0;
    for (int i = 0; i < m_screens.size(); ++i) {
        const auto& s = m_screens[i];
        if (globalX >= s.x && globalX < s.x + s.width && globalY >= s.y && globalY < s.y + s.height) {
            idx = i;
            localX = globalX - s.x;
            localY = globalY - s.y;
            break;
        }
    }
    if (idx < 0) {
        m_remoteCursorDot->setVisible(false);
        return;
    }
    QGraphicsRectItem* item = m_screenItems[idx];
    if (!item) { m_remoteCursorDot->setVisible(false); return; }
    const QRectF r = item->rect();
    if (m_screens[idx].width <= 0 || m_screens[idx].height <= 0 || r.width() <= 0 || r.height() <= 0) { m_remoteCursorDot->setVisible(false); return; }
    const double fx = static_cast<double>(localX) / static_cast<double>(m_screens[idx].width);
    const double fy = static_cast<double>(localY) / static_cast<double>(m_screens[idx].height);
    const double sceneX = r.left() + fx * r.width();
    const double sceneY = r.top() + fy * r.height();
    const double dotSize = 10.0;
    m_remoteCursorDot->setRect(sceneX - dotSize/2.0, sceneY - dotSize/2.0, dotSize, dotSize);
    m_remoteCursorDot->setVisible(true);
}

void ScreenCanvas::hideRemoteCursor() {
    if (m_remoteCursorDot) m_remoteCursorDot->setVisible(false);
}

void ScreenCanvas::ensureZOrder() {
    // Put screens at a low Z, remote cursor very high, media in between
    for (QGraphicsRectItem* r : m_screenItems) {
        if (r) r->setZValue(Z_SCREENS);
    }
    if (m_remoteCursorDot) m_remoteCursorDot->setZValue(Z_REMOTE_CURSOR);
    if (!m_scene) return;
    // Normalize media base Z so overlays (set to ~9000) always win
    for (QGraphicsItem* it : m_scene->items()) {
        auto* media = dynamic_cast<ResizableMediaBase*>(it);
    if (media) media->setZValue(Z_MEDIA_BASE);
    }
}

void ScreenCanvas::setMediaHandleSelectionSizePx(int px) {
    m_mediaHandleSelectionSizePx = qMax(4, px);
    if (!m_scene) return;
    const QList<QGraphicsItem*> sel = m_scene->selectedItems();
    for (QGraphicsItem* it : sel) {
        if (auto* rp = dynamic_cast<ResizablePixmapItem*>(it)) {
            rp->setHandleSelectionSize(m_mediaHandleSelectionSizePx);
        } else if (auto* rv = dynamic_cast<ResizableMediaBase*>(it)) {
            rv->setHandleSelectionSize(m_mediaHandleSelectionSizePx);
        }
    }
}

void ScreenCanvas::setMediaHandleVisualSizePx(int px) {
    m_mediaHandleVisualSizePx = qMax(4, px);
    if (!m_scene) return;
    const QList<QGraphicsItem*> sel = m_scene->selectedItems();
    for (QGraphicsItem* it : sel) {
        if (auto* rp = dynamic_cast<ResizablePixmapItem*>(it)) {
            rp->setHandleVisualSize(m_mediaHandleVisualSizePx);
        } else if (auto* rv = dynamic_cast<ResizableMediaBase*>(it)) {
            rv->setHandleVisualSize(m_mediaHandleVisualSizePx);
        }
    }
}

void ScreenCanvas::setMediaHandleSizePx(int px) {
    setMediaHandleVisualSizePx(px);
    setMediaHandleSelectionSizePx(px);
}

// Drag & drop handlers
void ScreenCanvas::dragEnterEvent(QDragEnterEvent* event) {
    if (event->mimeData()->hasUrls() || event->mimeData()->hasImage()) {
    ensureDragPreview(event->mimeData());
    m_dragPreviewLastScenePos = mapToScene(event->position().toPoint());
    updateDragPreviewPos(m_dragPreviewLastScenePos);
    if (!m_dragCursorHidden) {
#ifdef Q_OS_MACOS
        MacCursorHider::hide();
#else
        QApplication::setOverrideCursor(Qt::BlankCursor);
#endif
        m_dragCursorHidden = true;
    }
        event->acceptProposedAction();
    } else {
        QGraphicsView::dragEnterEvent(event);
    }
}

void ScreenCanvas::dragMoveEvent(QDragMoveEvent* event) {
    if (event->mimeData()->hasUrls() || event->mimeData()->hasImage()) {
    ensureDragPreview(event->mimeData());
    m_dragPreviewLastScenePos = mapToScene(event->position().toPoint());
    updateDragPreviewPos(m_dragPreviewLastScenePos);
        event->acceptProposedAction();
    } else {
        QGraphicsView::dragMoveEvent(event);
    }
}

void ScreenCanvas::dragLeaveEvent(QDragLeaveEvent* event) {
    Q_UNUSED(event);
    clearDragPreview();
    if (m_dragCursorHidden) {
#ifdef Q_OS_MACOS
    MacCursorHider::show();
#else
    QApplication::restoreOverrideCursor();
#endif
    m_dragCursorHidden = false;
    }
}

void ScreenCanvas::dropEvent(QDropEvent* event) {
    QImage image;
    QString filename;
    QString droppedPath;
    if (event->mimeData()->hasImage()) {
        image = qvariant_cast<QImage>(event->mimeData()->imageData());
        filename = "pasted-image";
    } else if (event->mimeData()->hasUrls()) {
        const auto urls = event->mimeData()->urls();
        if (!urls.isEmpty()) {
            const QUrl& url = urls.first();
            const QString path = url.toLocalFile();
            if (!path.isEmpty()) {
                droppedPath = path;
                filename = QFileInfo(path).fileName();
                image.load(path); // may fail if it's a video
            }
        }
    }
    const QPointF scenePos = mapToScene(event->position().toPoint());
    if (!image.isNull()) {
        const double w = image.width() * m_scaleFactor;
        const double h = image.height() * m_scaleFactor;
        auto* item = new ResizablePixmapItem(QPixmap::fromImage(image), m_mediaHandleVisualSizePx, m_mediaHandleSelectionSizePx, filename);
        item->setFlags(QGraphicsItem::ItemIsMovable | QGraphicsItem::ItemIsSelectable | QGraphicsItem::ItemSendsGeometryChanges);
        item->setPos(scenePos.x() - w/2.0, scenePos.y() - h/2.0);
        if (image.width() > 0) item->setScale(w / image.width());
    m_scene->addItem(item);
    // Auto-select the newly dropped item
    if (m_scene) m_scene->clearSelection();
    item->setSelected(true);
    } else if (!droppedPath.isEmpty()) {
        // Decide if it's a video by extension
        const QString ext = QFileInfo(droppedPath).suffix().toLower();
        static const QSet<QString> kVideoExts = {"mp4","mov","m4v","avi","mkv","webm"};
        if (kVideoExts.contains(ext)) {
            // Create with placeholder logical size; adopt real size on first frame
            auto* vitem = new ResizableVideoItem(droppedPath, m_mediaHandleVisualSizePx, m_mediaHandleSelectionSizePx, filename, m_videoControlsFadeMs);
            vitem->setInitialScaleFactor(m_scaleFactor);
            // Start with a neutral 1.0 scale on placeholder size and center approx.
            vitem->setScale(m_scaleFactor);
            const double w = 640.0 * m_scaleFactor;
            const double h = 360.0 * m_scaleFactor;
            vitem->setPos(scenePos.x() - w/2.0, scenePos.y() - h/2.0);
            m_scene->addItem(vitem);
            // Auto-select the newly dropped video item
            if (m_scene) m_scene->clearSelection();
            vitem->setSelected(true);
            // If a real preview frame exists, set it as poster after adding to the scene
            if (!m_dragPreviewPixmap.isNull()) {
                vitem->setExternalPosterImage(m_dragPreviewPixmap.toImage());
            }
        } else {
            QGraphicsView::dropEvent(event);
            return;
        }
    } else {
        QGraphicsView::dropEvent(event);
        return;
    }
    ensureZOrder();
    event->acceptProposedAction();
    // Keep the preview visible at the drop location briefly until the new item paints, then clear
    clearDragPreview();
    if (m_dragCursorHidden) {
#ifdef Q_OS_MACOS
    MacCursorHider::show();
#else
    QApplication::restoreOverrideCursor();
#endif
    m_dragCursorHidden = false;
    }
}

void ScreenCanvas::ensureDragPreview(const QMimeData* mime) {
    if (!m_scene) return;
    if (m_dragPreviewItem) return; // already created
    QPixmap preview;
    m_dragPreviewIsVideo = false;
    m_dragPreviewPixmap = QPixmap();
    m_dragPreviewBaseSize = QSize();
    // Try image data directly
    if (mime->hasImage()) {
        QImage img = qvariant_cast<QImage>(mime->imageData());
        if (!img.isNull()) {
            preview = QPixmap::fromImage(img);
        }
    }
    // Or a local file URL
    if (preview.isNull() && mime->hasUrls()) {
        const auto urls = mime->urls();
        if (!urls.isEmpty()) {
            const QUrl& url = urls.first();
            const QString path = url.toLocalFile();
            if (!path.isEmpty()) {
                const QString ext = QFileInfo(path).suffix().toLower();
                static const QSet<QString> kVideoExts = {"mp4","mov","m4v","avi","mkv","webm"};
                if (kVideoExts.contains(ext)) {
                    m_dragPreviewIsVideo = true;
                    // Start background probe to fetch first frame ASAP; do not show any placeholder
                    startVideoPreviewProbe(path);
                    // Return early: we'll create the pixmap item when the first frame arrives
                    return;
                } else {
                    preview.load(path); // may fail if not an image
                }
            }
        }
    }
    if (preview.isNull()) return; // nothing to preview yet
    m_dragPreviewPixmap = preview;
    m_dragPreviewBaseSize = preview.size();
    // Create a QGraphicsPixmapItem as the preview
    auto* pmItem = new QGraphicsPixmapItem(preview);
    // Start from transparent and fade in
    pmItem->setOpacity(0.0);
    pmItem->setZValue(5000.0); // on top of everything during drag
    pmItem->setFlag(QGraphicsItem::ItemIgnoresTransformations, false); // scale with scene factor
    // Apply initial scale to match our canvas scale factor
    pmItem->setScale(m_scaleFactor);
    m_scene->addItem(pmItem);
    m_dragPreviewItem = pmItem;
    startDragPreviewFadeIn();
}

void ScreenCanvas::updateDragPreviewPos(const QPointF& scenePos) {
    if (!m_dragPreviewItem) return;
    const QSize sz = m_dragPreviewBaseSize.isValid() ? m_dragPreviewBaseSize : QSize(320,180);
    const double w = sz.width() * m_scaleFactor;
    const double h = sz.height() * m_scaleFactor;
    m_dragPreviewItem->setPos(scenePos.x() - w/2.0, scenePos.y() - h/2.0);
}

void ScreenCanvas::clearDragPreview() {
    stopDragPreviewFade();
    if (m_dragPreviewItem && m_scene) {
        m_scene->removeItem(m_dragPreviewItem);
        delete m_dragPreviewItem;
    }
    m_dragPreviewItem = nullptr;
    m_dragPreviewBaseSize = QSize();
    m_dragPreviewPixmap = QPixmap();
    m_dragPreviewIsVideo = false;
    stopVideoPreviewProbe();
}

QPixmap ScreenCanvas::makeVideoPlaceholderPixmap(const QSize& pxSize) {
    QSize s = pxSize.isValid() ? pxSize : QSize(320, 180);
    QPixmap pm(s);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing);
    QColor bg(0,0,0,180);
    p.setBrush(bg);
    p.setPen(Qt::NoPen);
    const qreal r = 8.0;
    p.drawRoundedRect(QRectF(0,0,s.width(), s.height()), r, r);
    // Draw a simple play triangle in the center
    const qreal triW = s.width() * 0.18;
    const qreal triH = triW * 1.0;
    QPointF c(s.width()/2.0, s.height()/2.0);
    QPolygonF tri;
    tri << QPointF(c.x() - triW/3.0, c.y() - triH/2.0)
        << QPointF(c.x() - triW/3.0, c.y() + triH/2.0)
        << QPointF(c.x() + triW*0.7, c.y());
    p.setBrush(QColor(255,255,255,230));
    p.drawPolygon(tri);
    p.end();
    return pm;
}

void ScreenCanvas::startVideoPreviewProbe(const QString& localFilePath) {
    stopVideoPreviewProbe();
    m_dragPreviewGotFrame = false;
#ifdef Q_OS_MACOS
    // Launch a very fast first-frame extraction via AVFoundation on a background thread
    if (m_dragPreviewFallbackTimer) { m_dragPreviewFallbackTimer->stop(); m_dragPreviewFallbackTimer->deleteLater(); m_dragPreviewFallbackTimer = nullptr; }
    QString pathCopy = localFilePath;
    std::thread([this, pathCopy]() {
        QImage img = MacVideoThumbnailer::firstFrame(pathCopy);
        if (!img.isNull()) {
            QImage copy = img;
            QMetaObject::invokeMethod(this, [this, copy]() { onFastVideoThumbnailReady(copy); }, Qt::QueuedConnection);
        }
    }).detach();
    // Fallback: if fast path doesn't answer quickly, start the decoder-based probe
    m_dragPreviewFallbackTimer = new QTimer(this);
    m_dragPreviewFallbackTimer->setSingleShot(true);
    connect(m_dragPreviewFallbackTimer, &QTimer::timeout, this, [this, localFilePath]() {
        startVideoPreviewProbeFallback(localFilePath);
    });
    m_dragPreviewFallbackTimer->start(120); // short grace period
#else
    startVideoPreviewProbeFallback(localFilePath);
#endif
}

void ScreenCanvas::startVideoPreviewProbeFallback(const QString& localFilePath) {
    if (m_dragPreviewPlayer) return; // already running
    m_dragPreviewPlayer = new QMediaPlayer(this);
    m_dragPreviewAudio = new QAudioOutput(this);
    m_dragPreviewAudio->setMuted(true);
    m_dragPreviewPlayer->setAudioOutput(m_dragPreviewAudio);
    m_dragPreviewSink = new QVideoSink(this);
    m_dragPreviewPlayer->setVideoSink(m_dragPreviewSink);
    m_dragPreviewPlayer->setSource(QUrl::fromLocalFile(localFilePath));
    connect(m_dragPreviewSink, &QVideoSink::videoFrameChanged, this, [this](const QVideoFrame& f){
        if (m_dragPreviewGotFrame || !f.isValid()) return;
        QImage img = f.toImage();
        if (img.isNull()) return;
        m_dragPreviewGotFrame = true;
        QPixmap newPm = QPixmap::fromImage(img);
        if (newPm.isNull()) return;
        m_dragPreviewPixmap = newPm;
        m_dragPreviewBaseSize = newPm.size();
        if (!m_dragPreviewItem) {
            auto* pmItem = new QGraphicsPixmapItem(m_dragPreviewPixmap);
            pmItem->setOpacity(0.0);
            pmItem->setZValue(5000.0);
            pmItem->setFlag(QGraphicsItem::ItemIgnoresTransformations, false);
            pmItem->setScale(m_scaleFactor);
            m_scene->addItem(pmItem);
            m_dragPreviewItem = pmItem;
            updateDragPreviewPos(m_dragPreviewLastScenePos);
            startDragPreviewFadeIn();
        } else if (auto* pmItem = qgraphicsitem_cast<QGraphicsPixmapItem*>(m_dragPreviewItem)) {
            pmItem->setPixmap(m_dragPreviewPixmap);
            updateDragPreviewPos(m_dragPreviewLastScenePos);
        }
        if (m_dragPreviewPlayer) m_dragPreviewPlayer->pause();
        if (m_dragPreviewFallbackTimer) { m_dragPreviewFallbackTimer->stop(); m_dragPreviewFallbackTimer->deleteLater(); m_dragPreviewFallbackTimer = nullptr; }
    });
    m_dragPreviewPlayer->play();
}

void ScreenCanvas::onFastVideoThumbnailReady(const QImage& img) {
    if (img.isNull()) return;
    if (m_dragPreviewGotFrame) return; // already satisfied by fallback
    m_dragPreviewGotFrame = true;
    QPixmap pm = QPixmap::fromImage(img);
    if (pm.isNull()) return;
    m_dragPreviewPixmap = pm;
    m_dragPreviewBaseSize = pm.size();
    if (!m_dragPreviewItem) {
        auto* pmItem = new QGraphicsPixmapItem(m_dragPreviewPixmap);
        pmItem->setOpacity(0.0);
        pmItem->setZValue(5000.0);
        pmItem->setFlag(QGraphicsItem::ItemIgnoresTransformations, false);
        pmItem->setScale(m_scaleFactor);
        if (m_scene) m_scene->addItem(pmItem);
        m_dragPreviewItem = pmItem;
        updateDragPreviewPos(m_dragPreviewLastScenePos);
        startDragPreviewFadeIn();
    } else if (auto* pix = qgraphicsitem_cast<QGraphicsPixmapItem*>(m_dragPreviewItem)) {
        pix->setPixmap(m_dragPreviewPixmap);
        updateDragPreviewPos(m_dragPreviewLastScenePos);
    }
    if (m_dragPreviewFallbackTimer) { m_dragPreviewFallbackTimer->stop(); m_dragPreviewFallbackTimer->deleteLater(); m_dragPreviewFallbackTimer = nullptr; }
    // If decoder path started, stop it
    if (m_dragPreviewPlayer) { m_dragPreviewPlayer->stop(); m_dragPreviewPlayer->deleteLater(); m_dragPreviewPlayer = nullptr; }
    if (m_dragPreviewSink) { m_dragPreviewSink->deleteLater(); m_dragPreviewSink = nullptr; }
    if (m_dragPreviewAudio) { m_dragPreviewAudio->deleteLater(); m_dragPreviewAudio = nullptr; }
}

void ScreenCanvas::stopVideoPreviewProbe() {
    if (m_dragPreviewFallbackTimer) {
        m_dragPreviewFallbackTimer->stop();
        m_dragPreviewFallbackTimer->deleteLater();
        m_dragPreviewFallbackTimer = nullptr;
    }
    if (m_dragPreviewPlayer) {
        m_dragPreviewPlayer->stop();
        m_dragPreviewPlayer->deleteLater();
        m_dragPreviewPlayer = nullptr;
    }
    if (m_dragPreviewSink) {
        m_dragPreviewSink->deleteLater();
        m_dragPreviewSink = nullptr;
    }
    if (m_dragPreviewAudio) {
        m_dragPreviewAudio->deleteLater();
        m_dragPreviewAudio = nullptr;
    }
}

void ScreenCanvas::startDragPreviewFadeIn() {
    // Stop any existing animation first
    stopDragPreviewFade();
    if (!m_dragPreviewItem) return;
    const qreal target = m_dragPreviewTargetOpacity;
    if (m_dragPreviewItem->opacity() >= target - 0.001) return;
    auto* anim = new QVariantAnimation(this);
    m_dragPreviewFadeAnim = anim;
    anim->setStartValue(0.0);
    anim->setEndValue(target);
    anim->setDuration(m_dragPreviewFadeMs);
    anim->setEasingCurve(QEasingCurve::OutCubic);
    connect(anim, &QVariantAnimation::valueChanged, this, [this](const QVariant& v){
        if (m_dragPreviewItem) m_dragPreviewItem->setOpacity(v.toReal());
    });
    connect(anim, &QVariantAnimation::finished, this, [this](){
        m_dragPreviewFadeAnim = nullptr;
    });
    anim->start(QAbstractAnimation::DeleteWhenStopped);
}

void ScreenCanvas::stopDragPreviewFade() {
    if (m_dragPreviewFadeAnim) {
        m_dragPreviewFadeAnim->stop();
        m_dragPreviewFadeAnim = nullptr;
    }
}

QGraphicsRectItem* ScreenCanvas::createScreenItem(const ScreenInfo& screen, int index, const QRectF& position) {
    // Border must be fully inside so the outer size matches the logical screen size.
    const int penWidth = m_screenBorderWidthPx; // configurable
    // Inset the rect by half the pen width so the stroke stays entirely inside.
    QRectF inner = position.adjusted(penWidth / 2.0, penWidth / 2.0,
                                     -penWidth / 2.0, -penWidth / 2.0);
    QGraphicsRectItem* item = new QGraphicsRectItem(inner);
    
    // Set appearance
    if (screen.primary) {
        item->setBrush(QBrush(QColor(74, 144, 226, 180))); // Primary screen - blue
        item->setPen(QPen(QColor(74, 144, 226), penWidth));
    } else {
        item->setBrush(QBrush(QColor(80, 80, 80, 180))); // Secondary screen - gray
        item->setPen(QPen(QColor(160, 160, 160), penWidth));
    }
    
    // Store screen index for click handling
    item->setData(0, index);
    
    // Add screen label
    QGraphicsTextItem* label = new QGraphicsTextItem(QString("Screen %1\n%2×%3")
        .arg(index + 1)
        .arg(screen.width)
        .arg(screen.height));
    label->setDefaultTextColor(Qt::white);
    label->setFont(QFont("Arial", 12, QFont::Bold));
    
    // Center the label on the screen
    QRectF labelRect = label->boundingRect();
    QRectF screenRect = item->rect();
    label->setPos(screenRect.center() - labelRect.center());
    label->setParentItem(item);
    
    return item;
}

void ScreenCanvas::setScreenBorderWidthPx(int px) {
    m_screenBorderWidthPx = qMax(0, px);
    // Update existing screen items to keep the same outer size while drawing the stroke fully inside
    // We know m_screenItems aligns with m_screens by index after createScreenItems().
    if (!m_scene) return;
    for (int i = 0; i < m_screenItems.size() && i < m_screens.size(); ++i) {
        QGraphicsRectItem* item = m_screenItems[i];
        if (!item) continue;
        const int penW = m_screenBorderWidthPx;
        // Compute the outer rect from the current item rect by reversing the previous inset.
        // Previous inset may differ; reconstruct outer by expanding by old half-pen, then re-inset by new half-pen.
        // Since we don't track the old pen per-item, approximate using current pen width on the item.
        int oldPenW = static_cast<int>(item->pen().widthF());
        QRectF currentInner = item->rect();
        QRectF outer = currentInner.adjusted(-(oldPenW/2.0), -(oldPenW/2.0), (oldPenW/2.0), (oldPenW/2.0));
        QRectF newInner = outer.adjusted(penW/2.0, penW/2.0, -penW/2.0, -penW/2.0);
        item->setRect(newInner);
        QPen p = item->pen();
        p.setWidthF(penW);
        item->setPen(p);
    }
}

QMap<int, QRectF> ScreenCanvas::calculateCompactPositions(double scaleFactor, double hSpacing, double vSpacing) const {
    QMap<int, QRectF> positions;
    
    if (m_screens.isEmpty()) {
        return positions;
    }
    
    // Simple left-to-right, top-to-bottom compact layout
    
    // First, sort screens by their actual position (left to right, then top to bottom)
    QList<QPair<int, ScreenInfo>> screenPairs;
    for (int i = 0; i < m_screens.size(); ++i) {
        screenPairs.append(qMakePair(i, m_screens[i]));
    }
    
    // Sort by Y first (top to bottom), then by X (left to right)
    std::sort(screenPairs.begin(), screenPairs.end(), 
              [](const QPair<int, ScreenInfo>& a, const QPair<int, ScreenInfo>& b) {
                  if (qAbs(a.second.y - b.second.y) < 100) { // If roughly same height
                      return a.second.x < b.second.x; // Sort by X
                  }
                  return a.second.y < b.second.y; // Sort by Y
              });
    
    // Layout screens with compact positioning
    double currentX = 0;
    double currentY = 0;
    double rowHeight = 0;
    int lastY = INT_MIN;
    
    for (const auto& pair : screenPairs) {
        int index = pair.first;
        const ScreenInfo& screen = pair.second;
        
        double screenWidth = screen.width * scaleFactor;
        double screenHeight = screen.height * scaleFactor;
        
        // Start new row if Y position changed significantly
        if (lastY != INT_MIN && qAbs(screen.y - lastY) > 100) {
            currentX = 0;
            currentY += rowHeight + vSpacing;
            rowHeight = 0;
        }
        
        // Position the screen
        QRectF rect(currentX, currentY, screenWidth, screenHeight);
    positions[index] = rect;
        
        // Update for next screen
    currentX += screenWidth + hSpacing;
        rowHeight = qMax(rowHeight, screenHeight);
        lastY = screen.y;
    }
    
    return positions;
}

QRectF ScreenCanvas::screensBoundingRect() const {
    QRectF bounds;
    bool first = true;
    for (auto* item : m_screenItems) {
        if (!item) continue;
        QRectF r = item->sceneBoundingRect();
        if (first) { bounds = r; first = false; }
        else { bounds = bounds.united(r); }
    }
    return bounds;
}

void ScreenCanvas::recenterWithMargin(int marginPx) {
    QRectF bounds = screensBoundingRect();
    if (bounds.isNull() || !bounds.isValid()) return;
    // Fit content with a fixed pixel margin in the viewport
    const QSize vp = viewport() ? viewport()->size() : size();
    const qreal availW = static_cast<qreal>(vp.width())  - 2.0 * marginPx;
    const qreal availH = static_cast<qreal>(vp.height()) - 2.0 * marginPx;

    if (availW <= 1 || availH <= 1 || bounds.width() <= 0 || bounds.height() <= 0) {
        // Fallback to a simple fit if viewport is too small
        fitInView(bounds, Qt::KeepAspectRatio);
        centerOn(bounds.center());
        return;
    }

    const qreal sx = availW / bounds.width();
    const qreal sy = availH / bounds.height();
    const qreal s = std::min(sx, sy);

    // Apply the scale in one shot and center on content
    QTransform t;
    t.scale(s, s);
    setTransform(t);
    centerOn(bounds.center());
    // After recenter, refresh overlays only for selected items (cheaper and sufficient)
    if (m_scene) {
        const QList<QGraphicsItem*> sel = m_scene->selectedItems();
        for (QGraphicsItem* it : sel) {
            if (auto* v = dynamic_cast<ResizableVideoItem*>(it)) {
                v->requestOverlayRelayout();
            }
            if (auto* b = dynamic_cast<ResizableMediaBase*>(it)) {
                b->requestLabelRelayout();
            }
        }
    }
    // Start momentum suppression: ignore decaying inertial scroll deltas until an increase occurs
    m_ignorePanMomentum = true;
    m_momentumPrimed = false;
    m_lastMomentumMag = 0.0;
    m_lastMomentumDelta = QPoint(0, 0);
    m_momentumTimer.restart();
}

void ScreenCanvas::keyPressEvent(QKeyEvent* event) {
    // Delete selected media items
    if (event->key() == Qt::Key_Delete || event->key() == Qt::Key_Backspace) {
        if (m_scene) {
            const QList<QGraphicsItem*> sel = m_scene->selectedItems();
            for (QGraphicsItem* it : sel) {
                if (auto* base = dynamic_cast<ResizableMediaBase*>(it)) {
                    base->ungrabMouse();
                    m_scene->removeItem(base);
                    delete base;
                }
            }
        }
        event->accept();
        return;
    }
    if (event->key() == Qt::Key_Space) {
        recenterWithMargin(53);
        event->accept();
        return;
    }
    QGraphicsView::keyPressEvent(event);
}

bool ScreenCanvas::event(QEvent* event) {
    if (event->type() == QEvent::Gesture) {
#ifdef Q_OS_MACOS
        // On macOS, rely on QNativeGestureEvent for trackpad pinch to avoid duplicate scaling
        return false;
#else
        return gestureEvent(static_cast<QGestureEvent*>(event));
#endif
    }
    // Handle native gestures (macOS trackpad pinch)
    if (event->type() == QEvent::NativeGesture) {
        auto* ng = static_cast<QNativeGestureEvent*>(event);
        if (ng->gestureType() == Qt::ZoomNativeGesture) {
            // Mark pinch as active and extend guard
            m_nativePinchActive = true;
            m_nativePinchGuardTimer->start();
            // ng->value() is a delta; use exponential to convert to multiplicative factor
            const qreal factor = std::pow(2.0, ng->value());
            // Figma/Canva-style: anchor at the cursor position first
            QPoint vpPos = viewport()->mapFromGlobal(QCursor::pos());
            if (!viewport()->rect().contains(vpPos)) {
                // Fallback to gesture position (view coords -> viewport) then last mouse pos/center
                QPoint viewPos = ng->position().toPoint();
                vpPos = viewport()->mapFrom(this, viewPos);
                if (!viewport()->rect().contains(vpPos)) {
                    vpPos = m_lastMousePos.isNull() ? viewport()->rect().center() : m_lastMousePos;
                }
            }
            zoomAroundViewportPos(vpPos, factor);
            event->accept();
            return true;
        }
    }
    return QGraphicsView::event(event);
}

bool ScreenCanvas::gestureEvent(QGestureEvent* event) {
#ifdef Q_OS_MACOS
    Q_UNUSED(event);
    return false; // handled via NativeGesture on macOS
#endif
    if (QGesture* pinch = event->gesture(Qt::PinchGesture)) {
        auto* pinchGesture = static_cast<QPinchGesture*>(pinch);
        if (pinchGesture->changeFlags() & QPinchGesture::ScaleFactorChanged) {
            const qreal factor = pinchGesture->scaleFactor();
            const QPoint anchor = m_lastMousePos.isNull() ? viewport()->rect().center() : m_lastMousePos;
            zoomAroundViewportPos(anchor, factor);
        }
        event->accept();
        return true;
    }
    return false;
}

void ScreenCanvas::mousePressEvent(QMouseEvent* event) {
    if (event->button() == Qt::LeftButton) {
        // Priority 0: forward to controls of currently selected videos before any other handling
        if (m_scene) {
            const QPointF scenePosEarly = mapToScene(event->pos());
            const QList<QGraphicsItem*> selEarly = m_scene->selectedItems();
            for (QGraphicsItem* it : selEarly) {
                if (auto* v = dynamic_cast<ResizableVideoItem*>(it)) {
                    const QPointF itemPos = v->mapFromScene(scenePosEarly);
                    if (v->handleControlsPressAtItemPos(itemPos)) {
                        m_overlayMouseDown = true; // consume subsequent release as well
                        event->accept();
                        return;
                    }
                }
            }
        }
        // Try to start resize on the topmost media item whose handle contains the point
        // BUT only if the item is already selected
        const QPointF scenePos = mapToScene(event->pos());
        ResizableMediaBase* topHandleItem = nullptr;
        qreal topZ = -std::numeric_limits<qreal>::infinity();
    const QList<QGraphicsItem*> sel = m_scene ? m_scene->selectedItems() : QList<QGraphicsItem*>();
    for (QGraphicsItem* it : sel) {
            if (auto* rp = dynamic_cast<ResizableMediaBase*>(it)) {
                // Only allow resize if item is selected
                if (rp->isSelected() && rp->isOnHandleAtItemPos(rp->mapFromScene(scenePos))) {
                    if (rp->zValue() > topZ) { topZ = rp->zValue(); topHandleItem = rp; }
                }
            }
        }
        if (topHandleItem) {
            if (topHandleItem->beginResizeAtScenePos(scenePos)) {
                // Set resize cursor during active resize
                Qt::CursorShape cursor = topHandleItem->cursorForScenePos(scenePos);
                viewport()->setCursor(cursor);
                event->accept();
                return;
            }
        }
        // Decide based on item type under cursor: media (or its overlays) -> scene, screens/empty -> pan
        const QList<QGraphicsItem*> hitItems = items(event->pos());
        auto toMedia = [](QGraphicsItem* x)->ResizableMediaBase* {
            while (x) { if (auto* m = dynamic_cast<ResizableMediaBase*>(x)) return m; x = x->parentItem(); }
            return nullptr;
        };
        ResizableMediaBase* mediaHit = nullptr;
        QGraphicsItem* rawHit = nullptr;
        for (QGraphicsItem* it : hitItems) { if ((mediaHit = toMedia(it))) { rawHit = it; break; } }
        if (mediaHit) {
            // For simple UX: modifiers do nothing; always single-select the clicked media
            if (m_scene) m_scene->clearSelection();
            if (!mediaHit->isSelected()) mediaHit->setSelected(true);
            // Map click to item pos and let video handle controls
            if (auto* v = dynamic_cast<ResizableVideoItem*>(mediaHit)) {
                const QPointF itemPos = v->mapFromScene(mapToScene(event->pos()));
                if (v->handleControlsPressAtItemPos(itemPos)) {
                    event->accept();
                    return;
                }
            }
            // Let default behavior run (for move/drag, etc.), but ignore modifiers
            QMouseEvent synthetic(event->type(), event->position(), event->scenePosition(), event->globalPosition(),
                                  event->button(), event->buttons(), Qt::NoModifier);
            QGraphicsView::mousePressEvent(&synthetic);
            // Enforce single selection after default handling
            if (m_scene) m_scene->clearSelection();
            mediaHit->setSelected(true);
            return;
        }

        // No item hit: allow interacting with controls of currently selected video items
        // This covers the case where the floating controls are positioned outside the item's hittable area
        for (QGraphicsItem* it : scene()->selectedItems()) {
            if (auto* v = dynamic_cast<ResizableVideoItem*>(it)) {
                const QPointF itemPos = v->mapFromScene(mapToScene(event->pos()));
                if (v->handleControlsPressAtItemPos(itemPos)) {
                    event->accept();
                    return;
                }
            }
        }
        // Otherwise start panning the view
        if (m_scene) {
            m_scene->clearSelection(); // single-click on empty space deselects items
        }
        m_panning = true;
        m_lastPanPoint = event->pos();
        event->accept();
        return;
    }
    QGraphicsView::mousePressEvent(event);
}

void ScreenCanvas::mouseDoubleClickEvent(QMouseEvent* event) {
    if (event->button() == Qt::LeftButton) {
        if (m_scene) {
            const QPointF scenePosSel = mapToScene(event->pos());
            const QList<QGraphicsItem*> sel = m_scene->selectedItems();
            for (QGraphicsItem* it : sel) {
                if (auto* v = dynamic_cast<ResizableVideoItem*>(it)) {
                    const QPointF itemPos = v->mapFromScene(scenePosSel);
                    if (v->handleControlsPressAtItemPos(itemPos)) {
                        // Swallow the remainder of this double-click sequence (final release)
                        m_overlayMouseDown = true;
                        event->accept();
                        return;
                    }
                }
            }
        }
        const QPointF scenePos = mapToScene(event->pos());
        const QList<QGraphicsItem*> hitItems = items(event->pos());
        auto toMedia = [](QGraphicsItem* x)->ResizableMediaBase* {
            while (x) { if (auto* m = dynamic_cast<ResizableMediaBase*>(x)) return m; x = x->parentItem(); }
            return nullptr;
        };
        ResizableMediaBase* mediaHit = nullptr;
        for (QGraphicsItem* it : hitItems) { if ((mediaHit = toMedia(it))) break; }
        if (mediaHit) {
            if (scene()) scene()->clearSelection();
            if (!mediaHit->isSelected()) mediaHit->setSelected(true);
            if (auto* v = dynamic_cast<ResizableVideoItem*>(mediaHit)) {
                const QPointF itemPos = v->mapFromScene(scenePos);
                if (v->handleControlsPressAtItemPos(itemPos)) { event->accept(); return; }
            }
            QGraphicsView::mouseDoubleClickEvent(event);
            // Enforce single selection again
            if (scene()) scene()->clearSelection();
            mediaHit->setSelected(true);
            return;
        }
        // If no media hit, pass through
    }
    QGraphicsView::mouseDoubleClickEvent(event);
}

void ScreenCanvas::mouseMoveEvent(QMouseEvent* event) {
    if (m_overlayMouseDown) {
        // If an overlay initiated the interaction, forward moves to any dragging sliders
        if (m_scene) {
            const QList<QGraphicsItem*> sel = m_scene->selectedItems();
            for (QGraphicsItem* it : sel) {
                if (auto* v = dynamic_cast<ResizableVideoItem*>(it)) {
                    if (v->isDraggingProgress() || v->isDraggingVolume()) {
                        v->updateDragWithScenePos(mapToScene(event->pos()));
                        event->accept();
                        return;
                    }
                }
            }
        }
        // Not a drag-type overlay; just swallow move
        event->accept();
        return;
    }
    // Track last mouse pos in viewport coords (used for zoom anchoring)
    m_lastMousePos = event->pos();
    
    // PRIORITY: Check if we're over a resize handle and set cursor accordingly
    // This must be done BEFORE any other cursor management to ensure it's not overridden
    // BUT only if the item is selected!
    const QPointF scenePos = mapToScene(event->pos());
    Qt::CursorShape resizeCursor = Qt::ArrowCursor;
    bool onResizeHandle = false;
    
    // Check only selected media items for handle hover (prioritize topmost)
    qreal topZ = -std::numeric_limits<qreal>::infinity();
    const QList<QGraphicsItem*> sel = m_scene ? m_scene->selectedItems() : QList<QGraphicsItem*>();
    for (QGraphicsItem* it : sel) {
        if (auto* rp = dynamic_cast<ResizableMediaBase*>(it)) {
            // Only show resize cursor if the item is selected
            if (rp->isSelected() && rp->zValue() >= topZ) {
                Qt::CursorShape itemCursor = rp->cursorForScenePos(scenePos);
                if (itemCursor != Qt::ArrowCursor) {
                    resizeCursor = itemCursor;
                    onResizeHandle = true;
                    topZ = rp->zValue();
                }
            }
        }
    }
    
    // Set the cursor with highest priority
    if (onResizeHandle) {
        viewport()->setCursor(resizeCursor);
    } else {
        viewport()->unsetCursor();
    }
    
    // Handle dragging and panning logic
    if (event->buttons() & Qt::LeftButton) {
        // If a selected video is currently dragging its sliders, update it even if cursor left its shape
    for (QGraphicsItem* it : sel) {
            if (auto* v = dynamic_cast<ResizableVideoItem*>(it)) {
                if (v->isSelected() && (v->isDraggingProgress() || v->isDraggingVolume())) {
                    v->updateDragWithScenePos(mapToScene(event->pos()));
                    event->accept();
                    return;
                }
            }
        }
        const QList<QGraphicsItem*> hitItems = items(event->pos());
        auto toMedia = [](QGraphicsItem* x)->ResizableMediaBase* {
            while (x) { if (auto* m = dynamic_cast<ResizableMediaBase*>(x)) return m; x = x->parentItem(); }
            return nullptr;
        };
        bool hitMedia = false;
        for (QGraphicsItem* it : hitItems) { if (toMedia(it)) { hitMedia = true; break; } }
        if (hitMedia) { QGraphicsView::mouseMoveEvent(event); return; }
    }
    if (m_panning) {
        // Pan the view
        QPoint delta = event->pos() - m_lastPanPoint;
        horizontalScrollBar()->setValue(horizontalScrollBar()->value() - delta.x());
        verticalScrollBar()->setValue(verticalScrollBar()->value() - delta.y());
        m_lastPanPoint = event->pos();
    }
    QGraphicsView::mouseMoveEvent(event);
}

void ScreenCanvas::mouseReleaseEvent(QMouseEvent* event) {
    if (event->button() == Qt::LeftButton) {
        // If overlay consumed the press, finish any active slider drag and swallow release
        if (m_overlayMouseDown) {
            if (m_scene) {
                const QList<QGraphicsItem*> sel = m_scene->selectedItems();
                for (QGraphicsItem* it : sel) {
                    if (auto* v = dynamic_cast<ResizableVideoItem*>(it)) {
                        if (v->isDraggingProgress() || v->isDraggingVolume()) {
                            v->endDrag();
                        }
                    }
                }
            }
            m_overlayMouseDown = false;
            event->accept();
            return;
        }
        // Finalize any active drag on selected video controls
        for (QGraphicsItem* it : m_scene->items()) {
            if (auto* v = dynamic_cast<ResizableVideoItem*>(it)) {
                if (v->isSelected() && (v->isDraggingProgress() || v->isDraggingVolume())) {
                    v->endDrag();
                    event->accept();
                    return;
                }
            }
        }
        if (m_panning) {
            m_panning = false;
            event->accept();
            return;
        }
        // Check if any item was being resized and reset cursor
        bool wasResizing = false;
        for (QGraphicsItem* it : m_scene->items()) {
            if (auto* rp = dynamic_cast<ResizableMediaBase*>(it)) {
                if (rp->isActivelyResizing()) {
                    wasResizing = true;
                    break;
                }
            }
        }
        if (wasResizing) {
            // Reset cursor after resize
            viewport()->unsetCursor();
        }
        // Route to base with modifiers stripped to avoid Qt toggling selection on Ctrl/Cmd
        QMouseEvent synthetic(event->type(), event->position(), event->scenePosition(), event->globalPosition(),
                              event->button(), event->buttons(), Qt::NoModifier);
        QGraphicsView::mouseReleaseEvent(&synthetic);
        // Enforce single-select policy regardless of modifiers once the click finishes
        if (m_scene) {
            const QList<QGraphicsItem*> sel = m_scene->selectedItems();
            if (!sel.isEmpty()) {
                // Keep the topmost media item under cursor if possible; otherwise keep the first selected
                const QList<QGraphicsItem*> hitItems = items(event->pos());
                auto toMedia = [](QGraphicsItem* x)->ResizableMediaBase* {
                    while (x) { if (auto* m = dynamic_cast<ResizableMediaBase*>(x)) return m; x = x->parentItem(); }
                    return nullptr;
                };
                ResizableMediaBase* keep = nullptr;
                for (QGraphicsItem* it : hitItems) { if ((keep = toMedia(it))) break; }
                if (!keep) {
                    for (QGraphicsItem* it : sel) { if ((keep = dynamic_cast<ResizableMediaBase*>(it))) break; }
                }
                m_scene->clearSelection();
                if (keep) keep->setSelected(true);
            }
        }
        return;
    }
    QGraphicsView::mouseReleaseEvent(event);
}

void ScreenCanvas::wheelEvent(QWheelEvent* event) {
    // On macOS during an active pinch, ignore wheel-based two-finger scroll to avoid jitter
#ifdef Q_OS_MACOS
    if (m_nativePinchActive) {
        event->ignore();
        return;
    }
#endif
    // If Cmd (macOS) or Ctrl (others) is held, treat the wheel as zoom, anchored under cursor
#ifdef Q_OS_MACOS
    const bool zoomModifier = event->modifiers().testFlag(Qt::MetaModifier);
#else
    const bool zoomModifier = event->modifiers().testFlag(Qt::ControlModifier);
#endif

    if (zoomModifier) {
        qreal deltaY = 0.0;
        if (!event->pixelDelta().isNull()) {
            deltaY = event->pixelDelta().y();
        } else if (!event->angleDelta().isNull()) {
            // angleDelta is in 1/8 degree; 120 typically represents one step
            deltaY = event->angleDelta().y() / 8.0; // convert to degrees-ish
        }
        if (deltaY != 0.0) {
            // Convert delta to an exponential zoom factor for smoothness
            const qreal factor = std::pow(1.0015, deltaY);
            zoomAroundViewportPos(event->position(), factor);
            event->accept();
            return;
        }
    }

    // Default behavior: scroll/pan content
    // Prefer pixelDelta for trackpads (gives smooth 2D vector), fallback to angleDelta for mouse wheels.
    QPoint delta;
    if (!event->pixelDelta().isNull()) {
        delta = event->pixelDelta();
    } else if (!event->angleDelta().isNull()) {
        delta = event->angleDelta() / 8; // small scaling for smoother feel
    }
    if (!delta.isNull()) {
        // Momentum suppression: after recenter, ignore decaying inertial deltas robustly
        if (m_ignorePanMomentum) {
            // If the platform provides scroll phase, end suppression on a new phase begin
#if QT_VERSION >= QT_VERSION_CHECK(6, 3, 0)
            if (event->phase() == Qt::ScrollUpdate && !m_momentumPrimed) {
                // still within the same fling; keep suppression
            }
            if (event->phase() == Qt::ScrollBegin) {
                // New gesture explicitly began
                m_ignorePanMomentum = false;
            }
            if (!m_ignorePanMomentum) {
                // process normally
            } else
#endif
            {
                // Hysteresis thresholds
                const double noiseGate = 1.0;   // ignore tiny bumps
                const double boostRatio = 1.25; // require 25% increase to consider momentum ended
                const qint64 graceMs = 180;     // ignore everything for a short time after recenter
                const qint64 elapsed = m_momentumTimer.isValid() ? m_momentumTimer.elapsed() : LLONG_MAX;

                // Use Manhattan length for magnitude; direction-agnostic
                const double mag = std::abs(delta.x()) + std::abs(delta.y());
                if (elapsed < graceMs) {
                    // Short time gate immediately after centering
                    event->accept();
                    return;
                }
                if (mag <= noiseGate) {
                    // Ignore tiny jitter
                    event->accept();
                    return;
                }
                if (!m_momentumPrimed) {
                    // First observed delta after centering becomes the baseline and direction
                    m_momentumPrimed = true;
                    m_lastMomentumMag = mag;
                    m_lastMomentumDelta = delta;
                    event->accept();
                    return; // cut immediately
                } else {
                    // If direction stays the same and magnitude does not significantly increase, keep ignoring
                    const bool sameDir = (m_lastMomentumDelta.x() == 0 || (delta.x() == 0) || (std::signbit(m_lastMomentumDelta.x()) == std::signbit(delta.x()))) &&
                                         (m_lastMomentumDelta.y() == 0 || (delta.y() == 0) || (std::signbit(m_lastMomentumDelta.y()) == std::signbit(delta.y())));
                    if (sameDir) {
                        if (mag <= m_lastMomentumMag * boostRatio) {
                            // still decaying or not enough increase: continue suppressing
                            m_lastMomentumMag = mag; // track new mag so we continue to follow the decay
                            m_lastMomentumDelta = delta;
                            event->accept();
                            return;
                        }
                        // Significant boost in same direction -> consider it a new intentional scroll; end suppression
                        m_ignorePanMomentum = false;
                    } else {
                        // Direction changed (user reversed) -> end suppression immediately to honor the user's input
                        m_ignorePanMomentum = false;
                    }
                }
            }
        }
        horizontalScrollBar()->setValue(horizontalScrollBar()->value() - delta.x());
        verticalScrollBar()->setValue(verticalScrollBar()->value() - delta.y());
        event->accept();
        return;
    }
    QGraphicsView::wheelEvent(event);
}

void ScreenCanvas::zoomAroundViewportPos(const QPointF& vpPosF, qreal factor) {
    QPoint vpPos = vpPosF.toPoint();
    if (!viewport()->rect().contains(vpPos)) {
        vpPos = viewport()->rect().center();
    }
    const QPointF sceneAnchor = mapToScene(vpPos);
    // Compose a new transform that scales around the scene anchor directly
    QTransform t = transform();
    t.translate(sceneAnchor.x(), sceneAnchor.y());
    t.scale(factor, factor);
    t.translate(-sceneAnchor.x(), -sceneAnchor.y());
    setTransform(t);
    // After zoom, refresh overlays only for selected items
    if (m_scene) {
        const QList<QGraphicsItem*> sel = m_scene->selectedItems();
        for (QGraphicsItem* it : sel) {
            if (auto* v = dynamic_cast<ResizableVideoItem*>(it)) {
                v->requestOverlayRelayout();
            }
            if (auto* b = dynamic_cast<ResizableMediaBase*>(it)) {
                b->requestLabelRelayout();
            }
        }
    }
}

MainWindow::~MainWindow() {
    if (m_webSocketClient->isConnected()) {
        m_webSocketClient->disconnect();
    }
}

void MainWindow::setupUI() {
    m_centralWidget = new QWidget(this);
    setCentralWidget(m_centralWidget);
    
    m_mainLayout = new QVBoxLayout(m_centralWidget);
    m_mainLayout->setSpacing(0); // Remove spacing, we'll handle it manually
    m_mainLayout->setContentsMargins(0, 0, 0, 0); // Remove margins from main layout
    
    // Top section with margins
    QWidget* topSection = new QWidget();
    QVBoxLayout* topLayout = new QVBoxLayout(topSection);
    topLayout->setContentsMargins(20, 20, 20, 20); // Apply margins only to top section
    topLayout->setSpacing(20);
    
    // Connection section (always visible)
    m_connectionLayout = new QHBoxLayout();
    
    // Back button (left-aligned, initially hidden)
    m_backButton = new QPushButton("← Back to Client List");
    m_backButton->setStyleSheet("QPushButton { padding: 8px 16px; font-weight: bold; }");
    m_backButton->setAutoDefault(false);
    m_backButton->setDefault(false);
    m_backButton->setFocusPolicy(Qt::NoFocus);
    m_backButton->hide(); // Initially hidden, shown only on screen view
    connect(m_backButton, &QPushButton::clicked, this, &MainWindow::onBackToClientListClicked);
    
    // Status label (no "Status:")
    m_connectionStatusLabel = new QLabel("DISCONNECTED");
    m_connectionStatusLabel->setStyleSheet("QLabel { color: red; font-weight: bold; }");

    // Enable/Disable toggle button with fixed width (left of Settings)
    m_connectToggleButton = new QPushButton("Disable");
    m_connectToggleButton->setStyleSheet("QPushButton { padding: 8px 16px; font-weight: bold; }");
    m_connectToggleButton->setFixedWidth(111);
    connect(m_connectToggleButton, &QPushButton::clicked, this, &MainWindow::onEnableDisableClicked);

    // Settings button
    m_settingsButton = new QPushButton("Settings");
    m_settingsButton->setStyleSheet("QPushButton { padding: 8px 16px; font-weight: bold; }");
    connect(m_settingsButton, &QPushButton::clicked, this, &MainWindow::showSettingsDialog);

    // Layout: [back][stretch][status][connect][settings]
    m_connectionLayout->addWidget(m_backButton);
    m_connectionLayout->addStretch();
    m_connectionLayout->addWidget(m_connectionStatusLabel);
    m_connectionLayout->addWidget(m_connectToggleButton);
    m_connectionLayout->addWidget(m_settingsButton);
    
    topLayout->addLayout(m_connectionLayout);
    m_mainLayout->addWidget(topSection);
    
    // Bottom section with margins (no separator line)
    QWidget* bottomSection = new QWidget();
    QVBoxLayout* bottomLayout = new QVBoxLayout(bottomSection);
    bottomLayout->setContentsMargins(20, 20, 20, 20); // Apply margins only to bottom section
    bottomLayout->setSpacing(20);
    
    // Create stacked widget for page navigation
    m_stackedWidget = new QStackedWidget();
    // Block stray key events (like space) at the stack level
    m_stackedWidget->installEventFilter(this);
    bottomLayout->addWidget(m_stackedWidget);
    m_mainLayout->addWidget(bottomSection);
    
    // Create client list page
    createClientListPage();
    
    // Create screen view page  
    createScreenViewPage();
    
    // Start with client list page
    m_stackedWidget->setCurrentWidget(m_clientListPage);

    // Receive remote cursor updates when watching
    connect(m_webSocketClient, &WebSocketClient::cursorPositionReceived, this,
            [this](const QString& targetId, int x, int y) {
                if (m_stackedWidget->currentWidget() == m_screenViewWidget && targetId == m_watchedClientId && m_screenCanvas) {
                    m_screenCanvas->updateRemoteCursor(x, y);
                }
            });
}

void MainWindow::createClientListPage() {
    m_clientListPage = new QWidget();
    QVBoxLayout* layout = new QVBoxLayout(m_clientListPage);
    layout->setSpacing(15);
    layout->setContentsMargins(0, 0, 0, 0);
    
    // Client list section
    m_clientListLabel = new QLabel("Connected Clients:");
    m_clientListLabel->setStyleSheet("QLabel { font-size: 16px; font-weight: bold; }");
    layout->addWidget(m_clientListLabel);
    
    m_clientListWidget = new QListWidget();
    // Use palette-based colors so light/dark themes adapt automatically
    // Add subtle hover effect and remove persistent selection highlight
    m_clientListWidget->setStyleSheet(
        "QListWidget { "
        "   border: 1px solid palette(mid); "
        "   border-radius: 5px; "
        "   padding: 5px; "
        "   background-color: palette(base); "
        "   color: palette(text); "
        "}"
        "QListWidget::item { "
        "   padding: 10px; "
        "   border-bottom: 1px solid palette(midlight); "
        "}" 
        // Hover: very light blue tint
        "QListWidget::item:hover { "
        "   background-color: rgba(74, 144, 226, 28); "
        "}"
        // Suppress active/selected highlight colors
        "QListWidget::item:selected { "
        "   background-color: transparent; "
        "   color: palette(text); "
        "}"
        "QListWidget::item:selected:active { "
        "   background-color: transparent; "
        "   color: palette(text); "
        "}"
        "QListWidget::item:selected:hover { "
        "   background-color: rgba(74, 144, 226, 28); "
        "   color: palette(text); "
        "}"
    );
    connect(m_clientListWidget, &QListWidget::itemClicked, this, &MainWindow::onClientItemClicked);
    // Prevent keyboard (space/enter) from triggering navigation
    m_clientListWidget->setFocusPolicy(Qt::NoFocus);
    m_clientListWidget->installEventFilter(this);
    // Enable hover state over items (for :hover style)
    m_clientListWidget->setMouseTracking(true);
    layout->addWidget(m_clientListWidget);
    
    m_noClientsLabel = new QLabel("No clients connected. Make sure other devices are running Mouffette and connected to the same server.");
    m_noClientsLabel->setStyleSheet("QLabel { color: #666; font-style: italic; text-align: center; }");
    m_noClientsLabel->setAlignment(Qt::AlignCenter);
    m_noClientsLabel->setWordWrap(true);
    layout->addWidget(m_noClientsLabel);
    
    // Selected client info
    m_selectedClientLabel = new QLabel();
    m_selectedClientLabel->setStyleSheet("QLabel { background-color: #e8f4fd; padding: 10px; border-radius: 5px; }");
    m_selectedClientLabel->setWordWrap(true);
    m_selectedClientLabel->hide();
    layout->addWidget(m_selectedClientLabel);
    
    // Add to stacked widget
    m_stackedWidget->addWidget(m_clientListPage);
    
    // Initially hide the separate "no clients" label since we'll show it in the list widget itself
    m_noClientsLabel->hide();
}

void MainWindow::createScreenViewPage() {
    // Screen view page
    m_screenViewWidget = new QWidget();
    m_screenViewLayout = new QVBoxLayout(m_screenViewWidget);
    m_screenViewLayout->setSpacing(15);
    m_screenViewLayout->setContentsMargins(0, 0, 0, 0);
    
    // Header row: hostname on the left, indicators on the right (replaces "Connected Clients:" title)
    QHBoxLayout* headerLayout = new QHBoxLayout();

    m_clientNameLabel = new QLabel();
    m_clientNameLabel->setStyleSheet("QLabel { font-size: 16px; font-weight: bold; color: palette(text); }");
    m_clientNameLabel->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);

    m_volumeIndicator = new QLabel("🔈 --");
    m_volumeIndicator->setStyleSheet("QLabel { font-size: 16px; color: palette(text); font-weight: bold; }");
    m_volumeIndicator->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    m_volumeIndicator->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);

    headerLayout->addWidget(m_clientNameLabel, 0, Qt::AlignLeft);
    headerLayout->addStretch();
    headerLayout->addWidget(m_volumeIndicator, 0, Qt::AlignRight);
    headerLayout->setContentsMargins(0, 0, 0, 0);

    m_screenViewLayout->addLayout(headerLayout);
    
    // Canvas container holds spinner and canvas with a stacked layout
    m_canvasContainer = new QWidget();
    m_canvasContainer->setObjectName("CanvasContainer");
    m_canvasContainer->setMinimumHeight(400);
    // Ensure stylesheet background/border is actually painted
    m_canvasContainer->setAttribute(Qt::WA_StyledBackground, true);
    // Match the dark background used by the client list container via palette(base)
    m_canvasContainer->setStyleSheet(
        "QWidget#CanvasContainer { "
        "   background-color: palette(base); "
        "   border: 1px solid palette(mid); "
        "   border-radius: 5px; "
        "}"
    );
    QVBoxLayout* containerLayout = new QVBoxLayout(m_canvasContainer);
    // Provide real inner padding so child doesn't cover the border area
    containerLayout->setContentsMargins(5,5,5,5);
    containerLayout->setSpacing(0);
    m_canvasStack = new QStackedWidget();
    // Match client list container: base background, no border on inner stack
    m_canvasStack->setStyleSheet(
        "QStackedWidget { "
        "   background-color: palette(base); "
        "   border: none; "
        "}"
    );
    containerLayout->addWidget(m_canvasStack);
    // Clip stack to rounded corners
    m_canvasStack->installEventFilter(this);

    // Spinner page
    m_loadingSpinner = new SpinnerWidget();
    // Initial appearance (easy to tweak):
    m_loadingSpinner->setRadius(22);        // circle radius in px
    m_loadingSpinner->setLineWidth(6);      // line width in px
    m_loadingSpinner->setColor(QColor("#4a90e2")); // brand blue
    m_loadingSpinner->setMinimumSize(QSize(48, 48));
    // Spinner page widget wraps the spinner centered
    QWidget* spinnerPage = new QWidget();
    QVBoxLayout* spinnerLayout = new QVBoxLayout(spinnerPage);
    spinnerLayout->setContentsMargins(0,0,0,0);
    spinnerLayout->setSpacing(0);
    spinnerLayout->addStretch();
    spinnerLayout->addWidget(m_loadingSpinner, 0, Qt::AlignCenter);
    spinnerLayout->addStretch();
    // Spinner page opacity effect & animation (fade entire loader area)
    m_spinnerOpacity = new QGraphicsOpacityEffect(spinnerPage);
    spinnerPage->setGraphicsEffect(m_spinnerOpacity);
    m_spinnerOpacity->setOpacity(0.0);
    m_spinnerFade = new QPropertyAnimation(m_spinnerOpacity, "opacity", this);
    m_spinnerFade->setDuration(m_loaderFadeDurationMs);
    m_spinnerFade->setStartValue(0.0);
    m_spinnerFade->setEndValue(1.0);
    // spinnerPage already created above

    // Canvas page
    QWidget* canvasPage = new QWidget();
    QVBoxLayout* canvasLayout = new QVBoxLayout(canvasPage);
    canvasLayout->setContentsMargins(0,0,0,0);
    canvasLayout->setSpacing(0);
    m_screenCanvas = new ScreenCanvas();
    m_screenCanvas->setMinimumHeight(400);
    // Ensure the viewport background matches and is rounded
    if (m_screenCanvas->viewport()) {
        m_screenCanvas->viewport()->setAttribute(Qt::WA_StyledBackground, true);
        m_screenCanvas->viewport()->setAutoFillBackground(true);
        // No border on the viewport to avoid inner border effects
        m_screenCanvas->viewport()->setStyleSheet("background-color: transparent; border: none;");
    }
    m_screenCanvas->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    // Full updates avoid ghosting when moving scene-level overlays with ItemIgnoresTransformations
    m_screenCanvas->setViewportUpdateMode(QGraphicsView::FullViewportUpdate);
    // Screens are not clickable; canvas supports panning and media placement
    canvasLayout->addWidget(m_screenCanvas);
    // Canvas/content opacity effect & animation (apply to the page, not the QGraphicsView viewport to avoid heavy repaints)
    m_canvasOpacity = new QGraphicsOpacityEffect(canvasPage);
    canvasPage->setGraphicsEffect(m_canvasOpacity);
    m_canvasOpacity->setOpacity(0.0);
    m_canvasFade = new QPropertyAnimation(m_canvasOpacity, "opacity", this);
    m_canvasFade->setDuration(m_fadeDurationMs);
    m_canvasFade->setStartValue(0.0);
    m_canvasFade->setEndValue(1.0);

    // Add pages and container to main layout
    m_canvasStack->addWidget(spinnerPage); // index 0: spinner
    m_canvasStack->addWidget(canvasPage);  // index 1: canvas
    m_canvasStack->setCurrentIndex(1);     // default to canvas page hidden (opacity 0) until data
    m_screenViewLayout->addWidget(m_canvasContainer, 1);
    // Clip container and viewport to rounded corners
    m_canvasContainer->installEventFilter(this);
    if (m_screenCanvas && m_screenCanvas->viewport()) m_screenCanvas->viewport()->installEventFilter(this);

    // Ensure focus is on canvas, and block stray key events
    m_screenViewWidget->installEventFilter(this);
    m_screenCanvas->setFocusPolicy(Qt::StrongFocus);
    m_screenCanvas->installEventFilter(this);
    
    // Send button
    m_sendButton = new QPushButton("Send Media to All Screens");
    m_sendButton->setStyleSheet("QPushButton { padding: 12px 24px; font-weight: bold; background-color: #4a90e2; color: white; border-radius: 5px; }");
    m_sendButton->setEnabled(false); // Initially disabled until media is placed
    connect(m_sendButton, &QPushButton::clicked, this, &MainWindow::onSendMediaClicked);
    // Keep button at bottom, centered
    m_screenViewLayout->addWidget(m_sendButton, 0, Qt::AlignHCenter);
    // Ensure header has no stretch, container expands, button fixed
    m_screenViewLayout->setStretch(0, 0); // header
    m_screenViewLayout->setStretch(1, 1); // container expands
    m_screenViewLayout->setStretch(2, 0); // button fixed

    // Volume label opacity effect & animation
    m_volumeOpacity = new QGraphicsOpacityEffect(m_volumeIndicator);
    m_volumeIndicator->setGraphicsEffect(m_volumeOpacity);
    m_volumeOpacity->setOpacity(0.0);
    m_volumeFade = new QPropertyAnimation(m_volumeOpacity, "opacity", this);
    m_volumeFade->setDuration(m_fadeDurationMs);
    m_volumeFade->setStartValue(0.0);
    m_volumeFade->setEndValue(1.0);

    // Loader delay timer
    m_loaderDelayTimer = new QTimer(this);
    m_loaderDelayTimer->setSingleShot(true);
    
    // Add to stacked widget
    m_stackedWidget->addWidget(m_screenViewWidget);
}

void MainWindow::setupMenuBar() {
    // File menu
    m_fileMenu = menuBar()->addMenu("File");
    
    m_exitAction = new QAction("Quit Mouffette", this);
    m_exitAction->setShortcut(QKeySequence::Quit);
    connect(m_exitAction, &QAction::triggered, this, [this]() {
        if (m_webSocketClient->isConnected()) {
            m_webSocketClient->disconnect();
        }
        QApplication::quit();
    });
    m_fileMenu->addAction(m_exitAction);
    
    // Help menu
    m_helpMenu = menuBar()->addMenu("Help");
    
    m_aboutAction = new QAction("About", this);
    connect(m_aboutAction, &QAction::triggered, this, [this]() {
        QMessageBox::about(this, "About Mouffette", 
            "Mouffette v1.0.0\n\n"
            "A cross-platform media sharing application that allows users to "
            "share and display media on other connected users' screens.\n\n"
            "Built with Qt and WebSocket technology.");
    });
    m_helpMenu->addAction(m_aboutAction);
}

void MainWindow::setupSystemTray() {
    // Create tray icon (no context menu, just click handling)
    m_trayIcon = new QSystemTrayIcon(this);
    
    // Set icon - try to load from resources, fallback to simple icon
    QIcon trayIconIcon(":/icons/mouffette.png");
    if (trayIconIcon.isNull()) {
        // Fallback to simple colored icon
        QPixmap pixmap(16, 16);
        pixmap.fill(Qt::blue);
        trayIconIcon = QIcon(pixmap);
    }
    m_trayIcon->setIcon(trayIconIcon);
    
    // Set tooltip
    m_trayIcon->setToolTip("Mouffette - Media Sharing");
    
    // Connect tray icon activation for non-context menu clicks
    connect(m_trayIcon, &QSystemTrayIcon::activated, this, &MainWindow::onTrayIconActivated);
    
    // Show the tray icon
    m_trayIcon->show();
}

void MainWindow::closeEvent(QCloseEvent *event) {
    if (m_trayIcon && m_trayIcon->isVisible()) {
        // Hide to tray instead of closing
        hide();
        event->ignore();
        
        // Show message first time
        static bool firstHide = true;
        if (firstHide) {
            showTrayMessage("Mouffette", "Application is now running in the background. Click the tray icon to show the window again.");
            firstHide = false;
        }
    } else {
        event->accept();
    }
}

void MainWindow::resizeEvent(QResizeEvent *event) {
    QMainWindow::resizeEvent(event);
    
    // If we're currently showing the screen view and have a canvas with content,
    // recenter the view to maintain good visibility after window resize
    if (m_stackedWidget && m_stackedWidget->currentWidget() == m_screenViewWidget && 
        m_screenCanvas && !m_selectedClient.getScreens().isEmpty()) {
        m_screenCanvas->recenterWithMargin(33);
    }
}

void MainWindow::onTrayIconActivated(QSystemTrayIcon::ActivationReason reason) {
    // Show/hide window on any click (left, right, or double-click)
    switch (reason) {
    case QSystemTrayIcon::Trigger:        // Single left-click
    case QSystemTrayIcon::DoubleClick:    // Double left-click  
    case QSystemTrayIcon::Context:        // Right-click
        {
            const bool minimized = (windowState() & Qt::WindowMinimized);
            const bool hidden = isHidden() || !isVisible();
            if (minimized || hidden) {
                // Reveal and focus the window if minimized or hidden
                if (minimized) {
                    setWindowState(windowState() & ~Qt::WindowMinimized);
                    showNormal();
                } else {
                    show();
                }
                raise();
                activateWindow();
            } else {
                // Fully visible: toggle to hide to tray
                hide();
            }
        }
        break;
    default:
        break;
    }
}

void MainWindow::showTrayMessage(const QString& title, const QString& message) {
    if (m_trayIcon) {
        m_trayIcon->showMessage(title, message, QSystemTrayIcon::Information, 3000);
    }
}

void MainWindow::onEnableDisableClicked() {
    if (!m_webSocketClient) return;
    
    if (m_connectToggleButton->text() == "Disable") {
        // Disable client: disconnect and prevent auto-reconnect
        m_userDisconnected = true;
        m_reconnectTimer->stop(); // Stop any pending reconnection
        if (m_webSocketClient->isConnected()) {
            m_webSocketClient->disconnect();
        }
        m_connectToggleButton->setText("Enable");
    } else {
        // Enable client: allow connections and start connecting
        m_userDisconnected = false;
        m_reconnectAttempts = 0; // Reset reconnection attempts
        connectToServer();
        m_connectToggleButton->setText("Disable");
    }
}

// Settings dialog: server URL with Save/Cancel
void MainWindow::showSettingsDialog() {
    QDialog dialog(this);
    dialog.setWindowTitle("Settings");
    QVBoxLayout* v = new QVBoxLayout(&dialog);
    QLabel* urlLabel = new QLabel("Server URL");
    QLineEdit* urlEdit = new QLineEdit(&dialog);
    if (m_serverUrlConfig.isEmpty()) m_serverUrlConfig = DEFAULT_SERVER_URL;
    urlEdit->setText(m_serverUrlConfig);
    v->addWidget(urlLabel);
    v->addWidget(urlEdit);

    QHBoxLayout* btnRow = new QHBoxLayout();
    btnRow->addStretch();
    QPushButton* cancelBtn = new QPushButton("Cancel");
    QPushButton* saveBtn = new QPushButton("Save");
    btnRow->addWidget(cancelBtn);
    btnRow->addWidget(saveBtn);
    v->addLayout(btnRow);

    connect(cancelBtn, &QPushButton::clicked, &dialog, &QDialog::reject);
    connect(saveBtn, &QPushButton::clicked, this, [this, urlEdit, &dialog]() {
        const QString newUrl = urlEdit->text().trimmed();
        if (!newUrl.isEmpty()) {
            bool changed = (newUrl != (m_serverUrlConfig.isEmpty() ? DEFAULT_SERVER_URL : m_serverUrlConfig));
            m_serverUrlConfig = newUrl;
            if (changed) {
                // Restart connection to apply new server URL
                if (m_webSocketClient->isConnected()) {
                    m_userDisconnected = false; // this is not a manual disconnect, we want reconnect
                    m_webSocketClient->disconnect();
                }
                connectToServer();
            }
        }
        dialog.accept();
    });

    dialog.exec();
}

void MainWindow::connectToServer() {
    const QString url = m_serverUrlConfig.isEmpty() ? DEFAULT_SERVER_URL : m_serverUrlConfig;
    m_webSocketClient->connectToServer(url);
}

void MainWindow::scheduleReconnect() {
    if (m_userDisconnected) {
        return; // Don't reconnect if user disabled the client
    }
    
    // Exponential backoff: 2^attempts seconds, capped at maxReconnectDelay
    int delay = qMin(static_cast<int>(qPow(2, m_reconnectAttempts)) * 1000, m_maxReconnectDelay);
    
    // Add some jitter to avoid thundering herd (±25%)
    int jitter = QRandomGenerator::global()->bounded(-delay/4, delay/4);
    delay += jitter;
    
    qDebug() << "Scheduling reconnect attempt" << (m_reconnectAttempts + 1) << "in" << delay << "ms";
    
    m_reconnectTimer->start(delay);
    m_reconnectAttempts++;
}

void MainWindow::attemptReconnect() {
    if (m_userDisconnected) {
        return; // Don't reconnect if user disabled the client
    }
    
    qDebug() << "Attempting reconnection...";
    connectToServer();
}

void MainWindow::onConnected() {
    setUIEnabled(true);
    // Reset reconnection state on successful connection
    m_reconnectAttempts = 0;
    m_reconnectTimer->stop();
    
    // Sync this client's info with the server
    syncRegistration();
    
    statusBar()->showMessage("Connected to server", 3000);
    
    // Show tray notification
    showTrayMessage("Mouffette Connected", "Successfully connected to Mouffette server");
}

void MainWindow::onDisconnected() {
    setUIEnabled(false);
    
    // Start smart reconnection if client is enabled and not manually disconnected
    if (!m_userDisconnected) {
        scheduleReconnect();
    }
    
    // Stop watching if any
    if (!m_watchedClientId.isEmpty()) {
        m_watchedClientId.clear();
    }
    
    // Clear client list
    m_availableClients.clear();
    updateClientList(m_availableClients);
    
    statusBar()->showMessage("Disconnected from server", 3000);
    
    // Show tray notification
    showTrayMessage("Mouffette Disconnected", "Disconnected from Mouffette server");
}

void MainWindow::startWatchingSelectedClient() {
    if (!m_webSocketClient || !m_webSocketClient->isConnected()) return;
    const QString targetId = m_selectedClient.getId();
    if (targetId.isEmpty()) return;
    if (m_watchedClientId == targetId) return; // already watching
    if (!m_watchedClientId.isEmpty()) {
        m_webSocketClient->unwatchScreens(m_watchedClientId);
    }
    m_webSocketClient->watchScreens(targetId);
    m_watchedClientId = targetId;
}

void MainWindow::stopWatchingCurrentClient() {
    if (!m_webSocketClient || !m_webSocketClient->isConnected()) { m_watchedClientId.clear(); return; }
    if (m_watchedClientId.isEmpty()) return;
    m_webSocketClient->unwatchScreens(m_watchedClientId);
    m_watchedClientId.clear();
}

void MainWindow::onConnectionError(const QString& error) {
    QMessageBox::warning(this, "Connection Error", 
        QString("Failed to connect to server:\n%1").arg(error));
    
    setUIEnabled(false);
    // No direct connect/disconnect buttons anymore
}

void MainWindow::onClientListReceived(const QList<ClientInfo>& clients) {
    qDebug() << "Received client list with" << clients.size() << "clients";
    
    // Check for new clients
    int previousCount = m_availableClients.size();
    m_availableClients = clients;
    updateClientList(clients);
    
    // Show notification if new clients appeared
    if (clients.size() > previousCount && previousCount >= 0) {
        int newClients = clients.size() - previousCount;
        if (newClients > 0) {
            QString message = QString("%1 new client%2 available for sharing")
                .arg(newClients)
                .arg(newClients == 1 ? "" : "s");
            showTrayMessage("New Clients Available", message);
        }
    }
}

void MainWindow::onRegistrationConfirmed(const ClientInfo& clientInfo) {
    m_thisClient = clientInfo;
    qDebug() << "Registration confirmed for:" << clientInfo.getMachineName();
    
    // Request initial client list
    m_webSocketClient->requestClientList();
}

void MainWindow::onClientSelectionChanged() {
    if (m_ignoreSelectionChange) {
        return;
    }
    
    QListWidgetItem* currentItem = m_clientListWidget->currentItem();
    
    if (currentItem) {
        int index = m_clientListWidget->row(currentItem);
        if (index >= 0 && index < m_availableClients.size()) {
            const ClientInfo& client = m_availableClients[index];
            m_selectedClient = client;
            
            // Show the screen view for the selected client
            showScreenView(client);
            if (m_webSocketClient && m_webSocketClient->isConnected()) {
                m_webSocketClient->requestScreens(client.getId());
            }
        }
    } else {
        m_selectedClientLabel->hide();
    }
}

// (Duplicate removed) onScreensInfoReceived is implemented later in the file

void MainWindow::syncRegistration() {
    QString machineName = getMachineName();
    QString platform = getPlatformName();
    QList<ScreenInfo> screens;
    int volumePercent = -1;
    // Only include screens/volume when actively watched; otherwise identity-only
    if (m_isWatched) {
        screens = getLocalScreenInfo();
        volumePercent = getSystemVolumePercent();
    }
    
    qDebug() << "Sync registration:" << machineName << "on" << platform << "with" << screens.size() << "screens";
    
    m_webSocketClient->registerClient(machineName, platform, screens, volumePercent);
}

void MainWindow::onScreensInfoReceived(const ClientInfo& clientInfo) {
    // Update the canvas only if it matches the currently selected client
    if (!clientInfo.getId().isEmpty() && clientInfo.getId() == m_selectedClient.getId()) {
        qDebug() << "Updating canvas with fresh screens for" << clientInfo.getMachineName();
        m_selectedClient = clientInfo; // keep selected client in sync
        
    // Switch to canvas page and stop spinner; show volume with fresh data
    if (m_loaderDelayTimer) m_loaderDelayTimer->stop();
    if (m_canvasStack) m_canvasStack->setCurrentIndex(1);
    if (m_screenCanvas) {
        m_screenCanvas->viewport()->update();
        if (m_screenCanvas->scene()) m_screenCanvas->scene()->update();
    }
    if (m_loadingSpinner) { m_loadingSpinner->stop(); }
        if (m_screenCanvas) {
            m_screenCanvas->setScreens(clientInfo.getScreens());
            m_screenCanvas->recenterWithMargin(33);
            m_screenCanvas->setFocus(Qt::OtherFocusReason);
        }
        // Fade-in canvas content
    if (m_canvasFade && m_canvasOpacity) {
            applyAnimationDurations();
            m_canvasOpacity->setOpacity(0.0);
            m_canvasFade->setStartValue(0.0);
            m_canvasFade->setEndValue(1.0);
            m_canvasFade->start();
        }
        // Update and fade-in volume
        if (m_volumeIndicator) {
            updateVolumeIndicator();
            m_volumeIndicator->show();
            if (m_volumeFade && m_volumeOpacity) {
                applyAnimationDurations();
                m_volumeOpacity->setOpacity(0.0);
                m_volumeFade->setStartValue(0.0);
                m_volumeFade->setEndValue(1.0);
                m_volumeFade->start();
            }
        }
        
        // Optional: refresh UI labels if platform/machine changed
        m_clientNameLabel->setText(QString("%1 (%2)").arg(clientInfo.getMachineName()).arg(clientInfo.getPlatform()));
    }
}

void MainWindow::onWatchStatusChanged(bool watched) {
    // Store watched state locally (as this client being watched by someone else)
    // We don't need a member; we can gate sending by this flag at runtime.
    // For simplicity, keep a static so our timers can read it.
    m_isWatched = watched;
    qDebug() << "Watch status changed:" << (watched ? "watched" : "not watched");

    // Begin/stop sending our cursor position to watchers (target side)
    if (watched) {
        if (!m_cursorTimer) {
            m_cursorTimer = new QTimer(this);
            m_cursorTimer->setInterval(m_cursorUpdateIntervalMs); // configurable
            connect(m_cursorTimer, &QTimer::timeout, this, [this]() {
                static int lastX = INT_MIN, lastY = INT_MIN;
                const QPoint p = QCursor::pos();
                if (p.x() != lastX || p.y() != lastY) {
                    lastX = p.x();
                    lastY = p.y();
                    if (m_webSocketClient && m_webSocketClient->isConnected() && m_isWatched) {
                        m_webSocketClient->sendCursorUpdate(p.x(), p.y());
                    }
                }
            });
        }
        // Apply any updated interval before starting
        m_cursorTimer->setInterval(m_cursorUpdateIntervalMs);
        if (!m_cursorTimer->isActive()) m_cursorTimer->start();
    } else {
        if (m_cursorTimer) m_cursorTimer->stop();
    }
}

QList<ScreenInfo> MainWindow::getLocalScreenInfo() {
    QList<ScreenInfo> screens;
    QList<QScreen*> screenList = QGuiApplication::screens();
    
    for (int i = 0; i < screenList.size(); ++i) {
        QScreen* screen = screenList[i];
        QRect geometry = screen->geometry();
        bool isPrimary = (screen == QGuiApplication::primaryScreen());
        
        ScreenInfo screenInfo(i, geometry.width(), geometry.height(), geometry.x(), geometry.y(), isPrimary);
        screens.append(screenInfo);
    }
    
    return screens;
}

QString MainWindow::getMachineName() {
    QString hostName = QHostInfo::localHostName();
    if (hostName.isEmpty()) {
        hostName = "Unknown Machine";
    }
    return hostName;
}

QString MainWindow::getPlatformName() {
#ifdef Q_OS_MACOS
    return "macOS";
#elif defined(Q_OS_WIN)
    return "Windows";
#elif defined(Q_OS_LINUX)
    return "Linux";
#else
    return "Unknown";
#endif
}

#ifdef Q_OS_MACOS
int MainWindow::getSystemVolumePercent() {
    // Return cached value; updated asynchronously in setupVolumeMonitoring()
    return m_cachedSystemVolume;
}
#elif defined(Q_OS_WIN)
int MainWindow::getSystemVolumePercent() {
    // Use Windows Core Audio APIs (MMDevice + IAudioEndpointVolume)
    // Headers are included only on Windows builds.
    HRESULT hr;
    IMMDeviceEnumerator* pEnum = nullptr;
    IMMDevice* pDevice = nullptr;
    IAudioEndpointVolume* pEndpointVol = nullptr;
    bool coInit = SUCCEEDED(CoInitialize(nullptr));
    int result = -1;
    do {
        hr = CoCreateInstance(CLSID_MMDeviceEnumerator, nullptr, CLSCTX_ALL, IID_IMMDeviceEnumerator, (void**)&pEnum);
        if (FAILED(hr) || !pEnum) break;
        hr = pEnum->GetDefaultAudioEndpoint(eRender, eMultimedia, &pDevice);
        if (FAILED(hr) || !pDevice) break;
        hr = pDevice->Activate(IID_IAudioEndpointVolume, CLSCTX_ALL, nullptr, (void**)&pEndpointVol);
        if (FAILED(hr) || !pEndpointVol) break;
        float volScalar = 0.0f;
        hr = pEndpointVol->GetMasterVolumeLevelScalar(&volScalar);
        if (FAILED(hr)) break;
        int vol = static_cast<int>(std::round(volScalar * 100.0f));
        vol = std::clamp(vol, 0, 100);
        result = vol;
    } while (false);
    if (pEndpointVol) pEndpointVol->Release();
    if (pDevice) pDevice->Release();
    if (pEnum) pEnum->Release();
    if (coInit) CoUninitialize();
    return result;
#elif defined(Q_OS_LINUX)
int MainWindow::getSystemVolumePercent() {
    return -1; // TODO: Implement via PulseAudio/PipeWire if needed
#else
int MainWindow::getSystemVolumePercent() {
    return -1;
}
#endif

void MainWindow::setupVolumeMonitoring() {
#ifdef Q_OS_MACOS
    // Asynchronous polling to avoid blocking the UI thread.
    if (!m_volProc) {
        m_volProc = new QProcess(this);
        // No visible window; ensure fast exit
        connect(m_volProc, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished), this,
                [this](int, QProcess::ExitStatus) {
            const QByteArray out = m_volProc->readAllStandardOutput().trimmed();
            bool ok = false;
            int vol = QString::fromUtf8(out).toInt(&ok);
            if (ok) {
                vol = std::clamp(vol, 0, 100);
                if (vol != m_cachedSystemVolume) {
                    m_cachedSystemVolume = vol;
                    if (m_webSocketClient->isConnected() && m_isWatched) {
                        syncRegistration();
                    }
                }
            }
        });
    }
    if (!m_volTimer) {
        m_volTimer = new QTimer(this);
        m_volTimer->setInterval(1200); // ~1.2s cadence
        connect(m_volTimer, &QTimer::timeout, this, [this]() {
            if (m_volProc->state() == QProcess::NotRunning) {
                m_volProc->start("/usr/bin/osascript", {"-e", "output volume of (get volume settings)"});
            }
        });
        m_volTimer->start();
    }
#else
    // Non-macOS: simple polling; Windows call is fast.
    QTimer* volTimer = new QTimer(this);
    volTimer->setInterval(1200);
    connect(volTimer, &QTimer::timeout, this, [this]() {
        int v = getSystemVolumePercent();
        if (v != m_cachedSystemVolume) {
            m_cachedSystemVolume = v;
            if (m_webSocketClient->isConnected() && m_isWatched) {
                syncRegistration();
            }
        }
    });
    volTimer->start();
#endif
}

void MainWindow::updateClientList(const QList<ClientInfo>& clients) {
    m_clientListWidget->clear();
    
    if (clients.isEmpty()) {
        // Show the "no clients" message centered in the list widget with larger font
        QListWidgetItem* item = new QListWidgetItem("No clients connected. Make sure other devices are running Mouffette and connected to the same server.");
        item->setFlags(Qt::NoItemFlags); // Make it non-selectable and non-interactive
        item->setTextAlignment(Qt::AlignCenter);
        QFont font = item->font();
        font.setItalic(true);
        font.setPointSize(16); // Make the font larger
        item->setFont(font);
        item->setForeground(QColor(102, 102, 102)); // #666 color
        
    // Set a custom size hint to center the item vertically in the list widget.
    // Use the viewport height (content area) to avoid off-by-margins that cause scrollbars.
    const int viewportH = m_clientListWidget->viewport() ? m_clientListWidget->viewport()->height() : m_clientListWidget->height();
    item->setSizeHint(QSize(m_clientListWidget->width(), qMax(0, viewportH)));
        
        m_clientListWidget->addItem(item);
    // Ensure no scrollbars are shown for the single placeholder item
    m_clientListWidget->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_clientListWidget->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        m_noClientsLabel->hide(); // Hide the separate label since we show message in list
    } else {
        m_noClientsLabel->hide();
    // Restore scrollbar policies when there are items to potentially scroll
    m_clientListWidget->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    m_clientListWidget->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        
        for (const ClientInfo& client : clients) {
            QString displayText = client.getDisplayText();
            QListWidgetItem* item = new QListWidgetItem(displayText);
            item->setToolTip(QString("ID: %1\nStatus: %2").arg(client.getId()).arg(client.getStatus()));
            m_clientListWidget->addItem(item);
        }
    }
    
    // Hide selected client info when list changes
    m_selectedClientLabel->hide();
}

void MainWindow::setUIEnabled(bool enabled) {
    // Client list depends on connection
    m_clientListWidget->setEnabled(enabled);
}

void MainWindow::updateConnectionStatus() {
    QString status = m_webSocketClient->getConnectionStatus();
    // Always display status in uppercase
    m_connectionStatusLabel->setText(status.toUpper());
    
    if (status == "Connected") {
        m_connectionStatusLabel->setStyleSheet("QLabel { color: green; font-weight: bold; }");
    } else if (status.startsWith("Connecting") || status.startsWith("Reconnecting")) {
        m_connectionStatusLabel->setStyleSheet("QLabel { color: orange; font-weight: bold; }");
    } else {
        m_connectionStatusLabel->setStyleSheet("QLabel { color: red; font-weight: bold; }");
    }
}


