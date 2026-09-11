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

Qt's own `QSGCurveProcessor` and `QSGCurveStrokeNode` render the outlines with
the GPU curve renderer used by Qt Quick. Immutable geometry is cached by the
actual `QRawFont`, glyph and current outline radius. Small groups of glyphs
retain their scene-graph nodes. Common prefixes and suffixes preserve group
boundaries when inserting into the start or middle of a long line; changing
alignment uses transforms. Small adjacent fragments are coalesced, so repeated
insertions do not accumulate one scene-graph node per keystroke. Color changes
update materials. Qt's `ItemObservesViewport` and `clipRect()` exclude off-screen
blocks and glyphs; ancestor pan/zoom and window resizing refresh the visible
region. Radius changes rebuild the per-glyph cache at the exact
requested width; no undocumented stroke-expansion environment flag is needed.

Retaining nodes alone was not enough. Three further issues were measured:

- `QSGTransformNode::setMatrix()` marks its subtree dirty even for an identical
  matrix. The adapter now compares before setting it.
- A multihash returned identical chunks in reverse occurrence order, swapping
  their nodes and moving otherwise unchanged geometry. Candidates now preserve
  occurrence order.
- Qt merged all chunks into one approximately 137 MB vertex buffer for the
  12,000-character stress case. A one-glyph change invalidated that entire batch.
  A small material subclass keeps chunks in separate batches while using Qt's
  unchanged stroke shader. This bounds geometry uploads to changed chunks.

No SVG is constructed or parsed in the live path, and the pre-generated Impact
atlas is no longer bundled or loaded. `TextGlyphPath` remains available only as
a reference test/benchmark target, not as a registered QML or application type.

The QML editor uses matching border insets. The canonical backend text and fit
geometry remain live, while synchronization of the hidden legacy editor is
deferred to commit. Returning to the legacy editor also restores synchronization.
Translucent borders use a viewport-sized `ShaderEffectSource`, applying alpha
once instead of accumulating it at overlapping strokes. Its destination matches
the source crop exactly; simply changing `layer.sourceRect` would stretch the
crop across the whole document. Texture resolution follows screen scale/DPR and
is capped at 4096 pixels per axis, so a huge or zoomed-out document does not
allocate a document-sized texture. Fully transparent borders skip geometry
generation; opaque borders do not pay for this offscreen pass.
Source and destination crops share integer-aligned local bounds, avoiding
Qt 6.11's fractional `ShaderEffectSource` target rounding (QTBUG-149373).

The private Qt interface is confined to one renderer translation unit. It needs
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
| Qt GPU curve adapter | Reuses Qt's shaping and curve rasterization, keeps editable text native, supports the required widths and removes the measured SVG bottleneck. Selected and covered by the rendering tests below. |

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

Release results on this machine, 12,000 characters, 48 px Impact text, 48 px
outline radius (100%), native fill visible in the production cases:

| Case | First edit | Steady p95 | Insert at start / middle |
| --- | ---: | ---: | ---: |
| No-wrap line, viewport on editing tail | 11.61 ms | 13.05 ms | 10.19 / 10.64 ms |
| Wrapped paragraph, viewport on editing tail | 21.79 ms | 19.79 ms | 20.80 / 20.89 ms |
| Artificial all-glyphs stress, culling disabled | 104.16 ms | 62.02 ms | 56.73 / 50.88 ms |

These are renderer-harness measurements, not a claim about complete application
input latency. They include the native editor's fill in the production cases,
but not the backend model update. The all-glyph case still has a substantial
GPU cost; viewport culling does not make genuinely visible thousands of glyphs
free. Normal production cases have a 50 ms regression ceiling, intentionally
looser than the observed values to allow for test-machine variability.
These are small samples on a shared desktop, not an isolated hardware
certification: earlier Debug runs transiently exceeded the production ceiling
(63 ms) and the artificial stress ceiling (314 ms). The final Debug CTest run
passed all five suites; the final Release production benchmark remained below
20 ms at p95. Structural assertions also verify bounded chunks, stable node
transforms and no regeneration of cached glyph geometry, independently of timing.

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

Validation was performed on macOS with Metal and Qt 6.11.2. Other RHI backends
have not been exercised here. This adapter does not implement Qt Quick's
software scene-graph backend, nor contours for bitmap-only color emoji fonts.

## Primary sources

- [Qt Shape processing and asynchronous behavior](https://doc.qt.io/qt-6/qml-qtquick-shapes-shape.html)
- [Qt Quick text render types](https://doc.qt.io/qt-6/qml-qtquick-textedit.html#renderType-prop)
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
