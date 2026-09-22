#pragma once
#include <QByteArray>
#include <QtGlobal>
#include <algorithm>
#include <cstring>
#include <deque>
#include <optional>

// Capture callbacks are not Opus packet boundaries. Preserve their sample
// timestamps through arbitrary native block sizes and never join across a
// capture dropout, duplicate buffer, restart, or bounded-mailbox overrun.
class AudioCapturePacketizer {
public:
    static constexpr int FrameBytes = 2 * sizeof(float);
    static constexpr int PacketFrames = 960;
    static constexpr int MaximumFrames = 4800;
    struct Packet { QByteArray pcm; qint64 timestampUs; bool discontinuity; };
    void reset() {
        chunks.clear(); queuedFrames = 0; lastStartUs = -1; lastEndUs = -1;
        discontinuous = true;
    }
    bool append(QByteArray pcm, qint64 timestampUs, bool discontinuity = false) {
        if (pcm.isEmpty() || pcm.size() % FrameBytes || timestampUs < 0) return false;
        int frames = int(pcm.size() / FrameBytes);
        // Native rounding and clock-rate error are small; a missing capture
        // quantum is not. Compare successive native blocks, not an indefinitely
        // extrapolated 48 kHz origin, so clock drift cannot cause periodic cuts.
        constexpr qint64 timestampToleranceUs = 2000;
        if (!discontinuity && lastStartUs >= 0 && timestampUs <= lastStartUs) return false;
        if (!discontinuity && lastEndUs >= 0 && lastEndUs - timestampUs > timestampToleranceUs) {
            // Some capture backends redeliver a partially overlapping block
            // after recovery. Keep only its new suffix instead of replaying
            // already transmitted samples or erasing a valid partial packet.
            const qint64 overlapFrames = ((lastEndUs - timestampUs) * 48000 + 500000) / 1000000;
            if (overlapFrames >= frames) return false;
            pcm.remove(0, qsizetype(overlapFrames) * FrameBytes);
            timestampUs += overlapFrames * 1000000 / 48000;
            frames -= int(overlapFrames);
        }
        if (discontinuity || (lastEndUs >= 0
            && (timestampUs - lastEndUs > timestampToleranceUs || lastEndUs - timestampUs > timestampToleranceUs))) {
            chunks.clear(); queuedFrames = 0; discontinuous = true;
        }
        lastStartUs = timestampUs;
        lastEndUs = timestampUs + qint64(frames) * 1000000 / 48000;
        if (frames > MaximumFrames) {
            const int skipped = frames - MaximumFrames;
            pcm.remove(0, qsizetype(skipped) * FrameBytes);
            timestampUs += qint64(skipped) * 1000000 / 48000;
            frames = MaximumFrames;
            chunks.clear(); queuedFrames = 0; discontinuous = true;
        }
        chunks.push_back({std::move(pcm), timestampUs, 0});
        queuedFrames += frames;
        while (queuedFrames > MaximumFrames) {
            auto& first = chunks.front();
            const int skip = std::min(queuedFrames - MaximumFrames, first.remainingFrames());
            first.offsetFrames += skip; queuedFrames -= skip; discontinuous = true;
            if (!first.remainingFrames()) chunks.pop_front();
        }
        return true;
    }
    std::optional<Packet> take() {
        if (queuedFrames < PacketFrames) return {};
        const auto& first = chunks.front();
        Packet packet{QByteArray(PacketFrames * FrameBytes, Qt::Uninitialized),
            first.timestampUs + qint64(first.offsetFrames) * 1000000 / 48000, discontinuous};
        discontinuous = false;
        int copied = 0;
        while (copied < PacketFrames) {
            auto& chunk = chunks.front();
            const int frames = std::min(PacketFrames - copied, chunk.remainingFrames());
            std::memcpy(packet.pcm.data() + copied * FrameBytes,
                chunk.pcm.constData() + chunk.offsetFrames * FrameBytes, size_t(frames) * FrameBytes);
            chunk.offsetFrames += frames; copied += frames; queuedFrames -= frames;
            if (!chunk.remainingFrames()) chunks.pop_front();
        }
        return packet;
    }
    int bufferedFrames() const { return queuedFrames; }
private:
    struct Chunk {
        QByteArray pcm;
        qint64 timestampUs;
        int offsetFrames;
        int remainingFrames() const { return int(pcm.size() / FrameBytes) - offsetFrames; }
    };
    std::deque<Chunk> chunks;
    int queuedFrames = 0;
    qint64 lastStartUs = -1, lastEndUs = -1;
    bool discontinuous = true;
};
