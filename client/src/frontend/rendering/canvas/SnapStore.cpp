#include "frontend/rendering/canvas/SnapStore.h"

#include "backend/domain/media/MediaItems.h"

#include <QGraphicsItem>
#include <QGraphicsScene>
#include <algorithm>

namespace {
template <typename T, typename Equals>
void sortAndDeduplicate(QVector<T>& values, Equals equals) {
    std::sort(values.begin(), values.end());
    values.erase(std::unique(values.begin(), values.end(), equals), values.end());
}
}

void SnapStore::clear() {
    m_ready = false;
    m_edgesX.clear();
    m_edgesY.clear();
    m_corners.clear();
}

void SnapStore::rebuild(const QHash<int, QRectF>& sceneScreenRects,
                        QGraphicsScene* mediaScene,
                        ResizableMediaBase* resizingItem) {
    clear();

    if (!resizingItem) {
        return;
    }

    for (auto it = sceneScreenRects.constBegin(); it != sceneScreenRects.constEnd(); ++it) {
        const QRectF& sr = it.value();
        m_edgesX.append(sr.left());
        m_edgesX.append(sr.right());
        m_edgesY.append(sr.top());
        m_edgesY.append(sr.bottom());
        m_corners.append(sr.topLeft());
        m_corners.append(sr.topRight());
        m_corners.append(sr.bottomLeft());
        m_corners.append(sr.bottomRight());
    }

    if (mediaScene) {
        const QList<QGraphicsItem*> sceneItems = mediaScene->items();
        for (QGraphicsItem* graphicsItem : sceneItems) {
            auto* media = dynamic_cast<ResizableMediaBase*>(graphicsItem);
            if (!media || media == resizingItem) {
                continue;
            }
            const QRectF r = media->sceneBoundingRect();
            m_edgesX.append(r.left());
            m_edgesX.append(r.right());
            m_edgesY.append(r.top());
            m_edgesY.append(r.bottom());
            m_corners.append(r.topLeft());
            m_corners.append(r.topRight());
            m_corners.append(r.bottomLeft());
            m_corners.append(r.bottomRight());
        }
    }

    sortAndDeduplicate<qreal>(m_edgesX, [](qreal a, qreal b) { return std::abs(a - b) < 1e-6; });
    sortAndDeduplicate<qreal>(m_edgesY, [](qreal a, qreal b) { return std::abs(a - b) < 1e-6; });
    std::sort(m_corners.begin(), m_corners.end(), [](const QPointF& lhs, const QPointF& rhs) {
        if (std::abs(lhs.x() - rhs.x()) > 1e-6) {
            return lhs.x() < rhs.x();
        }
        return lhs.y() < rhs.y();
    });
    m_corners.erase(std::unique(m_corners.begin(), m_corners.end(), [](const QPointF& a, const QPointF& b) {
        return std::abs(a.x() - b.x()) < 1e-6 && std::abs(a.y() - b.y()) < 1e-6;
    }), m_corners.end());

    m_ready = true;
}

bool SnapStore::ready() const {
    return m_ready;
}

const QVector<qreal>& SnapStore::edgesX() const {
    return m_edgesX;
}

const QVector<qreal>& SnapStore::edgesY() const {
    return m_edgesY;
}

const QVector<QPointF>& SnapStore::corners() const {
    return m_corners;
}
