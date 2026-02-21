# Quick Canvas Migration — Gate Status (Evidence-Based)

Date: 2026-02-21
Plan reference: `QUICK_CANVAS_MIGRATION_PLAN.md`
Method: conservative evaluation from repository evidence only (missing evidence => `FAIL` gate).

## Overall

- Current implementation is **aligned with Phase 4 objectives**.
- Recent overlay fix is **valid for final target architecture**, but it is **Phase 6 scope work landed early**.
- All gates up to and including **Phase 4** are now evidenced as complete in-repo.
- Program is **not yet final-rollout complete** because Phase 5+ gates remain open.

## Phase-by-phase gate checklist

| Phase | Gate status | Reason (strict) | Evidence |
|---|---|---|---|
| Phase -2 — structuring/hygiene | PASS | Boundary map and automated architecture checks are present and integrated in build flow. | `docs/ARCHITECTURE_BOUNDARIES.md`, `tools/check_architecture_boundaries.sh`, `tools/architecture_screen_canvas_allowlist.txt`, `build.sh` |
| Phase -1 — characterization harness | PASS | Baseline fixtures/scripts exist and are actively used. | `tests/baseline/` + `tests/baseline/invariance/fallback_legacy_path.json` |
| Phase -0.5 — operational controls | PASS | PR template is now placed at repo root and phase-label/process checks are enforced in CI workflow. | `../.github/PULL_REQUEST_TEMPLATE.md`, `../.github/workflows/quick-canvas-gates.yml`, `docs/RELEASE_CHANNELS.md`, `docs/RENDERER_INCIDENT_PROTOCOL.md` |
| Phase 0 — freeze/flag/telemetry | PASS | Runtime flag + telemetry path resolution are implemented. | `src/backend/managers/app/SettingsManager.cpp`, `src/backend/managers/app/MigrationTelemetryManager.cpp` |
| Phase 0.5 — contract extraction | PASS | Contracts and host abstraction are present and used. | `src/shared/rendering/ICanvasHost.h`, `src/shared/rendering/IMediaSceneAdapter.h`, `src/shared/rendering/IOverlayProjection.h`, `src/backend/controllers/CanvasSessionController.cpp`, `src/backend/handlers/ScreenEventHandler.cpp` |
| Phase 1 — Quick shell | PASS | Quick host/controller and `CanvasRoot.qml` path are integrated behind runtime selection with legacy fallback. | `src/frontend/rendering/canvas/QuickCanvasHost.cpp`, `src/frontend/rendering/canvas/QuickCanvasController.cpp`, `resources/qml/CanvasRoot.qml` |
| Phase 2 — camera/input parity | PASS | Phase-2 parity script passes with focal drift within threshold and is enforced by CI gate workflow. | `tests/baseline/run_phase2_interaction_parity.js` (`[PHASE2_PARITY] PASS`), `../.github/workflows/quick-canvas-gates.yml` |
| Phase 3 — static layer port | PASS | Static quick layers exist and are registered. | `resources/resources.qrc` (`ScreenItem.qml`, `UiZone.qml`, `RemoteCursor.qml`) |
| Phase 4 — media visual port | PASS | Media delegates + DTO bridge + text edit commit path implemented; perf delta now uses committed fixture logs and emits durable gate report artifacts. | `resources/qml/ImageItem.qml`, `resources/qml/VideoItem.qml`, `resources/qml/TextItem.qml`, `resources/qml/CanvasRoot.qml`, `src/frontend/rendering/canvas/QuickCanvasController.cpp`, `tests/baseline/run_phase4_visual_parity.js`, `tests/baseline/run_phase4_perf_delta.js`, `tests/baseline/fixtures/perf_logs/phase4_quick_auto_targeted.log`, `tests/baseline/fixtures/perf_logs/phase4_legacy_auto.log`, `tests/baseline/run_quick_canvas_gate_report.js`, `tests/baseline/artifacts/quick_canvas_gate_report.json`, `../.github/workflows/quick-canvas-gates.yml` |
| Phase 5 — selection/handles/snap visuals | FAIL | Planned quick artifacts not present (`SelectionChrome.qml`, `SnapGuides.qml`). | Missing files under `resources/qml/` |
| Phase 6 — overlays out of scene | PASS (early) | QWidget overlays are now re-anchored to visible Quick viewport and keep widget UX path. | `src/frontend/rendering/canvas/ScreenCanvas.cpp` (`setOverlayViewport`), `src/frontend/rendering/canvas/QuickCanvasHost.cpp`, `src/frontend/ui/overlays/canvas/CanvasGlobalOverlayHost.cpp` |
| Phase 7 — integration hardening | FAIL | No complete green E2E matrix evidence for session/upload/reconnect/remote-scene on Quick path. | Plan gate in `QUICK_CANVAS_MIGRATION_PLAN.md` |
| Phase 8 — pixel parity + performance | FAIL | Full visual signoff + proven p95 perf improvement not yet evidenced. | Plan gate in `QUICK_CANVAS_MIGRATION_PLAN.md`, perf script requires zoom data |
| Phase 9 — controlled rollout | FAIL | No staged rollout/soak evidence in repo artifacts. | Plan gate in `QUICK_CANVAS_MIGRATION_PLAN.md` |
| Phase 10 — legacy decommission | FAIL | Legacy still intentionally present (as required before final gates). | `src/frontend/rendering/canvas/LegacyCanvasHost.*` |

## Direct answer to "did we follow current phase objectives?"

- **Yes for Phase 4 implementation scope** (media visual port and DTO bridge are in place).
- **Not strictly phase-isolated**: overlay fix is **Phase 6-type work** introduced early to restore required widget UX in Quick mode.
- This is acceptable architecturally (it matches target architecture), but if phase discipline is strict, classify it as an **approved out-of-order dependency fix**.

## Remaining blockers after Phase 4 closure

1. Phase 5 implementation/signoff remains open (`SelectionChrome.qml`, `SnapGuides.qml` not present).
2. Phase 7 full integration-hardening matrix evidence remains open.
3. Phase 8+ rollout/decommission gates remain open by plan.
