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

class QuickCanvasController : public QObject {
    Q_OBJECT

public:
    explicit QuickCanvasController(QObject* parent = nullptr);
    ~QuickCanvasController() override = default;

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

private slots:
    void handleMediaSelectRequested(const QString& mediaId, bool additive);
    void handleMediaMoveEnded(const QString& mediaId, qreal sceneX, qreal sceneY);
    void handleMediaResizeRequested(const QString& mediaId, const QString& handleId, qreal sceneX, qreal sceneY, bool snap);
    void handleMediaResizeEnded(const QString& mediaId);
    void handleTextCommitRequested(const QString& mediaId, const QString& text);
    void handleTextCreateRequested(qreal viewX, qreal viewY);

private:
    void pushStaticLayerModels();
    void scheduleMediaModelSync();
    void syncMediaModelFromScene();
    void pushMediaModelOnly();
    bool beginLiveResizeSession(const QString& mediaId);
    bool endLiveResizeSession(const QString& mediaId, qreal sceneX, qreal sceneY, qreal scale);
    bool commitMediaTransform(const QString& mediaId, qreal sceneX, qreal sceneY, qreal scale);
    bool pushLiveResizeGeometry(const QString& mediaId, qreal sceneX, qreal sceneY, qreal scale);
    void pushSelectionAndSnapModels();
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
    QGraphicsScene* m_mediaScene = nullptr;
    QTimer* m_mediaSyncTimer = nullptr;
    bool m_mediaSyncPending = false;
    bool m_draggingMedia = false;
    bool m_resizeActive = false;
    QString m_resizeMediaId;
    QString m_resizeHandleId;
    QSize m_resizeBaseSize;
    QPointF m_resizeFixedItemPoint;
    QPointF m_resizeFixedScenePoint;
    qreal m_resizeLastSceneX = 0.0;
    qreal m_resizeLastSceneY = 0.0;
    qreal m_resizeLastScale = 1.0;
    bool m_resizeSnapCacheReady = false;
    QVector<qreal> m_resizeSnapEdgesX;
    QVector<qreal> m_resizeSnapEdgesY;
    QVector<QPointF> m_resizeSnapCorners;
    bool m_pendingInitialSceneScaleRefresh = false;
    bool m_textToolActive = false;
    bool m_initialFitCompleted = false;
    bool m_initialFitPending = false;
    int m_initialFitMarginPx = 53;
    int m_initialFitRetryCount = 0;
    QTimer* m_initialFitRetryTimer = nullptr;
};

#endif // QUICKCANVASCONTROLLER_H
