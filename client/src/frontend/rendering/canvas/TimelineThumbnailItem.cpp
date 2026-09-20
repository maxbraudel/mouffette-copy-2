#include "frontend/rendering/canvas/TimelineThumbnailItem.h"
#include "backend/media/MediaResidencyManager.h"
#include "backend/media/DecodeScheduler.h"
#include "backend/media/IndexedMediaDecoder.h"
#include "shared/rendering/SharedImageTexture.h"

#include <QQuickWindow>
#include <QSGImageNode>
#include <QSGTexture>
#include <algorithm>
#include <cmath>
#include <limits>

namespace {
class StripNode final : public QSGNode {
public:
    ~StripNode() override {
        // Materials must die before the textures, on the render thread.
        while (auto* child = firstChild()) { removeChildNode(child); delete child; }
    }
    QHash<qint64, std::shared_ptr<QSGTexture>> textures;
};
qreal preferredWidth(qreal height, const QSize& size) {
    const qreal aspect = qreal(size.width()) / std::max(1, size.height());
    return std::max(qreal(20), height * std::clamp(aspect, qreal(.5), qreal(2.4)));
}
}

TimelineThumbnailItem::TimelineThumbnailItem(QQuickItem* parent) : QQuickItem(parent) {
    setFlag(ItemHasContents, true);
    m_requestTimer.setSingleShot(true);
    connect(&m_requestTimer, &QTimer::timeout, this, &TimelineThumbnailItem::requestVisible);
    connect(this, &TimelineThumbnailItem::layoutChanged, this, [this] {
        rebuildLayout(); m_requestTimer.start(0);
    });
    connect(&DecodeScheduler::instance(), &DecodeScheduler::optionalCachesCleared, this, [this] {
        clearRequests();
        m_samples.clear(); m_failedSamples.clear();
        // A normal LRU purge must not erase what is on screen. Critical memory
        // pressure disables optional caching first, and releases these pins too.
        if (!DecodeScheduler::instance().optionalCachingEnabled()) m_tiles.clear();
        rebuildLayout();
        if (DecodeScheduler::instance().optionalCachingEnabled()) m_requestTimer.start(0);
    });
    connect(&DecodeScheduler::instance(), &DecodeScheduler::optionalCacheAvailabilityChanged, this, [this] {
        rebuildLayout();
        if (DecodeScheduler::instance().optionalCachingEnabled()) m_requestTimer.start(0);
        else m_requestTimer.stop();
    });
    connect(&MediaResidencyManager::instance(), &MediaResidencyManager::ownerChanged,
            this, [this](const QString& owner) { if (owner == m_ownerId) refresh(); });
}

TimelineThumbnailItem::~TimelineThumbnailItem() {
    DecodeScheduler::instance().cancel(this, 0);
}

void TimelineThumbnailItem::clearRequests() {
    DecodeScheduler::instance().cancel(this, 0);
    ++m_generation;
    m_pendingSamples.clear(); m_pendingVisibleSamples.clear();
}

void TimelineThumbnailItem::setOwnerId(const QString& value) {
    if (m_ownerId == value) return;
    clearRequests(); m_samples.clear(); m_tiles.clear(); m_failedSamples.clear();
    m_stepMs = 0;
    m_sourceHash.clear();
    m_ownerId = value;
    refresh();
    emit ownerIdChanged();
}

void TimelineThumbnailItem::refresh() {
    auto& manager = MediaResidencyManager::instance();
    const auto asset = manager.asset(m_ownerId);
    const auto preview = manager.preview(m_ownerId);
    if (m_asset.lock() != asset) {
        clearRequests(); m_samples.clear(); m_failedSamples.clear();
    }
    // A removed, failed or invalidated source must never retain its old image.
    const QString hash = asset ? asset->sha256 : preview ? preview->sha256 : QString();
    if ((!asset && !preview) || (!m_sourceHash.isEmpty() && m_sourceHash != hash)) m_tiles.clear();
    m_sourceHash = hash;
    m_asset = asset;
    m_preview = preview;
    rebuildLayout();
    m_requestTimer.start(0);
}

void TimelineThumbnailItem::setSourceInMs(qreal value) {
    if (!std::isfinite(value) || m_sourceInMs == value) return;
    m_sourceInMs = value; emit layoutChanged();
}
void TimelineThumbnailItem::setPixelsPerMs(qreal value) {
    if (!std::isfinite(value) || m_pixelsPerMs == value) return;
    m_pixelsPerMs = value; emit layoutChanged();
}
void TimelineThumbnailItem::setVisibleLeft(qreal value) {
    if (!std::isfinite(value) || m_visibleLeft == value) return;
    m_visibleLeft = value; emit layoutChanged();
}
void TimelineThumbnailItem::setVisibleRight(qreal value) {
    if (!std::isfinite(value) || m_visibleRight == value) return;
    m_visibleRight = value; emit layoutChanged();
}
void TimelineThumbnailItem::geometryChange(const QRectF& geometry, const QRectF& oldGeometry) {
    QQuickItem::geometryChange(geometry, oldGeometry);
    if (geometry.size() != oldGeometry.size()) { rebuildLayout(); m_requestTimer.start(0); }
}

QVector<TimelineThumbnailItem::Tile> TimelineThumbnailItem::cells(
        qreal left, qreal right, const ResidentMediaAsset* asset, bool video, const QSize& size) const {
    QVector<Tile> result;
    if (right <= left || m_pixelsPerMs <= 0 || height() <= 0) return result;
    const qreal cellWidth = video ? m_stepMs * m_pixelsPerMs : preferredWidth(height(), size);
    if (!std::isfinite(cellWidth) || cellWidth <= 0) return result;
    const qreal firstTimeMs = video ? std::floor((m_sourceInMs + left / m_pixelsPerMs) / m_stepMs) * m_stepMs : 0;
    const qreal firstX = video ? (firstTimeMs - m_sourceInMs) * m_pixelsPerMs : std::floor(left / cellWidth) * cellWidth;
    // A source-time cell is also its complete screen interval. Keeping the old
    // fixed pixel width here would leave gaps whenever the zoom changes.
    const int count = int(std::ceil((right - firstX) / cellWidth));
    result.reserve(count);
    for (int n = 0; n < count; ++n) {
        Tile tile;
        tile.x = firstX + n * cellWidth; tile.width = cellWidth;
        tile.timeUs = qRound64((video ? firstTimeMs + n * m_stepMs : m_sourceInMs + tile.x / m_pixelsPerMs) * 1000);
        tile.frameIndex = asset && video ? IndexedMediaDecoder::frameAt(*asset, tile.timeUs) : -1;
        result.append(std::move(tile));
    }
    return result;
}

quint64 TimelineThumbnailItem::retainedThumbnailBytes() const {
    QSet<qint64> images;
    quint64 bytes = 0;
    for (const auto& tile : m_tiles) if (tile.image && !images.contains(tile.image->cacheKey())) {
        images.insert(tile.image->cacheKey()); bytes += tile.image->sizeInBytes();
    }
    return bytes;
}

void TimelineThumbnailItem::rebuildLayout() {
    const auto asset = m_asset.lock();
    const auto preview = m_preview.lock();
    const bool video = !asset || asset->video;
    const QSize size = asset ? asset->displaySize : preview ? preview->displaySize : QSize();
    const qreal left = std::max(qreal(0), m_visibleLeft), right = std::min(width(), m_visibleRight);
    if ((!asset && !preview) || right <= left || height() <= 0 || m_pixelsPerMs <= 0
            || (video && !DecodeScheduler::instance().optionalCachingEnabled())) {
        m_tiles.clear();
    } else {
        const qreal target = preferredWidth(height(), size);
        if (video) {
            if (m_stepMs <= 0) m_stepMs = 1000 * std::exp2(std::round(std::log2(target / m_pixelsPerMs / 1000)));
            // Overlapping thresholds retain the level during small reversals.
            while (m_stepMs * m_pixelsPerMs < target * .65) m_stepMs *= 2;
            while (m_stepMs * m_pixelsPerMs > target * 1.6) m_stepMs /= 2;
        }
        auto next = cells(left, right, asset.get(), video, size);
        QVector<Tile> candidates = m_tiles;
        QHash<int, Tile> previousTargets;
        for (const auto& tile : m_tiles) if (tile.image && tile.frameIndex >= 0) previousTargets.insert(tile.frameIndex, tile);
        const auto& prepared = asset ? asset->thumbnails : preview->thumbnails;
        for (const auto& frame : prepared) {
            if (frame.image.isNull()) continue;
            Tile candidate;
            candidate.image = std::make_shared<const QImage>(frame.image);
            candidate.imageTimeUs = frame.timestampUs;
            candidate.imageFrameIndex = asset && video ? IndexedMediaDecoder::frameAt(*asset, frame.timestampUs) : -1;
            candidates.append(std::move(candidate));
        }
        for (auto it = m_samples.cbegin(); it != m_samples.cend(); ++it) if (auto image = it.value().lock()) {
            if (!asset || it.key() < 0 || it.key() >= asset->frameIndex.size()) continue;
            Tile candidate;
            candidate.image = std::move(image); candidate.imageFrameIndex = it.key();
            candidate.imageTimeUs = asset->frameIndex[it.key()].timestampUs;
            candidates.append(std::move(candidate));
        }
        const auto nearest = [](const QVector<Tile>& choices, qint64 time) -> const Tile* {
            const Tile* best = nullptr;
            double distance = std::numeric_limits<double>::infinity();
            for (const auto& choice : choices) if (choice.image) {
                const double d = std::abs(double(choice.imageTimeUs) - double(time));
                if (d < distance) { best = &choice; distance = d; }
            }
            return best;
        };
        QSet<qint64> pinned;
        QVector<Tile> accepted;
        quint64 bytes = 0;
        for (auto& tile : next) {
            const qint64 targetTime = asset && tile.frameIndex >= 0 ? asset->frameIndex[tile.frameIndex].timestampUs : tile.timeUs;
            if (tile.frameIndex >= 0) {
                tile.image = m_samples.value(tile.frameIndex).lock();
                if (tile.image) { tile.imageFrameIndex = tile.frameIndex; tile.imageTimeUs = targetTime; }
            }
            if (!tile.image) {
                // Keep this cell's previous image until its exact replacement is
                // ready; other completed cells must not make it flicker again.
                const auto previous = previousTargets.constFind(tile.frameIndex);
                const Tile* fallback = previous != previousTargets.cend() ? &previous.value() : nearest(candidates, targetTime);
                if (fallback) {
                    tile.image = fallback->image; tile.imageTimeUs = fallback->imageTimeUs;
                    tile.imageFrameIndex = fallback->imageFrameIndex;
                }
            }
            if (!tile.image) continue;
            const auto key = tile.image->cacheKey();
            if (video && !pinned.contains(key) && bytes + quint64(tile.image->sizeInBytes()) > VisibleImageLimit) {
                const auto* fallback = nearest(accepted, targetTime);
                if (fallback) {
                    tile.image = fallback->image; tile.imageTimeUs = fallback->imageTimeUs;
                    tile.imageFrameIndex = fallback->imageFrameIndex;
                } else tile.image.reset();
            }
            if (!tile.image) continue;
            if (!pinned.contains(tile.image->cacheKey())) {
                pinned.insert(tile.image->cacheKey()); bytes += tile.image->sizeInBytes(); accepted.append(tile);
            }
            if (tile.imageFrameIndex >= 0) m_samples.insert(tile.imageFrameIndex, tile.image);
        }
        m_tiles = std::move(next);
    }
    const bool ready = std::any_of(m_tiles.cbegin(), m_tiles.cend(), [](const auto& tile) { return bool(tile.image); });
    if (m_hasThumbnails != ready) { m_hasThumbnails = ready; emit hasThumbnailsChanged(); }
    update();
}

QSGNode* TimelineThumbnailItem::updatePaintNode(QSGNode* oldNode, UpdatePaintNodeData*) {
    auto* node = static_cast<StripNode*>(oldNode);
    if (!window() || m_tiles.isEmpty()) { delete node; return nullptr; }
    if (!node) node = new StripNode;
    const qreal left = std::max(qreal(0), m_visibleLeft), right = std::min(width(), m_visibleRight);
    QSet<qint64> used;
    auto* child = node->firstChild();
    for (const auto& tile : m_tiles) {
        if (!tile.image || tile.image->isNull()) continue;
        const auto& image = *tile.image;
        const auto key = image.cacheKey();
        if (!node->textures.contains(key)) {
            auto texture = sharedImageTexture(window(), image);
            if (!texture) continue;
            node->textures.insert(key, std::move(texture));
        }
        used.insert(key);
        auto* quad = static_cast<QSGImageNode*>(child);
        const bool append = !quad;
        if (!quad) { quad = window()->createImageNode(); quad->setFiltering(QSGTexture::Linear); }
        quad->setTexture(node->textures.value(key).get());
        const qreal scale = std::max(tile.width / image.width(), height() / image.height());
        const qreal start = std::max(tile.x, left), end = std::min(tile.x + tile.width, right);
        const qreal sourceX = (image.width() - tile.width / scale) / 2;
        const qreal sourceY = (image.height() - height() / scale) / 2;
        quad->setSourceRect({sourceX + (start - tile.x) / scale, sourceY, (end - start) / scale, height() / scale});
        quad->setRect({start, 0, end - start, height()});
        if (append) node->appendChildNode(quad);
        child = quad->nextSibling();
    }
    while (child) { auto* next = child->nextSibling(); node->removeChildNode(child); delete child; child = next; }
    for (auto it = node->textures.begin(); it != node->textures.end();) {
        if (!used.contains(it.key())) it = node->textures.erase(it); else ++it;
    }
    return node;
}

void TimelineThumbnailItem::requestVisible() {
    const auto asset = m_asset.lock();
    if (!asset || !asset->video || !DecodeScheduler::instance().optionalCachingEnabled()) return;
    rebuildLayout();
    const qreal left = std::max(qreal(0), m_visibleLeft), right = std::min(width(), m_visibleRight);
    QSet<int> visible, desired;
    for (const auto& tile : m_tiles) if (tile.frameIndex >= 0) visible.insert(tile.frameIndex);
    const auto nearby = cells(std::max(qreal(0), left - (right - left)), std::min(width(), right + (right - left)),
                              asset.get(), true, asset->displaySize);
    for (const auto& tile : nearby) if (tile.frameIndex >= 0) desired.insert(tile.frameIndex);
    auto& scheduler = DecodeScheduler::instance();
    scheduler.retainThumbnailRequests(this, *asset, desired);
    m_pendingSamples.intersect(desired); m_pendingVisibleSamples.intersect(desired); m_failedSamples.intersect(desired);
    QSet<int> retained = desired;
    for (const auto& tile : m_tiles) if (tile.imageFrameIndex >= 0) retained.insert(tile.imageFrameIndex);
    for (auto it = m_samples.begin(); it != m_samples.end();) {
        if (!retained.contains(it.key()) || it.value().expired()) it = m_samples.erase(it); else ++it;
    }
    auto ordered = desired.values();
    std::sort(ordered.begin(), ordered.end(), [&visible](int a, int b) {
        if (visible.contains(a) != visible.contains(b)) return visible.contains(a);
        return a < b;
    });
    const auto generation = m_generation;
    for (int index : ordered) {
        const bool onscreen = visible.contains(index);
        if (!m_samples.value(index).expired() || m_failedSamples.contains(index)) continue;
        if (m_pendingSamples.contains(index) && (!onscreen || m_pendingVisibleSamples.contains(index))) continue;
        m_pendingSamples.insert(index);
        if (onscreen) m_pendingVisibleSamples.insert(index);
        scheduler.requestThumbnail(this, asset, asset->frameIndex[index].timestampUs,
            [this, generation, index](Image image) {
                if (generation != m_generation) return;
                m_pendingSamples.remove(index); m_pendingVisibleSamples.remove(index);
                if (!image || image->isNull()) m_failedSamples.insert(index);
                else m_samples.insert(index, std::move(image));
                rebuildLayout();
            }, onscreen);
    }
}
