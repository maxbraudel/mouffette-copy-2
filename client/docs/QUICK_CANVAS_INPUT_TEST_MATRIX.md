# Quick Canvas Input Test Matrix

## Automated Qt regression suites

`CanvasInteraction` and `CanvasInteractionScaled` use real window input and
production QML, not a JavaScript approximation of the state machine:

- Creation tool, automatic selection, deselection, reselection and double-click editing.
- Double-click across the full text body at independent media/camera scales,
  including oversized clipped media, native hover and double-click events.
- Adjacent-media clicks cannot combine into text activation; Shift preserves
  multiselection and resize-handle double-clicks never activate text.
- Repeated selection and dragging for text, image and video delegates.
- External deselection during editing; switching text; Shift multiselection.
- Highlighted multiline text preservation, real keyboard edit, commit and re-entry.
- Topmost visible target, transparency, hidden media and reordered stacking.
- Exact resize handles, including their outer half without prior hover.
- Stationary cursor after geometry change; transformed canvas, pan and zoom.
- Deleting an active editor and selecting another item.
- Deleting an item mid-drag, then selecting and dragging another item.
- Deleting the active resize target releases the global handle and backend session.
- Middle-button pan held across the watchdog interval and model publication.
- Single background deselection and no duplicate selection dispatch per press.
- QML type/reference/binding/grab configuration warnings fail the suite.

`CanvasSelectionBackend` uses the real controller and scene to check external and
additive selection, late commits, pending content publication across selection
changes, and deleted-ID requests. Its packaged `CanvasPage` regression covers
creation, keyboard input, leaving/reopening editing, actual media enlargement,
viewport resize, caret placement and overlapping page controls. It also checks
initial text size against the camera square at multiple percentages/zooms, and
persisted scale without resizing old media. It does not initialize application
networking or runtime storage.

Its Alt/Option-scroll cases cover image/video/text, cursor over media or empty
canvas, mouse/trackpad, natural scrolling and selection locks. A publication
regression sends two 100-packet bursts: no document/media/selection model change
during input, one preview per rendered frame, and exactly one atomic geometry
change per selected media at commit. It also covers a final packet before the
next frame, zero-delta end without Alt, Alt key release, phase-less wheel timeout,
selection change, editing revocation and removal during a gesture.

`TextItemQml` / `TextItemQmlScaled`, `TextOutlineItem` and `TextOutlineMotion`
protect the highlight, border and movement optimizations. Continuous uniform
scaling at DPR 1/2 checks that text layout stays unchanged, existing outline masks
are reused throughout the gesture, and the settled raster quality is refreshed
in the background afterward. Gesture, release, worker and publication timings
are reported separately so deferred work cannot hide a release-time stall.
The outline tests also verify that another gesture, text/font/color/width/scale
change or source removal rejects obsolete quality results. Deleting an item
must not wait for its worker; opaque and translucent colors must preserve full
coverage when recolored after refinement.

JS baselines remain supplemental checks. Performance checks below are manual
workload checks, not claims made by the interaction suite.

## Functional Cases

1. Select media with left press; release without movement => remains selected.
2. Press empty canvas; release => clears selection.
3. Press media A then quickly drag in same gesture => no jump, anchor remains under cursor.
4. Press selected media and drag slowly => follows cursor exactly.
5. Select A, then press-drag B in one gesture => A deselected, B selected and dragged.
6. Press media and release outside media bounds => selection remains.
7. Resize-handle drag => resize only, no move/pan.
8. Text edit active => canvas pan/move arbitration behaves as expected.

## Robustness Edge Cases

1. Overlapping media, top-most receives ownership.
2. Rapid click-drag-click alternation (100 repetitions) => no stale owner state.
3. Drag cancel (Esc/system cancel) => no stuck interaction mode.
4. Zoomed canvas (>1.0 and <1.0) => drag anchor remains stable.
5. Mixed modifiers (Shift/Alt) => move/resize semantics preserved.

## Performance Checks

1. Continuous drag for 30s on scene with 100+ media items.
2. Input latency remains subjectively smooth (no hitch spikes).
3. No visible frame drops during drag + snap guide updates.

## Pass Criteria

- 100% functional pass.
- 0 invariant violations in logs.
- No selection/owner desync observed.
