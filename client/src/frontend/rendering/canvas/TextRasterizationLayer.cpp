#include "TextRasterizationLayer.h"
#include "ScreenCanvas.h"
#include "backend/domain/media/TextMediaItem.h"
#include <QPainter>
#include <QStyleOptionGraphicsItem>
#include <QDebug>
#include <QtConcurrent>
#include <QMutexLocker>
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
    , m_pendingRenders(0)
    , m_asyncRenderInProgress(false)
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
    // Cancel all pending async renders
    for (QFutureWatcher<QPixmap>* watcher : m_activeWatchers) {
        watcher->cancel();
        watcher->waitForFinished();
        delete watcher;
    }
    m_activeWatchers.clear();
    
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
    if (!qFuzzyCompare(m_rasterizationFactor, factor)) {
        m_rasterizationFactor = factor;
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
    const int viewportTilesX = (viewportRect.width() + TILE_SIZE - 1) / TILE_SIZE;
    const int viewportTilesY = (viewportRect.height() + TILE_SIZE - 1) / TILE_SIZE;
    
    // Add buffer tiles around viewport
    const int tilesX = viewportTilesX + (VIEWPORT_BUFFER_TILES * 2);
    const int tilesY = viewportTilesY + (VIEWPORT_BUFFER_TILES * 2);
    
    // Calculate starting offset for tiles (negative coordinates for buffer tiles)
    const int startTileX = -VIEWPORT_BUFFER_TILES;
    const int startTileY = -VIEWPORT_BUFFER_TILES;
    
    // Create map of existing tiles for quick lookup
    QMap<QPair<int, int>, Tile*> existingTiles;
    for (Tile& tile : m_tiles) {
        const int tileX = tile.viewportRect.x() / TILE_SIZE;
        const int tileY = tile.viewportRect.y() / TILE_SIZE;
        existingTiles[qMakePair(tileX, tileY)] = &tile;
    }
    
    // Create tiles to cover viewport + buffer
    QList<Tile> newTiles;
    for (int tileY = startTileY; tileY < startTileY + tilesY; ++tileY) {
        for (int tileX = startTileX; tileX < startTileX + tilesX; ++tileX) {
            auto key = qMakePair(tileX, tileY);
            
            if (existingTiles.contains(key)) {
                // Keep existing tile
                newTiles.append(*existingTiles[key]);
            } else {
                // Create new tile with overlap to prevent gaps during zoom
                // Tiles extend TILE_OVERLAP pixels into adjacent tiles
                const int x = tileX * TILE_SIZE;
                const int y = tileY * TILE_SIZE;
                const int width = TILE_SIZE + TILE_OVERLAP;
                const int height = TILE_SIZE + TILE_OVERLAP;
                
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
        const bool stillNeeded = (tileX >= startTileX && tileX < startTileX + tilesX &&
                                   tileY >= startTileY && tileY < startTileY + tilesY);
        
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
    
    // Calculate pixmap size based on scene rect dimensions and resolution
    // The pixmap should have enough pixels to render the scene area at the target resolution
    const QSize pixmapSize(
        static_cast<int>(std::ceil(tileSceneRect.width() * resolution)),
        static_cast<int>(std::ceil(tileSceneRect.height() * resolution))
    );
    
    // Safety check - ensure valid size
    if (pixmapSize.width() <= 0 || pixmapSize.height() <= 0) {
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
    painter.scale(resolution, resolution);
    
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
    // The pixmap was rendered at resolution, so scale it back down
    tile.item->setScale(1.0 / resolution);
    
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
    
    // Start async rendering if not already in progress
    if (!m_asyncRenderInProgress) {
        startAsyncRasterization();
    }
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
        // Cancel pending async renders
        for (QFutureWatcher<QPixmap>* watcher : m_activeWatchers) {
            watcher->cancel();
        }
        
        // Clear all tiles when hiding layer
        for (Tile& tile : m_tiles) {
            if (tile.item) {
                delete tile.item;
                tile.item = nullptr;
            }
        }
        m_tiles.clear();
        m_rasterTimer->stop();
        m_asyncRenderInProgress = false;
    }
}

// ============================================================================
// ASYNC RENDERING IMPLEMENTATION
// ============================================================================

void TextRasterizationLayer::startAsyncRasterization() {
    if (!m_canvas) {
        return;
    }
    
    QMutexLocker locker(&m_renderMutex);
    
    // Mark render in progress
    m_asyncRenderInProgress = true;
    
    // Calculate resolution for rendering
    const qreal resolution = computeRasterResolution();
    
    // Calculate bounds of all text items in scene coordinates
    const QRectF textBounds = calculateTextBounds();
    if (textBounds.isEmpty()) {
        // No text items visible - clear all tiles synchronously (fast)
        for (Tile& tile : m_tiles) {
            if (tile.item && !tile.item->pixmap().isNull()) {
                tile.item->setPixmap(QPixmap());
                tile.item->setVisible(false);
            }
        }
        m_asyncRenderInProgress = false;
        return;
    }
    
    // Prepare text data for async rendering (thread-safe copies)
    // Sort text items first by z-order, then copy their data
    QList<TextMediaItem*> sortedItems = m_textItems;
    std::sort(sortedItems.begin(), sortedItems.end(), 
              [](TextMediaItem* a, TextMediaItem* b) { 
                  return a->zValue() < b->zValue(); 
              });
    
    QList<TextItemRenderData> textItemsData;
    for (TextMediaItem* item : sortedItems) {
        if (!item || !item->isVisible()) {
            continue;
        }
        
        // Extract thread-safe data from text item
        TextItemRenderData itemData;
        itemData.sceneTransform = item->sceneTransform();
        itemData.sceneBoundingRect = item->sceneBoundingRect();
        itemData.text = item->text();
        itemData.font = item->font();
        itemData.textColor = item->textColor();
        itemData.borderWidthPercent = item->textBorderWidth();
        itemData.borderColor = item->textBorderColor();
        itemData.highlightEnabled = item->highlightEnabled();
        itemData.highlightColor = item->highlightColor();
        itemData.directPaintingEnabled = item->directPaintingEnabled();
        itemData.zValue = item->zValue();
        
        textItemsData.append(itemData);
    }
    
    // Submit tiles for async rendering
    int tilesSubmitted = 0;
    for (int i = 0; i < m_tiles.size(); ++i) {
        Tile& tile = m_tiles[i];
        
        // Skip if already rendering
        if (tile.rendering.loadAcquire() != 0) {
            continue;
        }
        
        // Map viewport tile to scene coordinates
        const QPolygonF scenePolygon = m_canvas->mapToScene(tile.viewportRect);
        const QRectF tileSceneRect = scenePolygon.boundingRect();
        
        const bool intersectsText = textBounds.intersects(tileSceneRect);
        
        // If tile doesn't intersect text, clear it synchronously (fast)
        if (!intersectsText) {
            if (tile.item && !tile.item->pixmap().isNull()) {
                tile.item->setPixmap(QPixmap());
                tile.item->setVisible(false);
            }
            continue;
        }
        
        // Check if tile needs updating
        const bool zoomChanged = !qFuzzyCompare(tile.lastZoom, m_currentZoom);
        if (!tile.dirty && !zoomChanged) {
            continue;
        }
        
        // Calculate pixmap size
        const QSize pixmapSize(
            static_cast<int>(std::ceil(tileSceneRect.width() * resolution)),
            static_cast<int>(std::ceil(tileSceneRect.height() * resolution))
        );
        
        if (pixmapSize.width() <= 0 || pixmapSize.height() <= 0) {
            continue;
        }
        
        // Filter text items for this tile (only items that intersect)
        QList<TextItemRenderData> tileTextItems;
        for (const TextItemRenderData& itemData : textItemsData) {
            if (tileSceneRect.intersects(itemData.sceneBoundingRect)) {
                tileTextItems.append(itemData);
            }
        }
        
        // Prepare render data (thread-safe)
        TileRenderData renderData;
        renderData.viewportRect = tile.viewportRect;
        renderData.sceneRect = tileSceneRect;
        renderData.pixmapSize = pixmapSize;
        renderData.resolution = resolution;
        renderData.textItems = tileTextItems;
        renderData.tileIndex = i;
        
        // Mark tile as rendering
        tile.rendering.storeRelease(1);
        
        // Create future watcher
        auto* watcher = new QFutureWatcher<QPixmap>(this);
        m_activeWatchers.append(watcher);
        
        // Connect completion signal
        connect(watcher, &QFutureWatcher<QPixmap>::finished, this, [this, i, tileSceneRect, resolution, watcher]() {
            if (!watcher->isCanceled()) {
                const QPixmap result = watcher->result();
                applyRenderedTile(i, result, tileSceneRect, resolution);
            }
            
            // Remove from active watchers
            m_activeWatchers.removeOne(watcher);
            watcher->deleteLater();
            
            // Decrement pending count
            m_pendingRenders.fetchAndAddRelaxed(-1);
            
            // Check if all tiles are done
            if (m_pendingRenders.loadAcquire() == 0 && m_activeWatchers.isEmpty()) {
                onAllTilesRendered();
            }
        });
        
        // Submit async render
        m_pendingRenders.fetchAndAddRelaxed(1);
        QFuture<QPixmap> future = QtConcurrent::run(renderTileAsync, renderData);
        watcher->setFuture(future);
        
        tilesSubmitted++;
    }
    
    // If no tiles were submitted, mark as complete
    if (tilesSubmitted == 0) {
        m_asyncRenderInProgress = false;
    }
}

QPixmap TextRasterizationLayer::renderTileAsync(const TileRenderData& data) {
    // This runs on a background thread - no Qt GUI operations allowed!
    
    QPixmap tilePixmap(data.pixmapSize);
    tilePixmap.fill(Qt::transparent);
    
    QPainter painter(&tilePixmap);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setRenderHint(QPainter::TextAntialiasing, true);
    painter.setRenderHint(QPainter::SmoothPixmapTransform, true);
    
    // Scale painter to resolution
    painter.scale(data.resolution, data.resolution);
    
    // Translate to tile's scene position
    painter.translate(-data.sceneRect.topLeft());
    
    // Render text items in z-order
    for (const TextItemRenderData& item : data.textItems) {
        if (item.text.isEmpty()) {
            continue;
        }
        
        painter.save();
        
        // Apply the item's scene transformation
        painter.setTransform(item.sceneTransform, true);
        
        // Get item's local bounding rect for rendering
        const QRectF localRect = item.sceneTransform.inverted().mapRect(item.sceneBoundingRect);
        
        // Setup text rendering
        QFont font = item.font;
        painter.setFont(font);
        
        // Draw highlight background if enabled
        if (item.highlightEnabled) {
            painter.fillRect(localRect, item.highlightColor);
        }
        
        // Setup text drawing options
        QTextOption textOption;
        textOption.setWrapMode(QTextOption::WrapAtWordBoundaryOrAnywhere);
        textOption.setAlignment(Qt::AlignLeft | Qt::AlignTop);
        
        // Draw text with border (stroke) if needed
        if (item.borderWidthPercent > 0.0) {
            const qreal borderWidth = item.font.pixelSize() * (item.borderWidthPercent / 100.0);
            
            // Draw border (stroke)
            QPen borderPen(item.borderColor);
            borderPen.setWidthF(borderWidth * 2.0); // Double width for proper outline
            borderPen.setJoinStyle(Qt::RoundJoin);
            
            QPainterPath path;
            path.addText(localRect.topLeft(), font, item.text);
            
            painter.strokePath(path, borderPen);
            painter.fillPath(path, item.textColor);
        } else {
            // Simple text rendering without border
            painter.setPen(item.textColor);
            painter.drawText(localRect, item.text, textOption);
        }
        
        painter.restore();
    }
    
    painter.end();
    
    return tilePixmap;
}

void TextRasterizationLayer::applyRenderedTile(int tileIndex, const QPixmap& pixmap, const QRectF& sceneRect, qreal resolution) {
    // Back on UI thread - safe to update graphics items
    
    if (tileIndex < 0 || tileIndex >= m_tiles.size()) {
        return;
    }
    
    Tile& tile = m_tiles[tileIndex];
    
    // Create or reuse tile pixmap item
    if (!tile.item) {
        tile.item = new QGraphicsPixmapItem(this);
        tile.item->setZValue(zValue());
    }
    
    // Update tile pixmap
    tile.item->setPixmap(pixmap);
    tile.item->setPos(sceneRect.topLeft());
    tile.item->setScale(1.0 / resolution);
    tile.item->setVisible(true);
    tile.item->update();
    
    // Mark tile as clean
    tile.dirty = false;
    tile.lastZoom = m_currentZoom;
    tile.rendering.storeRelease(0);
}

void TextRasterizationLayer::onAllTilesRendered() {
    // All async rendering complete
    m_asyncRenderInProgress = false;
    m_lastRasterZoom = m_currentZoom;
    m_dirty = false;
}
