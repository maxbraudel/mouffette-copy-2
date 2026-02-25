#include "frontend/rendering/canvas/SnapEngine.h"

#include "frontend/rendering/canvas/SnapStore.h"
#include "backend/domain/media/MediaItems.h"
#include "frontend/rendering/canvas/ScreenCanvas.h"

#include <QTransform>
#include <cmath>
#include <limits>

SnapEngine::AxisSnapResult SnapEngine::applyAxisSnapWithTargets(ResizableMediaBase* target,
                                                                 qreal proposedScale,
                                                                 const QPointF& fixedScenePoint,
                                                                 const QSize& baseSize,
                                                                 int activeHandleValue,
                                                                 bool shiftPressed,
                                                                 ScreenCanvas* screenCanvas,
                                                                 const SnapStore& snapStore) {
    AxisSnapResult result;
    result.scale = proposedScale;

    if (!target || !screenCanvas || !snapStore.ready()) {
        return result;
    }

    using H = ResizableMediaBase::Handle;
    const H activeHandle = static_cast<H>(activeHandleValue);
    const bool isHorizontal = (activeHandle == H::LeftMid || activeHandle == H::RightMid);
    const bool isVertical = (activeHandle == H::TopMid || activeHandle == H::BottomMid);
    if (!isHorizontal && !isVertical) {
        return result;
    }
    if (!shiftPressed) {
        return result;
    }

    const QVector<qreal>& axisTargets = isHorizontal ? snapStore.edgesX() : snapStore.edgesY();
    if (axisTargets.isEmpty()) {
        return result;
    }

    const qreal snapDistanceScene = screenCanvas->snapDistancePx() / screenCanvas->effectiveViewScale();
    constexpr qreal releaseFactor = 1.4;
    const qreal releaseDist = snapDistanceScene * releaseFactor;

    const qreal halfW = (baseSize.width() * proposedScale) / 2.0;
    const qreal halfH = (baseSize.height() * proposedScale) / 2.0;
    const qreal movingEdgePos = [&]() {
        switch (activeHandle) {
        case H::LeftMid: return fixedScenePoint.x() - 2 * halfW;
        case H::RightMid: return fixedScenePoint.x() + 2 * halfW;
        case H::TopMid: return fixedScenePoint.y() - 2 * halfH;
        case H::BottomMid: return fixedScenePoint.y() + 2 * halfH;
        default: return 0.0;
        }
    }();

    auto computeScaleFor = [&](qreal edgeScenePos) {
        if (isHorizontal) {
            const qreal desiredHalfWidth = (activeHandle == H::LeftMid)
                ? (fixedScenePoint.x() - edgeScenePos) / 2.0
                : (edgeScenePos - fixedScenePoint.x()) / 2.0;
            if (desiredHalfWidth <= 0) return proposedScale;
            return (desiredHalfWidth * 2.0) / baseSize.width();
        }
        const qreal desiredHalfHeight = (activeHandle == H::TopMid)
            ? (fixedScenePoint.y() - edgeScenePos) / 2.0
            : (edgeScenePos - fixedScenePoint.y()) / 2.0;
        if (desiredHalfHeight <= 0) return proposedScale;
        return (desiredHalfHeight * 2.0) / baseSize.height();
    };

    bool snapActive = target->isAxisSnapActive();
    const H snapHandle = target->axisSnapHandle();
    const qreal snapTargetScale = target->axisSnapTargetScale();
    if (snapActive && snapHandle == activeHandle) {
        auto snappedEdgePosForScale = [&](qreal s) {
            const qreal hw = (baseSize.width() * s) / 2.0;
            const qreal hh = (baseSize.height() * s) / 2.0;
            switch (activeHandle) {
            case H::LeftMid: return fixedScenePoint.x() - 2 * hw;
            case H::RightMid: return fixedScenePoint.x() + 2 * hw;
            case H::TopMid: return fixedScenePoint.y() - 2 * hh;
            case H::BottomMid: return fixedScenePoint.y() + 2 * hh;
            default: return 0.0;
            }
        };
        const qreal snappedEdgePos = snappedEdgePosForScale(snapTargetScale);
        const qreal distToLocked = std::abs(movingEdgePos - snappedEdgePos);
        if (distToLocked <= releaseDist) {
            result.scale = snapTargetScale;
            result.snapped = true;
            result.snappedEdgeScenePos = snappedEdgePos;
            return result;
        }
        target->setAxisSnapActive(false, H::None, 0.0);
        snapActive = false;
    }

    qreal bestDist = snapDistanceScene;
    qreal bestScale = proposedScale;
    qreal bestEdge = 0.0;
    for (qreal edge : axisTargets) {
        const qreal dist = std::abs(movingEdgePos - edge);
        if (dist < bestDist) {
            const qreal candidateScale = computeScaleFor(edge);
            if (candidateScale > 0.0) {
                bestDist = dist;
                bestScale = candidateScale;
                bestEdge = edge;
            }
        }
    }

    if (!snapActive && bestScale != proposedScale && bestDist < snapDistanceScene) {
        target->setAxisSnapActive(true, activeHandle, bestScale);
        result.scale = bestScale;
        result.snapped = true;
        result.snappedEdgeScenePos = bestEdge;
        return result;
    }

    result.scale = proposedScale;
    result.snapped = false;
    result.snappedEdgeScenePos = 0.0;
    return result;
}

bool SnapEngine::applyCornerSnapWithTargets(int activeHandleValue,
                                             const QPointF& fixedScenePoint,
                                             qreal proposedW,
                                             qreal proposedH,
                                             qreal& snappedW,
                                             qreal& snappedH,
                                             QPointF& snappedCorner,
                                             bool shiftPressed,
                                             ScreenCanvas* screenCanvas,
                                             const SnapStore& snapStore) {
    const ResizableMediaBase::Handle activeHandleEnum = static_cast<ResizableMediaBase::Handle>(activeHandleValue);
    if (!screenCanvas || !snapStore.ready() || snapStore.corners().isEmpty()) {
        return false;
    }

    using H = ResizableMediaBase::Handle;
    const bool isCorner = (activeHandleEnum == H::TopLeft || activeHandleEnum == H::TopRight
        || activeHandleEnum == H::BottomLeft || activeHandleEnum == H::BottomRight);
    if (!isCorner) {
        return false;
    }
    if (!shiftPressed) {
        return false;
    }

    const qreal cornerZone = screenCanvas->cornerSnapDistancePx() / screenCanvas->effectiveViewScale();

    auto movingCornerPoint = [&](qreal w, qreal h) {
        QPointF tl;
        if (activeHandleEnum == H::TopLeft) {
            tl = QPointF(fixedScenePoint.x() - w, fixedScenePoint.y() - h);
            return tl;
        }
        if (activeHandleEnum == H::TopRight) {
            tl = QPointF(fixedScenePoint.x(), fixedScenePoint.y() - h);
            return QPointF(tl.x() + w, tl.y());
        }
        if (activeHandleEnum == H::BottomLeft) {
            tl = QPointF(fixedScenePoint.x() - w, fixedScenePoint.y());
            return QPointF(tl.x(), tl.y() + h);
        }
        tl = QPointF(fixedScenePoint.x(), fixedScenePoint.y());
        return QPointF(tl.x() + w, tl.y() + h);
    };

    const QPointF candidate = movingCornerPoint(proposedW, proposedH);
    qreal bestErr = std::numeric_limits<qreal>::max();
    QPointF bestTarget;
    for (const QPointF& targetCorner : snapStore.corners()) {
        const qreal dx = std::abs(candidate.x() - targetCorner.x());
        const qreal dy = std::abs(candidate.y() - targetCorner.y());
        if (dx > cornerZone || dy > cornerZone) {
            continue;
        }
        const qreal err = std::hypot(dx, dy);
        if (err < bestErr) {
            bestErr = err;
            bestTarget = targetCorner;
        }
    }

    if (bestErr == std::numeric_limits<qreal>::max()) {
        return false;
    }

    qreal outW = proposedW;
    qreal outH = proposedH;
    if (activeHandleEnum == H::TopLeft) {
        outW = fixedScenePoint.x() - bestTarget.x();
        outH = fixedScenePoint.y() - bestTarget.y();
    } else if (activeHandleEnum == H::TopRight) {
        outW = bestTarget.x() - fixedScenePoint.x();
        outH = fixedScenePoint.y() - bestTarget.y();
    } else if (activeHandleEnum == H::BottomLeft) {
        outW = fixedScenePoint.x() - bestTarget.x();
        outH = bestTarget.y() - fixedScenePoint.y();
    } else {
        outW = bestTarget.x() - fixedScenePoint.x();
        outH = bestTarget.y() - fixedScenePoint.y();
    }

    if (outW <= 0.0 || outH <= 0.0) {
        return false;
    }

    snappedW = outW;
    snappedH = outH;
    snappedCorner = bestTarget;
    return true;
}
