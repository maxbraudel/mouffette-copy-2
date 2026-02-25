#include "frontend/rendering/canvas/DragSnapEngine.h"

#include "frontend/rendering/canvas/SnapStore.h"

#include <algorithm>
#include <cmath>
#include <limits>

// ---------------------------------------------------------------------------
// Internal helpers
// ---------------------------------------------------------------------------
namespace {

// Accumulates the best (minimum absolute delta) snap correction for one axis,
// and collects ALL target edge positions that produce that same best delta.
// This ensures that when both left and right edges of the moving item snap
// simultaneously (e.g. width match), both indicator lines are emitted.
struct AxisSnapAccum {
    qreal bestDelta    = 0.0;
    qreal bestDeltaAbs = std::numeric_limits<qreal>::max();
    QVector<qreal> indicators; // all target edge X (or Y) values at bestDelta
    bool adjusted = false;

    // Consider snapping fromEdge → toEdge. indicatorVal is the target edge coordinate
    // to display as a guide line if this snap wins.
    void consider(qreal fromEdge, qreal toEdge, qreal threshold) {
        const qreal delta = toEdge - fromEdge;
        const qreal absd  = std::abs(delta);
        if (absd >= threshold) return;

        if (absd < bestDeltaAbs - 1e-6) {
            // Strictly better — replace
            bestDeltaAbs = absd;
            bestDelta    = delta;
            indicators.clear();
            indicators.append(toEdge);
            adjusted = true;
        } else if (absd <= bestDeltaAbs + 1e-6) {
            // Equally good — accumulate (same snap distance, extra aligned edge)
            if (!indicators.contains(toEdge)) {
                indicators.append(toEdge);
            }
            adjusted = true;
        }
    }
};

} // namespace

// ---------------------------------------------------------------------------
// DragSnapEngine::applyDragSnap
// ---------------------------------------------------------------------------
DragSnapResult DragSnapEngine::applyDragSnap(
    const QPointF& proposedPos,
    const QSizeF& movingItemSceneSize,
    const SnapStore& snapStore,
    qreal snapDistanceScene,
    qreal cornerSnapDistScene,
    bool shiftPressed)
{
    if (!shiftPressed || !snapStore.ready()) {
        return DragSnapResult{ proposedPos, {}, false };
    }

    const qreal mW = movingItemSceneSize.width();
    const qreal mH = movingItemSceneSize.height();

    // Moving rect in scene space
    const QRectF movingRect(proposedPos, QSizeF(mW, mH));

    // Moving corners
    const QPointF movingCorners[4] = {
        movingRect.topLeft(),
        movingRect.topRight(),
        movingRect.bottomLeft(),
        movingRect.bottomRight()
    };

    const QVector<qreal>& edgesX = snapStore.edgesX();
    const QVector<qreal>& edgesY = snapStore.edgesY();

    // ── 1. Corner-to-corner snapping (seeds bestDx/bestDy) ───────────────
    // Find the single nearest corner match to establish the translation vector.
    // Then fall through to edge accumulation (which also picks up all aligned
    // edges at the snapped position, including opposite-side matches).
    bool cornerCaptured = false;
    qreal bestCornerErr = std::numeric_limits<qreal>::max();
    qreal cornerSeedDx  = 0.0;
    qreal cornerSeedDy  = 0.0;

    for (const QPointF& targetCorner : snapStore.corners()) {
        for (int i = 0; i < 4; ++i) {
            const qreal dx = std::abs(movingCorners[i].x() - targetCorner.x());
            const qreal dy = std::abs(movingCorners[i].y() - targetCorner.y());
            if (dx > cornerSnapDistScene || dy > cornerSnapDistScene) continue;
            const qreal err = std::hypot(dx, dy);
            if (err < bestCornerErr) {
                bestCornerErr    = err;
                cornerCaptured   = true;
                cornerSeedDx     = targetCorner.x() - movingCorners[i].x();
                cornerSeedDy     = targetCorner.y() - movingCorners[i].y();
            }
        }
    }

    // ── 2. Build axis accumulators ────────────────────────────────────────
    // For corners, seed the accumulators with the corner translation so that
    // additional aligned edges at the snapped position are also collected.
    // For pure edge snapping, start fresh from all target edges.
    AxisSnapAccum accumX, accumY;

    if (cornerCaptured) {
        // Seed X accumulator: the corner snap gives us a specific dx.
        // We "pre-accept" it by treating it as a consider() result.
        const qreal absDx = std::abs(cornerSeedDx);
        accumX.bestDelta    = cornerSeedDx;
        accumX.bestDeltaAbs = absDx;
        accumX.adjusted     = true;
        // Seed Y similarly
        const qreal absDy = std::abs(cornerSeedDy);
        accumY.bestDelta    = cornerSeedDy;
        accumY.bestDeltaAbs = absDy;
        accumY.adjusted     = true;

        // Now collect all X edges aligned with the snapped rect (within tight tolerance)
        const qreal snappedLeft  = movingRect.left()  + cornerSeedDx;
        const qreal snappedRight = movingRect.right() + cornerSeedDx;
        const qreal tightTol = std::min<qreal>(1.0, cornerSnapDistScene * 0.1);
        for (qreal ex : edgesX) {
            if (std::abs(ex - snappedLeft)  < tightTol) { if (!accumX.indicators.contains(ex)) accumX.indicators.append(ex); }
            if (std::abs(ex - snappedRight) < tightTol) { if (!accumX.indicators.contains(ex)) accumX.indicators.append(ex); }
        }
        // All Y edges aligned with the snapped rect
        const qreal snappedTop    = movingRect.top()    + cornerSeedDy;
        const qreal snappedBottom = movingRect.bottom() + cornerSeedDy;
        for (qreal ey : edgesY) {
            if (std::abs(ey - snappedTop)    < tightTol) { if (!accumY.indicators.contains(ey)) accumY.indicators.append(ey); }
            if (std::abs(ey - snappedBottom) < tightTol) { if (!accumY.indicators.contains(ey)) accumY.indicators.append(ey); }
        }
    } else {
        // Pure edge snapping: consider all four moving edges against all target edges
        for (qreal ex : edgesX) {
            accumX.consider(movingRect.left(),  ex, snapDistanceScene);
            accumX.consider(movingRect.right(), ex, snapDistanceScene);
        }
        for (qreal ey : edgesY) {
            accumY.consider(movingRect.top(),    ey, snapDistanceScene);
            accumY.consider(movingRect.bottom(), ey, snapDistanceScene);
        }

        // After finding the best X delta, collect ALL X edges that produce
        // the same delta from either moving edge (catches left+right simultaneous snaps)
        if (accumX.adjusted) {
            const qreal snappedLeft  = movingRect.left()  + accumX.bestDelta;
            const qreal snappedRight = movingRect.right() + accumX.bestDelta;
            const qreal tightTol = snapDistanceScene * 0.05;
            for (qreal ex : edgesX) {
                if (std::abs(ex - snappedLeft)  < tightTol && !accumX.indicators.contains(ex)) accumX.indicators.append(ex);
                if (std::abs(ex - snappedRight) < tightTol && !accumX.indicators.contains(ex)) accumX.indicators.append(ex);
            }
        }
        if (accumY.adjusted) {
            const qreal snappedTop    = movingRect.top()    + accumY.bestDelta;
            const qreal snappedBottom = movingRect.bottom() + accumY.bestDelta;
            const qreal tightTol = snapDistanceScene * 0.05;
            for (qreal ey : edgesY) {
                if (std::abs(ey - snappedTop)    < tightTol && !accumY.indicators.contains(ey)) accumY.indicators.append(ey);
                if (std::abs(ey - snappedBottom) < tightTol && !accumY.indicators.contains(ey)) accumY.indicators.append(ey);
            }
        }
    }

    if (!accumX.adjusted && !accumY.adjusted) {
        return DragSnapResult{ proposedPos, {}, false };
    }

    // ── 3. Build result ───────────────────────────────────────────────────
    const QPointF snappedPos(proposedPos.x() + (accumX.adjusted ? accumX.bestDelta : 0.0),
                             proposedPos.y() + (accumY.adjusted ? accumY.bestDelta : 0.0));

    QVector<QLineF> guides;
    // One vertical indicator line per unique snapped X edge
    for (qreal ix : accumX.indicators) {
        guides.append(QLineF(ix, -1e6, ix, 1e6));
    }
    // One horizontal indicator line per unique snapped Y edge
    for (qreal iy : accumY.indicators) {
        guides.append(QLineF(-1e6, iy, 1e6, iy));
    }

    return DragSnapResult{ snappedPos, guides, true };
}
