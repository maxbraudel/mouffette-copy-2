#include "backend/audiosharing/AudioStreamCodec.h"
#include <QtTest>
#include <opus.h>
#include <array>
#include <cmath>

namespace {
constexpr int Frames = 960;
constexpr int Rate = 48000;
constexpr double Pi = 3.14159265358979323846;
enum Pattern { SeparateTones, LeftOnly, RightOnly, OppositePhase };

void fill(std::array<float, Frames * 2>& pcm, int packet, Pattern pattern) {
    for (int frame = 0; frame < Frames; ++frame) {
        const double time = double(packet * Frames + frame) / Rate;
        const float left = float(0.3 * std::sin(2 * 3.14159265358979323846 * 400 * time));
        const float right = float(0.2 * std::sin(2 * 3.14159265358979323846 * 1000 * time));
        pcm[frame * 2] = pattern == RightOnly ? 0.0f : left;
        pcm[frame * 2 + 1] = pattern == LeftOnly ? 0.0f : pattern == OppositePhase ? -left : right;
    }
}

struct Spectrum {
    double real = 0, imaginary = 0;
    void add(float value, int frame, int frequency) {
        const double phase = 2 * 3.14159265358979323846 * frequency * frame / Rate;
        real += value * std::cos(phase);
        imaginary += value * std::sin(phase);
    }
    double amplitude(int frames) const { return 2 * std::hypot(real, imaginary) / frames; }
};
}

class AudioStereoTest final : public QObject {
    Q_OBJECT
private slots:
    void codecPreservesChannelSeparation_data() {
        QTest::addColumn<int>("bitrate");
        QTest::addColumn<int>("pattern");
        for (const int bitrate : {32000, 96000}) {
            const std::array<const char*, 4> names{"separate-tones", "left-only", "right-only", "opposite-phase"};
            for (int pattern = SeparateTones; pattern <= OppositePhase; ++pattern)
                QTest::newRow(qPrintable(QStringLiteral("%1-%2").arg(bitrate).arg(names[pattern]))) << bitrate << pattern;
        }
    }
    void codecPreservesChannelSeparation() {
        QFETCH(int, bitrate);
        QFETCH(int, pattern);
        AudioStreamEncoder encoder;
        AudioStreamDecoder decoder;
        QString error;
        QVERIFY2(encoder.initialize(bitrate, error), qPrintable(error));
        std::array<float, Frames * 2> input{};
        Spectrum left400, right400, left1000, right1000;
        double leftEnergy = 0, rightEnergy = 0, sumEnergy = 0, differenceEnergy = 0;
        int measuredFrames = 0;
        for (int packet = 0; packet < 75; ++packet) {
            fill(input, packet, Pattern(pattern));
            const auto encoded = encoder.encode(input.data(), error);
            QVERIFY2(!encoded.isEmpty(), qPrintable(error));
            QCOMPARE(opus_packet_get_nb_channels(reinterpret_cast<const unsigned char*>(encoded.constData())), 2);
            const auto decoded = decoder.decode(encoded, error);
            QCOMPARE(decoded.size(), qsizetype(Frames * 2 * sizeof(float)));
            // Measure settled audio rather than the codec's initial lookahead.
            if (packet < 10) continue;
            const auto* samples = reinterpret_cast<const float*>(decoded.constData());
            for (int frame = 0; frame < Frames; ++frame) {
                const float left = samples[frame * 2], right = samples[frame * 2 + 1];
                QVERIFY(std::isfinite(left) && std::isfinite(right));
                const int index = packet * Frames + frame;
                left400.add(left, index, 400); right400.add(right, index, 400);
                left1000.add(left, index, 1000); right1000.add(right, index, 1000);
                leftEnergy += double(left) * left; rightEnergy += double(right) * right;
                sumEnergy += double(left + right) * (left + right);
                differenceEnergy += double(left - right) * (left - right);
            }
            measuredFrames += Frames;
        }
        if (pattern != RightOnly) QVERIFY(left400.amplitude(measuredFrames) > 0.2);
        if (pattern == SeparateTones || pattern == RightOnly) QVERIFY(right1000.amplitude(measuredFrames) > 0.12);
        if (pattern == SeparateTones) {
            // Detect a mono mix, a channel swap, and excessive crosstalk, not
            // merely the advertised number of channels in the packet header.
            QVERIFY(right400.amplitude(measuredFrames) < left400.amplitude(measuredFrames) * 0.05);
            QVERIFY(left1000.amplitude(measuredFrames) < right1000.amplitude(measuredFrames) * 0.05);
        } else if (pattern == LeftOnly) {
            QVERIFY(rightEnergy < leftEnergy * 0.001);
        } else if (pattern == RightOnly) {
            QVERIFY(leftEnergy < rightEnergy * 0.001);
        } else {
            // Anti-phase stereo vanishes when downmixed. Its side information
            // must survive both the full and reduced bitrate profiles.
            QVERIFY(differenceEnergy > measuredFrames * 0.1);
            QVERIFY(sumEnergy < differenceEnergy * 0.001);
        }
    }
    void silenceBitrateChangesAndResetsKeepStereo() {
        AudioStreamEncoder encoder;
        AudioStreamDecoder decoder;
        QString error;
        QVERIFY2(encoder.initialize(96000, error), qPrintable(error));
        std::array<float, Frames * 2> input{};
        for (int packet = 0; packet < 90; ++packet) {
            if (packet % 10 == 0) encoder.setBitrate(packet % 20 ? 32000 : 96000);
            if (packet == 45) { encoder.reset(); decoder.reset(); }
            if (packet >= 30) fill(input, packet, SeparateTones);
            const auto encoded = encoder.encode(input.data(), error);
            QVERIFY2(!encoded.isEmpty(), qPrintable(error));
            QCOMPARE(opus_packet_get_nb_channels(reinterpret_cast<const unsigned char*>(encoded.constData())), 2);
            const auto pcm = decoder.decode(encoded, error);
            QCOMPARE(pcm.size(), qsizetype(Frames * 2 * sizeof(float)));
            const auto* samples = reinterpret_cast<const float*>(pcm.constData());
            for (int sample = 0; sample < Frames * 2; ++sample) QVERIFY(std::isfinite(samples[sample]));
        }
    }
    void quietStereoSurvivesContentClassificationHistory_data() {
        QTest::addColumn<int>("bitrate");
        QTest::addColumn<bool>("noiseHistory");
        QTest::newRow("32k-silence-then-quiet-stereo-and-noise") << 32000 << false;
        QTest::newRow("32k-mono-noise-then-quiet-stereo") << 32000 << true;
        QTest::newRow("96k-silence-then-quiet-stereo-and-noise") << 96000 << false;
        QTest::newRow("96k-mono-noise-then-quiet-stereo") << 96000 << true;
    }
    void quietStereoSurvivesContentClassificationHistory() {
        QFETCH(int, bitrate);
        QFETCH(bool, noiseHistory);
        AudioStreamEncoder encoder;
        AudioStreamDecoder decoder;
        QString error;
        QVERIFY2(encoder.initialize(bitrate, error), qPrintable(error));
        std::array<float, Frames * 2> input{};
        std::array<std::array<double, 2>, 2> amplitude{};
        quint32 random = 0x5aa731;
        double lowpass = 0;
        int measuredPackets = 0;
        for (int packet = 0; packet < 245; ++packet) {
            for (int frame = 0; frame < Frames; ++frame) {
                const double time = double(packet * Frames + frame) / Rate;
                random = random * 1664525u + 1013904223u;
                const double noise = (double(random >> 8) / 16777216.0 - 0.5) * 2;
                lowpass = 0.985 * lowpass + 0.015 * noise;
                if (packet < 200) input[frame * 2] = input[frame * 2 + 1] = noiseHistory ? float(0.03 * noise) : 0.0f;
                else {
                    const double background = noiseHistory ? 0.0 : 0.0001 * lowpass * 8;
                    input[frame * 2] = float(0.03 * std::sin(2 * Pi * 650 * time) + background);
                    input[frame * 2 + 1] = float(0.03 * std::sin(2 * Pi * 1350 * time) + background);
                }
            }
            const auto encoded = encoder.encode(input.data(), error);
            QVERIFY2(!encoded.isEmpty(), qPrintable(error));
            QCOMPARE(opus_packet_get_nb_channels(reinterpret_cast<const unsigned char*>(encoded.constData())), 2);
            const auto decoded = decoder.decode(encoded, error);
            QCOMPARE(decoded.size(), qsizetype(Frames * 2 * sizeof(float)));
            if (packet < 215) continue;
            const auto* samples = reinterpret_cast<const float*>(decoded.constData());
            for (int channel = 0; channel < 2; ++channel) {
                Spectrum leftTone, rightTone;
                for (int frame = 0; frame < Frames; ++frame) {
                    QVERIFY(std::isfinite(samples[frame * 2 + channel]));
                    leftTone.add(samples[frame * 2 + channel], frame, 650);
                    rightTone.add(samples[frame * 2 + channel], frame, 1350);
                }
                amplitude[channel][0] += leftTone.amplitude(Frames);
                amplitude[channel][1] += rightTone.amplitude(Frames);
            }
            ++measuredPackets;
        }
        for (auto& channel : amplitude) for (auto& value : channel) value /= measuredPackets;
        const auto detail = QStringLiteral("quiet stereo L650=%1 R1350=%2 cross L1350=%3 R650=%4")
            .arg(amplitude[0][0]).arg(amplitude[1][1]).arg(amplitude[0][1]).arg(amplitude[1][0]);
        // The old automatic speech/music classification plus FEC could select
        // hybrid/SILK with a collapsed side channel: both outputs contained the
        // half-volume mono sum despite a stereo packet header. This fixture
        // reproduces that state after innocuous silence or mono noise history.
        QVERIFY2(amplitude[0][0] > 0.02 && amplitude[0][0] < 0.04, qPrintable(detail));
        QVERIFY2(amplitude[1][1] > 0.02 && amplitude[1][1] < 0.04, qPrintable(detail));
        QVERIFY2(amplitude[0][1] < amplitude[1][1] * 0.05, qPrintable(detail));
        QVERIFY2(amplitude[1][0] < amplitude[0][0] * 0.05, qPrintable(detail));
    }
};

QTEST_GUILESS_MAIN(AudioStereoTest)
#include "tst_AudioStereo.moc"
