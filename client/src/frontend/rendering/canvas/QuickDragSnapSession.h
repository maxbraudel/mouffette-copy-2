#ifndef QUICKDRAGSNAPSESSION_H
#define QUICKDRAGSNAPSESSION_H

#include "frontend/rendering/canvas/DragSnapEngine.h"
#include "frontend/rendering/canvas/SnapStore.h"

#include <QHash>
#include <QPointF>
#include <QRectF>
#include <QSizeF>

class QGraphicsScene;
class ResizableMediaBase;
class ScreenCanvas;

// Owns the state for a single drag-snap session (one item being dragged).
// Lifecycle: begin() → update() per tick → end().
class QuickDragSnapSession {
public:
    QuickDragSnapSession() = default;

    // Call at drag-start. Rebuilds the snap cache for the moving item.
    void begin(ResizableMediaBase* item,
               QGraphicsScene* mediaScene,
               const QHash<int, QRectF>& sceneScreenRects,
               ScreenCanvas* screenCanvas);

    // Call every drag tick. proposedScenePos is the raw top-left scene position
    // from QML (not snapped). Returns a DragSnapResult with the snapped position
    // and guide lines. When shiftPressed is false, returns unmodified position.
    DragSnapResult update(const QPointF& proposedScenePos, bool shiftPressed) const;

    // Call at drag-end to release resources.
    void end();

    bool active() const { return m_active; }

private:
    SnapStore       m_snapStore;
    ScreenCanvas*   m_screenCanvas  = nullptr;
    QSizeF          m_itemSceneSize;
    bool            m_active        = false;
};

#endif // QUICKDRAGSNAPSESSION_H
