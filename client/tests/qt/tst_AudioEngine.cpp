#include "backend/audiosharing/AudioEngine.h"
#include "backend/audiosharing/AudioCaptureBuffer.h"
#include "backend/audiosharing/AudioPlaybackTimeline.h"
#include "backend/audiosharing/AudioPlaybackBuffer.h"
#include "backend/audiosharing/AudioStreamCodec.h"
#include "backend/media/PlaybackAudio.h"
#include <QAudioOutput>
#include <QAudioDevice>
#include <QMediaDevices>
#include <QScopeGuard>
#include <QSignalSpy>
#include <QTimer>
#include <QtTest>
#include <opus.h>
extern "C" {
#include <libavcodec/avcodec.h>
}
#include <array>
#include <cmath>
#include <thread>
#ifdef Q_OS_MACOS
#include <CoreGraphics/CoreGraphics.h>
#endif

namespace {
struct FakeCaptureState {
    SystemAudioCapture::Pcm pcm;
    SystemAudioCapture::State state;
    int stops = 0;
};
class FakeCapture : public SystemAudioCapture {
public:
    std::shared_ptr<FakeCaptureState> callbacks;
    explicit FakeCapture(std::shared_ptr<FakeCaptureState> state) : callbacks(std::move(state)) {}
    void start(Pcm pcm, State state) override { callbacks->pcm = std::move(pcm); callbacks->state = std::move(state); }
    void stop() override { ++callbacks->stops; }
};
QByteArray constantPcm(int frames, float value) {
    QByteArray result(frames * 2 * sizeof(float), Qt::Uninitialized);
    std::fill_n(reinterpret_cast<float*>(result.data()), frames * 2, value);
    return result;
}
double energy(const QByteArray& pcm) {
    const auto* samples = reinterpret_cast<const float*>(pcm.constData());
    double sum = 0;
    for (int i = 0; i < pcm.size() / int(sizeof(float)); ++i) sum += samples[i] * samples[i];
    return pcm.isEmpty() ? 0 : std::sqrt(sum / (pcm.size() / sizeof(float)));
}
// Real decoder/output path, independent frequencies identify accidental routing.
std::shared_ptr<const ResidentMediaAsset> toneAsset(int frequency, int rightFrequency = 0) {
    auto asset = std::make_shared<ResidentMediaAsset>();
    asset->durationUs = 8000000;
    auto& store = asset->audioPackets;
    auto* codec = avcodec_parameters_alloc();
    if (!codec) return {};
    store.codec = std::shared_ptr<AVCodecParameters>(codec, [](auto* p) { avcodec_parameters_free(&p); });
    codec->codec_type = AVMEDIA_TYPE_AUDIO; codec->codec_id = AV_CODEC_ID_PCM_F32LE;
    codec->format = AV_SAMPLE_FMT_FLT; codec->sample_rate = 48000;
    av_channel_layout_default(&codec->ch_layout, 2);
    codec->bits_per_coded_sample = 32; codec->block_align = 8;
    store.timeBaseNum = 1; store.timeBaseDen = 48000;
    store.bytes.resize(48000 * 8 * 2 * sizeof(float));
    auto* pcm = reinterpret_cast<float*>(store.bytes.data());
    for (int frame = 0; frame < 48000 * 8; ++frame) {
        pcm[frame * 2] = 0.03f * std::sin(6.283185307179586 * frequency * frame / 48000);
        pcm[frame * 2 + 1] = 0.03f * std::sin(6.283185307179586
            * (rightFrequency ? rightFrequency : frequency) * frame / 48000);
    }
    for (int frame = 0; frame < 48000 * 8; frame += 960)
        store.packets.append({qint64(frame) * 8, 960 * 8, frame, frame, 960, 0, {}});
    asset->firstAudioPcm = store.bytes.left(9600 * 8);
    return asset;
}
}
class AudioEngineTest final : public QObject {
    Q_OBJECT
private slots:
    void codecProfiles_data() {
        QTest::addColumn<int>("bitrate");
        QTest::newRow("music-96k") << 96000;
        QTest::newRow("economy-32k") << 32000;
    }
    void codecProfiles() {
        QFETCH(int, bitrate);
        AudioStreamEncoder encoder; AudioStreamDecoder decoder; QString error;
        QVERIFY2(encoder.initialize(bitrate, error), qPrintable(error));
        std::array<float, 1920> samples{};
        qint64 encodedBytes = 0;
        double leftEnergy = 0, rightEnergy = 0;
        for (int packet = 0; packet < 50; ++packet) {
            for (int frame = 0; frame < 960; ++frame) {
                const double time = double(packet * 960 + frame) / 48000;
                samples[frame * 2] = float(0.3 * std::sin(time * 2 * 3.141592653589793 * 440));
                samples[frame * 2 + 1] = float(0.15 * std::sin(time * 2 * 3.141592653589793 * 880));
            }
            const auto encoded = encoder.encode(samples.data(), error);
            QVERIFY2(!encoded.isEmpty(), qPrintable(error));
            QVERIFY(encoded.size() <= 1275); encodedBytes += encoded.size();
            const auto decoded = decoder.decode(encoded, error);
            QCOMPARE(decoded.size(), qsizetype(1920 * sizeof(float)));
            const auto* pcm = reinterpret_cast<const float*>(decoded.constData());
            for (int frame = 0; frame < 960; ++frame) {
                QVERIFY(std::isfinite(pcm[frame * 2])); QVERIFY(std::isfinite(pcm[frame * 2 + 1]));
                leftEnergy += pcm[frame * 2] * pcm[frame * 2];
                rightEnergy += pcm[frame * 2 + 1] * pcm[frame * 2 + 1];
            }
        }
        // Both channels survive with their intended relative level, and the
        // complete second remains bounded by constrained VBR at each profile.
        QVERIFY(leftEnergy > 1000); QVERIFY(rightEnergy > 200);
        QVERIFY(leftEnergy > rightEnergy * 2);
        QVERIFY(encodedBytes * 8 < bitrate * 1.4);
        encoder.reset(); decoder.reset(); encoder.setBitrate(bitrate == 96000 ? 32000 : 96000);
        QVERIFY(!decoder.decode(encoder.encode(samples.data(), error), error).isEmpty());
        QVERIFY(error.isEmpty());
    }
    void rejectsWrongDurationAndOversizedPackets() {
        AudioStreamDecoder decoder; QString error;
        QVERIFY(decoder.decode({}, error).isEmpty()); QVERIFY(!error.isEmpty());
        QVERIFY(decoder.decode(QByteArray(1276, '\0'), error).isEmpty());
        int status = 0;
        auto* native = opus_encoder_create(48000, 2, OPUS_APPLICATION_AUDIO, &status);
        QVERIFY(native);
        std::array<float, 960> samples{};
        QByteArray packet(1275, Qt::Uninitialized);
        const int bytes = opus_encode_float(native, samples.data(), 480,
            reinterpret_cast<unsigned char*>(packet.data()), packet.size());
        opus_encoder_destroy(native);
        QVERIFY(bytes > 0); packet.resize(bytes);
        QVERIFY(decoder.decode(packet, error).isEmpty()); // 10 ms is not the protocol's 20 ms.
        AudioStreamEncoder encoder; QVERIFY(encoder.initialize(96000, error));
        std::array<float, 1920> silence{};
        QVERIFY(!decoder.decode(encoder.encode(silence.data(), error), error).isEmpty());
        QVERIFY(error.isEmpty());
    }
    void playbackPreservesPhaseAcrossCaptureAndNetworkJitter_data() {
        QTest::addColumn<int>("rate");
        QTest::newRow("48k") << 48000;
        QTest::newRow("44k1") << 44100;
        QTest::newRow("96k") << 96000;
    }
    void playbackPreservesPhaseAcrossCaptureAndNetworkJitter() {
        QFETCH(int, rate);
        AudioPlaybackBuffer queue; AudioPlaybackRenderer renderer(rate);
        constexpr qint64 sourceEpoch = 900000000, localEpoch = 1000000;
        constexpr double pi = 3.14159265358979323846;
        std::array<float, 1920> pcm{};
        std::vector<float> output(size_t(rate / 100) * 2);
        int packet = 0;
        float previous = 0, maximumJump = 0;
        double energy = 0;
        AudioPlaybackRenderer::Clock lastClock;
        for (int callback = 0; callback < 600; ++callback) {
            const auto local = localEpoch + callback * 10000;
            // 200 ppm clock mismatch, +/-500 us native timestamp jitter, and
            // a separate changing network delay of up to 25 ms.
            while (localEpoch + packet * 20004 + 20000 + (packet * 13 % 26) * 1000 <= local) {
                const qint64 captureJitter = (packet % 3 - 1) * 500;
                for (int frame = 0; frame < 960; ++frame) {
                    const auto sample = float(0.3 * std::sin(2 * pi * 997 * (packet * 960 + frame) / 48000));
                    pcm[frame * 2] = sample; pcm[frame * 2 + 1] = -sample;
                }
                const auto timestamp = sourceEpoch + packet * 20004 + captureJitter;
                QVERIFY(queue.push(pcm.data(), timestamp, localEpoch + 80000 + packet * 20004 + captureJitter));
                ++packet;
            }
            std::fill(output.begin(), output.end(), 0.0f);
            const auto clock = renderer.render(queue, output.data(), rate / 100, 2, rate, local);
            if (clock.sourceUs >= 0) lastClock = clock;
            for (size_t frame = 0; frame < output.size() / 2; ++frame) {
                const auto value = output[frame * 2];
                QVERIFY(std::isfinite(value));
                QCOMPARE(output[frame * 2 + 1], -value);
                if (callback > 12) {
                    maximumJump = std::max(maximumJump, std::abs(value - previous));
                    energy += value * value;
                }
                previous = value;
            }
        }
        const float expectedMaximumJump = float(0.6 * std::sin(pi * 997 / rate));
        QVERIFY2(maximumJump < expectedMaximumJump * 1.15f,
            qPrintable(QStringLiteral("A packet boundary changed waveform phase: jump=%1 bound=%2")
                .arg(maximumJump).arg(expectedMaximumJump * 1.15f)));
        QVERIFY(energy > rate * 0.2);
        QCOMPARE(renderer.takeUnderruns(), 0);
        QCOMPARE(renderer.takeRebuffers(), 0);
        QVERIFY(lastClock.sourceUs > sourceEpoch);
        QVERIFY(std::abs((lastClock.localUs - localEpoch) - (lastClock.sourceUs - sourceEpoch) - 80000) < 5000);
    }
    void playbackFadesDropoutsAndRebuffersChangedDeadlines() {
        AudioPlaybackBuffer queue; AudioPlaybackRenderer renderer;
        std::array<float, 1920> pcm; pcm.fill(0.25f);
        std::array<float, 960> output{};
        constexpr qint64 epoch = 900000000, local = 1000000;
        QVERIFY(queue.push(pcm.data(), epoch, local));
        QVERIFY(queue.push(pcm.data(), epoch + 20000, local + 20000));
        float previous = 0, maximumJump = 0;
        for (int callback = 0; callback < 6; ++callback) {
            output.fill(0);
            renderer.render(queue, output.data(), 480, 2, 48000, local + callback * 10000);
            for (int frame = 0; frame < 480; ++frame) {
                maximumJump = std::max(maximumJump, std::abs(output[frame * 2] - previous));
                previous = output[frame * 2];
            }
        }
        QCOMPARE(renderer.takeUnderruns(), 1);
        QVERIFY(maximumJump < 0.003f); // 5 ms fades, no hard step into silence.
        QVERIFY(std::all_of(output.begin(), output.end(), [](float value) { return value == 0; }));
        // An adaptive jitter target rose 50 ms. Recovery must respect the new
        // deadline immediately, not spend seconds drifting toward it.
        QVERIFY(queue.push(pcm.data(), epoch + 60000, local + 110000));
        QVERIFY(queue.push(pcm.data(), epoch + 80000, local + 130000));
        for (int callback = 6; callback < 11; ++callback) {
            output.fill(0);
            const auto clock = renderer.render(queue, output.data(), 480, 2, 48000, local + callback * 10000);
            QCOMPARE(clock.sourceUs, qint64(-1));
        }
        output.fill(0);
        const auto clock = renderer.render(queue, output.data(), 480, 2, 48000, local + 110000);
        QVERIFY(clock.sourceUs >= epoch + 60000 && clock.sourceUs < epoch + 71000);
        QVERIFY(renderer.takeRebuffers() > 0);
    }
    void playbackBandlimitsLowRateDeviceFallback() {
        auto energy = [](double frequency) {
            AudioPlaybackBuffer queue; AudioPlaybackRenderer renderer(16000);
            std::array<float, 1920> pcm{};
            std::array<float, 160> output{};
            constexpr double pi = 3.14159265358979323846;
            int packet = 0;
            double sum = 0;
            for (int callback = 0; callback < 30; ++callback) {
                while (queue.size() < 4) {
                    for (int frame = 0; frame < 960; ++frame) {
                        const auto sample = float(0.3 * std::sin(2 * pi * frequency * (packet * 960 + frame) / 48000));
                        pcm[frame * 2] = sample; pcm[frame * 2 + 1] = sample;
                    }
                    queue.push(pcm.data(), 900000000 + packet * 20000, 1000000 + packet * 20000); ++packet;
                }
                output.fill(0);
                renderer.render(queue, output.data(), 160, 1, 16000, 1000000 + callback * 10000);
                if (callback > 5) for (float value : output) sum += value * value;
            }
            return sum;
        };
        const auto passband = energy(3000), aboveNyquist = energy(13000);
        QVERIFY(passband > 100);
        QVERIFY2(aboveNyquist < passband * 0.001,
            qPrintable(QStringLiteral("Resampling aliased high-frequency audio: energy ratio=%1").arg(aboveNyquist / passband)));
    }
    void playbackHandlesAnEarlierDeadlineWithoutExtrapolatingPcm() {
        AudioPlaybackBuffer queue; AudioPlaybackRenderer renderer;
        std::array<float, 1920> pcm{};
        for (int frame = 0; frame < 960; ++frame) {
            pcm[frame * 2] = 0.2f + float(frame % 2) * 0.05f;
            pcm[frame * 2 + 1] = pcm[frame * 2];
        }
        for (int packet = 0; packet < 8; ++packet)
            QVERIFY(queue.push(pcm.data(), 900000000 + packet * 20000,
                1000000 + packet * 20000 + (packet < 2 ? 140000 : 80000)));
        std::array<float, 960> output{};
        for (int callback = 0; callback < 12; ++callback) {
            output.fill(0);
            renderer.render(queue, output.data(), 480, 2, 48000, 1140000 + callback * 10000);
            for (float value : output) { QVERIFY(std::isfinite(value)); QVERIFY(std::abs(value) < 0.3f); }
        }
        QVERIFY(renderer.takeRebuffers() > 0);
    }
    void playbackQueueIsBoundedAndPublishesCompletePcm() {
        AudioPlaybackBuffer queue;
        std::array<float, 1920> pcm{};
        for (int i = 0; i < AudioPlaybackBuffer::Capacity; ++i)
            QVERIFY(queue.push(pcm.data(), i * 20000, i * 20000));
        QVERIFY(!queue.push(pcm.data(), AudioPlaybackBuffer::Capacity * 20000, 0));
        QCOMPARE(queue.size(), size_t(AudioPlaybackBuffer::Capacity));
        while (queue.peek()) queue.pop();
        std::atomic<bool> valid{true};
        constexpr int packets = 20000;
        std::thread producer([&] {
            std::array<float, 1920> samples{};
            for (int packet = 0; packet < packets; ++packet) {
                samples.fill(float(packet));
                // Producer retries are only for this stress test; the live
                // live producer requests a callback-safe discard when full.
                while (!queue.push(samples.data(), qint64(packet + 20) * 20000, packet))
                    std::this_thread::yield();
            }
        });
        for (int packet = 0; packet < packets; ++packet) {
            const AudioPlaybackBuffer::Block* block;
            while (!(block = queue.peek())) std::this_thread::yield();
            if (block->sourceTimestampUs != qint64(packet + 20) * 20000 || block->presentationUs != packet
                || !std::all_of(block->pcm.begin(), block->pcm.end(), [packet](float value) { return value == float(packet); }))
                valid.store(false);
            queue.pop();
        }
        producer.join();
        QVERIFY(valid.load()); QCOMPARE(queue.size(), size_t(0));
    }
    void timelineRecoversLatencyChanges() {
        AudioPlaybackTimeline timeline;
        const qint64 epoch = 900000000;
        auto decision = timeline.enqueue(epoch, 1000000);
        QVERIFY(decision.accept && decision.rebuffer);
        QCOMPARE(timeline.sourceAt(1080000), epoch);
        for (int frame = 1; frame < 20; ++frame) {
            decision = timeline.enqueue(epoch + frame * 20000, 1000000 + frame * 20000);
            QVERIFY(decision.accept && !decision.rebuffer);
        }
        // A permanent extra 150 ms of transit delay must not cause every
        // subsequent packet to miss the old playout anchor indefinitely.
        int recovered = 0;
        for (int frame = 20; frame < 30; ++frame) {
            decision = timeline.enqueue(epoch + frame * 20000, 1150000 + frame * 20000);
            recovered += decision.accept;
        }
        QVERIFY(recovered >= 8);
        const auto margin = epoch + 29 * 20000 - timeline.sourceAt(1150000 + 29 * 20000);
        QVERIFY(margin >= 60000 && margin <= 100000);
        decision = timeline.enqueue(epoch + 30 * 20000, 1160000 + 30 * 20000, true);
        QVERIFY(decision.accept && decision.rebuffer);
        timeline.reset();
        QVERIFY(timeline.enqueue(epoch + 31 * 20000, 3000000).rebuffer);
    }
    void timelineControlsLongRunningClockDrift() {
        AudioPlaybackTimeline timeline;
        const qint64 epoch = 900000000;
        for (int frame = 0; frame < 10000; ++frame) {
            const auto result = timeline.enqueue(epoch + frame * 20000, 1000000 + frame * 20020);
            QVERIFY(result.accept); // A 0.1% source clock mismatch stays playable.
            const auto margin = epoch + frame * 20000 - timeline.sourceAt(1000000 + frame * 20020);
            QVERIFY(margin > 40000 && margin <= 150000);
        }
    }
    void lateAudioUsesTheVideoDeadlineInsteadOfStartingAnotherBuffer() {
        AudioPlaybackTimeline timeline;
        const qint64 source = 900000000;
        // Video established source->local=1s; audio arrives 70ms later.
        auto decision = timeline.enqueue(source, 1070000, false, 1080000);
        QVERIFY(decision.accept);
        QCOMPARE(timeline.sourceAt(1080000), source);
        // An expired packet must not turn its late arrival into a new epoch.
        decision = timeline.enqueue(source + 20000, 3100000, false, 1100000);
        QVERIFY(!decision.accept);
        QCOMPARE(timeline.sourceAt(1100000), source + 20000);
        decision = timeline.enqueue(source + 2200000, 3260000, false, 3280000);
        QVERIFY(decision.accept);
        QCOMPARE(timeline.sourceAt(3280000), source + 2200000);
    }
    void malformedRemoteAudioReportsTheFailingSourceAndEpoch() {
        AudioEngine client;
        QSignalSpy states(&client, &AudioEngine::captureStateChanged);
        QSignalSpy errors(&client, &AudioEngine::playbackFailed);
        const auto source = QStringLiteral("decoder-failure-source");
        const auto epoch = QUuid::createUuid().toString(QUuid::WithoutBraces);
        for (int i = 0; i < 5; ++i)
            client.playPacket(source, epoch, i + 1, AudioEngine::nowUs(), QByteArray::fromHex("ff"));
        QTRY_COMPARE_WITH_TIMEOUT(errors.size(), 1, 3000);
        QCOMPARE(errors.first().at(0).toString(), source);
        QCOMPARE(errors.first().at(1).toString(), epoch);
        QVERIFY(errors.first().at(2).toString().contains(QStringLiteral("could not be decoded")));
        QTest::qWait(100);
        QCOMPARE(errors.size(), 1);
        client.shutdown();
    }
    void captureBufferPreservesSystemAudioAndFadesMissingSamples() {
        AudioCaptureBuffer buffer; buffer.reset(1000000);
        const auto system = constantPcm(960, 0.1f);
        buffer.append(reinterpret_cast<const float*>(system.constData()), 960, 1000000);
        // Repeated native samples must not be summed or amplified.
        buffer.append(reinterpret_cast<const float*>(system.constData()), 960, 1000000);
        QVERIFY(!buffer.take(1079950));
        auto first = buffer.take(1080000); QVERIFY(first);
        QCOMPARE(first->timestampUs, 1000000); QVERIFY(first->discontinuity);
        const auto* pcm = reinterpret_cast<const float*>(first->pcm.constData());
        QCOMPARE(pcm[0], 0.0f);
        for (int frame = AudioCaptureBuffer::FadeFrames; frame < 960; ++frame)
            QVERIFY(std::abs(pcm[frame * 2] - 0.1f) < 0.00001);
        for (int frame = 1; frame < 960; ++frame)
            QVERIFY(std::abs(pcm[frame * 2] - pcm[(frame - 1) * 2]) < 0.002f);
        auto silent = buffer.take(1100000); QVERIFY(silent); QVERIFY(!silent->discontinuity);
        pcm = reinterpret_cast<const float*>(silent->pcm.constData());
        QVERIFY(std::abs(pcm[0] - 0.1f) < 0.00001);
        for (int frame = AudioCaptureBuffer::FadeFrames; frame < 960; ++frame)
            QCOMPARE(pcm[frame * 2], 0.0f);
        buffer.append(reinterpret_cast<const float*>(system.constData()), 960, 1040000, true);
        auto resumed = buffer.take(1120000); QVERIFY(resumed);
        pcm = reinterpret_cast<const float*>(resumed->pcm.constData());
        QCOMPARE(pcm[0], 0.0f);
        for (int frame = AudioCaptureBuffer::FadeFrames; frame < 960; ++frame)
            QVERIFY(std::abs(pcm[frame * 2] - 0.1f) < 0.00001);
    }
    void captureBufferBoundsBacklogAndClearsOldEpochs() {
        AudioCaptureBuffer buffer; buffer.reset(1000000);
        const auto system = constantPcm(960, 0.2f);
        buffer.append(reinterpret_cast<const float*>(system.constData()), 960, 1000000);
        auto live = buffer.take(2000000); QVERIFY(live && live->discontinuity);
        QVERIFY(live->timestampUs >= 1900000); QCOMPARE(energy(live->pcm), 0);
        QVERIFY(!buffer.take(2000000));
        buffer.clear(); QVERIFY(!buffer.take(3000000));
        buffer.reset(4000000);
        buffer.append(reinterpret_cast<const float*>(system.constData()), 960, 1000000);
        auto fresh = buffer.take(4080000); QVERIFY(fresh); QCOMPARE(energy(fresh->pcm), 0);
        buffer.reset(5000000);
        buffer.append(reinterpret_cast<const float*>(system.constData()), 960, 5000000);
        buffer.append(reinterpret_cast<const float*>(system.constData()), 960, 5000000);
        auto duplicate = buffer.take(5080000); QVERIFY(duplicate);
        const auto* pcm = reinterpret_cast<const float*>(duplicate->pcm.constData());
        for (int frame = AudioCaptureBuffer::FadeFrames; frame < 960; ++frame)
            QVERIFY(std::abs(pcm[frame * 2] - 0.2f) < 0.00001);
    }
    void capturePermissionFailureStopAndRestartFenceQueuedCallbacks() {
        auto callbacks = std::make_shared<FakeCaptureState>();
        AudioEngine engine(nullptr, [callbacks] { return std::make_unique<FakeCapture>(callbacks); });
        QSignalSpy packets(&engine, &AudioEngine::packetReady), states(&engine, &AudioEngine::captureStateChanged);
        engine.startCapture("first");
        callbacks->state(false, "permission denied");
        QTRY_COMPARE(states.size(), 1);
        QTest::qWait(120); QVERIFY(packets.isEmpty());
        auto oldPcm = callbacks->pcm; auto oldState = callbacks->state;
        engine.startCapture("second"); callbacks->state(true, {});
        QTRY_VERIFY(!states.isEmpty() && states.last().at(0).toBool());
        oldState(false, "late failure");
        oldPcm(constantPcm(960, 1), AudioEngine::nowUs(), false);
        QTRY_VERIFY_WITH_TIMEOUT(packets.size() >= 2, 1000);
        AudioStreamDecoder decoder; QString error;
        for (const auto& packet : packets) {
            QCOMPARE(packet.at(0).toString(), QString("second"));
            QCOMPARE(energy(decoder.decode(packet.at(3).toByteArray(), error)), 0);
        }
        engine.stopCapture(); packets.clear();
        callbacks->state(true, {}); callbacks->pcm(constantPcm(960, 1), AudioEngine::nowUs(), false);
        QTest::qWait(120); QVERIFY(packets.isEmpty());
        QVERIFY(callbacks->stops >= 2);
        engine.startCapture("third"); callbacks->state(true, {});
        QTRY_VERIFY_WITH_TIMEOUT(!packets.isEmpty(), 1000);
        QCOMPARE(packets.first().at(0).toString(), QString("third"));
        QCOMPARE(packets.first().at(1).toULongLong(), quint64(1));
        engine.shutdown(); packets.clear(); engine.startCapture("after-shutdown");
        QTest::qWait(100); QVERIFY(packets.isEmpty());
    }
    void remoteEpochMuteAndClock() {
        if (QMediaDevices::defaultAudioOutput().isNull()) QSKIP("No audio output");
        AudioEngine engine; QSignalSpy clocks(&engine, &AudioEngine::playbackClock);
        AudioStreamEncoder encoder; QString error; QVERIFY(encoder.initialize(96000, error));
        std::array<float, 1920> silence{};
        QString epoch = "first"; quint64 sequence = 0;
        QTimer producer; producer.setInterval(20);
        connect(&producer, &QTimer::timeout, &engine, [&] {
            const auto now = AudioEngine::nowUs();
            engine.playPacket("endpoint", epoch, ++sequence, now, encoder.encode(silence.data(), error), now + 80000);
        });
        producer.start(); QTRY_VERIFY_WITH_TIMEOUT(!clocks.isEmpty(), 3000);
        QCOMPARE(clocks.last().at(1).toString(), epoch);
        engine.setPlaybackMuted(true); QTest::qWait(80); clocks.clear(); QTest::qWait(150); QVERIFY(clocks.isEmpty());
        epoch = "second"; sequence = 0; engine.setPlaybackMuted(false);
        QTRY_VERIFY_WITH_TIMEOUT(!clocks.isEmpty(), 3000); QCOMPARE(clocks.last().at(1).toString(), epoch);
        producer.stop(); engine.resetPlayback("endpoint"); QTest::qWait(80);
        clocks.clear(); QTest::qWait(150); QVERIFY(clocks.isEmpty());
    }
    void systemCapturePreservesStereo_data() {
        QTest::addColumn<int>("bitrate");
        QTest::newRow("96k") << 96000;
        QTest::newRow("32k") << 32000;
    }
    void systemCapturePreservesStereo() {
        QFETCH(int, bitrate);
        auto callbacks = std::make_shared<FakeCaptureState>();
        AudioEngine engine(nullptr, [callbacks] { return std::make_unique<FakeCapture>(callbacks); });
        QSignalSpy states(&engine, &AudioEngine::captureStateChanged);
        engine.startCapture("stereo-system", bitrate);
        callbacks->state(true, {});
        QTRY_VERIFY(!states.isEmpty() && states.last().at(0).toBool());
        const auto asset = toneAsset(400, 1000); QVERIFY(asset);
        const auto system = asset->firstAudioPcm.left(960 * 2 * sizeof(float));
        const auto origin = AudioEngine::nowUs();
        quint64 sequence = 0;
        QTimer producer; producer.setInterval(10); producer.setTimerType(Qt::PreciseTimer);
        connect(&producer, &QTimer::timeout, &engine, [&] {
            const auto complete = quint64(std::max<qint64>(0, AudioEngine::nowUs() - origin) / 20000);
            while (sequence < complete)
                callbacks->pcm(system, origin + qint64(sequence++) * 20000, false);
        });
        AudioStreamDecoder decoder;
        std::array<std::array<double, 2>, 2> amplitude{};
        int packets = 0; QString error;
        const auto connection = connect(&engine, &AudioEngine::packetReady, &engine,
            [&](const QString&, quint64, qint64, const QByteArray& packet) {
                QCOMPARE(opus_packet_get_nb_channels(reinterpret_cast<const unsigned char*>(packet.constData())), 2);
                const auto pcm = decoder.decode(packet, error);
                QCOMPARE(pcm.size(), qsizetype(960 * 2 * sizeof(float)));
                const auto* samples = reinterpret_cast<const float*>(pcm.constData());
                constexpr std::array<int, 2> frequencies{400, 1000};
                for (int channel = 0; channel < 2; ++channel) {
                    for (int frequency = 0; frequency < 2; ++frequency) {
                        double real = 0, imaginary = 0;
                        for (int frame = 0; frame < 960; ++frame) {
                            const double angle = 6.283185307179586 * frequencies[frequency] * frame / 48000;
                            const float sample = samples[frame * 2 + channel];
                            QVERIFY(std::isfinite(sample));
                            real += sample * std::cos(angle); imaginary += sample * std::sin(angle);
                        }
                        amplitude[channel][frequency] += 2 * std::hypot(real, imaginary) / 960;
                    }
                }
                ++packets;
            });
        const auto cleanup = qScopeGuard([&] { disconnect(connection); producer.stop(); engine.shutdown(); });
        producer.start();
        QTest::qWait(300); amplitude = {}; packets = 0;
        QTRY_VERIFY_WITH_TIMEOUT(packets >= 30, 2000);
        const double left = amplitude[0][0] / packets, right = amplitude[1][1] / packets;
        QVERIFY(left > 0.018 && left < 0.038);
        QVERIFY(right > 0.018 && right < 0.038);
        QVERIFY2(amplitude[0][1] / packets < right * 0.05, "Right system channel leaked into the left channel");
        QVERIFY2(amplitude[1][0] / packets < left * 0.05, "Left system channel leaked into the right channel");
    }
    void applicationPlaybackIsExcluded_data() {
        QTest::addColumn<bool>("native");
        QTest::newRow("in-process-system-source") << false;
        QTest::newRow("native-capture-opt-in") << true;
    }
    void applicationPlaybackIsExcluded() {
        QFETCH(bool, native);
        if (native && !qEnvironmentVariableIntValue("MOUFFETTE_TEST_SYSTEM_AUDIO_CAPTURE"))
            QSKIP("Set MOUFFETTE_TEST_SYSTEM_AUDIO_CAPTURE=1 for native capture (quiet test tones)");
#ifdef Q_OS_MACOS
        if (native && !CGPreflightScreenCaptureAccess())
            QSKIP("Screen recording access is not already granted; no permission prompt requested");
#endif
        if (QMediaDevices::defaultAudioOutput().isNull()) QSKIP("No audio output for the real playback exclusion test");
        auto callbacks = std::make_shared<FakeCaptureState>();
        AudioEngine engine(nullptr, native ? AudioEngine::CaptureFactory{} : [callbacks] { return std::make_unique<FakeCapture>(callbacks); });
        QSignalSpy states(&engine, &AudioEngine::captureStateChanged), clocks(&engine, &AudioEngine::playbackClock);
        engine.startCapture("routing");
        if (!native) callbacks->state(true, {});
        QTRY_VERIFY_WITH_TIMEOUT(!states.isEmpty(), 10000);
        QVERIFY2(states.last().at(0).toBool(), qPrintable(states.last().at(1).toString()));
        const auto sceneAsset = toneAsset(400), previewAsset = toneAsset(1000);
        QVERIFY(sceneAsset && previewAsset);
        QAudioOutput sceneOutput, previewOutput;
        PlaybackAudio scene, preview;
        scene.setOutput(&sceneOutput); scene.setAsset(sceneAsset);
        preview.setOutput(&previewOutput); preview.setAsset(previewAsset);
        QTRY_VERIFY(scene.preparedAt(0) && preview.preparedAt(0));
        scene.play(0); preview.play(0);
        AudioStreamEncoder monitorEncoder; QString error; QVERIFY(monitorEncoder.initialize(96000, error));
        quint64 sequence = 0;
        const auto systemOrigin = AudioEngine::nowUs();
        quint64 systemSequence = 0;
        QTimer producer; producer.setInterval(20); producer.setTimerType(Qt::PreciseTimer);
        connect(&producer, &QTimer::timeout, &engine, [&] {
            const auto now = AudioEngine::nowUs();
            std::array<float, 1920> monitor{};
            auto system = constantPcm(960, 0);
            auto* pcm = reinterpret_cast<float*>(system.data());
            for (int frame = 0; frame < 960; ++frame) {
                monitor[frame * 2] = monitor[frame * 2 + 1] = 0.03f * std::sin(6.283185307179586 * 1600 * frame / 48000);
                pcm[frame * 2] = pcm[frame * 2 + 1] = 0.015f * std::sin(6.283185307179586 * 800 * frame / 48000);
            }
            if (!native) {
                // Model a native sample clock, not the number of GUI timer
                // wakeups: Qt coalesces missed intervals during sink startup.
                const auto complete = quint64(std::max<qint64>(0, now - systemOrigin) / 20000);
                while (systemSequence < complete)
                    callbacks->pcm(system, systemOrigin + qint64(systemSequence++) * 20000, false);
            }
            engine.playPacket("monitor", "monitor", ++sequence, now, monitorEncoder.encode(monitor.data(), error), now + 80000);
        });
        producer.start();
        QTRY_VERIFY(scene.presentedSincePlay() && preview.presentedSincePlay() && !clocks.isEmpty());
        AudioStreamDecoder decoder;
        std::array<double, 4> amplitudes{};
        int count = 0;
        const auto connection = connect(&engine, &AudioEngine::packetReady, &engine,
            [&](const QString&, quint64, qint64 timestamp, const QByteArray& packet) {
                QVERIFY(AudioEngine::nowUs() - timestamp < 250000);
                const auto pcm = decoder.decode(packet, error); QVERIFY2(!pcm.isEmpty(), qPrintable(error));
                const auto* samples = reinterpret_cast<const float*>(pcm.constData());
                const std::array<int, 4> frequencies{400, 1000, 1600, 800};
                for (int f = 0; f < 4; ++f) {
                    double real = 0, imaginary = 0;
                    for (int frame = 0; frame < 960; ++frame) {
                        const double angle = 6.283185307179586 * frequencies[f] * frame / 48000;
                        real += samples[frame * 2] * std::cos(angle);
                        imaginary += samples[frame * 2] * std::sin(angle);
                    }
                    amplitudes[f] += 2 * std::hypot(real, imaginary) / 960;
                }
                ++count;
            });
        const auto cleanup = qScopeGuard([&] { disconnect(connection); producer.stop(); scene.pause(); preview.pause(); engine.shutdown(); });
        QTest::qWait(200); amplitudes.fill(0); count = 0;
        QTRY_VERIFY_WITH_TIMEOUT(count >= 20, 1500);
        auto verifyExclusion = [&] {
            qInfo() << "excluded scene/preview/monitor; system=" << amplitudes[0] / count
                    << amplitudes[1] / count << amplitudes[2] / count << amplitudes[3] / count;
            QVERIFY2(amplitudes[0] / count < 0.002, "Scene playback entered the system capture");
            QVERIFY2(amplitudes[1] / count < 0.002, "Canvas playback entered the system capture");
            QVERIFY2(amplitudes[2] / count < 0.002, "Remote monitoring entered the system capture");
            if (!native) QVERIFY2(amplitudes[3] / count > 0.008, "Other-application audio was lost");
        };
        verifyExclusion();
        sceneOutput.setMuted(true); QTest::qWait(200); amplitudes.fill(0); count = 0;
        QTRY_VERIFY_WITH_TIMEOUT(count >= 10, 1000); verifyExclusion();
        sceneOutput.setVolume(0.5); sceneOutput.setMuted(false);
        QTest::qWait(200); amplitudes.fill(0); count = 0;
        QTRY_VERIFY_WITH_TIMEOUT(count >= 15, 1200); verifyExclusion();
        // Incoming monitoring and local media controls cannot change publication.
        engine.setPlaybackMuted(true);
        QTest::qWait(150); amplitudes.fill(0); count = 0;
        QTRY_VERIFY_WITH_TIMEOUT(count >= 10, 1000); verifyExclusion();
        scene.pause(); QTest::qWait(200); amplitudes.fill(0); count = 0;
        QTRY_VERIFY_WITH_TIMEOUT(count >= 10, 1000); verifyExclusion();
        scene.play(0); QTest::qWait(200); amplitudes.fill(0); count = 0;
        QTRY_VERIFY_WITH_TIMEOUT(count >= 10, 1000); verifyExclusion();
        // Even without native PCM, active Mouffette playback publishes silence.
        if (!native) {
            producer.stop();
            QTest::qWait(300); amplitudes.fill(0); count = 0;
            QTRY_VERIFY_WITH_TIMEOUT(count >= 10, 1000);
            for (const auto amplitude : amplitudes) QVERIFY(amplitude / count < 0.002);
        }
    }
};
QTEST_MAIN(AudioEngineTest)
#include "tst_AudioEngine.moc"
