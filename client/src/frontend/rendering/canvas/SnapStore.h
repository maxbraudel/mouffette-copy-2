#ifndef SNAPSTORE_H
#define SNAPSTORE_H

#include <QHash>
#include <QPointF>
#include <QRectF>
#include <QVector>

class QGraphicsScene;
class ResizableMediaBase;

class SnapStore {
public:
    void clear();
    void rebuild(const QHash<int, QRectF>& sceneScreenRects,
                 QGraphicsScene* mediaScene,
                 ResizableMediaBase* resizingItem);

    bool ready() const;
    const QVector<qreal>& edgesX() const;
    const QVector<qreal>& edgesY() const;
    const QVector<QPointF>& corners() const;

private:
    bool m_ready = false;
    QVector<qreal> m_edgesX;
    QVector<qreal> m_edgesY;
    QVector<QPointF> m_corners;
};

#endif // SNAPSTORE_H
