#include "backend/audiosharing/AudioOutputRouting.h"
#include <QtTest>
#include <array>
#include <vector>

namespace {
QAudioFormat format(int rate, QAudioFormat::ChannelConfig layout) {
    QAudioFormat value;
    value.setSampleRate(rate);
    value.setChannelConfig(layout);
    value.setSampleFormat(QAudioFormat::Float);
    return value;
}
}

class AudioOutputRoutingTest final : public QObject {
    Q_OBJECT
private slots:
    void stereo48kTakesPriorityOverPreferredMono() {
        const auto preferred = format(44100, QAudioFormat::ChannelConfigMono);
        std::vector<QAudioFormat> attempted;
        const auto selected = chooseStereoOutputFormat(preferred, [&](const QAudioFormat& candidate) {
            attempted.push_back(candidate);
            return candidate.channelCount() == 1 || candidate.sampleRate() == 48000;
        });
        QCOMPARE(selected.channelConfig(), QAudioFormat::ChannelConfigStereo);
        QCOMPARE(selected.sampleRate(), 48000);
        QCOMPARE(selected.sampleFormat(), QAudioFormat::Float);
        QCOMPARE(attempted.size(), size_t(1));
    }
    void stereoAtNativeRatePrecedesMonoFallback() {
        const auto preferred = format(44100, QAudioFormat::ChannelConfigMono);
        const auto selected = chooseStereoOutputFormat(preferred, [](const QAudioFormat& candidate) {
            return candidate.sampleRate() == 44100;
        });
        QCOMPARE(selected.channelConfig(), QAudioFormat::ChannelConfigStereo);
        QCOMPARE(selected.sampleRate(), 44100);
    }
    void alternateStereoRatePrecedesNativeSurroundFallback() {
        const auto preferred = format(48000, QAudioFormat::ChannelConfigSurround5Dot1);
        const auto selected = chooseStereoOutputFormat(preferred, [](const QAudioFormat& candidate) {
            return candidate.channelCount() == 6 || candidate.sampleRate() == 96000;
        });
        QCOMPARE(selected.channelConfig(), QAudioFormat::ChannelConfigStereo);
        QCOMPARE(selected.sampleRate(), 96000);
    }
    void genuinelyMonoHardwareIsTheLastFallback() {
        auto preferred = format(48000, QAudioFormat::ChannelConfigMono);
        preferred.setSampleFormat(QAudioFormat::Int16);
        std::vector<int> stereoRates;
        const auto selected = chooseStereoOutputFormat(preferred, [&](const QAudioFormat& candidate) {
            if (candidate.channelCount() == 2) stereoRates.push_back(candidate.sampleRate());
            return candidate.channelCount() == 1 && candidate.sampleFormat() == QAudioFormat::Float;
        });
        QCOMPARE(selected.channelConfig(), QAudioFormat::ChannelConfigMono);
        QCOMPARE(selected.sampleFormat(), QAudioFormat::Float);
        QCOMPARE(stereoRates.size(), size_t(7));
        std::sort(stereoRates.begin(), stereoRates.end());
        QVERIFY(std::adjacent_find(stereoRates.begin(), stereoRates.end()) == stereoRates.end());
    }
    void unsupportedFloatOutputFailsExplicitly() {
        const auto selected = chooseStereoOutputFormat(format(48000, QAudioFormat::ChannelConfigStereo),
            [](const QAudioFormat&) { return false; });
        QVERIFY(!selected.isValid());
        QVERIFY(!chooseStereoOutputFormat(QAudioDevice{}).isValid());
    }
    void stereoMapsOnlyToFrontSpeakers_data() {
        QTest::addColumn<int>("configuration");
        QTest::newRow("stereo") << int(QAudioFormat::ChannelConfigStereo);
        QTest::newRow("5.1") << int(QAudioFormat::ChannelConfigSurround5Dot1);
        QTest::newRow("7.1") << int(QAudioFormat::ChannelConfigSurround7Dot1);
        QTest::newRow("custom-height-layout") << int(QAudioFormat::channelConfig(
            QAudioFormat::TopFrontRight, QAudioFormat::FrontRight, QAudioFormat::LFE,
            QAudioFormat::FrontLeft, QAudioFormat::TopFrontLeft));
    }
    void stereoMapsOnlyToFrontSpeakers() {
        QFETCH(int, configuration);
        const auto output = format(48000, QAudioFormat::ChannelConfig(configuration));
        const AudioStereoOutputMapping routing(output);
        QVERIFY(routing.isValid());
        QCOMPARE(routing.channels(), output.channelCount());
        std::array<float, QAudioFormat::NChannelPositions> samples{};
        routing.add(samples.data(), 0.75f, -0.25f);
        routing.add(samples.data(), 0.125f, 0.125f);
        for (int channel = 0; channel < int(samples.size()); ++channel) {
            const float expected = channel == output.channelOffset(QAudioFormat::FrontLeft) ? 0.875f
                : channel == output.channelOffset(QAudioFormat::FrontRight) ? -0.125f : 0.0f;
            QCOMPARE(samples[channel], expected);
        }
    }
    void monoDownmixOccursOnlyAtTheHardwareWrite() {
        const AudioStereoOutputMapping routing(format(48000, QAudioFormat::ChannelConfigMono));
        QVERIFY(routing.isValid());
        std::array<float, 2> samples{};
        routing.add(samples.data(), 0.75f, -0.25f);
        QCOMPARE(samples[0], 0.25f);
        QCOMPARE(samples[1], 0.0f);
        // A phase-inverted stereo input correctly cancels only on this mono
        // hardware output, never in the canonical stereo publication buffer.
        routing.add(samples.data(), 0.5f, -0.5f);
        QCOMPARE(samples[0], 0.25f);
    }
    void unspecifiedLayoutUsesQtChannelCountDefault() {
        auto output = format(48000, QAudioFormat::ChannelConfigSurround5Dot1);
        output.setChannelCount(6);
        QCOMPARE(output.channelConfig(), QAudioFormat::ChannelConfigUnknown);
        const AudioStereoOutputMapping routing(output);
        QVERIFY(routing.isValid());
        std::array<float, 6> samples{};
        routing.add(samples.data(), 0.25f, -0.5f);
        QCOMPARE(samples[0], 0.25f); QCOMPARE(samples[1], -0.5f);
        for (int channel = 2; channel < int(samples.size()); ++channel) QCOMPARE(samples[channel], 0.0f);
    }
    void knownLayoutWithoutFrontPairDoesNotAliasAnotherSpeaker() {
        const auto output = format(48000, QAudioFormat::channelConfig(QAudioFormat::FrontCenter, QAudioFormat::LFE));
        const AudioStereoOutputMapping routing(output);
        QVERIFY(!routing.isValid());
        std::array<float, 2> samples{};
        routing.add(samples.data(), 0.25f, -0.5f);
        QCOMPARE(samples[0], 0.0f); QCOMPARE(samples[1], 0.0f);
        QVERIFY(!AudioStereoOutputMapping{}.isValid());
    }
};

QTEST_GUILESS_MAIN(AudioOutputRoutingTest)
#include "tst_AudioOutputRouting.moc"
