# Mouffette — Qt Quick Canvas Migration Plan (Maximum-Safety / Zero-Regression)

## 1) Mission and non-negotiable outcome

This migration must end with exactly this runtime architecture:

```text
CanvasViewPage (Qt Widgets)
 ├── Canvas container (QWidget)
 │     └── QQuickWidget (Qt Quick canvas engine)
 │           ├── Screen rectangles (Quick)
 │           ├── Background + frame (Quick)
 │           ├── Media items (Quick)
 │           │     ├── Image
 │           │     ├── Video (VideoOutput)
 │           │     └── Text (Text)
 │           └── Selection chrome / resize handles (Quick)
 ├── Global overlays (Qt Widgets)              ← unchanged
 └── Media attached overlays (Qt Widgets)      ← moved OUT of canvas
```

And preserve:

- no behavior change
- no style change
- no UX change
- no backend/protocol/session/file-upload behavior change
- no feature regression
- canvas logic remains source of truth

This is a controlled renderer swap, not a product redesign.

---

## 2) Why the current architecture is suboptimal for a modern creative editor

The current canvas stack (`QGraphicsView` + `QGraphicsScene` + custom media items) is functionally rich, but it is CPU-heavy for interactive creative workloads.

### 2.1 CPU repaint and relayout pressure

Hot paths (`ScreenCanvas.cpp`, `MediaItems.cpp`, `TextMediaItem.cpp`) are repeatedly hit during zoom/pan:

- geometry recomputation
- text shaping/layout invalidation
- dirty region propagation
- scene repaints for multiple layers/items

This creates frame instability when interaction frequency is high.

### 2.2 Vector/rich text zoom costs are amplified

Text styling and editability imply expensive layout interactions. With frequent transform updates:

- text metrics and painting are recomputed often
- selection/overlay updates trigger additional redraw work
- visible lag appears, especially on text-heavy scenes

### 2.3 Mixed media composition multiplies cost

The canvas simultaneously manages images, videos, text, snap guides, selection chrome, and overlays. In a CPU-based scene graph this can degrade smoothness under continuous manipulation.

### 2.4 Future graphical effects are constrained

Advanced effects (animated highlights, smooth outlines, blend/shader effects) are much easier and more performant in a GPU scene graph.

---

## 3) Why Qt Quick is the right renderer target

Qt Quick scene graph provides GPU-accelerated retained rendering optimized for transforms/composition, which directly addresses zoom/pan lag while keeping business logic in C++.

Key benefit: **rendering model changes, app contracts do not**.

---

## 4) Real codebase constraints discovered (must be handled explicitly)

Before migration, the current code has tight coupling that must be decoupled safely:

1. Session model stores concrete `ScreenCanvas*` today.
2. Controllers/handlers call `ScreenCanvas` concrete methods directly.
3. Upload workflow inspects `QGraphicsScene`/`QGraphicsItem` directly.
4. Overlay placement assumes graphics-scene-native coordinates.

Therefore, a robust plan needs a **contract extraction phase** before renderer replacement.

---

## 5) Golden invariants (quality contract)

The following invariants are mandatory throughout all phases:

1. `WebSocketClient` message schema unchanged.
2. Session identity semantics unchanged (`persistentClientId`, `canvasSessionId`, etc.).
3. Upload correctness unchanged (start/progress/finish/cancel/unload/remove).
4. Watch/reconnect semantics unchanged.
5. Existing settings and user workflow unchanged.
6. Old renderer remains runtime-selectable until final cutover gate passes.

---

## 6) Architecture contracts to introduce first (critical safety step)

To make “Qt Quick = renderer + input surface” true, introduce explicit interfaces.

### 6.1 Renderer-agnostic canvas host contract

Create `ICanvasHost` (C++ abstract interface) covering only operations needed by controllers/handlers:

- set screens / clear screens
- camera operations (recenter/reset/zoom transform exposure)
- selection/read-only media querying hooks
- overlay anchor projection API
- remote cursor update/hide
- scene launch status hooks

Implementations:

- `LegacyCanvasHost` → wraps existing `ScreenCanvas`
- `QuickCanvasHost` → wraps `QQuickWidget` + `QuickCanvasController`

### 6.2 Renderer-agnostic media scene adapter

Create `IMediaSceneAdapter` to avoid direct `QGraphicsScene` assumptions in upload/selection flows.

Responsibilities:

- enumerate media entities (id, fileId, type, sourcePath)
- report transform/selection state
- notify media add/remove/update

This keeps upload/session logic independent from rendering technology.

### 6.3 Overlay projection contract

Create `IOverlayProjection` API:

- scene/media logical coordinates → viewport coordinates
- viewport → global coordinates

All QWidget overlays must use this contract; no direct renderer internals.

---

## 7) Final target architecture (explicit mapping)

Post-migration runtime:

- `CanvasViewPage` remains QWidget shell.
- Canvas area hosts `QQuickWidget` through `QuickCanvasHost`.
- Quick renders:
  - background/frame
  - screens/ui zones/remote cursor
  - media visuals (image/video/text)
  - selection chrome + resize handles + snap guides
- All overlays remain QWidget-based and anchored via projection adapter.
- Business logic stays in controllers/managers/handlers and shared canvas logic layer.

---

## 8) Phase-by-phase migration (strict-gated)

## Phase -2 — Pre-migration codebase structuring and hygiene (recommended)

### Goals
- Stabilize architecture boundaries before any renderer work.
- Reduce hidden coupling and surprise regressions.

### Actions
1. Define module ownership map (single source of truth):
  - canvas logic
  - canvas rendering
  - overlays
  - media domain
  - sessions
  - upload/files
2. Enforce include direction rules:
  - backend must not depend on Quick/QML types
  - controllers/handlers depend on interfaces, not concrete render classes
3. Introduce naming and folder conventions for new quickcanvas module.
4. Add lightweight architecture lint checks:
  - disallow direct includes of `ScreenCanvas.h` outside legacy wrapper and renderer adapters
  - disallow QWidget overlay code from directly querying QGraphicsScene
5. Identify and remove dead/duplicate pathways in current canvas initialization code to minimize branch complexity.
6. Document migration-safe extension points in this file and link them from onboarding docs.

### Exit gate
- Module boundary map published and approved.
- Include-direction checks active in CI (or pre-merge scripts).
- Known coupling hotspots cataloged with owners.

---

## Phase -1 — Characterization test harness (must be green before refactor)

### Goals
- Capture current behavior exactly before structural changes.
- Make regressions immediately visible during migration.

### Actions
1. Build a deterministic canvas fixture suite:
  - text-heavy scene
  - mixed image/video/text scene
  - selection/snap stress scene
  - reconnect/upload active scene
2. Add interaction replay scripts:
  - zoom/pan path replay
  - drag/resize path replay
  - text edit lifecycle replay
3. Add screenshot-baseline snapshots for key checkpoints:
  - idle
  - selected item
  - resizing
  - overlays visible
4. Add protocol/session invariance tests:
  - no schema change in outbound/inbound messages
  - same session identity transitions
5. Add upload invariance tests:
  - same file IDs/media associations before vs after actions
  - same cancel/unload semantics

### Exit gate
- Baseline fixtures and replay scripts are reproducible locally and in CI.
- Snapshot baseline approved by product/design.
- Invariance tests pass on legacy path.

---

## Phase -0.5 — Operational safety and delivery controls

### Goals
- Ensure migration can proceed incrementally without blocking feature delivery.

### Actions
1. Add PR template section for migration risk assessment.
2. Require phase label on each PR (`phase--2`, `phase--1`, `phase-0.5`, etc.).
3. Add mandatory rollback note in each migration PR.
4. Define release channels:
  - dev-only
  - internal dogfood
  - staged production
5. Establish incident protocol for renderer regressions (owner, SLA, rollback decision tree).

### Exit gate
- Delivery process is documented and adopted by the team.
- Rollback ownership and escalation path are explicit.

---

## Phase 0 — Freeze + safety net + observability

### Goals
- Establish rollback-first migration environment.

### Actions
1. Create branch `feature/quick-canvas-migration`.
2. Add runtime feature flag `useQuickCanvasRenderer` in settings.
3. Add migration telemetry (FPS/frame time/input latency/error counters).
4. Capture baseline recordings/screenshots on canonical scenes.
5. Create parity checklist and release gate template.

### Exit gate
- Flag works in runtime.
- Baseline metrics recorded.
- Rollback path validated.

---

## Phase 0.5 — Contract extraction (mandatory)

### Goals
- Remove hard dependency on `ScreenCanvas` concrete type from orchestration paths.

### Actions
1. Introduce `ICanvasHost`, `IMediaSceneAdapter`, `IOverlayProjection`.
2. Add `LegacyCanvasHost` wrapper over current `ScreenCanvas`.
3. Update `SessionManager` to hold host abstraction pointer, not renderer concrete type.
4. Refactor `CanvasSessionController`, `ScreenEventHandler`, `UploadEventHandler` to use contracts only.
5. Keep behavior identical (legacy renderer still active).

### Exit gate
- App runs with only `LegacyCanvasHost` and zero behavior change.
- No direct concrete `ScreenCanvas` calls remain in orchestration modules (except inside legacy wrapper).

---

## Phase 1 — Introduce Qt Quick shell (no functional migration)

### Goals
- Integrate `QQuickWidget` host path with blank/static rendering.

### Actions
1. Add module:
   - `QuickCanvasHost.h/.cpp`
   - `QuickCanvasController.h/.cpp`
   - `qml/CanvasRoot.qml`
2. Update build (`Qt6::Quick`, `Qt6::QuickWidgets`, QML resources).
3. Plug `QuickCanvasHost` behind feature flag in session/controller creation path.
4. Render only background + frame with style parity.

### Exit gate
- App runs with both hosts selectable at runtime.
- No regressions when legacy path is selected.

---

## Phase 2 — Camera/input parity

### Goals
- Match pan/zoom behavior exactly.

### Actions
1. Implement camera model in `QuickCanvasController`:
   - anchor-preserving zoom
   - pan inertia semantics matching current behavior
2. Match wheel/trackpad gesture handling.
3. Validate reconnect/loading transitions remain identical.

### Exit gate
- Zoom focal drift within tolerance threshold.
- Input parity test scripts pass.

---

## Phase 3 — Static layer port

### Goals
- Port non-interactive scene visuals.

### QML components
- `ScreenItem.qml`
- `UiZone.qml`
- `RemoteCursor.qml`

### Exit gate
- Screens/zone/cursor visual parity approved.

---

## Phase 4 — Media visual port

### Goals
- Port image/video/text rendering while preserving media model state.

### QML components
- `ImageItem.qml`
- `VideoItem.qml` (`VideoOutput`)
- `TextItem.qml` (`Text`)

### Actions
1. Feed renderer DTO model from existing media logic.
2. Keep model/business state in C++ shared layer.
3. Text edit mode: use existing edit UX contract (overlay editor if needed).

### Exit gate
- Media visual parity and interaction parity tests pass.
- Text zoom performance meets target thresholds.

---

## Phase 5 — Selection/handles/snap visuals

### Goals
- Move interaction visuals to Quick while preserving selection/snap logic behavior.

### QML components
- `SelectionChrome.qml`
- `SnapGuides.qml`

### Exit gate
- Selection, resize handles, and snapping parity signed off.

---

## Phase 6 — Overlay migration out of scene

### Goals
- Keep QWidget overlays unchanged in UX but detached from renderer internals.

### Actions
1. Global overlays remain QWidget.
2. Media-attached overlays moved to QWidget layer above Quick host.
3. Position overlays using `IOverlayProjection` adapter only.
4. Validate clipping, z-order, hit-testing, and DPI scaling.

### Exit gate
- Overlay parity (function + visual) passes full test matrix.

---

## Phase 7 — Integration hardening (sessions/uploads/reconnect)

### Goals
- Prove unchanged behavior in all critical business flows.

### Actions
1. Session switching and per-client canvas persistence tests.
2. Upload/cancel/unload/reconcile tests on Quick path.
3. Watch/unwatch/reconnect/remote-scene launch-stop tests.

### Exit gate
- Full end-to-end matrix green on supported platforms.

---

## Phase 8 — Pixel parity + performance gate

### Goals
- Certify zero visible regression objective and measurable perf gains.

### Visual gates
- screenshot diff suites for canonical scenes
- typography/style/token parity checks

### Performance gates
- improved p95 frame time on text-heavy zoom scenes
- no new interaction stalls above threshold
- CPU utilization does not regress under stress

### Exit gate
- Engineering + product + design signoff.

---

## Phase 9 — Controlled rollout with dual runtime

### Goals
- Safe progressive enablement.

### Actions
1. Keep legacy path available by flag.
2. Internal opt-in first.
3. Progressive rollout with telemetry and bug triage.
4. Mandatory soak period before default flip.

### Exit gate
- Stability achieved for agreed duration.

---

## Phase 10 — Legacy renderer decommission

### Preconditions
- All prior gates passed
- no critical/open parity regressions
- rollback window complete

### Actions
1. Remove legacy rendering-only branches.
2. Keep common logic layer and contracts.
3. Update architecture docs and onboarding docs.

### Exit gate
- Qt Quick renderer is default and stable.

---

## 9) File structure (final)

```text
src/frontend/rendering/quickcanvas/
 ├── QuickCanvasHost.h/.cpp
 ├── QuickCanvasController.h/.cpp
 ├── interfaces/
 │    ├── ICanvasHost.h
 │    ├── IMediaSceneAdapter.h
 │    └── IOverlayProjection.h
 └── qml/
      ├── CanvasRoot.qml
      ├── ScreenItem.qml
      ├── UiZone.qml
      ├── RemoteCursor.qml
      ├── ImageItem.qml
      ├── VideoItem.qml
      ├── TextItem.qml
      ├── SelectionChrome.qml
      └── SnapGuides.qml
```

---

## 10) Mandatory acceptance criteria

1. Final runtime architecture matches target exactly.
2. No protocol/backend/session/upload semantic changes.
3. No user-visible behavior regressions in validated workflows.
4. Style parity accepted by design QA.
5. Text-heavy zoom performance is clearly improved.
6. Runtime rollback capability existed through rollout and was tested.

---

## 11) Full regression matrix

## Functional
- client select, canvas switch, session persistence
- drag/drop image/video/text
- selection, resize, snap, z-order
- text edit/create/save/cancel
- video control interactions
- upload start/progress/finish/cancel/unload/remove
- watch/unwatch and reconnect while on canvas
- remote scene start/stop and validation feedback

## Visual
- border/handle thickness
- spacing/margins/colors/token parity
- text metrics/layout parity by platform
- overlay alignment over transformed media

## Performance
- text-only stress zoom
- mixed-media stress pan/zoom
- long-session memory stability

---

## 12) Risks and mitigations

| Risk | Severity | Mitigation |
|---|---|---|
| Hidden coupling to ScreenCanvas concrete type | High | Phase 0.5 contract extraction mandatory |
| Overlay positioning mismatch | High | Projection contract + DPI/scale tests |
| Text rendering visual drift | High | platform-specific font parity checks + design signoff |
| Upload flow regression | High | renderer-agnostic media adapter + E2E upload gates |
| Rollout instability | High | dual runtime + progressive enablement + telemetry |

---

## 13) Rollback strategy

1. Keep `useQuickCanvasRenderer` flag available until full stabilization.
2. Ability to force legacy path in runtime config/build profile.
3. If any release gate fails, revert flag default to legacy immediately.
4. No destructive schema/config changes during migration window.

---

## 14) Team operating model (industry standard)

- Parallel workstreams:
  1. contract extraction + orchestration refactor
  2. quick renderer implementation
  3. overlays/projection integration
  4. QA automation + perf validation
- Weekly gate review, no phase skipping.
- Definition of Done:
  - all acceptance criteria passed
  - rollback tested
  - docs updated
  - product/design signoff complete

---

## 15) Final guarantee statement

If all phases and gates above are executed without skipping, this plan is designed to deliver the exact target architecture and preserve user-visible behavior/style while removing current zoom lag by moving rendering to Qt Quick GPU scene graph.
