#ifndef SCREENCANVAS_H
#define SCREENCANVAS_H

#include <QGraphicsView>
#include <QGraphicsRectItem>
#include <QGraphicsEllipseItem>
#include <QGraphicsPathItem>
#include <QElapsedTimer>
#include <QMap>
#include <QPixmap>
#include <QTimer>
#include <memory>
#include <QSet>
#include "ClientInfo.h" // for ScreenInfo
#include "MediaItems.h" // for ResizableMediaBase / ResizableVideoItem
#include <QGestureEvent>
#include <QPinchGesture>
#include <QString>
#ifdef Q_OS_MACOS
template<typename T> class QFutureWatcher;
#endif
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
class QPushButton;
class QToolButton;
class QScrollArea;
class QScrollBar;

// Extracted canvas that manages screen layout, zoom/pan, drag&drop previews, and media interaction.
class ScreenCanvas : public QGraphicsView {
    Q_OBJECT
public:
    explicit ScreenCanvas(QWidget* parent = nullptr);
    ~ScreenCanvas() override;
    
    // Access to upload button in media list overlay
    QPushButton* getUploadButton() const { return m_uploadButton; }
    void setOverlayActionsEnabled(bool enabled);
    bool overlayActionsEnabled() const { return m_overlayActionsEnabled; }
    static QString overlayDisabledButtonStyle();
    void setScreens(const QList<ScreenInfo>& screens);
    void clearScreens();
    bool hasActiveScreens() const;
    // Schedule a one-time recenter after the next event loop turn if screens just appeared
    void requestDeferredInitialRecenter(int marginPx = 53);
    void hideContentPreservingState(); // Hide content without clearing, preserving viewport
    void showContentAfterReconnect();  // Show content after reconnection
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
    // Screen spacing no longer used; screens reflect OS layout exactly

    // Host scene lifecycle
    enum class HostSceneMode { None, Remote, Test };
    void startHostSceneState(HostSceneMode mode);
    void stopHostSceneState();
    bool isHostSceneActive() const { return m_hostSceneActive; }
    // Serialize current canvas state (screens + media) for remote scene start
    QJsonObject serializeSceneState() const;
    void scheduleAutoDisplayAndPlayback();
    void handleAutoDisplay();
    void handleAutoPlayback();
    // Remote scene integration setters
    void setWebSocketClient(class WebSocketClient* client);
    void setRemoteSceneTargetClientId(const QString& id) { m_remoteSceneTargetClientId = id; }
    void setRemoteSceneTarget(const QString& id, const QString& machineName) {
        m_remoteSceneTargetClientId = id;
        m_remoteSceneTargetMachineName = machineName;
    }
    void updateRemoteSceneTargetFromClientList(const QList<ClientInfo>& clients);
    bool isRemoteSceneLaunched() const { return m_sceneLaunched; }
    bool isRemoteSceneLaunching() const { return m_sceneLaunching; }
    QString remoteSceneTargetClientId() const { return m_remoteSceneTargetClientId; }
    QString remoteSceneTargetMachineName() const { return m_remoteSceneTargetMachineName; }
    void handleRemoteConnectionLost();
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
    void setCornerSnapDistancePx(int px) { m_cornerSnapDistancePx = qMax(1, px); }
    int cornerSnapDistancePx() const { return m_cornerSnapDistancePx; }
    
    // Z-order management for media items (public interface)
    void moveMediaUp(QGraphicsItem* item);
    void moveMediaDown(QGraphicsItem* item);
    
    // Media list overlay management
    void refreshInfoOverlay();

    // Snapping support for side (midpoint) resize handles: clamps scale so the moving edge
    // sticks to nearby screen borders when within snap distance.
    qreal applyAxisSnapWithHysteresis(ResizableMediaBase* item,
                                      qreal proposedScale,
                                      const QPointF& fixedScenePoint,
                                      const QSize& baseSize,
                                      ResizableMediaBase::Handle activeHandle) const;

    struct CornerAltSnapResult {
        bool cornerSnapped = false;
        qreal snappedW = 0.0;
        qreal snappedH = 0.0;
        QPointF snappedCorner; // scene point of moving corner
    };
    CornerAltSnapResult applyCornerAltSnapWithHysteresis(ResizableMediaBase* item,
                                                         ResizableMediaBase::Handle activeHandle,
                                                         const QPointF& fixedScenePoint,
                                                         const QSize& originalBaseSize,
                                                         qreal proposedW,
                                                         qreal proposedH) const;

signals:
    // Emitted when a new media item is added to the canvas
    void mediaItemAdded(ResizableMediaBase* mediaItem);
    void remoteSceneLaunchStateChanged(bool active, const QString& targetClientId, const QString& targetMachineName);

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
    void showEvent(QShowEvent* event) override;
    void dragEnterEvent(QDragEnterEvent* event) override;
    void dragMoveEvent(QDragMoveEvent* event) override;
    void dragLeaveEvent(QDragLeaveEvent* event) override;
    void dropEvent(QDropEvent* event) override;
    void drawForeground(QPainter* painter, const QRectF& rect) override; // may be used for future overlay-specific painting
    void drawBackground(QPainter* painter, const QRectF& rect) override; // snap indicators drawn here (below overlays)

private:
    static QSet<ScreenCanvas*> s_activeCanvases;
    static void registerCanvas(ScreenCanvas* canvas);
    static void unregisterCanvas(ScreenCanvas* canvas);
    static void dispatchUploadStateChanged();

    bool gestureEvent(QGestureEvent* event);
    void ensureDragPreview(const QMimeData* mime);
    void updateDragPreviewPos(const QPointF& scenePos);
    void clearDragPreview();
    void startVideoPreviewProbe(const QString& localFilePath);
    void startVideoPreviewProbeFallback(const QString& localFilePath);
    void stopVideoPreviewProbe();
    void startDragPreviewFadeIn();
    void stopDragPreviewFade();
    void onFastVideoThumbnailReady(const QImage& img);
#ifdef Q_OS_MACOS
    void startFastMacThumbnailProbe(const QString& localFilePath);
    void cancelFastMacThumbnailProbe();
#endif

    void createScreenItems();
    QGraphicsRectItem* createScreenItem(const ScreenInfo& screen, int index, const QRectF& position);
    QMap<int, QRectF> calculateCompactPositions(double scaleFactor) const;
    // Map remote global cursor position (remote screen coords) to scene space using stored screen layout.
    QPointF mapRemoteCursorToScene(int remoteX, int remoteY) const;
    QRectF screensBoundingRect() const;
    void zoomAroundViewportPos(const QPointF& vpPos, qreal factor);
    void recreateRemoteCursorItem();
    // Global top-right info overlay (lists media files)
    void initInfoOverlay();
    void scheduleInfoOverlayRefresh();
    void layoutInfoOverlay();
    void maybeRefreshInfoOverlayOnSceneChanged();
    void updateInfoOverlayGeometryForViewport(); // fast path: recalc height/scroll cap on resize
    void updateOverlayVScrollVisibilityAndGeometry(); // overlay scrollbar sizing/visibility
    void applyTextEllipsisIfConstrained(bool isWidthConstrained); // apply ellipsis to text labels
    std::pair<int, bool> calculateDesiredWidthAndConstraint(); // calculate desired width and constraint state consistently
    void updateGlobalSettingsPanelVisibility(); // Update global settings panel based on toggle + selection
    void ensureSettingsToggleButton();
    void updateSettingsToggleButtonGeometry();
    
    void OnSceneChanged();
    // Snap-to-screen helpers
    QPointF snapToScreenBorders(const QPointF& scenePos, const QRectF& mediaBounds, bool shiftPressed) const;
    QPointF snapToMediaAndScreenTargets(const QPointF& scenePos, const QRectF& mediaBounds, bool shiftPressed, ResizableMediaBase* movingItem) const;
    struct ResizeSnapResult {
        qreal scale {1.0};
        bool cornerSnapped {false};
        QPointF snappedMovingCornerScene; // valid if cornerSnapped
    };
    // Extended resize snapping: considers both screens and other media items (corner precedence over edges).
    // movingItem is excluded from candidate media set.
    ResizeSnapResult snapResizeToScreenBorders(qreal currentScale,
                                               const QPointF& fixedCorner,
                                               const QPointF& fixedItemPoint,
                                               const QSize& baseSize,
                                               bool shiftPressed,
                                               ResizableMediaBase* movingItem) const;
    QList<QRectF> getScreenBorderRects() const;

    QGraphicsScene* m_scene = nullptr;
    QList<QGraphicsRectItem*> m_screenItems;
    QList<ScreenInfo> m_screens;
    QList<QGraphicsRectItem*> m_uiZoneItems; // per-screen uiZones overlays
    bool m_contentHiddenPreservingState = false;
    bool m_infoWidgetWasVisibleBeforeHide = false;
    bool m_pendingInitialRecenter = false;
    int m_pendingInitialRecenterMargin = 53;
    bool m_overlayActionsEnabled = true;
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
    QElapsedTimer m_lastOverlayLayoutTimer; // throttle rapid overlay relayouts during pinch/scroll
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

    QGraphicsItem* m_dragPreviewItem = nullptr;
    QSize m_dragPreviewBaseSize;
    QSize m_dragPreviewVideoSize; // actual video dimensions (if video)
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
#ifdef Q_OS_MACOS
    QFutureWatcher<QImage>* m_dragPreviewThumbnailWatcher = nullptr;
    QString m_dragPreviewPendingVideoPath;
    QTimer* m_dragPreviewFallbackDelayTimer = nullptr;
#endif
    
    // Snap-to-screen settings
    int m_snapDistancePx = 10; // pixels within which snapping occurs
    int m_cornerSnapDistancePx = 20; // larger region for corner snapping precedence
    
    // Z-order management for media items
    qreal m_nextMediaZValue = 1.0;
    void assignNextZValue(QGraphicsItem* item);
    QList<QGraphicsItem*> getMediaItemsSortedByZ() const;

    // Selection chrome (border + corner handles) drawn as separate scene items above media, below overlays
    struct SelectionChrome {
        QGraphicsPathItem* borderWhite = nullptr;
        QGraphicsPathItem* borderBlue = nullptr;
        // 0: TL, 1: TR, 2: BL, 3: BR, 4: TopMid, 5: BottomMid, 6: LeftMid, 7: RightMid
        QGraphicsRectItem* handles[8] = { nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr };
    };
    QMap<ResizableMediaBase*, SelectionChrome> m_selectionChromeMap;
    void updateSelectionChrome();
    void updateSelectionChromeGeometry(ResizableMediaBase* item);
    void clearSelectionChromeFor(ResizableMediaBase* item);
    void clearAllSelectionChrome();

    // Info overlay widgets (viewport child, independent from scene transforms)
    QWidget* m_infoWidget = nullptr;       // panel widget parented to viewport()
    QVBoxLayout* m_infoLayout = nullptr;   // main layout (no margins)
    QWidget* m_contentWidget = nullptr;    // content container (with margins)
    QScrollArea* m_contentScroll = nullptr; // scroll area for content when overlay is too tall
    QScrollBar* m_overlayVScroll = nullptr;  // custom overlay vertical scrollbar (floating)
    QTimer* m_scrollbarHideTimer = nullptr; // timer to auto-hide scrollbar after inactivity
    QVBoxLayout* m_contentLayout = nullptr; // content layout (for media items)
    QWidget* m_overlayHeaderWidget = nullptr; // container for overlay header row (holds upload button)
    QToolButton* m_settingsToggleButton = nullptr; // toggle to show/hide media settings panel
    bool m_settingsPanelPreferredVisible = false;
    class MediaSettingsPanel* m_globalSettingsPanel = nullptr; // global settings panel driven by toolbar toggle
    QPushButton* m_launchSceneButton = nullptr; // new Launch Remote Scene toggle button
    QPushButton* m_launchTestSceneButton = nullptr; // new Launch Test Scene toggle button
    QPushButton* m_uploadButton = nullptr; // upload button in media list overlay
    MouseBlockingRoundedRectItem* m_infoBorderRect = nullptr; // graphics rect for info overlay border
    bool m_infoRefreshQueued = false;
    int m_lastMediaItemCount = -1; // cache to detect add/remove
    // Mapping media item -> container widget in overlay (pour gestion sélection visuelle)
    QHash<ResizableMediaBase*, QWidget*> m_mediaContainerByItem;
    // Reverse mapping container widget -> media item (pour clic sélection)
    QHash<QWidget*, ResizableMediaBase*> m_mediaItemByContainer;

private slots:
    // Remote scene feedback handlers
    void onRemoteSceneValidationReceived(const QString& targetClientId, bool success, const QString& errorMessage);
    void onRemoteSceneLaunchedReceived(const QString& targetClientId);
    void onRemoteSceneLaunchTimeout();

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

    // Launch Remote Scene toggle state
    bool m_sceneLaunched = false;
    bool m_sceneLaunching = false; // Loading state while waiting for remote validation
    QTimer* m_sceneLaunchTimeoutTimer = nullptr;
    
    // Configurable timeout for remote scene launch (milliseconds)
    static constexpr int REMOTE_SCENE_LAUNCH_TIMEOUT_MS = 10000; // 10 seconds
    
    // Launch Test Scene toggle state
    bool m_testSceneLaunched = false;

    // Unified host scene state (either remote or test scene active)
    bool m_hostSceneActive = false;
    HostSceneMode m_hostSceneMode = HostSceneMode::None;
    QTimer* m_autoDisplayTimer = nullptr;
    QTimer* m_autoPlayTimer = nullptr;
    struct SavedSelection {
        ResizableMediaBase* media = nullptr;
        std::weak_ptr<bool> guard;
    };
    struct VideoPreState {
        ResizableVideoItem* video = nullptr;
        std::weak_ptr<bool> guard;
        qint64 posMs = 0;
        bool wasPlaying = false;
    };
    // Remember selection present just before entering host scene so it can be restored afterward (multi-select supported)
    QList<SavedSelection> m_prevSelectionBeforeHostScene;
    QList<VideoPreState> m_prevVideoStates;
    void updateLaunchSceneButtonStyle();
    void updateLaunchTestSceneButtonStyle();

    // Persistent selection helpers: remember selection at press and restore after drags
    bool m_leftMouseActive = false;
    bool m_draggingSincePress = false;
    QPoint m_pressViewPos;
    QList<ResizableMediaBase*> m_selectionAtPress;

    // Manual drag of selected item under occlusion
    ResizableMediaBase* m_draggingSelected = nullptr;
    QPointF m_dragStartScene;
    QPointF m_dragItemStartPos;

    // Snap visual indicators routed through dedicated SnapGuideItem (no local caching needed)
    void updateSnapIndicators(const QVector<QLineF>& lines);
    void clearSnapIndicators();
    void keyReleaseEvent(QKeyEvent* event) override; // clear indicators when Shift released
    // Snap guides item (renders between scene content and overlay panels)
    class SnapGuideItem* m_snapGuides = nullptr;
    // Persist last chosen snap guide positions for hysteresis / tie-breaking
    mutable qreal m_lastSnapVerticalX = std::numeric_limits<qreal>::quiet_NaN();
    mutable qreal m_lastSnapHorizontalY = std::numeric_limits<qreal>::quiet_NaN();

    // Remote scene networking
    class WebSocketClient* m_wsClient = nullptr; // not owned
    QString m_remoteSceneTargetClientId; // target client to receive remote scene commands
    QString m_remoteSceneTargetMachineName; // machine name of target (stable across reconnections)
    void emitRemoteSceneLaunchStateChanged();
};

#endif // SCREENCANVAS_H
