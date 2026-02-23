# Quick Canvas Input Coordinator

## Scope

This document defines the authoritative input ownership model for Quick Canvas.

- Single input owner: `InputLayer.qml` (`inputCoordinator`)
- Render-only layers: `MediaLayer.qml`, `SelectionLayer.qml`, `SelectionChrome.qml`
- C++ commit authority: `QuickCanvasController` for selection sync and final move/resize commit

## Rollout Flag

Temporary rollout switch:

- Default mode: coordinator enabled.
- Legacy mode: pass `--legacy-input-arbitration` to disable coordinator ownership paths.

Example launch:

- `./MouffetteClient.app/Contents/MacOS/MouffetteClient --legacy-input-arbitration`

## State Model

Coordinator states:

- `idle`
- `move`
- `resize`
- `pan`
- `text`

Coordinator tracked context:

- `ownerId` (media id or `canvas`)
- `pressTargetKind` (`unknown | background | media | handle`)
- `pressTargetMediaId`

## Pointer-Down Priority Rules

Pointer-down arbitration order is deterministic:

1. Resize handle
2. Media body (select first, move after threshold)
3. Text-create (text tool active + background only)
4. Pan (background only)

## Ownership Rules

- Media delegates emit intent only (`primaryPressed` / `selectRequested`).
- Selection resize start/end is granted only by coordinator.
- Pan is denied if pointer-down begins on media or handle.
- Gesture ownership transitions to active mode only through coordinator begin/end methods.

## C++ Boundary

C++ remains authoritative for:

- selection synchronization (`handleMediaSelectRequested`)
- final move commit (`handleMediaMoveEnded`)
- resize commit (`handleMediaResizeEnded`)

QML owns live interaction and temporary visual geometry during active gesture.

## Verification Checklist

Run:

- `node tests/baseline/run_phase2_interaction_runtime_matrix.js`
- `node tests/baseline/run_phase4_visual_parity.js`
- `./build.sh`

Run both interaction modes:

- Coordinator mode (default): normal launch.
- Legacy mode: launch with `--legacy-input-arbitration` and compare behavior.

Manual smoke checklist:

1. Add media A, then media B.
2. First left-click on B selects B immediately.
3. Press-drag on selected B moves B; pan does not start.
4. Press-drag canvas background pans; media does not move.
5. Resize handle drag starts resize, blocks pan/move.
6. Text tool tap on background creates text; tap on media does not create text.
