# Quick Canvas Input Test Matrix

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
