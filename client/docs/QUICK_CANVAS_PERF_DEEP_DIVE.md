# Quick Canvas Performance Deep Dive (macOS)

## Scope
This report investigates residual jerkiness in Quick Canvas interactions (zoom / pan / resize), including selected-text zoom lag, after architecture migration and first-pass optimizations.

## What was inspected
- Qt bootstrap and renderer setup in `src/main.cpp`
- Quick shell creation and scene wiring in `src/frontend/rendering/canvas/QuickCanvasController.cpp` and `src/frontend/rendering/canvas/QuickCanvasHost.cpp`
- Legacy bridge/session wiring in `src/backend/controllers/CanvasSessionController.cpp` and `src/frontend/rendering/canvas/LegacySceneMirror.cpp`
- Legacy canvas maintenance cadence in `src/frontend/rendering/canvas/ScreenCanvas.cpp`
- QML hot paths: `resources/qml/CanvasRoot.qml`, `SelectionChrome.qml`, `SnapGuides.qml`, `TextItem.qml`

## Key findings (ranked)

### 1) Structural frame-pacing risk: `QQuickWidget` inside QWidget shell
The Quick renderer is embedded through `QQuickWidget` (`QuickCanvasController::initialize`) rather than a `QQuickWindow`/`QQuickView`-based top-level composition. In Qt, `QQuickWidget` commonly incurs extra offscreen render + composition overhead and does not match the smoothest frame pacing profile available with threaded Quick window paths.

Why this matches symptoms:
- Pan/zoom transform many items each frame (full-scene redraw pressure), while move can appear smoother because geometry churn can be more localized.
- Jerkiness is broad (not isolated to a single operation), consistent with a pipeline-level limiter.

### 2) Hybrid architecture still keeps a live legacy `ScreenCanvas`
Quick mode still constructs and keeps an active `ScreenCanvas` via `LegacySceneMirror`:
- `CanvasSessionController` creates `ScreenCanvas` explicitly.
- `QuickCanvasHost::create` sets the mirror hidden but still active and connected.
- `QuickCanvasController` uses `legacyMirror->scene()` as the authoritative scene.

Even hidden, the legacy scene/view path has active maintenance hooks and timers (scene-changed maintenance at ~16ms, zoom relayout at ~8ms). This competes for UI-thread/frame budget during scene-mutating operations (especially resize).

### 3) Legacy update mode forced to full viewport
Session configuration applies `QGraphicsView::FullViewportUpdate` to every host via interface call. In Quick mode this propagates to hidden `ScreenCanvas` through `QuickCanvasHost -> LegacySceneMirror -> ScreenCanvas`.

Impact:
- Any scene mutation in the hidden legacy view can force expensive full invalidation work.
- This is likely a major contributor to resize jerk (where scene mutation is continuous).

### 4) Selected-state overlay rendering still has expensive characteristics
`SelectionChrome.qml` currently forces `Shape.CurveRenderer` for dashed border rendering. For rapidly changing zoom/pan transforms, this renderer choice can be heavier than simpler geometry paths in some environments.

This aligns with "selected text zoom lag" reports where selection chrome is active and highly dynamic.

### 5) Snap guide delegates use full-size `Canvas` instances
`SnapGuides.qml` creates a full-parent `Canvas` per guide delegate and repaints with mapped endpoints. During resize/snap-heavy gestures this can increase CPU work and repaint overhead.

### 6) No explicit graphics backend / frame diagnostics setup
`main.cpp` has standard `QApplication` startup and no explicit Quick backend preference, swap interval policy, or scene graph diagnostics hooks. That means backend selection and pacing behavior are left to platform defaults, which can vary.

## Evidence pointers
- `src/frontend/rendering/canvas/QuickCanvasController.cpp`
  - `QQuickWidget` construction and QML source setup.
  - Scene sync timer at 16ms.
  - `QGraphicsScene::changed` / `selectionChanged` connections.
- `src/backend/controllers/CanvasSessionController.cpp`
  - Explicit `ScreenCanvas` creation for quick path bridge.
  - Host-wide `FullViewportUpdate` setting.
- `src/frontend/rendering/canvas/QuickCanvasHost.cpp`
  - Hidden legacy mirror still attached (`setVisible(false)` + scene hookup).
- `src/frontend/rendering/canvas/ScreenCanvas.cpp`
  - Scene change maintenance and relayout timers (16ms / 8ms cadence).
- `resources/qml/SelectionChrome.qml`
  - `Shape.CurveRenderer` usage.
- `resources/qml/SnapGuides.qml`
  - Per-guide `Canvas` delegates and endpoint mapping.
- `resources/qml/TextItem.qml`
  - Text rendering path (`Text.QtRendering`).

## Root-cause conclusion
The dominant issue appears architectural/frame-pacing related rather than a single bug:
1) `QQuickWidget` composition path sets a lower ceiling for smoothness under heavy transform workloads.
2) Quick mode still pays legacy-scene maintenance costs via the hidden `ScreenCanvas` bridge.
3) Selected/snap overlay rendering choices add measurable overhead on top of (1)+(2).

## Recommended remediation order

### Phase A (high impact, low risk)
1. Stop forcing `FullViewportUpdate` in quick-host sessions (keep for pure legacy only).
2. In quick mode, disable non-essential hidden-legacy maintenance during active gestures (overlay relayout/info refresh/sync work not required for immediate visual feedback).
3. Replace/relax heavy overlay render paths:
   - test `SelectionChrome` with geometry renderer/default instead of forced curve renderer,
   - avoid per-guide full-size `Canvas` delegates where possible.

### Phase B (high impact, medium risk)
4. Move quick interactions further away from legacy-scene mutation during gestures:
   - keep gesture visuals fully QML-local,
   - defer scene commits to gesture end or lower-frequency checkpoints.

### Phase C (highest potential impact, larger change)
5. Evaluate migration from `QQuickWidget` embedding to `QQuickWindow/QQuickView`-driven composition for the canvas surface to improve frame pacing and reduce composition overhead.

## Instrumentation to add next (for hard confirmation)
1. Per-frame timing buckets in Quick path (pan / zoom / resize):
   - input handling,
   - model update cost,
   - QML binding/update cost proxy,
   - scene commit cost.
2. Legacy bridge work counters while in quick mode:
   - scene-changed batches,
   - overlay relayout count/time,
   - forced full updates.
3. Runtime backend diagnostics (scene graph backend, render loop mode, vsync pacing).

## Success criteria
- Stable 60fps feel (or display-rate-matched smoothness) for pan/zoom on representative scenes.
- Selected-text zoom smoothness close to unselected-media zoom.
- Resize interaction with no visible stutter under normal content density.
- Quantified reduction in hidden legacy work while quick shell is active.
