#include "frontend/rendering/canvas/TextGlyphPath.h"

#include <cstdio>

#include <QFont>
#include <QFontMetricsF>
#include <QGlyphRun>
#include <QPainterPath>
#include <QPainterPathStroker>
#include <QRawFont>
#include <QString>
#include <QTextLayout>
#include <QTextOption>

// Forward declarations for file-local helpers defined after recompute().
static QPainterPath extractLargestSubpath(const QPainterPath& path);
static QVector<float> compileGlyphElements(const QPainterPath& path);
static void appendGlyphSvgWithOffset(const QVector<float>& elems,
                                     qreal tx, qreal ty, QByteArray& out);

// ---------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------

TextGlyphPath::TextGlyphPath(QQuickItem* parent)
    : QQuickItem(parent)
{
    // This item is purely a computation engine — it has no visual content,
    // no size, and does not participate in layout.
    // ItemHasContents is NOT set (it is off by default), which tells the
    // scene graph there is nothing to paint — the correct way to declare
    // a non-visual QQuickItem in Qt 6.

    // Each setter emits inputChanged(). Connecting it to polish() requests a
    // polish event for this item.  The Qt scene-graph guarantees that
    // updatePolish() fires once per frame, after ALL QML bindings have
    // evaluated but before tessellation — so multiple setters firing in one
    // frame collapse to exactly one recompute at the right time.
    //
    // If polish() is called before the item has a QQuickWindow (e.g. during
    // initial property setup), Qt sets the polishScheduled flag and
    // automatically adds the item to the window's polish list as soon as it
    // enters the scene — so the initial computation is also handled correctly.
    connect(this, &TextGlyphPath::inputChanged,
            this, [this]() { polish(); });
}

void TextGlyphPath::clearGlyphCaches()
{
    m_glyphPathCache.clear();
    m_strokeElemCache.clear();
    m_cachedGlyphRuns.clear();
    m_layoutCacheKey.clear();
    m_cachedOutlinePixels = -1.0;
}

void TextGlyphPath::clearStrokeCache()
{
    m_strokeElemCache.clear();
    m_cachedOutlinePixels = -1.0;
}

// ---------------------------------------------------------------------------
// Core computation  (polish-phase callback — same frame as property changes)
// ---------------------------------------------------------------------------

void TextGlyphPath::updatePolish()
{
    // ── 1. Font ──────────────────────────────────────────────────────────────
    QFont font;
    font.setFamily(m_fontFamily);
    font.setPixelSize(qMax(1, m_fontPixelSize));
    font.setWeight(QFont::Weight(m_fontWeight));
    font.setItalic(m_fontItalic);
    font.setKerning(true);
    font.setHintingPreference(QFont::PreferNoHinting);

    const QString text = m_fontUppercase ? m_textContent.toUpper() : m_textContent;

    // ── 2. Layout positions (cached by text+font+layout inputs) ─────────────
    // Glyph positions are independent of outlinePixels.  When the user drags
    // the border-width slider, only m_outlinePixels changes — there is no need
    // to run QTextLayout again.  The layout cache key includes all inputs that
    // can affect glyph positions: text content, font, wrapping width, alignment.
    {
        const QString layoutKey = text
            + QLatin1Char('\0') + m_fontFamily
            + QLatin1Char('\0') + QString::number(m_fontPixelSize)
            + QLatin1Char('\0') + QString::number(m_fontWeight)
            + QLatin1Char('\0') + (m_fontItalic ? QLatin1Char('1') : QLatin1Char('0'))
            + QLatin1Char('\0') + QString::number(m_itemWidth, 'f', 1)
            + QLatin1Char('\0') + m_horizontalAlignment
            + QLatin1Char('\0') + (m_fitToText ? QLatin1Char('1') : QLatin1Char('0'));

        if (layoutKey != m_layoutCacheKey) {
            // QTextLayout is a SINGLE-PARAGRAPH engine. '\n' (U+000A) is NOT treated
            // as a hard break by createLine() — it is simply ignored, causing all
            // content to appear on one line. QML Text splits on '\n' and stacks one
            // QTextLayout per paragraph. We must mirror that here exactly.
            QString normalizedText = text;
            normalizedText.replace(QLatin1String("\r\n"), QLatin1String("\n"));
            const QStringList paragraphs = normalizedText.split(QLatin1Char('\n'));

            QTextOption textOption;
            textOption.setWrapMode(m_fitToText ? QTextOption::NoWrap : QTextOption::WordWrap);
            Qt::Alignment hAlign = Qt::AlignHCenter;
            if (m_horizontalAlignment == QLatin1String("left"))       hAlign = Qt::AlignLeft;
            else if (m_horizontalAlignment == QLatin1String("right")) hAlign = Qt::AlignRight;
            textOption.setAlignment(hAlign);
            // Qt Quick internally sets UseDesignMetrics on every QTextLayout it creates
            // (both QQuickText and QQuickTextEdit).  Without this flag QTextLayout rounds
            // each glyph's advance width to the nearest pixel before placing the next
            // glyph.  With the flag, fractional sub-pixel advances are used — matching
            // exactly what TextEdit shows.  Without it the accumulated rounding error
            // over a word produces visually different inter-character spacing between
            // display mode (our glyph paths) and edit mode (TextEdit).
            textOption.setUseDesignMetrics(true);

            const qreal availWidth = qMax(1.0, m_itemWidth);
            const QFontMetricsF fm(font);
            // QTextDocumentLayout (used by TextEdit) calls line.setLeadingIncluded(true)
            // on every line and then advances Y by qCeil(ascent+descent+leading) per line
            // (see getLineHeightParams() in qtextdocumentlayout.cpp).  The qCeil() snaps
            // each line height to an integer pixel boundary, accumulating cleanly.
            // We must mirror this exactly — NOT add leading separately after each
            // non-first line — otherwise the per-line Y positions diverge progressively.
            const qreal emptyLineHeight = qCeil(fm.ascent() + fm.descent() + fm.leading());

            m_cachedGlyphRuns.clear();
            qreal totalHeight = 0.0;
            for (const QString& para : paragraphs) {
                if (para.isEmpty()) {
                    totalHeight += emptyLineHeight;
                    continue;
                }
                QTextLayout layout(para, font);
                layout.setTextOption(textOption);
                layout.beginLayout();
                qreal lineY = 0.0;
                while (true) {
                    QTextLine line = layout.createLine();
                    if (!line.isValid()) break;
                    line.setLineWidth(availWidth);
                    line.setPosition(QPointF(0.0, lineY));
                    // Mirror QTextDocumentLayout exactly: advance by qCeil(ascent+descent+leading)
                    // so that each line snaps to an integer pixel boundary, matching the
                    // integer-ceiled rawHeight used by getLineHeightParams() internally.
                    lineY += qCeil(line.ascent() + line.descent() + line.leading());
                }
                layout.endLayout();
                for (const QGlyphRun& run : layout.glyphRuns()) {
                    const QRawFont rf    = run.rawFont();
                    const QString keyPfx = rf.familyName()
                        + QLatin1Char('|') + rf.styleName()
                        + QLatin1Char('|') + QString::number(rf.pixelSize(), 'f', 3)
                        + QLatin1Char('|');
                    CachedGlyphRun cgr;
                    cgr.rawFont       = rf;
                    cgr.ids           = run.glyphIndexes();
                    cgr.positions     = run.positions();
                    cgr.runPrefixHash = static_cast<quint32>(qHash(keyPfx) & 0xFFFFFFFFu);
                    cgr.startY        = totalHeight;
                    m_cachedGlyphRuns.append(std::move(cgr));
                }
                totalHeight += (lineY > 0.0 ? lineY : emptyLineHeight);
            }
            m_layoutCacheKey = layoutKey;
        }
    }

    // ── 3. Extract glyph paths using per-unique-glyph cache ──────────────────
    // textStrokeShape in QML applies `transform: Translate { y: textDisplayNode.topPadding }`
    // so vertical centering is a synchronous QML binding — zero offset is correct here.
    // m_glyphPathCache  : raw glyph shape at origin — invalidated on font change.
    // m_strokeElemCache : compact flat float array per unique glyph — invalidated
    //                     on font or outlinePixels change.
    // m_cachedGlyphRuns : flattened glyph positions from step 2 — reused on cache hit.
    //
    // Per-instance cost in the hot loop below: one quint64 hash-lookup + one call
    // to appendGlyphSvgWithOffset() which iterates the pre-compiled float array
    // and writes coordinates (via snprintf to a stack buffer) into a QByteArray.
    // No QPainterPath copies, no QString allocations per glyph.
    const bool needsStroke = (m_outlinePixels > 0.0);
    // Pre-reserve the output byte array to avoid repeated reallocations.
    // QByteArray is cheaper than QString for pure-ASCII SVG data; it is converted
    // to QString::fromLatin1 (a memcopy) once at the end.
    QByteArray svgOut;
    if (needsStroke)
        svgOut.reserve(qMax(0, m_textContent.length()) * 300);

    for (const CachedGlyphRun& cgr : m_cachedGlyphRuns) {
        // runPrefixHash was computed once at layout-cache populate time.
        // Combining it with the 32-bit glyph ID gives a collision-resistant
        // quint64 key for the path caches — no per-glyph QString construction.
        const quint32 runHash       = cgr.runPrefixHash;
        const QList<quint32>& ids   = cgr.ids;
        const QList<QPointF>& poses = cgr.positions;
        const int count = qMin(ids.size(), poses.size());
        for (int i = 0; i < count; ++i) {
            const quint32 id       = ids[i];
            const quint64 cacheKey = (quint64(runHash) << 32) | quint64(id);

            // ── Glyph cache lookup / populate on miss ─────────────────────────
            // cachedGlyph is needed for stroke expansion below.
            auto fillIt = m_glyphPathCache.find(cacheKey);
            if (fillIt == m_glyphPathCache.end())
                fillIt = m_glyphPathCache.insert(cacheKey, cgr.rawFont.pathForGlyph(id));

            const QPainterPath& cachedGlyph = fillIt.value();
            if (cachedGlyph.isEmpty())
                continue;

            // poses[i] is relative to the paragraph's layout origin;
            // cgr.startY stacks paragraphs.  Vertical alignment is applied
            // by the QML Translate on textStrokeShape (not baked here).
            const qreal tx = poses[i].x();
            const qreal ty = poses[i].y() + cgr.startY;

            // ── Stroke glyph (cache lookup / populate on miss) ────────────────
            if (needsStroke) {
                auto strokeIt = m_strokeElemCache.find(cacheKey);
                if (strokeIt == m_strokeElemCache.end()) {
                    QPainterPathStroker stroker;
                    stroker.setWidth(m_outlinePixels * 2.0);
                    // RoundJoin prevents miter spikes on sharp convex vertices
                    // (the apex of A, V, W, M, N, etc.). RoundCap is consistent;
                    // it has no geometric effect on closed glyph contours.
                    stroker.setJoinStyle(Qt::RoundJoin);
                    stroker.setCapStyle(Qt::RoundCap);
                    // Belt-and-suspenders: explicit miter limit guards against
                    // accidental spikes if join style is ever changed.
                    stroker.setMiterLimit(1.5);
                    // Stroke only the outer silhouette contour.
                    //
                    // Font glyph paths for counter letters (A, B, D, O, P, Q,
                    // R, e, g, …) contain one subpath per counter hole in addition
                    // to the main outer silhouette.  Stroking ALL subpaths with
                    // QPainterPathStroker creates a stroke ring around every
                    // counter too — including an inward expansion that pushes
                    // stroke geometry INTO the counter hole region.  Because the
                    // fill TextEdit renders on top (z:1) and the stroke Shape is
                    // below (z:0), any geometry inside the counter hole is visible
                    // through the transparent counter and appears as the artefact
                    // the user sees.
                    //
                    // extractLargestSubpath() keeps only the outer silhouette
                    // (the subpath with the largest absolute area).  Stroking
                    // that single contour expands purely outward and never touches
                    // the counter hole interior.
                    QPainterPath outerContour = extractLargestSubpath(cachedGlyph);
                    QPainterPath expanded = stroker.createStroke(outerContour);
                    //
                    // Store the stroke ring directly WITHOUT calling united().
                    //
                    // united() was previously used to merge the expanded ring with
                    // the original glyph fill, but it is not needed: the inner
                    // wall of the stroke ring (which contracts into the glyph body)
                    // is covered by the fill TextEdit on top (z:1) and is never
                    // visible.  Skipping united() has two important benefits:
                    //
                    //  1. Speed — removes the O(n²) QPathClipper boolean operation
                    //             per unique glyph.
                    //
                    //  2. No overlap holes — united() produced a per-glyph path
                    //     with OddEvenFill topology.  When adjacent-glyph stroke
                    //     blobs were concatenated via addPath() and the QML Shape
                    //     applied OddEvenFill, overlapping border regions had
                    //     winding count 2 (even) → rendered as transparent holes.
                    //     Without united(), the cached shape is a clean annular
                    //     stroke ring; with ShapePath.WindingFill in QML, every
                    //     overlap region adds winding values (≥1 = non-zero =
                    //     filled), so no holes ever appear.
                    strokeIt = m_strokeElemCache.insert(cacheKey, compileGlyphElements(expanded));
                }
                appendGlyphSvgWithOffset(strokeIt.value(), tx, ty, svgOut);
            }
        }
    }

    // ── 4. SVG output already assembled inline ───────────────────────────────
    // svgOut (QByteArray, pure ASCII) was built directly during the glyph loop;
    // convert to QString via fromLatin1 — a single memcopy, no per-char work.
    const QString newStroke = svgOut.isEmpty() ? QString() : QString::fromLatin1(svgOut);
    if (newStroke != m_strokePath) {
        m_strokePath = newStroke;
        emit pathsChanged();
    }
}

// ---------------------------------------------------------------------------
// Extracts the subpath with the largest absolute area from a multi-contour
// glyph path.
//
// Font paths for letters with counter holes (A, B, D, O, P, Q, R, e, g, …)
// contain one closed subpath per counter in addition to the outer silhouette.
// Only the outer silhouette should be widened when building an outline stroke —
// widening counter subpaths inward creates geometry inside the transparent
// counter hole that bleeds through the fill text and produces the artefact.
//
// Area is approximated via the shoelace formula over every element's coordinate
// (treating Bézier handles as polygon vertices).  The approximation is coarse
// but always sufficient to distinguish a large outer silhouette (hundreds of
// sq-px) from small inner counters (tens of sq-px).
// ---------------------------------------------------------------------------
static QPainterPath extractLargestSubpath(const QPainterPath& path)
{
    const int total = path.elementCount();
    if (total == 0)
        return path;

    // Locate the start index of each subpath (each MoveToElement opens one).
    QVector<int> starts;
    for (int i = 0; i < total; ++i) {
        if (path.elementAt(i).type == QPainterPath::MoveToElement)
            starts.append(i);
    }
    if (starts.size() <= 1)
        return path; // single contour — nothing to choose between

    // Compute approximate signed area for each subpath via shoelace formula.
    int   bestIdx  = 0;
    qreal bestArea = 0.0;
    for (int si = 0; si < starts.size(); ++si) {
        const int from = starts[si];
        const int to   = (si + 1 < starts.size()) ? starts[si + 1] - 1 : total - 1;
        qreal area = 0.0;
        qreal px = path.elementAt(from).x;
        qreal py = path.elementAt(from).y;
        for (int ei = from + 1; ei <= to; ++ei) {
            const qreal cx = path.elementAt(ei).x;
            const qreal cy = path.elementAt(ei).y;
            area += px * cy - cx * py;
            px = cx;
            py = cy;
        }
        area = qAbs(area) * 0.5;
        if (area > bestArea) {
            bestArea = area;
            bestIdx  = si;
        }
    }

    // Reconstruct the winning subpath as a new QPainterPath.
    const int from = starts[bestIdx];
    const int to   = (bestIdx + 1 < starts.size()) ? starts[bestIdx + 1] - 1 : total - 1;
    QPainterPath result;
    result.setFillRule(path.fillRule());
    for (int ei = from; ei <= to; ) {
        const QPainterPath::Element& el = path.elementAt(ei);
        switch (el.type) {
        case QPainterPath::MoveToElement:
            result.moveTo(el.x, el.y);
            ++ei;
            break;
        case QPainterPath::LineToElement:
            result.lineTo(el.x, el.y);
            ++ei;
            break;
        case QPainterPath::CurveToElement: {
            const QPainterPath::Element& cp2 = path.elementAt(ei + 1);
            const QPainterPath::Element& ep  = path.elementAt(ei + 2);
            result.cubicTo(el.x, el.y, cp2.x, cp2.y, ep.x, ep.y);
            ei += 3;
            break;
        }
        default: // CurveToDataElement — already consumed by CurveToElement
            ++ei;
            break;
        }
    }
    result.closeSubpath();
    return result;
}

// ---------------------------------------------------------------------------
// Compile a QPainterPath into a compact flat float array for fast per-instance
// SVG serialisation.
//
// Format (read sequentially):
//   MoveTo  : tag 1.0f + 2 coords  (x, y)
//   LineTo  : tag 2.0f + 2 coords  (x, y)
//   CurveTo : tag 3.0f + 6 coords  (cp1x, cp1y, cp2x, cp2y, ex, ey)
// Subpaths are implicitly closed: appendGlyphSvgWithOffset() writes a 'Z'
// before each MoveTo (if a subpath is open) and after the last element.
// ---------------------------------------------------------------------------
static QVector<float> compileGlyphElements(const QPainterPath& path)
{
    const int n = path.elementCount();
    QVector<float> elems;
    // Conservative reserve: each element needs at most 7 floats (CurveTo).
    elems.reserve(n * 4);
    for (int i = 0; i < n; ) {
        const QPainterPath::Element& el = path.elementAt(i);
        switch (el.type) {
        case QPainterPath::MoveToElement:
            elems.append(1.0f);
            elems.append(static_cast<float>(el.x));
            elems.append(static_cast<float>(el.y));
            ++i;
            break;
        case QPainterPath::LineToElement:
            elems.append(2.0f);
            elems.append(static_cast<float>(el.x));
            elems.append(static_cast<float>(el.y));
            ++i;
            break;
        case QPainterPath::CurveToElement: {
            // Qt emits CurveToElement (cp1) followed by two CurveToDataElements
            // (cp2 and endpoint).  Consume all three here.
            const QPainterPath::Element& cp2 = path.elementAt(i + 1);
            const QPainterPath::Element& ep  = path.elementAt(i + 2);
            elems.append(3.0f);
            elems.append(static_cast<float>(el.x));
            elems.append(static_cast<float>(el.y));
            elems.append(static_cast<float>(cp2.x));
            elems.append(static_cast<float>(cp2.y));
            elems.append(static_cast<float>(ep.x));
            elems.append(static_cast<float>(ep.y));
            i += 3;
            break;
        }
        default: // CurveToDataElement — already consumed above
            ++i;
            break;
        }
    }
    return elems;
}

// ---------------------------------------------------------------------------
// Append one glyph instance's translated SVG path data directly to `out`.
//
// For each element in the pre-compiled float array, coordinates are output as
// (stored_coord + tx) or (stored_coord + ty) without creating any intermediate
// QPainterPath or string copy.  This is called once per character instance;
// the element array is shared across all instances of the same glyph.
// ---------------------------------------------------------------------------
static void appendGlyphSvgWithOffset(const QVector<float>& elems,
                                     qreal tx, qreal ty, QByteArray& out)
{
    // Write SVG path data using snprintf into a 32-byte stack buffer, then
    // append the stack buffer to `out`.  This avoids the per-number QString
    // heap allocation that QString::number() creates, and the per-character
    // UTF-16 overhead of QString::operator+=.  All SVG path data is pure
    // ASCII, so QByteArray is the correct container.
    const float* d   = elems.constData();
    const float* end = d + elems.size();
    char buf[32];
    int  n;
    bool hasOpen = false;
    while (d < end) {
        const float tag = *d++;
        if (tag == 1.0f) {                              // MoveTo
            if (hasOpen) out.append("Z ", 2);
            n = snprintf(buf, sizeof(buf), "M%.3f ", static_cast<double>(*d++) + tx);
            out.append(buf, n);
            n = snprintf(buf, sizeof(buf), "%.3f ",  static_cast<double>(*d++) + ty);
            out.append(buf, n);
            hasOpen = true;
        } else if (tag == 2.0f) {                       // LineTo
            n = snprintf(buf, sizeof(buf), "L%.3f ", static_cast<double>(*d++) + tx);
            out.append(buf, n);
            n = snprintf(buf, sizeof(buf), "%.3f ",  static_cast<double>(*d++) + ty);
            out.append(buf, n);
        } else if (tag == 3.0f) {                       // CurveTo (cp1x cp1y cp2x cp2y ex ey)
            out.append('C');
            for (int k = 0; k < 3; ++k) {
                n = snprintf(buf, sizeof(buf), "%.3f ", static_cast<double>(*d++) + tx);
                out.append(buf, n);
                n = snprintf(buf, sizeof(buf), "%.3f ", static_cast<double>(*d++) + ty);
                out.append(buf, n);
            }
        }
    }
    if (hasOpen)
        out.append("Z ", 2);
}

// ---------------------------------------------------------------------------
// Setters
// ---------------------------------------------------------------------------
// Font-shape setters: clear both caches because glyph paths change entirely.
// Outline setter: clear only the stroke cache (fill paths are unaffected).
// All other setters: no cache clearing needed (layout/position changes only).

#define SETTER_IMPL(Type, name, member)       \
void TextGlyphPath::set##name(Type v) {       \
    if (m_##member == v) return;              \
    m_##member = v;                           \
    emit inputChanged();                      \
}

// ── Font setters (glyph shapes change — invalidate both caches) ──────────────
void TextGlyphPath::setFontFamily(const QString& v)
{
    if (m_fontFamily == v) return;
    m_fontFamily = v;
    clearGlyphCaches();
    emit inputChanged();
}

void TextGlyphPath::setFontPixelSize(int v)
{
    if (m_fontPixelSize == v) return;
    m_fontPixelSize = v;
    clearGlyphCaches();
    emit inputChanged();
}

void TextGlyphPath::setFontWeight(int v)
{
    if (m_fontWeight == v) return;
    m_fontWeight = v;
    clearGlyphCaches();
    emit inputChanged();
}

void TextGlyphPath::setFontItalic(bool v)
{
    if (m_fontItalic == v) return;
    m_fontItalic = v;
    clearGlyphCaches();
    emit inputChanged();
}

// ── Outline setter (fill shapes unchanged — invalidate stroke cache only) ────
void TextGlyphPath::setOutlinePixels(qreal v)
{
    if (m_outlinePixels == v) return;
    m_outlinePixels = v;
    clearStrokeCache();
    emit inputChanged();
}

// ── Remaining setters (layout/position changes, no cache impact) ─────────────
SETTER_IMPL(const QString&, TextContent,          textContent)
SETTER_IMPL(bool,           FontUppercase,         fontUppercase)
SETTER_IMPL(qreal,          ItemWidth,             itemWidth)
SETTER_IMPL(const QString&, HorizontalAlignment,   horizontalAlignment)
SETTER_IMPL(bool,           FitToText,             fitToText)

#undef SETTER_IMPL
