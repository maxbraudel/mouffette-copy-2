// MediaItems.h - Extracted media item hierarchy from MainWindow.cpp
#pragma once

// Qt core/graphics includes
#include <QGraphicsItem>
#include <QUuid>
#include <QGraphicsRectItem>
#include <QtSvgWidgets/QGraphicsSvgItem>
#include <QPixmap>
#include <QImage>
#include <QVideoFrame>
#include <QVideoSink>
#include <QSet>
#include <QTimer>
#include <QVariantAnimation>
#include <QMutex>
#include <QAtomicInteger>
#include <QRunnable>
#include <atomic>
#include <memory>
#include <cmath>
#include <functional>

#include "OverlayPanels.h"
#include "RoundedRectItem.h"
#include "FileManager.h"

class QMediaPlayer;
class QAudioOutput;
class MediaSettingsPanel;

// Base resizable media item (image/video) providing selection chrome, resize handles, and overlay panels.
class ResizableMediaBase : public QGraphicsItem {
public:
    enum class UploadState { NotUploaded, Uploading, Uploaded };
    // Resize handles (now public so external helpers like ScreenCanvas can reference them)
    enum Handle { None, TopLeft, TopRight, BottomLeft, BottomRight, LeftMid, RightMid, TopMid, BottomMid };
    ~ResizableMediaBase() override;
    explicit ResizableMediaBase(const QSize& baseSizePx, int visualSizePx, int selectionSizePx, const QString& filename = QString());

    void setSourcePath(const QString& p);
    QString sourcePath() const { return m_sourcePath; }
    // Stable unique identifier for this media item (persists across uploads)
    QString mediaId() const { return m_mediaId; }
    // Shared file identifier (multiple media items can have same fileId)
    QString fileId() const { return m_fileId; }
    void setFileId(const QString& fileId) { m_fileId = fileId; }
    // Display name shown in overlays: filename if set, otherwise derived from sourcePath
    QString displayName() const;
    // Native media base size in pixels (unscaled)
    QSize baseSizePx() const { return m_baseSize; }

    // Upload status API (non-QObject notification via static callback)
    UploadState uploadState() const { return m_uploadState; }
    int uploadProgress() const { return m_uploadProgress; } // 0..100 when Uploading
    void setUploadNotUploaded() { m_uploadState = UploadState::NotUploaded; m_uploadProgress = 0; notifyUploadChanged(); }
    void setUploadUploading(int progress) { m_uploadState = UploadState::Uploading; m_uploadProgress = std::clamp(progress, 0, 100); notifyUploadChanged(); }
    void setUploadUploaded() { m_uploadState = UploadState::Uploaded; m_uploadProgress = 100; notifyUploadChanged(); }
    static void setUploadChangedNotifier(std::function<void()> cb) { s_uploadChangedNotifier = std::move(cb); }
    
    // File error callback: called when a media item detects its source file is missing/corrupted
    static void setFileErrorNotifier(std::function<void(ResizableMediaBase*)> cb) { s_fileErrorNotifier = std::move(cb); }
    void notifyFileError() { if (s_fileErrorNotifier) s_fileErrorNotifier(this); }

    static void setHeightOfMediaOverlaysPx(int px); // global override height (px) for overlays
    static int  getHeightOfMediaOverlaysPx();
    static void setCornerRadiusOfMediaOverlaysPx(int px);
    static int  getCornerRadiusOfMediaOverlaysPx();

    void requestLabelRelayout();
    bool isOnHandleAtItemPos(const QPointF& itemPos) const; // used by view for cursor decision
    bool beginResizeAtScenePos(const QPointF& scenePos);    // start interactive resize
    Qt::CursorShape cursorForScenePos(const QPointF& scenePos) const;
    bool isActivelyResizing() const { return m_activeHandle != None; }
    void setHandleVisualSize(int px);
    void setHandleSelectionSize(int px);

    // Exposed so ScreenCanvas can relayout overlays after zoom changes.
    void updateOverlayLayout();
    void updateOverlayVisibility();

    // Grid unit control: 1 scene "pixel" in view coordinates (e.g., ScreenCanvas scale)
    static void setSceneGridUnit(double u);
    static double sceneGridUnit();
    
    // Snap-to-screen integration (set by ScreenCanvas)
    static void setScreenSnapCallback(std::function<QPointF(const QPointF&, const QRectF&, bool)> callback);
    static std::function<QPointF(const QPointF&, const QRectF&, bool)> screenSnapCallback();
    static void setResizeSnapCallback(std::function<qreal(qreal, const QPointF&, const QPointF&, const QSize&, bool)> callback);
    static std::function<qreal(qreal, const QPointF&, const QPointF&, const QSize&, bool)> resizeSnapCallback();

    // Access to top overlay panel (filename + utility buttons).
    OverlayPanel* topPanel() const { return m_topPanel.get(); }

    // Called by view prior to removing item from scene & scheduling deletion.
    // Default implementation cancels interactive state & hides overlays.
    virtual void prepareForDeletion();
    bool isBeingDeleted() const { return m_beingDeleted; }
    // Visibility toggle (content only; overlays & selection chrome remain)
    void setContentVisible(bool v) { m_contentVisible = v; update(); if (m_topPanel) m_topPanel->setVisible(true); }
    bool isContentVisible() const { return m_contentVisible; }
    void setContentOpacity(qreal op) { m_contentOpacity = std::clamp(op, 0.0, 1.0); update(); }
    qreal contentOpacity() const { return m_contentOpacity; }
    // Effective display opacity = user contentOpacity() * m_contentDisplayOpacity (animation multiplier)
    qreal animatedDisplayOpacity() const { return m_contentDisplayOpacity; }
    // Fading helpers (seconds can be fractional). If duration <= 0, apply immediately.
    void fadeContentIn(double seconds);
    void fadeContentOut(double seconds);
    void cancelFade();
    
    // Override in derived classes to indicate media type for settings panel
    virtual bool isVideoMedia() const { return false; }

protected:
    QSize m_baseSize;
    Handle m_activeHandle = None;
    QPointF m_fixedItemPoint;  // item coords
    QPointF m_fixedScenePoint; // scene coords
    qreal m_initialScale = 1.0;
    qreal m_initialGrabDist = 1.0;
    int m_visualSize = 8;      // display size of handles (px)
    int m_selectionSize = 12;  // hit zone size (px)
    QString m_sourcePath;      // original path if any
    QString m_filename;
    QString m_mediaId; // persistent unique id for the canvas item
    QString m_fileId;  // shared file id (multiple media can have same fileId)
    std::unique_ptr<OverlayPanel> m_topPanel; // legacy bottom panel removed
    OverlayStyle m_overlayStyle;
    static int heightOfMediaOverlays;
    static int cornerRadiusOfMediaOverlays;
    // Per-media settings panel (absolute, docked left)
    std::unique_ptr<MediaSettingsPanel> m_settingsPanel;

    // Core paint helpers
    void paintSelectionAndLabel(QPainter* painter);

    // Overlay helpers
    void initializeOverlays();
    qreal toItemLengthFromPixels(int px) const;
    Handle hitTestHandle(const QPointF& p) const;
    Handle opposite(Handle h) const;
    QPointF handlePoint(Handle h) const;

    // Subclass hook for live geometry changes (resize/drag) to keep overlays glued
    virtual void onInteractiveGeometryChanged() {}

    // QGraphicsItem overrides
    QRectF boundingRect() const override;
    QPainterPath shape() const override;
    QVariant itemChange(GraphicsItemChange change, const QVariant &value) override;
    void mousePressEvent(QGraphicsSceneMouseEvent* event) override;
    void mouseMoveEvent(QGraphicsSceneMouseEvent* event) override;
    void mouseReleaseEvent(QGraphicsSceneMouseEvent* event) override;
    void hoverMoveEvent(QGraphicsSceneHoverEvent* event) override;
    void hoverLeaveEvent(QGraphicsSceneHoverEvent* event) override;

private:
    void notifyUploadChanged() { if (s_uploadChangedNotifier) s_uploadChangedNotifier(); }
    static std::function<void()> s_uploadChangedNotifier;
    static std::function<void(ResizableMediaBase*)> s_fileErrorNotifier;
    UploadState m_uploadState = UploadState::NotUploaded;
    int m_uploadProgress = 0;
    void relayoutIfNeeded(); // helper to keep overlays positioned (unused externally)
    static double s_sceneGridUnit;
    static std::function<QPointF(const QPointF&, const QRectF&, bool)> s_screenSnapCallback;
    static std::function<qreal(qreal, const QPointF&, const QPointF&, const QSize&, bool)> s_resizeSnapCallback;
    static inline double snapToGrid(double v) { const double u = (s_sceneGridUnit > 1e-9 ? s_sceneGridUnit : 1.0); return std::round(v / u) * u; }
    static inline QPointF snapPointToGrid(const QPointF& p) { return QPointF(snapToGrid(p.x()), snapToGrid(p.y())); }
protected:
    bool m_beingDeleted = false;
    bool m_contentVisible = true; // controlled by visibility toggle overlay button
    qreal m_contentOpacity = 1.0; // multiplicative opacity for content only
    qreal m_contentDisplayOpacity = 1.0; // animated multiplier (0..1) for fade in/out
    QVariantAnimation* m_fadeAnimation = nullptr; // owned manually (ResizableMediaBase not QObject)
};

// Simple pixmap media item
class ResizablePixmapItem : public ResizableMediaBase {
public:
    explicit ResizablePixmapItem(const QPixmap& pm, int visualSizePx, int selectionSizePx, const QString& filename = QString());
    void paint(QPainter* painter, const QStyleOptionGraphicsItem* option, QWidget* widget) override;
private:
    QPixmap m_pix;
};

// Forward declaration for async worker
class FrameConversionWorker;

// Video media item with in-item controls overlays & performance instrumentation
class ResizableVideoItem : public ResizableMediaBase {
public:
    explicit ResizableVideoItem(const QString& filePath, int visualSizePx, int selectionSizePx, const QString& filename = QString(), int controlsFadeMs = 140);
    ~ResizableVideoItem() override;

    // Public control helpers used by ScreenCanvas (drag gestures etc.)
    void togglePlayPause();
    void toggleRepeat();
    void toggleMute();
    void stopToBeginning();
    void seekToRatio(qreal r);
    void setInitialScaleFactor(qreal f) { m_initialScaleFactor = f; }
    void setExternalPosterImage(const QImage& img);
    bool isDraggingProgress() const { return m_draggingProgress; }
    bool isDraggingVolume() const { return m_draggingVolume; }
    void updateDragWithScenePos(const QPointF& scenePos);
    void endDrag();
    void requestOverlayRelayout() { updateControlsLayout(); }

    // Performance / diagnostics
    void setFrameProcessingBudget(int ms) { m_frameProcessBudgetMs = std::max(1, ms); }
    void setRepaintBudget(int ms) { m_repaintBudgetMs = std::max(1, ms); }
    void getFrameStats(int& received, int& processed, int& skipped) const;
    void getFrameStatsExtended(int& received, int& processed, int& skipped, int& dropped, int& conversionsStarted, int& conversionsCompleted) const;
    void resetFrameStats();
    void onFrameConversionComplete(QImage convertedImage, quint64 serial); // async callback
    bool handleControlsPressAtItemPos(const QPointF& itemPos); // view-level forwarding

    // QGraphicsItem
    void paint(QPainter* painter, const QStyleOptionGraphicsItem* option, QWidget* widget) override;
    QRectF boundingRect() const override;
    QPainterPath shape() const override;
    
    // Override to indicate this is video media
    bool isVideoMedia() const override { return true; }
    void mousePressEvent(QGraphicsSceneMouseEvent* event) override;
    void mouseDoubleClickEvent(QGraphicsSceneMouseEvent* event) override;
    QVariant itemChange(GraphicsItemChange change, const QVariant &value) override;
    void mouseMoveEvent(QGraphicsSceneMouseEvent* event) override;
    void mouseReleaseEvent(QGraphicsSceneMouseEvent* event) override;

    void prepareForDeletion() override; // extra cleanup (stop timers, hide controls)

protected:
    void onInteractiveGeometryChanged() override;

private:
    void maybeAdoptFrameSize(const QVideoFrame& f);
    void adoptBaseSize(const QSize& sz);
    void setControlsVisible(bool show);
    void updateControlsLayout();
    bool isVisibleInAnyView() const;
    bool shouldProcessFrame() const; // currently unused (kept for future tuning)
    bool shouldRepaint() const;
    void logFrameStats() const;
    void updateProgressBar();

    qreal baseWidth() const { return static_cast<qreal>(m_baseSize.width()); }
    qreal baseHeight() const { return static_cast<qreal>(m_baseSize.height()); }

    QMediaPlayer* m_player = nullptr;
    QAudioOutput* m_audio = nullptr;
    QVideoSink* m_sink = nullptr;
    QVideoFrame m_lastFrame;
    QImage m_lastFrameImage;
    qint64 m_durationMs = 0;
    qint64 m_positionMs = 0;
    bool m_primingFirstFrame = false;
    bool m_firstFramePrimed = false;
    bool m_savedMuted = false;
    QImage m_posterImage; bool m_posterImageSet = false;
    QGraphicsRectItem* m_controlsBg = nullptr;
    RoundedRectItem* m_playBtnRectItem = nullptr; QGraphicsSvgItem* m_playIcon = nullptr; QGraphicsSvgItem* m_pauseIcon = nullptr;
    RoundedRectItem* m_stopBtnRectItem = nullptr; QGraphicsSvgItem* m_stopIcon = nullptr;
    RoundedRectItem* m_repeatBtnRectItem = nullptr; QGraphicsSvgItem* m_repeatIcon = nullptr;
    RoundedRectItem* m_muteBtnRectItem = nullptr; QGraphicsSvgItem* m_muteIcon = nullptr; QGraphicsSvgItem* m_muteSlashIcon = nullptr;
    QGraphicsRectItem* m_volumeBgRectItem = nullptr; QGraphicsRectItem* m_volumeFillRectItem = nullptr;
    QGraphicsRectItem* m_progressBgRectItem = nullptr; QGraphicsRectItem* m_progressFillRectItem = nullptr;
    bool m_adoptedSize = false; qreal m_initialScaleFactor = 1.0;
    QRectF m_playBtnRectItemCoords; QRectF m_stopBtnRectItemCoords; QRectF m_repeatBtnRectItemCoords; QRectF m_muteBtnRectItemCoords; QRectF m_volumeRectItemCoords; QRectF m_progRectItemCoords;
    bool m_repeatEnabled = false;
    bool m_draggingProgress = false; bool m_draggingVolume = false; bool m_holdLastFrameAtEnd = false;
    QTimer* m_progressTimer = nullptr; qreal m_smoothProgressRatio = 0.0; bool m_seeking = false;
    bool m_controlsLockedUntilReady = true; int m_controlsFadeMs = 140; QVariantAnimation* m_controlsFadeAnim = nullptr; bool m_controlsDidInitialFade = false;
    qint64 m_lastFrameProcessMs = 0; qint64 m_lastRepaintMs = 0; int m_frameProcessBudgetMs = 16; int m_repaintBudgetMs = 16;
    mutable int m_framesReceived = 0; mutable int m_framesProcessed = 0; mutable int m_framesSkipped = 0;
    std::atomic<bool> m_conversionBusy{false}; QVideoFrame m_pendingFrame; QMutex m_frameMutex; std::atomic<quint64> m_frameSerial{0}; quint64 m_lastProcessedSerial = 0;
    mutable int m_framesDropped = 0; mutable int m_conversionsStarted = 0; mutable int m_conversionsCompleted = 0;
};

// Worker used for asynchronous frame conversion to ARGB images; posts back to main thread.
class FrameConversionWorker : public QRunnable {
public:
    FrameConversionWorker(ResizableVideoItem* item, QVideoFrame frame, quint64 serial);
    void run() override;
    static void registerItem(ResizableVideoItem* item);
    static void unregisterItem(ResizableVideoItem* item);
    static QSet<uintptr_t> s_activeItems;
private:
    uintptr_t m_itemPtr; QVideoFrame m_frame; quint64 m_serial;
};
