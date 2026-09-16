#pragma once

#include <QColor>
#include <QQuickItem>
#include <memory>

// A border for the actual TextEdit document. Qt owns shaping, wrapping and
// glyph rasterization; this adapter retains small groups of Qt image quads and
// immutable per-glyph masks instead of re-evaluating curves on every GPU frame.
// The Qt Quick private API is deliberately isolated in the .cpp file.
class TextOutlineItem : public QQuickItem
{
    Q_OBJECT
    Q_PROPERTY(QQuickItem* source READ source WRITE setSource NOTIFY sourceChanged)
    Q_PROPERTY(qreal outlinePixels READ outlinePixels WRITE setOutlinePixels NOTIFY outlinePixelsChanged)
    Q_PROPERTY(QColor color READ color WRITE setColor NOTIFY colorChanged)
    Q_PROPERTY(bool rasterUpdatesDeferred READ rasterUpdatesDeferred WRITE setRasterUpdatesDeferred NOTIFY rasterUpdatesDeferredChanged)
    Q_PROPERTY(QRectF renderedRect READ renderedRect NOTIFY viewportChanged)
    Q_PROPERTY(QSize renderedPixelSize READ renderedPixelSize NOTIFY viewportChanged)

public:
    explicit TextOutlineItem(QQuickItem* parent = nullptr);
    ~TextOutlineItem() override;

    QQuickItem* source() const;
    void setSource(QQuickItem* source);
    qreal outlinePixels() const;
    void setOutlinePixels(qreal width);
    QColor color() const;
    void setColor(const QColor& color);
    bool rasterUpdatesDeferred() const;
    void setRasterUpdatesDeferred(bool deferred);
    QRectF renderedRect() const;
    QSize renderedPixelSize() const;

    // C++ diagnostics used by the rendering regression/benchmark tests.
    static constexpr qint64 maskCacheBudgetBytes = 64 * 1024 * 1024;
    struct Statistics {
        int glyphs = 0;
        int chunks = 0;
        int generatedGlyphs = 0;
        int rebuiltChunks = 0;
        int movedChunks = 0;
        int layoutPasses = 0;
        int uploadedGlyphs = 0;
        int atlasedGlyphs = 0;
        qint64 cachedMaskBytes = 0;
        qint64 textureBytes = 0;
        qint64 triangles = 0;
        qint64 polishNanoseconds = 0;
        qint64 syncNanoseconds = 0;
        int refinementJobsStarted = 0;
        int refinementJobsApplied = 0;
        int refinementJobsDiscarded = 0;
        qint64 refinementNanoseconds = 0;
        qint64 refinementApplyNanoseconds = 0;
    };
    Statistics statistics() const;
    bool qualityRefinementPending() const;

signals:
    void sourceChanged();
    void outlinePixelsChanged();
    void colorChanged();
    void rasterUpdatesDeferredChanged();
    void viewportChanged();

protected:
    void updatePolish() override;
    QSGNode* updatePaintNode(QSGNode* oldNode, UpdatePaintNodeData*) override;
    void geometryChange(const QRectF& newGeometry, const QRectF& oldGeometry) override;
    void itemChange(ItemChange change, const ItemChangeData& data) override;

private:
    void scheduleLayout();
    void scheduleViewport();
    void startQualityRefinement(qreal rasterScale);
    struct Private;
    std::unique_ptr<Private> d;
};
