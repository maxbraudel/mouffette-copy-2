# Quick Canvas Migration — Numbered Prompt List (Copy-Ready)

Use prompts in order.
Each prompt is fully self-contained and copy-paste friendly.

---

## Prompt 1 — Implementation of Phase -2 (Codebase Structuring)

```text
You are implementing Phase -2 of the Mouffette Quick Canvas migration.

Phase objective:
Stabilize codebase structure and architecture boundaries before migration.

Mandatory constraints:
- Implement ONLY Phase -2.
- No future phase work.
- No behavior, UX, style, protocol, session, or upload semantic changes.
- Keep all runtime behavior identical.
- Keep changes minimal and reversible.

Required tasks:
1) Define and document ownership boundaries in code/docs for:
   - canvas logic
   - renderer layer
   - overlays
   - sessions
   - upload/files
2) Enforce include direction rules:
   - backend must not depend on Quick/QML types
   - orchestration must not depend on concrete renderer internals
3) Add architecture guardrails (checks/scripts/docs comments) to prevent forbidden coupling.
4) Safely simplify obvious duplicate initialization paths only if behavior-neutral.

Execution protocol:
- First list files you will touch and risks.
- Implement Phase -2 tasks.
- Run build/validation.
- Report changed files and remaining risks.

Exit criteria:
- Boundaries are explicit and documented.
- Guardrails are present.
- No behavior regression.
```

## Prompt 2 — Verification of Phase -2

```text
Audit Phase -2 completion and safety.

Phase plan recap (copied from implementation prompt):
- Objective: stabilize codebase structure and architecture boundaries before migration.
- Constraints:
   - implement only Phase -2
   - no future phase work
   - no behavior/UX/style/protocol/session/upload semantic changes
   - keep runtime behavior identical
   - keep changes minimal and reversible
- Required tasks:
   1) define and document ownership boundaries for canvas logic, renderer layer, overlays, sessions, upload/files
   2) enforce include direction rules (backend must not depend on Quick/QML; orchestration must not depend on concrete renderer internals)
   3) add architecture guardrails to prevent forbidden coupling
   4) safely simplify duplicate initialization paths only if behavior-neutral
- Exit criteria:
   - boundaries are explicit and documented
   - guardrails are present
   - no behavior regression

Output format (strict):
1) Exit criteria checklist PASS/FAIL
2) Missing items
3) Risk level (Low/Medium/High)
4) Decision: ADVANCE / DO NOT ADVANCE
5) If DO NOT ADVANCE: minimal remediation tasks

Rules:
- Audit only, no implementation.
- Be strict and conservative.
```

## Prompt 3 — Optional Deep Verification for Phase -2 (Architecture Conformance)

```text
Run an architecture conformance audit after Phase -2.

Phase plan recap (copied from implementation prompt):
- Objective: stabilize codebase structure and architecture boundaries before migration.
- Constraints:
   - implement only Phase -2
   - no future phase work
   - no behavior/UX/style/protocol/session/upload semantic changes
   - keep runtime behavior identical
   - keep changes minimal and reversible
- Required tasks:
   1) define and document ownership boundaries for canvas logic, renderer layer, overlays, sessions, upload/files
   2) enforce include direction rules (backend must not depend on Quick/QML; orchestration must not depend on concrete renderer internals)
   3) add architecture guardrails to prevent forbidden coupling
   4) safely simplify duplicate initialization paths only if behavior-neutral
- Exit criteria:
   - boundaries are explicit and documented
   - guardrails are present
   - no behavior regression

Must verify:
- include direction constraints are respected,
- no new forbidden dependencies,
- orchestration paths remain renderer-agnostic where intended,
- no behavior changes were introduced.

Output:
- confirmed-safe areas,
- violations,
- severity,
- exact fixes required.
```

---

## Prompt 4 — Implementation of Phase -1 (Characterization Baseline)

```text
You are implementing Phase -1 of the Mouffette Quick Canvas migration.

Phase objective:
Create deterministic baseline checks to detect regressions during migration.

Mandatory constraints:
- Implement ONLY Phase -1.
- No Quick migration work yet.
- No behavior/style/protocol/session/upload changes.

Required tasks:
1) Add deterministic fixtures/scenarios:
   - text-heavy zoom scene
   - mixed media scene
   - selection/snap stress scene
   - reconnect/upload active scene
2) Add interaction replay checks:
   - zoom/pan
   - drag/resize
   - text edit lifecycle
3) Add screenshot baseline checks.
4) Add invariance checks for:
   - protocol semantics
   - session identity behavior
   - upload behavior

Execution protocol:
- List touched files first.
- Implement baseline harness.
- Run baseline checks.
- Report artifacts and reproducibility.

Exit criteria:
- Baseline checks reproducible and green on legacy path.
- Baseline assets available for parity comparison.
```

## Prompt 5 — Verification of Phase -1

```text
Audit Phase -1 completion and safety.

Phase plan recap (copied from implementation prompt):
- Objective: create deterministic baseline checks to detect regressions during migration.
- Constraints:
   - implement only Phase -1
   - no Quick migration work yet
   - no behavior/style/protocol/session/upload changes
- Required tasks:
   1) add deterministic fixtures/scenarios (text-heavy zoom, mixed media, selection/snap stress, reconnect/upload active)
   2) add interaction replay checks (zoom/pan, drag/resize, text edit lifecycle)
   3) add screenshot baseline checks
   4) add invariance checks for protocol semantics, session identity behavior, upload behavior
- Exit criteria:
   - baseline checks reproducible and green on legacy path
   - baseline assets available for parity comparison

Output format (strict):
1) Exit criteria checklist PASS/FAIL
2) Missing items
3) Risk level
4) Decision: ADVANCE / DO NOT ADVANCE
5) Minimal remediation if needed

Rules:
- Audit only.
- No optimistic assumptions.
```

## Prompt 6 — Optional Deep Verification for Phase -1 (Baseline Quality)

```text
Audit baseline quality and determinism.

Phase plan recap (copied from implementation prompt):
- Objective: create deterministic baseline checks to detect regressions during migration.
- Constraints:
   - implement only Phase -1
   - no Quick migration work yet
   - no behavior/style/protocol/session/upload changes
- Required tasks:
   1) add deterministic fixtures/scenarios (text-heavy zoom, mixed media, selection/snap stress, reconnect/upload active)
   2) add interaction replay checks (zoom/pan, drag/resize, text edit lifecycle)
   3) add screenshot baseline checks
   4) add invariance checks for protocol semantics, session identity behavior, upload behavior
- Exit criteria:
   - baseline checks reproducible and green on legacy path
   - baseline assets available for parity comparison

Must verify:
- fixtures are deterministic,
- replay checks are stable,
- snapshot baselines are usable,
- invariance checks actually cover protocol/session/upload critical paths.

Output:
- quality score,
- weak points,
- required improvements.
```

---

## Prompt 7 — Implementation of Phase -0.5 (Operational Safety Controls)

```text
You are implementing Phase -0.5 of the Mouffette Quick Canvas migration.

Phase objective:
Add delivery-process safety controls before major refactor work.

Mandatory constraints:
- Implement ONLY Phase -0.5.
- No runtime behavior changes.

Required tasks:
1) Add migration PR checklist template.
2) Add phase labeling convention for migration PRs.
3) Require rollback note in migration PRs.
4) Define release channels (dev/internal/staged).
5) Define escalation and ownership protocol for regressions.

Execution protocol:
- List process/tooling/docs files touched.
- Implement controls.
- Show how team will use them.

Exit criteria:
- Controls are documented and operational.
- Rollback ownership and escalation path are explicit.
```

## Prompt 8 — Verification of Phase -0.5

```text
Audit Phase -0.5 completion.

Phase plan recap (copied from implementation prompt):
- Objective: add delivery-process safety controls before major refactor work.
- Constraints:
   - implement only Phase -0.5
   - no runtime behavior changes
- Required tasks:
   1) add migration PR checklist template
   2) add phase labeling convention for migration PRs
   3) require rollback note in migration PRs
   4) define release channels (dev/internal/staged)
   5) define escalation and ownership protocol for regressions
- Exit criteria:
   - controls are documented and operational
   - rollback ownership and escalation path are explicit

Output format:
1) Exit criteria PASS/FAIL checklist
2) Missing controls
3) Risk level
4) Decision ADVANCE / DO NOT ADVANCE
5) Minimal remediation
```

---

## Prompt 9 — Implementation of Phase 0 (Freeze + Flag + Telemetry)

```text
You are implementing Phase 0 of the Mouffette Quick Canvas migration.

Phase objective:
Create runtime safety net and observability.

Mandatory constraints:
- Implement ONLY Phase 0.
- No renderer replacement yet.
- No behavior/style/protocol/session/upload changes.

Required tasks:
1) Add runtime feature flag `useQuickCanvasRenderer` default OFF.
2) Keep legacy renderer path as default and working.
3) Add migration telemetry hooks for performance/regression monitoring.
4) Confirm immediate rollback is possible by flipping flag.

Execution protocol:
- Show where flag is read and applied.
- Build and validate both paths.

Exit criteria:
- Flag works.
- Legacy path unchanged.
- Telemetry available.
```

## Prompt 10 — Verification of Phase 0

```text
Audit Phase 0 completion and rollback safety.

Phase plan recap (copied from implementation prompt):
- Objective: create runtime safety net and observability.
- Constraints:
   - implement only Phase 0
   - no renderer replacement yet
   - no behavior/style/protocol/session/upload changes
- Required tasks:
   1) add runtime feature flag useQuickCanvasRenderer default OFF
   2) keep legacy renderer path as default and working
   3) add migration telemetry hooks for performance/regression monitoring
   4) confirm immediate rollback is possible by flipping flag
- Exit criteria:
   - flag works
   - legacy path unchanged
   - telemetry available

Output:
1) PASS/FAIL exit criteria
2) rollback readiness status
3) risks
4) ADVANCE / DO NOT ADVANCE
5) remediation if needed
```

---

## Prompt 11 — Implementation of Phase 0.5 (Contract Extraction Critical)

```text
You are implementing Phase 0.5 of the Mouffette Quick Canvas migration.

Phase objective:
Decouple orchestration/business logic from concrete ScreenCanvas rendering internals.

Mandatory constraints:
- Implement ONLY Phase 0.5.
- No Quick media rendering yet.
- No behavior/protocol/session/upload semantic changes.

Required tasks:
1) Create interfaces:
   - ICanvasHost
   - IMediaSceneAdapter
   - IOverlayProjection
2) Create LegacyCanvasHost adapter over existing ScreenCanvas.
3) Refactor orchestration modules to depend on interfaces:
   - session handling
   - canvas session controller
   - screen event handling
   - upload event handling
4) Remove concrete ScreenCanvas dependency from orchestration paths (except legacy adapter internals).

Execution protocol:
- Provide before/after dependency map.
- Build and run parity checks.

Exit criteria:
- Contract-based orchestration is active.
- Legacy runtime behavior remains identical.
```

## Prompt 12 — Verification of Phase 0.5

```text
Audit Phase 0.5 completion.

Phase plan recap (copied from implementation prompt):
- Objective: decouple orchestration/business logic from concrete ScreenCanvas rendering internals.
- Constraints:
   - implement only Phase 0.5
   - no Quick media rendering yet
   - no behavior/protocol/session/upload semantic changes
- Required tasks:
   1) create interfaces ICanvasHost, IMediaSceneAdapter, IOverlayProjection
   2) create LegacyCanvasHost adapter over existing ScreenCanvas
   3) refactor orchestration modules to depend on interfaces (session handling, canvas session controller, screen event handling, upload event handling)
   4) remove concrete ScreenCanvas dependency from orchestration paths (except legacy adapter internals)
- Exit criteria:
   - contract-based orchestration is active
   - legacy runtime behavior remains identical

Must verify:
- interfaces exist and are used,
- orchestration no longer relies on concrete ScreenCanvas internals,
- no behavior regression.

Output:
- PASS/FAIL by criterion,
- risks,
- ADVANCE / DO NOT ADVANCE,
- exact fix list if needed.
```

## Prompt 13 — Optional Deep Verification for Phase 0.5 (Regression Audit)

```text
Run deep regression audit after contract extraction.

Phase plan recap (copied from implementation prompt):
- Objective: decouple orchestration/business logic from concrete ScreenCanvas rendering internals.
- Constraints:
   - implement only Phase 0.5
   - no Quick media rendering yet
   - no behavior/protocol/session/upload semantic changes
- Required tasks:
   1) create interfaces ICanvasHost, IMediaSceneAdapter, IOverlayProjection
   2) create LegacyCanvasHost adapter over existing ScreenCanvas
   3) refactor orchestration modules to depend on interfaces (session handling, canvas session controller, screen event handling, upload event handling)
   4) remove concrete ScreenCanvas dependency from orchestration paths (except legacy adapter internals)
- Exit criteria:
   - contract-based orchestration is active
   - legacy runtime behavior remains identical

Must verify invariance of:
- protocol behavior,
- session behavior,
- upload/file behavior,
- reconnect/watch behavior,
- fallback legacy path.

Output:
- confirmed-safe areas,
- suspected regressions,
- severity,
- must-fix list.
```

---

## Prompt 14 — Implementation of Phase 1 (Quick Shell)

```text
You are implementing Phase 1 of the Mouffette Quick Canvas migration.

Phase objective:
Introduce Quick canvas shell path with minimal visuals only.

Mandatory constraints:
- Implement ONLY Phase 1.
- No media migration.
- No overlay migration.
- Keep fallback intact.

Required tasks:
1) Add QuickCanvasHost + QuickCanvasController.
2) Add CanvasRoot.qml (background/frame only).
3) Add Quick/QuickWidgets/QML build integration.
4) Wire runtime selection between LegacyCanvasHost and QuickCanvasHost via feature flag.

Execution protocol:
- Show touched files.
- Build and test both paths.

Exit criteria:
- Both runtime paths operational.
- Legacy path unchanged.
- Quick shell visible and stable.
```

## Prompt 15 — Verification of Phase 1

```text
Audit Phase 1 completion.

Phase plan recap (copied from implementation prompt):
- Objective: introduce Quick canvas shell path with minimal visuals only.
- Constraints:
   - implement only Phase 1
   - no media migration
   - no overlay migration
   - keep fallback intact
- Required tasks:
   1) add QuickCanvasHost + QuickCanvasController
   2) add CanvasRoot.qml (background/frame only)
   3) add Quick/QuickWidgets/QML build integration
   4) wire runtime selection between LegacyCanvasHost and QuickCanvasHost via feature flag
- Exit criteria:
   - both runtime paths operational
   - legacy path unchanged
   - quick shell visible and stable

Output:
- PASS/FAIL checklist,
- risks,
- ADVANCE / DO NOT ADVANCE,
- minimal remediation.
```

---

## Prompt 16 — Implementation of Phase 2 (Camera/Input Parity)

```text
You are implementing Phase 2 of the Mouffette Quick Canvas migration.

Phase objective:
Match pan/zoom/input behavior between legacy and quick paths.

Mandatory constraints:
- Implement ONLY Phase 2.
- No media migration yet.

Required tasks:
1) Implement anchor-preserving zoom.
2) Implement pan behavior parity.
3) Implement wheel + trackpad parity.
4) Keep compatibility with loading/reconnect transitions.

Exit criteria:
- interaction parity checks pass,
- zoom focal drift within defined tolerance.
```

## Prompt 17 — Verification of Phase 2

```text
Audit Phase 2 completion and interaction parity.

Phase plan recap (copied from implementation prompt):
- Objective: match pan/zoom/input behavior between legacy and quick paths.
- Constraints:
   - implement only Phase 2
   - no media migration yet
- Required tasks:
   1) implement anchor-preserving zoom
   2) implement pan behavior parity
   3) implement wheel + trackpad parity
   4) keep compatibility with loading/reconnect transitions
- Exit criteria:
   - interaction parity checks pass
   - zoom focal drift within defined tolerance

Output:
- PASS/FAIL by criterion,
- measured drift/perf notes,
- ADVANCE / DO NOT ADVANCE,
- exact remediation list.
```

---

## Prompt 18 — Implementation of Phase 3 (Static Layers)

```text
You are implementing Phase 3 of the Mouffette Quick Canvas migration.

Phase objective:
Port static non-interactive canvas layers to Quick.

Mandatory constraints:
- Implement ONLY Phase 3.
- No media items yet.

Required tasks:
1) Add ScreenItem.qml
2) Add UiZone.qml
3) Add RemoteCursor.qml
4) Bind data from C++ controller
5) Preserve visual ordering/style parity

Exit criteria:
- static layers match legacy behavior/appearance.
```

## Prompt 19 — Verification of Phase 3

```text
Audit Phase 3 completion.

Phase plan recap (copied from implementation prompt):
- Objective: port static non-interactive canvas layers to Quick.
- Constraints:
   - implement only Phase 3
   - no media items yet
- Required tasks:
   1) add ScreenItem.qml
   2) add UiZone.qml
   3) add RemoteCursor.qml
   4) bind data from C++ controller
   5) preserve visual ordering/style parity
- Exit criteria:
   - static layers match legacy behavior/appearance

Output:
- PASS/FAIL by criterion,
- visual mismatch list,
- ADVANCE / DO NOT ADVANCE,
- exact fixes.
```

---

## Prompt 20 — Implementation of Phase 4 (Media Visual Port)

```text
You are implementing Phase 4 of the Mouffette Quick Canvas migration.

Phase objective:
Port image/video/text rendering to Quick while preserving existing business logic.

Mandatory constraints:
- Implement ONLY Phase 4.
- No protocol/session/upload semantic change.
- Preserve text edit contract behavior.

Required tasks:
1) Add ImageItem.qml
2) Add VideoItem.qml using VideoOutput
3) Add TextItem.qml using Text
4) Bind media renderer DTO/state from existing model
5) Preserve transform, z-order, and selection-related visual behavior

Exit criteria:
- media visual parity achieved,
- text-heavy zoom performance improved vs legacy baseline.
```

## Prompt 21 — Verification of Phase 4

```text
Audit Phase 4 completion.

Phase plan recap (copied from implementation prompt):
- Objective: port image/video/text rendering to Quick while preserving existing business logic.
- Constraints:
   - implement only Phase 4
   - no protocol/session/upload semantic change
   - preserve text edit contract behavior
- Required tasks:
   1) add ImageItem.qml
   2) add VideoItem.qml using VideoOutput
   3) add TextItem.qml using Text
   4) bind media renderer DTO/state from existing model
   5) preserve transform, z-order, and selection-related visual behavior
- Exit criteria:
   - media visual parity achieved
   - text-heavy zoom performance improved vs legacy baseline

Output:
- PASS/FAIL criteria,
- parity gaps,
- perf delta summary,
- ADVANCE / DO NOT ADVANCE,
- fixes required.
```

## Prompt 22 — Optional Deep Verification for Phase 4 (Regression Audit)

```text
Run deep regression audit after media visual port.

Phase plan recap (copied from implementation prompt):
- Objective: port image/video/text rendering to Quick while preserving existing business logic.
- Constraints:
   - implement only Phase 4
   - no protocol/session/upload semantic change
   - preserve text edit contract behavior
- Required tasks:
   1) add ImageItem.qml
   2) add VideoItem.qml using VideoOutput
   3) add TextItem.qml using Text
   4) bind media renderer DTO/state from existing model
   5) preserve transform, z-order, and selection-related visual behavior
- Exit criteria:
   - media visual parity achieved
   - text-heavy zoom performance improved vs legacy baseline

Must verify:
- upload semantics unchanged,
- session/protocol invariance,
- reconnect/watch parity,
- fallback path health.

Output:
- safe areas,
- regressions,
- severity,
- must-fix list.
```

---

## Prompt 23 — Implementation of Phase 5 (Selection/Handles/Snap Visuals)

```text
You are implementing Phase 5 of the Mouffette Quick Canvas migration.

Phase objective:
Port selection/resize/snap visuals to Quick while preserving logic semantics.

Mandatory constraints:
- Implement ONLY Phase 5.
- Do not change snapping decision rules unless parity fix explicitly requires it.

Required tasks:
1) Add SelectionChrome.qml
2) Add SnapGuides.qml
3) Connect to existing selection/snap logic outputs

Exit criteria:
- visual + functional parity for selection/resize/snap.
```

## Prompt 24 — Verification of Phase 5

```text
Audit Phase 5 completion.

Phase plan recap (copied from implementation prompt):
- Objective: port selection/resize/snap visuals to Quick while preserving logic semantics.
- Constraints:
   - implement only Phase 5
   - do not change snapping decision rules unless parity fix explicitly requires it
- Required tasks:
   1) add SelectionChrome.qml
   2) add SnapGuides.qml
   3) connect to existing selection/snap logic outputs
- Exit criteria:
   - visual + functional parity for selection/resize/snap

Output:
- PASS/FAIL checklist,
- interaction parity issues,
- ADVANCE / DO NOT ADVANCE,
- remediation steps.
```

---

## Prompt 25 — Implementation of Phase 6 (Move Attached Overlays Out of Canvas)

```text
You are implementing Phase 6 of the Mouffette Quick Canvas migration.

Phase objective:
Move media-attached overlays out of scene rendering and keep them as QWidget overlays above Quick canvas.

Mandatory constraints:
- Implement ONLY Phase 6.
- No UX/style behavior changes.
- Use IOverlayProjection for coordinates.

Required tasks:
1) Keep global overlays QWidget-based.
2) Re-anchor media-attached overlays through projection mapping.
3) Validate hit-testing, z-order, DPI scaling.

Exit criteria:
- overlay parity (behavior + visual) is confirmed.
```

## Prompt 26 — Verification of Phase 6

```text
Audit Phase 6 completion.

Phase plan recap (copied from implementation prompt):
- Objective: move media-attached overlays out of scene rendering and keep them as QWidget overlays above Quick canvas.
- Constraints:
   - implement only Phase 6
   - no UX/style behavior changes
   - use IOverlayProjection for coordinates
- Required tasks:
   1) keep global overlays QWidget-based
   2) re-anchor media-attached overlays through projection mapping
   3) validate hit-testing, z-order, DPI scaling
- Exit criteria:
   - overlay parity (behavior + visual) is confirmed

Output:
- PASS/FAIL criteria,
- overlay alignment/hit-testing issues,
- ADVANCE / DO NOT ADVANCE,
- exact fixes.
```

## Prompt 27 — Optional Deep Verification for Phase 6 (Regression Audit)

```text
Run deep regression audit after overlay migration.

Phase plan recap (copied from implementation prompt):
- Objective: move media-attached overlays out of scene rendering and keep them as QWidget overlays above Quick canvas.
- Constraints:
   - implement only Phase 6
   - no UX/style behavior changes
   - use IOverlayProjection for coordinates
- Required tasks:
   1) keep global overlays QWidget-based
   2) re-anchor media-attached overlays through projection mapping
   3) validate hit-testing, z-order, DPI scaling
- Exit criteria:
   - overlay parity (behavior + visual) is confirmed

Must verify:
- no session/protocol/upload behavior changes,
- no overlay interaction regressions,
- fallback path operational.

Output:
- safe areas,
- regressions,
- severity,
- must-fix list.
```

---

## Prompt 28 — Implementation of Phase 7 (Integration Hardening)

```text
You are implementing Phase 7 of the Mouffette Quick Canvas migration.

Phase objective:
Harden end-to-end parity of all critical business flows.

Mandatory constraints:
- Implement ONLY Phase 7.
- Fix integration regressions only; no redesign.

Required tasks:
1) Validate session switching parity.
2) Validate reconnect/watch parity.
3) Validate upload/cancel/unload/remove parity.
4) Validate remote scene launch/stop parity.

Exit criteria:
- full E2E matrix is green on target platforms.
```

## Prompt 29 — Verification of Phase 7

```text
Audit Phase 7 completion.

Phase plan recap (copied from implementation prompt):
- Objective: harden end-to-end parity of all critical business flows.
- Constraints:
   - implement only Phase 7
   - fix integration regressions only; no redesign
- Required tasks:
   1) validate session switching parity
   2) validate reconnect/watch parity
   3) validate upload/cancel/unload/remove parity
   4) validate remote scene launch/stop parity
- Exit criteria:
   - full E2E matrix is green on target platforms

Output:
- PASS/FAIL matrix,
- remaining integration risks,
- ADVANCE / DO NOT ADVANCE,
- must-fix list.
```

---

## Prompt 30 — Implementation of Phase 8 (Pixel + Performance Gate)

```text
Execute Phase 8 quality gate.

Phase objective:
Formally certify visual parity + performance gains.

Required checks:
1) Screenshot parity comparisons on baseline scenarios.
2) Interaction replay parity outcomes.
3) Legacy vs Quick perf comparison:
   - text-heavy zoom
   - mixed-media pan/zoom
   - severe stall detection

Output:
- PASS/FAIL with blockers and severity.

Constraints:
- no broad implementation changes in this gate step.
```

## Prompt 31 — Verification of Phase 8 (Gate Decision)

```text
Review Phase 8 evidence and issue final gate decision.

Phase plan recap (copied from implementation prompt):
- Objective: formally certify visual parity + performance gains.
- Constraints:
   - no broad implementation changes in this gate step
- Required checks:
   1) screenshot parity comparisons on baseline scenarios
   2) interaction replay parity outcomes
   3) legacy vs Quick performance comparison (text-heavy zoom, mixed-media pan/zoom, severe stall detection)
- Output expectation from implementation prompt:
   - PASS/FAIL with blockers and severity

Output:
1) PASS/FAIL per criterion
2) blocker list
3) Decision: ADVANCE / DO NOT ADVANCE
4) exact remediation plan if needed
```

---

## Prompt 32 — Implementation of Phase 9 (Controlled Rollout)

```text
You are implementing Phase 9 of the Mouffette Quick Canvas migration.

Phase objective:
Enable safe staged rollout with dual runtime maintained.

Mandatory constraints:
- Implement ONLY Phase 9.
- Do not delete legacy path.

Required tasks:
1) Keep dual runtime path active.
2) Enable internal rollout controls.
3) Ensure telemetry and triage checkpoints are operational.
4) Define default-flip criteria and enforce them.

Exit criteria:
- rollout controls and observability are production-ready.
```

## Prompt 33 — Verification of Phase 9

```text
Audit Phase 9 completion.

Phase plan recap (copied from implementation prompt):
- Objective: enable safe staged rollout with dual runtime maintained.
- Constraints:
   - implement only Phase 9
   - do not delete legacy path
- Required tasks:
   1) keep dual runtime path active
   2) enable internal rollout controls
   3) ensure telemetry and triage checkpoints are operational
   4) define default-flip criteria and enforce them
- Exit criteria:
   - rollout controls and observability are production-ready

Output:
- PASS/FAIL criteria,
- rollout risk level,
- ADVANCE / DO NOT ADVANCE,
- remediation steps.
```

---

## Prompt 34 — Implementation of Phase 10 (Legacy Decommission)

```text
You are implementing Phase 10 of the Mouffette Quick Canvas migration.

Preconditions (must verify first):
1) All prior phase gates passed.
2) No critical regressions open.
3) Soak period complete.

Phase objective:
Remove legacy renderer-only code and finalize architecture.

Mandatory constraints:
- Implement ONLY Phase 10.
- Preserve behavior in final quick path.
- Keep business logic intact.

Required tasks:
1) Remove deprecated legacy renderer-only branches.
2) Keep shared contracts/logic clean and maintainable.
3) Update docs to final architecture.

Exit criteria:
- final architecture conformance confirmed,
- build/tests green,
- no regression evidence.
```

## Prompt 35 — Verification of Phase 10 (Final Conformance)

```text
Audit final architecture conformance and migration completion.

Phase plan recap (copied from implementation prompt):
- Preconditions:
   1) all prior phase gates passed
   2) no critical regressions open
   3) soak period complete
- Objective: remove legacy renderer-only code and finalize architecture.
- Constraints:
   - implement only Phase 10
   - preserve behavior in final quick path
   - keep business logic intact
- Required tasks:
   1) remove deprecated legacy renderer-only branches
   2) keep shared contracts/logic clean and maintainable
   3) update docs to final architecture
- Exit criteria:
   - final architecture conformance confirmed
   - build/tests green
   - no regression evidence

Output:
1) Final acceptance criteria PASS/FAIL matrix
2) Remaining risk list (if any)
3) Final recommendation:
   - MIGRATION_COMPLETE
   - COMPLETE_WITH_KNOWN_RISKS
   - NOT_COMPLETE
```

---

## Prompt 36 — Generic Gap-Fix Prompt (Use after any failed verification)

```text
Implement ONLY the remediation items from the failed verification report for phase <PHASE_NAME>.

Rules:
- no extra scope,
- no future phase work,
- preserve behavior/style/protocol/session/upload invariants.

After implementation:
- map each fixed item to the original failed check,
- confirm readiness for re-verification.
```

---

## Prompt 37 — Safe Cleanup Prompt (Use after any passed phase)

```text
Perform safe cleanup for the just-completed phase only.

Rules:
- remove only dead code proven obsolete by that phase,
- no behavior/style changes,
- keep fallback path unless phase explicitly allows removal.

Deliver:
- removed code list,
- safety justification for each removal.
```

---

## Quick usage map

- Implement phase N → Prompt for that phase implementation
- Validate phase N → Prompt for that phase verification
- If failed → Prompt 36
- Optional cleanup after pass → Prompt 37
- Continue to next numbered phase
