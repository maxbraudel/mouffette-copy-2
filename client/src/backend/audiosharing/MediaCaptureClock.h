#pragma once
#include <QtGlobal>
#include <chrono>
#ifdef Q_OS_MACOS
#include <mach/mach_time.h>
#elif defined(Q_OS_WIN)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
// Qt guards NOMINMAX and the Windows SDK target macros; the lean include also
// prevents winsock.h from conflicting with later QtNetwork/winsock2 includes.
#include <qt_windows.h>
#endif

namespace MediaCaptureClock {
// Capture timestamps, UI and audio worker must share the *native media* epoch.
// On macOS steady_clock can include accumulated sleep time while CoreMedia's
// host clock uses mach_absolute_time; anchoring each stream on its first frame
// would conceal capture latency and create different audio/video offsets.
inline qint64 nowUs() {
#ifdef Q_OS_MACOS
    static const mach_timebase_info_data_t timebase = [] {
        mach_timebase_info_data_t value{1, 1};
        mach_timebase_info(&value);
        return value;
    }();
    return qint64(static_cast<long double>(mach_absolute_time()) * timebase.numer
                  / (static_cast<long double>(timebase.denom) * 1000));
#elif defined(Q_OS_WIN)
    // WASAPI GetBuffer returns this same counter converted to 100 ns units.
    // QPC/frequency are guaranteed to exist on supported Windows versions.
    static const qint64 frequency = [] {
        LARGE_INTEGER value{};
        QueryPerformanceFrequency(&value);
        return qint64(value.QuadPart);
    }();
    LARGE_INTEGER counter{};
    QueryPerformanceCounter(&counter);
    const qint64 ticks = counter.QuadPart;
    // Split whole seconds before scaling to avoid multiplying a large uptime
    // counter, while retaining sub-microsecond precision in the remainder.
    return (ticks / frequency) * 1000000
        + qint64(static_cast<long double>(ticks % frequency) * 1000000 / frequency);
#else
    return std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
#endif
}
}
