# Quick Canvas Migration — Release Channels

This document operationalizes Phase -0.5 delivery controls from the migration plan.

## Channels

1. **Dev-only**
   - Default for active migration work.
   - `useQuickCanvasRenderer` may be enabled locally via settings/env.

2. **Internal dogfood**
   - Limited internal users.
   - Quick path opt-in only.
   - Daily review of migration telemetry and bug list.

3. **Staged production**
   - Incremental rollout cohorts.
   - Legacy path remains runtime fallback during soak.

## Promotion Criteria

- Dev-only -> Internal dogfood:
  - Build + baseline + phase checks pass.
  - No critical regression open in impacted area.

- Internal dogfood -> Staged production:
  - Soak period complete.
  - No P0/P1 regression unresolved.
  - Rollback validated in current build.

## Rollback Rule

If any release gate fails, immediately return default to legacy renderer and pause promotion.
