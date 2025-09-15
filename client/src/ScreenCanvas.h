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

class QMimeData;
class QMediaPlayer;
class QVideoSink;
class QAudioOutput;
class QVariantAnimation;

// Extracted canvas that manages screen layout, zoom/pan, drag&drop previews, and media interaction.
class ScreenCanvas : public QGraphicsView {
    Q_OBJECT
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

protected:
    bool event(QEvent* event) override;
    bool viewportEvent(QEvent* event) override; // handle native gestures delivered to viewport (macOS pinch)
    void keyPressEvent(QKeyEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseDoubleClickEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void wheelEvent(QWheelEvent* event) override;
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
    QRectF screensBoundingRect() const;
    void zoomAroundViewportPos(const QPointF& vpPos, qreal factor);
    void ensureZOrder();

    QGraphicsScene* m_scene = nullptr;
    QList<QGraphicsRectItem*> m_screenItems;
    QList<ScreenInfo> m_screens;
    bool m_panning = false;
    QPoint m_lastPanPoint;
    QPoint m_lastMousePos;
    bool m_overlayMouseDown = false;
    bool m_nativePinchActive = false;
    QTimer* m_nativePinchGuardTimer = nullptr;
    bool m_ignorePanMomentum = false;
    bool m_momentumPrimed = false;
    double m_lastMomentumMag = 0.0;
    QPoint m_lastMomentumDelta;
    QElapsedTimer m_momentumTimer;
    QGraphicsEllipseItem* m_remoteCursorDot = nullptr;
    double m_scaleFactor = 0.2;
    int m_mediaHandleSelectionSizePx = 30;
    int m_mediaHandleVisualSizePx = 12;
    int m_screenBorderWidthPx = 1;

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
};

#endif // SCREENCANVAS_H
