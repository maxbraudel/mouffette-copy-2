#include "backend/audiosharing/AudioCaptureResampler.h"
#include <QtTest>
#include <algorithm>
#include <array>
#include <cmath>
#include <limits>

namespace {
constexpr int FrameBytes = 2 * sizeof(float);
constexpr double Pi = 3.14159265358979323846;
QByteArray tone(qint64 start, int frames) {
    QByteArray result(frames * FrameBytes, Qt::Uninitialized);
    auto* samples = reinterpret_cast<float*>(result.data());
    for (int frame = 0; frame < frames; ++frame) {
        const double time = double(start + frame) / 48000;
        // Distinct frequencies, DC offsets, and polarities identify any mono
        // conversion; neither channel naturally crosses zero (hole detection).
        samples[frame * 2] = float(0.2 + 0.1 * std::sin(time * 2 * Pi * 997));
        samples[frame * 2 + 1] = float(-0.3 + 0.07 * std::sin(time * 2 * Pi * 1733));
    }
    return result;
}
}

class AudioCaptureStabilityTest final : public QObject {
    Q_OBJECT
private slots:
    void continuousWaveformIgnoresJitterAndTracksClockDrift_data() {
        QTest::addColumn<int>("driftPpm");
        QTest::newRow("no-drift") << 0;
        QTest::newRow("clock-200ppm-slower") << 200;
        QTest::newRow("clock-200ppm-faster") << -200;
    }
    void continuousWaveformIgnoresJitterAndTracksClockDrift() {
        QFETCH(int, driftPpm);
        AudioCaptureResampler resampler;
        constexpr qint64 origin = 123000000;
        constexpr std::array<int, 7> blockSizes{128, 256, 441, 480, 512, 960, 1024};
        constexpr std::array<qint64, 5> jitter{0, 500, -500, 250, -250};
        qint64 generated = 0, produced = 0;
        std::array<float, 2> previous{}, maximumJump{};
        // Over a minute, an uncompensated 200 ppm clock would drift by 12 ms.
        for (int block = 0; generated < 48000 * 60; ++block) {
            const int frames = blockSizes[block % blockSizes.size()];
            const qint64 timestamp = origin + qRound64(double(generated) * 1000000 / 48000
                * (1.0 + double(driftPpm) / 1000000)) + jitter[block % jitter.size()];
            auto result = resampler.append(tone(generated, frames), timestamp);
            QVERIFY2(resampler.errorString().isEmpty(), qPrintable(resampler.errorString()));
            QVERIFY(result);
            QCOMPARE(result->discontinuity, produced == 0);
            // Every returned sample occupies the next contiguous 48 kHz slot.
            QCOMPARE(result->timestampUs, origin + produced * 1000000 / 48000);
            const auto* samples = reinterpret_cast<const float*>(result->pcm.constData());
            const int count = int(result->pcm.size() / FrameBytes);
            for (int frame = 0; frame < count; ++frame) {
                const float left = samples[frame * 2], right = samples[frame * 2 + 1];
                QVERIFY(std::isfinite(left) && std::isfinite(right));
                QVERIFY(left > 0.08f && left < 0.32f);
                QVERIFY(right < -0.21f && right > -0.39f);
                for (int channel = 0; channel < 2; ++channel) {
                    const auto value = samples[frame * 2 + channel];
                    if (produced + frame > 64)
                        maximumJump[channel] = std::max(maximumJump[channel], std::abs(value - previous[channel]));
                    previous[channel] = value;
                }
            }
            produced += count; generated += frames;
        }
        const double expectedFrames = generated * (1.0 + double(driftPpm) / 1000000);
        // Includes the filter's small retained tail and the bounded servo error.
        QVERIFY2(std::abs(produced - expectedFrames) < 96,
            qPrintable(QStringLiteral("Clock did not converge: %1 frames").arg(produced - expectedFrames)));
        QVERIFY2(maximumJump[0] < 0.2 * std::sin(Pi * 997 / 48000) * 1.05,
            qPrintable(QStringLiteral("Left waveform jumped by %1").arg(maximumJump[0])));
        QVERIFY2(maximumJump[1] < 0.14 * std::sin(Pi * 1733 / 48000) * 1.05,
            qPrintable(QStringLiteral("Right waveform jumped by %1").arg(maximumJump[1])));
    }
    void duplicateAndPartiallyOverlappingBuffersNeverReplaySamples() {
        AudioCaptureResampler baseline, overlap;
        QByteArray expected, actual;
        auto append = [](AudioCaptureResampler& resampler, QByteArray& output, int start, int frames) {
            auto block = resampler.append(tone(start, frames), 1000000 + qint64(start) * 1000000 / 48000);
            if (block) output += block->pcm;
        };
        append(baseline, expected, 0, 960);
        append(overlap, actual, 0, 960);
        QVERIFY(!overlap.append(tone(0, 960), 1000000));
        QVERIFY(!overlap.append(tone(0, 480), 999999));
        append(baseline, expected, 960, 480);
        append(overlap, actual, 480, 960);
        append(baseline, expected, 1440, 480);
        append(overlap, actual, 1440, 480);
        QCOMPARE(actual, expected);
    }
    void arbitraryEpochRemainsOnOneAbsoluteSampleGrid() {
        AudioCaptureResampler resampler;
        constexpr qint64 origin = 1000011;
        constexpr qint64 originFrame = (origin * 48 + 500) / 1000;
        constexpr std::array<int, 7> sizes{128, 256, 441, 480, 512, 960, 1024};
        qint64 generated = 0, produced = 0;
        for (int block = 0; block < 300; ++block) {
            const int frames = sizes[block % sizes.size()];
            const auto result = resampler.append(tone(generated, frames), origin + generated * 1000000 / 48000);
            QVERIFY(result);
            QCOMPARE((result->timestampUs * 48 + 500) / 1000, originFrame + produced);
            generated += frames;
            produced += result->pcm.size() / FrameBytes;
        }
    }
    void realGapsAndClockRestartsBeginANewSegment() {
        AudioCaptureResampler resampler;
        auto first = resampler.append(tone(0, 960), 1000000);
        QVERIFY(first && first->discontinuity);
        auto gap = resampler.append(tone(1920, 960), 1040000);
        QVERIFY(gap && gap->discontinuity);
        QCOMPARE(gap->timestampUs, 1040000);
        auto next = resampler.append(tone(2880, 960), 1060000);
        QVERIFY(next && !next->discontinuity);
        auto restart = resampler.append(tone(0, 960), 100000);
        QVERIFY(restart && restart->discontinuity);
        QCOMPARE(restart->timestampUs, 100000);
        auto explicitRestart = resampler.append(tone(0, 960), 110000, true);
        QVERIFY(explicitRestart && explicitRestart->discontinuity);
        QCOMPARE(explicitRestart->timestampUs, 110000);
        resampler.reset();
        auto reset = resampler.append(tone(0, 960), 3000000);
        QVERIFY(reset && reset->discontinuity);
        QCOMPARE(reset->timestampUs, 3000000);
    }
    void tinyBlocksKeepFirstSampleTimestampDespiteFilterDelay() {
        AudioCaptureResampler resampler;
        qint64 produced = 0;
        for (int frame = 0; frame < 200; ++frame) {
            const auto block = resampler.append(tone(frame, 1), 1000000 + qint64(frame) * 1000000 / 48000);
            if (!block) continue; // Filter must receive enough input first.
            QCOMPARE(block->timestampUs, 1000000 + produced * 1000000 / 48000);
            QCOMPARE(block->discontinuity, produced == 0);
            produced += block->pcm.size() / FrameBytes;
        }
        QVERIFY(produced > 150 && produced < 200);
    }
    void oversizedAndInvalidInputCannotPoisonFutureCapture() {
        AudioCaptureResampler resampler;
        QVERIFY(!resampler.append({}, 1000000));
        QVERIFY(!resampler.append(QByteArray(7, '\0'), 1000000));
        QVERIFY(!resampler.append(tone(0, 960), -1));
        auto oversized = resampler.append(tone(0, 48000), 1000000);
        QVERIFY(oversized && oversized->discontinuity);
        QCOMPARE(oversized->timestampUs, 1900000);
        QVERIFY(oversized->pcm.size() <= AudioCaptureResampler::MaximumFrames * FrameBytes);
        auto invalid = tone(0, 960);
        auto* samples = reinterpret_cast<float*>(invalid.data());
        samples[0] = std::numeric_limits<float>::quiet_NaN();
        samples[1] = std::numeric_limits<float>::infinity();
        samples[2] = std::numeric_limits<float>::max();
        samples[3] = -std::numeric_limits<float>::max();
        for (int block = 0; block < 5; ++block) {
            const auto result = resampler.append(block == 0 ? invalid : tone(block * 960, 960),
                2000000 + block * 20000);
            QVERIFY(result);
            const auto* output = reinterpret_cast<const float*>(result->pcm.constData());
            for (int sample = 0; sample < result->pcm.size() / int(sizeof(float)); ++sample)
                QVERIFY(std::isfinite(output[sample]));
        }
        QVERIFY(resampler.errorString().isEmpty());
    }
};
QTEST_APPLESS_MAIN(AudioCaptureStabilityTest)
#include "tst_AudioCaptureStability.moc"
