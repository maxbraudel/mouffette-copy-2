# Quick Canvas: selection and input ownership

## Authorities

These are different states, each with one writer, not competing copies of selection:

| State | Authority | Consumers |
| --- | --- | --- |
| Selected media | `CanvasDocument::selectedMediaIds()` | Controller publishes `selectionChromeModel`; visuals, chrome, overlays and edit session consume it |
| Active text editor | Per-canvas `TextEditSession.activeEditor` | Hosted `TextItem.editing`, `anyMediaEditing` and `currentEditingMediaItem` are read-only projections |
| Pointer gesture | Per-canvas `InputLayer.inputCoordinator` | Native media/resize/pan handlers request ownership and release their own session |
| Committed content and geometry | C++ media items | Stable `MediaListModel` updates existing QML delegates |

The unused single-ID `SelectionStore` has been removed. Do not introduce a second
mutable selected-ID collection in QML alongside the scene. Widget/backend
selection follows the same projection path as pointer selection.

Live drag/resize geometry is temporary presentation state, not a second committed
model. Selection-only updates must not cancel pending content publication: it can
contain a text edit, style change, fit-to-text resize or newly created item.

## Selection transforms and scene locking

A press on an already selected media preserves the selection for dragging.
Double-click text activation and explicit list selection still replace it unless
Shift is held. `QuickCanvasController` snapshots the selected media's geometry
when a transform starts. Movement applies the active media's final displacement
to every snapshot. Resize applies its width/height factors and relative anchor
movement to each media's original rectangle. Alt changes base dimensions while
preserving each media's existing scale. Modifier changes never accumulate drift.

Only the manipulated media resolves snap targets; the other members of the
transform are excluded from those targets. The controller's `liveTransforms`
projection supplies content, selection chrome and overlays with the same
provisional geometry. Release commits the whole selection to `CanvasDocument`.
Text fit mode changes only when a free resize commits.

Images and videos keep this same selectable, movable and resizable shell while
their content is loading or waiting for memory. Uniform and Alt resize, including
group transforms, operate on geometry independently of media residency. Loading
reveals the content inside the existing delegate without resetting edited geometry;
content controls and settings still wait for media readiness.

`editingEnabled` combines project permission with the document's scene lock.
Both QML input and C++ commands enforce it. Test-scene startup and remote prepare
lock synchronously, discard provisional transforms, end native edit ownership,
and restore the committed visuals. Late release callbacks cannot commit an old
transaction, including after the scene stops. Camera navigation remains available.

## Native pointer lifecycle

The always-enabled canvas `PointHandler` observes left presses and their
release/cancellation, including presses which enter or leave text editing.

1. Resolve the exact resize handle and topmost media at the press coordinates.
2. Finish an editor when the press targets another item, a handle or the background.
3. `beginPrimaryGesture()` chooses **handle > media > canvas** once.
4. The same decision controls background deselection and media ownership.
5. A media press selects once; the single viewport-level move handler exceeds
   the drag threshold and starts a move using the original press anchor. Image,
   video and text renderers never participate in move ownership. A double tap
   may open the selected text's editor.
6. Release/cancellation clears the primary owner. Native drag handlers close
   their own granted mode; cleanup of an old item must not reset a newer owner.

`pressTargetKind` and `pressTargetMediaId` are derived from the primary owner.
Delegates must not assign owner fields. Recovery-only `forceReset()` clears both
the active mode and primary owner. Synchronous text creation uses `try/finally`,
not watchdog cleanup. The pan watchdog recognizes left and middle-button handlers.

`CanvasRoot.mediaIdAtPoint()` examines live delegates, their actual transforms,
visibility, enabled state, opacity and stacking order. Decorative children and
delayed DTO geometry are not alternate pickers. Fully transparent media disable
their whole input subtree so children cannot swallow another item's press.

Native `containmentMask` checks make media and resize surfaces eligible before
Qt grants a grab. Rejecting a move only after `DragHandler.active` becomes true
is too late to protect another target. Hover is cursor feedback, never ownership.
Resize masks use exact handle hit tests even without an earlier mouse move.

Qt documents the typed [containment mask](https://doc.qt.io/qt-6/qml-qtquick-item.html#containmentMask-prop)
and distinguishes [opacity from input eligibility](https://doc.qt.io/qt-6/qml-qtquick-item.html#opacity-prop).
Selection chrome reparents its `Repeater`, not individual delegates, respecting
[Qt's sibling ownership](https://doc.qt.io/qt-6/qml-qtquick-repeater.html#details).

## Text edit lifecycle

Only a selected, editable text can enter the canvas edit session. Opening B
finishes A first. External deselection finishes the current editor too. Finishing
snapshots the live document and releases ownership before emitting the commit:
synchronous listeners may publish selection or open a newer editor. `begin()`
rechecks selection and ownership after those callbacks. Destroying a visual
abandons only that visual's session.

A text commit changes content, never selection. A late commit cannot resurrect
an old selection. Inspector style/focus changes do not themselves force an exit:
live text styling must remain usable while editing.

One `TextEdit` document is retained across display/edit transitions. Suspending
the existing model `Binding` must not restore its initial empty value. Its explicit
restore policy preserves text and highlight without a synthetic text change on
entry. See [Qt Binding restore semantics](https://doc.qt.io/qt-6/qml-qtqml-binding.html#restoreMode-prop).
Standalone text previews may use a local edit flag; hosted canvas text always
derives editing from its injected session. The remote renderer is passive.

## Coordinates and lifetime

- Handler `scenePosition` means QQuickWindow coordinates, not backend scene units.
- Map from the window to viewport coordinates for picking.
- Map to content-root coordinates for drag anchors and deltas. Qt's mapping
  already accounts for pan and zoom; do not apply them a second time.
- C++ converts QML content coordinates at its backend boundary.
- Media IDs resolve through lifetime-guarded references. A queued request for
  a deleted item is ignored; never dereference a cached `QGraphicsItem*` first.

## Camera and viewport

`CanvasDocument` owns a camera center in scene coordinates and the scene-space
side length `D` of the square inscribed in the viewport. `QuickCanvasController`
receives the actual CanvasRoot size (logical pixels), independently of its
rendering window. It publishes the existing scale/pan projection:

```text
scale = min(viewportWidth, viewportHeight) / D
pan = viewportCenter - cameraCenter * scale
```

Resize only republishes that projection; it never changes the logical camera,
clamps its scale, fits content again, or schedules camera autosave. Manual pan
and cursor/pinch-anchored zoom update the document in one operation. Manual zoom
uses `1000 / D` with limits `[0.2, 10]`; fitting can exceed these limits, and
subsequent gestures can return gradually into the range. Screen labels, handles,
and snap thresholds continue to use logical screen pixels.

The local project viewport stores `cameraVersion: 2`, `centerX`, `centerY`, and
`squareSceneSize`, alongside the legacy matrix snapshot. A legacy matrix is
converted at the first positive viewport size. Restore precedes topology
publication so initial fitting cannot overwrite it. An untouched project saved
before its first fit has no viewport record. Camera changes use the existing
workspace autosave debounce; camera data never enters remote scene state.

## Regression verification

From `client`, configure with `BUILD_TESTING=ON`, then:

```sh
cmake --build --preset macos-debug --parallel 4
ctest --preset macos-debug --output-on-failure
ctest --test-dir out/build/macos-debug -R CanvasInteraction --output-on-failure
bash tools/check_architecture_boundaries.sh
```

`CanvasInteraction` drives production QML with real window mouse/keyboard events
and deterministic storage. It also runs at `QT_SCALE_FACTOR=2`.
`CanvasSelectionBackend` exercises the real controller, scene and publication
without application startup, networking or cache cleanup. See the
[test matrix](QUICK_CANVAS_INPUT_TEST_MATRIX.md).

The original failure was reproduced before the edit-session fix: an externally
deselected text, or A after clicking B, remained in editing mode and kept its media
handlers disabled. Per-delegate writes to global editing flags could then hide
that forgotten editor. The regressions are asserted directly; JS simulation and
static checks alone cannot test Qt's native grabs or QML binding lifecycles.
