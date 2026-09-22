#include "backend/audiosharing/AudioCaptureResampler.h"
#include "backend/audiosharing/AudioCaptureMix.h"
#include "backend/audiosharing/AudioStreamCodec.h"
#include "backend/audiosharing/AudioPlaybackBuffer.h"
#include <QtTest>
#include <array>
#include <cmath>
#include <deque>
#include <vector>

// Exercise the actual capture clock, mixing, Opus and output resampling together.
// Independent native/codec/output tests cannot detect clicks introduced at their
// boundaries. No sound device or wall-clock scheduling is needed for this test.
class AudioSharingContinuityTest : public QObject {
    Q_OBJECT
private slots:
    void stereoSurvivesJitterAndRecovery_data() {
        QTest::addColumn<int>("bitrate");
        QTest::addColumn<int>("rate");
        QTest::addColumn<bool>("loss");
        for (int bitrate : {32000, 96000})
            for (int rate : {44100, 48000, 96000})
                for (bool loss : {false, true})
                    QTest::newRow(qPrintable(QString("%1-%2-%3").arg(bitrate).arg(rate).arg(loss))) << bitrate << rate << loss;
    }
    void stereoSurvivesJitterAndRecovery() {
        QFETCH(int, bitrate); QFETCH(int, rate); QFETCH(bool, loss);
        constexpr qint64 epoch = 1000011, deadlineDelay = 150000;
        constexpr double tau = 6.283185307179586;
        constexpr std::array<int, 2> frequencies{400, 1000};
        AudioCaptureResampler capture;
        AudioCaptureMix mix; mix.reset(epoch);
        AudioStreamEncoder encoder; AudioStreamDecoder decoder; QString error;
        QVERIFY2(encoder.initialize(bitrate, error), qPrintable(error));
        AudioPlaybackBuffer queue; AudioPlaybackRenderer renderer(rate);
        struct Packet { QByteArray bytes; qint64 timestamp, arrival; bool discontinuity; };
        std::deque<Packet> network;
        std::vector<float> output(size_t(rate / 100) * 2);
        std::array<float, 2> previous{}, maximumJump{};
        std::array<std::array<double, 2>, 2> amplitude{};
        qint64 lastArrival = 0;
        int sequence = 0;
        bool afterLoss = false;
        for (int tick = 0; tick < 600; ++tick) {
            const qint64 now = epoch + tick * 10000;
            // Complete 10 ms native blocks become available at their end. The
            // timestamp wobble must never become zero holes in the mixed PCM.
            if (tick && !(loss && tick >= 200 && tick < 210)) {
                QByteArray pcm(480 * 2 * sizeof(float), Qt::Uninitialized);
                auto* input = reinterpret_cast<float*>(pcm.data());
                for (int frame = 0; frame < 480; ++frame)
                    for (int channel = 0; channel < 2; ++channel)
                        input[frame * 2 + channel] = float(0.2 * std::sin(tau * frequencies[channel]
                            * ((tick - 1) * 480 + frame) / 48000));
                auto block = capture.append(pcm, now - 10000 + (tick % 3 - 1) * 500);
                QVERIFY2(capture.errorString().isEmpty(), qPrintable(capture.errorString()));
                if (block) mix.appendSystem(reinterpret_cast<const float*>(block->pcm.constData()),
                    block->pcm.size() / (2 * sizeof(float)), block->timestampUs, block->discontinuity);
            }
            while (auto packet = mix.take(now)) {
                const auto compressed = encoder.encode(reinterpret_cast<const float*>(packet->pcm.constData()), error);
                QVERIFY2(!compressed.isEmpty(), qPrintable(error));
                ++sequence;
                // A long network hole exceeds PLC's bounded recovery window.
                if (loss && sequence >= 150 && sequence < 154) { afterLoss = true; continue; }
                const auto arrival = std::max(lastArrival, now + (sequence * 7 % 13) * 1000);
                network.push_back({compressed, packet->timestampUs - encoder.lookaheadUs(), arrival, afterLoss});
                lastArrival = arrival; afterLoss = false;
            }
            while (!network.empty() && network.front().arrival <= now) {
                const auto packet = network.front(); network.pop_front();
                if (packet.discontinuity) decoder.reset();
                const auto pcm = decoder.decode(packet.bytes, error);
                QVERIFY2(!pcm.isEmpty(), qPrintable(error));
                QVERIFY(queue.push(reinterpret_cast<const float*>(pcm.constData()), packet.timestamp,
                    packet.timestamp + deadlineDelay, packet.discontinuity));
            }
            std::fill(output.begin(), output.end(), 0);
            renderer.render(queue, output.data(), rate / 100, 2, rate, now);
            for (int frame = 0; frame < rate / 100; ++frame)
                for (int channel = 0; channel < 2; ++channel) {
                    const auto value = output[frame * 2 + channel];
                    QVERIFY(std::isfinite(value));
                    maximumJump[channel] = std::max(maximumJump[channel], std::abs(value - previous[channel]));
                    previous[channel] = value;
                }
            if (tick < 30) continue;
            for (int channel = 0; channel < 2; ++channel)
                for (int frequency = 0; frequency < 2; ++frequency) {
                    double real = 0, imaginary = 0;
                    for (int frame = 0; frame < rate / 100; ++frame) {
                        const auto angle = tau * frequencies[frequency] * frame / rate;
                        real += output[frame * 2 + channel] * std::cos(angle);
                        imaginary += output[frame * 2 + channel] * std::sin(angle);
                    }
                    amplitude[channel][frequency] += 2 * std::hypot(real, imaginary) / (rate / 100);
                }
        }
        for (int channel = 0; channel < 2; ++channel) {
            QVERIFY2(maximumJump[channel] < 0.045f,
                qPrintable(QString("channel %1 discontinuous waveform: %2").arg(channel).arg(maximumJump[channel])));
            QVERIFY(amplitude[channel][channel] > 60);
            QVERIFY2(amplitude[channel][channel] > amplitude[channel][1 - channel] * 8,
                "Left/right content was collapsed or leaked into the other channel");
        }
        if (!loss) QCOMPARE(renderer.takeUnderruns(), 0);
    }
};
QTEST_GUILESS_MAIN(AudioSharingContinuityTest)
#include "tst_AudioSharingContinuity.moc"
