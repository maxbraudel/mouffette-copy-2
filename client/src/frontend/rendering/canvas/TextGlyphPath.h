#pragma once

#include <QByteArray>
#include <QHash>
#include <QPainterPath>
#include <QPointF>
#include <QQuickItem>
#include <QRawFont>
#include <QString>
#include <QVector>

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

    // ── Outputs ─────────────────────────────────────────────────────────────
    Q_PROPERTY(QString strokePath      READ strokePath      NOTIFY pathsChanged)

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
    QString strokePath()       const { return m_strokePath; }

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

signals:
    void inputChanged();
    void pathsChanged();

protected:
    // Called by the Qt scene-graph polish mechanism once per frame, after all
    // QML bindings have fired but before tessellation (updatePaintNode).  This
    // guarantees the stroke path is always up-to-date in the same frame as the
    // property changes that triggered it.
    void updatePolish() override;

private:
    void clearGlyphCaches();      // invalidates both fill + stroke caches
    void clearStrokeCache();      // invalidates stroke cache only (outlinePixels changed)
    // ── Input state ─────────────────────────────────────────────────────────
    QString m_textContent;
    QString m_fontFamily        { QStringLiteral("Arial") };
    int     m_fontPixelSize     { 22 };
    int     m_fontWeight        { 400 };
    bool    m_fontItalic        { false };
    bool    m_fontUppercase     { false };
    qreal   m_outlinePixels    { 0.0 };
    qreal   m_itemWidth        { 100.0 };
    QString m_horizontalAlignment { QStringLiteral("center") };
    bool    m_fitToText        { false };

    // ── Output state ────────────────────────────────────────────────────────
    QString m_strokePath;

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
    QString               m_layoutCacheKey;

    // ── Per-unique-glyph path caches (keyed by packed quint64) ───────────────
    // Key format: upper 32 bits = truncated font-identity hash (runPrefixHash),
    //             lower 32 bits = glyph ID.
    // Avoids one QString heap allocation per glyph lookup vs the old
    // QString-keyed design.  Qt's qHash(quint64) is built-in, no custom hasher.
    //
    // m_glyphPathCache  : raw glyph outline at origin (QRawFont::pathForGlyph).
    //                     Needed when populating m_strokeElemCache.
    //                     Invalidated on any font property change.
    // m_strokeElemCache : stroke ring encoded as compact flat QVector<float>.
    //                     Iterated once per instance; SVG coords written with
    //                     per-instance (tx,ty) offset via snprintf — zero heap
    //                     allocation per glyph instance.
    //                     Invalidated on font or outlinePixels change.
    // m_cachedOutlinePixels: the outlinePixels value used to build the stroke
    //                        cache (-1 = empty/invalid).
    QHash<quint64, QPainterPath>    m_glyphPathCache;
    QHash<quint64, QVector<float>>  m_strokeElemCache;
    qreal m_cachedOutlinePixels { -1.0 };
};
