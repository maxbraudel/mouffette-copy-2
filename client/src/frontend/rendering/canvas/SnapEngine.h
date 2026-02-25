#ifndef SNAPENGINE_H
#define SNAPENGINE_H

#include <QPointF>
#include <QSize>

class ResizableMediaBase;
class ScreenCanvas;
class SnapStore;

class SnapEngine {
public:
    struct AxisSnapResult {
        qreal scale = 1.0;
        bool snapped = false;
        qreal snappedEdgeScenePos = 0.0;
    };

    static AxisSnapResult applyAxisSnapWithTargets(ResizableMediaBase* target,
                                                    qreal proposedScale,
                                                    const QPointF& fixedScenePoint,
                                                    const QSize& baseSize,
                                                    int activeHandleValue,
                                                    bool shiftPressed,
                                                    ScreenCanvas* screenCanvas,
                                                    const SnapStore& snapStore);

    static bool applyCornerSnapWithTargets(int activeHandleValue,
                                           const QPointF& fixedScenePoint,
                                           qreal proposedW,
                                           qreal proposedH,
                                           qreal& snappedW,
                                           qreal& snappedH,
                                           QPointF& snappedCorner,
                                           bool shiftPressed,
                                           ScreenCanvas* screenCanvas,
                                           const SnapStore& snapStore);
};

#endif // SNAPENGINE_H
