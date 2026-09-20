#pragma once

#include "backend/media/ResidentMediaAsset.h"
#include <QObject>
#include <QHash>
#include <QSet>
#include <QPointer>
#include <QTimer>
#include <QElapsedTimer>
#include <QVariantList>
#include <QVariantMap>
#include <atomic>
#include <memory>

class QFutureWatcherBase;

// One process-wide authority for validated resident assets. All methods and
// signals are on the application thread; worker jobs only access job atomics.
class MediaResidencyManager final : public QObject
{
    Q_OBJECT
    Q_PROPERTY(QVariantMap summary READ summary NOTIFY changed)
    Q_PROPERTY(QVariantList assets READ assets NOTIFY changed)
public:
    struct MemorySnapshot {
        quint64 totalBytes = 0;
        quint64 availableBytes = 0;
        quint64 processBytes = 0;
        bool availableEstimated = false;
        int pressure = 0; // 0 normal, 1 advisory warning, 2 critical/OS low-memory
        bool pressureKnown = false; // otherwise native notifications are the fallback
        quint64 residentBytes = 0; // RSS, distinct from macOS phys_footprint
    };
    static MediaResidencyManager& instance();
    explicit MediaResidencyManager(QObject* parent = nullptr);
    ~MediaResidencyManager() override;

    // Configure once at startup, before restoring or receiving media.
    void setSafetyReserve(int percent, int minimumMiB);

    void acquire(const QString& ownerId, const QString& path,
                 const QString& expectedSha256 = {});
    void release(const QString& ownerId);
    // Cancellation is asynchronous: cache removal on Windows must wait until
    // the probe/decoder has closed its file handle, including retired entries.
    bool hasBackgroundWorkForPath(const QString& path) const;
    bool ready(const QString& ownerId) const;
    QString state(const QString& ownerId) const;
    double progress(const QString& ownerId) const;
    QString errorString(const QString& ownerId) const;
    QString sha256(const QString& ownerId) const;
    std::shared_ptr<const ResidentMediaAsset> asset(const QString& ownerId) const;
    // Display-only; does not relax asset(), ready(), scene pins or remote state.
    std::shared_ptr<const ResidentMediaPreview> preview(const QString& ownerId) const;
    bool pinOwners(const QStringList& ownerIds, const QString& group);
    void unpinGroup(const QString& group);
    void setRemoteState(const QString& sha256, const QString& targetId,
                        const QString& state, double progress, const QString& error = {});
    void clearRemoteStates(const QString& targetId);
    QVariantMap summary() const;
    QVariantList assets() const;
    Q_INVOKABLE void retry(const QString& ownerId);
    Q_INVOKABLE void sampleNow();

    // Dependency injection for deterministic pressure/admission tests. Never
    // exposed to QML or production settings.
    void setMemorySnapshotForTesting(const MemorySnapshot& snapshot);
    void clearMemorySnapshotForTesting();

signals:
    void ownerChanged(const QString& ownerId);
    void changed();
    void errorOccurred(const QString& ownerId, const QString& message);
    void sceneStopRequested(const QString& group);
    void backgroundWorkFinished(const QString& path);

private:
    struct Entry;
    using EntryPtr = std::shared_ptr<Entry>;
    struct Owner { EntryPtr entry; QString expectedSha256; QString path; QString signature; };
    void startProbe(const EntryPtr& entry);
    void schedule();
    void startDecode(const EntryPtr& entry, quint64 allowance);
    void validatePlayback(const EntryPtr& entry);
    void finishPlaybackValidation(const EntryPtr& entry, quint64 generation, const QString& error);
    void cancelPlaybackValidation(const EntryPtr& entry);
    void publish(const EntryPtr& entry);
    void evict(const EntryPtr& entry);
    bool protectedEntry(const EntryPtr& entry) const;
    quint64 reserveBytes() const;
    int pinnedPlayerCount(const EntryPtr& entry) const;
    quint64 playbackBudgetBytes() const;
    quint64 pendingPlaybackBudgetBytes() const;
    quint64 reservedBudgetBytes() const;
    int pressureLevel() const;
    bool allocationsBlocked() const;
    quint64 loadableBytes() const;
    QString waitingReason(quint64 required) const;
    bool admitsBudget(quint64 additional, bool includeReservations = true);
    void refreshSystemMemory();
    static MemorySnapshot readSystemMemory();
    void setupPressureNotifications();
    void finishBackgroundWork(const QString& path);

    QHash<QString, Owner> m_owners;
    QHash<QString, int> m_backgroundPaths;
    QSet<QFutureWatcherBase*> m_jobs;
    QList<EntryPtr> m_entries;
    QHash<QString, QHash<QString, QVariantMap>> m_remoteStates;
    // Remote occurrences can share an owner ID, but still need separate players.
    QHash<QString, QStringList> m_pins;
    QSet<QString> m_stopRequested;
    QTimer m_timer;
    QElapsedTimer m_clock;
    qint64 m_lastHealthySampleMs = -1000;
    MemorySnapshot m_memory;
    int m_reservePercent = 0;
    int m_reserveMinMiB = 512;
    bool m_testMemory = false;
    bool m_scheduling = false;
    bool m_decoding = false;
    int m_healthySamples = 0;
    qint64 m_pressureSinceMs = -1;
    std::atomic_int m_nativePressure{0};
    void* m_pressureSource = nullptr;
    QPointer<QObject> m_pressureNotifier;
};
