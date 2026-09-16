# Source-sized media textures

## Incident and cause

Large images could stall the canvas after free (Alt) resizing, although local
videos remained responsive. Images and passive remote video spans used
`RemoteVideoFrameItem`, previously a `QQuickPaintedItem` drawing a `QImage` into
`boundingRect()`. Local videos use Qt's `VideoOutput` instead.

The painted item's backing texture followed its logical width and height,
multiplied by the window device pixel ratio. Its requested `FramebufferObject`
target only enables accelerated painting on OpenGL; on Metal and Direct3D Qt
uses the raster image target. Every size change could allocate and repaint a
canvas-sized surface, then upload it to the GPU. Even the OpenGL target requires
resizing the framebuffer. See [Qt's painted item contract](https://doc.qt.io/qt-6/qquickpainteditem.html).

For example, a 10,000 × 10,000 logical item at DPR 2 requests 400 million pixels:
about 1.49 GiB for **one** four-byte-per-pixel buffer, before other CPU/GPU copies
or backend limits. These dimensions describe the presentation rectangle, not
the source image's resolution.

Normal resize changes the parent item's scale; Alt-resize changes the base
width/height. Shrinking with normal resize after a large Alt-resize therefore
did not shrink the painted backing surface. A pure scale change on a previously
small item was already geometrical; it must stay that way.

The decoder and residency manager are not size-dependent: the image is decoded
once at its source resolution, with orientation applied, and retained as an
immutable shared `QImage`. Geometry changes do not decode another image.

An isolated `QQuickRenderControl` probe on macOS/Metal with Qt 6.11.2 reproduced
the allocation growth using the previous renderer and a **32 × 24** source:

| Item dimensions | Target DPR | Observed old texture | RGBA pixel bytes |
| --- | --- | --- | --- |
| 128 × 96 | 1 | 128 × 96 | 49,152 |
| 2,048 × 1,024 | 1 | 2,048 × 1,024 | 8,388,608 |
| 128 × 96 | 2 | 256 × 192 | 196,608 |
| 2,048 × 1,024 | 2 | 4,096 × 2,048 | 33,554,432 |

These are texture pixel counts, not a measurement of total process memory.
The replacement's GPU tests retain the same **32 × 24** texture (3,072 RGBA pixel
bytes) even for a **1,000,000 × 750,000** destination at DPR 1 and 2.

## Rendering contract

`RemoteVideoFrameItem` retains its existing QML name but is now a `QQuickItem`
with a `QSGImageNode` created by the active window's scene-graph factory.

- Upload the source image on the rendering thread only when its `cacheKey()`
  changes or the scene-graph node is recreated.
- Keep texture dimensions independent of item size, transforms and window DPR.
  Normal resize, Alt-resize, pan and zoom reuse the texture.
- Keep linear sampling, source orientation, alpha and inherited opacity/clips.
- Own textures inside the scene-graph subtree so Qt destroys them on the correct
  thread. Keep the content key in that subtree too, so a recreated node cannot
  incorrectly skip its first upload.
- Clear the node when the frame disappears or the item has empty dimensions;
  repaint when the source is destroyed or replaced.

The shared frame source suppresses republication of the exact same immutable
image using its constant-time `cacheKey()`; it never compares all pixels.
Availability notifications only fire on empty/nonempty transitions, so video
frames do not repeatedly invalidate QML loader availability bindings.

Related canvas cleanup removes duplicate size bindings in `CanvasRoot.qml`:
`MediaVisual.qml` already propagates its anchored size to each delegate. The
media model also checks the expected row position before a linear lookup,
making the usual unchanged-order publication O(N) instead of O(N²).

## Regression coverage

`MediaFrameItem` exercises the actual scene graph and GPU readback, including
extreme destination dimensions, transforms, texture reuse, transparency and
frame/source lifecycle. It is also run at a higher device pixel ratio. Assertions
on texture dimensions and reuse prevent the allocation regression without
depending on machine-specific timing thresholds.

`CanvasSelectionBackend` checks that extreme geometry preserves the resident
asset, source pixels, byte count and frame notifications, and that importing an
identical file shares the resident frame without invalidating existing owners.
Its existing canvas cases cover normal/Alt/group resize and loading shells.
The remote lifecycle, video playback and media overlay suites exercise the
shared QML integration.

Validation on macOS 26.1 / Qt 6.11.2 / Metal: the development application and
tests build successfully; the GPU regression suite passes all nine functional
cases, and 34 of the 35 CTest entries pass across the full run and targeted
reruns. The remaining `MediaOverlayScaled` native-window suite has intermittent
focus/animation timing failures on the interactive desktop. Its toolbar case
passes independently after waiting for the actual wide layout. Test fixtures
also now keep panel rows and click targets within the real window dimensions
when Cocoa constrains a high-DPR window. The baseline, schema, interaction
parity/ownership/runtime-matrix and randomized-input checks pass.

For a manual reproduction, import an image and a video; perform large Alt-resizes
then shrink with normal resize, pan/zoom, and change selection. Repeat with a
transparent image and with remote display spans. The image texture should
remain source-sized throughout; only visible screen pixels need rendering.
