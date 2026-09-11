# Quick Canvas Input Test Matrix

## Automated Qt regression suites

`CanvasInteraction` and `CanvasInteractionScaled` use real window input and
production QML, not a JavaScript approximation of the state machine:

- Creation tool, automatic selection, deselection, reselection and double-click editing.
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
changes, and deleted-ID requests. It does not initialize the full application.

`TextItemQml` / `TextItemQmlScaled`, `TextOutlineItem` and `TextOutlineMotion`
protect the prior highlight, border and movement optimizations.

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
