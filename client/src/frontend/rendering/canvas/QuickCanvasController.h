#ifndef QUICKCANVASCONTROLLER_H
#define QUICKCANVASCONTROLLER_H

#include <QObject>
#include <QHash>
#include <QRectF>

#include "backend/domain/models/ClientInfo.h"

class QQuickWidget;
class QWidget;
class QGraphicsScene;
class QTimer;

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

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

private slots:
    void handleMediaSelectRequested(const QString& mediaId, bool additive);
    void handleTextCommitRequested(const QString& mediaId, const QString& text);

private:
    void pushStaticLayerModels();
    void scheduleMediaModelSync();
    void syncMediaModelFromScene();
    void pushRemoteCursorState();
    QPointF mapRemoteCursorToQuickScene(int globalX, int globalY, bool* ok) const;
    void rebuildScreenRects();
    qreal currentSceneUnitScale() const;
    QRectF scaleSceneRect(const QRectF& rect) const;
    void refreshSceneUnitScaleIfNeeded(bool force = false);

    QQuickWidget* m_quickWidget = nullptr;
    QList<ScreenInfo> m_screens;
    QHash<int, QRectF> m_sceneScreenRects;
    bool m_remoteCursorVisible = false;
    qreal m_remoteCursorX = 0.0;
    qreal m_remoteCursorY = 0.0;
    QGraphicsScene* m_mediaScene = nullptr;
    QTimer* m_mediaSyncTimer = nullptr;
    bool m_mediaSyncPending = false;
    qreal m_sceneUnitScale = 1.0;
    bool m_pendingInitialSceneScaleRefresh = false;
};

#endif // QUICKCANVASCONTROLLER_H
