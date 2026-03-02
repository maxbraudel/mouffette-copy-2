#pragma once

#include <QObject>
#include <QString>

class QTimer;

/**
 * TextGlyphPath
 *
 * Computes two SVG path strings from QTextLayout + QRawFont glyph outlines:
 *   - fillPath   : the filled glyph contours (for text color)
 *   - strokePath : the expanded glyph region (for border/outline color)
 *
 * Paths are recomputed only when an input property changes (batched via a
 * zero-interval timer so multiple bindings firing in the same frame collapse
 * to a single recompute).  Camera pan/zoom never triggers a recompute: the
 * QtQuick.Shapes renderer caches tessellated geometry and uses GPU transforms.
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
class TextGlyphPath : public QObject
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
    Q_PROPERTY(QString fillPath   READ fillPath   NOTIFY pathsChanged)
    Q_PROPERTY(QString strokePath READ strokePath NOTIFY pathsChanged)

public:
    explicit TextGlyphPath(QObject* parent = nullptr);

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

private slots:
    void recompute();

private:
    void scheduleRecompute();

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

    QTimer* m_recomputeTimer { nullptr };
};
