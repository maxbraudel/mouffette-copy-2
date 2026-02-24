#include "frontend/rendering/canvas/DragSnapEngine.h"

#include "frontend/rendering/canvas/SnapStore.h"

#include <algorithm>
#include <cmath>
#include <limits>

// ---------------------------------------------------------------------------
// Internal helpers
// ---------------------------------------------------------------------------
namespace {

struct EdgeSnapAccum {
    qreal bestDx    = 0.0;
    qreal bestDy    = 0.0;
    qreal bestDxAbs = std::numeric_limits<qreal>::max();
    qreal bestDyAbs = std::numeric_limits<qreal>::max();
    qreal indicatorX = std::numeric_limits<qreal>::quiet_NaN();
    qreal indicatorY = std::numeric_limits<qreal>::quiet_NaN();
    bool  adjusted   = false;

    void considerX(qreal fromEdge, qreal toEdge, qreal indicatorXVal, qreal threshold) {
        const qreal delta = toEdge - fromEdge;
        const qreal absd  = std::abs(delta);
        if (absd < threshold && absd < bestDxAbs) {
            bestDxAbs    = absd;
            bestDx       = delta;
            adjusted     = true;
            indicatorX   = indicatorXVal;
        }
    }

    void considerY(qreal fromEdge, qreal toEdge, qreal indicatorYVal, qreal threshold) {
        const qreal delta = toEdge - fromEdge;
        const qreal absd  = std::abs(delta);
        if (absd < threshold && absd < bestDyAbs) {
            bestDyAbs    = absd;
            bestDy       = delta;
            adjusted     = true;
            indicatorY   = indicatorYVal;
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

    // ── 1. Corner-to-corner snapping (highest priority) ──────────────────
    bool cornerCaptured = false;
    qreal bestCornerErr = std::numeric_limits<qreal>::max();
    QPointF bestCornerTranslation;
    QPointF bestCornerIndicator;

    for (const QPointF& targetCorner : snapStore.corners()) {
        for (int i = 0; i < 4; ++i) {
            const qreal dx = std::abs(movingCorners[i].x() - targetCorner.x());
            const qreal dy = std::abs(movingCorners[i].y() - targetCorner.y());
            if (dx > cornerSnapDistScene || dy > cornerSnapDistScene) {
                continue;
            }
            const qreal err = std::hypot(dx, dy);
            if (err < bestCornerErr) {
                bestCornerErr = err;
                cornerCaptured = true;
                bestCornerTranslation = targetCorner - movingCorners[i];
                bestCornerIndicator   = targetCorner;
            }
        }
    }

    if (cornerCaptured) {
        const QPointF snappedPos  = proposedPos + bestCornerTranslation;
        const QRectF  snappedRect(snappedPos, QSizeF(mW, mH));

        // Detect full overlap: check if all four borders align with any target rect
        // Full-overlap tolerance is tight to avoid false positives
        const qreal fullTol = std::min<qreal>(0.75, cornerSnapDistScene * 0.15);
        auto isFullOverlap = [&](const QRectF& r) {
            return std::abs(r.left()   - snappedRect.left())   < fullTol &&
                   std::abs(r.right()  - snappedRect.right())  < fullTol &&
                   std::abs(r.top()    - snappedRect.top())    < fullTol &&
                   std::abs(r.bottom() - snappedRect.bottom()) < fullTol;
        };

        // Check all target rects by reconstructing from corner pairs
        // (SnapStore doesn't expose rects directly; we detect overlap via edge proximity)
        const QVector<qreal>& edgesX = snapStore.edgesX();
        const QVector<qreal>& edgesY = snapStore.edgesY();
        bool fullOverlap = false;
        for (qreal ex : edgesX) {
            for (qreal ey : edgesY) {
                // We already matched one corner at bestCornerIndicator; if this ex,ey
                // forms the opposite corner, we have a full-overlap candidate
                const QPointF opp(ex, ey);
                const qreal diagDist = std::hypot(
                    std::abs(opp.x() - bestCornerIndicator.x()),
                    std::abs(opp.y() - bestCornerIndicator.y()));
                if (diagDist < 1.0) continue; // same corner
                // Construct candidate rect from these two corner coordinates
                QRectF candidate(
                    std::min(bestCornerIndicator.x(), ex),
                    std::min(bestCornerIndicator.y(), ey),
                    std::abs(ex - bestCornerIndicator.x()),
                    std::abs(ey - bestCornerIndicator.y()));
                if (isFullOverlap(candidate)) {
                    fullOverlap = true;
                    break;
                }
            }
            if (fullOverlap) break;
        }

        QVector<QLineF> guides;
        if (fullOverlap) {
            guides.append(QLineF(snappedRect.left(),  snappedRect.top(),    snappedRect.right(), snappedRect.top()));
            guides.append(QLineF(snappedRect.left(),  snappedRect.bottom(), snappedRect.right(), snappedRect.bottom()));
            guides.append(QLineF(snappedRect.left(),  snappedRect.top(),    snappedRect.left(),  snappedRect.bottom()));
            guides.append(QLineF(snappedRect.right(), snappedRect.top(),    snappedRect.right(), snappedRect.bottom()));
        } else {
            // Infinite cross through the snapped corner point
            guides.append(QLineF(bestCornerIndicator.x(), -1e6, bestCornerIndicator.x(),  1e6));
            guides.append(QLineF(-1e6, bestCornerIndicator.y(),  1e6, bestCornerIndicator.y()));
        }
        return DragSnapResult{ snappedPos, guides, true };
    }

    // ── 2. Edge (axis-aligned) snapping ──────────────────────────────────
    EdgeSnapAccum accum;
    const QVector<qreal>& edgesX = snapStore.edgesX();
    const QVector<qreal>& edgesY = snapStore.edgesY();

    for (qreal ex : edgesX) {
        accum.considerX(movingRect.left(),  ex, ex, snapDistanceScene);
        accum.considerX(movingRect.right(), ex, ex, snapDistanceScene);
    }
    for (qreal ey : edgesY) {
        accum.considerY(movingRect.top(),    ey, ey, snapDistanceScene);
        accum.considerY(movingRect.bottom(), ey, ey, snapDistanceScene);
    }

    if (accum.adjusted) {
        const QPointF snappedPos(proposedPos.x() + accum.bestDx,
                                 proposedPos.y() + accum.bestDy);
        QVector<QLineF> guides;
        if (!std::isnan(accum.indicatorX)) {
            guides.append(QLineF(accum.indicatorX, -1e6, accum.indicatorX, 1e6));
        }
        if (!std::isnan(accum.indicatorY)) {
            guides.append(QLineF(-1e6, accum.indicatorY, 1e6, accum.indicatorY));
        }
        return DragSnapResult{ snappedPos, guides, true };
    }

    // ── 3. No snap ───────────────────────────────────────────────────────
    return DragSnapResult{ proposedPos, {}, false };
}
