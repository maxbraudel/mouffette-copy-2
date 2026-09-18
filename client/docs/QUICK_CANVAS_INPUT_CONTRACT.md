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

- `CanvasDocument` is the only selection authority; QML consumes its projection.
- Media selection is triggered on primary press. An already-selected item becomes
  primary without clearing the group; Shift adds and promotes.
- Only the primary owns handles, editing overlays and transforms. Secondary
  selections retain their border and name, without hidden resize hit targets.
- Drag start does not re-select when the same gesture already started on that media.
- Selection changes are published through selection chrome model updates.
- One viewport-level move handler owns all media types; renderer delegates never
  own the native move grab.

## Drag Anchor Contract

- Press-time anchor is captured in content-root coordinates.
- Drag updates compute:
  - `mediaX = pressMediaX + (currentPointerContentX - pressPointerContentX)`
  - `mediaY = pressMediaY + (currentPointerContentY - pressPointerContentY)`
- Anchor is never recomputed at drag threshold crossing.

## Deselect Contract

- Empty-canvas deselect uses the current press owner's exact hit decision, not hover state.
- Releasing after a media press/drag must never clear selection.

## Text Edit Contract

- One selected text editor per canvas, owned by `TextEditSession`.
- Switching target or external deselection finishes the old editor.
- Committing text does not select it. Deletion cannot clear a newer editor.
- Entering editing preserves the existing document, highlight and outline.

## Alt/Option Scroll Contract

- Vertical scroll scales the primary item around its own center.
- Input packets accumulate into one provisional transaction; `liveTransforms`
  publishes at most once per rendered frame without changing document/model rows.
- Releasing Alt or `ScrollEnd`, including a zero-delta end, commits the final
  geometry. Phase-less mouse wheels commit after 160 ms without input.
- Another transform, copy and window suspension finish the transaction.
  Selection changes, revoked editing or removal cancel provisional geometry.
- The primary commits position and scale atomically once per gesture.

## Mode Contract (InputCoordinator)

- `mode` is one of: `idle | move | resize | pan | text`.
- `text` is synchronous text creation, distinct from the long-lived edit session.
- `ownerId` is required for non-canvas modes.
- `forceReset()` is recovery-only and should be rare.

## Coordinate Spaces

- Window space: handler `scenePosition` / `scenePressPosition`.
- View space: viewport-local pointer coordinates after mapping from the window.
- Content space: media geometry and drag math.
- Scene space: external integration points where needed.

Rule: all drag-anchor math must stay in content space.

See [ownership and lifecycle details](QUICK_CANVAS_INPUT_COORDINATOR.md).

## Validation Checklist

1. Unselected media quick press-drag keeps anchor under cursor.
2. Selected media drag keeps anchor under cursor.
3. Click media selects and stays selected on release.
4. Empty canvas press/release deselects.
5. Press media then release outside does not deselect.
6. Resize handle interaction never starts move/pan.
