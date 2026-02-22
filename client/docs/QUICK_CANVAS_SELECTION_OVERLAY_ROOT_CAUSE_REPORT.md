# Quick Canvas Selection Overlay Root-Cause Report

Date: 2026-02-22

## Scope
Investigate why pan/zoom becomes jerky when selection border and resize handles are visible, and identify structural causes (not masking workarounds).

## Executive conclusion
Primary issue is **approach/architecture**, not a single bug:

1. Selection chrome is implemented as a **viewport-space overlay** that recomputes geometry from scene values on every camera transform.
2. Chrome rendering and interaction are split into many retained QML objects (repeaters, shape paths, hover/drag handlers), which increases per-frame CPU and hit-test pressure while camera moves.
3. Current implementation is performant enough when chrome is hidden, confirming overlay path is the dominant incremental cost.

This matches observed behavior exactly: smooth pan/zoom without chrome, jerky with chrome.

---

## Evidence from current code

### A) Camera transform updates are per-frame during pan/zoom
- `viewScale`, `panX`, `panY` are continuously updated in pan/zoom handlers.
- `CanvasViewport` applies these to `contentRoot` transform.

Relevant:
- `resources/qml/CanvasRoot.qml` (camera state + handlers)
  - pan/zoom state: lines around `34-36`
  - pan updates: lines around `626-627`
  - wheel zoom/pan updates: lines around `694-695`

### B) Selection chrome recomputes viewport geometry from camera transform
For each selected entry, chrome computes points using content transform:
- `_viewScale`, `_contentX`, `_contentY`
- `p1`, `p2`
- then sets `x`, `y`, `width`, `height` from those values.

Relevant:
- `resources/qml/SelectionChrome.qml`
  - transform-derived bindings: lines around `80-101`
  - chrome box geometry bindings: lines around `108-111`

Implication: every camera change invalidates these bindings and geometry for all selected chrome delegates.

### C) Chrome rendering is multi-node and dynamic
Each selected chrome delegate creates:
- one `Shape` with **two dashed `ShapePath`s**,
- one repeater of 8 handle items,
- each handle item has a visual `Rectangle` and handlers.

Relevant:
- `resources/qml/SelectionChrome.qml`
  - dashed shape paths: lines around `113-149`
  - 8-handle repeater: lines around `153+`

Implication: heavy retained object graph and stroke/path recomputation while camera moves.

### D) Interaction subsystem adds hit-test overhead
For each handle there is:
- `HoverHandler`
- `DragHandler`

Relevant:
- `resources/qml/SelectionChrome.qml`
  - `HoverHandler`: around `185`
  - `DragHandler`: around `205`

Even when not resizing, pointer/hover resolution over many interactive nodes adds cost during motion.

### E) Overlay depends on full selection model mapping
`SelectionChrome` consumes `selectionChromeModel` and also maps to `mediaModel` via id-index map.

Relevant:
- `resources/qml/SelectionChrome.qml`
  - model index rebuild: around `31-39`

This is manageable alone, but contributes to the per-frame invalidation surface.

### F) Bridge-side sync can still add background pressure
Quick path still listens to `QGraphicsScene::changed` and schedules media sync.
- Sync timer: 16ms
- On sync: rebuild `mediaModel`, rebuild `selectionChromeModel`, publish both.

Relevant:
- `src/frontend/rendering/canvas/QuickCanvasController.cpp`
  - 16ms sync timer: around `143`
  - scene changed connection: around `390-391`
  - rebuild full media model: around `897+`
  - rebuild selection/snap models: around `1076+`

Even if not the primary pan-path cause, this can intermittently steal frame budget when scene emits changes.

### G) Hidden legacy maintenance suppression is in place but not sufficient
- Hidden legacy mirror is still present, but visual maintenance is suppressed.

Relevant:
- `src/frontend/rendering/canvas/QuickCanvasHost.cpp` around `65-68`
- `src/frontend/rendering/canvas/ScreenCanvas.cpp` around `1621` and `1686`

So the remaining lag strongly points to Quick selection overlay architecture itself.

---

## Root-cause ranking

### 1) Structural mismatch (highest)
**Viewport-space chrome with per-frame JS geometry recomputation** during camera transforms.

### 2) Rendering complexity (high)
Dashed multi-path `Shape` per selection + handle delegate tree causes expensive scenegraph updates.

### 3) Interaction graph complexity (high)
Per-handle hover/drag handlers scale poorly and increase event/hit-test cost.

### 4) Bridge sync coupling (medium)
Background scene sync/model republish can create additional hitches when scene changes occur.

---

## Industry-standard approach
Modern editors and canvas tools typically use one of these approaches:

1. **World-space selection adorners** (same transform space as content), so pan/zoom is mostly matrix updates, not geometry recomputation in script.
2. **Single overlay renderer** (custom GPU path / custom item) for border + handles, not one object tree per handle.
3. **Single interaction controller** with mathematical hit-testing for handles, not many `HoverHandler`/`DragHandler` instances.
4. **Decoupled visual motion from model sync**: avoid rebuilding full models during camera movement.

In Qt Quick terms, common performant design is:
- one custom `QQuickItem` (or one lightweight QML item) for selection chrome drawing,
- one pointer handler for resize interaction,
- compute handle hit regions procedurally,
- keep selected bounds in scene space and apply transform matrices directly.

---

## Recommended redesign (no masking)

### Phase 1: Replace object-heavy chrome implementation
- Replace current per-selection repeater + per-handle handlers with:
  - one selection overlay item,
  - one draw path for border,
  - one procedural handle hit-test routine.
- Keep border/handles always visible.

### Phase 2: Move chrome to world space
- Parent chrome under `contentRoot` (scene space), not viewport layer.
- Maintain handle visual size via inverse scale (`1/viewScale`) only for handle glyph sizing (lightweight), not full geometry remapping.

### Phase 3: Reduce bridge-induced sync hitches
- Gate `pushMediaModelOnly` and `pushSelectionAndSnapModels` while interaction mode is `pan`/`zoom`.
- Publish only when needed (selection change, resize commit, move commit, text edits).

---

## Validation plan (to prove root fix)

1. Add frame-time markers around:
   - pan/zoom handler update,
   - selection overlay update,
   - selection overlay paint,
   - scene-model publish.
2. Compare before/after metrics:
   - average frame time,
   - p95 frame time,
   - dropped-frame ratio during 5s pan and 5s zoom tests.
3. Ensure border/handles remain always visible with no behavioral regression.

---

## Decision
Given current evidence, this is a structural design issue in the selection overlay architecture. The correct fix is to redesign overlay rendering and interaction topology, not to hide or simplify visuals conditionally during navigation.
