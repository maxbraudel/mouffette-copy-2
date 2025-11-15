#include "TextRasterizationLayer.h"
#include "ScreenCanvas.h"
#include "backend/domain/media/TextMediaItem.h"
#include <QPainter>
#include <QStyleOptionGraphicsItem>
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

void TextRasterizationLayer::performRasterization() {
    if (!m_canvas || m_textItems.isEmpty() || !m_layerVisible) {
        // Clear the layer if no content
        setPixmap(QPixmap());
        return;
    }
    
    const QRectF sceneBounds = calculateTextBounds();
    if (sceneBounds.isEmpty() || !sceneBounds.isValid()) {
        setPixmap(QPixmap());
        return;
    }
    
    const qreal resolution = computeRasterResolution();
    
    // Calculate pixmap size at target resolution
    const QSize pixmapSize(
        static_cast<int>(std::ceil(sceneBounds.width() * resolution)),
        static_cast<int>(std::ceil(sceneBounds.height() * resolution))
    );
    
    if (pixmapSize.width() <= 0 || pixmapSize.height() <= 0) {
        setPixmap(QPixmap());
        return;
    }
    
    // Create the composited text layer pixmap
    QPixmap textLayer(pixmapSize);
    textLayer.fill(Qt::transparent);
    
    QPainter painter(&textLayer);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setRenderHint(QPainter::TextAntialiasing, true);
    painter.setRenderHint(QPainter::SmoothPixmapTransform, true);
    
    // Scale painter to resolution for high-quality rendering
    painter.scale(resolution, resolution);
    
    // Translate to align with scene bounds
    painter.translate(-sceneBounds.topLeft());
    
    // Sort text items by z-order (bottom to top) to maintain visual stacking
    QList<TextMediaItem*> sortedItems = m_textItems;
    std::sort(sortedItems.begin(), sortedItems.end(), 
              [](TextMediaItem* a, TextMediaItem* b) { 
                  return a->zValue() < b->zValue(); 
              });
    
    // Render each text item in z-order
    for (TextMediaItem* item : sortedItems) {
        if (!item || !item->isVisible()) {
            continue;
        }
        
        painter.save();
        
        // Apply the item's scene transformation
        painter.setTransform(item->sceneTransform(), true);
        
        // Temporarily enable direct painting so the item renders to our layer
        const bool wasDirectPainting = item->directPaintingEnabled();
        item->setDirectPaintingEnabled(true);
        
        // Render text item using its existing paint logic
        QStyleOptionGraphicsItem opt;
        opt.state = QStyle::State_None;
        
        item->paint(&painter, &opt, nullptr);
        
        // Restore original direct painting state
        item->setDirectPaintingEnabled(wasDirectPainting);
        
        painter.restore();
    }
    
    painter.end();
    
    // Set the composited pixmap
    setPixmap(textLayer);
    
    // Position the layer at the scene bounds origin
    setPos(sceneBounds.topLeft());
    
    // Compensate for resolution scaling to display at correct size
    setScale(1.0 / resolution);
    
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
        // Clear pixmap when hiding layer
        setPixmap(QPixmap());
        m_rasterTimer->stop();
    }
}
