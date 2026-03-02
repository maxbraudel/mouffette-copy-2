#include "frontend/rendering/canvas/TextGlyphPath.h"

#include <QFont>
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

// ---------------------------------------------------------------------------
// Core computation
// ---------------------------------------------------------------------------

void TextGlyphPath::recompute()
{
    // ── 1. Font ─────────────────────────────────────────────────────────────
    QFont font;
    font.setFamily(m_fontFamily);
    font.setPixelSize(qMax(1, m_fontPixelSize));
    font.setWeight(QFont::Weight(m_fontWeight));
    font.setItalic(m_fontItalic);

    const QString text = m_fontUppercase ? m_textContent.toUpper() : m_textContent;

    // ── 2. Text layout (handles wrapping, alignment, multi-line) ────────────
    QTextLayout layout(text, font);

    QTextOption textOption;
    textOption.setWrapMode(m_fitToText ? QTextOption::NoWrap : QTextOption::WordWrap);

    Qt::Alignment hAlign = Qt::AlignHCenter;
    if (m_horizontalAlignment == QLatin1String("left"))       hAlign = Qt::AlignLeft;
    else if (m_horizontalAlignment == QLatin1String("right")) hAlign = Qt::AlignRight;
    textOption.setAlignment(hAlign);

    layout.setTextOption(textOption);

    layout.beginLayout();
    const qreal availWidth = qMax(1.0, m_itemWidth);
    qreal lineY = 0.0;
    while (true) {
        QTextLine line = layout.createLine();
        if (!line.isValid())
            break;
        line.setLineWidth(availWidth);
        line.setPosition(QPointF(0.0, lineY));
        lineY += line.height();
    }
    layout.endLayout();

    // ── 3. Vertical alignment offset ────────────────────────────────────────
    const qreal totalHeight = layout.boundingRect().height();
    qreal vertOffset = 0.0;
    if (m_verticalAlignment == QLatin1String("center"))
        vertOffset = (m_itemHeight - totalHeight) * 0.5;
    else if (m_verticalAlignment == QLatin1String("bottom"))
        vertOffset = m_itemHeight - totalHeight;

    // ── 4. Extract glyph paths → unified fill path ───────────────────────────
    // QRawFont::pathForGlyph() returns paths in font coordinates (y-up, baseline
    // at y=0).  QGlyphRun::positions() are in layout coordinates (y-down).
    // Transform per glyph: x' = x + pos.x,  y' = -y + pos.y + vertOffset
    QPainterPath fillPath;
    const QList<QGlyphRun> glyphRuns = layout.glyphRuns();
    for (const QGlyphRun& run : glyphRuns) {
        const QRawFont rawFont      = run.rawFont();
        const QList<quint32>& ids   = run.glyphIndexes();
        const QList<QPointF>& poses = run.positions();
        const int count = qMin(ids.size(), poses.size());
        for (int i = 0; i < count; ++i) {
            QPainterPath glyph = rawFont.pathForGlyph(ids[i]);
            if (glyph.isEmpty())
                continue;
            QTransform t(1.0, 0.0, 0.0, 1.0,
                         poses[i].x(),
                         poses[i].y() + vertOffset);
            fillPath.addPath(t.map(glyph));
        }
    }

    // ── 5. Convert to SVG strings ────────────────────────────────────────────
    QString newFill;
    QString newStroke;

    if (!fillPath.isEmpty()) {
        newFill = painterPathToSvg(fillPath);

        if (m_outlinePixels > 0.0) {
            // Expand the glyph outlines outward by outlinePixels on each side.
            QPainterPathStroker stroker;
            stroker.setWidth(m_outlinePixels * 2.0);
            stroker.setJoinStyle(Qt::MiterJoin);
            stroker.setCapStyle(Qt::FlatCap);
            QPainterPath expandedRing = stroker.createStroke(fillPath);
            // Unite with fill so the interior of the glyph is fully covered.
            QPainterPath fullOutline = fillPath.united(expandedRing);
            newStroke = painterPathToSvg(fullOutline);
        }
    }

    if (newFill != m_fillPath || newStroke != m_strokePath) {
        m_fillPath   = newFill;
        m_strokePath = newStroke;
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
// Setters — all follow identical pattern: guard equality, update, emit signal
// ---------------------------------------------------------------------------

#define SETTER_IMPL(Type, name, member)       \
void TextGlyphPath::set##name(Type v) {       \
    if (m_##member == v) return;              \
    m_##member = v;                           \
    emit inputChanged();                      \
}

SETTER_IMPL(const QString&, TextContent,       textContent)
SETTER_IMPL(const QString&, FontFamily,        fontFamily)
SETTER_IMPL(int,            FontPixelSize,     fontPixelSize)
SETTER_IMPL(int,            FontWeight,        fontWeight)
SETTER_IMPL(bool,           FontItalic,        fontItalic)
SETTER_IMPL(bool,           FontUppercase,     fontUppercase)
SETTER_IMPL(qreal,          OutlinePixels,    outlinePixels)
SETTER_IMPL(qreal,          ItemWidth,        itemWidth)
SETTER_IMPL(qreal,          ItemHeight,       itemHeight)
SETTER_IMPL(const QString&, HorizontalAlignment, horizontalAlignment)
SETTER_IMPL(const QString&, VerticalAlignment,   verticalAlignment)
SETTER_IMPL(bool,           FitToText,        fitToText)

#undef SETTER_IMPL
