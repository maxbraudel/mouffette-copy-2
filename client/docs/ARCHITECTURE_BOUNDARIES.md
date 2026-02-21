# Mouffette Client — Phase -2 Architecture Boundaries

This document defines ownership boundaries and include-direction rules for the Quick Canvas migration pre-phase.

Scope: **Phase -2 only** (structure/guardrails). No runtime behavior changes.

## Ownership boundaries

### 1) Canvas logic
- Ownership: `src/frontend/rendering/canvas/` + canvas-facing interaction logic in `src/backend/domain/media/`.
- Responsibilities:
  - scene/camera/selection/snap interactions,
  - media-item render-side behavior,
  - canvas-specific interaction state.

### 2) Renderer layer
- Current renderer ownership: `ScreenCanvas` and related render primitives in `src/frontend/rendering/canvas/`.
- Future renderer (Quick) must stay behind explicit contracts.
- Renderer internals are not orchestration API.

### 3) Overlays
- Ownership:
  - canvas-attached overlay controls: `src/frontend/rendering/canvas/OverlayPanels.*`,
  - global widget overlays/panels: `src/frontend/ui/overlays/canvas/` and page/widget hosts.
- Policy: overlay UI remains presentation-layer concern, not backend orchestration concern.

### 4) Sessions
- Ownership: `src/backend/domain/session/SessionManager.*`.
- Responsibilities:
  - session identity/indexing,
  - per-client canvas-session metadata,
  - session lifecycle bookkeeping.

### 5) Upload/files
- Ownership:
  - upload transport/state: `src/backend/network/UploadManager.*`,
  - file identity/repository/tracking/cache: `src/backend/files/`.
- Responsibilities:
  - file IDs, remote presence tracking, upload/unload semantics,
  - no renderer implementation coupling should be required in final target architecture.

## Include-direction rules

### Rule A — Backend must not depend on Quick/QML types
- Forbidden in `src/backend/**`:
  - `QQuick*`, `QQml*`, `QtQuick`, `QtQml` includes.

### Rule B — Orchestration must not grow new concrete renderer coupling
- Orchestration scope: `src/backend/controllers/**`, `src/backend/handlers/**`, `src/backend/managers/**`.
- Concrete renderer includes from `frontend/rendering/canvas/*` are blocked for new files.
- Existing legacy couplings are tracked in an explicit allowlist and must shrink over migration phases.

## Guardrails (Phase -2)

- Check script: `tools/check_architecture_boundaries.sh`
- Legacy orchestration allowlist: `tools/architecture_screen_canvas_allowlist.txt`

Run manually:

```bash
./tools/check_architecture_boundaries.sh
```

The script fails when:
- backend introduces Quick/QML includes,
- orchestration adds new concrete canvas-renderer includes outside the allowlist.

## Temporary legacy exceptions (explicit debt)

Current allowed orchestration files with direct concrete canvas include are intentionally frozen in:
- `tools/architecture_screen_canvas_allowlist.txt`

These are migration debt markers, not target architecture.
