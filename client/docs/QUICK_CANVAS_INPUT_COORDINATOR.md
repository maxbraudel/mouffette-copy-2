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

The Settings overlay consumes pointer and wheel events within its bounds,
including scroll-limit, momentum and modifier-wheel events rejected by its
Flickable. These events cannot pan/zoom the canvas or scale selected media, even
when the settings content fits without scrolling. The panel has no fixed title
band and uses the same native Qt Quick scrollbar styling as the timeline.

## Selection transforms and scene locking

A press on an already selected media preserves the selection and promotes it to
primary. Shift adds and promotes. `CanvasDocument::primarySelectedMediaId()` is
the explicit authority; when it disappears, the most recently activated surviving
selection takes over. Only the primary displays handles, buttons and editing
overlays. Secondary selections retain their border and name, and have no hidden
resize surfaces. The media list can select invisible items.

`QuickCanvasController` snapshots only the primary geometry. Movement, uniform
resize, free resize and Alt/Option scroll affect this one occurrence; group copy
and delete still act on all selected occurrences. Stationary secondary selections
remain valid snap targets. Modifier changes always derive from the original
geometry, without accumulated drift. Release commits the primary to the document
or its timeline draft. Free resize disables text fit when committed.

Alt/Option vertical scroll scales the primary around its center, independently of
natural scrolling. Input packets accumulate; `QQuickWindow::afterAnimating`
publishes the latest provisional transform once per frame. No document changes
occur during that preview. Releasing Alt or ScrollEnd commits once. Phase-less
mouse wheels finish after 160 ms; phased input has a 1500 ms missing-end timeout.
Changing selection or revoking editing cancels provisional transforms. Captured
timeline media use an explicit unsaved draft, discarded on seeking, changing the
primary or starting playback. Copy and window suspension finish scale gestures.

Uniform previews also defer text-outline raster-density changes. The renderer
keeps the existing glyph masks while their scene-graph transform changes, then
requests background refinement after the gesture settles. One shared worker
processes immutable contour values; font/document objects remain on the GUI
thread and GPU textures remain on the scene-graph thread. Each item has at most
one outstanding job, with changes coalesced into the latest request. A generation
check discards obsolete results after another gesture, content/style change or
view change. The old masks stay visible until a complete replacement is ready.
Text edits and viewport culling remain active; camera-only zoom and free resize
keep their normal quality policy. This avoids synchronous glyph rasterization
at resolution-bucket crossings and on release. The motion benchmark measures
gesture frames, release, worker preparation and publication separately.

Images and videos keep this same selectable, movable and resizable shell while
their content is loading or waiting for memory. Uniform and Alt resize operate on geometry independently of media residency. Loading
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

1. Resolve the exact resize handle and media at the press coordinates, prioritizing selected bodies over unselected media.
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
visibility, enabled state, opacity and stacking order. In editing mode, selected
bodies take priority over unselected media even when visually covered. Among
candidates with the same selection state, z and sibling order break ties. This
changes only input priority, never the document's media stacking. Clicking an
exposed part of another media or clearing selection restores access to it.
Decorative children and delayed DTO geometry are not alternate pickers. Fully transparent media disable
their whole input subtree so children cannot swallow another item's press.

Native `containmentMask` checks make media and resize surfaces eligible before
Qt grants a grab. Rejecting a move only after `DragHandler.active` becomes true
is too late to protect another target. Hover is cursor feedback, never ownership.
Resize masks use exact handle hit tests even without an earlier mouse move.

Left-button camera pan accepts both mouse and trackpad devices on every OS.
Its containment mask admits only background presses and retains that choice
through release, including when the press finishes a text edit or the pointer
crosses a media. The handler stays enabled throughout native grab cleanup.

The top input layer derives cursor feedback from the active gesture and the
same live hit tests: closed hand during pan, directional arrows on resize
handles, I-beam for the text tool or the active editor, arrow otherwise.
The wheel-only `MouseArea` shares this feedback: `acceptedButtons: Qt.NoButton`
alone still installs an arrow that masks lower layers. Sharing its cursor
binding also refreshes tool changes under a stationary pointer. Floating
controls retain their own cursors above the canvas input layer.

Qt documents the typed [containment mask](https://doc.qt.io/qt-6/qml-qtquick-item.html#containmentMask-prop)
and distinguishes [opacity from input eligibility](https://doc.qt.io/qt-6/qml-qtquick-item.html#opacity-prop).
Selection chrome keeps its `Repeater` and visuals inside the selection layer,
above the entire media layer. A local transform mirrors the content camera for
scene-space geometry and constant-size handles. The visuals must not be
reparented into the lower content root: [Qt's z order](https://doc.qt.io/qt-6/qml-qtquick-item.html#z-prop)
compares siblings, so even a very large child z cannot escape that subtree.

## Text edit lifecycle

Creation and the viewport's native `doubleTapped` signal share
`CanvasRoot.requestTextEditing`. Creation selects the placeholder; reopening
places the caret at the clicked window coordinate. Activation waits until the
pointer release and selection publication finish, resolves the current visual
by ID, and rechecks selection/editability. New presses, controller changes and
edit cancellation invalidate pending requests. `TextEditSession` remains the
only editor owner; neither a delayed callback nor an old renderer can revive it.
Selection, movement and double-click activation all use the same viewport-space
media picker, independent of scaled renderer subtrees. The one TapHandler keeps
the first tap's media/controller identity so taps on adjacent texts or across
workspace switches cannot combine into an edit request.

Only the primary, editable text can enter the canvas edit session. Opening B
finishes A first. External deselection finishes the current editor too. Finishing
snapshots the live document and releases ownership before emitting the commit:
synchronous listeners may publish selection or open a newer editor. `begin()`
rechecks selection and ownership after those callbacks. Destroying a visual
abandons only that visual's session.

A text commit changes content, never selection. A late commit to a non-primary
or removed media is ignored. Inspector style/focus changes do not themselves force an exit:
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

Interactive text creation uses `MOUFFETTE_CANVAS_TEXT_INITIAL_HEIGHT_PERCENT`
(integer 1..100, default 8) from `AppConfig`. Its initial scene height is
`D * percent / 100`; after fitting the placeholder, uniform media scale is that
height divided by the fitted base height. Centering and publication happen only
after this transform is set. The font size stays unchanged, fit-to-text continues
to size the block during editing, and zoom/resize never recalculates existing
media scale. `CanvasDocument.addText` accepts an optional scene height; callers
omitting it retain the original default scale. Persistence uses the existing
media scale field without a project migration. Rebuild after editing the bundled
`client/.env`, or restart with `--env-file` for an external configuration file.

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
