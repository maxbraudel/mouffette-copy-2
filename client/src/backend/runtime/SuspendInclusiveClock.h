#pragma once

#include <QtGlobal>
#include <QDateTime>
#include <limits>
#if defined(Q_OS_MACOS)
#include <mach/mach_time.h>
#elif defined(Q_OS_WIN)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#elif defined(Q_OS_LINUX)
#include <time.h>
#else
#include <chrono>
#endif

// Durations use one monotonic, suspend-inclusive time domain. Civil timestamps
// are only for persistence and presentation, never for an authority deadline.
namespace MouffetteClock {
inline qint64 nowMs()
{
#if defined(Q_OS_MACOS)
    static const mach_timebase_info_data_t timebase = [] {
        mach_timebase_info_data_t value{};
        mach_timebase_info(&value);
        return value;
    }();
    if (!timebase.denom) return -1;
    const __uint128_t milliseconds = static_cast<__uint128_t>(mach_continuous_time())
        * timebase.numer / timebase.denom / 1'000'000U;
    return milliseconds > static_cast<__uint128_t>(std::numeric_limits<qint64>::max())
        ? std::numeric_limits<qint64>::max() : static_cast<qint64>(milliseconds);
#elif defined(Q_OS_WIN)
    return static_cast<qint64>(GetTickCount64());
#elif defined(Q_OS_LINUX)
    timespec value{};
    if (clock_gettime(CLOCK_BOOTTIME, &value) != 0) return -1;
    return static_cast<qint64>(value.tv_sec) * 1000 + value.tv_nsec / 1'000'000;
#else
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
#endif
}
// Persisted project records and UI countdowns retain epoch-shaped timestamps.
// Capture the civil anchor once: wall-clock changes cannot extend a running
// inactivity deadline. A new process reconciles durable records at startup.
inline qint64 anchoredEpochMs()
{
    static const qint64 civilAnchor = QDateTime::currentMSecsSinceEpoch();
    static const qint64 monotonicAnchor = nowMs();
    return civilAnchor + qMax<qint64>(0, nowMs() - monotonicAnchor);
}

class ElapsedTimer {
public:
    void start() { m_startedAt = nowMs(); }
    qint64 restart() { const auto age = elapsed(); start(); return age; }
    void invalidate() { m_startedAt = -1; }
    bool isValid() const { return m_startedAt >= 0; }
    qint64 elapsed() const { return isValid() ? qMax<qint64>(0, nowMs() - m_startedAt) : 0; }
private:
    qint64 m_startedAt = -1;
};
}
