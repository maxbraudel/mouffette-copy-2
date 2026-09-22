#include "backend/audiosharing/AudioCapturePacketizer.h"
#include "backend/audiosharing/AudioCaptureTimestamp.h"
#include "backend/audiosharing/AudioStreamCodec.h"
#include <QtTest>
#include <array>
#include <cmath>
#include <limits>
#include <vector>

namespace {
QByteArray numberedPcm(int start, int frames) {
    QByteArray pcm(frames * 2 * sizeof(float), Qt::Uninitialized);
    auto* values = reinterpret_cast<float*>(pcm.data());
    for (int frame = 0; frame < frames; ++frame) {
        values[2 * frame] = float(start + frame);
        values[2 * frame + 1] = -float(start + frame);
    }
    return pcm;
}
bool finitePcm(const QByteArray& pcm) {
    if (pcm.size() != 960 * 2 * sizeof(float)) return false;
    const auto* samples = reinterpret_cast<const float*>(pcm.constData());
    return std::all_of(samples, samples + 1920, [](float value) { return std::isfinite(value); });
}
}
class AudioCapturePipelineTest final : public QObject {
    Q_OBJECT
private slots:
    void arbitraryNativeBlocksPreserveEverySampleAndTimestamp() {
        AudioCapturePacketizer packetizer;
        constexpr qint64 origin = 100000000;
        int generated = 0, received = 0;
        // 128, 256, 441, 480 and 512 frame callbacks all occur in native audio
        // APIs; none may reset the Opus boundary or duplicate the last sample.
        const std::array<int, 7> sizes{128, 256, 441, 480, 512, 960, 1024};
        for (int block = 0; block < 500; ++block) {
            const int count = sizes[block % sizes.size()];
            QVERIFY(packetizer.append(numberedPcm(generated, count), origin + qint64(generated) * 1000000 / 48000));
            generated += count;
            while (const auto packet = packetizer.take()) {
                const qint64 expectedUs = origin + qint64(received) * 1000000 / 48000;
                QVERIFY(std::abs(packet->timestampUs - expectedUs) <= 1);
                QCOMPARE(packet->discontinuity, received == 0);
                const auto* samples = reinterpret_cast<const float*>(packet->pcm.constData());
                for (int frame = 0; frame < 960; ++frame) {
                    QCOMPARE(samples[frame * 2], float(received + frame));
                    QCOMPARE(samples[frame * 2 + 1], -float(received + frame));
                }
                received += 960;
            }
        }
        QCOMPARE(packetizer.bufferedFrames(), generated - received);
        QVERIFY(packetizer.bufferedFrames() < 960);
    }
    void captureGapsAreNeverSplicedIntoOneOpusFrame() {
        AudioCapturePacketizer packetizer;
        QVERIFY(packetizer.append(numberedPcm(0, 480), 1000000));
        QVERIFY(!packetizer.take());
        // The previous implementation silently joined up to 30 ms of missing
        // audio to an unrelated half-packet, producing a false timeline.
        QVERIFY(packetizer.append(numberedPcm(1440, 960), 1030000));
        const auto packet = packetizer.take();
        QVERIFY(packet); QVERIFY(packet->discontinuity);
        QCOMPARE(packet->timestampUs, 1030000);
        QCOMPARE(packet->pcm, numberedPcm(1440, 960));
        QVERIFY(packetizer.append(numberedPcm(2400, 480), 1050000));
        QVERIFY(packetizer.append(numberedPcm(2880, 960), 1060000, true));
        const auto restarted = packetizer.take();
        QVERIFY(restarted && restarted->discontinuity);
        QCOMPARE(restarted->pcm, numberedPcm(2880, 960));
    }
    void nativeTimestampJitterNeverDropsOrDuplicatesPcm() {
        AudioCapturePacketizer packetizer;
        int consumed = 0;
        const std::array<qint64, 5> jitterUs{0, 700, -500, 300, -800};
        for (int block = 0; block < 200; ++block) {
            const qint64 timestamp = 1000000 + qint64(block) * 10000 + jitterUs[block % jitterUs.size()];
            QVERIFY(packetizer.append(numberedPcm(block * 480, 480), timestamp));
            while (const auto packet = packetizer.take()) {
                QCOMPARE(packet->discontinuity, consumed == 0);
                QCOMPARE(packet->pcm, numberedPcm(consumed, 960));
                QVERIFY(std::abs(packet->timestampUs - (1000000 + qint64(consumed) * 1000000 / 48000)) <= 800);
                consumed += 960;
            }
        }
        QCOMPARE(consumed, 96000);
    }
    void repeatedCallbacksDoNotReplayAudio() {
        AudioCapturePacketizer packetizer;
        const auto samples = numberedPcm(0, 960);
        QVERIFY(packetizer.append(samples, 1000000));
        QVERIFY(packetizer.take());
        QVERIFY(!packetizer.append(samples, 1000000));
        QVERIFY(!packetizer.append(samples, 999999));
        QVERIFY(!packetizer.take());
        QVERIFY(packetizer.append(numberedPcm(960, 960), 1020000));
        const auto next = packetizer.take();
        QVERIFY(next && !next->discontinuity);
    }
    void backlogKeepsOnlyTheLatestHundredMilliseconds() {
        AudioCapturePacketizer packetizer;
        for (int block = 0; block < 30; ++block) {
            QVERIFY(packetizer.append(numberedPcm(block * 480, 480), 1000000 + block * 10000));
            QVERIFY(packetizer.bufferedFrames() <= 4800);
        }
        const auto latest = packetizer.take();
        QVERIFY(latest && latest->discontinuity);
        QCOMPARE(latest->timestampUs, 1200000);
        QCOMPARE(latest->pcm, numberedPcm(9600, 960));
        packetizer.reset();
        QVERIFY(packetizer.append(numberedPcm(0, 48000), 2000000));
        QCOMPARE(packetizer.bufferedFrames(), 4800);
        const auto tail = packetizer.take();
        QVERIFY(tail && tail->discontinuity);
        QCOMPARE(tail->timestampUs, 2900000);
        QCOMPARE(tail->pcm, numberedPcm(43200, 960));
    }
    void overlappingNativeBuffersKeepOnlyNewSamples() {
        AudioCapturePacketizer packetizer;
        QVERIFY(packetizer.append(numberedPcm(0, 960), 1000000));
        QVERIFY(packetizer.take());
        QVERIFY(packetizer.append(numberedPcm(480, 960), 1010000));
        QCOMPARE(packetizer.bufferedFrames(), 480);
        QVERIFY(packetizer.append(numberedPcm(1440, 480), 1030000));
        const auto next = packetizer.take();
        QVERIFY(next && !next->discontinuity);
        QCOMPARE(next->timestampUs, 1020000);
        QCOMPARE(next->pcm, numberedPcm(960, 960));
        packetizer.reset();
        QVERIFY(packetizer.append(numberedPcm(0, 960), 1000000));
        QVERIFY(packetizer.take());
        // Microsecond PTS rounding must not round the overlap up to an extra
        // sample when its duration is not an integer number of microseconds.
        QVERIFY(packetizer.append(numberedPcm(441, 960), 1000000 + qint64(441) * 1000000 / 48000));
        QVERIFY(packetizer.append(numberedPcm(1401, 519), 1000000 + qint64(1401) * 1000000 / 48000));
        const auto rounded = packetizer.take();
        QVERIFY(rounded); QCOMPARE(rounded->pcm, numberedPcm(960, 960));
    }
    void missingNativeTimestampNamesFirstSampleAndPreservesCaptureAge() {
        AudioCaptureTimestamp clock;
        auto point = clock.map(-1, 480, 1010000);
        QCOMPARE(point.timestampUs, 1000000); QVERIFY(point.discontinuity);
        point = clock.map(-1, 480, 1050000);
        QCOMPARE(point.timestampUs, 1010000); QVERIFY(!point.discontinuity);
        // A delayed valid native timestamp retains its true age.
        point = clock.map(1020000, 480, 1090000);
        QCOMPARE(point.timestampUs, 1020000); QVERIFY(!point.discontinuity);
        point = clock.map(-1, 480, 1200000);
        QCOMPARE(point.timestampUs, 1190000); QVERIFY(point.discontinuity);
        point = clock.map(-1, 960, 1250000, true);
        QCOMPARE(point.timestampUs, 1230000); QVERIFY(point.discontinuity);
    }
    void codecDoesNotPropagateNonfiniteNativeSamples() {
        AudioStreamEncoder encoder; AudioStreamDecoder decoder; QString error;
        QVERIFY2(encoder.initialize(96000, error), qPrintable(error));
        std::array<float, 1920> native{};
        native[0] = std::numeric_limits<float>::quiet_NaN();
        native[17] = std::numeric_limits<float>::infinity();
        native[300] = -std::numeric_limits<float>::infinity();
        native[1800] = std::numeric_limits<float>::max();
        for (int packet = 0; packet < 20; ++packet) {
            const auto encoded = encoder.encode(native.data(), error);
            QVERIFY2(!encoded.isEmpty(), qPrintable(error));
            QVERIFY(finitePcm(decoder.decode(encoded, error)));
            native.fill(0.1f);
        }
    }
    void missingNativeTimestampsDoNotAccumulateFractionalFrameRounding() {
        AudioCaptureTimestamp clock;
        constexpr qint64 origin = 1000000;
        for (qint64 block = 0; block < 100000; ++block) {
            const qint64 arrival = origin + (block + 1) * 512 * 1000000 / 48000;
            const auto point = clock.map(-1, 512, arrival);
            QCOMPARE(point.timestampUs, origin + block * 512 * 1000000 / 48000);
            QCOMPARE(point.discontinuity, block == 0);
        }
    }
    void opusConcealmentAdvancesStateAndRecoversAfterPacketLoss() {
        AudioStreamEncoder encoder; AudioStreamDecoder decoder; QString error;
        QVERIFY(encoder.initialize(96000, error));
        std::array<float, 1920> native{};
        double concealedEnergy = 0, recoveredEnergy = 0;
        for (int packet = 0; packet < 20; ++packet) {
            for (int frame = 0; frame < 960; ++frame)
                native[frame * 2] = native[frame * 2 + 1] = float(0.4 * std::sin((packet * 960 + frame) * 0.07));
            const auto encoded = encoder.encode(native.data(), error);
            auto pcm = packet == 10 ? decoder.conceal(error) : decoder.decode(encoded, error);
            QVERIFY2(finitePcm(pcm), qPrintable(error));
            const auto* samples = reinterpret_cast<const float*>(pcm.constData());
            for (int index = 0; index < 1920; ++index) {
                if (packet == 10) concealedEnergy += samples[index] * samples[index];
                if (packet == 11) recoveredEnergy += samples[index] * samples[index];
            }
        }
        QVERIFY(concealedEnergy > 1.0); QVERIFY(recoveredEnergy > 10.0);
        const auto fecPacket = encoder.encode(native.data(), error);
        QVERIFY(finitePcm(decoder.decode(fecPacket, error, true)));
        QVERIFY(finitePcm(decoder.decode(fecPacket, error)));
        decoder.reset(); QVERIFY(finitePcm(decoder.conceal(error)));
    }
    void codecLookaheadMatchesActualSignalDelay() {
        AudioStreamEncoder encoder; AudioStreamDecoder decoder; QString error;
        QVERIFY(encoder.initialize(96000, error));
        const int lookahead = encoder.lookaheadSamples();
        QVERIFY(lookahead > 0 && lookahead < 960);
        QCOMPARE(encoder.lookaheadUs(), qint64(lookahead) * 1000000 / 48000);
        std::vector<float> input, output;
        std::array<float, 1920> native{};
        quint32 randomState = 0x12abc789;
        for (int packet = 0; packet < 12; ++packet) {
            for (int frame = 0; frame < 960; ++frame) {
                // Broadband correlation measures sample delay without the
                // frequency-dependent phase shifts of a narrowband sine test.
                randomState = randomState * 1664525u + 1013904223u;
                const float value = (float(randomState >> 8) / float(0x1000000) - 0.5f) * 0.5f;
                native[frame * 2] = native[frame * 2 + 1] = value;
                input.push_back(value);
            }
            const auto pcm = decoder.decode(encoder.encode(native.data(), error), error);
            QVERIFY(finitePcm(pcm));
            const auto* decoded = reinterpret_cast<const float*>(pcm.constData());
            for (int frame = 0; frame < 960; ++frame) output.push_back(decoded[frame * 2]);
        }
        double bestError = std::numeric_limits<double>::max(); int bestDelay = -1;
        for (int delay = lookahead - 5; delay <= lookahead + 5; ++delay) {
            double squared = 0;
            for (int frame = 1920; frame < int(input.size()) - 960; ++frame) {
                const double residual = output[frame + delay] - input[frame];
                squared += residual * residual;
            }
            if (squared < bestError) { bestError = squared; bestDelay = delay; }
        }
        QVERIFY2(std::abs(bestDelay - lookahead) <= 1,
            qPrintable(QStringLiteral("Opus lookahead %1 samples, measured %2").arg(lookahead).arg(bestDelay)));
    }

};
QTEST_GUILESS_MAIN(AudioCapturePipelineTest)
#include "tst_AudioCapturePipeline.moc"
