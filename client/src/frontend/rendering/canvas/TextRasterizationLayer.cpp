#include "TextRasterizationLayer.h"
#include "ScreenCanvas.h"
#include "backend/domain/media/TextMediaItem.h"
#include <QPainter>
#include <QStyleOptionGraphicsItem>
#include <QDebug>
#include <cmath>
#include <algorithm>

TextRasterizationLayer::TextRasterizationLayer(ScreenCanvas* canvas)
    : QObject()
    , QGraphicsPixmapItem()
    , m_canvas(canvas)
    , m_rasterTimer(new QTimer(this))
    , m_currentZoom(1.0)
    , m_lastRasterZoom(1.0)
    , m_rasterizationFactor(1)
    , m_layerVisible(true)
    , m_dirty(true)
{
    // Z-order: Below selection chrome (11998), above media items (1-9999)
    setZValue(9998.0);
    
    // This item paints content
    setFlag(QGraphicsItem::ItemHasNoContents, false);
    
    // Configure debounce timer
    m_rasterTimer->setSingleShot(true);
    m_rasterTimer->setInterval(RASTER_DELAY_MS);
    connect(m_rasterTimer, &QTimer::timeout, this, &TextRasterizationLayer::performRasterization);
}

TextRasterizationLayer::~TextRasterizationLayer() {
    // Timer is a child of this QObject, will be auto-deleted
}

void TextRasterizationLayer::registerTextItem(TextMediaItem* item) {
    if (!item || m_textItems.contains(item)) {
        return;
    }
    
    m_textItems.append(item);
    invalidate();
}

void TextRasterizationLayer::unregisterTextItem(TextMediaItem* item) {
    if (m_textItems.removeOne(item)) {
        invalidate();
    }
}

void TextRasterizationLayer::onTextItemChanged(TextMediaItem* item) {
    Q_UNUSED(item);
    invalidate();
}

void TextRasterizationLayer::setCurrentZoomFactor(qreal zoom) {
    if (qFuzzyCompare(m_currentZoom, zoom)) {
        return;
    }
    
    m_currentZoom = zoom;
    
    // Only re-rasterize if zoom threshold exceeded
    if (shouldRerasterForZoom(zoom)) {
        scheduleRasterization();
    }
}

void TextRasterizationLayer::setRasterizationFactor(qreal factor) {
    const qreal clampedFactor = std::clamp(factor, 0.1, 4.0);
    
    if (!qFuzzyCompare(m_rasterizationFactor, clampedFactor)) {
        m_rasterizationFactor = clampedFactor;
        invalidate();
    }
}

bool TextRasterizationLayer::shouldRerasterForZoom(qreal newZoom) const {
    // Always rasterize if we haven't rasterized yet
    if (qFuzzyIsNull(m_lastRasterZoom)) {
        return true;
    }
    
    const qreal ratio = newZoom / m_lastRasterZoom;
    const qreal deviation = std::abs(ratio - 1.0);
    
    // Re-raster if zoom changed by more than threshold percentage
    return deviation > ZOOM_THRESHOLD;
}

void TextRasterizationLayer::invalidate() {
    m_dirty = true;
    
    // Mark all tiles as dirty
    for (Tile& tile : m_tiles) {
        tile.dirty = true;
    }
    
    scheduleRasterization();
}

void TextRasterizationLayer::scheduleRasterization() {
    if (!m_layerVisible) {
        return;
    }
    
    // Restart timer to debounce rapid invalidations
    m_rasterTimer->start();
}

QRectF TextRasterizationLayer::calculateTextBounds() const {
    QRectF bounds;
    
    for (const TextMediaItem* item : m_textItems) {
        if (!item || !item->isVisible()) {
            continue;
        }
        
        bounds = bounds.united(item->sceneBoundingRect());
    }
    
    return bounds;
}

qreal TextRasterizationLayer::computeRasterResolution() const {
    // Render at zoom-adjusted resolution multiplied by rasterization factor
    // Factor allows debugging (e.g., 0.5 = half resolution for visible pixelation)
    // No limits - resolution scales directly with zoom level
    return m_currentZoom * m_rasterizationFactor;
}

QRectF TextRasterizationLayer::getVisibleSceneRect() const {
    if (!m_canvas || !m_canvas->viewport()) {
        return QRectF();
    }
    
    // Get viewport rectangle and map to scene coordinates
    const QRect viewportRect = m_canvas->viewport()->rect();
    const QPolygonF scenePolygon = m_canvas->mapToScene(viewportRect);
    
    // Return bounding rect with small margin for smooth panning
    const qreal margin = 128.0; // One tile margin
    QRectF visibleRect = scenePolygon.boundingRect();
    visibleRect.adjust(-margin, -margin, margin, margin);
    
    return visibleRect;
}

void TextRasterizationLayer::updateTileGrid() {
    if (!m_canvas || !m_canvas->viewport()) {
        return;
    }
    
    // Get viewport dimensions (in pixels)
    const QRect viewportRect = m_canvas->viewport()->rect();
    if (viewportRect.isEmpty()) {
        return;
    }
    
    // Calculate tile grid dimensions based on viewport size
    const int tilesX = (viewportRect.width() + TILE_SIZE - 1) / TILE_SIZE;
    const int tilesY = (viewportRect.height() + TILE_SIZE - 1) / TILE_SIZE;
    const int neededTiles = tilesX * tilesY;
    
    // Create map of existing tiles for quick lookup
    QMap<QPair<int, int>, Tile*> existingTiles;
    for (Tile& tile : m_tiles) {
        const int tileX = tile.viewportRect.x() / TILE_SIZE;
        const int tileY = tile.viewportRect.y() / TILE_SIZE;
        existingTiles[qMakePair(tileX, tileY)] = &tile;
    }
    
    // Create tiles to cover viewport
    QList<Tile> newTiles;
    for (int tileY = 0; tileY < tilesY; ++tileY) {
        for (int tileX = 0; tileX < tilesX; ++tileX) {
            auto key = qMakePair(tileX, tileY);
            
            if (existingTiles.contains(key)) {
                // Keep existing tile
                newTiles.append(*existingTiles[key]);
            } else {
                // Create new tile with overlap to prevent gaps during zoom
                // Tiles extend TILE_OVERLAP pixels into adjacent tiles
                const int x = tileX * TILE_SIZE;
                const int y = tileY * TILE_SIZE;
                const int width = TILE_SIZE + (tileX < tilesX - 1 ? TILE_OVERLAP : 0);
                const int height = TILE_SIZE + (tileY < tilesY - 1 ? TILE_OVERLAP : 0);
                
                Tile newTile;
                newTile.viewportRect = QRect(x, y, width, height);
                newTile.dirty = true;
                newTile.item = nullptr;
                newTile.lastZoom = 0.0;
                newTiles.append(newTile);
            }
        }
    }
    
    // Delete tiles that are no longer needed
    for (Tile& oldTile : m_tiles) {
        const int tileX = oldTile.viewportRect.x() / TILE_SIZE;
        const int tileY = oldTile.viewportRect.y() / TILE_SIZE;
        const bool stillNeeded = (tileX < tilesX && tileY < tilesY);
        
        if (!stillNeeded && oldTile.item) {
            delete oldTile.item;
        }
    }
    
    m_tiles = newTiles;
}

void TextRasterizationLayer::renderTile(Tile& tile, const QList<TextMediaItem*>& sortedItems, qreal resolution) {
    if (!m_canvas) {
        return;
    }
    
    // Map viewport tile to scene coordinates
    const QPolygonF scenePolygon = m_canvas->mapToScene(tile.viewportRect);
    const QRectF tileSceneRect = scenePolygon.boundingRect();
    
    // Clamp resolution to prevent excessive memory usage
    const qreal clampedResolution = std::min(resolution, 10.0);
    
    // Calculate pixmap size based on scene rect dimensions and resolution
    // The pixmap should have enough pixels to render the scene area at the target resolution
    const QSize pixmapSize(
        static_cast<int>(std::ceil(tileSceneRect.width() * clampedResolution)),
        static_cast<int>(std::ceil(tileSceneRect.height() * clampedResolution))
    );
    
    // Safety check - prevent creating huge pixmaps
    const int maxPixmapDimension = 8192;
    if (pixmapSize.width() <= 0 || pixmapSize.height() <= 0 ||
        pixmapSize.width() > maxPixmapDimension || pixmapSize.height() > maxPixmapDimension) {
        qWarning() << "TextRasterizationLayer: Skipping tile - pixmap size:" << pixmapSize;
        return;
    }
    
    // Create or reuse tile pixmap item
    if (!tile.item) {
        tile.item = new QGraphicsPixmapItem(this);
        tile.item->setZValue(zValue());
    }
    
    // Create pixmap for this tile
    QPixmap tilePixmap(pixmapSize);
    tilePixmap.fill(Qt::transparent);
    
    QPainter painter(&tilePixmap);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setRenderHint(QPainter::TextAntialiasing, true);
    painter.setRenderHint(QPainter::SmoothPixmapTransform, true);
    
    // Scale painter to resolution
    painter.scale(clampedResolution, clampedResolution);
    
    // Translate to tile's scene position
    painter.translate(-tileSceneRect.topLeft());
    
    // No clipping needed - tiles have overlap to prevent gaps
    
    // Render text items that intersect this tile
    for (TextMediaItem* item : sortedItems) {
        if (!item || !item->isVisible()) {
            continue;
        }
        
        // Only render items that intersect this tile
        if (!tileSceneRect.intersects(item->sceneBoundingRect())) {
            continue;
        }
        
        painter.save();
        
        // Apply the item's scene transformation
        painter.setTransform(item->sceneTransform(), true);
        
        // Temporarily enable direct painting
        const bool wasDirectPainting = item->directPaintingEnabled();
        item->setDirectPaintingEnabled(true);
        
        // Render text item (clipped to tile bounds)
        QStyleOptionGraphicsItem opt;
        opt.state = QStyle::State_None;
        item->paint(&painter, &opt, nullptr);
        
        // Restore state
        item->setDirectPaintingEnabled(wasDirectPainting);
        
        painter.restore();
    }
    
    painter.end();
    
    // Update tile pixmap item
    tile.item->setPixmap(tilePixmap);
    tile.item->setPos(tileSceneRect.topLeft());
    
    // Scale the pixmap DOWN to scene coordinates (inverse of resolution)
    // The pixmap was rendered at clampedResolution, so scale it back down
    tile.item->setScale(1.0 / clampedResolution);
    
    tile.item->setVisible(true);
    tile.item->update(); // Force immediate visual update
    
    // Mark tile as clean and store zoom
    tile.dirty = false;
    tile.lastZoom = m_currentZoom;
}

void TextRasterizationLayer::cleanupUnusedTiles(const QRectF& viewportBounds) {
    // Tiles are now viewport-based, so cleanup is handled by updateTileGrid()
    // This function is kept for compatibility but does nothing
    Q_UNUSED(viewportBounds);
}

void TextRasterizationLayer::performRasterization() {
    if (!m_canvas || m_textItems.isEmpty() || !m_layerVisible) {
        // Clear all tiles if no content
        for (Tile& tile : m_tiles) {
            if (tile.item) {
                delete tile.item;
                tile.item = nullptr;
            }
        }
        m_tiles.clear();
        return;
    }
    
    // Update tile grid based on viewport (creates fixed number of tiles)
    updateTileGrid();
    
    // Calculate resolution for rendering
    const qreal resolution = computeRasterResolution();
    
    // Calculate bounds of all text items in scene coordinates
    const QRectF textBounds = calculateTextBounds();
    if (textBounds.isEmpty()) {
        // No text items visible - clear all tiles
        for (Tile& tile : m_tiles) {
            if (tile.item) {
                if (tile.item->pixmap().isNull() == false) {
                    tile.item->setPixmap(QPixmap()); // Clear to transparent
                    tile.item->setVisible(false);
                }
            }
        }
        return;
    }
    
    // Sort text items once by z-order
    QList<TextMediaItem*> sortedItems = m_textItems;
    std::sort(sortedItems.begin(), sortedItems.end(), 
              [](TextMediaItem* a, TextMediaItem* b) { 
                  return a->zValue() < b->zValue(); 
              });
    
    // Render each tile
    for (Tile& tile : m_tiles) {
        // Map viewport tile to scene coordinates to check if it intersects text
        const QPolygonF scenePolygon = m_canvas->mapToScene(tile.viewportRect);
        const QRectF tileSceneRect = scenePolygon.boundingRect();
        
        const bool intersectsText = textBounds.intersects(tileSceneRect);
        
        // If tile doesn't intersect text, clear it
        if (!intersectsText) {
            if (tile.item && !tile.item->pixmap().isNull()) {
                tile.item->setPixmap(QPixmap()); // Clear to transparent
                tile.item->setVisible(false);
            }
            continue;
        }
        
        // Check if tile needs updating (dirty flag or zoom changed)
        const bool zoomChanged = !qFuzzyCompare(tile.lastZoom, m_currentZoom);
        if (tile.dirty || zoomChanged) {
            renderTile(tile, sortedItems, resolution);
        }
    }
    
    // Update state
    m_lastRasterZoom = m_currentZoom;
    m_dirty = false;
}

void TextRasterizationLayer::rasterizeAllText() {
    // Force immediate rasterization (bypass debounce)
    m_rasterTimer->stop();
    performRasterization();
}

void TextRasterizationLayer::setLayerVisible(bool visible) {
    if (m_layerVisible == visible) {
        return;
    }
    
    m_layerVisible = visible;
    
    if (visible && m_dirty) {
        scheduleRasterization();
    } else if (!visible) {
        // Clear all tiles when hiding layer
        for (Tile& tile : m_tiles) {
            if (tile.item) {
                delete tile.item;
                tile.item = nullptr;
            }
        }
        m_tiles.clear();
        m_rasterTimer->stop();
    }
}
