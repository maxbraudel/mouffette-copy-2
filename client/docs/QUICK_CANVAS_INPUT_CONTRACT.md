# Quick Canvas Input Contract

## Goals
- Deterministic gesture ownership: exactly one owner for a left-button gesture.
- Stable drag anchor: pointer-to-media offset captured at press and reused for full drag.
- Predictable deselection: only a gesture that starts on empty canvas can clear selection.
- Minimal runtime churn on drag: QML updates live geometry, C++ commits on drag end.

## Ownership Priority
For each left-button gesture:
1. Resize handle
2. Media
3. Canvas

Ownership is established at gesture start and remains stable until gesture end/cancel.

## Selection Contract
- Media selection is triggered on primary press.
- Drag start does not re-select when the same gesture already started on that media.
- Selection changes are published through selection chrome model updates.

## Drag Anchor Contract
- Press-time anchor is captured in content-root coordinates.
- Drag updates compute:
  - `mediaX = pressMediaX + (currentPointerContentX - pressPointerContentX)`
  - `mediaY = pressMediaY + (currentPointerContentY - pressPointerContentY)`
- Anchor is never recomputed at drag threshold crossing.

## Deselect Contract
- Empty-canvas deselect is allowed only if the last gesture press target was canvas.
- Releasing after a media press/drag must never clear selection.

## Mode Contract (InputCoordinator)
- `mode` is one of: `idle | move | resize | pan | text`.
- `ownerId` is required for non-canvas modes.
- `forceReset()` is recovery-only and should be rare.

## Coordinate Spaces
- View space: pointer positions from handlers.
- Content space: media geometry and drag math.
- Scene space: external integration points where needed.

Rule: all drag-anchor math must stay in content space.

## Validation Checklist
1. Unselected media quick press-drag keeps anchor under cursor.
2. Selected media drag keeps anchor under cursor.
3. Click media selects and stays selected on release.
4. Empty canvas press/release deselects.
5. Press media then release outside does not deselect.
6. Resize handle interaction never starts move/pan.
