#pragma once

#include "backend/runtime/SuspendInclusiveClock.h"

#include <QByteArray>
#include <QCryptographicHash>
#include <QHash>
#include <QMutex>
#include <QMutexLocker>
#include <QString>

#include <functional>
#include <utility>

// This is only the age/capacity gate for metadata that the caller has already
// proved inactive and physically deleted. It never authorizes dropping an
// intent, provisional proof, failed cleanup or active replay obligation.
// Share one instance with isolated disk workers using std::shared_ptr.
class RemoteCacheHistory final {
public:
    using Clock = std::function<qint64()>;
    static constexpr qint64 RetentionMs = 24LL * 60 * 60 * 1000;
    static constexpr qsizetype MaximumEntries = 4096;

    explicit RemoteCacheHistory(Clock clock = MouffetteClock::nowMs,
                                qint64 retentionMs = RetentionMs,
                                qsizetype capacity = MaximumEntries)
        : m_clock(std::move(clock))
        , m_retentionMs(qMax<qint64>(1, retentionMs))
        , m_capacity(qMax<qsizetype>(1, capacity))
    {}

    bool eligible(const QString& key, const QByteArray& fingerprint)
    {
        if (key.isEmpty() || fingerprint.isEmpty()) return false;
        // Store a fixed-size fingerprint even if the caller supplies JSON.
        const auto digest = QCryptographicHash::hash(fingerprint, QCryptographicHash::Sha256);
        QMutexLocker lock(&m_mutex);
        const qint64 now = m_clock();
        if (now < 0) return false;
        if (m_lastNow >= 0 && now < m_lastNow) {
            // Clock replacement/reset must never make retained proofs older.
            for (auto& entry : m_observations) entry.unchangedSince = now;
        }
        m_lastNow = now;
        auto it = m_observations.find(key);
        if (it == m_observations.end()) {
            // Never evict a tracked proof to make an unobserved one eligible.
            if (m_observations.size() >= m_capacity) return false;
            m_observations.insert(key, {digest, now});
            return false;
        }
        if (it->digest != digest) {
            *it = {digest, now};
            return false;
        }
        return now - it->unchangedSince >= m_retentionMs;
    }

    void forget(const QString& key)
    {
        QMutexLocker lock(&m_mutex);
        m_observations.remove(key);
    }

    qsizetype size() const
    {
        QMutexLocker lock(&m_mutex);
        return m_observations.size();
    }

    qsizetype capacity() const { return m_capacity; }

private:
    struct Observation { QByteArray digest; qint64 unchangedSince; };
    Clock m_clock;
    const qint64 m_retentionMs;
    const qsizetype m_capacity;
    mutable QMutex m_mutex;
    QHash<QString, Observation> m_observations;
    qint64 m_lastNow = -1;
};
