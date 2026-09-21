#pragma once
#include "backend/media/ResidentMediaAsset.h"
#include <QObject>
#include <QPointer>
#include <QSet>
#include <QThreadPool>
#include <functional>
#include <memory>

struct SharedMediaFrame {
    QVideoFrame frame;
    std::shared_ptr<const QImage> thumbnail;
    quint64 bytes = 0;
    bool editingPreview = false;
};
using SharedMediaFramePtr = std::shared_ptr<const SharedMediaFrame>;

// Main-thread admission and priorities; bounded worker-owned FFmpeg contexts.
class DecodeScheduler final : public QObject {
    Q_OBJECT
public:
    enum Priority { Thumbnail = 0, VisibleThumbnail = 1, Prefetch = 2, Prepare = 3, Scrub = 4, Playback = 5 };
    using Completion = std::function<void(SharedMediaFramePtr, const QString&)>;
    static DecodeScheduler& instance();
    ~DecodeScheduler() override;
    int workerCount() const;
    void request(QObject* cursor, quint64 generation, std::shared_ptr<const ResidentMediaAsset> asset,
                 qint64 timestampUs, Priority priority, qint64 deadlineNs, Completion completion);
    void cancel(QObject* cursor, quint64 generation);
    SharedMediaFramePtr cached(const ResidentMediaAsset& asset, qint64 timestampUs);
    void requestThumbnail(QObject* owner, std::shared_ptr<const ResidentMediaAsset> asset,
                          qint64 timestampUs, std::function<void(std::shared_ptr<const QImage>)> completion,
                          bool visible = true);
    // Preserve useful subscriptions as the viewport moves; indices are source
    // frame indices, independent of the strip's current tile positions.
    void retainThumbnailRequests(QObject* owner, const ResidentMediaAsset& asset,
                                 const QSet<int>& desiredFrameIndices);
    QImage thumbnail(const ResidentMediaAsset& asset, qint64 timestampUs) const;
    void evictOptionalCaches();
    void setOptionalCachingEnabled(bool enabled);
    bool optionalCachingEnabled() const;
    quint64 optionalFrameBytes() const;
    quint64 thumbnailBytes() const;
    quint64 trackedThumbnailBytes() const;
    quint64 trackedFrameBytes() const;
    int pendingJobs() const;
    int runningThumbnailJobs() const;
    static constexpr quint64 FrameLimit = 64ULL * 1024 * 1024;
    static constexpr quint64 ThumbnailLimit = 32ULL * 1024 * 1024;
signals:
    void optionalCacheAvailabilityChanged();
    void optionalCachesCleared();
private:
    friend class VideoPlaybackBackendTest;
    // Deterministic integration tests delay only one occurrence's results;
    // real validation, other cursors and the scene clock continue normally.
    void holdCompletionsForTesting(QObject* owner, bool hold);
    DecodeScheduler();
    struct Impl;
    std::unique_ptr<Impl> d;
    void observeOwner(QObject* owner);
    void dispatch();
};
