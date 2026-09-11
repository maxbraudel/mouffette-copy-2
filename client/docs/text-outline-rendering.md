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
alignment uses transforms. Color changes update materials. Off-item blocks and
glyphs are excluded. Radius changes rebuild the per-glyph cache at the exact
requested width; no undocumented stroke-expansion environment flag is needed.

No SVG is constructed or parsed in the live path, and the pre-generated Impact
atlas is no longer bundled or loaded. `TextGlyphPath` remains available only as
a reference test/benchmark target, not as a registered QML or application type.

The QML editor uses matching border insets. The canonical backend text and fit
geometry remain live, while synchronization of the hidden legacy editor is
deferred to commit. Returning to the legacy editor also restores synchronization.

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
| Qt GPU curve adapter | Reuses Qt's shaping and curve rasterization, keeps editable text native, supports the required widths and removes the measured SVG bottleneck. Selected subject to the rendering tests below. |

## Verification

`tst_TextOutlineItem` uses real Qt Quick windows and the production Metal backend
on macOS. It compares border pixels against a QPainter reference made from the
same document's glyphs, verifies left/center/right positioning and disconnected
contours, and measures live edits in a 12,000-character text. The reference uses
nonzero winding, so overlapping strokes form a union rather than cancelling.
The long-text case keeps the entire logical line in the outline item's bounds,
preventing viewport culling from making the CPU benchmark artificially small.

The old `tst_TextGlyphPath reportLongTextRebuildCost` target records the SVG
baseline independently. The architecture boundary checks and the shared local/
remote delegate gate remain applicable.

## Primary sources

- [Qt Shape processing and asynchronous behavior](https://doc.qt.io/qt-6/qml-qtquick-shapes-shape.html)
- [Qt Quick text render types](https://doc.qt.io/qt-6/qml-qtquick-textedit.html#renderType-prop)
- [Public QSGTextNode API](https://doc.qt.io/qt-6/qsgtextnode.html)
- [Qt's fixed-width curve text outline implementation](https://github.com/qt/qtdeclarative/blob/v6.11.2/src/quick/scenegraph/qsgcurveglyphnode.cpp)
- [Qt's curve glyph processing](https://github.com/qt/qtdeclarative/blob/v6.11.2/src/quick/scenegraph/qsgcurveglyphatlas.cpp)
- [Qt's curve stroke nodes and expansion behavior](https://github.com/qt/qtdeclarative/blob/v6.11.2/src/quick/scenegraph/qsgcurvestrokenode.cpp)
- [QTextLayout and QTextLine glyph retrieval](https://doc.qt.io/qt-6/qtextlayout.html)
- [PathText](https://doc.qt.io/qt-6/qml-qtquick-pathtext.html)
- [QQuickPaintedItem rendering targets](https://doc.qt.io/qt-6/qquickpainteditem.html)
- [MSDF generator](https://github.com/Chlumsky/msdfgen)
