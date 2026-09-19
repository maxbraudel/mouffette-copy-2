#include "frontend/rendering/canvas/TimelineThumbnailItem.h"
#include "backend/media/MediaResidencyManager.h"

#include <QQuickWindow>
#include <QSGImageNode>
#include <QSGTexture>
#include <QSet>
#include <algorithm>
#include <cmath>

namespace {
class StripNode final : public QSGNode {
public:
    ~StripNode() override {
        // Materials must die before the textures, on the render thread.
        while (auto* child = firstChild()) { removeChildNode(child); delete child; }
    }
    QHash<qint64, std::shared_ptr<QSGTexture>> textures;
};

const QImage& sample(const QVector<ResidentThumbnail>& frames, qreal timeUs) {
    auto next = std::lower_bound(frames.cbegin(), frames.cend(), timeUs,
        [](const ResidentThumbnail& frame, qreal time) { return frame.timestampUs < time; });
    if (next == frames.cbegin()) return next->image;
    if (next == frames.cend()) return frames.last().image;
    const auto previous = next - 1;
    return timeUs - previous->timestampUs <= next->timestampUs - timeUs ? previous->image : next->image;
}
}

TimelineThumbnailItem::TimelineThumbnailItem(QQuickItem* parent) : QQuickItem(parent) {
    setFlag(ItemHasContents, true);
    connect(&MediaResidencyManager::instance(), &MediaResidencyManager::ownerChanged,
            this, [this](const QString& owner) { if (owner == m_ownerId) refresh(); });
}

void TimelineThumbnailItem::setOwnerId(const QString& value) {
    if (m_ownerId == value) return;
    m_ownerId = value;
    refresh();
    emit ownerIdChanged();
}

void TimelineThumbnailItem::refresh() {
    const auto asset = MediaResidencyManager::instance().asset(m_ownerId);
    m_asset = asset;
    const bool ready = asset && !asset->thumbnails.isEmpty();
    if (m_hasThumbnails != ready) { m_hasThumbnails = ready; emit hasThumbnailsChanged(); }
    update();
}

void TimelineThumbnailItem::setSourceInMs(qreal value) {
    if (!std::isfinite(value) || m_sourceInMs == value) return;
    m_sourceInMs = value; update(); emit layoutChanged();
}
void TimelineThumbnailItem::setPixelsPerMs(qreal value) {
    if (!std::isfinite(value) || m_pixelsPerMs == value) return;
    m_pixelsPerMs = value; update(); emit layoutChanged();
}
void TimelineThumbnailItem::setVisibleLeft(qreal value) {
    if (!std::isfinite(value) || m_visibleLeft == value) return;
    m_visibleLeft = value; update(); emit layoutChanged();
}
void TimelineThumbnailItem::setVisibleRight(qreal value) {
    if (!std::isfinite(value) || m_visibleRight == value) return;
    m_visibleRight = value; update(); emit layoutChanged();
}
void TimelineThumbnailItem::geometryChange(const QRectF& geometry, const QRectF& oldGeometry) {
    QQuickItem::geometryChange(geometry, oldGeometry);
    if (geometry.size() != oldGeometry.size()) update();
}

QSGNode* TimelineThumbnailItem::updatePaintNode(QSGNode* oldNode, UpdatePaintNodeData*) {
    auto* node = static_cast<StripNode*>(oldNode);
    const auto asset = m_asset.lock();
    const qreal left = std::max(qreal(0), m_visibleLeft);
    const qreal right = std::min(width(), m_visibleRight);
    if (!window() || !asset || asset->thumbnails.isEmpty() || height() <= 0
        || right <= left || m_pixelsPerMs <= 0) {
        delete node;
        return nullptr;
    }
    if (!node) node = new StripNode;
    const qreal aspect = qreal(asset->displaySize.width()) / std::max(1, asset->displaySize.height());
    const qreal tileWidth = std::max(qreal(20), height() * std::clamp(aspect, qreal(0.5), qreal(2.4)));
    QSet<qint64> used;
    auto* child = node->firstChild();
    for (qreal x = std::floor(left / tileWidth) * tileWidth; x < right; x += tileWidth) {
        const auto& image = sample(asset->thumbnails, (m_sourceInMs + x / m_pixelsPerMs) * 1000);
        const auto key = image.cacheKey();
        if (!node->textures.contains(key)) {
            std::shared_ptr<QSGTexture> texture(window()->createTextureFromImage(image));
            if (!texture) continue;
            node->textures.insert(key, std::move(texture));
        }
        used.insert(key);
        auto* quad = static_cast<QSGImageNode*>(child);
        if (!quad) {
            quad = window()->createImageNode();
            quad->setFiltering(QSGTexture::Linear);
            node->appendChildNode(quad);
        }
        child = quad->nextSibling();
        quad->setTexture(node->textures.value(key).get());
        // Cover a tile without stretching. Crop extreme ratios, then clip the
        // source rect at the viewport/clip edges instead of squeezing a frame.
        const qreal scale = std::max(tileWidth / image.width(), height() / image.height());
        const qreal start = std::max(x, left), end = std::min(x + tileWidth, right);
        const qreal sourceX = (image.width() - tileWidth / scale) / 2;
        const qreal sourceY = (image.height() - height() / scale) / 2;
        quad->setSourceRect({sourceX + (start - x) / scale, sourceY, (end - start) / scale, height() / scale});
        quad->setRect({start, 0, end - start, height()});
    }
    while (child) {
        auto* next = child->nextSibling();
        node->removeChildNode(child); delete child; child = next;
    }
    for (auto it = node->textures.begin(); it != node->textures.end();) {
        if (!used.contains(it.key())) it = node->textures.erase(it);
        else ++it;
    }
    return node;
}
