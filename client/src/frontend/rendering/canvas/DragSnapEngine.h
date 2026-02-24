#ifndef DRAPSNAPENGINE_H
#define DRAPSNAPENGINE_H

#include <QLineF>
#include <QPointF>
#include <QSizeF>
#include <QVector>

class SnapStore;

// Result of a drag-snap computation.
struct DragSnapResult {
    QPointF snappedPos;          // final scene-space top-left position after snapping
    QVector<QLineF> guideLines;  // snap indicator lines to display (empty when no snap)
    bool snapped = false;
};

// Pure-static engine that computes drag (translation) snapping.
// Mirrors the logic of ScreenCanvas::snapToMediaAndScreenTargets but operates
// entirely on pre-built SnapStore data so it works in both widget and Quick canvas.
//
// Coordinate space: all inputs and outputs are in backend scene units.
class DragSnapEngine {
public:
    // Compute the snapped position for an item being dragged.
    //
    // proposedPos          – raw scene-space top-left position (item origin) from the drag gesture
    // movingItemSceneSize  – current rendered size of the item in scene units (baseSize * scale)
    // snapStore            – pre-built cache of all target edges and corners (excludes the moving item)
    // snapDistanceScene    – edge-snap engagement radius in scene units
    // cornerSnapDistScene  – corner-snap engagement radius in scene units (should be larger)
    //
    // Returns a DragSnapResult with snappedPos, guideLines, and a snapped flag.
    // When shiftPressed is false, returns the proposedPos unchanged with no guide lines.
    static DragSnapResult applyDragSnap(
        const QPointF& proposedPos,
        const QSizeF& movingItemSceneSize,
        const SnapStore& snapStore,
        qreal snapDistanceScene,
        qreal cornerSnapDistScene,
        bool shiftPressed);
};

#endif // DRAPSNAPENGINE_H
