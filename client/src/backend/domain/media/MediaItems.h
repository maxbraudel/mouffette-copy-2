// MediaItems.h - Extracted media item hierarchy from MainWindow.cpp
#pragma once

// Qt core/graphics includes
#include <QGraphicsItem>
#include <QUuid>
#include <QGraphicsRectItem>
#include <QtSvgWidgets/QGraphicsSvgItem>
#include <QPixmap>
#include <QImage>
#include <QString>
#include <QVideoFrame>
#include <QSizeF>
#include <QVideoSink>
#include <QMediaPlayer>
#include <QAudioOutput>
#include <QSet>
#include <QTimer>
#include <QVariantAnimation>
#include <memory>
#include <cmath>
#include <functional>

#include "frontend/rendering/canvas/OverlayPanels.h"
#include "frontend/rendering/canvas/RoundedRectItem.h"
#include "backend/files/FileManager.h"

class QMediaPlayer;
class QAudioOutput;
class MediaSettingsPanel;

// Base resizable media item (image/video) providing selection chrome, resize handles, and overlay panels.
class ResizableMediaBase : public QGraphicsItem {
public:
    struct MediaSettingsState {
        bool displayAutomatically = true;
        bool displayDelayEnabled = false;
        QString displayDelayText = QStringLiteral("1");
    bool unmuteAutomatically = true;
    bool unmuteDelayEnabled = false;
    QString unmuteDelayText = QStringLiteral("0");
        bool playAutomatically = true;
        bool playDelayEnabled = false;
        QString playDelayText = QStringLiteral("1");
        bool pauseDelayEnabled = false;
        QString pauseDelayText = QStringLiteral("1");
        bool repeatEnabled = false;
        QString repeatCountText = QStringLiteral("1");
        bool fadeInEnabled = false;
        QString fadeInText = QStringLiteral("1");
    bool fadeOutEnabled = false;
    QString fadeOutText = QStringLiteral("1");
    bool audioFadeInEnabled = false;
    QString audioFadeInText = QStringLiteral("1");
    bool audioFadeOutEnabled = false;
    QString audioFadeOutText = QStringLiteral("1");
        bool opacityOverrideEnabled = false;
        QString opacityText = QStringLiteral("100");
        bool volumeOverrideEnabled = false;
        QString volumeText = QStringLiteral("100");
        bool hideDelayEnabled = false;
        QString hideDelayText = QStringLiteral("1");
        bool hideWhenVideoEnds = false;
        bool muteDelayEnabled = false;
        QString muteDelayText = QStringLiteral("1");
        bool muteWhenVideoEnds = false;
    };

    enum class UploadState { NotUploaded, Uploading, Uploaded };
    // Resize handles (now public so external helpers like ScreenCanvas can reference them)
    enum Handle { None, TopLeft, TopRight, BottomLeft, BottomRight, LeftMid, RightMid, TopMid, BottomMid };
    ~ResizableMediaBase() override;
    explicit ResizableMediaBase(const QSize& baseSizePx, int visualSizePx, int selectionSizePx, const QString& filename = QString());

    // Lifetime guard for external schedulers storing raw pointers (e.g., delayed timers).
    std::weak_ptr<bool> lifetimeGuard() const;

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
    
    // Phase 4.3: FileManager injected (not singleton) - static setter for all media items
    static void setFileManager(FileManager* manager) { s_fileManager = manager; }
    
    // File error callback: called when a media item detects its source file is missing/corrupted
    static void setFileErrorNotifier(std::function<void(ResizableMediaBase*)> cb) { s_fileErrorNotifier = std::move(cb); }
    void notifyFileError() { if (s_fileErrorNotifier) s_fileErrorNotifier(this); }

    static void setHeightOfMediaOverlaysPx(int px); // global override height (px) for overlays
    static int  getHeightOfMediaOverlaysPx();
    static void setCornerRadiusOfMediaOverlaysPx(int px);
    static int  getCornerRadiusOfMediaOverlaysPx();

    void requestLabelRelayout();

    // Programmatic visibility control honoring fade settings (used by host scene auto display)
    void showWithConfiguredFade();
    void hideWithConfiguredFade();
    void showImmediateNoFade();
    void hideImmediateNoFade();

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
    static void setScreenSnapCallback(std::function<QPointF(const QPointF&, const QRectF&, bool, ResizableMediaBase*)> callback);
    static std::function<QPointF(const QPointF&, const QRectF&, bool, ResizableMediaBase*)> screenSnapCallback();
    struct ResizeSnapFeedback {
        qreal scale;    // resulting scale
        bool cornerSnapped; // true if a corner snap happened (implies we may need translation)
        QPointF snappedMovingCornerScene; // desired scene position of moving corner when snapped
    };
    static void setResizeSnapCallback(std::function<ResizeSnapFeedback(qreal, const QPointF&, const QPointF&, const QSize&, bool, ResizableMediaBase*)> callback);
    static std::function<ResizeSnapFeedback(qreal, const QPointF&, const QPointF&, const QSize&, bool, ResizableMediaBase*)> resizeSnapCallback();

    // Access to top overlay panel (filename + utility buttons).
    OverlayPanel* topPanel() const { return m_topPanel.get(); }

    // Called by view prior to removing item from scene & scheduling deletion.
    // Default implementation cancels interactive state & hides overlays.
    virtual void prepareForDeletion();
    bool isBeingDeleted() const { return m_beingDeleted; }
    // Visibility toggle (content only; overlays & selection chrome remain)
    void setContentVisible(bool v) { m_contentVisible = v; update();}
    bool isContentVisible() const { return m_contentVisible; }
    void setContentOpacity(qreal op) { m_contentOpacity = std::clamp(op, 0.0, 1.0); update(); }
    qreal contentOpacity() const { return m_contentOpacity; }
    // Effective display opacity = user contentOpacity() * m_contentDisplayOpacity (animation multiplier)
    qreal animatedDisplayOpacity() const { return m_contentDisplayOpacity; }
    // Fading helpers (seconds can be fractional). If duration <= 0, apply immediately.
    void fadeContentIn(double seconds);
    void fadeContentOut(double seconds);
    void cancelFade();

    const MediaSettingsState& mediaSettingsState() const { return m_mediaSettings; }
    void setMediaSettingsState(const MediaSettingsState& state);
    bool autoDisplayEnabled() const;
    int autoDisplayDelayMs() const;
    bool autoUnmuteEnabled() const;
    int autoUnmuteDelayMs() const;
    bool autoPlayEnabled() const;
    int autoPlayDelayMs() const;
    bool autoPauseEnabled() const;
    int autoPauseDelayMs() const;
    bool autoHideEnabled() const;
    int autoHideDelayMs() const;
    bool hideWhenVideoEnds() const;
    bool autoMuteEnabled() const;
    int autoMuteDelayMs() const;
    bool muteWhenVideoEnds() const;
    double fadeInDurationSeconds() const;
    double fadeOutDurationSeconds() const;
    double audioFadeInDurationSeconds() const;
    double audioFadeOutDurationSeconds() const;
    bool opacityOverrideEnabled() const;
    int opacityPercent() const;

    // Axis snap hysteresis state accessors (used by ScreenCanvas helper)
    bool isAxisSnapActive() const { return m_axisSnapActive; }
    Handle axisSnapHandle() const { return m_axisSnapHandle; }
    qreal axisSnapTargetScale() const { return m_axisSnapTargetScale; }
    void setAxisSnapActive(bool active, Handle handle, qreal targetScale) {
        m_axisSnapActive = active;
        m_axisSnapHandle = handle;
        m_axisSnapTargetScale = targetScale;
    }
    
    // Override in derived classes to indicate media type for settings panel
    virtual bool isVideoMedia() const { return false; }
    virtual bool isTextMedia() const { return false; }

protected:
    virtual bool allowAltResize() const;
    virtual void onAltResizeModeEngaged() {}
    QSize m_baseSize;
    Handle m_activeHandle = None;
    // Tracks if the current (or last) midpoint axis resize used Alt (option) for axis-only stretch
    bool m_lastAxisAltStretch = false;
    QSize m_axisStretchOriginalBaseSize; // captured at first Alt midpoint movement
    bool  m_axisStretchOrigCaptured = false;
    qreal m_axisStretchInitialOffset = 0.0; // initial cursor-to-moving-edge offset

    // Corner Alt stretch state (independent width/height non-uniform scaling from a corner)
    bool  m_cornerStretchOrigCaptured = false;
    QSize m_cornerStretchOriginalBaseSize; // baked original size when Alt first used on a corner
    qreal m_cornerStretchInitialOffsetX = 0.0; // cursor-to-moving-edge offset along X (normalized direction)
    qreal m_cornerStretchInitialOffsetY = 0.0; // cursor-to-moving-edge offset along Y (normalized direction)
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
    std::unique_ptr<OverlayPanel> m_topPanel;
    OverlayStyle m_overlayStyle;
    static int heightOfMediaOverlays;
    static int cornerRadiusOfMediaOverlays;
    
    // Phase 4.3: FileManager injected (not singleton) - shared by all media items
    static FileManager* s_fileManager;

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
    virtual void onMediaSettingsChanged();

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
    static double s_sceneGridUnit;
    static std::function<QPointF(const QPointF&, const QRectF&, bool, ResizableMediaBase*)> s_screenSnapCallback;
    static std::function<ResizeSnapFeedback(qreal, const QPointF&, const QPointF&, const QSize&, bool, ResizableMediaBase*)> s_resizeSnapCallback;
    static inline double snapToGrid(double v) { const double u = (s_sceneGridUnit > 1e-9 ? s_sceneGridUnit : 1.0); return std::round(v / u) * u; }
    static inline QPointF snapPointToGrid(const QPointF& p) { return QPointF(snapToGrid(p.x()), snapToGrid(p.y())); }
protected:
    bool m_beingDeleted = false;
    bool m_contentVisible = true; // controlled by visibility toggle overlay button
    qreal m_contentOpacity = 1.0; // multiplicative opacity for content only
    qreal m_contentDisplayOpacity = 1.0; // animated multiplier (0..1) for fade in/out
    QVariantAnimation* m_fadeAnimation = nullptr; // owned manually (ResizableMediaBase not QObject)
    // When true, content fills the item bounds without preserving the source aspect ratio.
    // This becomes true after an Alt-based non-uniform stretch and stays until explicitly reset.
    bool m_fillContentWithoutAspect = false;
    // Axis (midpoint) resize snapping state (hysteresis)
    bool m_axisSnapActive = false;
    Handle m_axisSnapHandle = None;
    qreal m_axisSnapTargetScale = 1.0;
    MediaSettingsState m_mediaSettings;
    std::shared_ptr<bool> m_lifetimeToken;
};

// Simple pixmap media item
class ResizablePixmapItem : public ResizableMediaBase {
public:
    explicit ResizablePixmapItem(const QPixmap& pm, int visualSizePx, int selectionSizePx, const QString& filename = QString());
    void paint(QPainter* painter, const QStyleOptionGraphicsItem* option, QWidget* widget) override;
private:
    QPixmap m_pix;
};

// Video media item with in-item controls overlays & performance instrumentation
class ResizableVideoItem : public ResizableMediaBase {
public:
    explicit ResizableVideoItem(const QString& filePath, int visualSizePx, int selectionSizePx, const QString& filename = QString(), int controlsFadeMs = 140);
    ~ResizableVideoItem() override;

    // Public control helpers used by ScreenCanvas (drag gestures etc.)
    void togglePlayPause();
    void toggleRepeat();
    void toggleMute();
    void setMuted(bool muted, bool skipFade = false);
    void stopToBeginning();
    void seekToRatio(qreal r);
    qint64 currentPositionMs() const { return m_positionMs; }
    qint64 displayedFrameTimestampMs() const { return m_lastFrameTimestampMs; }
    bool isPlaying() const { return m_player && m_player->playbackState() == QMediaPlayer::PlayingState; }
    void pauseAndSetPosition(qint64 posMs);
    void setInitialScaleFactor(qreal f) { m_initialScaleFactor = f; }
    void setExternalPosterImage(const QImage& img);
    bool isDraggingProgress() const { return m_draggingProgress; }
    bool isDraggingVolume() const { return m_draggingVolume; }
    void requestOverlayRelayout() { updateControlsLayout(); }
    void setApplicationSuspended(bool suspended);
    QMediaPlayer* mediaPlayer() const { return m_player; }
    void applyVolumeOverrideFromState();

    // Performance / diagnostics
    void setRepaintBudget(int ms) { m_repaintBudgetMs = std::max(1, ms); }
    void getFrameStats(int& received, int& processed, int& skipped) const;
    void getFrameStatsExtended(int& received, int& processed, int& skipped, int& dropped, int& conversionFailures) const;
    void resetFrameStats();
    // Audio state accessors for scene serialization
    bool isMuted() const { return m_effectiveMuted; }
    qreal volume() const { return m_userVolumeRatio; }
    
    // Repeat session management (public for host scene automation)
    void initializeSettingsRepeatSessionForPlaybackStart();
    void cancelSettingsRepeatSession();
    bool settingsRepeatAvailable() const;
    bool shouldAutoRepeat() const;

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
    QSizeF computeFrameDisplaySize(const QVideoFrame& frame) const;
    QImage applyViewportCrop(const QImage& image, const QVideoFrame& frame) const;
    void adoptBaseSize(const QSize& sz, bool force = false);
    void setControlsVisible(bool show);
    void updateControlsLayout();
    bool isVisibleInAnyView() const;
    bool shouldRepaint() const;
    void logFrameStats() const;
    void updateProgressBar();
    void updatePlayPauseIconState(bool playing);
    bool isEffectivelyPlayingForControls() const;
    QImage convertFrameToImage(const QVideoFrame& frame) const;
    void restartPrimingSequence();
    void teardownPlayback();
    void onMediaSettingsChanged() override;
    bool consumeAutoRepeatOpportunity();
    qint64 nearStartThresholdMs() const;
    void setVolumeFromControl(qreal ratio, bool fromSettings);
    void applyVolumeRatio(qreal ratio);
    qreal volumeFromSettingsState() const;
    void ensureAudioFadeAnimation();
    void stopAudioFadeAnimation(bool resetVolumeGuard);
    void startAudioFade(qreal startVolume, qreal endVolume, double durationSeconds, bool targetMuted);
    void finalizeAudioFade(bool targetMuted);
    void ensureControlsPanel();
    void updateControlsVisualState();

    qreal baseWidth() const { return static_cast<qreal>(m_baseSize.width()); }
    qreal baseHeight() const { return static_cast<qreal>(m_baseSize.height()); }

    QMediaPlayer* m_player = nullptr;
    QAudioOutput* m_audio = nullptr;
    QVideoSink* m_sink = nullptr;
    QImage m_lastFrameImage;
    QSizeF m_lastFrameDisplaySize;
    qint64 m_lastFrameTimestampMs = -1;
    qint64 m_durationMs = 0;
    qint64 m_positionMs = 0;
    bool m_primingFirstFrame = false;
    bool m_firstFramePrimed = false;
    bool m_savedMuted = false;
    bool m_effectiveMuted = false;
    bool m_pendingMuteTarget = false;
    QVariantAnimation* m_audioFadeAnimation = nullptr;
    bool m_volumeChangeFromAudioFade = false;
    qreal m_audioFadeStartVolume = 1.0;
    qreal m_audioFadeTargetVolume = 1.0;
    qreal m_lastUserVolumeBeforeMute = 1.0;
    qreal m_userVolumeRatio = 1.0;
    QImage m_posterImage; bool m_posterImageSet = false;
    std::unique_ptr<OverlayPanel> m_controlsPanel;
    std::shared_ptr<OverlayButtonElement> m_playPauseButton;
    std::shared_ptr<OverlayButtonElement> m_stopButton;
    std::shared_ptr<OverlayButtonElement> m_repeatButton;
    std::shared_ptr<OverlayButtonElement> m_muteButton;
    std::shared_ptr<OverlaySliderElement> m_volumeSlider;
    std::shared_ptr<OverlaySliderElement> m_progressSlider;
    bool m_adoptedSize = false; qreal m_initialScaleFactor = 1.0;
    bool m_repeatEnabled = false;
    bool m_draggingProgress = false; bool m_draggingVolume = false; bool m_holdLastFrameAtEnd = false;
    QTimer* m_progressTimer = nullptr; qreal m_smoothProgressRatio = 0.0; bool m_seeking = false;
    bool m_controlsLockedUntilReady = true; int m_controlsFadeMs = 140; QVariantAnimation* m_controlsFadeAnim = nullptr; bool m_controlsDidInitialFade = false;
    qint64 m_lastRepaintMs = 0; int m_repaintBudgetMs = 16;
    mutable int m_framesReceived = 0; mutable int m_framesProcessed = 0; mutable int m_framesSkipped = 0;
    mutable int m_framesDropped = 0; mutable int m_conversionFailures = 0;
    bool m_appSuspended = false; bool m_wasPlayingBeforeSuspend = false;
    bool m_sinkDetached = false; qint64 m_resumePositionMs = 0; bool m_needsReprimeAfterResume = false;
    bool m_playbackTornDown = false;
    bool m_expectedPlayingState = false;
    bool m_seamlessLoopJumpPending = false;
    qint64 m_lastSeamlessLoopTriggerMs = 0;
    bool m_settingsRepeatEnabled = false;
    int m_settingsRepeatLoopCount = 0;
    int m_settingsRepeatLoopsRemaining = 0;
    bool m_settingsRepeatSessionActive = false;
    bool m_volumeChangeFromSettings = false;
    bool m_displaySizeLocked = false; // When true, prevent frame dimensions from overriding display size
};

