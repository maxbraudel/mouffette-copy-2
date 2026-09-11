#pragma once

#include <QColor>
#include <QQuickItem>
#include <memory>

// A border for the actual TextEdit document. Qt owns shaping, wrapping and
// glyph rasterization; this adapter retains small groups of GPU curve geometry.
// The Qt Quick private API is deliberately isolated in the .cpp file.
class TextOutlineItem : public QQuickItem
{
    Q_OBJECT
    Q_PROPERTY(QQuickItem* source READ source WRITE setSource NOTIFY sourceChanged)
    Q_PROPERTY(qreal outlinePixels READ outlinePixels WRITE setOutlinePixels NOTIFY outlinePixelsChanged)
    Q_PROPERTY(QColor color READ color WRITE setColor NOTIFY colorChanged)

public:
    explicit TextOutlineItem(QQuickItem* parent = nullptr);
    ~TextOutlineItem() override;

    QQuickItem* source() const;
    void setSource(QQuickItem* source);
    qreal outlinePixels() const;
    void setOutlinePixels(qreal width);
    QColor color() const;
    void setColor(const QColor& color);

    // C++ diagnostics used by the rendering regression/benchmark tests.
    struct Statistics {
        int glyphs = 0;
        int chunks = 0;
        int generatedGlyphs = 0;
        int rebuiltChunks = 0;
        qint64 triangles = 0;
        qint64 polishNanoseconds = 0;
        qint64 syncNanoseconds = 0;
    };
    Statistics statistics() const;

signals:
    void sourceChanged();
    void outlinePixelsChanged();
    void colorChanged();

protected:
    void updatePolish() override;
    QSGNode* updatePaintNode(QSGNode* oldNode, UpdatePaintNodeData*) override;
    void geometryChange(const QRectF& newGeometry, const QRectF& oldGeometry) override;

private:
    void scheduleLayout();
    struct Private;
    std::unique_ptr<Private> d;
};
