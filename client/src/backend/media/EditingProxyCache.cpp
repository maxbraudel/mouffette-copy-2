#include "backend/media/EditingProxyCache.h"
#include "backend/media/IndexedMediaDecoder.h"
#include "backend/media/MediaPreviewStore.h"
#include <QFutureWatcher>
#include <QPointer>
#include <QSet>
#include <QThread>
#include <QThreadPool>
#include <QTimer>
#include <QtConcurrent>
#include <atomic>
#include <limits>

namespace {
struct ProxyJob {
    std::weak_ptr<const ResidentMediaAsset> asset;
    QSet<QObject*> owners;
    std::atomic_bool cancelled{false};
    int next = 0;
    int focus = -1;
    quint64 use = 0;
    bool complete = false, running = false;
};
struct ProxyResult { int next = 0; bool failed = false; };
}
struct EditingProxyCache::Impl {
    QHash<QString, std::shared_ptr<ProxyJob>> jobs;
    QHash<QObject*, QString> owners;
    QHash<QObject*, QMetaObject::Connection> destroyed;
    QSet<QObject*> interactiveOwners;
    std::atomic_bool enabled{true}, interactive{false};
    QThreadPool worker;
    QTimer timer;
    bool running = false;
    quint64 sequence = 0;
};
EditingProxyCache& EditingProxyCache::instance() { static EditingProxyCache value; return value; }
EditingProxyCache::EditingProxyCache() : d(std::make_unique<Impl>()) {
    d->worker.setMaxThreadCount(1);
    d->worker.setThreadPriority(QThread::LowPriority);
    d->worker.setExpiryTimeout(1000);
    d->timer.setSingleShot(true);
    connect(&d->timer, &QTimer::timeout, this, &EditingProxyCache::dispatch);
}
EditingProxyCache::~EditingProxyCache() {
    d->enabled.store(false);
    for (const auto& job : d->jobs) job->cancelled.store(true);
    d->worker.waitForDone();
}
void EditingProxyCache::acquire(QObject* owner, std::shared_ptr<const ResidentMediaAsset> asset) {
    Q_ASSERT(QThread::currentThread() == thread());
    if (!owner) return;
    if (!asset || !MediaPreviewStore::supportsScrubProxy(*asset)) { release(owner); return; }
    if (d->owners.value(owner) == asset->sha256) return;
    release(owner);
    auto& job = d->jobs[asset->sha256];
    if (!job) { job = std::make_shared<ProxyJob>(); job->asset = asset; job->use = ++d->sequence; }
    job->owners.insert(owner); d->owners.insert(owner, asset->sha256);
    d->destroyed.insert(owner, connect(owner, &QObject::destroyed, this, [this, owner] { release(owner); }));
    if (!d->timer.isActive()) d->timer.start(250);
}
void EditingProxyCache::release(QObject* owner) {
    Q_ASSERT(QThread::currentThread() == thread());
    setInteractive(owner, false);
    const QString identity = d->owners.take(owner);
    if (d->destroyed.contains(owner)) disconnect(d->destroyed.take(owner));
    auto it = d->jobs.find(identity);
    if (it == d->jobs.end()) return;
    it.value()->owners.remove(owner);
    if (it.value()->owners.isEmpty()) { it.value()->cancelled.store(true); d->jobs.erase(it); }
}
void EditingProxyCache::setEnabled(bool enabled) {
    d->enabled.store(enabled);
    if (enabled) { if (!d->timer.isActive()) d->timer.start(250); }
    else d->timer.stop();
}
void EditingProxyCache::setInteractive(QObject* owner, bool active) {
    if (active) d->interactiveOwners.insert(owner); else d->interactiveOwners.remove(owner);
    d->interactive.store(!d->interactiveOwners.isEmpty());
    if (!d->interactive.load() && !d->timer.isActive()) d->timer.start(150);
}
int EditingProxyCache::pendingAssets() const {
    int count = 0;
    for (const auto& job : d->jobs) if (!job->complete) ++count;
    return count;
}
void EditingProxyCache::prioritize(QObject* owner, int frameIndex) {
    const auto job = d->jobs.value(d->owners.value(owner));
    if (!job) return;
    // A long source can exceed the disk budget. Revisit the pointer's window
    // even after the background scan completed and its early images expired.
    const int begin = std::max(0, frameIndex - 16);
    if (job->focus < 0 || frameIndex < job->focus || frameIndex >= job->focus + 32)
        job->focus = begin;
    job->complete = false;
    if (!d->timer.isActive()) d->timer.start(150);
}
void EditingProxyCache::dispatch() {
    if (d->running || !d->enabled.load() || d->interactive.load()) return;
    std::shared_ptr<ProxyJob> next;
    for (const auto& job : d->jobs)
        if (!job->complete && !job->cancelled.load() && !job->asset.expired() && (!next || job->use < next->use)) next = job;
    if (!next) return;
    const auto asset = next->asset.lock();
    if (!asset) return;
    next->running = true; d->running = true;
    const int focus = next->focus;
    const int start = focus >= 0 ? focus : next->next;
    auto* watcher = new QFutureWatcher<ProxyResult>(this);
    connect(watcher, &QFutureWatcher<ProxyResult>::finished, this, [this, watcher, next, focus, count = asset->frameIndex.size()] {
        const auto result = watcher->result(); watcher->deleteLater();
        d->running = false; next->running = false;
        if (focus >= 0) {
            if (next->focus == focus && (result.failed || result.next >= std::min(qsizetype(focus + 32), count)))
                next->focus = -1;
        } else next->next = result.next;
        next->complete = result.failed || (next->focus < 0 && next->next >= count);
        next->use = ++d->sequence;
        if (d->enabled.load() && !d->interactive.load()) d->timer.start(10);
    });
    watcher->setFuture(QtConcurrent::run(&d->worker, [this, next, asset, start] {
        // One session on the one bounded worker preserves GOP history between
        // batches. The idle thread expires after one second, releasing the last
        // shared source reference even when its canvas has been closed.
        thread_local IndexedMediaDecoder decoder;
        ProxyResult result{start, false};
        // Once AVIO observes cancellation, retain it even if a quick Play/Pause
        // changes the global state before the decoder checks its return value.
        auto stopped = std::make_shared<std::atomic_bool>(false);
        auto cancelled = [this, next, stopped] {
            if (next->cancelled.load() || !d->enabled.load() || d->interactive.load()) stopped->store(true);
            return stopped->load();
        };
        // A batch is a short contiguous window, not one full-source job. It
        // yields between windows and cooperatively interrupts within a seek.
        const int end = std::min(int(asset->frameIndex.size()), start + 32);
        for (int index = start; index < end && !cancelled(); ++index) {
            if (!MediaPreviewStore::hasScrubFrame(*asset, index)) {
                QString error;
                const auto image = decoder.previewImage(*asset, index,
                    QSize(MediaPreviewStore::ProxyExtent, MediaPreviewStore::ProxyExtent), error, cancelled);
                if (cancelled()) break;
                if (image.isNull() || !MediaPreviewStore::storeScrubFrame(*asset, index, image)) { result.failed = true; break; }
            }
            result.next = index + 1;
        }
        return result;
    }));
}
