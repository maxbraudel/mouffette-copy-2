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
    , m_rasterizationFactor(0.5)
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
    
    // Immediately remove tiles that are outside text bounds to prevent ghosts
    const QRectF textBounds = calculateTextBounds();
    const QRectF visibleRect = getVisibleSceneRect();
    const QRectF neededBounds = textBounds.united(visibleRect);
    
    // Remove tiles outside the current needed area immediately
    for (int i = m_tiles.size() - 1; i >= 0; --i) {
        const QRectF tileRect(m_tiles[i].sceneRect);
        if (!neededBounds.intersects(tileRect)) {
            if (m_tiles[i].item) {
                // Hide and delete the tile immediately
                if (m_tiles[i].item->scene()) {
                    m_tiles[i].item->scene()->removeItem(m_tiles[i].item);
                }
                delete m_tiles[i].item;
            }
            m_tiles.removeAt(i);
        } else {
            // Mark remaining tiles as dirty
            m_tiles[i].dirty = true;
        }
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
    const QRectF textBounds = calculateTextBounds();
    const QRectF visibleRect = getVisibleSceneRect();
    const QRectF neededBounds = textBounds.united(visibleRect);
    
    if (neededBounds.isEmpty()) {
        return;
    }
    
    // Calculate tile grid dimensions
    const int minTileX = static_cast<int>(std::floor(neededBounds.left() / TILE_SIZE));
    const int maxTileX = static_cast<int>(std::ceil(neededBounds.right() / TILE_SIZE));
    const int minTileY = static_cast<int>(std::floor(neededBounds.top() / TILE_SIZE));
    const int maxTileY = static_cast<int>(std::ceil(neededBounds.bottom() / TILE_SIZE));
    
    // Create map of existing tiles for quick lookup
    QMap<QPair<int, int>, Tile*> existingTiles;
    for (Tile& tile : m_tiles) {
        const int tileX = tile.sceneRect.x() / TILE_SIZE;
        const int tileY = tile.sceneRect.y() / TILE_SIZE;
        existingTiles[qMakePair(tileX, tileY)] = &tile;
    }
    
    // Add new tiles as needed
    for (int tileY = minTileY; tileY < maxTileY; ++tileY) {
        for (int tileX = minTileX; tileX < maxTileX; ++tileX) {
            auto key = qMakePair(tileX, tileY);
            
            if (!existingTiles.contains(key)) {
                Tile newTile;
                newTile.sceneRect = QRect(tileX * TILE_SIZE, tileY * TILE_SIZE, TILE_SIZE, TILE_SIZE);
                newTile.dirty = true;
                newTile.item = nullptr;
                m_tiles.append(newTile);
            }
        }
    }
}

void TextRasterizationLayer::renderTile(Tile& tile, const QList<TextMediaItem*>& sortedItems, qreal resolution) {
    const QRectF tileSceneRect(tile.sceneRect);
    
    // Clamp resolution to prevent excessive memory usage
    const qreal clampedResolution = std::min(resolution, 10.0);
    
    // Calculate exact pixmap size for this tile
    const QSize pixmapSize(
        static_cast<int>(std::ceil(TILE_SIZE * clampedResolution)),
        static_cast<int>(std::ceil(TILE_SIZE * clampedResolution))
    );
    
    // Safety check - prevent creating huge pixmaps
    const int maxPixmapDimension = 8192; // 8K max
    if (pixmapSize.width() <= 0 || pixmapSize.height() <= 0 ||
        pixmapSize.width() > maxPixmapDimension || pixmapSize.height() > maxPixmapDimension) {
        qWarning() << "TextRasterizationLayer: Skipping tile - pixmap too large:" << pixmapSize 
                   << "resolution:" << resolution << "clamped:" << clampedResolution;
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
    
    // Expand clip rect slightly to prevent anti-aliasing seams
    // This renders a bit beyond tile bounds to ensure smooth edges
    QRectF expandedClip = tileSceneRect;
    expandedClip.adjust(-1, -1, 1, 1); // 1 pixel overlap for anti-aliasing
    painter.setClipRect(expandedClip);
    
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
    tile.item->setScale(1.0 / clampedResolution);
    tile.item->setVisible(true);
    tile.item->update(); // Force immediate visual update
    
    // Mark tile as clean and store resolution
    tile.dirty = false;
    tile.lastResolution = clampedResolution;
}

void TextRasterizationLayer::cleanupUnusedTiles(const QRectF& viewportBounds) {
    // Remove tiles that are far from viewport to save memory
    // With smaller tiles, keep fewer extra tiles around
    const qreal cleanupMargin = TILE_SIZE * 2.0; // Keep 2 tiles worth of margin
    QRectF keepBounds = viewportBounds;
    keepBounds.adjust(-cleanupMargin, -cleanupMargin, cleanupMargin, cleanupMargin);
    
    for (int i = m_tiles.size() - 1; i >= 0; --i) {
        const QRectF tileRect(m_tiles[i].sceneRect);
        
        // Remove tiles far from viewport
        if (!keepBounds.intersects(tileRect)) {
            if (m_tiles[i].item) {
                delete m_tiles[i].item;
            }
            m_tiles.removeAt(i);
        }
    }
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
    
    // Get visible viewport area
    const QRectF visibleRect = getVisibleSceneRect();
    const qreal resolution = computeRasterResolution();
    
    // Calculate bounds of all text items
    const QRectF textBounds = calculateTextBounds();
    if (textBounds.isEmpty()) {
        // No text items visible - clear all tiles
        for (Tile& tile : m_tiles) {
            if (tile.item) {
                delete tile.item;
                tile.item = nullptr;
            }
        }
        m_tiles.clear();
        return;
    }
    
    // Expand bounds to include viewport with margin
    QRectF neededBounds = textBounds.united(visibleRect);
    
    // Remove tiles outside needed bounds first to clean up ghosts
    for (int i = m_tiles.size() - 1; i >= 0; --i) {
        const QRectF tileRect(m_tiles[i].sceneRect);
        if (!neededBounds.intersects(tileRect)) {
            if (m_tiles[i].item) {
                delete m_tiles[i].item;
            }
            m_tiles.removeAt(i);
        }
    }
    
    // Update tile grid to cover needed bounds
    updateTileGrid();
    
    // Sort text items once by z-order
    QList<TextMediaItem*> sortedItems = m_textItems;
    std::sort(sortedItems.begin(), sortedItems.end(), 
              [](TextMediaItem* a, TextMediaItem* b) { 
                  return a->zValue() < b->zValue(); 
              });
    
    // Get tiles that need updating
    for (Tile& tile : m_tiles) {
        const QRectF tileSceneRect(tile.sceneRect);
        
        // Only update tiles in viewport OR dirty tiles anywhere
        const bool inViewport = visibleRect.intersects(tileSceneRect);
        const bool intersectsText = textBounds.intersects(tileSceneRect);
        
        // If tile doesn't intersect text anymore, clear it immediately
        if (!intersectsText && tile.item) {
            tile.item->setPixmap(QPixmap()); // Clear to transparent
            tile.item->setVisible(false);
            continue;
        }
        
        // CRITICAL: Only update resolution for tiles IN VIEWPORT
        // Tiles outside viewport keep their last resolution (showing old pixelation)
        if (inViewport && intersectsText) {
            // Always render viewport tiles at current resolution
            renderTile(tile, sortedItems, resolution);
        } else if (tile.dirty && intersectsText) {
            // Render dirty tiles outside viewport at their last resolution
            // This handles content changes without updating resolution
            const qreal tileResolution = tile.lastResolution > 0 ? tile.lastResolution : resolution;
            renderTile(tile, sortedItems, tileResolution);
        }
    }
    
    // Clean up tiles far from viewport to save memory
    cleanupUnusedTiles(visibleRect);
    
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
