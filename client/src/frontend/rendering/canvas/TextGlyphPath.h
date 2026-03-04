#pragma once

#include <QHash>
#include <QPainterPath>
#include <QQuickItem>
#include <QString>

/**
 * TextGlyphPath
 *
 * Computes two SVG path strings from QTextLayout + QRawFont glyph outlines:
 *   - fillPath   : the filled glyph contours (for text color)
 *   - strokePath : the expanded glyph region (for border/outline color)
 *
 * Paths are recomputed only when an input property changes.  Recomputation is
 * deferred to the scene-graph polish phase (QQuickItem::updatePolish) so that
 * all QML bindings that fire in a single frame are batched into exactly one
 * recompute call — which runs BEFORE updatePaintNode/tessellation in the same
 * frame.  This makes the stroke shape pixel-perfect in sync with the TextEdit
 * fill on every frame, including during live resize operations.
 *
 * The item has zero size and ItemHasNoContents so it is invisible and does not
 * participate in layout.  Camera pan/zoom never triggers a recompute.
 *
 * Register as QML type:
 *   qmlRegisterType<TextGlyphPath>("Mouffette.Canvas", 1, 0, "TextGlyphPath");
 *
 * QML usage:
 *   import Mouffette.Canvas 1.0
 *   TextGlyphPath { id: gp; ... }
 *   Shape { ShapePath { fillColor: ...; PathSvg { path: gp.strokePath } } }
 *   Shape { ShapePath { fillColor: ...; PathSvg { path: gp.fillPath   } } }
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
    Q_PROPERTY(qreal   itemHeight       READ itemHeight       WRITE setItemHeight       NOTIFY inputChanged)
    Q_PROPERTY(QString horizontalAlignment READ horizontalAlignment WRITE setHorizontalAlignment NOTIFY inputChanged)
    Q_PROPERTY(QString verticalAlignment   READ verticalAlignment   WRITE setVerticalAlignment   NOTIFY inputChanged)
    Q_PROPERTY(bool    fitToText        READ fitToText        WRITE setFitToText        NOTIFY inputChanged)

    // ── Outputs ─────────────────────────────────────────────────────────────
    Q_PROPERTY(QString fillPath        READ fillPath        NOTIFY pathsChanged)
    Q_PROPERTY(QString strokePath      READ strokePath      NOTIFY pathsChanged)
    // Height of the laid-out text block (total content height, before vertical
    // alignment offset). Use this instead of QML Text.paintedHeight so that
    // edit-mode topPadding matches the display-mode glyph positions exactly.
    Q_PROPERTY(qreal   textBlockHeight READ textBlockHeight NOTIFY pathsChanged)

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
    qreal   itemHeight()       const { return m_itemHeight; }
    QString horizontalAlignment() const { return m_horizontalAlignment; }
    QString verticalAlignment()   const { return m_verticalAlignment; }
    bool    fitToText()        const { return m_fitToText; }
    QString fillPath()         const { return m_fillPath; }
    QString strokePath()       const { return m_strokePath; }
    qreal   textBlockHeight()  const { return m_textBlockHeight; }

    // Setters
    void setTextContent      (const QString& v);
    void setFontFamily       (const QString& v);
    void setFontPixelSize    (int v);
    void setFontWeight       (int v);
    void setFontItalic       (bool v);
    void setFontUppercase    (bool v);
    void setOutlinePixels   (qreal v);
    void setItemWidth       (qreal v);
    void setItemHeight      (qreal v);
    void setHorizontalAlignment(const QString& v);
    void setVerticalAlignment  (const QString& v);
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
    qreal   m_itemHeight       { 100.0 };
    QString m_horizontalAlignment { QStringLiteral("center") };
    QString m_verticalAlignment   { QStringLiteral("center") };
    bool    m_fitToText        { false };

    // ── Output state ────────────────────────────────────────────────────────
    QString m_fillPath;
    QString m_strokePath;
    qreal   m_textBlockHeight { 0.0 };

    // ── Per-unique-glyph path caches ─────────────────────────────────────────
    // Keyed by "raw-font identity + glyph index". Glyph IDs are local to each
    // font face, so using only the numeric ID can collide across fallback
    // fonts (same ID in two different fonts means different outlines).
    // Invalidated on font property change.
    // m_glyphPathCache  : raw glyph shape at origin
    // m_strokeGlyphCache: stroked+filled shape at origin
    // m_cachedOutlinePixels: the outlinePixels value the stroke cache was built
    //                        for; -1 means the stroke cache is empty/invalid.
    QHash<QString, QPainterPath> m_glyphPathCache;
    QHash<QString, QPainterPath> m_strokeGlyphCache;
    qreal m_cachedOutlinePixels { -1.0 };
};
