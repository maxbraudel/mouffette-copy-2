#include "TextRasterizationLayer.h"
#include "ScreenCanvas.h"
#include "backend/domain/media/TextMediaItem.h"
#include <QPainter>
#include <QStyleOptionGraphicsItem>
#include <QDebug>
#include <QtConcurrent>
#include <QMutexLocker>
#include <QTextOption>
#include <QPainterPath>
#include <QImage>
#include <utility>
#include <cmath>
#include <algorithm>
#include <QPointF>

namespace {
// Aligns a scene-space rectangle so its edges land on exact pixel boundaries
// once multiplied by the raster resolution. This avoids sub-pixel rounding
// discrepancies between neighboring tiles that can manifest as seams.
QRectF alignRectToResolution(const QRectF& rect, qreal resolution) {
    if (resolution <= 0.0) {
        return rect;
    }

    const qreal invResolution = 1.0 / resolution;
    const qreal left = std::floor(rect.left() * resolution) * invResolution;
    const qreal top = std::floor(rect.top() * resolution) * invResolution;
    const qreal right = std::ceil((rect.left() + rect.width()) * resolution) * invResolution;
    const qreal bottom = std::ceil((rect.top() + rect.height()) * resolution) * invResolution;

    return QRectF(QPointF(left, top), QPointF(right, bottom));
}
} // namespace

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
    , m_generationCounter(0)
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
    for (QFutureWatcher<TileRenderResult>* watcher : m_activeWatchers) {
        watcher->cancel();
        watcher->waitForFinished();
        watcher->deleteLater();
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
    
    bool invertible = false;
    const QTransform invTransform = m_canvas->viewportTransform().inverted(&invertible);
    if (!invertible) {
        return;
    }

    QRectF tileSceneRect = invTransform.mapRect(QRectF(tile.viewportRect));
    tileSceneRect = alignRectToResolution(tileSceneRect, resolution);
    
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
    
    // Always request async rendering (generation tracking prevents stale results)
    startAsyncRasterization();
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
        for (QFutureWatcher<TileRenderResult>* watcher : m_activeWatchers) {
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

    const qreal resolution = computeRasterResolution();
    const QRectF textBounds = calculateTextBounds();

    if (textBounds.isEmpty()) {
        for (Tile& tile : m_tiles) {
            if (tile.item && !tile.item->pixmap().isNull()) {
                tile.item->setPixmap(QPixmap());
                tile.item->setVisible(false);
            }
            tile.activeJobs.storeRelaxed(0);
            tile.pendingGeneration = 0;
        }
        QMutexLocker locker(&m_renderMutex);
        m_asyncRenderInProgress = false;
        m_currentGeneration = 0;
        return;
    }

    QList<TextMediaItem*> sortedItems = m_textItems;
    std::sort(sortedItems.begin(), sortedItems.end(), [](TextMediaItem* a, TextMediaItem* b) {
        return a->zValue() < b->zValue();
    });

    QList<TextItemRenderData> textItemsData;
    textItemsData.reserve(sortedItems.size());
    for (TextMediaItem* item : sortedItems) {
        if (!item || !item->isVisible()) {
            continue;
        }

        TextItemRenderData data;
        data.sceneTransform = item->sceneTransform();
        data.sceneBoundingRect = item->sceneBoundingRect();
        data.text = item->text();
        data.font = item->font();
        data.textColor = item->textColor();
        data.borderWidthPercent = item->textBorderWidth();
        data.borderColor = item->textBorderColor();
        data.highlightEnabled = item->highlightEnabled();
        data.highlightColor = item->highlightColor();
        data.directPaintingEnabled = item->directPaintingEnabled();
        data.zValue = item->zValue();

        textItemsData.append(std::move(data));
    }

    auto textItemsShared = QSharedPointer<QList<TextItemRenderData>>::create();
    textItemsShared->swap(textItemsData);

    const quint64 generation = m_generationCounter.fetchAndAddRelaxed(1) + 1;
    {
        QMutexLocker locker(&m_renderMutex);
        m_asyncRenderInProgress = true;
        m_currentGeneration = generation;
    }

    GenerationState& generationState = m_generationStates[generation];
    generationState.pendingTiles = 0;
    generationState.results.clear();

    int tilesSubmitted = 0;
    for (int i = 0; i < m_tiles.size(); ++i) {
        Tile& tile = m_tiles[i];

        const QPolygonF scenePolygon = m_canvas->mapToScene(tile.viewportRect);
        const QRectF tileSceneRect = scenePolygon.boundingRect();

        if (!textBounds.intersects(tileSceneRect)) {
            if (tile.item && !tile.item->pixmap().isNull()) {
                tile.item->setPixmap(QPixmap());
                tile.item->setVisible(false);
            }
            tile.dirty = false;
            tile.lastZoom = m_currentZoom;
            tile.pendingGeneration = 0;
            continue;
        }

        const bool zoomChanged = !qFuzzyCompare(tile.lastZoom, m_currentZoom);
        if (!tile.dirty && !zoomChanged) {
            continue;
        }

        const QSize pixmapSize(
            static_cast<int>(std::ceil(tileSceneRect.width() * resolution)),
            static_cast<int>(std::ceil(tileSceneRect.height() * resolution))
        );

        if (pixmapSize.width() <= 0 || pixmapSize.height() <= 0) {
            continue;
        }

        TileRenderData renderData;
        renderData.viewportRect = tile.viewportRect;
        renderData.sceneRect = tileSceneRect;
        renderData.pixmapSize = pixmapSize;
        renderData.resolution = resolution;
        renderData.textItems = textItemsShared;
        renderData.tileIndex = i;
        renderData.generation = generation;

        tile.pendingGeneration = generation;
        tile.activeJobs.fetchAndAddRelaxed(1);

        ++generationState.pendingTiles;

        auto* watcher = new QFutureWatcher<TileRenderResult>(this);
        m_activeWatchers.append(watcher);

        const int tileIndex = i;
        const quint64 tileGeneration = generation;

        connect(watcher, &QFutureWatcher<TileRenderResult>::finished, this, [this, watcher, tileIndex, tileGeneration]() {
            const bool canceled = watcher->isCanceled();
            TileRenderResult result;
            if (!canceled) {
                result = watcher->result();
            } else {
                result.tileIndex = tileIndex;
                result.generation = tileGeneration;
                result.resolution = 1.0;
                result.hasContent = false;
            }
            handleRenderFinished(result, canceled);

            m_activeWatchers.removeOne(watcher);
            watcher->deleteLater();

            m_pendingRenders.fetchAndAddRelaxed(-1);
            if (m_pendingRenders.loadAcquire() == 0 && m_activeWatchers.isEmpty()) {
                onAllTilesRendered();
            }
        });

        m_pendingRenders.fetchAndAddRelaxed(1);
        watcher->setFuture(QtConcurrent::run(renderTileAsync, renderData));
        ++tilesSubmitted;
    }

    if (tilesSubmitted == 0) {
        m_generationStates.remove(generation);
        QMutexLocker locker(&m_renderMutex);
        m_asyncRenderInProgress = false;
    }
}

TextRasterizationLayer::TileRenderResult TextRasterizationLayer::renderTileAsync(const TileRenderData& data) {
    TileRenderResult result;
    result.tileIndex = data.tileIndex;
    result.sceneRect = data.sceneRect;
    result.resolution = data.resolution;
    result.generation = data.generation;

    QImage image(data.pixmapSize, QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::transparent);

    if (data.textItems && !data.textItems->isEmpty()) {
        QPainter painter(&image);
        painter.setRenderHint(QPainter::Antialiasing, true);
        painter.setRenderHint(QPainter::TextAntialiasing, true);
        painter.setRenderHint(QPainter::SmoothPixmapTransform, true);

        painter.scale(data.resolution, data.resolution);
        painter.translate(-data.sceneRect.topLeft());

        for (const TextItemRenderData& item : std::as_const(*data.textItems)) {
            if (item.text.isEmpty() || !data.sceneRect.intersects(item.sceneBoundingRect)) {
                continue;
            }

            painter.save();
            painter.setTransform(item.sceneTransform, true);

            const QRectF localRect = item.sceneTransform.inverted().mapRect(item.sceneBoundingRect);

            painter.setFont(item.font);

            if (item.highlightEnabled) {
                painter.fillRect(localRect, item.highlightColor);
            }

            QTextOption textOption;
            textOption.setWrapMode(QTextOption::WrapAtWordBoundaryOrAnywhere);
            textOption.setAlignment(Qt::AlignLeft | Qt::AlignTop);

            if (item.borderWidthPercent > 0.0) {
                const qreal borderWidth = item.font.pixelSize() * (item.borderWidthPercent / 100.0);

                QPen borderPen(item.borderColor);
                borderPen.setWidthF(borderWidth * 2.0);
                borderPen.setJoinStyle(Qt::RoundJoin);

                QPainterPath path;
                path.addText(localRect.topLeft(), item.font, item.text);

                painter.strokePath(path, borderPen);
                painter.fillPath(path, item.textColor);
            } else {
                painter.setPen(item.textColor);
                painter.drawText(localRect, item.text, textOption);
            }

            painter.restore();
            result.hasContent = true;
        }

        painter.end();
    }

    result.image = std::move(image);
    return result;
}

void TextRasterizationLayer::handleRenderFinished(const TileRenderResult& result, bool canceled) {
    if (result.generation == 0) {
        return;
    }

    if (result.tileIndex < 0 || result.tileIndex >= m_tiles.size()) {
        return;
    }

    Tile& tile = m_tiles[result.tileIndex];
    tile.activeJobs.fetchAndAddRelaxed(-1);

    auto stateIt = m_generationStates.find(result.generation);
    if (stateIt == m_generationStates.end()) {
        return;
    }

    GenerationState& state = stateIt.value();

    if (!canceled) {
        state.results.insert(result.tileIndex, result);
    }

    if (--state.pendingTiles <= 0) {
        const bool isCurrent = (result.generation == m_currentGeneration);
        QHash<int, TileRenderResult> generationResults = state.results;
        m_generationStates.erase(stateIt);

        if (isCurrent && !generationResults.isEmpty()) {
            applyGenerationResults(result.generation, generationResults);
        }
    }
}

void TextRasterizationLayer::applyGenerationResults(quint64 generation, const QHash<int, TileRenderResult>& results) {
    for (auto it = results.constBegin(); it != results.constEnd(); ++it) {
        const int tileIndex = it.key();
        if (tileIndex < 0 || tileIndex >= m_tiles.size()) {
            continue;
        }

        const TileRenderResult& result = it.value();
        Tile& tile = m_tiles[tileIndex];

        if (tile.pendingGeneration > generation) {
            continue;
        }

        tile.pendingGeneration = 0;

        if (!tile.item) {
            tile.item = new QGraphicsPixmapItem(this);
            tile.item->setZValue(zValue());
        }

        if (!result.hasContent || result.image.isNull()) {
            tile.item->setPixmap(QPixmap());
            tile.item->setVisible(false);
        } else {
            QPixmap pixmap = QPixmap::fromImage(result.image);
            pixmap.setDevicePixelRatio(result.resolution);
            tile.item->setPixmap(pixmap);
            tile.item->setPos(result.sceneRect.topLeft());
            tile.item->setScale(1.0);
            tile.item->setVisible(true);
            tile.item->update();
        }

        tile.dirty = false;
        tile.lastZoom = m_currentZoom;
    }
}

void TextRasterizationLayer::onAllTilesRendered() {
    m_asyncRenderInProgress = false;

    for (const Tile& tile : std::as_const(m_tiles)) {
        if (tile.dirty) {
            scheduleRasterization();
            return;
        }
    }

    m_lastRasterZoom = m_currentZoom;
    m_dirty = false;
}
