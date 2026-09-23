#pragma once
#include <QByteArray>
#include <array>
#include <algorithm>
#include <cmath>
#include <optional>
#include <opus.h>

// Buffers the native system capture after AudioCaptureResampler clock correction.
// The fixed collection window is a deadline, never a reason to splice raw
// callback timestamps into PCM. Missing input fades to silence.
class AudioCaptureBuffer {
public:
    static constexpr int Rate = 48000, PacketFrames = 960, Capacity = Rate / 2;
    static constexpr int FadeFrames = Rate / 200; // 5 ms, same envelope for L/R.
    static constexpr qint64 CollectionUs = 60000;
    struct Packet { QByteArray pcm; qint64 timestampUs; bool discontinuity; };
    void reset(qint64 nowUs) {
        next = frameAt(nowUs); discontinuous = true;
        envelope = {}; softClip = {};
        for (auto& slot : ring) slot.frame = -1;
    }
    void clear() { next = -1; }
    void append(const float* pcm, int frames, qint64 timestampUs, bool transition = false) {
        if (next < 0 || !pcm || frames <= 0 || timestampUs < 0) return;
        const auto start = frameAt(timestampUs);
        const auto first = std::max<qint64>(0, next - start);
        const auto last = std::min<qint64>(frames, next + Capacity - start);
        for (auto i = first; i < last; ++i) {
            const auto frame = start + i;
            auto& slot = ring[size_t(frame % Capacity)];
            if (slot.frame != frame) { slot = {}; slot.frame = frame; }
            slot.transition |= transition && i == first;
            for (int channel = 0; channel < 2; ++channel) {
                const float value = pcm[i * 2 + channel];
                const float clean = std::isfinite(value) ? value : 0.0f;
                slot.pcm[channel] = clean;
            }
        }
    }
    std::optional<Packet> take(qint64 nowUs) {
        if (next < 0) return {};
        const auto ready = frameAt(nowUs - CollectionUs);
        // A blocked consumer resumes live. The receiver sees a timestamp gap;
        // the new source samples start softly instead of a full-level jump.
        if (ready - next > Rate / 10) {
            next = ready - PacketFrames; discontinuous = true;
            envelope = {}; softClip = {};
        }
        if (next + PacketFrames > ready) return {};
        Packet result{QByteArray(PacketFrames * 2 * sizeof(float), Qt::Uninitialized),
            next * 1000000 / Rate, discontinuous};
        discontinuous = false;
        auto* output = reinterpret_cast<float*>(result.pcm.data());
        for (int i = 0; i < PacketFrames; ++i, ++next) {
            const auto& slot = ring[size_t(next % Capacity)];
            const bool valid = slot.frame == next;
            const auto sample = envelope.sample(slot.pcm, valid, valid && slot.transition);
            for (int channel = 0; channel < 2; ++channel)
                output[i * 2 + channel] = sample[channel];
        }
        // Stateful soft clipping preserves continuity for native levels over unity.
        opus_pcm_soft_clip(output, PacketFrames, 2, softClip.data());
        return result;
    }
private:
    struct Envelope {
        std::array<float, 2> last{}, from{};
        bool present = false;
        int remaining = 0;
        std::array<float, 2> sample(const float* input, bool available, bool transition) {
            if (available != present || transition) { from = last; remaining = FadeFrames; }
            const float weight = remaining
                ? float(0.5 + 0.5 * std::cos(3.14159265358979323846 * (FadeFrames - remaining) / FadeFrames)) : 0;
            for (int channel = 0; channel < 2; ++channel)
                last[channel] = (available ? input[channel] : 0) * (1 - weight) + from[channel] * weight;
            if (remaining) --remaining;
            present = available; return last;
        }
    };
    struct Sample {
        qint64 frame = -1;
        float pcm[2]{};
        bool transition = false;
    };
    std::array<Sample, Capacity> ring;
    Envelope envelope;
    std::array<float, 2> softClip{};
    qint64 next = -1;
    bool discontinuous = true;
    static qint64 frameAt(qint64 time) { return qRound64(time * (Rate / 1000000.0)); }

};
