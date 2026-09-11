#include "frontend/rendering/canvas/TextGlyphPath.h"
#include "frontend/rendering/canvas/GlyphBorderAtlas.h"

#include <cstdint>
#include <cstdio>
#include <cmath>
#include <utility>

#include <QFont>
#include <QFontMetricsF>
#include <QGlyphRun>
#include <QPainterPath>
#include <QPainterPathStroker>
#include <QRawFont>
#include <QString>
#include <QTextLayout>
#include <QTextOption>

// Forward declaration for file-local helper defined after updatePolish().
static QPainterPath extractLargestSubpath(const QPainterPath& path);

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
    m_strokeSvgCache.clear();  // font shape changed — all cached strokes invalid
    m_cachedGlyphRuns.clear();
    m_layoutInputs = LayoutCacheInputs{};  // fontPixelSize = -1  forces rebuild
    m_cachedOutlinePixels = -1.0;
    m_cachedVertOffset    = -1.0;
    m_totalHeight         = 0.0;
    m_vertOffset          = 0.0;
    m_glyphRunsHash       = 0;
    m_reflowHash          = 0;
    m_prevReflowHash      = 0;
    m_maxRightEdge             = 0.0;
    m_intrinsicContentWidth    = 0.0;
    m_strokeXOffset            = 0.0;
    m_svgBuiltAtWidth     = 0.0;
    m_svgBuiltHorizontalAlignment.clear();
}

void TextGlyphPath::clearStrokeCache()
{
    // NOTE: m_strokeSvgCache is NOT cleared here.  The cache key now encodes
    // outlinePixels (quantized) and pixelSize (via runPrefixHash), so existing
    // entries remain valid for their respective thickness/size combinations.
    // Clearing the hash on every slider drag was the main source of repeated
    // buildStrokeGlyphSvgFromAtlas() calls; with key-encoded thickness, old
    // entries stay warm and the new key hits or misses naturally.
    m_cachedOutlinePixels = -1.0;
    m_cachedVertOffset    = -1.0;
    m_strokeXOffset       = 0.0;
    m_svgBuiltAtWidth     = 0.0;
    m_svgBuiltHorizontalAlignment.clear();
    // NOTE: m_prevReflowHash is intentionally NOT reset here.  Since
    // strokeCacheWarm is always false after this call (m_cachedOutlinePixels=-1),
    // the reflowStable && strokeCacheWarm fast-exit is never taken anyway.
    // Keeping m_prevReflowHash intact lets the pure x-shift fast-exit work
    // correctly on the NEXT slider drag that doesn't change glyph positions.
    // m_glyphRunsHash / m_reflowHash intentionally unchanged: positions are still valid.
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
    // Glyph positions are independent of outlinePixels.  Fields are compared
    // individually (cheapest first) — no heap allocation on every call.
    //
    // layoutContentChanged is set to true only when QTextLayout produces a
    // result that actually differs from the previous run (detected via a cheap
    // integer hash of all glyph positions).  When it stays false (e.g. a
    // resize that doesn't cause a line-break change), step 3 bails out
    // before calling buildStrokeSvg() at all.
    bool layoutContentChanged = false;
    {
        const bool layoutDirty =
            m_fontPixelSize       != m_layoutInputs.fontPixelSize ||
            m_fontWeight          != m_layoutInputs.fontWeight    ||
            m_fontItalic          != m_layoutInputs.fontItalic    ||
            m_fitToText           != m_layoutInputs.fitToText     ||
            m_itemWidth           != m_layoutInputs.itemWidth     ||
            text                  != m_layoutInputs.text          ||
            m_fontFamily          != m_layoutInputs.fontFamily    ||
            m_horizontalAlignment != m_layoutInputs.hAlign;

        if (layoutDirty) {
            // ── Skip-layout fast path ─────────────────────────────────────────
            // For left-aligned text with no wrap changes, QTextLayout produces
            // identical glyph positions regardless of the exact itemWidth (as
            // long as no line exceeds the available width).  Track the maximum
            // rightmost glyph x across all runs (m_maxRightEdge).  If the new
            // itemWidth ≥ m_maxRightEdge and the ONLY dirty field is itemWidth,
            // skip QTextLayout entirely — positions are guaranteed unchanged.
            const bool onlyWidthDirty =
                (m_itemWidth           != m_layoutInputs.itemWidth)   &&
                (m_fontPixelSize       == m_layoutInputs.fontPixelSize) &&
                (m_fontWeight          == m_layoutInputs.fontWeight)    &&
                (m_fontItalic          == m_layoutInputs.fontItalic)    &&
                (m_fitToText           == m_layoutInputs.fitToText)     &&
                (text                  == m_layoutInputs.text)          &&
                (m_fontFamily          == m_layoutInputs.fontFamily)    &&
                (m_horizontalAlignment == m_layoutInputs.hAlign);

            // Skip QTextLayout entirely when only itemWidth changed and the
            // text still fits: no line-break change is possible for any alignment
            // when m_itemWidth >= m_intrinsicContentWidth (the widest line's
            // natural width, independent of container width and alignment).
            const bool skipLayout = onlyWidthDirty
                && m_itemWidth >= m_intrinsicContentWidth
                && !m_cachedGlyphRuns.isEmpty();

            if (skipLayout) {
                // Width grew but text still fits — update cache key, skip layout.
                m_layoutInputs.itemWidth = m_itemWidth;
                // layoutContentChanged stays false.
            } else {
            // QTextLayout is a SINGLE-PARAGRAPH engine. '\n' (U+000A) is NOT treated
            // as a hard break by createLine() — it is simply ignored, causing all
            // content to appear on one line. QML Text splits on '\n' and stacks one
            // QTextLayout per paragraph. We must mirror that here exactly.
            QString normalizedText = text;
            normalizedText.replace(QLatin1String("\r\n"), QLatin1String("\n"));
            const QStringList paragraphs = normalizedText.split(QLatin1Char('\n'));

            QTextOption textOption;
            textOption.setWrapMode(m_fitToText ? QTextOption::NoWrap : QTextOption::WordWrap);
            textOption.setFlags(textOption.flags() | QTextOption::IncludeTrailingSpaces);
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
            m_intrinsicContentWidth = 0.0;
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
                QVector<QTextLine> laidOutLines;
                while (true) {
                    QTextLine line = layout.createLine();
                    if (!line.isValid()) break;
                    line.setLineWidth(availWidth);
                    line.setPosition(QPointF(0.0, lineY));
                    laidOutLines.append(line);
                    // QML's TextEdit document is configured to include trailing
                    // spaces in its effective line advance. Track the same width
                    // here so the no-reflow fast path cannot accept a too-small box.
                    const QString lineText = para.mid(line.textStart(), line.textLength());
                    const qreal fullAdvance = qMax(line.naturalTextWidth(), fm.horizontalAdvance(lineText));
                    m_intrinsicContentWidth = qMax(m_intrinsicContentWidth, fullAdvance);
                    // Mirror QTextDocumentLayout exactly: advance by qCeil(ascent+descent+leading)
                    // so that each line snaps to an integer pixel boundary, matching the
                    // integer-ceiled rawHeight used by getLineHeightParams() internally.
                    lineY += qCeil(line.ascent() + line.descent() + line.leading());
                }
                layout.endLayout();
                for (const QTextLine& line : std::as_const(laidOutLines)) {
                    const QString lineText = para.mid(line.textStart(), line.textLength());
                    const qreal fullAdvance = qMax(line.naturalTextWidth(), fm.horizontalAdvance(lineText));
                    const qreal trailingAdvance = qMax<qreal>(0.0, fullAdvance - line.naturalTextWidth());
                    qreal trailingAlignmentCorrection = 0.0;
                    if (m_horizontalAlignment == QLatin1String("center")) {
                        trailingAlignmentCorrection = -trailingAdvance * 0.5;
                    } else if (m_horizontalAlignment == QLatin1String("right")) {
                        trailingAlignmentCorrection = -trailingAdvance;
                    }

                    for (const QGlyphRun& run : line.glyphRuns()) {
                        const QRawFont rf    = run.rawFont();
                        const QString keyPfx = rf.familyName()
                            + QLatin1Char('|') + rf.styleName()
                            + QLatin1Char('|') + QString::number(rf.pixelSize(), 'f', 3)
                            + QLatin1Char('|');
                        CachedGlyphRun cgr;
                        cgr.rawFont       = rf;
                        cgr.ids           = run.glyphIndexes();
                        cgr.positions     = run.positions();
                        if (!qFuzzyIsNull(trailingAlignmentCorrection)) {
                            for (QPointF& position : cgr.positions) {
                                position.rx() += trailingAlignmentCorrection;
                            }
                        }
                        cgr.runPrefixHash = static_cast<quint32>(qHash(keyPfx) & 0xFFFFFFFFu);
                        cgr.startY        = totalHeight;
                        m_cachedGlyphRuns.append(std::move(cgr));
                    }
                }
                totalHeight += (lineY > 0.0 ? lineY : emptyLineHeight);
            }

            // ── Position hash: detect no-op reflows ──────────────────────
            // Iterate all rebuilt runs and mix (startY × position × glyphId)
            // into a single quint64.  Collisions are astronomically unlikely
            // and a false-positive just means one unnecessary SVG rebuild
            // (emit is still guarded by string comparison).  A false-negative
            // would skip a valid update — prevented by the multiply-then-XOR
            // mixing (different orderings produce different hashes).
            //
            // m_glyphRunsHash: full hash including X positions.
            // m_reflowHash:    hash of IDs + Y only (no X).  Stable during
            //                  pure alignment x-shifts — used to detect whether
            //                  a full SVG rebuild is actually needed.
            quint64 newRunHash    = quint64(m_cachedGlyphRuns.size());
            quint64 newReflowHash = quint64(m_cachedGlyphRuns.size());
            for (const CachedGlyphRun& cgr : m_cachedGlyphRuns) {
                newRunHash    ^= qHash(cgr.startY, 0x9E3779B9u);
                newReflowHash ^= qHash(cgr.startY, 0x9E3779B9u);
                const int rc = qMin(cgr.ids.size(), cgr.positions.size());
                for (int i = 0; i < rc; ++i) {
                    newRunHash = newRunHash * 6364136223846793005ULL + quint64(cgr.ids[i]);
                    newRunHash = newRunHash * 6364136223846793005ULL
                               + quint64(qRound(cgr.positions[i].x() * 16));
                    newRunHash = newRunHash * 6364136223846793005ULL
                               + quint64(qRound(cgr.positions[i].y() * 16));
                    // Reflow hash: IDs + Y only (X intentionally excluded)
                    newReflowHash = newReflowHash * 6364136223846793005ULL + quint64(cgr.ids[i]);
                    newReflowHash = newReflowHash * 6364136223846793005ULL
                                  + quint64(qRound(cgr.positions[i].y() * 16));
                }
            }
            layoutContentChanged = (newRunHash != m_glyphRunsHash);
            m_glyphRunsHash  = newRunHash;
            m_reflowHash     = newReflowHash;

            // Track rightmost glyph edge so the skip-layout fast path above
            // can bypass QTextLayout on subsequent width-only changes.
            m_maxRightEdge = 0.0;
            for (const CachedGlyphRun& cgr : m_cachedGlyphRuns)
                for (const QPointF& p : cgr.positions)
                    m_maxRightEdge = qMax(m_maxRightEdge, p.x());

            // Update cached inputs so the next call skips the layout step.
            m_layoutInputs.fontPixelSize = m_fontPixelSize;
            m_layoutInputs.fontWeight    = m_fontWeight;
            m_layoutInputs.fontItalic    = m_fontItalic;
            m_layoutInputs.fitToText     = m_fitToText;
            m_layoutInputs.itemWidth     = m_itemWidth;
            m_layoutInputs.text          = text;
            m_layoutInputs.fontFamily    = m_fontFamily;
            m_layoutInputs.hAlign        = m_horizontalAlignment;
            m_totalHeight                = totalHeight;  // save for vertOffset computation
            } // end !skipLayout
        }
    }

    // ── 2b. Compute vertical offset ────────────────────────────────────────
    // Mirrors textDisplayNode.topPadding exactly, but computed here in C++ from
    // the authoritative m_totalHeight so the offset is baked into the SVG
    // y-coordinates.  This eliminates the QML binding timing dependency that
    // caused border drift when verticalAlignment is center or bottom:
    //   topPadding = (height - contentHeight) * 0.5  [center]
    //              = (height - contentHeight)         [bottom]
    //              = 0                                [top]
    {
        const qreal spare = qMax(0.0, m_itemHeight - m_totalHeight);
        if (m_verticalAlignment == QLatin1String("center"))
            m_vertOffset = spare * 0.5;
        else if (m_verticalAlignment == QLatin1String("bottom"))
            m_vertOffset = spare;
        else
            m_vertOffset = 0.0;  // top alignment — no offset needed
    }

    // ── 3. Assemble stroke SVG ────────────────────────────────────────────────
    // m_strokeSvgCache stores pre-built relative SVG per unique glyph shape.
    // Warm-path cost per glyph instance: 4 wf3() calls + one QByteArray memcpy
    // regardless of how many Bézier segments the glyph contains.
    //
    // Decision logic (in order of cheapness):
    //   • needsStroke == false           → clear path, done
    //   • m_reflowHash stable
    //     AND strokeCacheWarm            → pure x-shift (alignment resize),
    //                                      update strokeXOffset only — no SVG rebuild,
    //                                      no Qt Shape retessellation
    //   • otherwise                      → full SVG rebuild (reflow or stroke size change)
    //
    // m_reflowHash is computed from IDs + Y positions only (no X), so it is
    // stable during center/right-aligned resizes that only shift glyphs
    // horizontally without changing line breaks.
    const bool needsStroke     = (m_outlinePixels > 0.0);
    // strokeCacheWarm: SVG is valid for the current outline thickness AND vertical offset.
    // Including m_cachedVertOffset ensures a full SVG rebuild whenever itemHeight or
    // verticalAlignment changes (which alter m_vertOffset and therefore all glyph y coords).
    const bool strokeCacheWarm = (m_cachedOutlinePixels == m_outlinePixels)
                               && qFuzzyCompare(m_cachedVertOffset + 1.0, m_vertOffset + 1.0);

    if (!needsStroke) {
        // Pre-warm tessellation: when the glyph layout just changed (text typed /
        // font changed) while the border is off, silently build the stroke SVG at
        // the last-used thickness and assign it to m_strokePath.  The QML Shape
        // (hidden via opacity:0, not visible:false) still receives the path and
        // tessellates it on the background thread (asynchronous:true).  When the
        // user re-enables the border the geometry is already resident in the GPU,
        // so the border appears without any blank-frame delay.
        //
        // If the path is NOT rebuilt (layoutContentChanged==false, e.g. only
        // outlinePixels toggled to 0), the existing m_strokePath is kept as-is so
        // the Shape retains its tessellated geometry for the instant re-enable case.
        if (layoutContentChanged) {
            const bool canPrewarm = m_prewarmOutlinePixels > 0.0
                                    && !m_cachedGlyphRuns.isEmpty()
                                    && GlyphBorderAtlas::instance().isLoaded();
            if (canPrewarm) {
                // Temporarily impersonate the pre-warm thickness so buildStrokeSvg()
                // builds the correct SVG without altering any persistent cache state.
                const qreal savedOutline       = m_outlinePixels;
                const qreal savedCachedOutline = m_cachedOutlinePixels;
                m_outlinePixels       = m_prewarmOutlinePixels;
                const QString prewarmedPath = QString::fromLatin1(buildStrokeSvg());
                m_outlinePixels       = savedOutline;
                m_cachedOutlinePixels = savedCachedOutline; // restore — don't pollute state
                if (prewarmedPath != m_strokePath) {
                    m_strokePath = prewarmedPath;
                    emit pathsChanged();
                }
            } else {
                // No pre-warm capability (atlas not loaded or never had a border).
                // Clear the path so the Shape has no geometry while border is off.
                if (!m_strokePath.isEmpty()) { m_strokePath.clear(); emit pathsChanged(); }
            }
        }
        // !layoutContentChanged: keep existing m_strokePath intact so the Shape
        // retains its tessellated geometry (instantly re-usable when border re-enabled).
        if (m_strokeXOffset != 0.0) { m_strokeXOffset = 0.0; emit strokeXOffsetChanged(); }
        return;
    }

    // Pure alignment x-shift: reflow hash is stable (same line breaks, same IDs,
    // same Y), but full position hash changed (X shifted due to center/right alignment).
    // Rebuild guard: also requires strokeCacheWarm (same outlinePixels).
    //
    // m_reflowHash == m_prevReflowHash means: IDs, Y positions and line structure
    // are identical to the last full SVG build.  The only valid change is a pure
    // x-shift from alignment, which strokeXOffset handles without retessellation.
    const bool reflowStable = (m_reflowHash == m_prevReflowHash);
    const bool builtAlignmentUnchanged =
        (m_svgBuiltHorizontalAlignment == m_horizontalAlignment);
    if (reflowStable && strokeCacheWarm && builtAlignmentUnchanged) {
        // No reflow, stroke shapes unchanged.  For center/right alignment the
        // text shifted purely in x — update the cheap QML Translate offset so
        // the border follows without any Qt Shape retessellation at all.
        qreal newOffset = 0.0;
        if (m_svgBuiltAtWidth > 0.0) {
            if (m_horizontalAlignment == QLatin1String("center"))
                newOffset = (m_itemWidth - m_svgBuiltAtWidth) * 0.5;
            else if (m_horizontalAlignment == QLatin1String("right"))
                newOffset = m_itemWidth - m_svgBuiltAtWidth;
        }
        if (!qFuzzyCompare(newOffset + 1.0, m_strokeXOffset + 1.0)) {
            m_strokeXOffset = newOffset;
            emit strokeXOffsetChanged();
        }
        return;
    }

    // Reflow or stroke-shape change — rebuild the full SVG.
    // Reset the x-offset: the freshly-built SVG bakes the current glyph positions.
    m_prevReflowHash  = m_reflowHash;
    const QString newStroke = QString::fromLatin1(buildStrokeSvg());
    m_cachedOutlinePixels = m_outlinePixels;
    m_cachedVertOffset    = m_vertOffset;   // mark vert offset as baked into this SVG
    m_svgBuiltAtWidth     = m_itemWidth;
    m_svgBuiltHorizontalAlignment = m_horizontalAlignment;
    if (m_strokeXOffset != 0.0) {
        m_strokeXOffset = 0.0;
        emit strokeXOffsetChanged();
    }
    if (newStroke != m_strokePath) {
        m_strokePath = newStroke;
        emit pathsChanged();
    }
}

// ---------------------------------------------------------------------------
// Fast float formatter: writes v to 3 decimal places + trailing space into
// buf, returns the byte count.  Avoids snprintf's locale handling, format
// string parsing, and syscall overhead, giving ~5× speedup per call.
// buf must be at least 24 bytes (worst case: sign + 10 integer digits +
// '.' + 3 fraction digits + space + NUL = 17 bytes; 24 gives headroom).
// ---------------------------------------------------------------------------
static int wf3(char* buf, double v)
{
    bool neg = (v < 0.0);
    if (neg) v = -v;
    auto iv = static_cast<long long>(v * 1000.0 + 0.5);
    long long ip = iv / 1000LL;
    int  fp = static_cast<int>(iv % 1000LL);
    int  k  = 0;
    if (neg) buf[k++] = '-';
    if (ip == 0) {
        buf[k++] = '0';
    } else {
        char d[14]; int di = 0;
        auto q = ip; while (q > 0) { d[di++] = static_cast<char>('0' + q % 10); q /= 10; }
        while (di > 0) buf[k++] = d[--di];
    }
    buf[k++] = '.';
    buf[k++] = static_cast<char>('0' + fp / 100);
    buf[k++] = static_cast<char>('0' + (fp / 10) % 10);
    buf[k++] = static_cast<char>('0' + fp % 10);
    buf[k++] = ' ';
    buf[k]   = '\0';
    return k;
}

// ---------------------------------------------------------------------------
// Build a StrokeGlyphSvg cache entry from a stroke QPainterPath.
//
// Each subpath's origin is stored as an absolute (ox, oy) float pair.
// All subsequent commands (l/c/z) are encoded as RELATIVE coordinates in
// body.  At render time, only "M(ox+tx) (oy+ty)" is formatted; body is
// appended via a single QByteArray::append() (memcpy).
// ---------------------------------------------------------------------------
static StrokeGlyphSvg buildStrokeGlyphSvg(const QPainterPath& path)
{
    StrokeGlyphSvg result;
    const int n = path.elementCount();
    if (n == 0) return result;

    StrokeSubpath* cur = nullptr;
    float cx = 0.0f, cy = 0.0f;  // current pen position
    char buf[24];

    for (int i = 0; i < n; ) {
        const QPainterPath::Element& el = path.elementAt(i);
        switch (el.type) {
        case QPainterPath::MoveToElement:
            if (cur && !cur->body.isEmpty() && !cur->body.endsWith('z'))
                cur->body.append("z ", 2);
            result.subpaths.emplaceBack();
            cur = &result.subpaths.last();
            cur->ox = static_cast<float>(el.x);
            cur->oy = static_cast<float>(el.y);
            cur->body.reserve(640);
            cx = static_cast<float>(el.x);
            cy = static_cast<float>(el.y);
            ++i;
            break;
        case QPainterPath::LineToElement:
            if (cur) {
                cur->body.append('l');
                cur->body.append(buf, wf3(buf, static_cast<double>(el.x) - cx));
                cur->body.append(buf, wf3(buf, static_cast<double>(el.y) - cy));
                cx = static_cast<float>(el.x);
                cy = static_cast<float>(el.y);
            }
            ++i;
            break;
        case QPainterPath::CurveToElement:
            if (cur && i + 2 < n) {
                const QPainterPath::Element& cp2 = path.elementAt(i + 1);
                const QPainterPath::Element& ep  = path.elementAt(i + 2);
                cur->body.append('c');
                cur->body.append(buf, wf3(buf, static_cast<double>(el.x)  - cx));
                cur->body.append(buf, wf3(buf, static_cast<double>(el.y)  - cy));
                cur->body.append(buf, wf3(buf, static_cast<double>(cp2.x) - cx));
                cur->body.append(buf, wf3(buf, static_cast<double>(cp2.y) - cy));
                cur->body.append(buf, wf3(buf, static_cast<double>(ep.x)  - cx));
                cur->body.append(buf, wf3(buf, static_cast<double>(ep.y)  - cy));
                cx = static_cast<float>(ep.x);
                cy = static_cast<float>(ep.y);
            }
            i += 3;
            break;
        default:
            ++i;
            break;
        }
    }
    if (cur && !cur->body.isEmpty() && !cur->body.endsWith('z'))
        cur->body.append("z ", 2);
    return result;
}

// ---------------------------------------------------------------------------
// Build a StrokeGlyphSvg from pre-baked atlas data.
//
// rawSubpaths contains the scaled-independent float coordinates stored in the
// atlas (refPixelSize space).  Every float is multiplied by |scale| before
// being formatted into the body QByteArray.  The result is structurally
// identical to what buildStrokeGlyphSvg() produces — the hot path in
// buildStrokeSvg() (the memcpy warm path) is completely unchanged.
// ---------------------------------------------------------------------------
static StrokeGlyphSvg buildStrokeGlyphSvgFromAtlas(
    const QVector<AtlasSubpathRaw>& rawSubpaths, double scale)
{
    StrokeGlyphSvg result;
    char buf[24];

    for (const AtlasSubpathRaw& raw : rawSubpaths) {
        StrokeSubpath sp;
        sp.ox = static_cast<float>(raw.ox * scale);
        sp.oy = static_cast<float>(raw.oy * scale);
        sp.body.reserve(raw.commands.size() * 20);

        int argIdx = 0;
        for (uint8_t cmd : raw.commands) {
            switch (cmd) {
            case 0: // LineTo: 2 floats (dx, dy)
                sp.body.append('l');
                sp.body.append(buf, wf3(buf, static_cast<double>(raw.args[argIdx++]) * scale));
                sp.body.append(buf, wf3(buf, static_cast<double>(raw.args[argIdx++]) * scale));
                break;
            case 1: // CurveTo: 6 floats (cp1x, cp1y, cp2x, cp2y, ex, ey)
                sp.body.append('c');
                sp.body.append(buf, wf3(buf, static_cast<double>(raw.args[argIdx++]) * scale));
                sp.body.append(buf, wf3(buf, static_cast<double>(raw.args[argIdx++]) * scale));
                sp.body.append(buf, wf3(buf, static_cast<double>(raw.args[argIdx++]) * scale));
                sp.body.append(buf, wf3(buf, static_cast<double>(raw.args[argIdx++]) * scale));
                sp.body.append(buf, wf3(buf, static_cast<double>(raw.args[argIdx++]) * scale));
                sp.body.append(buf, wf3(buf, static_cast<double>(raw.args[argIdx++]) * scale));
                break;
            case 2: // ClosePath
                sp.body.append("z ", 2);
                break;
            default:
                break;
            }
        }
        if (!sp.body.isEmpty() && !sp.body.endsWith('z'))
            sp.body.append("z ", 2);

        if (!sp.body.isEmpty())
            result.subpaths.emplaceBack(std::move(sp));
    }
    return result;
}

QByteArray TextGlyphPath::buildStrokeSvg()
{
    QByteArray svgOut;
    svgOut.reserve(qMax(0, m_textContent.length()) * 256);

    char buf[24];
    for (const CachedGlyphRun& cgr : m_cachedGlyphRuns) {
        const quint32 runHash       = cgr.runPrefixHash;
        const QList<quint32>& ids   = cgr.ids;
        const QList<QPointF>& poses = cgr.positions;
        const int count = qMin(ids.size(), poses.size());
        // Quantized outline key component: 0.001-pixel precision, matching the
        // SVG coordinate encoder. Encodes the
        // current outlinePixels into the cache key so different thicknesses get
        // independent cache slots.  This eliminates the need to clear the entire
        // m_strokeSvgCache on every outlinePixels change.
        const quint32 quantizedOutline =
            static_cast<quint32>(qRound(m_outlinePixels * 1000.0));

        for (int i = 0; i < count; ++i) {
            const quint32 id        = ids[i];
            const quint64 glyphKey  = (quint64(runHash) << 32) | quint64(id);
            const QPair<quint64,quint32> cacheKey { glyphKey, quantizedOutline };

            auto strokeIt = m_strokeSvgCache.find(cacheKey);
            if (strokeIt == m_strokeSvgCache.end()) {
                // Cold path: try atlas first, fall back to QPainterPathStroker.
                bool builtFromAtlas = false;
                const GlyphBorderAtlas& atlas = GlyphBorderAtlas::instance();
                const bool atlasFontMatches =
                    cgr.rawFont.familyName().compare(QStringLiteral("Impact"), Qt::CaseInsensitive) == 0
                    && !m_fontItalic
                    && m_fontWeight == static_cast<int>(QFont::Normal);
                if (atlas.isLoaded() && atlasFontMatches && m_fontPixelSize > 0) {
                    const double thicknessPct =
                        (static_cast<double>(m_outlinePixels) /
                         static_cast<double>(m_fontPixelSize)) * 100.0;
                    const float snappedPct = atlas.nearestThicknessStep(
                        static_cast<float>(thicknessPct));
                    const double snappedPixels = snappedPct > 0.0f
                        ? (static_cast<double>(snappedPct) / 100.0)
                            * static_cast<double>(m_fontPixelSize)
                        : -1.0;
                    const double scale =
                        static_cast<double>(m_fontPixelSize) / atlas.refPixelSize();
                    QVector<AtlasSubpathRaw> rawSubpaths;
                    // The atlas stores discrete percentages. Never substitute a
                    // visibly different thickness merely to get a cache hit.
                    if (snappedPixels > 0.0
                        && std::abs(snappedPixels - m_outlinePixels) <= 0.05
                        && atlas.lookup(static_cast<uint32_t>(id),
                                     snappedPct,
                                     rawSubpaths)) {
                        strokeIt = m_strokeSvgCache.insert(
                            cacheKey,
                            buildStrokeGlyphSvgFromAtlas(rawSubpaths, scale));
                        builtFromAtlas = true;
                    }
                }
                if (!builtFromAtlas) {
                    // Fallback: QPainterPathStroker (atlas not loaded or glyph missing).
                    // Fill shape key uses only glyphKey (no thickness) — fill is
                    // thickness-independent.
                    const quint64 fillKey = glyphKey;
                    auto fillIt = m_glyphPathCache.find(fillKey);
                    if (fillIt == m_glyphPathCache.end())
                        fillIt = m_glyphPathCache.insert(fillKey,
                                                         cgr.rawFont.pathForGlyph(id));
                    const QPainterPath& cachedGlyph = fillIt.value();
                    if (cachedGlyph.isEmpty())
                        continue;

                    QPainterPathStroker stroker;
                    stroker.setWidth(m_outlinePixels * 2.0);
                    stroker.setJoinStyle(Qt::RoundJoin);
                    stroker.setCapStyle(Qt::RoundCap);
                    stroker.setMiterLimit(1.5);
                    QPainterPath outerContour = extractLargestSubpath(cachedGlyph);
                    QPainterPath expanded     = stroker.createStroke(outerContour);
                    strokeIt = m_strokeSvgCache.insert(cacheKey,
                                                       buildStrokeGlyphSvg(expanded));
                }
            }

            // Warm path: emit "M(ox+tx)(oy+ty)" + memcpy(body) per subpath.
            // For a glyph with 2 stroke subpaths this is 4 wf3() calls total
            // regardless of how many Bezier segments the glyph contains.
            const float tx = static_cast<float>(poses[i].x());
            // m_vertOffset is baked directly into the SVG y-coordinates so the Shape
            // needs no external QML Translate.y compensation.  It mirrors
            // textDisplayNode.topPadding but is computed from the authoritative
            // m_totalHeight, not from QML's contentHeight, eliminating timing desync.
            const float ty = static_cast<float>(poses[i].y() + cgr.startY + m_vertOffset);
            for (const StrokeSubpath& sp : strokeIt.value().subpaths) {
                svgOut.append('M');
                svgOut.append(buf, wf3(buf, static_cast<double>(sp.ox) + tx));
                svgOut.append(buf, wf3(buf, static_cast<double>(sp.oy) + ty));
                svgOut.append(sp.body);
            }
        }
    }
    return svgOut;
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

// ── Outline setter (fill shapes unchanged; stroke SVG cache keyed by thickness
//   so no hash clear needed — clearStrokeCache() resets build state only) ────
void TextGlyphPath::setOutlinePixels(qreal v)
{
    if (m_outlinePixels == v) return;
    if (v > 0.0)
        m_prewarmOutlinePixels = v;  // remember for pre-warm when border is turned off
    m_outlinePixels = v;
    clearStrokeCache();  // resets svgBuiltAtWidth / cachedOutlinePixels state, not the hash
    emit inputChanged();
}

// ── Remaining setters (layout/position changes, no cache impact) ─────────────
SETTER_IMPL(const QString&, TextContent,          textContent)
SETTER_IMPL(bool,           FontUppercase,         fontUppercase)
SETTER_IMPL(qreal,          ItemWidth,             itemWidth)
SETTER_IMPL(const QString&, HorizontalAlignment,   horizontalAlignment)
SETTER_IMPL(bool,           FitToText,             fitToText)
SETTER_IMPL(const QString&, VerticalAlignment,      verticalAlignment)
SETTER_IMPL(qreal,          ItemHeight,             itemHeight)

#undef SETTER_IMPL
