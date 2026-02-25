#ifndef SNAPENGINE_H
#define SNAPENGINE_H

#include <QPointF>
#include <QSize>

class ResizableMediaBase;
class ScreenCanvas;
class SnapStore;

class SnapEngine {
public:
    enum class CornerSnapKind {
        None,
        Corner,
        EdgeX,
        EdgeY,
        EdgeXY
    };

    struct AxisSnapResult {
        qreal scale = 1.0;
        bool snapped = false;
        qreal snappedEdgeScenePos = 0.0;
    };

    struct CornerSnapResult {
        CornerSnapKind kind = CornerSnapKind::None;
        bool snapped = false;
        qreal snappedW = 0.0;
        qreal snappedH = 0.0;
        qreal snappedEdgeX = 0.0;
        qreal snappedEdgeY = 0.0;
    };

    static AxisSnapResult applyAxisSnapWithTargets(ResizableMediaBase* target,
                                                    qreal proposedScale,
                                                    const QPointF& fixedScenePoint,
                                                    const QSize& baseSize,
                                                    int activeHandleValue,
                                                    bool shiftPressed,
                                                    ScreenCanvas* screenCanvas,
                                                    const SnapStore& snapStore);

    static CornerSnapResult applyCornerSnapWithTargets(int activeHandleValue,
                                                       const QPointF& fixedScenePoint,
                                                       qreal proposedW,
                                                       qreal proposedH,
                                                       bool shiftPressed,
                                                       ScreenCanvas* screenCanvas,
                                                       const SnapStore& snapStore);
};

#endif // SNAPENGINE_H
