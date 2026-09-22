#pragma once
#include <QtGlobal>
#include <algorithm>

// Remote and local monotonic clocks have different epochs. Keep their best
// observed offset, rather than making a delayed TCP burst the new live edge.
// Video can supply the same source clock before the first audio packet arrives.
class AudioPacketFreshness {
public:
    static constexpr qint64 MaximumLagUs = 150000;
    void reset() { started = false; driftRemainder = 0; }
    qint64 localTimeUs(qint64 sourceUs) const {
        return started && sourceUs >= 0 ? sourceUs + bestOffset : -1;
    }
    bool observe(qint64 sourceUs, qint64 arrivalUs) {
        if (sourceUs < 0 || arrivalUs < 0) return false;
        const qint64 offset = arrivalUs - sourceUs;
        if (!started) { started = true; bestOffset = offset; lastArrival = arrivalUs; }
        else {
            // Permit 200 ppm of clock drift, never an abrupt network delay.
            // Audio and several video streams may observe this clock much
            // more frequently than every 5 ms. Keep the fractional allowance
            // so frequent observations cannot silently disable clock drift.
            const qint64 driftTicks = driftRemainder + std::max<qint64>(0, arrivalUs - lastArrival);
            const qint64 drift = driftTicks / 5000;
            const qint64 candidate = std::min(offset, bestOffset + drift);
            lastArrival = std::max(lastArrival, arrivalUs);
            // A rejected backlog must not move its own presentation deadline.
            if (offset - candidate > MaximumLagUs) return false;
            bestOffset = candidate;
            driftRemainder = candidate == offset ? 0 : driftTicks % 5000;
        }
        return offset - bestOffset <= MaximumLagUs;
    }
private:
    bool started = false;
    qint64 bestOffset = 0, lastArrival = 0, driftRemainder = 0;
};
