#pragma once
#include <QtGlobal>
#include <algorithm>
#include <cmath>

// Align remote sample timestamps with the helper's consumption clock. Arrival
// jitter is smoothed; discontinuities recover live instead of dropping forever.
class AudioPlaybackTimeline {
public:
    struct Decision { bool accept = false; bool rebuffer = false; };
    static constexpr qint64 PacketUs = 20000, TargetDelayUs = 80000, MaximumDelayUs = 150000;
    qint64 sourceAt(qint64 localUs) const { return remoteAnchor + localUs - localAnchor; }
    void reset() { started = false; }
    Decision enqueue(qint64 timestampUs, qint64 arrivalUs, bool full = false) {
        bool rebuffer = !started || full || arrivalUs - lastArrival > 300000
            || timestampUs < lastTimestamp - 1000000;
        const qint64 margin = started ? timestampUs - sourceAt(arrivalUs) : TargetDelayUs;
        if (margin > MaximumDelayUs || margin < -MaximumDelayUs) rebuffer = true;
        lastArrival = arrivalUs; lastTimestamp = timestampUs;
        if (!rebuffer && margin + PacketUs <= 0) {
            if (++latePackets < 3) return {};
            rebuffer = true;
        }
        if (rebuffer) {
            started = true; remoteAnchor = timestampUs; localAnchor = arrivalUs + TargetDelayUs;
            smoothedMargin = TargetDelayUs; latePackets = 0;
            return {true, true};
        }
        latePackets = 0;
        smoothedMargin += (double(margin) - smoothedMargin) / 32;
        // Small clock/rate drift correction stays below 1.25% per 20 ms frame,
        // with a deadband so packet arrival jitter does not wobble playback.
        if (smoothedMargin < 60000)
            localAnchor += std::min<qint64>(250, qint64((TargetDelayUs - smoothedMargin) / 64));
        else if (smoothedMargin > 100000)
            localAnchor -= std::min<qint64>(250, qint64((smoothedMargin - TargetDelayUs) / 64));
        return {true, false};
    }
private:
    bool started = false;
    qint64 localAnchor = 0, remoteAnchor = 0, lastArrival = 0, lastTimestamp = 0;
    double smoothedMargin = TargetDelayUs;
    int latePackets = 0;
};
