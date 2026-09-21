#pragma once
#include <QtGlobal>
#include <chrono>

namespace MediaCaptureClock {
// The OS monotonic clock has the same epoch in the UI and audio worker.
inline qint64 nowUs() {
    return std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}
}
