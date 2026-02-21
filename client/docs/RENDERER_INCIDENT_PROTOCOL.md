# Quick Canvas Migration — Incident Protocol

## Trigger Conditions

Escalate immediately when any of the following is observed on quick renderer path:

- crash / assertion / startup failure
- upload/session/reconnect semantic regression
- severe interaction regression (selection, text edit, zoom/pan)
- release-gate script failure in CI or pre-release audit

## Immediate Response (SLA)

- **Acknowledge:** within 15 minutes
- **Mitigate:** within 30 minutes by forcing legacy path default
- **Stabilize:** within same business day with root-cause triage note

## Decision Tree

1. Is impact user-blocking or data-affecting?
   - Yes -> force legacy default now, halt quick rollout.
2. Is issue isolated to quick path and reproducible?
   - Yes -> keep legacy default, patch quick path, re-validate gates.
3. Is rollback insufficient?
   - Revert offending PR(s) and re-run baseline + phase checks.

## Ownership

- Primary owner: migration assignee on PR
- Secondary owner: reviewer of impacted subsystem
- Release owner: person approving channel promotion

## Required Incident Record

For every incident, record:

- reproduction steps
- affected channel(s)
- mitigation action time
- rollback status
- root cause summary
- follow-up gate to prevent recurrence
