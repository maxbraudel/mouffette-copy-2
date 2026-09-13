## Summary

- What changed:
- Why:

## Risk Assessment

- Scope risk: Low / Medium / High
- Regression risk area(s): rendering / overlays / sessions / upload / reconnect / perf

## Rollback Plan (required)

- If issue occurs, immediate action:
  1. Revert the offending change
  2. Re-run the Qt Quick architecture and native test gates

## Validation

- [ ] `cd client && ./scripts/build-development.sh` passes
- [ ] Baseline checks pass (`node client/tests/baseline/run_baseline_checks.js`)
- [ ] Native `ctest` and `QmlArchitectureGate` pass
- [ ] Visual evidence attached when a QML surface changes

## Incident / Escalation

- Owner on-call:
- Escalation path followed from `client/docs/RENDERER_INCIDENT_PROTOCOL.md`
