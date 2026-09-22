#pragma once
#include <QByteArray>
#include <array>
#include <algorithm>
#include <cmath>
#include <optional>

// Both inputs use MediaCaptureClock, not callback arrival time. A short fixed
// collection window lets native capture and scene output arrive independently.
// An absent input contributes silence, including when only a scene is playing.
class AudioCaptureMix {
public:
    static constexpr int Rate = 48000, PacketFrames = 960, Capacity = Rate / 2;
    static constexpr qint64 CollectionUs = 60000;
    struct Packet { QByteArray pcm; qint64 timestampUs; bool discontinuity; };
    void reset(qint64 nowUs) {
        next = frameAt(nowUs); discontinuous = true;
        for (auto& slot : ring) slot.frame = -1;
    }
    void clear() { next = -1; }
    void appendSystem(const float* pcm, int frames, qint64 timestampUs) { append(pcm, frames, timestampUs, false); }
    void appendScene(const float* pcm, int frames, qint64 timestampUs) { append(pcm, frames, timestampUs, true); }
    std::optional<Packet> take(qint64 nowUs) {
        if (next < 0) return {};
        const auto ready = frameAt(nowUs - CollectionUs);
        // A blocked GUI resumes live; never emit a burst of obsolete audio.
        if (ready - next > Rate / 10) { next = ready - PacketFrames; discontinuous = true; }
        if (next + PacketFrames > ready) return {};
        Packet result{QByteArray(PacketFrames * 2 * sizeof(float), Qt::Uninitialized),
            next * 1000000 / Rate, discontinuous};
        discontinuous = false;
        auto* output = reinterpret_cast<float*>(result.pcm.data());
        for (int i = 0; i < PacketFrames; ++i, ++next) {
            auto& slot = ring[size_t(next % Capacity)];
            for (int channel = 0; channel < 2; ++channel)
                output[i * 2 + channel] = slot.frame == next
                    ? std::clamp(slot.system[channel] + slot.scene[channel], -1.0f, 1.0f) : 0.0f;
        }
        return result;
    }
private:
    struct Sample { qint64 frame = -1; float system[2]{}, scene[2]{}; };
    std::array<Sample, Capacity> ring;
    qint64 next = -1;
    bool discontinuous = true;
    static qint64 frameAt(qint64 time) { return qRound64(time * (Rate / 1000000.0)); }
    void append(const float* pcm, int frames, qint64 timestampUs, bool scene) {
        if (next < 0 || !pcm || frames <= 0 || timestampUs < 0) return;
        const auto start = frameAt(timestampUs);
        const auto first = std::max<qint64>(0, next - start);
        const auto last = std::min<qint64>(frames, next + Capacity - start);
        for (auto i = first; i < last; ++i) {
            const auto frame = start + i;
            auto& slot = ring[size_t(frame % Capacity)];
            if (slot.frame != frame) { slot = {}; slot.frame = frame; }
            for (int channel = 0; channel < 2; ++channel) {
                const float value = pcm[i * 2 + channel];
                const float clean = std::isfinite(value) ? value : 0.0f;
                if (scene) slot.scene[channel] += clean;
                else slot.system[channel] = clean; // Native overlap must not double the level.
            }
        }
    }
};
