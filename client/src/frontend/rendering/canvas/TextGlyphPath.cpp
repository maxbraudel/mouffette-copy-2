#include "frontend/rendering/canvas/TextGlyphPath.h"

#include <QFont>
#include <QFontMetricsF>
#include <QGlyphRun>
#include <QPainterPath>
#include <QPainterPathStroker>
#include <QRawFont>
#include <QString>
#include <QTextLayout>
#include <QTextOption>
#include <QTimer>
#include <QTransform>

// Forward declaration for file-local helper defined after recompute().
static QString painterPathToSvg(const QPainterPath& path);

// ---------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------

TextGlyphPath::TextGlyphPath(QObject* parent)
    : QObject(parent)
{
    m_recomputeTimer = new QTimer(this);
    m_recomputeTimer->setSingleShot(true);
    m_recomputeTimer->setInterval(0);  // fire on the next event-loop tick
    connect(m_recomputeTimer, &QTimer::timeout, this, &TextGlyphPath::recompute);
    connect(this, &TextGlyphPath::inputChanged, this, &TextGlyphPath::scheduleRecompute);
}

void TextGlyphPath::scheduleRecompute()
{
    if (!m_recomputeTimer->isActive())
        m_recomputeTimer->start();
}

void TextGlyphPath::clearGlyphCaches()
{
    m_glyphPathCache.clear();
    m_strokeGlyphCache.clear();
    m_cachedOutlinePixels = -1.0;
}

void TextGlyphPath::clearStrokeCache()
{
    m_strokeGlyphCache.clear();
    m_cachedOutlinePixels = -1.0;
}

// ---------------------------------------------------------------------------
// Core computation
// ---------------------------------------------------------------------------

void TextGlyphPath::recompute()
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

    // ── 2. Split text into paragraphs, lay each out separately ───────────────
    // QTextLayout is a SINGLE-PARAGRAPH engine. '\n' (U+000A) is NOT treated as
    // a hard break by createLine() — it is simply ignored, causing all content
    // to appear on one line. QML Text splits on '\n' and stacks one QTextLayout
    // per paragraph. We must mirror that here so line-break behaviour matches.
    QString normalizedText = text;
    normalizedText.replace(QLatin1String("\r\n"), QLatin1String("\n"));
    const QStringList paragraphs = normalizedText.split(QLatin1Char('\n'));

    QTextOption textOption;
    textOption.setWrapMode(m_fitToText ? QTextOption::NoWrap : QTextOption::WordWrap);
    textOption.setUseDesignMetrics(true);
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

    struct ParaData {
        QList<QGlyphRun> glyphRuns;
        qreal startY;   // cumulative Y offset within the overall text block
    };
    QList<ParaData> allParas;
    allParas.reserve(paragraphs.size());

    qreal totalHeight = 0.0;
    for (const QString& para : paragraphs) {
        ParaData pd;
        pd.startY = totalHeight;
        if (para.isEmpty()) {
            // Blank line: no glyphs but must advance Y by one line height.
            totalHeight += emptyLineHeight;
            allParas.append(std::move(pd));
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
        pd.glyphRuns = layout.glyphRuns();
        totalHeight += (lineY > 0.0 ? lineY : emptyLineHeight);
        allParas.append(std::move(pd));
    }

    // ── 3. Vertical alignment offset ─────────────────────────────────────────
    // vertOffset is intentionally NOT baked into the SVG paths here.
    // QML applies the same vertical-centering offset via textDisplayNode.topPadding
    // (which is a synchronous QML binding on height/contentHeight).  Baking it here
    // would desync the stroke from the text fill because TextGlyphPath updates on a
    // 0-interval timer (one event-loop tick later), while textDisplayNode.topPadding
    // updates immediately — causing visible border drift during any resize operation.
    // The textStrokeShape Shape applies `transform: Translate { y: textDisplayNode.topPadding }`
    // to place the top-aligned paths at the correct vertical offset.
    const qreal vertOffset = 0.0;

    // ── 4. Extract glyph paths using per-unique-glyph cache ──────────────────
    // m_glyphPathCache   : raw glyph shape at origin — invalidated on font change.
    // m_strokeGlyphCache : stroked+filled shape at origin — invalidated on font
    //                      or outlinePixels change.
    //
    // QPainterPathStroker and united() are called on ONE glyph (~50–100 elements)
    // at a time, never on the fully merged text. Each unique glyph ID is computed
    // once and reused for every repeated instance across all paragraphs.
    // pd.startY offsets each paragraph's glyphs to the correct vertical position.
    QPainterPath fillPath;
    QPainterPath strokePath;

    const bool needsStroke = (m_outlinePixels > 0.0);
    for (const ParaData& pd : allParas) {
        for (const QGlyphRun& run : pd.glyphRuns) {
            const QRawFont rawFont      = run.rawFont();
            const QList<quint32>& ids   = run.glyphIndexes();
            const QList<QPointF>& poses = run.positions();
            const QString runKeyPrefix = rawFont.familyName()
                    + QLatin1Char('|')
                    + rawFont.styleName()
                    + QLatin1Char('|')
                    + QString::number(rawFont.pixelSize(), 'f', 3)
                    + QLatin1Char('|');
            const int count = qMin(ids.size(), poses.size());
            for (int i = 0; i < count; ++i) {
                const quint32 id = ids[i];
                const QString cacheKey = runKeyPrefix + QString::number(id);

                // ── Fill glyph (cache lookup / populate on miss) ──────────────
                auto fillIt = m_glyphPathCache.find(cacheKey);
                if (fillIt == m_glyphPathCache.end())
                    fillIt = m_glyphPathCache.insert(cacheKey, rawFont.pathForGlyph(id));

                const QPainterPath& cachedGlyph = fillIt.value();
                if (cachedGlyph.isEmpty())
                    continue;

                // poses[i] is relative to the paragraph's layout origin.
                // pd.startY stacks paragraphs; vertOffset applies v-alignment.
                const QTransform t(1.0, 0.0, 0.0, 1.0,
                                   poses[i].x(),
                                   poses[i].y() + pd.startY + vertOffset);
                fillPath.addPath(t.map(cachedGlyph));

                // ── Stroke glyph (cache lookup / populate on miss) ────────────
                if (needsStroke) {
                    auto strokeIt = m_strokeGlyphCache.find(cacheKey);
                    if (strokeIt == m_strokeGlyphCache.end()) {
                        QPainterPathStroker stroker;
                        stroker.setWidth(m_outlinePixels * 2.0);
                        stroker.setJoinStyle(Qt::MiterJoin);
                        stroker.setCapStyle(Qt::FlatCap);
                        QPainterPath expanded = stroker.createStroke(cachedGlyph);
                        strokeIt = m_strokeGlyphCache.insert(cacheKey, cachedGlyph.united(expanded));
                    }
                    strokePath.addPath(t.map(strokeIt.value()));
                }
            }
        }
    }

    // ── 5. Convert to SVG strings ─────────────────────────────────────────────
    QString newFill;
    QString newStroke;

    if (!fillPath.isEmpty()) {
        newFill = painterPathToSvg(fillPath);
        if (needsStroke && !strokePath.isEmpty())
            newStroke = painterPathToSvg(strokePath);
    }

    if (newFill != m_fillPath || newStroke != m_strokePath || totalHeight != m_textBlockHeight) {
        m_fillPath        = newFill;
        m_strokePath      = newStroke;
        m_textBlockHeight = totalHeight;
        emit pathsChanged();
    }
}

// ---------------------------------------------------------------------------
// QPainterPath → SVG path string (file-local helper)
// ---------------------------------------------------------------------------
static QString painterPathToSvg(const QPainterPath& path)
{
    QString svg;
    svg.reserve(path.elementCount() * 24);

    bool hasOpenSubpath = false;

    for (int i = 0; i < path.elementCount(); ++i) {
        const QPainterPath::Element el = path.elementAt(i);
        switch (el.type) {
        case QPainterPath::MoveToElement:
            if (hasOpenSubpath) svg += QLatin1String("Z ");
            svg += QLatin1Char('M');
            svg += QString::number(el.x, 'f', 3);
            svg += QLatin1Char(' ');
            svg += QString::number(el.y, 'f', 3);
            svg += QLatin1Char(' ');
            hasOpenSubpath = true;
            break;

        case QPainterPath::LineToElement:
            svg += QLatin1Char('L');
            svg += QString::number(el.x, 'f', 3);
            svg += QLatin1Char(' ');
            svg += QString::number(el.y, 'f', 3);
            svg += QLatin1Char(' ');
            break;

        case QPainterPath::CurveToElement: {
            // Followed by exactly 2 CurveToDataElements (control point 2, end point)
            const QPainterPath::Element& cp2 = path.elementAt(i + 1);
            const QPainterPath::Element& ep  = path.elementAt(i + 2);
            svg += QLatin1Char('C');
            svg += QString::number(el.x,  'f', 3); svg += QLatin1Char(' ');
            svg += QString::number(el.y,  'f', 3); svg += QLatin1Char(' ');
            svg += QString::number(cp2.x, 'f', 3); svg += QLatin1Char(' ');
            svg += QString::number(cp2.y, 'f', 3); svg += QLatin1Char(' ');
            svg += QString::number(ep.x,  'f', 3); svg += QLatin1Char(' ');
            svg += QString::number(ep.y,  'f', 3); svg += QLatin1Char(' ');
            i += 2;
            break;
        }

        case QPainterPath::CurveToDataElement:
            // Consumed above — should not be reached.
            break;
        }
    }

    if (hasOpenSubpath)
        svg += QLatin1Char('Z');

    return svg;
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
SETTER_IMPL(qreal,          ItemHeight,            itemHeight)
SETTER_IMPL(const QString&, HorizontalAlignment,   horizontalAlignment)
SETTER_IMPL(const QString&, VerticalAlignment,     verticalAlignment)
SETTER_IMPL(bool,           FitToText,             fitToText)

#undef SETTER_IMPL
