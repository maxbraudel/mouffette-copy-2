#include "frontend/rendering/canvas/SnapEngine.h"

#include "frontend/rendering/canvas/SnapStore.h"
#include "backend/domain/media/MediaItems.h"
#include "frontend/rendering/canvas/ScreenCanvas.h"

#include <algorithm>
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

SnapEngine::CornerSnapResult SnapEngine::applyCornerSnapWithTargets(int activeHandleValue,
                                                                     const QPointF& fixedScenePoint,
                                                                     qreal proposedW,
                                                                     qreal proposedH,
                                                                     bool shiftPressed,
                                                                     ScreenCanvas* screenCanvas,
                                                                     const SnapStore& snapStore) {
    CornerSnapResult result;
    const ResizableMediaBase::Handle activeHandleEnum = static_cast<ResizableMediaBase::Handle>(activeHandleValue);
    if (!screenCanvas || !snapStore.ready() || snapStore.corners().isEmpty()) {
        return result;
    }

    using H = ResizableMediaBase::Handle;
    const bool isCorner = (activeHandleEnum == H::TopLeft || activeHandleEnum == H::TopRight
        || activeHandleEnum == H::BottomLeft || activeHandleEnum == H::BottomRight);
    if (!isCorner) {
        return result;
    }
    if (!shiftPressed) {
        return result;
    }

    const qreal cornerZone = screenCanvas->cornerSnapDistancePx() / screenCanvas->effectiveViewScale();
    const qreal edgeZone = screenCanvas->snapDistancePx() / screenCanvas->effectiveViewScale();

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

    qreal bestCornerErr = std::numeric_limits<qreal>::max();
    QPointF bestCornerTarget;
    for (const QPointF& targetCorner : snapStore.corners()) {
        const qreal dx = std::abs(candidate.x() - targetCorner.x());
        const qreal dy = std::abs(candidate.y() - targetCorner.y());
        if (dx > cornerZone || dy > cornerZone) {
            continue;
        }
        const qreal err = std::hypot(dx, dy);
        if (err < bestCornerErr) {
            bestCornerErr = err;
            bestCornerTarget = targetCorner;
        }
    }

    qreal bestEdgeXDist = std::numeric_limits<qreal>::max();
    qreal bestEdgeX = 0.0;
    for (qreal edgeX : snapStore.edgesX()) {
        const qreal dist = std::abs(candidate.x() - edgeX);
        if (dist <= edgeZone && dist < bestEdgeXDist) {
            bestEdgeXDist = dist;
            bestEdgeX = edgeX;
        }
    }

    qreal bestEdgeYDist = std::numeric_limits<qreal>::max();
    qreal bestEdgeY = 0.0;
    for (qreal edgeY : snapStore.edgesY()) {
        const qreal dist = std::abs(candidate.y() - edgeY);
        if (dist <= edgeZone && dist < bestEdgeYDist) {
            bestEdgeYDist = dist;
            bestEdgeY = edgeY;
        }
    }

    const bool hasCorner = bestCornerErr < std::numeric_limits<qreal>::max();
    const bool hasEdgeX = bestEdgeXDist < std::numeric_limits<qreal>::max();
    const bool hasEdgeY = bestEdgeYDist < std::numeric_limits<qreal>::max();

    if (!hasCorner && !hasEdgeX && !hasEdgeY) {
        return result;
    }

    enum class Choice { None, Corner, EdgeX, EdgeY, EdgeXY };
    Choice choice = Choice::None;

    // Priority rule: if a valid corner snap exists, it ALWAYS wins over border snaps.
    // This prevents border/corner overlap conflicts where edge candidates could override
    // an intended corner capture.
    if (hasCorner) {
        choice = Choice::Corner;
    } else if (hasEdgeX && hasEdgeY) {
        choice = Choice::EdgeXY;
    } else if (hasEdgeX) {
        choice = Choice::EdgeX;
    } else if (hasEdgeY) {
        choice = Choice::EdgeY;
    }

    qreal snappedCornerX = candidate.x();
    qreal snappedCornerY = candidate.y();
    if (choice == Choice::Corner) {
        snappedCornerX = bestCornerTarget.x();
        snappedCornerY = bestCornerTarget.y();
    } else if (choice == Choice::EdgeX) {
        snappedCornerX = bestEdgeX;
    } else if (choice == Choice::EdgeY) {
        snappedCornerY = bestEdgeY;
    } else if (choice == Choice::EdgeXY) {
        snappedCornerX = bestEdgeX;
        snappedCornerY = bestEdgeY;
    }

    qreal outW = proposedW;
    qreal outH = proposedH;
    if (activeHandleEnum == H::TopLeft) {
        outW = fixedScenePoint.x() - snappedCornerX;
        outH = fixedScenePoint.y() - snappedCornerY;
    } else if (activeHandleEnum == H::TopRight) {
        outW = snappedCornerX - fixedScenePoint.x();
        outH = fixedScenePoint.y() - snappedCornerY;
    } else if (activeHandleEnum == H::BottomLeft) {
        outW = fixedScenePoint.x() - snappedCornerX;
        outH = snappedCornerY - fixedScenePoint.y();
    } else {
        outW = snappedCornerX - fixedScenePoint.x();
        outH = snappedCornerY - fixedScenePoint.y();
    }

    if (outW <= 0.0 || outH <= 0.0) {
        return result;
    }

    result.snapped = true;
    result.snappedW = outW;
    result.snappedH = outH;
    result.snappedEdgeX = snappedCornerX;
    result.snappedEdgeY = snappedCornerY;
    switch (choice) {
        case Choice::Corner: result.kind = CornerSnapKind::Corner; break;
        case Choice::EdgeX:  result.kind = CornerSnapKind::EdgeX; break;
        case Choice::EdgeY:  result.kind = CornerSnapKind::EdgeY; break;
        case Choice::EdgeXY: result.kind = CornerSnapKind::EdgeXY; break;
        default:             result.kind = CornerSnapKind::None; break;
    }
    return result;
}
