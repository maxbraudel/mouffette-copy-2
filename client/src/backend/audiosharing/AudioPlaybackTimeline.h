#pragma once
#include <QtGlobal>
#include "AudioOutputTiming.h"
#include <algorithm>
#include <cmath>

// Align remote sample timestamps with the device consumption clock. Arrival
// jitter is smoothed; discontinuities recover live instead of dropping forever.
class AudioPlaybackTimeline {
public:
    struct Decision { bool accept = false; bool rebuffer = false; };
    static constexpr qint64 PacketUs = 20000, TargetDelayUs = 80000, MaximumDelayUs = 150000;
    qint64 sourceAt(qint64 localUs) const { return remoteAnchor + localUs - localAnchor; }
    qint64 presentationAt(qint64 sourceUs) const { return localAnchor + sourceUs - remoteAnchor; }
    void reset() { started = false; }
    void setOutputQuantumUs(qint64 quantumUs) {
        const auto quantum = AudioOutputTiming::normalizeQuantumUs(quantumUs);
        const auto change = AudioOutputTiming::extraPlayoutDelayUs(quantum)
            - AudioOutputTiming::extraPlayoutDelayUs(outputQuantumUs);
        outputQuantumUs = quantum;
        if (started) {
            localAnchor += change;
            smoothedMargin += change;
        }
    }
    Decision enqueue(qint64 timestampUs, qint64 arrivalUs, bool full = false, qint64 presentationUs = -1) {
        const auto maximumDelay = AudioOutputTiming::maximumPlayoutDelayUs(outputQuantumUs);
        const auto targetDelay = TargetDelayUs + AudioOutputTiming::extraPlayoutDelayUs(outputQuantumUs);
        if (presentationUs >= 0) {
            // The network uses the fastest audio/video observations of this
            // source clock. Delayed audio cannot establish a later live edge.
            if (presentationUs + PacketUs <= arrivalUs
                || presentationUs - arrivalUs > maximumDelay) return {};
            const bool rebuffer = !started || full
                || std::abs((timestampUs - sourceAt(arrivalUs)) - (presentationUs - arrivalUs)) > maximumDelay;
            remoteAnchor = timestampUs;
            localAnchor = presentationUs;
            lastArrival = arrivalUs;
            lastTimestamp = timestampUs;
            smoothedMargin = presentationUs - arrivalUs;
            started = true;
            latePackets = 0;
            return {true, rebuffer};
        }
        bool rebuffer = !started || full || arrivalUs - lastArrival > 300000
            || timestampUs < lastTimestamp - 1000000;
        const qint64 margin = started ? timestampUs - sourceAt(arrivalUs) : targetDelay;
        if (margin > maximumDelay || margin < -maximumDelay) rebuffer = true;
        lastArrival = arrivalUs; lastTimestamp = timestampUs;
        if (!rebuffer && margin + PacketUs <= 0) {
            if (++latePackets < 3) return {};
            rebuffer = true;
        }
        if (rebuffer) {
            started = true; remoteAnchor = timestampUs; localAnchor = arrivalUs + targetDelay;
            smoothedMargin = targetDelay; latePackets = 0;
            return {true, true};
        }
        latePackets = 0;
        smoothedMargin += (double(margin) - smoothedMargin) / 32;
        // Small clock/rate drift correction stays below 1.25% per 20 ms frame,
        // with a deadband so packet arrival jitter does not wobble playback.
        if (smoothedMargin < targetDelay - 20000)
            localAnchor += std::min<qint64>(250, qint64((targetDelay - smoothedMargin) / 64));
        else if (smoothedMargin > targetDelay + 20000)
            localAnchor -= std::min<qint64>(250, qint64((smoothedMargin - targetDelay) / 64));
        return {true, false};
    }
private:
    qint64 outputQuantumUs = AudioOutputTiming::DefaultQuantumUs;
    bool started = false;
    qint64 localAnchor = 0, remoteAnchor = 0, lastArrival = 0, lastTimestamp = 0;
    double smoothedMargin = TargetDelayUs;
    int latePackets = 0;
};
