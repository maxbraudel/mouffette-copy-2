#include "backend/media/MediaResidencyManager.h"
#include "backend/media/MediaDecoder.h"
#include "backend/media/ResidentVideoPlayer.h"
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDateTime>
#include <QFile>
#include <QFileInfo>
#include <QFutureWatcher>
#include <QPointer>
#include <QtConcurrent/QtConcurrentRun>
#include <algorithm>
#include <limits>
#include <utility>
#ifdef Q_OS_MACOS
#include <mach/mach.h>
#include <sys/sysctl.h>
#include <dispatch/dispatch.h>
#elif defined(Q_OS_WIN)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <psapi.h>
#include <QWinEventNotifier>
#endif

namespace {
constexpr quint64 GiB = 1024ULL * 1024 * 1024;
constexpr quint64 MiB = 1024ULL * 1024;
constexpr quint64 DecodeScratch = 16 * MiB;
QString signature(const QString& path) {
    const QFileInfo info(path);
    return QStringLiteral("%1:%2:%3:%4").arg(info.size()).arg(info.lastModified().toMSecsSinceEpoch())
        .arg(info.birthTime().toMSecsSinceEpoch()).arg(info.metadataChangeTime().toMSecsSinceEpoch());
}
QString normalizedPath(const QString& path) {
    const QFileInfo info(path);
    return info.canonicalFilePath().isEmpty() ? info.absoluteFilePath() : info.canonicalFilePath();
}
struct ProbeResult {
    MediaDecoder::Probe probe;
    QString hash;
    QString error;
};
struct DecodeResult {
    std::shared_ptr<ResidentMediaAsset> asset;
    QString error;
};
}

struct MediaResidencyManager::Entry {
    QString path;
    QString signature;
    QString hash;
    QString state = QStringLiteral("analysing");
    QString error;
    QSet<QString> owners;
    quint64 estimated = 0;
    quint64 reserved = 0;
    quint64 scratch = DecodeScratch;
    int activePlayers = 0;
    int pendingPlayers = 0;
    std::atomic<quint64> budgeted{0};
    std::atomic<quint64> required{0};
    double progress = 0;
    std::shared_ptr<const ResidentMediaAsset> data;
    std::unique_ptr<ResidentVideoPlayer> validationPlayer;
    bool validationAdmissionRefused = false;
    std::shared_ptr<std::atomic_bool> cancelled = std::make_shared<std::atomic_bool>(false);
    std::atomic<quint64> allocated{0};
    quint64 generation = 0;
    bool requiresHealthySamples = false;
};

MediaResidencyManager& MediaResidencyManager::instance() {
    static QPointer<MediaResidencyManager> service;
    if (!service) service = new MediaResidencyManager(QCoreApplication::instance());
    return *service;
}
MediaResidencyManager::MediaResidencyManager(QObject* parent) : QObject(parent) {
    m_clock.start();
    m_memory = readSystemMemory();
    setupPressureNotifications();
    m_timer.setInterval(1000);
    connect(&m_timer, &QTimer::timeout, this, &MediaResidencyManager::sampleNow);
    m_timer.start();
}
MediaResidencyManager::~MediaResidencyManager() {
    m_timer.stop();
    for (const auto& entry : m_entries) {
        entry->cancelled->store(true);
        cancelPlaybackValidation(entry);
    }
    // Workers queue progress to this QObject. Keep it alive until every
    // canceled worker has stopped, so no cross-thread QPointer/invoke race is
    // possible during final process/service destruction. Ordinary release is
    // asynchronous and uses backgroundWorkFinished instead.
    for (auto* job : std::as_const(m_jobs)) job->waitForFinished();
#ifdef Q_OS_MACOS
    if (m_pressureSource) {
        dispatch_set_context(static_cast<dispatch_source_t>(m_pressureSource), nullptr);
        dispatch_source_cancel(static_cast<dispatch_source_t>(m_pressureSource));
        dispatch_release(static_cast<dispatch_source_t>(m_pressureSource));
    }
#elif defined(Q_OS_WIN)
    if (m_pressureNotifier) {
        static_cast<QWinEventNotifier*>(m_pressureNotifier.data())->setEnabled(false);
        delete m_pressureNotifier.data();
    }
    if (m_pressureSource) CloseHandle(m_pressureSource);
#endif
}
void MediaResidencyManager::setupPressureNotifications() {
#ifdef Q_OS_MACOS
    auto source = dispatch_source_create(DISPATCH_SOURCE_TYPE_MEMORYPRESSURE, 0,
        DISPATCH_MEMORYPRESSURE_NORMAL | DISPATCH_MEMORYPRESSURE_WARN | DISPATCH_MEMORYPRESSURE_CRITICAL,
        dispatch_get_main_queue());
    if (!source) return;
    m_pressureSource = source;
    dispatch_set_context(source, this);
    dispatch_source_set_event_handler_f(source, [](void* context) {
        auto* self = static_cast<MediaResidencyManager*>(context);
        if (!self || self->m_testMemory) return;
        const auto flags = dispatch_source_get_data(static_cast<dispatch_source_t>(self->m_pressureSource));
        self->m_nativePressure.store((flags & DISPATCH_MEMORYPRESSURE_CRITICAL) ? 2
            : ((flags & DISPATCH_MEMORYPRESSURE_WARN) ? 1 : 0));
        self->sampleNow();
    });
    dispatch_resume(source);
#elif defined(Q_OS_WIN)
    HANDLE event = CreateMemoryResourceNotification(LowMemoryResourceNotification);
    if (!event) return;
    m_pressureSource = event;
    auto* notifier = new QWinEventNotifier(event, this);
    m_pressureNotifier = notifier;
    connect(notifier, &QWinEventNotifier::activated, this, [this, notifier]() {
        notifier->setEnabled(false); // level-triggered, rearmed after recovery
        // Windows' low-physical-memory event is a hard allocation constraint,
        // unlike macOS' advisory pressure warning.
        m_nativePressure.store(2);
        sampleNow();
    });
#endif
}
MediaResidencyManager::MemorySnapshot MediaResidencyManager::readSystemMemory() {
    MemorySnapshot result;
#ifdef Q_OS_MACOS
    uint64_t physical = 0;
    size_t size = sizeof(physical);
    sysctlbyname("hw.memsize", &physical, &size, nullptr, 0);
    result.totalBytes = physical;
    vm_statistics64_data_t stats{};
    mach_msg_type_number_t count = HOST_VM_INFO64_COUNT;
    const auto host = mach_host_self();
    vm_size_t pageSize = 0;
    host_page_size(host, &pageSize);
    if (host_statistics64(host, HOST_VM_INFO64, reinterpret_cast<host_info64_t>(&stats), &count) == KERN_SUCCESS)
        result.availableBytes = (quint64(stats.free_count) + stats.inactive_count) * pageSize;
    mach_port_deallocate(mach_task_self(), host);
    task_vm_info_data_t process{};
    count = TASK_VM_INFO_COUNT;
    if (task_info(mach_task_self(), TASK_VM_INFO, reinterpret_cast<task_info_t>(&process), &count) == KERN_SUCCESS)
        result.processBytes = process.resident_size;
    result.availableEstimated = true;
    // Dispatch events can be coalesced and only report transitions. Reconcile
    // the latched notification with the current level on every measurement.
    int pressureFlags = 0;
    size = sizeof(pressureFlags);
    if (sysctlbyname("kern.memorystatus_vm_pressure_level", &pressureFlags, &size, nullptr, 0) == 0) {
        result.pressure = (pressureFlags & DISPATCH_MEMORYPRESSURE_CRITICAL) ? 2
            : ((pressureFlags & DISPATCH_MEMORYPRESSURE_WARN) ? 1 : 0);
        result.pressureKnown = true;
    }
#elif defined(Q_OS_WIN)
    MEMORYSTATUSEX status{};
    status.dwLength = sizeof(status);
    if (GlobalMemoryStatusEx(&status)) {
        result.totalBytes = status.ullTotalPhys;
        result.availableBytes = status.ullAvailPhys;
    }
    PROCESS_MEMORY_COUNTERS_EX process{};
    if (GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&process), sizeof(process)))
        result.processBytes = process.WorkingSetSize;
#else
    QFile memory(QStringLiteral("/proc/meminfo"));
    if (memory.open(QIODevice::ReadOnly)) {
        for (const auto& line : memory.readAll().split('\n')) {
            const auto fields = line.simplified().split(' ');
            if (fields.size() < 2) continue;
            if (fields[0] == "MemTotal:") result.totalBytes = fields[1].toULongLong() * 1024;
            if (fields[0] == "MemAvailable:") result.availableBytes = fields[1].toULongLong() * 1024;
        }
    }
    QFile status(QStringLiteral("/proc/self/status"));
    if (status.open(QIODevice::ReadOnly)) for (const auto& line : status.readAll().split('\n')) {
        const auto fields = line.simplified().split(' ');
        if (fields.size() > 1 && fields[0] == "VmRSS:") result.processBytes = fields[1].toULongLong() * 1024;
    }
#endif
    result.availableBytes = std::min(result.totalBytes, result.availableBytes);
    result.processBytes = std::min(result.processBytes, result.totalBytes - result.availableBytes);
    return result;
}
void MediaResidencyManager::setSafetyReserve(int percent, int minimumMiB)
{
    m_reservePercent = qBound(0, percent, 100);
    m_reserveMinMiB = qMax(0, minimumMiB);
    sampleNow();
}

quint64 MediaResidencyManager::reserveBytes() const
{
    const quint64 percentage = (m_memory.totalBytes / 100) * m_reservePercent
        + (m_memory.totalBytes % 100) * m_reservePercent / 100;
    return std::max(quint64(m_reserveMinMiB) * MiB, percentage);
}
quint64 MediaResidencyManager::residentBytes() const {
    quint64 bytes = 0;
    for (const auto& e : m_entries) bytes += e->data ? e->data->residentBytes : e->allocated.load();
    return bytes;
}
quint64 MediaResidencyManager::playbackBudgetBytes() const {
    quint64 bytes = 0;
    for (const auto& e : m_entries) {
        if (!e->data || !e->data->video) continue;
        int pinnedPlayers = 0;
        for (const auto& owners : m_pins)
            for (const auto& id : owners) if (e->owners.contains(id)) ++pinnedPlayers;
        bytes += e->data->playbackBudgetBytes * quint64(std::max(e->activePlayers, pinnedPlayers));
    }
    return bytes;
}
quint64 MediaResidencyManager::reservedBudgetBytes() const {
    quint64 bytes = pendingPlaybackBudgetBytes();
    for (const auto& e : m_entries)
        bytes += e->reserved - std::min(e->reserved, e->allocated.load());
    return bytes;
}
quint64 MediaResidencyManager::pendingPlaybackBudgetBytes() const {
    quint64 bytes = 0;
    for (const auto& e : m_entries) {
        if (!e->data || !e->data->video) continue;
        int pinnedPlayers = 0;
        for (const auto& owners : m_pins)
            for (const auto& id : owners) if (e->owners.contains(id)) ++pinnedPlayers;
        // A first decoded frame commits the player's allocations to the OS
        // measurement. Keep only unopened scene slots and still-priming players
        // as future commitments; charging prepared players again double-counts
        // memory already absent from availableBytes.
        const int pending = e->pendingPlayers + std::max(0, pinnedPlayers - e->activePlayers);
        bytes += e->data->playbackBudgetBytes * quint64(pending);
    }
    return bytes;
}
int MediaResidencyManager::pressureLevel() const {
    return std::max(m_memory.pressure, m_nativePressure.load());
}
bool MediaResidencyManager::allocationsBlocked() const {
    // A warning describes system-wide reclaim/compression activity, not an
    // allocation failure. Admit fully budgeted work while headroom exists;
    // critical pressure and the configured byte reserve remain hard limits.
    return pressureLevel() >= 2 || m_memory.availableBytes < reserveBytes();
}
quint64 MediaResidencyManager::loadableBytes() const {
    if (allocationsBlocked()) return 0;
    const quint64 available = m_memory.availableBytes;
    const quint64 reserve = reserveBytes();
    if (available <= reserve) return 0;
    const quint64 remaining = available - reserve;
    return remaining - std::min(remaining, reservedBudgetBytes());
}
QString MediaResidencyManager::waitingReason(quint64 required) const {
    if (pressureLevel() >= 2) {
        return QStringLiteral("Waiting for critical system memory pressure to recover (%1 MiB available)")
            .arg(m_memory.availableBytes / MiB);
    }
    if (required <= loadableBytes())
        return QStringLiteral("Waiting for available RAM to stabilize before reloading");
    return QStringLiteral("Waiting for RAM: preparation needs %1 MiB; %2 MiB usable (%3 MiB available, %4 MiB system reserve, %5 MiB pending allocations)")
        .arg((required + MiB - 1) / MiB).arg(loadableBytes() / MiB)
        .arg(m_memory.availableBytes / MiB).arg((reserveBytes() + MiB - 1) / MiB)
        .arg((reservedBudgetBytes() + MiB - 1) / MiB);
}
void MediaResidencyManager::refreshSystemMemory() {
    if (m_testMemory) return;
    m_memory = readSystemMemory();
    if (m_memory.pressureKnown) m_nativePressure.store(m_memory.pressure);
#ifdef Q_OS_WIN
    if (m_pressureSource) {
        BOOL low = FALSE;
        if (QueryMemoryResourceNotification(m_pressureSource, &low)) {
            m_nativePressure.store(low ? 2 : 0);
            if (!low && m_pressureNotifier)
                static_cast<QWinEventNotifier*>(m_pressureNotifier.data())->setEnabled(true);
        }
    }
#endif
}
bool MediaResidencyManager::admitsBudget(quint64 additional, bool includeReservations) {
    refreshSystemMemory();
    if (allocationsBlocked()) return false;
    const quint64 pending = includeReservations ? reservedBudgetBytes() : 0;
    return m_memory.availableBytes >= reserveBytes()
        && pending <= m_memory.availableBytes - reserveBytes()
        && additional <= m_memory.availableBytes - reserveBytes() - pending;
}
void MediaResidencyManager::acquire(const QString& ownerId, const QString& path, const QString& expected) {
    if (ownerId.isEmpty() || path.isEmpty()) return;
    const QFileInfo info(path);
    const QString canonical = info.canonicalFilePath().isEmpty() ? info.absoluteFilePath() : info.canonicalFilePath();
    const QString stamp = signature(canonical);
    const auto previous = m_owners.constFind(ownerId);
    if (previous != m_owners.cend() && previous->path == canonical
        && previous->signature == stamp && previous->expectedSha256 == expected) return;
    release(ownerId);
    for (const auto& e : m_entries) {
        if (e->path != canonical || e->signature != stamp || e->cancelled->load()) continue;
        e->owners.insert(ownerId);
        m_owners.insert(ownerId, {e, expected, canonical, stamp});
        QTimer::singleShot(0, this, [this, ownerId]() { emit ownerChanged(ownerId); emit changed(); });
        return;
    }
    auto entry = std::make_shared<Entry>();
    entry->path = canonical;
    entry->signature = stamp;
    entry->owners.insert(ownerId);
    m_entries.append(entry);
    m_owners.insert(ownerId, {entry, expected, canonical, stamp});
    publish(entry);
    startProbe(entry);
}
void MediaResidencyManager::release(const QString& ownerId) {
    auto found = m_owners.find(ownerId);
    if (found == m_owners.end()) return;
    const auto e = found->entry;
    m_owners.erase(found);
    e->owners.remove(ownerId);
    for (auto it = m_pins.begin(); it != m_pins.end(); ++it) it->removeAll(ownerId);
    if (e->owners.isEmpty()) {
        e->cancelled->store(true);
        cancelPlaybackValidation(e);
        ++e->generation;
        e->data.reset();
        m_entries.removeAll(e);
        QTimer::singleShot(0, this, &MediaResidencyManager::sampleNow);
    }
    emit changed();
}
bool MediaResidencyManager::hasBackgroundWorkForPath(const QString& path) const {
    return m_backgroundPaths.value(normalizedPath(path)) > 0;
}
void MediaResidencyManager::finishBackgroundWork(const QString& path) {
    auto it = m_backgroundPaths.find(path);
    if (it == m_backgroundPaths.end() || --it.value() > 0) return;
    m_backgroundPaths.erase(it);
    // A completed probe can immediately start a decode. Notify only after its
    // result is processed and only if no successor still owns the file.
    QTimer::singleShot(0, this, [this, path]() {
        if (!m_backgroundPaths.contains(path)) emit backgroundWorkFinished(path);
    });
}
bool MediaResidencyManager::ready(const QString& id) const {
    const auto it = m_owners.constFind(id);
    return it != m_owners.cend() && it->entry->state == QLatin1String("ready") && it->entry->data
        && (it->expectedSha256.isEmpty() || it->expectedSha256 == it->entry->hash);
}
QString MediaResidencyManager::state(const QString& id) const {
    const auto it = m_owners.constFind(id);
    if (it == m_owners.cend()) return QStringLiteral("queued");
    if (!it->expectedSha256.isEmpty() && !it->entry->hash.isEmpty() && it->expectedSha256 != it->entry->hash)
        return QStringLiteral("error");
    return it->entry->state;
}
double MediaResidencyManager::progress(const QString& id) const {
    const auto it = m_owners.constFind(id);
    return it == m_owners.cend() ? 0 : it->entry->progress;
}
QString MediaResidencyManager::errorString(const QString& id) const {
    const auto it = m_owners.constFind(id);
    if (it == m_owners.cend()) return {};
    if (!it->expectedSha256.isEmpty() && !it->entry->hash.isEmpty() && it->expectedSha256 != it->entry->hash)
        return QStringLiteral("The source file has changed (SHA-256 mismatch)");
    return it->entry->error;
}
QString MediaResidencyManager::sha256(const QString& id) const {
    const auto it = m_owners.constFind(id);
    return it == m_owners.cend() ? QString() : it->entry->hash;
}
std::shared_ptr<const ResidentMediaAsset> MediaResidencyManager::asset(const QString& id) const {
    return ready(id) ? m_owners.value(id).entry->data : nullptr;
}
void MediaResidencyManager::publish(const EntryPtr& e) {
    const auto owners = e->owners.values();
    for (const auto& id : owners) if (m_owners.contains(id)) emit ownerChanged(id);
    emit changed();
}
void MediaResidencyManager::startProbe(const EntryPtr& e) {
    const auto cancelled = e->cancelled;
    const QString path = e->path, stamp = e->signature;
    ++m_backgroundPaths[path];
    const quint64 generation = ++e->generation;
    e->state = QStringLiteral("analysing");
    e->error.clear();
    auto* watcher = new QFutureWatcher<ProbeResult>(this);
    m_jobs.insert(watcher);
    connect(watcher, &QFutureWatcher<ProbeResult>::finished, this, [this, watcher, e, generation, path]() {
        const auto result = watcher->result();
        m_jobs.remove(watcher);
        watcher->deleteLater();
        finishBackgroundWork(path);
        if (e->generation != generation || e->cancelled->load() || e->owners.isEmpty()) return;
        if (!result.error.isEmpty() || signature(e->path) != e->signature) {
            e->state = QStringLiteral("error");
            e->error = result.error.isEmpty() ? QStringLiteral("The source changed during import") : result.error;
            publish(e);
            for (const auto& id : e->owners) emit errorOccurred(id, e->error);
            return;
        }
        e->hash = result.hash;
        e->estimated = result.probe.estimatedBytes;
        e->scratch = std::max({DecodeScratch, result.probe.scratchBytes, result.probe.playbackBudgetBytes});
        // Merge complete content identities, never merely paths or filenames.
        for (const auto other : m_entries) {
            if (other == e || other->hash != e->hash || other->cancelled->load()) continue;
            const auto owners = e->owners;
            for (const auto& id : owners) {
                m_owners[id].entry = other;
                other->owners.insert(id);
            }
            e->owners.clear(); e->cancelled->store(true);
            m_entries.removeAll(e);
            if (other->state == QLatin1String("error")) {
                other->state = QStringLiteral("queued");
                other->error.clear();
            }
            publish(other);
            schedule();
            return;
        }
        const quint64 usable = m_memory.totalBytes > reserveBytes() ? m_memory.totalBytes - reserveBytes() : 0;
        if (e->estimated > usable || e->scratch > usable - std::min(usable, e->estimated)) {
            e->state = QStringLiteral("capacity_insufficient");
            e->error = QStringLiteral("The media and its preparation buffers exceed this computer's RAM capacity and safety reserve");
        } else {
            e->state = QStringLiteral("queued");
        }
        publish(e);
        schedule();
    });
    watcher->setFuture(QtConcurrent::run([path, stamp, cancelled]() {
        ProbeResult result;
        if (cancelled->load()) { result.error = QStringLiteral("cancelled"); return result; }
        result.probe = MediaDecoder::probe(path);
        if (!result.probe.accepted()) { result.error = result.probe.error; return result; }
        QFile file(path);
        if (!file.open(QIODevice::ReadOnly)) { result.error = file.errorString(); return result; }
        QCryptographicHash hash(QCryptographicHash::Sha256);
        while (!file.atEnd()) {
            if (cancelled->load()) { result.error = QStringLiteral("cancelled"); return result; }
            const auto bytes = file.read(MiB);
            if (bytes.isEmpty() && file.error() != QFileDevice::NoError) { result.error = file.errorString(); return result; }
            hash.addData(bytes);
        }
        if (signature(path) != stamp) { result.error = QStringLiteral("The source changed during import"); return result; }
        result.hash = QString::fromLatin1(hash.result().toHex());
        return result;
    }));
}
bool MediaResidencyManager::protectedEntry(const EntryPtr& entry) const {
    for (const auto& owners : m_pins) for (const auto& id : entry->owners)
        if (owners.contains(id)) return true;
    return false;
}
bool MediaResidencyManager::pinOwners(const QStringList& owners, const QString& group) {
    if (group.isEmpty()) return false;
    for (const auto& id : owners) if (!ready(id)) return false;
    const auto previous = m_pins.value(group);
    m_pins.insert(group, owners);
    // Reserve all scene decoder budgets together before creating any player.
    if (!owners.isEmpty() && !admitsBudget(0)) {
        if (previous.isEmpty()) m_pins.remove(group); else m_pins.insert(group, previous);
        return false;
    }
    m_stopRequested.remove(group);
    emit changed();
    return true;
}
void MediaResidencyManager::unpinGroup(const QString& group) {
    m_pins.remove(group); m_stopRequested.remove(group);
    emit changed();
    QTimer::singleShot(0, this, &MediaResidencyManager::sampleNow);
}
void MediaResidencyManager::evict(const EntryPtr& e) {
    if (protectedEntry(e)) return;
    cancelPlaybackValidation(e);
    e->state = QStringLiteral("waiting_for_memory");
    e->error = waitingReason(e->estimated + e->scratch);
    e->progress = 0;
    e->requiresHealthySamples = true;
    m_healthySamples = 0;
    m_lastHealthySampleMs = m_clock.elapsed();
    // Consumers synchronously clear their frame/audio references in response.
    // Admission still uses a fresh OS measurement, so delayed scene-graph
    // destruction is never mistaken for immediately available physical RAM.
    publish(e);
    e->data.reset();
    e->allocated.store(0);
    e->activePlayers = 0;
    e->pendingPlayers = 0;
    emit changed();
}
void MediaResidencyManager::schedule() {
    if (m_scheduling || m_decoding) return;
    m_scheduling = true;
    auto candidates = m_entries;
    std::sort(candidates.begin(), candidates.end(), [](const EntryPtr& a, const EntryPtr& b) {
        return a->estimated == b->estimated ? a->hash < b->hash : a->estimated < b->estimated;
    });
    refreshSystemMemory();
    for (const auto& e : candidates) {
        if (e->state != QLatin1String("queued") && e->state != QLatin1String("waiting_for_memory")
            && e->state != QLatin1String("capacity_insufficient")) continue;
        const quint64 capacity = m_memory.totalBytes - std::min(m_memory.totalBytes, reserveBytes());
        if (e->estimated > capacity || e->scratch > capacity - std::min(capacity, e->estimated)) {
            if (e->state != QLatin1String("capacity_insufficient")) {
                e->state = QStringLiteral("capacity_insufficient");
                e->error = QStringLiteral("The media and its preparation buffers exceed this computer's RAM capacity and safety reserve");
                publish(e);
            }
            continue;
        }
        const quint64 usable = loadableBytes();
        if (allocationsBlocked()
            || (e->requiresHealthySamples && m_healthySamples < 2) || e->estimated > usable
            || e->scratch > usable - std::min(usable, e->estimated)) {
            const auto reason = waitingReason(e->estimated + e->scratch);
            const bool changed = e->state != QLatin1String("waiting_for_memory") || e->error != reason;
            if (e->state != QLatin1String("waiting_for_memory")) {
                m_healthySamples = 0;
                m_lastHealthySampleMs = m_clock.elapsed();
            }
            e->state = QStringLiteral("waiting_for_memory");
            e->error = reason;
            e->requiresHealthySamples = true;
            if (changed) {
                publish(e);
            }
            continue;
        }
        // Deduplication shares bytes, not ownership of a particular path. A
        // surviving duplicate must reload from its own verified source after
        // the original occurrence has been deleted.
        bool sourceFound = false;
        auto ownerIds = e->owners.values();
        std::sort(ownerIds.begin(), ownerIds.end());
        for (const auto& id : ownerIds) {
            const Owner owner = m_owners.value(id);
            if (QFileInfo::exists(owner.path) && signature(owner.path) == owner.signature) {
                e->path = owner.path;
                e->signature = owner.signature;
                sourceFound = true;
                break;
            }
        }
        if (!sourceFound) {
            e->state = QStringLiteral("error");
            e->error = QStringLiteral("The source file is missing or has changed");
            publish(e);
            for (const auto& id : e->owners) emit errorOccurred(id, e->error);
            continue;
        }
        startDecode(e, usable);
        break; // one decoder avoids concurrent peak allocations and overcommit
    }
    m_scheduling = false;
}
void MediaResidencyManager::startDecode(const EntryPtr& e, quint64 allowance) {
    m_decoding = true;
    e->cancelled = std::make_shared<std::atomic_bool>(false);
    const auto cancel = e->cancelled;
    const auto generation = ++e->generation;
    e->reserved = std::min(allowance, e->estimated + e->scratch);
    e->required.store(0); e->budgeted.store(0);
    e->state = QStringLiteral("decoding"); e->error.clear(); e->progress = 0;
    publish(e);
    QPointer<MediaResidencyManager> self(this);
    const QString path = e->path;
    ++m_backgroundPaths[path];
    auto* watcher = new QFutureWatcher<DecodeResult>(this);
    m_jobs.insert(watcher);
    connect(watcher, &QFutureWatcher<DecodeResult>::finished, this, [this, watcher, e, generation, path]() {
        auto result = watcher->result(); watcher->deleteLater();
        m_jobs.remove(watcher);
        finishBackgroundWork(path);
        m_decoding = false;
        e->reserved = 0; e->allocated.store(0);
        if (e->generation != generation || e->owners.isEmpty()) { schedule(); return; }
        if (e->cancelled->load() || result.error == QLatin1String("memory_unavailable")) {
            e->state = QStringLiteral("waiting_for_memory");
            e->requiresHealthySamples = true; e->progress = 0;
            m_healthySamples = 0;
            m_lastHealthySampleMs = m_clock.elapsed();
            const quint64 required = e->required.load();
            if (required > e->scratch) e->estimated = std::max(e->estimated, required - e->scratch);
            e->error = waitingReason(e->estimated + e->scratch);
            const quint64 maximum = m_memory.totalBytes > reserveBytes() ? m_memory.totalBytes - reserveBytes() : 0;
            if (required > maximum) {
                e->state = QStringLiteral("capacity_insufficient");
                e->error = QStringLiteral("The media and its preparation buffers exceed this computer's RAM capacity and safety reserve");
            }
        } else if (!result.asset || result.asset->sha256 != e->hash || signature(e->path) != e->signature) {
            e->state = QStringLiteral("error");
            e->error = !result.error.isEmpty() ? result.error : QStringLiteral("The source changed during decoding");
            for (const auto& id : e->owners) emit errorOccurred(id, e->error);
        } else {
            const std::weak_ptr<Entry> weak = e;
            QPointer<MediaResidencyManager> manager(this);
            result.asset->reservePlayback = [manager, weak, generation] {
                const auto entry = weak.lock();
                if (!manager || !entry || entry->generation != generation || !entry->data
                    || (entry->state != QLatin1String("ready") && !entry->validationPlayer)) return false;
                const quint64 pendingBefore = manager->pendingPlaybackBudgetBytes();
                ++entry->activePlayers;
                ++entry->pendingPlayers;
                const bool consumesPinnedSlot = manager->pendingPlaybackBudgetBytes() == pendingBefore;
                // Consuming an admitted scene slot adds no new commitment.
                // Other priming players may already have allocated buffers
                // without publishing a frame, so charging all of their full
                // reservations again can reject an already admitted scene.
                const bool admitted = consumesPinnedSlot
                    ? manager->admitsBudget(entry->data->playbackBudgetBytes, false)
                    : manager->admitsBudget(0);
                if (entry->validationPlayer) entry->validationAdmissionRefused = !admitted;
                if (!admitted) {
                    --entry->activePlayers; --entry->pendingPlayers; return false;
                }
                emit manager->changed();
                return true;
            };
            result.asset->playbackPrepared = [manager, weak, generation] {
                const auto entry = weak.lock();
                if (!manager || !entry || entry->generation != generation) return;
                entry->pendingPlayers = std::max(0, entry->pendingPlayers - 1);
                // The decoder's real allocations are now visible to the OS.
                // Refresh before exposing the released future reservation.
                manager->refreshSystemMemory();
                emit manager->changed();
                QTimer::singleShot(0, manager, &MediaResidencyManager::sampleNow);
            };
            result.asset->releasePlayback = [manager, weak, generation](bool prepared) {
                const auto entry = weak.lock();
                if (!entry || entry->generation != generation) return;
                entry->activePlayers = std::max(0, entry->activePlayers - 1);
                if (!prepared) entry->pendingPlayers = std::max(0, entry->pendingPlayers - 1);
                if (manager) {
                    emit manager->changed();
                    QTimer::singleShot(0, manager, &MediaResidencyManager::sampleNow);
                }
            };
            e->data = std::move(result.asset);
            e->estimated = e->data->residentBytes;
            if (e->data->video) {
                validatePlayback(e);
                return;
            }
            e->state = QStringLiteral("ready"); e->error.clear(); e->progress = 1;
            e->requiresHealthySamples = false;
        }
        publish(e);
        QTimer::singleShot(0, this, &MediaResidencyManager::sampleNow);
    });
    const bool simulatedMemory = m_testMemory;
    const quint64 safetyReserve = reserveBytes();
    watcher->setFuture(QtConcurrent::run([self, e, generation, allowance, cancel, simulatedMemory, safetyReserve]() {
        DecodeResult result;
        MediaDecoder::DecodeCallbacks callbacks;
        callbacks.cancelled = [cancel]() { return cancel->load(); };
        QElapsedTimer budgetClock;
        budgetClock.start();
        qint64 lastBudgetCheck = -250;
        callbacks.reserve = [e, allowance, cancel, simulatedMemory, safetyReserve,
                             &budgetClock, &lastBudgetCheck](quint64 bytes) {
            e->required.store(std::max(e->required.load(), bytes));
            if (cancel->load() || bytes > allowance) return false;
            const quint64 previous = e->budgeted.load();
            const quint64 growth = bytes > previous ? bytes - previous : 0;
            if (!simulatedMemory && (budgetClock.elapsed() - lastBudgetCheck >= 250 || growth >= 64 * MiB)) {
                const auto system = readSystemMemory();
                lastBudgetCheck = budgetClock.elapsed();
                if (system.pressure >= 2 || system.availableBytes < safetyReserve
                    || growth > system.availableBytes - safetyReserve) return false;
            }
            e->budgeted.store(bytes);
            return true;
        };
        callbacks.allocated = [e](quint64 bytes) { e->allocated.store(bytes); };
        callbacks.progress = [self, e, generation](double progress) {
            if (!self) return;
            QMetaObject::invokeMethod(self, [self, e, generation, progress]() {
                if (!self || e->generation != generation || e->cancelled->load()) return;
                e->progress = std::clamp(progress, 0.0, 0.999);
                self->publish(e);
            }, Qt::QueuedConnection);
        };
        result.asset = MediaDecoder::decode(e->path, callbacks, &result.error);
        return result;
    }));
}
void MediaResidencyManager::validatePlayback(const EntryPtr& e) {
    // Keep the existing decoding state until the actual platform player has
    // produced a renderable start image. This gates local imports and receiver
    // residency alike, with one temporary, budgeted decoder per shared asset.
    m_decoding = true;
    e->validationAdmissionRefused = false;
    e->validationPlayer = std::make_unique<ResidentVideoPlayer>(this);
    const QPointer<ResidentVideoPlayer> player(e->validationPlayer.get());
    const std::weak_ptr<Entry> weak = e;
    const quint64 generation = e->generation;
    auto finish = [this, weak, generation](const QString& error) {
        // Never destroy a player from inside one of its multimedia callbacks.
        QMetaObject::invokeMethod(this, [this, weak, generation, error] {
            if (const auto entry = weak.lock()) finishPlaybackValidation(entry, generation, error);
        }, Qt::QueuedConnection);
    };
    connect(player, &ResidentVideoPlayer::frameReady, this, [player, finish](qint64) {
        if (!player) return;
        const auto frame = player->preparedFrame(0);
        if (!frame.isValid()) return;
        finish(frame.toImage().isNull()
            ? QStringLiteral("The video player cannot render this video's first image") : QString());
    });
    connect(player, &ResidentVideoPlayer::errorOccurred, this,
            [finish](QMediaPlayer::Error error, const QString& message) {
        if (error != QMediaPlayer::NoError) finish(message);
    });
    player->setAsset(e->data);
    // Automatic poster loading may defer budget admission. Import validation
    // requires an explicit result so it cannot remain silently pending.
    if (player && player->error() == QMediaPlayer::NoError) player->prepare(0);
}
void MediaResidencyManager::cancelPlaybackValidation(const EntryPtr& e) {
    if (!e->validationPlayer) return;
    auto player = std::move(e->validationPlayer);
    disconnect(player.get(), nullptr, this, nullptr);
    player.reset();
    m_decoding = false;
}
void MediaResidencyManager::finishPlaybackValidation(
    const EntryPtr& e, quint64 generation, const QString& error) {
    if (e->generation != generation || !e->validationPlayer || e->owners.isEmpty()) return;
    const bool waitingForMemory = e->cancelled->load() || e->validationAdmissionRefused;
    QString failure = e->validationPlayer->error() != QMediaPlayer::NoError
        ? e->validationPlayer->errorString() : error;
    if (failure.isEmpty() && e->validationPlayer->error() != QMediaPlayer::NoError)
        failure = QStringLiteral("The video playback engine could not prepare this video");
    cancelPlaybackValidation(e);
    if (waitingForMemory) {
        evict(e);
    } else if (!failure.isEmpty()) {
        e->data.reset();
        e->state = QStringLiteral("error");
        e->error = failure;
        e->progress = 0;
        publish(e);
        const auto owners = e->owners.values();
        for (const auto& id : owners) if (m_owners.contains(id)) emit errorOccurred(id, failure);
    } else {
        e->state = QStringLiteral("ready");
        e->error.clear();
        e->progress = 1;
        e->requiresHealthySamples = false;
        publish(e);
    }
    QTimer::singleShot(0, this, &MediaResidencyManager::sampleNow);
}
void MediaResidencyManager::sampleNow() {
    refreshSystemMemory();
    const int pressure = pressureLevel();
    const qint64 sampleTime = m_clock.elapsed();
    if (!allocationsBlocked()) {
        if (sampleTime - m_lastHealthySampleMs >= 1000) {
            m_healthySamples = std::min(2, m_healthySamples + 1);
            m_lastHealthySampleMs = sampleTime;
        }
    } else {
        m_healthySamples = 0;
        m_lastHealthySampleMs = sampleTime;
    }
    // Use the same hard constraints for admission, recovery and reclamation.
    // Advisory warnings may persist with ample budget for bounded loading.
    if (allocationsBlocked()) {
        for (const auto& e : m_entries) if (e->state == QLatin1String("decoding")) e->cancelled->store(true);
        auto candidates = m_entries;
        std::sort(candidates.begin(), candidates.end(), [](const EntryPtr& a, const EntryPtr& b) {
            const auto aa = a->data ? a->data->residentBytes : 0;
            const auto bb = b->data ? b->data->residentBytes : 0;
            return aa == bb ? a->hash < b->hash : aa > bb;
        });
        quint64 planned = 0;
        const quint64 deficit = m_memory.availableBytes < reserveBytes()
            ? reserveBytes() - m_memory.availableBytes : 0;
        for (const auto& e : candidates) {
            if (!e->data || protectedEntry(e)) continue;
            planned += e->data->residentBytes;
            evict(e);
            if (pressure < 2 && planned >= deficit) break;
        }
        const qint64 now = m_clock.elapsed();
        if (m_pressureSinceMs < 0) m_pressureSinceMs = now;
        if (now - m_pressureSinceMs >= 2000) {
            for (const auto& e : candidates) {
                if (!e->data || !protectedEntry(e)) continue;
                QStringList groups;
                for (auto it = m_pins.cbegin(); it != m_pins.cend(); ++it) {
                    for (const auto& owner : e->owners) if (it->contains(owner)) { groups.append(it.key()); break; }
                }
                for (const auto& group : groups) if (!m_stopRequested.contains(group)) {
                    m_stopRequested.insert(group); emit sceneStopRequested(group);
                }
                break; // next sample measures recovery before stopping more scenes
            }
        }
    } else {
        m_pressureSinceMs = -1;
        schedule();
    }
    emit changed();
}
void MediaResidencyManager::retry(const QString& id) {
    auto it = m_owners.find(id);
    if (it == m_owners.end()) return;
    const auto e = it->entry;
    if (e->state == QLatin1String("error")) {
        // Restart the shared job once. Releasing/reacquiring only this owner
        // would attach straight back to an errored same-path entry while
        // another occurrence still owns it.
        e->cancelled = std::make_shared<std::atomic_bool>(false);
        startProbe(e);
        publish(e);
    } else if (state(id) == QLatin1String("error")) {
        const Owner owner = it.value();
        release(id);
        acquire(id, owner.path, owner.expectedSha256);
    } else {
        sampleNow(); // retries cannot bypass memory admission or hysteresis
    }
}
void MediaResidencyManager::setRemoteState(const QString& sha, const QString& target, const QString& state, double progress, const QString& error) {
    m_remoteStates[sha][target] = {{QStringLiteral("targetId"), target}, {QStringLiteral("state"), state},
        {QStringLiteral("progress"), progress}, {QStringLiteral("error"), error}};
    emit changed();
}
void MediaResidencyManager::clearRemoteStates(const QString& target) {
    for (auto it = m_remoteStates.begin(); it != m_remoteStates.end(); ++it) it->remove(target);
    emit changed();
}
QVariantMap MediaResidencyManager::summary() const {
    quint64 reserved = 0;
    for (const auto& e : m_entries) reserved += e->reserved - std::min(e->reserved, e->allocated.load());
    const quint64 available = std::min(m_memory.availableBytes, m_memory.totalBytes);
    const quint64 process = std::min(m_memory.processBytes, m_memory.totalBytes - available);
    const int pressure = pressureLevel();
    return {{QStringLiteral("totalBytes"), QVariant::fromValue(m_memory.totalBytes)},
        {QStringLiteral("availableBytes"), QVariant::fromValue(available)},
        {QStringLiteral("processBytes"), QVariant::fromValue(process)},
        {QStringLiteral("otherBytes"), QVariant::fromValue(m_memory.totalBytes - available - process)},
        {QStringLiteral("mediaBytes"), QVariant::fromValue(residentBytes())},
        {QStringLiteral("reservedBytes"), QVariant::fromValue(reserved)},
        {QStringLiteral("playbackBudgetBytes"), QVariant::fromValue(playbackBudgetBytes())},
        {QStringLiteral("pendingPlaybackBudgetBytes"), QVariant::fromValue(pendingPlaybackBudgetBytes())},
        {QStringLiteral("loadableBytes"), QVariant::fromValue(loadableBytes())},
        {QStringLiteral("reserveBytes"), QVariant::fromValue(reserveBytes())},
        {QStringLiteral("availableEstimated"), m_memory.availableEstimated},
        {QStringLiteral("pressure"), pressure >= 2 ? QStringLiteral("critical") : (pressure ? QStringLiteral("warning") : QStringLiteral("normal"))}};
}
QVariantList MediaResidencyManager::assets() const {
    auto entries = m_entries;
    std::sort(entries.begin(), entries.end(), [](const EntryPtr& a, const EntryPtr& b) {
        const auto aa = a->data ? a->data->residentBytes : a->allocated.load();
        const auto bb = b->data ? b->data->residentBytes : b->allocated.load();
        return aa == bb ? a->path < b->path : aa > bb;
    });
    QVariantList rows;
    for (const auto& e : entries) {
        auto owners = e->owners.values(); std::sort(owners.begin(), owners.end());
        QVariantList remote;
        const auto values = m_remoteStates.value(e->hash);
        for (const auto& value : values) remote.append(value);
        rows.append(QVariantMap{{QStringLiteral("ownerId"), owners.value(0)},
            {QStringLiteral("assetId"), e->hash}, {QStringLiteral("displayName"), QFileInfo(e->path).fileName()},
            {QStringLiteral("sourcePath"), e->path}, {QStringLiteral("state"), state(owners.value(0))},
            {QStringLiteral("progress"), e->progress}, {QStringLiteral("error"), errorString(owners.value(0))},
            {QStringLiteral("residentBytes"), QVariant::fromValue(e->data ? e->data->residentBytes : e->allocated.load())},
            {QStringLiteral("estimatedBytes"), QVariant::fromValue(e->estimated)},
            {QStringLiteral("preparationBudgetBytes"), QVariant::fromValue(e->estimated + e->scratch)},
            {QStringLiteral("playbackBudgetBytes"), QVariant::fromValue(e->data ? e->data->playbackBudgetBytes * quint64(e->activePlayers) : 0)},
            {QStringLiteral("activePlayers"), e->activePlayers},
            {QStringLiteral("occurrences"), owners.size()}, {QStringLiteral("owners"), owners},
            {QStringLiteral("protected"), protectedEntry(e)}, {QStringLiteral("remoteStates"), remote}});
    }
    return rows;
}
void MediaResidencyManager::setMemorySnapshotForTesting(const MemorySnapshot& snapshot) {
    m_testMemory = true; m_memory = snapshot; m_nativePressure.store(0); sampleNow();
}
void MediaResidencyManager::clearMemorySnapshotForTesting() {
    m_testMemory = false; sampleNow();
}
