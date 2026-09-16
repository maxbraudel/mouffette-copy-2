# Text outline rendering

`TextOutlineItem` reads the existing `TextEdit` document, so shaping, wrapping,
font fallback and glyph positions have the same authority as the text fill.
Its Qt private API usage is isolated in `TextOutlineItem.cpp` and requires the
matching Qt build. Immutable per-glyph masks and retained image nodes provide
the border; translucent borders use a cropped `ShaderEffectSource` to apply
alpha once across overlapping glyphs.

## Camera zoom stalls

The camera and Alt+wheel both transform existing scene-graph content. Previously,
only Alt+wheel enabled deferred raster updates. Camera zoom crossed density
buckets in `updatePolish`, cleared the glyph cache and synchronously stroked all
visible glyphs again on the GUI thread. Increasing density enlarged both the
CPU masks and the subsequent texture uploads. Once the camera stopped, those
costs disappeared because the new masks were cached.

The viewport cache aggravated this: zooming in made the visible rectangle
smaller, so it remained contained in the previous crop indefinitely. Refinement
then included many glyphs that were no longer on screen.

Density changes now retain the existing masks. A restartable 120 ms idle timer
coalesces inherited camera transforms before submitting quality work to the
existing shared, single-worker pool. Explicit Alt-resize release can submit
immediately. New motion invalidates pending results; worker results never
install obsolete content. Initial content still renders immediately.

The crop retains a 96-screen-pixel guard but is rebuilt when the old rectangle
extends more than twice that guard beyond the current viewport. This bounds
zoom refinement to useful content. When zooming out or panning reveals an
uncached glyph, its initial mask uses at most unit density; its own density is
tracked so it will be refined even if the camera remains in the same density
bucket. Translucent composition targets also follow the current screen density
when zooming out instead of retaining the previous magnified resolution.

## Regression coverage

- `TextOutlineMotion` measures complete offscreen Qt Quick/Metal frames,
  including GPU completion. Its camera cases use a 12,000-character paragraph,
  48 px Impact, a 30% border, a 1280 × 800 viewport, and 48 samples in each
  direction from 35% to 1000%. They cover normal and Retina density, opaque and
  translucent borders, and a native text control. They check that zoom-in
  generates/uploads no border masks, quality work starts after motion,
  the crop stays bounded, and neither movement nor publication stalls a frame
  beyond the 50 ms regression ceiling. This ceiling tolerates CI load; it is
  not the interactive frame-time target.
- `TextOutlineItem::cameraZoomRefinesOnlyAfterMotionStops` checks idle coalescing
  without any explicit gesture hint and compares the final 1000% border with
  the document's vector geometry. Existing tests cover stale worker results,
  destruction, cache budgets, font fallback, wrapping and fractional motion.
- `CanvasInteraction::selectedBorderedTextRetainsMasksDuringCameraZoom` drives
  the production canvas with selected text and verifies retained masks,
  unchanged document geometry and final refinement at both test scale factors.
- `TextItemQml` checks alignment, clipping and uniform translucent alpha.

On macOS 26.1 / Qt 6.11.2 / Metal, the original Retina camera-zoom fixture
reproduced a 675.5 ms worst frame (p95 236.0 ms), with 216 glyph masks regenerated
and 207 uploaded during zoom-in. The corrected path performs zero mask
regenerations and zero uploads during that gesture. Exact timings are printed
by the test and depend on the machine and concurrent load. In the complete
native validation run, the corrected Retina zoom-in measured 2.05 ms p95 and
2.61 ms maximum; zoom-out measured 7.62 ms p95 and 10.43 ms maximum.

Both Development and Release applications built successfully. The complete
native suite passed 33 of 35 test suites. All outline tests and the new selected
camera-zoom case passed, including scale factor 2. Two existing scaled suites
failed and were independently reproduced using the pre-change Debug test
binaries: `CanvasInteractionScaled` clicks outside the available 720 × 374
window in five caret-selection fixtures, and `MediaOverlayScaled` has an
unrelated empty-screen-hint pixel mismatch. The architecture gate, baseline,
render schema and deterministic/randomized input checks passed.

```sh
cmake --build out/build/macos-release
ctest --test-dir out/build/macos-release --output-on-failure
out/build/macos-release/tst_TextOutlineMotion \
  'unchangedParagraphMotion:border-camera-zoom-retina'
```
