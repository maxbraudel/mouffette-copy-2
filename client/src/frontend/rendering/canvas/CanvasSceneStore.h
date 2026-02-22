#ifndef CANVASSCENESTORE_H
#define CANVASSCENESTORE_H

#include <QObject>
#include <QHash>
#include <QRectF>
#include <QPointF>
#include <QSize>
#include "backend/domain/models/ClientInfo.h"

class CanvasSceneStore : public QObject {
    Q_OBJECT
public:
    explicit CanvasSceneStore(QObject* parent = nullptr);
    ~CanvasSceneStore() override = default;

    void setScreens(const QList<ScreenInfo>& screens);
    QList<ScreenInfo> screens() const;

    void setSceneScreenRects(const QHash<int, QRectF>& rects);
    QHash<int, QRectF> sceneScreenRects() const;

    void setRemoteCursor(bool visible, qreal x, qreal y);
    bool remoteCursorVisible() const;
    QPointF remoteCursorPos() const;

    void setSceneUnitScale(qreal scale);
    qreal sceneUnitScale() const;

signals:
    void screensChanged();
    void remoteCursorChanged();
    void sceneUnitScaleChanged();

private:
    QList<ScreenInfo> m_screens;
    QHash<int, QRectF> m_sceneScreenRects;
    bool m_remoteCursorVisible = false;
    QPointF m_remoteCursorPos;
    qreal m_sceneUnitScale = 1.0;
};

#endif // CANVASSCENESTORE_H
