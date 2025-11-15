#ifndef TEXTRASTERIZATIONLAYER_H
#define TEXTRASTERIZATIONLAYER_H

#include <QGraphicsPixmapItem>
#include <QPixmap>
#include <QList>
#include <QTimer>
#include <QFuture>
#include <QFutureWatcher>
#include <QMutex>
#include <QAtomicInt>
#include <QFont>
#include <QColor>
#include <QTransform>

class TextMediaItem;
class ScreenCanvas;

/**
 * Global text rasterization layer that consolidates all text media elements
 * into a single composited QPixmap for optimal rendering performance.
 * 
 * This layer renders all text items at zoom-dependent resolution and only
 * re-rasterizes when zoom threshold is exceeded or text content changes.
 * 
 * Z-order: 9998 (below selection chrome at 11998, above media items 1-9999)
 */
class TextRasterizationLayer : public QObject, public QGraphicsPixmapItem {
    Q_OBJECT

public:
    explicit TextRasterizationLayer(ScreenCanvas* canvas);
    ~TextRasterizationLayer() override;
    
    // Core rasterization
    void invalidate();
    void rasterizeAllText();
    
    // Zoom management
    void setCurrentZoomFactor(qreal zoom);
    qreal currentZoomFactor() const { return m_currentZoom; }
    
    // Rasterization factor (for debugging - lower values = more pixelation)
    void setRasterizationFactor(qreal factor);
    qreal rasterizationFactor() const { return m_rasterizationFactor; }
    
    // Text item registration
    void registerTextItem(TextMediaItem* item);
    void unregisterTextItem(TextMediaItem* item);
    void onTextItemChanged(TextMediaItem* item);
    
    // Layer visibility
    void setLayerVisible(bool visible);
    bool isLayerVisible() const { return m_layerVisible; }
    
    // Configuration
    static constexpr qreal ZOOM_THRESHOLD = 0.15;  // Re-raster if zoom changes >15%
    static constexpr int RASTER_DELAY_MS = 0;      // No debounce - immediate rasterization
    static constexpr int TILE_SIZE = 256;          // Small tile size for fine-grained updates
    static constexpr int TILE_OVERLAP = 2;         // Pixel overlap between tiles to prevent gaps
    static constexpr int VIEWPORT_BUFFER_TILES = 1; // Number of tile layers to render outside viewport

private slots:
    void performRasterization();

private:
    struct TextItemRenderData {
        QTransform sceneTransform;
        QRectF sceneBoundingRect;
        QString text;
        QFont font;
        QColor textColor;
        qreal borderWidthPercent = 0.0;
        QColor borderColor;
        bool highlightEnabled = false;
        QColor highlightColor;
        bool directPaintingEnabled = false;
        qreal zValue = 0.0;
        
        // Default constructor
        TextItemRenderData() = default;
        
        // Copy constructor
        TextItemRenderData(const TextItemRenderData&) = default;
        
        // Move constructor
        TextItemRenderData(TextItemRenderData&&) noexcept = default;
        
        // Copy assignment
        TextItemRenderData& operator=(const TextItemRenderData&) = default;
        
        // Move assignment
        TextItemRenderData& operator=(TextItemRenderData&&) noexcept = default;
    };
    
    struct TileRenderData {
        QRect viewportRect;
        QRectF sceneRect;
        QSize pixmapSize;
        qreal resolution;
        QList<TextItemRenderData> textItems;
        int tileIndex;
    };
    
    struct Tile {
        QGraphicsPixmapItem* item = nullptr;
        QRect viewportRect;  // Tile bounds in viewport coordinates (pixels)
        qreal lastZoom = 0.0;
        bool dirty = true;
        QAtomicInt rendering; // 0 = idle, 1 = rendering
    };
    
    void scheduleRasterization();
    QRectF calculateTextBounds() const;
    QRectF getVisibleSceneRect() const;
    qreal computeRasterResolution() const;
    bool shouldRerasterForZoom(qreal newZoom) const;
    void updateTileGrid();
    void renderTile(Tile& tile, const QList<TextMediaItem*>& sortedItems, qreal resolution);
    void cleanupUnusedTiles(const QRectF& viewportBounds);
    
    // Async rendering
    static QPixmap renderTileAsync(const TileRenderData& data);
    void startAsyncRasterization();
    void applyRenderedTile(int tileIndex, const QPixmap& pixmap, const QRectF& sceneRect, qreal resolution);
    void onAllTilesRendered();
    
    ScreenCanvas* m_canvas;
    QList<TextMediaItem*> m_textItems;
    QTimer* m_rasterTimer;
    qreal m_currentZoom;
    qreal m_lastRasterZoom;
    qreal m_rasterizationFactor;
    bool m_layerVisible;
    bool m_dirty;
    QList<Tile> m_tiles;
    
    // Async rendering state
    QList<QFutureWatcher<QPixmap>*> m_activeWatchers;
    QMutex m_renderMutex;
    QAtomicInt m_pendingRenders;
    bool m_asyncRenderInProgress;
};

#endif // TEXTRASTERIZATIONLAYER_H
