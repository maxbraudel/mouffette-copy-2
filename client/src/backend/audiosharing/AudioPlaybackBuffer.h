#pragma once
#include <QtGlobal>
#include "AudioOutputTiming.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstring>

// Exactly one decoder producer and one device callback consumer. PCM storage is
// fixed and owned by the stream; neither side allocates, frees or takes a lock.
class AudioPlaybackBuffer {
public:
    static constexpr int Frames = 960, Channels = 2;
    // Cover the largest permitted device budget plus two packet boundaries.
    // Normal devices still consume at their smaller scheduled latency.
    static constexpr int Capacity = int((AudioOutputTiming::MaximumPlayoutDelayUs + 19999) / 20000) + 2;
    struct Block {
        std::array<float, Frames * Channels> pcm{};
        qint64 timestampUs = 0, sourceTimestampUs = 0, presentationUs = 0;
        bool discontinuity = false;
    };
    bool push(const float* pcm, qint64 timestampUs, qint64 presentationUs, bool discontinuity = false) {
        // Native timestamps describe capture time, not an exact sample index.
        // Keep the source clock separately: small timestamp wobble/drift must
        // never cut or duplicate PCM at the 960-frame packet boundaries.
        const qint64 delta = timestampUs - m_lastSourceUs;
        m_sampleTimestampUs = m_lastSourceUs < 0 || discontinuity ? timestampUs : m_sampleTimestampUs +
            (std::abs(delta - 20000) <= 2000 ? 20000 : delta);
        m_lastSourceUs = timestampUs;
        const auto write = m_write.load(std::memory_order_relaxed);
        if (write - m_read.load(std::memory_order_acquire) >= Capacity) return false;
        auto& block = m_blocks[write % Capacity];
        std::memcpy(block.pcm.data(), pcm, sizeof(block.pcm));
        block.timestampUs = m_sampleTimestampUs; block.sourceTimestampUs = timestampUs; block.presentationUs = presentationUs;
        block.discontinuity = discontinuity;
        m_write.store(write + 1, std::memory_order_release);
        return true;
    }
    // Producer requests a drop without ever changing the consumer's cursor.
    // Existing PCM remains protected until the next callback acknowledges it.
    void requestDiscardQueued() {
        m_discardThrough.store(m_write.load(std::memory_order_relaxed), std::memory_order_release);
    }
    // Consumer only, BEFORE obtaining any block pointers for this callback.
    // Never call from peek(): a lookahead peek must not release a block which
    // the resampler is still reading while the producer fills free slots.
    bool applyDiscardRequest() {
        const auto through = m_discardThrough.load(std::memory_order_acquire);
        if (through <= m_read.load(std::memory_order_relaxed)) return false;
        m_read.store(through, std::memory_order_release);
        return true;
    }
    const Block* peek(size_t offset = 0) const {
        const auto read = m_read.load(std::memory_order_relaxed);
        if (m_write.load(std::memory_order_acquire) - read <= offset) return nullptr;
        return &m_blocks[(read + offset) % Capacity];
    }
    void pop() { m_read.fetch_add(1, std::memory_order_release); }
    size_t size() const {
        // Producer/main-thread observation: read first, then its own write.
        const auto read = m_read.load(std::memory_order_acquire);
        return m_write.load(std::memory_order_acquire) - read;
    }
private:
    std::array<Block, Capacity> m_blocks;
    qint64 m_lastSourceUs = -1, m_sampleTimestampUs = 0; // Producer only.
    alignas(64) std::atomic<size_t> m_write{0};
    alignas(64) std::atomic<size_t> m_read{0};
    alignas(64) std::atomic<size_t> m_discardThrough{0};
};

// A band-limited fractional-delay filter, prepared on the producer thread.
// Scaling the tap count with downsampling keeps low-rate device fallbacks from
// folding high frequencies into audible aliases. The callback uses table reads
// and multiply-adds only; no filter design or allocation happens there.
class AudioPlaybackResampler {
public:
    static constexpr int Phases = 256, MaximumTaps = 192;
    explicit AudioPlaybackResampler(int rate = 48000) { configure(rate); }
    int sampleRate() const { return m_rate; }
    void configure(int rate) {
        m_rate = rate;
        const double ratio = std::min(1.0, double(rate) / 48000);
        m_taps = std::clamp(int(std::ceil(32 / ratio / 2)) * 2, 32, MaximumTaps);
        const double cutoff = ratio * 0.90;
        constexpr double pi = 3.14159265358979323846;
        for (int phase = 0; phase < Phases; ++phase) {
            double sum = 0;
            for (int tap = 0; tap < m_taps; ++tap) {
                const double distance = tap + 1 - m_taps / 2 - double(phase) / Phases;
                const double angle = pi * distance * cutoff;
                const double sinc = std::abs(angle) < 1e-12 ? 1 : std::sin(angle) / angle;
                const double windowAngle = pi * distance / (m_taps / 2);
                const double window = std::abs(distance) >= m_taps / 2 ? 0 :
                    0.42 + 0.5 * std::cos(windowAngle) + 0.08 * std::cos(2 * windowAngle);
                m_coefficients[phase][tap] = float(sinc * cutoff * window);
                sum += m_coefficients[phase][tap];
            }
            for (int tap = 0; tap < m_taps; ++tap) m_coefficients[phase][tap] /= float(sum);
        }
    }
    int taps() const { return m_taps; }
    const float* coefficients(double fraction) const {
        return m_coefficients[std::clamp(int(fraction * Phases), 0, Phases - 1)].data();
    }
private:
    std::array<std::array<float, MaximumTaps>, Phases> m_coefficients{};
    int m_rate = 48000, m_taps = 32;
};

// Device-thread state. Packet arrivals only set a desired clock mapping; they
// cannot move the sample cursor. A small, continuous rate correction tracks
// drift. Large discontinuities restart with a short crossfade.
class AudioPlaybackRenderer {
public:
    struct Clock { qint64 sourceUs = -1, localUs = -1; };
    static constexpr qint64 RecoveryRunwayUs = 40000;
    static constexpr int FadeDurationMs = 5;
    explicit AudioPlaybackRenderer(int rate = 48000) : m_resampler(rate) {}
    void configure(int rate) { if (m_resampler.sampleRate() != rate) m_resampler.configure(rate); }
    int takeUnderruns() { return m_underruns.exchange(0, std::memory_order_relaxed); }
    int takeRebuffers() { return m_rebuffers.exchange(0, std::memory_order_relaxed); }
    Clock render(AudioPlaybackBuffer& queue, float* output, int frames, int channels,
                 int rate, qint64 presentationUs,
                 qint64 outputQuantumUs = AudioOutputTiming::DefaultQuantumUs) {
        Clock result;
        const auto requiredRunway = AudioOutputTiming::recoveryRunwayUs(outputQuantumUs);
        if (!output || frames <= 0 || channels <= 0 || rate < 8000) return result;
        if (queue.applyDiscardRequest()) {
            beginFade(rate);
            m_recovering = true;
            m_available = false;
            m_previousEndUs = -1;
            m_seenBlockUs = -1;
            m_rebuffers.fetch_add(1, std::memory_order_relaxed);
        }
        const auto* first = queue.peek();
        if (first && !m_started && presentationUs + qint64(frames) * 1000000 / rate > first->presentationUs) {
            m_positionUs = first->timestampUs + double(presentationUs - first->presentationUs);
            m_started = true;
        }
        if (!m_started) return result;
        if (first) {
            const double desired = first->timestampUs + double(presentationUs - first->presentationUs);
            const double error = desired - m_positionUs;
            if (std::abs(error) > 100000 || (!m_available && std::abs(error) > 5000)) {
                m_rebuffers.fetch_add(1, std::memory_order_relaxed);
                m_positionUs = desired;
                beginFade(rate);
                m_recovering = true;
            }
            // While rebuffering, follow the deadline exactly. Waiting for a
            // second packet must never turn its late arrival into added latency.
            if (m_recovering) m_positionUs = desired;
            // +/-0.5%, with a two-second phase loop. Unlike packet-by-packet
            // timestamp jumps, this preserves waveform phase and sample order.
            m_speed = m_recovering ? 1.0 : 1.0 + std::clamp(error / 2000000.0, -0.005, 0.005);
        }
        const double stepUs = 1000000.0 / rate * m_speed;
        for (int frame = 0; frame < frames; ++frame) {
            const auto* block = queue.peek();
            while (block && block->timestampUs + 20000 <= m_positionUs) {
                std::copy(block->pcm.end() - m_previous.size(), block->pcm.end(), m_previous.begin());
                m_previousEndUs = block->timestampUs + 20000;
                queue.pop(); block = queue.peek();
            }
            if (block && block->timestampUs != m_seenBlockUs) {
                const auto mapping = block->presentationUs - block->timestampUs;
                if (block->discontinuity || (m_seenBlockUs >= 0 && std::abs(mapping - m_mappingUs) > 15000)) {
                    // A deliberate jitter-target change is a new deadline,
                    // not oscillator drift. Rebuffer once with a short fade;
                    // slow rate correction would miss packets for seconds.
                    m_positionUs = block->timestampUs + double(presentationUs +
                        qint64(frame) * 1000000 / rate - block->presentationUs);
                    beginFade(rate); m_recovering = true;
                    m_previousEndUs = -1;
                    m_rebuffers.fetch_add(1, std::memory_order_relaxed);
                }
                m_seenBlockUs = block->timestampUs; m_mappingUs = mapping;
            }
            std::array<float, 2> sample{};
            const bool containsPosition = block && m_positionUs >= block->timestampUs
                && m_positionUs < block->timestampUs + 20000;
            // An isolated packet is not a stable playout reservoir. Startup and
            // recovery require at least two packets (or one full device quantum);
            // otherwise remain silent instead of chopping every packet into a
            // short burst. This never extends the externally assigned deadline.
            if (m_recovering && containsPosition && contiguousRunwayUs(queue, m_positionUs)
                    + 1000000.0 / rate >= requiredRunway)
                m_recovering = false;
            const bool available = containsPosition && !m_recovering;
            if (available) {
                const double position = (m_positionUs - block->timestampUs) * (48000.0 / 1000000);
                const int index = std::clamp(int(position), 0, AudioPlaybackBuffer::Frames - 1);
                const float fraction = float(std::clamp(position - index, 0.0, 1.0));
                const auto taps = m_resampler.taps();
                const auto* coefficients = m_resampler.coefficients(fraction);
                const auto* next = index + taps / 2 >= AudioPlaybackBuffer::Frames ? queue.peek(1) : nullptr;
                // Filter lookahead must not leak the new signal before a
                // discontinuity's fade begins at the packet boundary.
                const bool contiguousNext = next && !next->discontinuity
                    && std::abs(next->timestampUs - block->timestampUs - 20000) <= 2
                    && std::abs((next->presentationUs - next->timestampUs)
                        - (block->presentationUs - block->timestampUs)) <= 15000;
                for (int channel = 0; channel < 2; ++channel) {
                    auto at = [&](int atFrame) {
                        if (atFrame < 0) return m_previousEndUs == block->timestampUs
                            ? m_previous[(AudioPlaybackResampler::MaximumTaps + atFrame) * 2 + channel] : block->pcm[channel];
                        if (atFrame >= AudioPlaybackBuffer::Frames)
                            return contiguousNext ? next->pcm[(atFrame - AudioPlaybackBuffer::Frames) * 2 + channel]
                                                  : block->pcm[(AudioPlaybackBuffer::Frames - 1) * 2 + channel];
                        return block->pcm[atFrame * 2 + channel];
                    };
                    for (int tap = 0; tap < taps; ++tap)
                        sample[channel] += coefficients[tap] * at(index + tap + 1 - taps / 2);
                }
                if (!m_available) beginFade(rate);
                // A clock describes an actual rendered sample and its local
                // estimated presentation time, never the future callback tail.
                result = {block->sourceTimestampUs + qRound64(m_positionUs - block->timestampUs),
                          presentationUs + qint64(frame) * 1000000 / rate};
            } else if (m_available) {
                m_underruns.fetch_add(1, std::memory_order_relaxed);
                beginFade(rate); m_recovering = true;
            }
            if (m_fadeFrames > 0) {
                // Raised-cosine endpoints have zero slope. In particular a
                // non-zero last sample cannot become an impulse when a queue
                // empties or its deadline changes.
                constexpr double pi = 3.14159265358979323846;
                const float weight = float(0.5 - 0.5 * std::cos(pi * double(m_fadeFrames) / m_fadeLength));
                for (int channel = 0; channel < 2; ++channel)
                    sample[channel] = sample[channel] * (1 - weight) + m_fadeFrom[channel] * weight;
                --m_fadeFrames;
            }
            if (channels == 1) output[frame] += 0.5f * (sample[0] + sample[1]);
            else { output[frame * channels] += sample[0]; output[frame * channels + 1] += sample[1]; }
            m_last = sample; m_available = available; m_positionUs += stepUs;
        }
        return result;
    }
private:
    static double contiguousRunwayUs(const AudioPlaybackBuffer& queue, double positionUs) {
        const auto* block = queue.peek();
        if (!block) return 0;
        auto endUs = block->timestampUs + 20000;
        const auto mapping = block->presentationUs - block->timestampUs;
        for (size_t offset = 1; offset < AudioPlaybackBuffer::Capacity; ++offset) {
            const auto* next = queue.peek(offset);
            if (!next || next->discontinuity || std::abs(next->timestampUs - endUs) > 2
                || std::abs((next->presentationUs - next->timestampUs) - mapping) > 15000) break;
            endUs = next->timestampUs + 20000;
        }
        return double(endUs) - positionUs;
    }
    void beginFade(int rate) {
        m_fadeLength = std::max(1, rate * FadeDurationMs / 1000);
        m_fadeFrames = m_fadeLength;
        m_fadeFrom = m_last;
    }
    AudioPlaybackResampler m_resampler;
    std::atomic<int> m_underruns{0}, m_rebuffers{0};
    bool m_started = false, m_available = false, m_recovering = true;
    double m_positionUs = 0, m_speed = 1;
    std::array<float, AudioPlaybackResampler::MaximumTaps * 2> m_previous{};
    std::array<float, 2> m_last{}, m_fadeFrom{};
    qint64 m_previousEndUs = -1, m_seenBlockUs = -1, m_mappingUs = 0;
    int m_fadeFrames = 0, m_fadeLength = 1;
};

// Correlated clock publication without a callback-side mutex. Atomic payloads
// avoid the data race that a seqlock over plain integers would otherwise have.
class AudioPlaybackClock {
public:
    void publish(AudioPlaybackRenderer::Clock clock) {
        m_version.fetch_add(1, std::memory_order_seq_cst);
        m_source.store(clock.sourceUs, std::memory_order_seq_cst);
        m_local.store(clock.localUs, std::memory_order_seq_cst);
        m_version.fetch_add(1, std::memory_order_seq_cst);
    }
    AudioPlaybackRenderer::Clock read() const {
        const auto before = m_version.load(std::memory_order_seq_cst);
        const AudioPlaybackRenderer::Clock clock{m_source.load(std::memory_order_seq_cst), m_local.load(std::memory_order_seq_cst)};
        const auto after = m_version.load(std::memory_order_seq_cst);
        return before == after && !(before & 1) ? clock : AudioPlaybackRenderer::Clock{};
    }
private:
    std::atomic<quint64> m_version{0};
    std::atomic<qint64> m_source{-1}, m_local{-1};
};
