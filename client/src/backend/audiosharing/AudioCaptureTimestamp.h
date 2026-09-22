#pragma once
#include <QtGlobal>
#include <algorithm>

// Native timestamps name the first sample, whereas callback arrival follows
// capture of the entire block. A missing timestamp must never turn buffer
// duration (or a scheduling stall) into an A/V clock offset.
class AudioCaptureTimestamp {
public:
    struct Result { qint64 timestampUs; bool discontinuity; };
    Result map(qint64 nativeUs, int frames, qint64 arrivalUs, bool discontinuity = false) {
        const qint64 durationUs = qint64(frames) * 1000000 / 48000;
        if (nativeUs < 0) {
            if (anchorUs >= 0 && !discontinuity && arrivalUs >= lastArrivalUs
                && arrivalUs - lastArrivalUs <= 100000)
                nativeUs = anchorUs + elapsedFrames * 1000000 / 48000;
            else {
                nativeUs = std::max<qint64>(0, arrivalUs - durationUs); discontinuity = true;
                anchorUs = nativeUs; elapsedFrames = 0;
            }
        } else { anchorUs = nativeUs; elapsedFrames = 0; }
        // Preserve the fractional duration of 128/256/512-frame callbacks.
        // Rounding every block separately would drift by 225 ms/hour at 512.
        elapsedFrames += frames;
        lastArrivalUs = arrivalUs;
        return {nativeUs, discontinuity};
    }
private:
    qint64 anchorUs = -1, elapsedFrames = 0, lastArrivalUs = 0;
};
