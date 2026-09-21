#include "backend/media/DecodeScheduler.h"
#include "backend/media/IndexedMediaDecoder.h"
#include "backend/media/ResidentVideoPlayer.h"
#include "backend/media/MediaPreviewStore.h"
#include <QFutureWatcher>
#include <QThread>
#include <QTransform>
#include <QtConcurrent/QtConcurrentRun>
#include <atomic>
#include <limits>
#include <tuple>
#include <vector>

namespace {
std::atomic<quint64> liveThumbnailBytes{0};
std::shared_ptr<const QImage> trackedThumbnail(const QImage& image) {
    const quint64 bytes = image.sizeInBytes();
    auto* stored = new QImage(image);
    liveThumbnailBytes.fetch_add(bytes, std::memory_order_relaxed);
    return std::shared_ptr<const QImage>(stored, [bytes](const QImage* value) {
        delete value;
        liveThumbnailBytes.fetch_sub(bytes, std::memory_order_relaxed);
    });
}
QString key(const ResidentMediaAsset& asset, int index) {
    // Immutable asset identity, not the occurrence identity.
    return asset.sha256 + QLatin1Char(':') + QString::number(index);
}
struct Listener { QPointer<QObject> owner; quint64 generation; DecodeScheduler::Completion completion; };
struct Job {
    QString key;
    std::shared_ptr<const ResidentMediaAsset> asset;
    int index;
    DecodeScheduler::Priority priority;
    qint64 deadline;
    std::vector<Listener> listeners;
    bool running = false;
    bool thumbnail = false;
    std::shared_ptr<std::atomic_bool> cancelled = std::make_shared<std::atomic_bool>(false);
    SharedMediaFramePtr cachedFrame;
};
struct CacheFrame { SharedMediaFramePtr frame; quint64 use = 0; };
struct CacheThumbnail { std::shared_ptr<const QImage> image; quint64 use = 0; };
struct DecoderPosition {
    QString assetIdentity;
    qint64 timestampUs = -1;
    bool original = false;
    // The pool may retire an idle thread (and its FFmpeg context). A remembered
    // timestamp is useful only while that exact thread-local session survives.
    std::weak_ptr<const int> lifetime;
};
struct DecoderSession {
    IndexedMediaDecoder decoder;
    std::shared_ptr<const int> lifetime = std::make_shared<const int>(0);
    DecoderPosition position{{}, -1, false, lifetime};
    void decoded(const ResidentMediaAsset& asset, int index, bool success) {
        if (success) {
            position.assetIdentity = asset.sha256;
            position.timestampUs = asset.frameIndex[index].timestampUs;
            position.original = !asset.allIntra;
        } else {
            // A failed/cancelled demux can reset or partially advance FFmpeg.
            // Do not advertise its previous frame as a reusable predecessor.
            position.assetIdentity.clear(); position.timestampUs = -1; position.original = false;
        }
    }
};
struct DecodeResult {
    SharedMediaFramePtr frame;
    QString error;
    DecoderPosition position;
};
std::tuple<int, qint64> affinity(const DecoderPosition& position, const Job& job) {
    if (!position.original || position.lifetime.expired() || position.timestampUs < 0
            || position.assetIdentity != job.asset->sha256 || job.asset->allIntra)
        return {3, std::numeric_limits<qint64>::max()};
    const qint64 target = job.asset->frameIndex[job.index].timestampUs;
    if (position.timestampUs < target) {
        const qint64 distance = target - position.timestampUs;
        // IndexedMediaDecoder continues only forward requests within one
        // second; beyond that it seeks to a GOP even on a warm source session.
        return {distance <= 1000000 ? 0 : 1, distance};
    }
    return {2, position.timestampUs - target};
}
}
struct DecodeScheduler::Impl {
    struct Worker {
        QThreadPool pool;
        DecoderPosition position;
        bool busy = false;
        Worker() { pool.setMaxThreadCount(1); pool.setExpiryTimeout(1000); }
    };
    std::vector<std::unique_ptr<Worker>> workers;
    QSet<QObject*> owners;
    struct HeldCompletion { quint64 generation; std::function<void()> deliver; };
    QHash<QObject*, QList<HeldCompletion>> heldCompletions;
    QHash<QString, std::shared_ptr<Job>> jobs;
    QHash<QString, CacheFrame> frames;
    QHash<QString, std::weak_ptr<const SharedMediaFrame>> live;
    QHash<QString, CacheThumbnail> thumbnails;
    quint64 frameBytes = 0, thumbnailBytes = 0, sequence = 0;
    int running = 0, runningThumbnails = 0, runningOptional = 0;
    bool optionalEnabled = true;
    void trim() {
        while (frameBytes > FrameLimit && !frames.isEmpty()) {
            auto oldest = frames.begin();
            for (auto it = frames.begin(); it != frames.end(); ++it) if (it->use < oldest->use) oldest = it;
            frameBytes -= oldest->frame->bytes; frames.erase(oldest);
        }
        while (thumbnailBytes > ThumbnailLimit && !thumbnails.isEmpty()) {
            auto oldest = thumbnails.begin();
            for (auto it = thumbnails.begin(); it != thumbnails.end(); ++it) if (it->use < oldest->use) oldest = it;
            thumbnailBytes -= oldest->image->sizeInBytes(); thumbnails.erase(oldest);
        }
        for (auto it = live.begin(); it != live.end();) {
            if (it.value().expired()) it = live.erase(it); else ++it;
        }
    }
};
DecodeScheduler& DecodeScheduler::instance() { static DecodeScheduler scheduler; return scheduler; }
DecodeScheduler::DecodeScheduler() : d(std::make_unique<Impl>()) {
    const int count = std::max(1, std::min(4, QThread::idealThreadCount()/2));
    for (int i = 0; i < count; ++i) d->workers.push_back(std::make_unique<Impl::Worker>());
}
DecodeScheduler::~DecodeScheduler() {
    for (const auto& job : std::as_const(d->jobs)) job->cancelled->store(true);
    for (const auto& worker : d->workers) worker->pool.waitForDone();
}
int DecodeScheduler::workerCount() const { return int(d->workers.size()); }
void DecodeScheduler::observeOwner(QObject* owner) {
    if (d->owners.contains(owner)) return;
    d->owners.insert(owner);
    connect(owner, &QObject::destroyed, this, [this, owner] {
        d->owners.remove(owner);
        d->heldCompletions.remove(owner);
        cancel(nullptr, 0); // QPointer listeners are already null at this point.
    });
}
SharedMediaFramePtr DecodeScheduler::cached(const ResidentMediaAsset& asset, qint64 timestampUs) {
    int index = IndexedMediaDecoder::frameAt(asset, timestampUs);
    QString identity = key(asset, index);
    auto frame = d->live.value(identity).lock();
    if (!frame && index == 0 && asset.firstFrame.frame.isValid()) {
        auto initial = std::make_shared<SharedMediaFrame>();
        initial->frame = asset.firstFrame.frame;
        // The same allocation is already included in the asset's prepared frame.
        initial->bytes = 0;
        frame = initial; d->live.insert(identity, frame);
    }
    auto it = d->frames.find(identity);
    if (it != d->frames.end()) it->use = ++d->sequence;
    return frame;
}
void DecodeScheduler::request(QObject* cursor, quint64 generation, std::shared_ptr<const ResidentMediaAsset> asset,
                              qint64 timestampUs, Priority priority, qint64 deadline, Completion completion) {
    Q_ASSERT(QThread::currentThread() == thread());
    if (!cursor || !asset) return;
    observeOwner(cursor);
    if (d->heldCompletions.contains(cursor)) {
        completion = [this, owner = QPointer<QObject>(cursor), generation,
                      original = std::move(completion)](SharedMediaFramePtr frame, const QString& error) {
            if (!owner) return;
            auto deliver = [owner, original, frame = std::move(frame), error]() {
                if (owner) original(frame, error);
            };
            const auto held = d->heldCompletions.find(owner);
            if (held != d->heldCompletions.end()) held->append({generation, std::move(deliver)});
            else deliver();
        };
    }
    if (auto frame = cached(*asset, timestampUs)) { completion(std::move(frame), {}); return; }
    int index = IndexedMediaDecoder::frameAt(*asset, timestampUs);
    if (index < 0) { completion({}, QStringLiteral("Missing video frame index")); return; }
    QString identity = (priority == Scrub ? QStringLiteral("scrub:") : QString()) + key(*asset, index);
    if (priority == Scrub) {
        if (auto frame = d->live.value(identity).lock()) {
            auto it = d->frames.find(identity);
            if (it != d->frames.end()) it->use = ++d->sequence;
            completion(std::move(frame), {}); return;
        }
    }
    auto job = d->jobs.value(identity);
    if (!job) {
        job = std::make_shared<Job>(Job{identity, std::move(asset), index, priority, deadline, {}, false});
        d->jobs.insert(identity, job);
    }
    job->priority = std::max(job->priority, priority);
    if (deadline > 0 && (job->deadline <= 0 || deadline < job->deadline)) job->deadline = deadline;
    job->listeners.push_back({cursor, generation, std::move(completion)});
    dispatch();
}
void DecodeScheduler::cancel(QObject* cursor, quint64 generation) {
    auto held = d->heldCompletions.find(cursor);
    if (held != d->heldCompletions.end()) {
        auto& completions = held.value();
        completions.erase(std::remove_if(completions.begin(), completions.end(),
            [generation](const auto& result) { return result.generation == generation; }), completions.end());
    }
    for (auto it = d->jobs.begin(); it != d->jobs.end();) {
        auto& listeners = it.value()->listeners;
        listeners.erase(std::remove_if(listeners.begin(), listeners.end(), [=](const auto& listener) { return !listener.owner || (listener.owner == cursor && listener.generation == generation); }), listeners.end());
        if (listeners.empty()) {
            it.value()->cancelled->store(true, std::memory_order_relaxed);
            it = d->jobs.erase(it);
        } else ++it;
    }
    dispatch();
}
void DecodeScheduler::holdCompletionsForTesting(QObject* owner, bool hold) {
    Q_ASSERT(QThread::currentThread() == thread());
    if (!owner) return;
    if (hold) {
        observeOwner(owner);
        if (!d->heldCompletions.contains(owner)) d->heldCompletions.insert(owner, {});
        return;
    }
    const auto completions = d->heldCompletions.take(owner);
    for (const auto& completion : completions) completion.deliver();
}
void DecodeScheduler::retainThumbnailRequests(QObject* owner, const ResidentMediaAsset& asset,
                                             const QSet<int>& desiredFrameIndices) {
    for (auto it = d->jobs.begin(); it != d->jobs.end();) {
        const auto& job = it.value();
        if (!job->thumbnail) { ++it; continue; }
        auto& listeners = job->listeners;
        listeners.erase(std::remove_if(listeners.begin(), listeners.end(), [&](const auto& listener) {
            return !listener.owner || (listener.owner == owner
                && (job->asset->sha256 != asset.sha256 || !desiredFrameIndices.contains(job->index)));
        }), listeners.end());
        if (listeners.empty()) {
            job->cancelled->store(true, std::memory_order_relaxed);
            it = d->jobs.erase(it);
        } else ++it;
    }
    dispatch();
}
void DecodeScheduler::dispatch() {
    while (d->running < workerCount()) {
        std::shared_ptr<Job> next;
        for (const auto& job : std::as_const(d->jobs)) {
            if (job->running || job->listeners.empty()) continue;
            // A thumbnail seek can traverse an entire GOP. Leave capacity for
            // a pointer/Play request that arrives after the optional work began.
            if (job->priority <= Prefetch && workerCount() > 1 && d->runningOptional >= workerCount() - 1) continue;
            if (!next || job->priority > next->priority || (job->priority == next->priority && job->deadline < next->deadline)) next = job;
        }
        if (!next) return;
        Impl::Worker* worker = nullptr;
        for (const auto& candidate : d->workers) {
            if (candidate->busy) continue;
            if (!worker || affinity(candidate->position, *next) < affinity(worker->position, *next))
                worker = candidate.get();
        }
        if (!worker) return;
        worker->busy = true;
        next->running = true; ++d->running;
        if (next->thumbnail) ++d->runningThumbnails;
        const bool optionalSlot = next->priority <= Prefetch;
        if (optionalSlot) ++d->runningOptional;
        using Result = DecodeResult;
        auto* watcher = new QFutureWatcher<Result>(this);
        connect(watcher, &QFutureWatcher<Result>::finished, this, [this, watcher, next, worker, optionalSlot] {
            auto [frame, error, position] = watcher->result(); watcher->deleteLater();
            worker->position = std::move(position);
            --d->running; worker->busy = false;
            if (next->thumbnail) --d->runningThumbnails;
            if (optionalSlot) --d->runningOptional;
            // A cancelled seek may already have a replacement with this key.
            if (d->jobs.value(next->key) == next) d->jobs.remove(next->key);
            const bool wanted = std::any_of(next->listeners.cbegin(), next->listeners.cend(),
                [](const Listener& listener) { return !listener.owner.isNull(); });
            // Work already running may complete after its last subscriber was
            // removed. It must not repopulate a cache that was just reclaimed.
            if (frame && !next->thumbnail && wanted) {
                const QString identity = frame->editingPreview ? next->key : key(*next->asset, next->index);
                d->live.insert(identity, frame);
                if (d->optionalEnabled) {
                    if (auto old = d->frames.find(identity); old != d->frames.end()) d->frameBytes -= old->frame->bytes;
                    d->frames.insert(identity, {frame, ++d->sequence}); d->frameBytes += frame->bytes;
                }
                d->trim();
            }
            const auto listeners = std::move(next->listeners);
            for (const auto& listener : listeners) if (listener.owner) listener.completion(frame, error);
            dispatch();
        });
        watcher->setFuture(QtConcurrent::run(&worker->pool, [asset = next->asset, index = next->index,
            thumbnail = next->thumbnail, scrub = next->priority == Scrub,
            cachedFrame = next->cachedFrame, token = next->cancelled]() -> Result {
            // One reusable context per bounded worker, not per asset or clip.
            // Cache/JPEG work leaves its real decoder position unchanged, even
            // if this job targets another source or a much later timestamp.
            thread_local DecoderSession session;
            QString error;
            const auto cancelled = [token] { return token->load(std::memory_order_relaxed); };
            const auto finish = [](SharedMediaFramePtr frame, QString error) -> Result {
                return {std::move(frame), std::move(error), session.position};
            };
            if (cancelled()) return finish({}, QStringLiteral("cancelled"));
            auto frame = std::make_shared<SharedMediaFrame>();
            if (thumbnail) {
                QImage thumbnailImage = MediaPreviewStore::thumbnail(*asset, index);
                const bool diskHit = !thumbnailImage.isNull();
                if (thumbnailImage.isNull() && cachedFrame) {
                    const auto& video = cachedFrame->frame;
                    QImage image = ResidentVideoPlayer::presentationFrame(video).toImage();
                    if (video.rotation() != QtVideo::Rotation::None) image = image.transformed(QTransform().rotate(int(video.rotation())));
                    if (video.mirrored()) image = image.flipped(Qt::Horizontal);
                    thumbnailImage = image.scaled(QSize(MediaThumbnails::Width, MediaThumbnails::Height), Qt::KeepAspectRatio, Qt::SmoothTransformation);
                } else if (thumbnailImage.isNull()) {
                    thumbnailImage = session.decoder.thumbnail(*asset, index, error, cancelled);
                    session.decoded(*asset, index, !thumbnailImage.isNull());
                }
                if (cancelled() || thumbnailImage.isNull()) return finish({}, error);
                if (!diskHit) MediaPreviewStore::storeThumbnail(*asset, index, thumbnailImage);
                // All subscribers share one tracked allocation even if a
                // callback evicts the LRU before the next subscriber runs.
                frame->thumbnail = trackedThumbnail(thumbnailImage);
                return finish(frame, {});
            }
            QVideoFrame video = scrub ? MediaPreviewStore::scrubFrame(*asset, index) : QVideoFrame{};
            frame->editingPreview = video.isValid();
            if (!video.isValid()) {
                video = session.decoder.videoFrame(*asset, index, error, cancelled);
                session.decoded(*asset, index, video.isValid());
            }
            if (cancelled() || !video.isValid()) return finish({}, error);
            frame->frame = std::move(video); frame->bytes = mediaFrameAllocationBytes(frame->frame); 
            return finish(frame, {});
        }));
    }
}
QImage DecodeScheduler::thumbnail(const ResidentMediaAsset& asset, qint64 timestampUs) const {
    const auto image = d->thumbnails.value(key(asset, IndexedMediaDecoder::frameAt(asset, timestampUs))).image;
    return image ? *image : QImage();
}
void DecodeScheduler::requestThumbnail(QObject* owner, std::shared_ptr<const ResidentMediaAsset> asset,
                                       qint64 timestampUs, std::function<void(std::shared_ptr<const QImage>)> completion,
                                       bool visible) {
    if (!owner || !asset || asset->frameIndex.isEmpty() || !d->optionalEnabled) { completion({}); return; }
    observeOwner(owner);
    const int index = IndexedMediaDecoder::frameAt(*asset, timestampUs);
    const auto identity = key(*asset, index);
    auto it = d->thumbnails.find(identity);
    if (it != d->thumbnails.end()) { it->use = ++d->sequence; completion(it->image); return; }
    const QString jobKey = QStringLiteral("thumbnail:") + identity;
    auto job = d->jobs.value(jobKey);
    if (!job) {
        // Within the thumbnail priority, source order lets inter-frame workers
        // reuse reference history instead of seeking to a random GOP per tile.
        job = std::make_shared<Job>(Job{jobKey, asset, index, visible ? VisibleThumbnail : Thumbnail,
            asset->frameIndex[index].timestampUs, {}, false, true});
        job->cachedFrame = cached(*asset, timestampUs);
        d->jobs.insert(jobKey, job);
    }
    if (visible) job->priority = std::max(job->priority, VisibleThumbnail);
    // Promotion from neighbour to visible updates one subscription, rather
    // than delivering the same bitmap twice to the same strip.
    job->listeners.erase(std::remove_if(job->listeners.begin(), job->listeners.end(),
        [owner](const auto& listener) { return listener.owner == owner; }), job->listeners.end());
    job->listeners.push_back({owner, 0, [this, identity, completion = std::move(completion)](auto frame, const QString&) {
        if (!frame || !frame->thumbnail || !d->optionalEnabled) { completion({}); return; }
        if (!d->thumbnails.contains(identity)) {
            d->thumbnailBytes += frame->thumbnail->sizeInBytes();
            d->thumbnails.insert(identity, {frame->thumbnail, ++d->sequence}); d->trim();
        }
        completion(d->thumbnails.value(identity).image);
    }});
    dispatch();
}

void DecodeScheduler::evictOptionalCaches() {
    d->frames.clear(); d->thumbnails.clear(); d->frameBytes = d->thumbnailBytes = 0; d->trim();
    emit optionalCachesCleared();
}
quint64 DecodeScheduler::optionalFrameBytes() const { return d->frameBytes; }
quint64 DecodeScheduler::thumbnailBytes() const { return d->thumbnailBytes; }
quint64 DecodeScheduler::trackedThumbnailBytes() const { return liveThumbnailBytes.load(std::memory_order_relaxed); }
quint64 DecodeScheduler::trackedFrameBytes() const { return mediaFrameCpuBytes(); }
int DecodeScheduler::pendingJobs() const {
    int pending = d->running;
    for (const auto& job : std::as_const(d->jobs)) if (!job->running) ++pending;
    return pending;
}
int DecodeScheduler::runningThumbnailJobs() const { return d->runningThumbnails; }

void DecodeScheduler::setOptionalCachingEnabled(bool enabled) {
    if (d->optionalEnabled == enabled) return;
    d->optionalEnabled = enabled;
    if (!enabled) {
        evictOptionalCaches();
        for (auto it = d->jobs.begin(); it != d->jobs.end();) {
            if (it.value()->thumbnail) {
                it.value()->listeners.clear();
                it.value()->cancelled->store(true, std::memory_order_relaxed);
                it = d->jobs.erase(it);
            } else ++it;
        }
    }
    emit optionalCacheAvailabilityChanged();
}
bool DecodeScheduler::optionalCachingEnabled() const { return d->optionalEnabled; }
