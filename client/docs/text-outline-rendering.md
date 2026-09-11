# Text outline rendering

## Findings

The previous implementation had two independent text layouts: the `TextEdit`
document for the fill, and `TextGlyphPath` for the border. The latter used
`WordWrap`, whereas `TextEdit.Wrap` uses `WrapAtWordBoundaryOrAnywhere`. It also
duplicated alignment, paragraph heights and trailing-space handling. A resize
shortcut could preserve old wrapped lines after the editor had reflowed them.
There is no single constant horizontal correction that fixes those differences.

The SVG renderer also discarded every glyph subpath except the largest. This
loses the dots of i/j, accents and components of punctuation. The pre-generated
Impact atlas was keyed by family name and glyph number, although glyph numbers
belong to a specific font face. This is a portability defect; the bundled face
was correctly selected on the machine used for this investigation.

The principal performance problem was the document-sized intermediate SVG.
For 12,000 characters, 48 px text and a 48 px outline radius, the reference test
produced 38,047,742 SVG bytes. Preparation alone took about 118 ms initially and
43 ms for an appended character in a Debug build, **before** Qt parsed the SVG
and tessellated the expanded contours. The QML component explicitly selected
synchronous Shape processing while editing. The atlas only saved the initial
per-glyph stroking; it could not remove those document-sized costs.

In addition, each live edit updated an invisible legacy `QGraphicsTextItem`,
triggering additional full-document layouts. Fit-to-text added border safety
space to the containing rectangle without applying matching content insets,
which clipped thick outlines on the aligned edge.

## Chosen implementation

`TextOutlineItem` reads each existing `QTextLine::glyphRuns()` from the actual
editor document. The resolved raw font, shaping, bidi order, capitalization,
wrapping, paragraph positions, padding and alignment therefore come from the
same source as the visible fill. It reads the editor's actual document offsets.
All glyph components are retained.

The first correction used Qt's `QSGCurveStrokeNode` and cached curve meshes. It
fixed editing costs, but **cached geometry is not cached rasterization**: the
analytic stroke shader still solves curve distances for every covered pixel on
every rendered frame. Thick overlapping outlines create considerable overdraw.
On the motion benchmark below, an unchanged paragraph consumed about 30.5 ms
per forced Retina frame; CPU outline polish/sync together were below 0.6 ms.
Pan and drag did not change the text or call the backend model. Neither a faster
layout nor another geometry cache could remove that GPU cost.

The renderer now paints each distinct glyph's exact stroke once into an
immutable `QImage`, using `QRawFont::pathForGlyph()` and `QPainter::strokePath()`.
This is a **per-glyph** operation, never a document-sized painter or texture.
Occurrences are ordinary `QSGImageNode` quads created by
`QQuickWindow::createImageNode()`. Textures are shared per glyph and created on
the scene-graph thread with `TextureCanUseAtlas`: Qt owns atlas packing, upload,
texture coordinates, the image shader and batching. If its atlas is full, Qt's
regular texture fallback remains correct. No custom curve shader/material,
private curve processor, atlas-packing implementation or new library is needed.

Masks are keyed by the actual raw font and glyph; the cache is invalidated when
outline radius or screen-density bucket changes. Half-octave density buckets
include DPR. Upgrades preserve screen detail, while downgrades have hysteresis;
a numerical tolerance prevents fractional translations at an exact zoom power
from toggling the resolution. Glyph textures are limited to approximately
2048 pixels per axis; extreme magnification can therefore soften their edges.
Unused CPU masks are evicted least-recently-used above a 64 MiB history budget
per outline item. The currently visible working set is retained even if it
alone exceeds that budget. GPU textures only retain the current working set.
Before replacing textures, obsolete image consumers and atlas slots are freed,
so old generations cannot unnecessarily force new glyphs out of the atlas.
Color changes recolor cached masks without reshaping or stroking them.

Small groups retain their scene-graph nodes. Common prefixes and suffixes
preserve group boundaries during edits; common alignment motion uses one
transform. Adjacent small fragments are coalesced. Qt's `ItemObservesViewport`
and `clipRect()` limit rendering to visible blocks/lines/glyphs plus a 96-DIP
screen-space guard. Transform-only changes inside that guard do not access the
document, rebuild groups, generate masks or upload textures. Crossing the guard
refreshes the visible placements; existing glyph images remain cached.

The earlier investigation also found three independent batching issues:

- `QSGTransformNode::setMatrix()` marks its subtree dirty even for an identical
  matrix. The adapter now compares before setting it.
- A multihash returned identical chunks in reverse occurrence order, swapping
  their nodes and moving otherwise unchanged geometry. Candidates now preserve
  occurrence order.
- Qt merged all chunks into one approximately 137 MB vertex buffer for the
  12,000-character stress case. A one-glyph change invalidated that entire batch.
  A material subclass initially isolated curve chunks. It is now removed:
  image quads reduce the measured 592-glyph paragraph from 56,784 triangles to
  1,184, and let Qt's ordinary image batching work efficiently.

No SVG is constructed or parsed in the live path, and the pre-generated Impact
atlas is no longer bundled or loaded. `TextGlyphPath` remains available only as
a reference test/benchmark target, not as a registered QML or application type.

The QML editor uses matching border insets. The canonical backend text and fit
geometry remain live, while synchronization of the hidden legacy editor is
deferred to commit. Quick Canvas resize gestures now mirror the native resize
lifecycle into the backend, so this unused document is also laid out once at
pointer release instead of once per queued pointer tick. Returning to the legacy
editor restores synchronization.

Non-uniform Alt-resize geometry is transactional. The visible QML delegate owns
the live width, height and position, including native text reflow and outline
updates. The hidden `QGraphicsItem` mirror keeps its original geometry during
pointer movement, avoiding repeated scene-index updates and legacy overlay
layout, then receives the final base size and position once on pointer release.
Releasing Alt mid-gesture commits that pending value before handing authority
back to uniform resize.

Translucent borders use a viewport-sized `ShaderEffectSource`, applying alpha
once instead of accumulating it at overlapping strokes. Its destination matches
the source crop exactly; simply changing `layer.sourceRect` would stretch the
crop across the whole document. Texture resolution follows the stable
screen-density/DPR bucket and is capped at 4096 pixels per axis, so a huge or
zoomed-out document does not allocate a document-sized texture. Fully transparent borders skip geometry
generation; opaque borders do not pay for this offscreen pass. A zero-width or
fully transparent border also disables viewport observation altogether. This is
important for borderless text: an empty outline adapter no longer wakes up,
polishes and requests scene-graph synchronization on every inherited camera
transform.
The guard keeps the crop and texture size stable during small translations.
Source and destination crops share integer-aligned local bounds, avoiding
Qt 6.11's fractional `ShaderEffectSource` target rounding (QTBUG-149373).

The remaining private Qt interface (editor offsets and font-engine lifetime)
is confined to one renderer translation unit. It needs
Qt 6.11+ and matching private headers/libraries; rebuilding and rerunning pixel
tests is required when upgrading Qt. The project already used Gui-private APIs
for its trailing-space behavior. Font engine caches populated on the GUI thread
are reset before the editor's render-thread work, following Qt's own practice.

## Alternatives investigated

| Option | Assessment for editable, wrapped text with a 0–100% border |
| --- | --- |
| `Text.Outline` / public `QSGTextNode` | Uses Qt's optimized text renderer, but exposes no configurable outline width. Qt's implementation uses a fixed outline. |
| `PathText` + `Shape.CurveRenderer` | Supports font paths and arbitrary pen widths, but does not supply the existing editor's multiline layout. Reconstructing it would preserve the alignment problem. |
| `QQuickPaintedItem` + `QTextCharFormat::TextOutline` | Simple CPU reference/fallback, but normally paints into an image and uploads it. The framebuffer optimization is OpenGL-specific, not Metal. |
| MSDF/MTSDF atlas (`msdfgen`) | A sound alternative with public scene-graph APIs and compact glyph quads. A 100% outline needs a large true-distance range/padding; integrating generation, eviction, font fallback and shaders is a substantial new renderer. |
| Image dilation / repeated shifted text | Work grows with the radius, glyph count or image area. The old brute-force shader is unsuitable for large text and thick borders. |
| Qt GPU curve adapter | Fixed the SVG/editing bottleneck, but analytic thick strokes still cost about 31 ms per unchanged Retina frame. Replaced. |
| Per-glyph raster masks + Qt image nodes/atlas | Keeps native shaping/editing; reuses Qt's standard texture rendering; one inexpensive quad per occurrence. Selected and covered by pixel, edit and motion tests. |

A cached `QPainterPathStroker` → `QSGCurveFillNode` alternative was also
implemented and tested, then removed. Pixel tests passed, but a 100% stroke
created almost 25 million triangles for the stress document and frame times
of tens of seconds. Solving all self-intersections in expanded outlines is not
an acceptable live rendering strategy. Merely setting the material's
`RequiresFullMatrix` flag also failed to solve the batching bottleneck.

## Verification

`tst_TextOutlineItem` uses real Qt Quick windows and the production Metal backend
on macOS. It compares border pixels against a QPainter reference made from the
same document's glyphs, verifies left/center/right positioning, disconnected
contours, wrap/resize, italic/fallback/RTL and outline-width transitions. The reference uses
nonzero winding, so overlapping strokes form a union rather than cancelling.
Additional cases cover fractional pan and zoom oscillations at exact density
boundaries, coordinates around 1,000,000, independent source/adapter reparenting,
and a history of more than 128 MiB of distinct masks evicted to the 64 MiB
budget. Pixel thresholds were retained when replacing curve rendering.

Performance measurements use `QQuickRenderControl` with a real Metal/QRhi
offscreen render target, like the rendering mechanism behind `QQuickWidget`.
The timer includes insertion, polish, synchronization, rendering and GPU
completion; it does not include screenshot readback or window presentation.
Pending Qt/platform events are dispatched between timed edits, as in the real
input loop, so deferred resource releases are not artificially held for a burst
of offscreen frames.
Both the normal viewport-culling path and an artificial all-glyphs path are
measured separately. The latter deliberately disables viewport observation to
exercise every placement and GPU node even when most of the line is off-screen.

Historical curve-renderer Release results, 12,000 characters, 48 px Impact text, 48 px
outline radius (100%), native fill visible in the production cases:

| Case | First edit | Steady p95 | Insert at start / middle |
| --- | ---: | ---: | ---: |
| No-wrap line, viewport on editing tail | 11.61 ms | 13.05 ms | 10.19 / 10.64 ms |
| Wrapped paragraph, viewport on editing tail | 21.79 ms | 19.79 ms | 20.80 / 20.89 ms |
| Artificial all-glyphs stress, culling disabled | 104.16 ms | 62.02 ms | 56.73 / 50.88 ms |

These historical numbers include the native editor's fill in production cases,
but not the backend model update. They are not complete application latency.
After replacing the curves with image quads, the final Debug editing run measured p95
11.50 ms (no-wrap), 10.26 ms (wrapped), and 23.65 ms (all-glyph stress), retaining
the existing incremental-edit and pixel assertions. This is not a like-for-like
Debug/Release speed comparison; the dedicated same-build motion comparison
below isolates the reported defect.

`tst_TextOutlineMotion` renders a 12,000-character wrapped Impact paragraph,
48 px font and 48 px outline radius, with native fill visible, to a 1280×800
logical Metal target at DPR 1 and 2. Camera pan and element drag are separate
cases, at zoom 1 and 0.35. A forced stationary pass isolates the GPU cost of
unchanged content. A 60-DIP motion sequence verifies zero document processing,
raster generation, chunk rebuilds and texture uploads. A further 480-DIP
sequence crosses the cache guard repeatedly and measures refresh frames too.
Translucent Retina cases use the same cropped `ShaderEffectSource` composition
as production; a readback outside timing verifies the actual 50% alpha.

Same-machine Release motion results, September 11, 2026; all values in ms.
The baseline executable was preserved before rebuilding the renderer.

| Retina case | Curve p50 / p95 | Cached-image p50 / p95 | Cached-image p95, 480-DIP traversal |
| --- | ---: | ---: | ---: |
| Camera pan, zoom 1 | 30.902 / 34.550 | 1.236 / 1.796 | 1.936 |
| Element drag, zoom 1 | 30.878 / 36.916 | 1.229 / 1.384 | 1.525 |
| Camera pan, zoom 0.35 | 14.501 / 24.300 | 0.931 / 1.638 | 2.757 |
| Element drag, zoom 0.35 | 15.066 / 26.227 | 0.936 / 1.067 | 3.051 |
| 50% alpha, pan, zoom 0.35 | Not measured | 0.568 / 0.698 | 3.400 |
| 50% alpha, drag, zoom 0.35 | Not measured | 0.597 / 0.765 | 3.455 |

Alt-resize has a separate live-reflow case. Changing width must still run Qt's
text layout so line breaks follow the pointer immediately, but the border no
longer destroys and recreates its scene-graph subtree. Chunks first retain the
same glyph sequence and otherwise recycle a node with the same quad count;
relative quad rectangles are updated in place. Across the 24-step regression
sequence, this reduced newly allocated chunks from 17 to 2, with no glyph-mask
generation or texture upload. The corresponding thick-border Debug benchmark
measured 11.48 ms p95 at DPR 1 and 12.47 ms p95 at DPR 2 on the shared test
machine; timings are reported as diagnostics, while the recycling limit is a
deterministic assertion.

The final run passed all 22 motion scenarios (960 timed moving/resize frames).
Large traversals refreshed placements five times per scenario, with zero new glyph
rasterizations or texture uploads. The maximum measured translucent traversal
frame was 4.112 ms. Small motions used only inherited scene-graph transforms.

The motion regression ceiling is 50 ms at p95, intentionally looser than
observed results for variable CI hardware; structural assertions independently
require two triangles per glyph and no rasterization of cached glyphs. These
are small samples on a shared desktop, not an isolated hardware certification
or a guarantee of complete application frame rate on every scene/backend.

`tst_TextItemQml` loads the real production QML component and checks all nine
horizontal/vertical alignments, border safety insets and uniform translucent
alpha. It compares the same alpha mask on a 600-pixel item and at the visible
tail of a 100,000-pixel item, checking both texture bounds and pixel placement.
That comparison also covers fractional panning.
CTest also runs it with doubled UI scaling. The live backend path was
measured separately on the real `TextMediaItem`: at 10,000 characters, canonical
text update plus fit went from about 27 ms to 2.7 ms per edit. The deferred
legacy-document update remains a one-time cost at commit.

The old `tst_TextGlyphPath reportLongTextRebuildCost` target records the SVG
baseline independently. The architecture boundary checks and the shared local/
remote delegate gate remain applicable.

The final full Debug application build and all 13 CTest suites passed (25
outline checks, 22 motion scenarios, both production-QML scaling suites, the
legacy reference and runtime-context tests). Architecture, baseline, render schema,
interaction parity/ownership, integration and randomized-input gates also passed.

Validation was performed on macOS with Metal and Qt 6.11.2. Other RHI backends
and the software scene graph have not been exercised here. Bitmap-only color
emoji fonts have no vector contours and therefore receive no path-based border.

## Primary sources

- [Qt Shape processing and asynchronous behavior](https://doc.qt.io/qt-6/qml-qtquick-shapes-shape.html)
- [Qt Quick text render types](https://doc.qt.io/qt-6/qml-qtquick-textedit.html#renderType-prop)
- [Qt image-node factory and atlas texture creation](https://doc.qt.io/qt-6/qquickwindow.html#createTextureFromImage)
- [Qt image nodes](https://doc.qt.io/qt-6/qsgimagenode.html)
- [Qt scene-graph batching](https://doc.qt.io/qt-6/qtquick-visualcanvas-scenegraph-renderer.html)
- [Qt render-thread atlas allocation and fallback](https://github.com/qt/qtdeclarative/blob/v6.11.2/src/quick/scenegraph/qsgdefaultrendercontext.cpp)
- [Public QSGTextNode API](https://doc.qt.io/qt-6/qsgtextnode.html)
- [Qt's fixed-width curve text outline implementation](https://github.com/qt/qtdeclarative/blob/v6.11.2/src/quick/scenegraph/qsgcurveglyphnode.cpp)
- [Qt's curve glyph processing](https://github.com/qt/qtdeclarative/blob/v6.11.2/src/quick/scenegraph/qsgcurveglyphatlas.cpp)
- [Qt's curve stroke nodes and expansion behavior](https://github.com/qt/qtdeclarative/blob/v6.11.2/src/quick/scenegraph/qsgcurvestrokenode.cpp)
- [Qt transform-node dirty behavior](https://github.com/qt/qtdeclarative/blob/v6.11.2/src/quick/scenegraph/coreapi/qsgnode.cpp)
- [Qt item viewport clipping](https://doc.qt.io/qt-6/qquickitem.html#clipRect)
- [Qt offscreen rendering control](https://doc.qt.io/qt-6/qquickrendercontrol.html)
- [Qt fractional ShaderEffectSource rounding fix](https://github.com/qt/qtdeclarative/commit/97beec0b03a23fa8c1c5416ebe651c8ce86fbaec)
- [QTextLayout and QTextLine glyph retrieval](https://doc.qt.io/qt-6/qtextlayout.html)
- [PathText](https://doc.qt.io/qt-6/qml-qtquick-pathtext.html)
- [QQuickPaintedItem rendering targets](https://doc.qt.io/qt-6/qquickpainteditem.html)
- [MSDF generator](https://github.com/Chlumsky/msdfgen)
