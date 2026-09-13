#ifndef QUICKCANVASCONTROLLER_H
#define QUICKCANVASCONTROLLER_H

#include <QObject>
#include <QHash>
#include <QImage>
#include <QPointF>
#include <QRectF>
#include <QSize>
#include <QStringList>
#include <QVector>
#include <QMetaObject>
#include <QPointer>
#include <memory>

#include "backend/domain/models/ClientInfo.h"
#include "frontend/rendering/canvas/QuickDragSnapSession.h"
#include "frontend/rendering/canvas/SnapEngine.h"

class QQuickWidget;
class QWidget;
class QGraphicsScene;
class QTimer;
class QMimeData;
class QMediaPlayer;
class QVideoSink;
class QAudioOutput;
class ScreenCanvas;
class ResizableMediaBase;
class ResizableVideoItem;
class CanvasSceneStore;
class QuickCanvasViewAdapter;
class PointerSession;
class ModelPublisher;
class SnapStore;
class MediaListModel;
class RemoteVideoFrameSource;

class QuickCanvasController : public QObject {
    Q_OBJECT

public:
    explicit QuickCanvasController(QObject* parent = nullptr);
    ~QuickCanvasController() override;

    bool initialize(QWidget* parentWidget, QString* errorMessage = nullptr);

    QWidget* widget() const;
    void setScreenCount(int screenCount);
    void setShellActive(bool active);
    void setScreens(const QList<ScreenInfo>& screens);
    void setMediaScene(QGraphicsScene* scene);
    void updateRemoteCursor(int globalX, int globalY);
    void hideRemoteCursor();
    void resetView();
    void recenterView();
    void setTextToolActive(bool active);
    qreal currentViewScale() const;
    void ensureInitialFit(int marginPx = 53);
    // Called synchronously by QuickCanvasHost after the prepared media has
    // entered the authoritative scene.
    void beginDropPreviewHandoff(const QString& mediaId);

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

signals:
    void textMediaCreateRequested(const QPointF& scenePos);
    void preparedLocalFileDropRequested(const QString& localPath,
                                        const QSize& nativeSize,
                                        const QImage& previewFrame,
                                        const QPointF& scenePos);

    // Overlay action signals (QML → C++)
    void mediaVisibilityToggleRequested(const QString& mediaId, bool visible);
    void mediaBringForwardRequested(const QString& mediaId);
    void mediaBringBackwardRequested(const QString& mediaId);
    void mediaDeleteRequested(const QString& mediaId);
    void mediaPlayPauseRequested(const QString& mediaId);
    void mediaStopRequested(const QString& mediaId);
    void mediaRepeatToggleRequested(const QString& mediaId);
    void mediaMuteToggleRequested(const QString& mediaId);
    void mediaVolumeChangeRequested(const QString& mediaId, qreal value);
    void mediaSeekRequested(const QString& mediaId, qreal ratio);
    void mediaFitToTextToggleRequested(const QString& mediaId);
    void mediaHorizontalAlignRequested(const QString& mediaId, const QString& alignment);
    void mediaVerticalAlignRequested(const QString& mediaId, const QString& alignment);

private slots:
    void handleMediaSelectRequested(const QString& mediaId, bool additive);
    void handleClearSelectionRequested();
    void handleMediaMoveStarted(const QString& mediaId, qreal sceneX, qreal sceneY, bool snap);
    void handleMediaMoveUpdated(const QString& mediaId, qreal sceneX, qreal sceneY, bool snap);
    void handleMediaMoveEnded(const QString& mediaId, qreal sceneX, qreal sceneY, bool snap);
    void handleMediaResizeRequested(const QString& mediaId, const QString& handleId, qreal sceneX, qreal sceneY, bool snap, bool altPressed);
    void handleMediaResizeEnded(const QString& mediaId);
    void handleTextCommitRequested(const QString& mediaId, const QString& text);
    void handleTextLiveUpdateRequested(const QString& mediaId, const QString& text);
    void handleTextCreateRequested(qreal viewX, qreal viewY);
    // Overlay action slots (wired from QML signals)
    void handleOverlayVisibilityToggle(const QString& mediaId, bool visible);
    void handleOverlayBringForward(const QString& mediaId);
    void handleOverlayBringBackward(const QString& mediaId);
    void handleOverlayDelete(const QString& mediaId);
    void handleOverlayPlayPause(const QString& mediaId);
    void handleOverlayStop(const QString& mediaId);
    void handleOverlayRepeatToggle(const QString& mediaId);
    void handleOverlayMuteToggle(const QString& mediaId);
    void handleOverlayVolumeChange(const QString& mediaId, qreal value);
    // Three-phase frame-acknowledged scrub protocol. Updates are coalesced while
    // one target is in flight; end releases only after the final native frame.
    void handleOverlaySeekBegin(const QString& mediaId, qreal ratio);
    void handleOverlaySeekUpdate(const QString& mediaId, qreal ratio);
    void handleOverlaySeekEnd(const QString& mediaId, qreal ratio);
    void handleOverlayFitToTextToggle(const QString& mediaId);
    void handleOverlayHorizontalAlign(const QString& mediaId, const QString& alignment);
    void handleOverlayVerticalAlign(const QString& mediaId, const QString& alignment);
    void handleFadeAnimationTick();
    void handleMediaSettingsChanged(ResizableMediaBase* media);
    void handleDropPreviewContentReady(const QString& mediaId);

private:
    void rebuildMediaItemIndex();
    ResizableMediaBase* mediaItemById(const QString& mediaId);
    bool remoteSceneLocksEdits() const;
    void pushStaticLayerModels();
    void scheduleMediaModelSync();
    void syncMediaModelFromScene();
    void pushMediaModelOnly();
    bool beginLiveResizeSession(const QString& mediaId);
    bool endLiveResizeSession(const QString& mediaId, qreal sceneX, qreal sceneY, qreal scale);
    bool pushLiveResizeGeometry(const QString& mediaId, qreal sceneX, qreal sceneY, qreal scale);
    bool pushLiveAltResizeGeometry(const QString& mediaId, qreal sceneX, qreal sceneY, qreal width, qreal height, qreal scale);
    void stagePendingAltResize(const QString& mediaId, const QSize& baseSize, const QPointF& scenePos);
    bool commitPendingAltResize(ResizableMediaBase* target);
    void clearPendingAltResize();
    void resetAltResizeState();
    static bool isAxisHandle(int handleValue);
    static bool isCornerHandle(int handleValue);
    static QPointF computeHandleItemPoint(int handleValue, const QSize& baseSize);
    void pushSelectionAndSnapModels();
    void pushSnapGuidesFromScreenCanvas();
    void pushLiveDragSnapPosition(const QString& mediaId, qreal sceneX, qreal sceneY);
    void clearLiveDragSnapPosition();
    void pushVideoStateModel();
    void pushRemoteCursorState();
    QPointF mapRemoteCursorToQuickScene(int globalX, int globalY, bool* ok) const;
    void rebuildScreenRects();
    qreal currentSceneUnitScale() const;
    void refreshSceneUnitScaleIfNeeded(bool force = false);
    QPointF mapViewPointToScene(const QPointF& viewPoint) const;
    void scheduleInitialFitIfNeeded(int marginPx = 53);
    bool tryInitialFitNow(int marginPx = 53);
    void buildResizeSnapCaches(ResizableMediaBase* resizingItem);
    bool acceptedSingleLocalMedia(const QMimeData* mimeData, QString* localPath,
                                  bool* isVideo, QString* rejectionReason = nullptr) const;
    void startLocalDragPreview(const QString& localPath, bool isVideo, const QPointF& sceneCenter);
    void updateLocalDragPreviewCenter(const QPointF& sceneCenter);
    void publishLocalDragPreview(bool visible);
    void startVideoPreviewFallback(quint64 generation);
    void stopVideoPreviewFallback();
    void maybeCompleteLocalDragPreparation(quint64 generation);
    void performPreparedLocalDrop();
    void failLocalDragPreview(const QString& message, quint64 generation);
    void clearLocalDragPreview(bool animate, bool restoreCursor = true);
    void cancelDropHandoffRenderBarrier();
    QString localPreviewCacheKey(const QString& localPath) const;
    bool restoreLocalPreviewFromCache(const QString& cacheKey);
    void storeLocalPreviewInCache();
    void syncSnapViewScale() const; // pushes currentViewScale() into the backing ScreenCanvas
    SnapEngine::AxisSnapResult applyAxisSnapWithCachedTargets(ResizableMediaBase* target,
                                                               qreal proposedScale,
                                                               const QPointF& fixedScenePoint,
                                                               const QSize& baseSize,
                                                               int activeHandle,
                                                               bool shiftPressed,
                                                               ScreenCanvas* screenCanvas) const;
    SnapEngine::CornerSnapResult applyCornerSnapWithCachedTargets(int activeHandle,
                                                                   const QPointF& fixedScenePoint,
                                                                   qreal proposedW,
                                                                   qreal proposedH,
                                                                   bool shiftPressed,
                                                                   ScreenCanvas* screenCanvas) const;

    QPointer<QQuickWidget> m_quickWidget;
    CanvasSceneStore* m_sceneStore = nullptr;
    QuickCanvasViewAdapter* m_viewAdapter = nullptr;
    MediaListModel*         m_mediaListModel = nullptr;
    PointerSession* m_pointerSession = nullptr;
    ModelPublisher* m_modelPublisher = nullptr;
    SnapStore* m_snapStore = nullptr;
    QuickDragSnapSession* m_dragSnapSession = nullptr;
    QGraphicsScene* m_mediaScene = nullptr;
    struct MediaItemReference {
        ResizableMediaBase* item = nullptr;
        std::weak_ptr<bool> lifetime;
    };
    // QGraphicsItem is not a QObject. Its lifetime token provides the guarded
    // lookup needed when a queued input arrives after deletion, before model sync.
    QHash<QString, MediaItemReference> m_mediaItemsById;
    QTimer* m_mediaSyncTimer = nullptr;
    QTimer* m_resizeDispatchTimer = nullptr;
    QTimer* m_videoStateTimer = nullptr;
    bool m_mediaSyncPending = false;
    // QGraphicsScene owns selection; QML receives only its read-only projection.
    // Batch clear + select into one projection update without touching media sync.
    bool m_selectionMutationInProgress = false;
    bool m_executingQueuedResize = false;
    bool m_hasQueuedResize = false;
    QString m_queuedResizeMediaId;
    QString m_queuedResizeHandleId;
    qreal m_queuedResizeSceneX = 0.0;
    qreal m_queuedResizeSceneY = 0.0;
    bool m_queuedResizeSnap = false;
    bool m_queuedResizeAlt = false;
    QSize m_resizeBaseSize;
    QPointF m_resizeFixedItemPoint;
    QPointF m_resizeFixedScenePoint;
    qreal m_resizeLastSceneX = 0.0;
    qreal m_resizeLastSceneY = 0.0;
    qreal m_resizeLastScale = 1.0;
    // Alt-resize session state (axis or corner non-uniform stretch)
    bool   m_lastResizeWasAlt          = false;
    bool   m_altAxisCaptured           = false;
    bool   m_altCornerCaptured         = false;
    QSize  m_altOrigBaseSize;                       // base size at start of alt capture
    QPointF m_altFixedScenePoint;                   // fixed corner scene point (after any bake)
    qreal  m_altAxisInitialOffset      = 0.0;       // cursor-to-moving-edge offset (axis)
    qreal  m_altCornerInitialOffsetX   = 0.0;       // cursor-to-moving-corner offset X (corner)
    qreal  m_altCornerInitialOffsetY   = 0.0;       // cursor-to-moving-corner offset Y (corner)
    // QML owns the live non-uniform geometry. Keep the legacy QGraphics item
    // unchanged during the gesture and commit this final value once on release.
    QString m_pendingAltResizeMediaId;
    QSize m_pendingAltResizeBaseSize;
    QPointF m_pendingAltResizeScenePos;
    // Uniform corner snap result — set inside the snap block, consumed by guide publishing below
    bool   m_uniformCornerSnapped    = false;
    QPointF m_uniformCornerSnappedPt;
    bool   m_uniformCornerSnapActive = false;
    int    m_uniformCornerSnapHandle = 0;
    qreal  m_uniformCornerSnapScale  = 1.0;
    // Last snapped scene position pushed to QML via pushLiveDragSnapPosition.
    // Used by handleMediaMoveEnded to commit exactly what was displayed rather
    // than re-running the snap engine (which could yield a different result).
    qreal m_lastSnapSceneX = 0.0;
    qreal m_lastSnapSceneY = 0.0;
    bool  m_lastSnapWasSnapped = false;
    bool m_pendingInitialSceneScaleRefresh = false;
    bool m_textToolActive = false;
    bool m_initialFitCompleted = false;
    bool m_initialFitPending = false;
    int m_initialFitMarginPx = 53;
    int m_initialFitRetryCount = 0;
    QTimer* m_initialFitRetryTimer = nullptr;
    QTimer* m_fadeTickTimer = nullptr;

    struct LocalPreviewCacheEntry {
        QSize nativeSize;
        QImage frame;
        bool video = false;
        qsizetype byteCost = 0;
    };
    QHash<QString, LocalPreviewCacheEntry> m_localPreviewCache;
    QStringList m_localPreviewCacheLru;
    qsizetype m_localPreviewCacheBytes = 0;
    quint64 m_localDragGeneration = 0;
    bool m_localDragAccepted = false;
    bool m_localDragIsVideo = false;
    bool m_localDropPending = false;
    QString m_localDragPath;
    QString m_localDragDisplayName;
    QString m_localDragCacheKey;
    QSize m_localDragNativeSize;
    QImage m_localDragFrame;
    QPointF m_localDragSceneCenter;
    QPointF m_localDropSceneCenter;
    QString m_localDropHandoffMediaId;
    QMetaObject::Connection m_dropHandoffRenderConnection;
    quint64 m_dropHandoffRenderGeneration = 0;
    int m_dropHandoffRenderedFramesRemaining = 0;
    RemoteVideoFrameSource* m_localDragFrameSource = nullptr;
    QMediaPlayer* m_localDragFallbackPlayer = nullptr;
    QVideoSink* m_localDragFallbackSink = nullptr;
    QAudioOutput* m_localDragFallbackAudio = nullptr;
    bool m_localDragCursorHidden = false;
};

#endif // QUICKCANVASCONTROLLER_H
