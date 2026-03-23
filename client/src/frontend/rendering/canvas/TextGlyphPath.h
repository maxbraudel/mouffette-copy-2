#pragma once

#include <QByteArray>
#include <QHash>
#include <QList>
#include <QPainterPath>
#include <QPair>
#include <QPointF>
#include <QQuickItem>
#include <QRawFont>
#include <QString>
#include <QVector>

// ---------------------------------------------------------------------------
// Pre-compiled per-glyph stroke SVG in relative coordinates.
// Stored in TextGlyphPath::m_strokeSvgCache, keyed by (font-hash | glyph-id).
// Declared at file scope so the free static helper buildStrokeGlyphSvg() in
// TextGlyphPath.cpp can construct and return values of these types.
// ---------------------------------------------------------------------------
struct StrokeSubpath {
    float      ox, oy;  // absolute MoveTo origin in glyph space
    QByteArray body;    // relative SVG commands: "l dx dy c ... z "
};
struct StrokeGlyphSvg {
    QList<StrokeSubpath> subpaths;
};

/**
 * TextGlyphPath
 *
 * Computes the stroke SVG path string from QTextLayout + QRawFont glyph outlines.
 * The stroke (expanded glyph region) is used for the border/outline colour.
 * Fill text is rendered natively by the single shared TextEdit in TextItem.qml.
 *
 * Paths are recomputed only when an input property changes.  Recomputation is
 * deferred to the scene-graph polish phase (QQuickItem::updatePolish) so that
 * all QML bindings that fire in a single frame are batched into exactly one
 * recompute call — which runs BEFORE updatePaintNode/tessellation in the same
 * frame.  This makes the stroke shape pixel-perfect in sync with the TextEdit
 * fill on every frame, including during live resize operations.
 *
 * The item has zero size and does not participate in layout (ItemHasContents is
 * NOT set, which is the off-by-default state for QQuickItem in Qt 6).
 * Camera pan/zoom never triggers a recompute.
 *
 * Register as QML type:
 *   qmlRegisterType<TextGlyphPath>("Mouffette.Canvas", 1, 0, "TextGlyphPath");
 *
 * QML usage:
 *   import Mouffette.Canvas 1.0
 *   TextGlyphPath { id: gp; ... }
 *   Shape { ShapePath { fillColor: outlineColor; PathSvg { path: gp.strokePath } } }
 */
class TextGlyphPath : public QQuickItem
{
    Q_OBJECT

    // ── Inputs ──────────────────────────────────────────────────────────────
    Q_PROPERTY(QString textContent       READ textContent       WRITE setTextContent       NOTIFY inputChanged)
    Q_PROPERTY(QString fontFamily        READ fontFamily        WRITE setFontFamily        NOTIFY inputChanged)
    Q_PROPERTY(int     fontPixelSize     READ fontPixelSize     WRITE setFontPixelSize     NOTIFY inputChanged)
    Q_PROPERTY(int     fontWeight        READ fontWeight        WRITE setFontWeight        NOTIFY inputChanged)
    Q_PROPERTY(bool    fontItalic        READ fontItalic        WRITE setFontItalic        NOTIFY inputChanged)
    Q_PROPERTY(bool    fontUppercase     READ fontUppercase     WRITE setFontUppercase     NOTIFY inputChanged)
    Q_PROPERTY(qreal   outlinePixels    READ outlinePixels    WRITE setOutlinePixels    NOTIFY inputChanged)
    Q_PROPERTY(qreal   itemWidth        READ itemWidth        WRITE setItemWidth        NOTIFY inputChanged)
    Q_PROPERTY(QString horizontalAlignment READ horizontalAlignment WRITE setHorizontalAlignment NOTIFY inputChanged)
    Q_PROPERTY(bool    fitToText        READ fitToText        WRITE setFitToText        NOTIFY inputChanged)
    Q_PROPERTY(QString verticalAlignment READ verticalAlignment WRITE setVerticalAlignment NOTIFY inputChanged)
    Q_PROPERTY(qreal   itemHeight        READ itemHeight        WRITE setItemHeight        NOTIFY inputChanged)

    // ── Outputs ─────────────────────────────────────────────────────────────
    Q_PROPERTY(QString strokePath      READ strokePath      NOTIFY pathsChanged)    // Horizontal translate to apply to the Shape item when only the alignment-derived
    // x-shift changed (no reflow).  Updating this moves the border without retessellation.
    Q_PROPERTY(qreal   strokeXOffset   READ strokeXOffset   NOTIFY strokeXOffsetChanged)
public:
    explicit TextGlyphPath(QQuickItem* parent = nullptr);

    // Getters
    QString textContent()       const { return m_textContent; }
    QString fontFamily()        const { return m_fontFamily; }
    int     fontPixelSize()     const { return m_fontPixelSize; }
    int     fontWeight()        const { return m_fontWeight; }
    bool    fontItalic()        const { return m_fontItalic; }
    bool    fontUppercase()     const { return m_fontUppercase; }
    qreal   outlinePixels()    const { return m_outlinePixels; }
    qreal   itemWidth()        const { return m_itemWidth; }
    QString horizontalAlignment() const { return m_horizontalAlignment; }
    bool    fitToText()        const { return m_fitToText; }
    QString verticalAlignment() const { return m_verticalAlignment; }
    qreal   itemHeight()        const { return m_itemHeight; }
    QString strokePath()       const { return m_strokePath; }
    qreal   strokeXOffset()    const { return m_strokeXOffset; }

    // Setters
    void setTextContent      (const QString& v);
    void setFontFamily       (const QString& v);
    void setFontPixelSize    (int v);
    void setFontWeight       (int v);
    void setFontItalic       (bool v);
    void setFontUppercase    (bool v);
    void setOutlinePixels   (qreal v);
    void setItemWidth       (qreal v);
    void setHorizontalAlignment(const QString& v);
    void setFitToText       (bool v);
    void setVerticalAlignment(const QString& v);
    void setItemHeight      (qreal v);

signals:
    void inputChanged();
    void pathsChanged();
    void strokeXOffsetChanged();

protected:
    // Called by the Qt scene-graph polish mechanism once per frame, after all
    // QML bindings have fired but before tessellation (updatePaintNode).  This
    // guarantees the stroke path is always up-to-date in the same frame as the
    // property changes that triggered it.
    void updatePolish() override;

private:
    void clearGlyphCaches();     // invalidates both fill + stroke caches
    void clearStrokeCache();     // invalidates stroke cache only (outlinePixels changed)
    QByteArray buildStrokeSvg(); // assembles SVG from cached layout + stroke element data
    // ── Input state ─────────────────────────────────────────────────────────
    QString m_textContent;
    QString m_fontFamily        { QStringLiteral("Impact") };
    int     m_fontPixelSize     { 22 };
    int     m_fontWeight        { 400 };
    bool    m_fontItalic        { false };
    bool    m_fontUppercase     { false };
    qreal   m_outlinePixels    { 0.0 };
    qreal   m_itemWidth        { 100.0 };
    QString m_horizontalAlignment { QStringLiteral("center") };
    bool    m_fitToText        { false };
    QString m_verticalAlignment { QStringLiteral("center") };
    qreal   m_itemHeight       { 100.0 };

    // ── Output state ─────────────────────────────────────────────────────────
    QString    m_strokePath;
    // Horizontal translate applied to the Shape when only the alignment-derived
    // x-shift changed (no reflow).  Avoids Qt Shape retessellation on every frame
    // during center/right-aligned resize.
    qreal      m_strokeXOffset    { 0.0 };
    // The itemWidth at which the current m_strokePath was built.  Used to compute
    // the correct strokeXOffset when the container resizes without a reflow.
    qreal      m_svgBuiltAtWidth  { 0.0 };

    // ── Per-unique-glyph path caches ─────────────────────────────────────────
    // Keyed by "raw-font identity + glyph index". Glyph IDs are local to each
    // font face, so using only the numeric ID can collide across fallback
    // fonts (same ID in two different fonts means different outlines).
    // Invalidated on font property change.
    // ── Layout position cache ────────────────────────────────────────────────
    // Glyph positions are independent of outlinePixels.  The layout cache key
    // covers all inputs that affect glyph placement (text, font, width, …).
    // On a cache hit the entire QTextLayout step is skipped — the dominant win
    // when the user drags the border-width slider frame after frame.
    //
    // Each CachedGlyphRun stores one QGlyphRun's worth of data plus the
    // paragraph startY offset and a pre-computed font-identity hash, so the
    // hot per-glyph loop does no string work at all.
    struct CachedGlyphRun {
        QRawFont       rawFont;
        QList<quint32> ids;           // glyph indexes
        QList<QPointF> positions;     // positions relative to paragraph origin
        quint32        runPrefixHash; // qHash("family|style|size|") at populate time
        qreal          startY;        // cumulative paragraph Y offset
    };
    QList<CachedGlyphRun> m_cachedGlyphRuns;

    // Compared field-by-field to avoid a heap-allocated key string on every
    // updatePolish() call.  Cheap int/bool fields are tested first so string
    // comparisons are skipped on the common case (only outlinePixels changed).
    // fontPixelSize defaults to -1 to force a rebuild on the very first call.
    struct LayoutCacheInputs {
        int     fontPixelSize   { -1 };
        int     fontWeight      { 0 };
        bool    fontItalic      { false };
        bool    fitToText       { false };
        qreal   itemWidth       { 0.0 };
        QString text;
        QString fontFamily;
        QString hAlign;
    };
    LayoutCacheInputs m_layoutInputs;

    // ── Per-unique-glyph path caches (keyed by packed quint64) ───────────────
    // Key format: upper 32 bits = truncated font-identity hash (runPrefixHash),
    //             lower 32 bits = glyph ID.
    // Avoids one QString heap allocation per glyph lookup vs the old
    // QString-keyed design.  Qt's qHash(quint64) is built-in, no custom hasher.
    //
    QHash<quint64, QPainterPath>   m_glyphPathCache;
    // Key: QPair<glyphKey, quantizedOutline>
    //   glyphKey        = (quint64(runPrefixHash) << 32) | quint64(glyphId)
    //   quantizedOutline= quint32(qRound(outlinePixels * 4))  — quarter-pixel precision
    // This encoding survives outlinePixels and fontPixelSize changes without a full
    // cache clear: old entries go naturally cold, new keys are inserted.
    // The cache is cleared only on font-shape changes (family / weight / italic)
    // via clearGlyphCaches().  clearStrokeCache() still resets the SVG build state
    // but no longer clears the hash itself.
    QHash<QPair<quint64,quint32>, StrokeGlyphSvg> m_strokeSvgCache;
    qreal   m_cachedOutlinePixels { -1.0 };
    // Vertical offset (in glyph-space pixels) baked into the stroke SVG y-coordinates.
    // Mirrors textDisplayNode.topPadding but computed entirely in C++ from m_totalHeight,
    // m_itemHeight, and m_verticalAlignment — no QML binding timing dependency.
    qreal   m_vertOffset          { 0.0 };
    qreal   m_cachedVertOffset    { -1.0 };  // sentinel; forces rebuild on first run
    qreal   m_totalHeight         { 0.0 };   // total text content height from last layout

    // Hash of all (startY, positions[], ids[]) in m_cachedGlyphRuns.
    // Compared after each QTextLayout run; if unchanged, buildStrokeSvg() is skipped.
    quint64 m_glyphRunsHash       { 0 };

    // Reflow-only hash: mixes only glyph IDs and Y positions (not X).
    // Stable during pure alignment x-shifts (center/right resize without line-break changes).
    // When m_glyphRunsHash changes but m_reflowHash is stable, only strokeXOffset needs updating.
    quint64 m_reflowHash          { 0 };

    // The reflow hash value at the time the last full SVG build was done.
    // Used to detect whether the current m_reflowHash represents a real change.
    quint64 m_prevReflowHash      { 0 };

    // Last non-zero outlinePixels value.  Stored whenever the user sets a border
    // so the pre-warm path builder knows which thickness to use when the border
    // is turned off.  Zero until the user has used a border at least once.
    qreal   m_prewarmOutlinePixels { 0.0 };

    // Maximum rightmost glyph x position in the last layout run.
    qreal   m_maxRightEdge        { 0.0 };

    // Intrinsic content width: maximum naturalTextWidth() across all layout lines.
    // Independent of itemWidth and alignment — represents the widest line of text.
    // Used by skipLayout: if m_itemWidth >= m_intrinsicContentWidth, no line-break
    // change is possible for any alignment, so QTextLayout can be skipped entirely.
    qreal   m_intrinsicContentWidth { 0.0 };
};
