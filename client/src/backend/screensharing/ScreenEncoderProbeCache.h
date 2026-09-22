#pragma once

#include <QByteArray>
#include <QHash>
#include <QMutex>
#include <QMutexLocker>
#include <QSize>

// Capture profile changes replace encoder objects, and several screens/layers
// can probe the same unavailable GPU. Keep failed probes outside those objects
// without permanently disabling a driver which can recover during the session.
class ScreenEncoderProbeCache final {
public:
    static constexpr qint64 RetryIntervalMs = 60000;

    struct Configuration {
        QByteArray backend;
        QSize size;
        int pixelFormat;
        bool operator==(const Configuration& other) const {
            return backend == other.backend && size == other.size && pixelFormat == other.pixelFormat;
        }
        friend size_t qHash(const Configuration& value, size_t seed = 0) {
            return qHashMulti(seed, value.backend, value.size.width(), value.size.height(), value.pixelFormat);
        }
    };

    template<typename Open>
    int open(const Configuration& configuration, qint64 monotonicMs, Open&& tryOpen) {
        // Serialize probing so simultaneous screen workers also share a failed
        // attempt. A successful codec context is never shared or cached.
        QMutexLocker lock(&m_mutex);
        // Capture sizes can change throughout a long session. Retain only the
        // current cooldown window, including failures for inputs never reused.
        for (auto it = m_failures.begin(); it != m_failures.end();) {
            if (monotonicMs - it->timestampMs >= RetryIntervalMs) it = m_failures.erase(it);
            else ++it;
        }
        const auto previous = m_failures.constFind(configuration);
        if (previous != m_failures.cend()
            && monotonicMs - previous->timestampMs < RetryIntervalMs)
            return previous->status;

        const int status = tryOpen();
        if (status < 0) m_failures.insert(configuration, {monotonicMs, status});
        else m_failures.remove(configuration);
        return status;
    }

private:
    friend class ScreenStreamCodecTest;
    struct Failure {
        qint64 timestampMs;
        int status;
    };
    QMutex m_mutex;
    QHash<Configuration, Failure> m_failures;
};
