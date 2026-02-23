#ifndef QUICKCANVASCONTROLLER_H
#define QUICKCANVASCONTROLLER_H

#include <QObject>
#include <QHash>
#include <QPointF>
#include <QRectF>
#include <QSize>
#include <QVector>

#include "backend/domain/models/ClientInfo.h"

class QQuickWidget;
class QWidget;
class QGraphicsScene;
class QTimer;
class ScreenCanvas;
class ResizableMediaBase;
class CanvasSceneStore;
class QuickCanvasViewAdapter;
class PointerSession;
class SelectionStore;
class ModelPublisher;
class SnapStore;

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

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

signals:
    void textMediaCreateRequested(const QPointF& scenePos);
    void localFilesDropRequested(const QStringList& localPaths, const QPointF& scenePos);

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

private slots:
    void handleMediaSelectRequested(const QString& mediaId, bool additive);
    void handleMediaMoveStarted(const QString& mediaId, qreal sceneX, qreal sceneY);
    void handleMediaMoveUpdated(const QString& mediaId, qreal sceneX, qreal sceneY);
    void handleMediaMoveEnded(const QString& mediaId, qreal sceneX, qreal sceneY);
    void handleMediaResizeRequested(const QString& mediaId, const QString& handleId, qreal sceneX, qreal sceneY, bool snap);
    void handleMediaResizeEnded(const QString& mediaId);
    void handleTextCommitRequested(const QString& mediaId, const QString& text);
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
    void handleOverlaySeek(const QString& mediaId, qreal ratio);

private:
    void rebuildMediaItemIndex();
    ResizableMediaBase* mediaItemById(const QString& mediaId);
    void pushStaticLayerModels();
    void scheduleMediaModelSync();
    void syncMediaModelFromScene();
    void pushMediaModelOnly();
    bool beginLiveResizeSession(const QString& mediaId);
    bool endLiveResizeSession(const QString& mediaId, qreal sceneX, qreal sceneY, qreal scale);
    bool commitMediaTransform(const QString& mediaId, qreal sceneX, qreal sceneY, qreal scale);
    bool pushLiveResizeGeometry(const QString& mediaId, qreal sceneX, qreal sceneY, qreal scale);
    void pushSelectionAndSnapModels();
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
    qreal applyAxisSnapWithCachedTargets(ResizableMediaBase* target,
                                         qreal proposedScale,
                                         const QPointF& fixedScenePoint,
                                         const QSize& baseSize,
                                         int activeHandle,
                                         ScreenCanvas* screenCanvas) const;
    bool applyCornerSnapWithCachedTargets(int activeHandle,
                                          const QPointF& fixedScenePoint,
                                          qreal proposedW,
                                          qreal proposedH,
                                          qreal& snappedW,
                                          qreal& snappedH,
                                          QPointF& snappedCorner,
                                          ScreenCanvas* screenCanvas) const;

    QQuickWidget* m_quickWidget = nullptr;
    CanvasSceneStore* m_sceneStore = nullptr;
    QuickCanvasViewAdapter* m_viewAdapter = nullptr;
    PointerSession* m_pointerSession = nullptr;
    SelectionStore* m_selectionStore = nullptr;
    ModelPublisher* m_modelPublisher = nullptr;
    SnapStore* m_snapStore = nullptr;
    QGraphicsScene* m_mediaScene = nullptr;
    QHash<QString, ResizableMediaBase*> m_mediaItemsById;
    QTimer* m_mediaSyncTimer = nullptr;
    QTimer* m_resizeDispatchTimer = nullptr;
    QTimer* m_videoStateTimer = nullptr;
    bool m_mediaSyncPending = false;
    bool m_selectionMutationInProgress = false;
    bool m_executingQueuedResize = false;
    bool m_hasQueuedResize = false;
    QString m_queuedResizeMediaId;
    QString m_queuedResizeHandleId;
    qreal m_queuedResizeSceneX = 0.0;
    qreal m_queuedResizeSceneY = 0.0;
    bool m_queuedResizeSnap = false;
    QSize m_resizeBaseSize;
    QPointF m_resizeFixedItemPoint;
    QPointF m_resizeFixedScenePoint;
    qreal m_resizeLastSceneX = 0.0;
    qreal m_resizeLastSceneY = 0.0;
    qreal m_resizeLastScale = 1.0;
    bool m_pendingInitialSceneScaleRefresh = false;
    bool m_textToolActive = false;
    bool m_initialFitCompleted = false;
    bool m_initialFitPending = false;
    int m_initialFitMarginPx = 53;
    int m_initialFitRetryCount = 0;
    QTimer* m_initialFitRetryTimer = nullptr;
};

#endif // QUICKCANVASCONTROLLER_H
