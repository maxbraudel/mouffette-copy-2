#pragma once
#include <QtGlobal>
#include <algorithm>

// Device buffering and network jitter are different budgets. Large native
// callbacks need PCM for their entire quantum before the first sample is
// submitted; treating every device as a 10/20 ms callback can starve forever.
// These limits share one policy across audio, transport and video scheduling.
namespace AudioOutputTiming {
inline constexpr qint64 DefaultQuantumUs = 20000;
inline constexpr qint64 MaximumQuantumUs = 100000;
inline constexpr qint64 NormalHeadroomUs = 60000;
inline constexpr qint64 NormalMaximumDelayUs = 150000;
constexpr qint64 normalizeQuantumUs(qint64 quantumUs) {
    return quantumUs <= 0 ? DefaultQuantumUs : std::min(quantumUs, MaximumQuantumUs);
}
constexpr qint64 recoveryRunwayUs(qint64 quantumUs) {
    return std::max<qint64>(40000, normalizeQuantumUs(quantumUs));
}
constexpr qint64 requiredHeadroomUs(qint64 quantumUs) {
    const auto quantum = normalizeQuantumUs(quantumUs);
    return quantum <= DefaultQuantumUs ? NormalHeadroomUs
        : quantum + recoveryRunwayUs(quantum) + 20000;
}
constexpr qint64 extraPlayoutDelayUs(qint64 quantumUs) {
    return requiredHeadroomUs(quantumUs) - NormalHeadroomUs;
}
constexpr qint64 minimumPlayoutDelayUs(qint64 quantumUs) {
    return 80000 + extraPlayoutDelayUs(quantumUs);
}
constexpr qint64 initialPlayoutDelayUs(qint64 quantumUs) {
    return 120000 + extraPlayoutDelayUs(quantumUs);
}
constexpr qint64 maximumPlayoutDelayUs(qint64 quantumUs) {
    return NormalMaximumDelayUs + extraPlayoutDelayUs(quantumUs);
}
inline constexpr qint64 MaximumPlayoutDelayUs = maximumPlayoutDelayUs(MaximumQuantumUs);
}
