## Summary

- What changed:
- Why:
- Migration phase label (required): `phase--2` / `phase--1` / `phase-0.5` / `phase-0` / `phase-1` / ...

## Risk Assessment (Quick Canvas migration)

- Scope risk: Low / Medium / High
- Regression risk area(s): rendering / overlays / sessions / upload / reconnect / perf
- Affected runtime path(s): legacy / quick / both

## Rollback Plan (required)

- Runtime flag fallback:
  - [ ] Verified `useQuickCanvasRenderer=false` path is safe for this change
- If issue occurs, immediate action:
  1. Revert flag default to legacy
  2. Disable quick path in release channel
  3. Revert PR if needed

## Validation

- [ ] `./client/build.sh` passes
- [ ] Baseline checks pass (`node client/tests/baseline/run_baseline_checks.js`)
- [ ] Phase-specific checks attached (logs or screenshots)
- [ ] Legacy path sanity checked (if impacted)
- [ ] Quick path sanity checked (if impacted)

## Incident / Escalation

- Owner on-call:
- Escalation path followed from `client/docs/RENDERER_INCIDENT_PROTOCOL.md`
