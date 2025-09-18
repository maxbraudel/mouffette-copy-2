#ifndef SCREENCANVAS_H
#define SCREENCANVAS_H

#include <QGraphicsView>
#include <QGraphicsRectItem>
#include <QGraphicsEllipseItem>
#include <QElapsedTimer>
#include <QMap>
#include <QPixmap>
#include <QTimer>
#include <QPointer>
#include "ClientInfo.h" // for ScreenInfo
#include "MediaItems.h" // for ResizableMediaBase / ResizableVideoItem
#include <QGestureEvent>
#include <QPinchGesture>
class QLabel;
class QVBoxLayout;
class MouseBlockingRoundedRectItem;
class QGraphicsProxyWidget; // kept for other uses, but not used by info overlay anymore

class QMimeData;
class QMediaPlayer;
class QVideoSink;
class QAudioOutput;
class QVariantAnimation;
class QResizeEvent;

// Extracted canvas that manages screen layout, zoom/pan, drag&drop previews, and media interaction.
class ScreenCanvas : public QGraphicsView {
    Q_OBJECT
public:
    ~ScreenCanvas() override;
public:
    explicit ScreenCanvas(QWidget* parent = nullptr);
    void setScreens(const QList<ScreenInfo>& screens);
    void clearScreens();
    void recenterWithMargin(int marginPx = 33);
    void setDragPreviewFadeDurationMs(int ms) { m_dragPreviewFadeMs = qMax(0, ms); }
    void setVideoControlsFadeDurationMs(int ms) { m_videoControlsFadeMs = qMax(0, ms); }
    void updateRemoteCursor(int globalX, int globalY);
    void hideRemoteCursor();
    void setMediaHandleSelectionSizePx(int px); // hit area
    void setMediaHandleVisualSizePx(int px);    // drawn square
    void setMediaHandleSizePx(int px);          // convenience
    void setScreenBorderWidthPx(int px);
    void setScreenLabelFontPointSize(int pt) { m_screenLabelFontPt = qMax(1, pt); createScreenItems(); }
    int screenLabelFontPointSize() const { return m_screenLabelFontPt; }
    void setScreenSpacingPx(int px) { m_screenSpacingPx = qMax(0, px); createScreenItems(); }
    int screenSpacingPx() const { return m_screenSpacingPx; }
    // Remote cursor style setters
    void setRemoteCursorDiameterPx(int d) { m_remoteCursorDiameterPx = qMax(2, d); if (m_remoteCursorDot) { recreateRemoteCursorItem(); } }
    void setRemoteCursorFillColor(const QColor& c) { m_remoteCursorFill = c; if (m_remoteCursorDot) { m_remoteCursorDot->setBrush(m_remoteCursorFill); } }
    void setRemoteCursorBorderColor(const QColor& c) { m_remoteCursorBorder = c; if (m_remoteCursorDot) { QPen p = m_remoteCursorDot->pen(); p.setColor(m_remoteCursorBorder); m_remoteCursorDot->setPen(p); } }
    void setRemoteCursorBorderWidthPx(qreal w) { m_remoteCursorBorderWidth = std::max<qreal>(0.0, w); if (m_remoteCursorDot) { QPen p = m_remoteCursorDot->pen(); p.setWidthF(m_remoteCursorBorderWidth); m_remoteCursorDot->setPen(p); } }
    int remoteCursorDiameterPx() const { return m_remoteCursorDiameterPx; }
    QColor remoteCursorFillColor() const { return m_remoteCursorFill; }
    QColor remoteCursorBorderColor() const { return m_remoteCursorBorder; }
    qreal remoteCursorBorderWidthPx() const { return m_remoteCursorBorderWidth; }
    void setRemoteCursorFixedSize(bool fixed) { m_remoteCursorFixedSize = fixed; if (m_remoteCursorDot) recreateRemoteCursorItem(); }
    bool remoteCursorFixedSize() const { return m_remoteCursorFixedSize; }

    // Snap-to-screen configuration
    void setSnapDistancePx(int px) { m_snapDistancePx = qMax(1, px); }
    int snapDistancePx() const { return m_snapDistancePx; }
    
    // Z-order management for media items (public interface)
    void moveMediaUp(QGraphicsItem* item);
    void moveMediaDown(QGraphicsItem* item);

protected:
    bool event(QEvent* event) override;
    bool viewportEvent(QEvent* event) override; // handle native gestures delivered to viewport (macOS pinch)
    void keyPressEvent(QKeyEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseDoubleClickEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void wheelEvent(QWheelEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    void dragEnterEvent(QDragEnterEvent* event) override;
    void dragMoveEvent(QDragMoveEvent* event) override;
    void dragLeaveEvent(QDragLeaveEvent* event) override;
    void dropEvent(QDropEvent* event) override;

private:
    bool gestureEvent(QGestureEvent* event);
    void ensureDragPreview(const QMimeData* mime);
    void updateDragPreviewPos(const QPointF& scenePos);
    void clearDragPreview();
    QPixmap makeVideoPlaceholderPixmap(const QSize& pxSize);
    void startVideoPreviewProbe(const QString& localFilePath);
    void startVideoPreviewProbeFallback(const QString& localFilePath);
    void stopVideoPreviewProbe();
    void startDragPreviewFadeIn();
    void stopDragPreviewFade();
    void onFastVideoThumbnailReady(const QImage& img);

    void createScreenItems();
    QGraphicsRectItem* createScreenItem(const ScreenInfo& screen, int index, const QRectF& position);
    QMap<int, QRectF> calculateCompactPositions(double scaleFactor, double hSpacing, double vSpacing) const;
    // Map remote global cursor position (remote screen coords) to scene space using stored screen layout.
    QPointF mapRemoteCursorToScene(int remoteX, int remoteY) const;
    QRectF screensBoundingRect() const;
    void zoomAroundViewportPos(const QPointF& vpPos, qreal factor);
    void ensureZOrder();
    void debugLogScreenSizes() const; // helper to verify screen rect pixel parity
    void recreateRemoteCursorItem();
    // Info overlay removed
    
    // Snap-to-screen helpers
    QPointF snapToScreenBorders(const QPointF& scenePos, const QRectF& mediaBounds, bool shiftPressed) const;
    qreal snapResizeToScreenBorders(qreal currentScale, const QPointF& fixedCorner, const QPointF& fixedItemPoint, const QSize& baseSize, bool shiftPressed) const;
    QList<QRectF> getScreenBorderRects() const;

    QGraphicsScene* m_scene = nullptr;
    QList<QGraphicsRectItem*> m_screenItems;
    QList<ScreenInfo> m_screens;
    // Mapping screen id -> scene rect (inner content rect used when drawing label)
    QHash<int, QRectF> m_sceneScreenRects;
    bool m_panning = false;
    QPoint m_lastPanPoint;
    QPoint m_lastMousePos;
    // Precise panning: keep the scene point under the cursor anchored during drag
    QPointF m_panAnchorScene;   // scene position under the cursor at mouse press
    QPoint  m_panAnchorView;    // viewport position at mouse press (for reference)
    bool m_overlayMouseDown = false;
    bool m_nativePinchActive = false;
    QTimer* m_nativePinchGuardTimer = nullptr;
    bool m_ignorePanMomentum = false;
    bool m_momentumPrimed = false;
    double m_lastMomentumMag = 0.0;
    QPoint m_lastMomentumDelta;
    QElapsedTimer m_momentumTimer;
    QGraphicsEllipseItem* m_remoteCursorDot = nullptr;
    // Remote cursor styling
    int m_remoteCursorDiameterPx = 30;
    QColor m_remoteCursorFill = Qt::white;
    QColor m_remoteCursorBorder = QColor(0,0,0,230);
    qreal m_remoteCursorBorderWidth = 2;
    bool m_remoteCursorFixedSize = false; // when false, cursor scales with zoom
    // Global initial media scale: set to 1.0 for 1:1 pixel parity between screens and imported media
    double m_scaleFactor = 1.0;
    int m_mediaHandleSelectionSizePx = 30;
    int m_mediaHandleVisualSizePx = 12;
    int m_screenBorderWidthPx = 1;
    int m_screenLabelFontPt = 48;
    int m_screenSpacingPx = 0; // horizontal & vertical spacing between screens

    QGraphicsItem* m_dragPreviewItem = nullptr;
    QSize m_dragPreviewBaseSize;
    bool m_dragPreviewIsVideo = false;
    QPixmap m_dragPreviewPixmap;
    bool m_dragCursorHidden = false;
    QPointF m_dragPreviewLastScenePos;
    int m_dragPreviewFadeMs = 180;
    QVariantAnimation* m_dragPreviewFadeAnim = nullptr;
    qreal m_dragPreviewTargetOpacity = 0.85;
    int m_videoControlsFadeMs = 140;
    QMediaPlayer* m_dragPreviewPlayer = nullptr;
    QVideoSink* m_dragPreviewSink = nullptr;
    QAudioOutput* m_dragPreviewAudio = nullptr;
    bool m_dragPreviewGotFrame = false;
    QTimer* m_dragPreviewFallbackTimer = nullptr;
    
    // Snap-to-screen settings
    int m_repaintBudgetMs = 16;
    int m_snapDistancePx = 10; // pixels within which snapping occurs
    
    // Z-order management for media items
    qreal m_nextMediaZValue = 1.0;
    void assignNextZValue(QGraphicsItem* item);
    QList<QGraphicsItem*> getMediaItemsSortedByZ() const;

    // Info overlay removed
};

#endif // SCREENCANVAS_H
