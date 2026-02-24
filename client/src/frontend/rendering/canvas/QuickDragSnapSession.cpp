#include "frontend/rendering/canvas/QuickDragSnapSession.h"

#include "frontend/rendering/canvas/ScreenCanvas.h"
#include "backend/domain/media/MediaItems.h"

#include <QGraphicsScene>
#include <cmath>

void QuickDragSnapSession::begin(ResizableMediaBase* item,
                                  QGraphicsScene* mediaScene,
                                  const QHash<int, QRectF>& sceneScreenRects,
                                  ScreenCanvas* screenCanvas) {
    m_active       = false;
    m_screenCanvas = screenCanvas;

    if (!item || !mediaScene || !screenCanvas) {
        return;
    }

    // Rebuild snap cache excluding the moving item
    m_snapStore.rebuild(sceneScreenRects, mediaScene, item);

    // Record the item's rendered scene size (base × scale) for snap geometry
    const QSize  base  = item->baseSizePx();
    const qreal  scale = std::abs(item->scale()) > 1e-9 ? std::abs(item->scale()) : 1.0;
    m_itemSceneSize = QSizeF(base.width() * scale, base.height() * scale);

    m_active = true;
}

DragSnapResult QuickDragSnapSession::update(const QPointF& proposedScenePos,
                                              bool shiftPressed) const {
    if (!m_active || !m_screenCanvas) {
        return DragSnapResult{ proposedScenePos, {}, false };
    }

    // Convert pixel snap distances to scene units using the current view transform
    const QTransform t = m_screenCanvas->transform();
    const qreal viewScale = (t.m11() > 1e-9) ? t.m11() : 1.0;
    const qreal snapDistScene       = m_screenCanvas->snapDistancePx()       / viewScale;
    const qreal cornerSnapDistScene = m_screenCanvas->cornerSnapDistancePx() / viewScale;

    return DragSnapEngine::applyDragSnap(
        proposedScenePos,
        m_itemSceneSize,
        m_snapStore,
        snapDistScene,
        cornerSnapDistScene,
        shiftPressed);
}

void QuickDragSnapSession::end() {
    m_snapStore.clear();
    m_screenCanvas  = nullptr;
    m_itemSceneSize = QSizeF();
    m_active        = false;
}
