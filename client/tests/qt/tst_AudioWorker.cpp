#include "backend/audiosharing/AudioWorker.h"
#include "backend/audiosharing/AudioWorkerClient.h"
#include "backend/audiosharing/AudioWorkerProtocol.h"
#include "backend/audiosharing/AudioPlaybackTimeline.h"
#include "backend/audiosharing/AudioPlaybackBuffer.h"
#include "backend/audiosharing/AudioStreamCodec.h"
#include "backend/media/MediaDecoder.h"
#include "backend/media/PlaybackAudio.h"
#include <QGuiApplication>
#include <QAudioDevice>
#include <QAudioSink>
#include <QAudioOutput>
#include <QLocalServer>
#include <QMediaDevices>
#include <QProcess>
#include <QScopeGuard>
#include <QSharedMemory>
#include <QSignalSpy>
#include <QTimer>
#include <QUuid>
#include <QtTest>
#include <opus.h>
#include <array>
#include <cmath>
#include <thread>

class AudioWorkerTest final : public QObject {
    Q_OBJECT
private slots:
    void helperPreviewMappingValidation_data() {
        QTest::addColumn<int>("capacity");
        QTest::addColumn<int>("headerFault");
        QTest::addColumn<QString>("expectedError");
        const int bytes = sizeof(AudioPreviewState);
        QTest::newRow("exact") << bytes << 0 << QString();
        // Explicit padding reproduces Windows allocation semantics on macOS and
        // Linux too. Acceptance must not depend on a particular OS page size.
        QTest::newRow("windows-page-rounded") << ((bytes + 4095) / 4096 * 4096) << 0 << QString();
        QTest::newRow("larger-allocation") << (bytes + 16384) << 0 << QString();
        QTest::newRow("truncated") << 16 << 0 << QStringLiteral("too small");
        QTest::newRow("wrong-magic") << bytes << 1 << QStringLiteral("Incompatible");
        QTest::newRow("wrong-version") << bytes << 2 << QStringLiteral("Incompatible");
        QTest::newRow("smaller-logical-size") << bytes << 3 << QStringLiteral("Incompatible");
        QTest::newRow("larger-logical-size") << bytes << 4 << QStringLiteral("Incompatible");
        QTest::newRow("missing-segment") << 0 << 0 << QStringLiteral("Could not attach");
        QTest::newRow("invalid-key") << 0 << 5 << QStringLiteral("Invalid");
    }
    void helperPreviewMappingValidation() {
        QFETCH(int, capacity); QFETCH(int, headerFault); QFETCH(QString, expectedError);
        const auto id = QUuid::createUuid().toString(QUuid::WithoutBraces);
        const auto key = (headerFault == 5 ? QStringLiteral("invalid-") : QStringLiteral("mouffette-pcm-")) + id;
        QSharedMemory memory(key);
        AudioPreviewState* shared = nullptr;
        if (capacity > 0) {
            QVERIFY2(memory.create(capacity), qPrintable(memory.errorString()));
            if (capacity >= int(sizeof(AudioPreviewState))) {
                shared = new (memory.data()) AudioPreviewState;
                if (headerFault == 1) shared->magic = 0;
                if (headerFault == 2) ++shared->version;
                if (headerFault == 3) --shared->byteSize;
                if (headerFault == 4) ++shared->byteSize;
            }
        }
        QLocalServer server;
        server.setSocketOptions(QLocalServer::UserAccessOption);
        QVERIFY2(server.listen(QStringLiteral("mft-%1").arg(id)), qPrintable(server.errorString()));
        QProcess helper;
        const auto cleanup = qScopeGuard([&] {
            if (helper.state() != QProcess::NotRunning) {
                helper.terminate();
                if (!helper.waitForFinished(1000)) { helper.kill(); helper.waitForFinished(1000); }
            }
        });
        helper.start(qApp->property("mouffetteAudioWorkerExecutable").toString(),
            {QStringLiteral("--audio-worker"), server.serverName(), QStringLiteral("test-token")});
        QTRY_VERIFY_WITH_TIMEOUT(server.hasPendingConnections(), 5000);
        auto* socket = server.nextPendingConnection();
        QByteArray input;
        QList<QCborMap> replies;
        auto receive = [&] {
            input += socket->readAll();
            QCborMap message; bool malformed = false;
            while (AudioWorkerProtocol::take(input, message, malformed))
                if (message.value(QStringLiteral("type")).toString() == QLatin1String("preview-state")) replies.append(message);
            QVERIFY(!malformed);
        };
        const auto connection = connect(socket, &QLocalSocket::readyRead, this, receive);
        const auto disconnectReceiver = qScopeGuard([&] { disconnect(connection); });
        receive();
        const QCborMap registration{{QStringLiteral("type"), QStringLiteral("preview-add")},
            {QStringLiteral("key"), key}, {QStringLiteral("device"), QByteArray()}};
        // Validate replay as well as initial registration. These checks must
        // run without an audio device; attachment is not device availability.
        for (int attempt = 1; attempt <= 2; ++attempt) {
            QVERIFY(AudioWorkerProtocol::sendControl(socket, registration));
            QTRY_COMPARE_WITH_TIMEOUT(replies.size(), attempt, 3000);
            const auto reply = replies.last();
            QCOMPARE(reply.value(QStringLiteral("key")).toString(), key);
            QCOMPARE(reply.value(QStringLiteral("attached")).toBool(), expectedError.isEmpty());
            const auto failure = reply.value(QStringLiteral("error")).toString();
            if (expectedError.isEmpty()) QVERIFY2(failure.isEmpty(), qPrintable(failure));
            else QVERIFY2(failure.contains(expectedError), qPrintable(failure));
        }
        if (shared && expectedError.isEmpty() && !QMediaDevices::defaultAudioOutput().isNull()) {
            QTRY_VERIFY_WITH_TIMEOUT(shared->consumerAvailable.load(), 3000);
            const auto start = AudioWorkerClient::nowUs() + 80000;
            for (int index = 0; index < AudioPreviewBlockCount; ++index) {
                auto& block = shared->blocks[index];
                block.generation.store(1); block.startUs.store(start + index * 50000);
                block.state.store(2, std::memory_order_release);
            }
            shared->playing.store(true, std::memory_order_release);
            QTRY_VERIFY_WITH_TIMEOUT(shared->presented.load(), 1000);
            QTRY_COMPARE_WITH_TIMEOUT(shared->blocks.back().state.load(), 0, 1500);
        }
        QVERIFY(AudioWorkerProtocol::sendControl(socket, {{QStringLiteral("type"), QStringLiteral("quit")}}));
        QTRY_COMPARE_WITH_TIMEOUT(helper.state(), QProcess::NotRunning, 3000);
        QCOMPARE(helper.exitStatus(), QProcess::NormalExit); QCOMPARE(helper.exitCode(), 0);
    }
    void previewRejectionIsScopedToItsChannel() {
        AudioWorkerClient client;
        QSignalSpy attachments(&client, &AudioWorkerClient::previewAttachmentChanged);
        QSignalSpy remoteFailure(&client, &AudioWorkerClient::playbackFailed);
        QSignalSpy workerFailure(&client, &AudioWorkerClient::failed);
        auto rejected = client.createPreviewChannel(); QVERIFY(rejected);
        // The parent has not handled the handshake yet, so registration cannot
        // race this intentionally incompatible immutable header.
        ++rejected->state()->version;
        auto accepted = client.createPreviewChannel(); QVERIFY(accepted);
        QVERIFY(!rejected->isAttachedToWorker()); QVERIFY(!accepted->isAttachedToWorker());
        QTRY_COMPARE_WITH_TIMEOUT(attachments.size(), 2, 5000);
        QVERIFY(!rejected->isAttachedToWorker()); QVERIFY(accepted->isAttachedToWorker());
        QVERIFY(!rejected->state()->consumerAvailable.load());
        for (const auto& reply : attachments) {
            const bool valid = reply.at(0).toString() == accepted->key();
            QCOMPARE(reply.at(1).toBool(), valid);
            if (valid) QVERIFY(reply.at(2).toString().isEmpty());
            else {
                QCOMPARE(reply.at(0).toString(), rejected->key());
                QVERIFY(reply.at(2).toString().contains(QStringLiteral("Incompatible")));
            }
        }
        QVERIFY(remoteFailure.isEmpty()); QVERIFY(workerFailure.isEmpty());
        client.shutdown();
        QVERIFY(!accepted->isAttachedToWorker());
    }
    void unacknowledgedPreviewFailsWithoutResettingRemotePlayback() {
        // A protocol peer that authenticates but never acknowledges previews
        // models an incompatible or stalled helper, independently of devices.
        const auto previousMode = qgetenv("MOUFFETTE_TEST_IGNORE_PREVIEW_REGISTRATION");
        const auto previousExecutable = qApp->property("mouffetteAudioWorkerExecutable");
        const auto restore = qScopeGuard([&] {
            if (previousMode.isNull()) qunsetenv("MOUFFETTE_TEST_IGNORE_PREVIEW_REGISTRATION");
            else qputenv("MOUFFETTE_TEST_IGNORE_PREVIEW_REGISTRATION", previousMode);
            qApp->setProperty("mouffetteAudioWorkerExecutable", previousExecutable);
        });
        qputenv("MOUFFETTE_TEST_IGNORE_PREVIEW_REGISTRATION", "1");
        qApp->setProperty("mouffetteAudioWorkerExecutable", QCoreApplication::applicationFilePath());
        AudioWorkerClient client;
        QSignalSpy attachments(&client, &AudioWorkerClient::previewAttachmentChanged);
        QSignalSpy remoteFailure(&client, &AudioWorkerClient::playbackFailed);
        QSignalSpy workerFailure(&client, &AudioWorkerClient::failed);
        QSignalSpy captureState(&client, &AudioWorkerClient::captureStateChanged);
        auto preview = client.createPreviewChannel(); QVERIFY(preview);
        QTRY_VERIFY_WITH_TIMEOUT(!captureState.isEmpty(), 3000); // Real authenticated handshake.
        auto removed = client.createPreviewChannel(); QVERIFY(removed);
        removed.reset(); // An unanswered, deleted channel must not emit a failure.
        QTRY_COMPARE_WITH_TIMEOUT(attachments.size(), 1, 8000);
        QCOMPARE(attachments.first().at(0).toString(), preview->key());
        QVERIFY(!attachments.first().at(1).toBool());
        QVERIFY(attachments.first().at(2).toString().contains(QStringLiteral("did not acknowledge")));
        QVERIFY(!preview->isAttachedToWorker());
        QVERIFY(remoteFailure.isEmpty()); QVERIFY(workerFailure.isEmpty());
        client.shutdown();
    }
    void canvasAudioWaitsForAttachment_data() {
        QTest::addColumn<bool>("ignoreRegistration");
        QTest::newRow("decode-and-play") << false;
        QTest::newRow("attachment-timeout-reaches-player") << true;
    }
    void canvasAudioWaitsForAttachment() {
        QFETCH(bool, ignoreRegistration);
        if (QMediaDevices::defaultAudioOutput().isNull()) QSKIP("No audio output; protocol tests still cover attachment");
        const auto path = QFINDTESTDATA("../fixtures/resident-timeline.mp4");
        QVERIFY(!path.isEmpty());
        const auto asset = MediaDecoder::decode(path);
        QVERIFY(asset && asset->audioPackets.codec);
        const auto previousMode = qgetenv("MOUFFETTE_TEST_IGNORE_PREVIEW_REGISTRATION");
        const auto previousExecutable = qApp->property("mouffetteAudioWorkerExecutable");
        const auto restore = qScopeGuard([&] {
            if (previousMode.isNull()) qunsetenv("MOUFFETTE_TEST_IGNORE_PREVIEW_REGISTRATION");
            else qputenv("MOUFFETTE_TEST_IGNORE_PREVIEW_REGISTRATION", previousMode);
            qApp->setProperty("mouffetteAudioWorkerExecutable", previousExecutable);
        });
        qputenv("MOUFFETTE_TEST_IGNORE_PREVIEW_REGISTRATION", ignoreRegistration ? "1" : "0");
        if (ignoreRegistration) qApp->setProperty("mouffetteAudioWorkerExecutable", QCoreApplication::applicationFilePath());
        auto* worker = AudioWorkerClient::instance();
        const auto cleanup = qScopeGuard([worker] { delete worker; });
        QAudioOutput output; output.setMuted(true);
        PlaybackAudio audio;
        QSignalSpy failures(&audio, &PlaybackAudio::failed);
        audio.setRole(PlaybackAudio::Role::ControlPreview);
        audio.setOutput(&output); audio.setAsset(asset);
        // Decoder readiness alone must not hide an unattached preview channel.
        QVERIFY(!audio.preparedAt(0)); QVERIFY(!audio.presentedSincePlay());
        if (ignoreRegistration) {
            QTRY_COMPARE_WITH_TIMEOUT(failures.size(), 1, 8000);
            QVERIFY(failures.first().first().toString().contains(QStringLiteral("did not acknowledge")));
            QVERIFY(!audio.preparedAt(0)); QVERIFY(!audio.presentedSincePlay());
        } else {
            QTRY_VERIFY_WITH_TIMEOUT(audio.preparedAt(0), 5000);
            audio.play(0);
            QTRY_VERIFY_WITH_TIMEOUT(audio.presentedSincePlay(), 2000);
            QVERIFY(failures.isEmpty());
        }
    }
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
    void protocolHandlesFragmentationAndRejectsInvalidFrames() {
        const QCborMap expected{{QStringLiteral("type"), QStringLiteral("mute")}, {QStringLiteral("muted"), true}};
        const auto body = QCborValue(expected).toCbor();
        QByteArray frame(4, Qt::Uninitialized); qToBigEndian<quint32>(body.size(), frame.data()); frame += body;
        QByteArray partial; QCborMap actual; bool malformed = false;
        for (qsizetype index = 0; index < frame.size() - 1; ++index) {
            partial += frame[index]; QVERIFY(!AudioWorkerProtocol::take(partial, actual, malformed)); QVERIFY(!malformed);
        }
        partial += frame.back(); QVERIFY(AudioWorkerProtocol::take(partial, actual, malformed));
        QCOMPARE(actual, expected); QVERIFY(partial.isEmpty());
        QByteArray oversized(4, '\0'); qToBigEndian<quint32>(AudioWorkerProtocol::MaximumMessage + 1, oversized.data());
        QVERIFY(!AudioWorkerProtocol::take(oversized, actual, malformed)); QVERIFY(malformed);
        QByteArray scalar(5, '\0'); qToBigEndian<quint32>(1, scalar.data()); scalar[4] = char(1);
        QVERIFY(!AudioWorkerProtocol::take(scalar, actual, malformed)); QVERIFY(malformed);
        QLocalSocket disconnected; QVERIFY(!AudioWorkerProtocol::sendControl(&disconnected, expected));
        QCOMPARE(disconnected.state(), QLocalSocket::UnconnectedState);
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
        QVERIFY(maximumJump < 0.003f); // 3 ms fades, no hard step into silence.
        QVERIFY(std::all_of(output.begin(), output.end(), [](float value) { return value == 0; }));
        // An adaptive jitter target rose 50 ms. Recovery must respect the new
        // deadline immediately, not spend seconds drifting toward it.
        QVERIFY(queue.push(pcm.data(), epoch + 60000, local + 110000));
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
                // producer drops newest when full to keep latency bounded.
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
    void helperDiscardsAudioDelayedInsideTheLocalSocket() {
        if (QMediaDevices::defaultAudioOutput().isNull()) QSKIP("No audio output");
        QLocalServer server;
        server.setSocketOptions(QLocalServer::UserAccessOption);
        const auto name = QStringLiteral("mft-%1").arg(QUuid::createUuid().toString(QUuid::WithoutBraces));
        QVERIFY2(server.listen(name), qPrintable(server.errorString()));
        QProcess helper;
        const auto cleanup = qScopeGuard([&] {
            if (helper.state() != QProcess::NotRunning) {
                helper.terminate();
                if (!helper.waitForFinished(1000)) { helper.kill(); helper.waitForFinished(1000); }
            }
        });
        helper.start(qApp->property("mouffetteAudioWorkerExecutable").toString(),
            {QStringLiteral("--audio-worker"), name, QStringLiteral("test-token")});
        QTRY_VERIFY_WITH_TIMEOUT(server.hasPendingConnections(), 5000);
        auto* socket = server.nextPendingConnection();
        QByteArray input;
        QList<QCborMap> messages;
        auto receive = [&] {
            input += socket->readAll();
            QCborMap message; bool malformed = false;
            while (AudioWorkerProtocol::take(input, message, malformed)) messages.append(message);
        };
        const auto receiveConnection = connect(socket, &QLocalSocket::readyRead, this, receive);
        const auto disconnectReceiver = qScopeGuard([&] { disconnect(receiveConnection); });
        receive();
        QTRY_VERIFY_WITH_TIMEOUT(!messages.isEmpty(), 3000);
        QCOMPARE(messages.first().value(QStringLiteral("token")).toString(), QStringLiteral("test-token"));
        auto clockCount = [&] {
            return std::count_if(messages.cbegin(), messages.cend(), [](const auto& message) {
                return message.value(QStringLiteral("type")).toString() == QLatin1String("clock");
            });
        };
        AudioStreamEncoder encoder; QString error;
        QVERIFY(encoder.initialize(96000, error));
        std::array<float, 1920> silence{};
        const auto opus = encoder.encode(silence.data(), error);
        quint64 sequence = 0;
        auto sendPacket = [&](qint64 age) {
            const auto time = AudioWorkerClient::nowUs();
            return AudioWorkerProtocol::send(socket, {
                {QStringLiteral("type"), QStringLiteral("play")},
                {QStringLiteral("source"), QStringLiteral("endpoint")},
                {QStringLiteral("epoch"), QStringLiteral("epoch")},
                {QStringLiteral("sequence"), qint64(++sequence)},
                {QStringLiteral("timestamp"), time - age},
                {QStringLiteral("receivedAt"), time - age},
                {QStringLiteral("opus"), opus}
            });
        };
        // A kernel IPC backlog was invisible to bytesToWrite(). It previously
        // reached the helper and became a fresh 80ms playback buffer.
        for (int i = 0; i < 4; ++i) QVERIFY(sendPacket(2000000));
        QTest::qWait(250);
        QCOMPARE(clockCount(), 0);
        QTimer producer;
        producer.setInterval(20);
        connect(&producer, &QTimer::timeout, this, [&] { sendPacket(0); });
        producer.start();
        QTRY_VERIFY_WITH_TIMEOUT(clockCount() > 0, 3000);
        producer.stop();
        QVERIFY(AudioWorkerProtocol::sendControl(socket, {{QStringLiteral("type"), QStringLiteral("quit")}}));
        QTRY_COMPARE_WITH_TIMEOUT(helper.state(), QProcess::NotRunning, 3000);
        QCOMPARE(helper.exitCode(), 0);
    }
    void helperHandshakeAndSharedConsumption() {
        AudioWorkerClient client;
        QSignalSpy state(&client, &AudioWorkerClient::captureStateChanged);
        QSignalSpy failure(&client, &AudioWorkerClient::failed);
        QSignalSpy attachments(&client, &AudioWorkerClient::previewAttachmentChanged);
        auto preview = client.createPreviewChannel(); QVERIFY(preview && preview->state());
        QTRY_VERIFY_WITH_TIMEOUT(!state.isEmpty(), 5000);
        QVERIFY2(failure.isEmpty(), failure.isEmpty() ? "" : qPrintable(failure.first().first().toString()));
        QCOMPARE(state.first().at(0).toBool(), false); QVERIFY(state.first().at(1).toString().isEmpty());
        QTRY_COMPARE_WITH_TIMEOUT(attachments.size(), 1, 3000);
        QCOMPARE(attachments.first().at(0).toString(), preview->key());
        QVERIFY(attachments.first().at(1).toBool()); QVERIFY(attachments.first().at(2).toString().isEmpty());
        QVERIFY(preview->isAttachedToWorker());
        if (QMediaDevices::defaultAudioOutput().isNull()) QSKIP("No audio output; helper handshake and preview attachment verified");
        QTRY_VERIFY_WITH_TIMEOUT(preview->state()->consumerAvailable.load(), 3000);
        auto* shared = preview->state();
        auto& stale = shared->blocks[0];
        stale.generation.store(1); stale.state.store(2, std::memory_order_release);
        shared->generation.store(2, std::memory_order_release);
        // The consumer retires old seeks even while transport is paused.
        QTRY_COMPARE_WITH_TIMEOUT(stale.state.load(), 0, 1000);
        const auto start = AudioWorkerClient::nowUs() + 80000;
        for (int index = 0; index < AudioPreviewBlockCount; ++index) {
            auto& block = shared->blocks[index];
            std::fill(block.samples.begin(), block.samples.end(), 0.0f);
            block.generation.store(2); block.startUs.store(start + index * 50000);
            block.state.store(2, std::memory_order_release);
        }
        shared->playing.store(true, std::memory_order_release);
        client.setPlaybackMuted(true); // Monitor mute does not mute local previews.
        QTRY_VERIFY_WITH_TIMEOUT(shared->presented.load(), 1000);
        QTRY_COMPARE_WITH_TIMEOUT(shared->blocks.back().state.load(), 0, 1500);
        std::thread([channel = std::move(preview)] {}).join(); // Decoder-job destruction is queued to the owner.
        QTest::qWait(30);
        client.shutdown();
    }
    void malformedRemoteAudioReportsTheFailingSourceAndEpoch() {
        AudioWorkerClient client;
        QSignalSpy states(&client, &AudioWorkerClient::captureStateChanged);
        QSignalSpy errors(&client, &AudioWorkerClient::playbackFailed);
        // Preparation authenticates the helper without starting native capture.
        auto preview = client.createPreviewChannel(); QVERIFY(preview);
        QTRY_VERIFY_WITH_TIMEOUT(!states.isEmpty(), 5000);
        const auto source = QStringLiteral("decoder-failure-source");
        const auto epoch = QUuid::createUuid().toString(QUuid::WithoutBraces);
        for (int i = 0; i < 5; ++i)
            client.playPacket(source, epoch, i + 1, AudioWorkerClient::nowUs(), QByteArray::fromHex("ff"));
        QTRY_COMPARE_WITH_TIMEOUT(errors.size(), 1, 3000);
        QCOMPARE(errors.first().at(0).toString(), source);
        QCOMPARE(errors.first().at(1).toString(), epoch);
        QVERIFY(errors.first().at(2).toString().contains(QStringLiteral("could not be decoded")));
        QTest::qWait(100);
        QCOMPARE(errors.size(), 1);
        client.shutdown();
    }
    void helperRemoteEpochMuteAndClock() {
        if (QMediaDevices::defaultAudioOutput().isNull()) QSKIP("No audio output");
        AudioWorkerClient client; QSignalSpy state(&client, &AudioWorkerClient::captureStateChanged);
        QSignalSpy clocks(&client, &AudioWorkerClient::playbackClock);
        QList<qint64> playbackAges;
        connect(&client, &AudioWorkerClient::playbackClock, &client,
            [&playbackAges](const QString&, const QString&, qint64 sourceTimestamp) {
                playbackAges.append(AudioWorkerClient::nowUs() - sourceTimestamp);
            });
        auto preview = client.createPreviewChannel(); QVERIFY(preview);
        QTRY_VERIFY_WITH_TIMEOUT(!state.isEmpty(), 5000);
        AudioStreamEncoder encoder; QString error; QVERIFY(encoder.initialize(96000, error));
        std::array<float, 1920> silence{};
        const auto opus = encoder.encode(silence.data(), error); QVERIFY(!opus.isEmpty());
        QString epoch = QStringLiteral("first"); quint64 sequence = 0;
        QTimer producer; producer.setInterval(20);
        connect(&producer, &QTimer::timeout, &client, [&] {
            const auto now = AudioWorkerClient::nowUs();
            client.playPacket(QStringLiteral("endpoint"), epoch, ++sequence, now, opus, now + 80000);
        });
        producer.start(); QTRY_VERIFY_WITH_TIMEOUT(!clocks.isEmpty(), 3000);
        QCOMPARE(clocks.last().at(0).toString(), QStringLiteral("endpoint")); QCOMPARE(clocks.last().at(1).toString(), epoch);
        QTRY_VERIFY_WITH_TIMEOUT(preview->isAttachedToWorker() && preview->state()->consumerAvailable.load(), 3000);
        auto* shared = preview->state();
        auto publishPreview = [&] {
            shared->presented.store(false);
            const auto start = AudioWorkerClient::nowUs() + 80000;
            for (int index = 0; index < AudioPreviewBlockCount; ++index) {
                auto& block = shared->blocks[index];
                QCOMPARE(block.state.load(), 0);
                block.generation.store(1); block.startUs.store(start + index * 50000);
                block.state.store(2, std::memory_order_release);
            }
            shared->playing.store(true, std::memory_order_release);
        };
        publishPreview();
        QTRY_VERIFY_WITH_TIMEOUT(shared->presented.load(), 1000);
        QTRY_COMPARE_WITH_TIMEOUT(shared->blocks.back().state.load(), 0, 1500);
        client.setPlaybackMuted(true); QTest::qWait(80); clocks.clear(); QTest::qWait(150); QVERIFY(clocks.isEmpty());
        publishPreview();
        QTRY_VERIFY_WITH_TIMEOUT(shared->presented.load(), 1000);
        QTRY_COMPARE_WITH_TIMEOUT(shared->blocks.back().state.load(), 0, 1500);
        QVERIFY(clocks.isEmpty());
        epoch = QStringLiteral("second"); sequence = 0; client.setPlaybackMuted(false);
        QTRY_VERIFY_WITH_TIMEOUT(!clocks.isEmpty(), 3000); QCOMPARE(clocks.last().at(1).toString(), epoch);
        producer.stop(); client.resetPlayback(QStringLiteral("endpoint")); QTest::qWait(80);
        clocks.clear(); QTest::qWait(150); QVERIFY(clocks.isEmpty());
        QVERIFY(!playbackAges.isEmpty());
        const auto maximumAge = *std::max_element(playbackAges.cbegin(), playbackAges.cend());
        QVERIFY2(maximumAge < 250000, qPrintable(QStringLiteral("Remote playback clock lagged by %1 us").arg(maximumAge)));
        client.shutdown();
    }
    void nativeCaptureSmokeOptIn() {
        if (!qEnvironmentVariableIntValue("MOUFFETTE_TEST_SYSTEM_AUDIO_CAPTURE"))
            QSKIP("Set MOUFFETTE_TEST_SYSTEM_AUDIO_CAPTURE=1 to test the authorized native capture backend");
        AudioWorkerClient client; QSignalSpy state(&client, &AudioWorkerClient::captureStateChanged);
        QSignalSpy packets(&client, &AudioWorkerClient::packetReady);
        QList<qint64> ages, arrivalGaps;
        qint64 lastArrival = 0;
        connect(&client, &AudioWorkerClient::packetReady, &client,
            [&ages, &arrivalGaps, &lastArrival](const QString&, quint64, qint64 timestamp, const QByteArray&) {
                const auto arrival = AudioWorkerClient::nowUs();
                ages.append(arrival - timestamp);
                if (lastArrival) arrivalGaps.append(arrival - lastArrival);
                lastArrival = arrival;
            });
        const auto epoch = QUuid::createUuid().toString();
        client.startCapture(epoch);
        QTRY_VERIFY_WITH_TIMEOUT(!state.isEmpty(), 10000);
        QVERIFY2(state.last().at(0).toBool(), qPrintable(state.last().at(1).toString()));

        const auto device = QMediaDevices::defaultAudioOutput();
        QAudioFormat format;
        format.setSampleRate(48000); format.setChannelCount(2); format.setSampleFormat(QAudioFormat::Float);
        if (device.isNull() || !device.isFormatSupported(format))
            QSKIP("Native capture activated; no 48 kHz stereo float output is available for the silent PCM probe");
        // This stream belongs to the test's main process, which the capture
        // helper must include. Silence exercises the native PCM/Opus path
        // without producing an audible test tone or relying on another app.
        QAudioSink silence(device, format);
        silence.setBufferSize(format.bytesForDuration(20000));
        silence.start([](QSpan<float> output) { std::fill(output.begin(), output.end(), 0.0f); });
        QCOMPARE(silence.error(), QtAudio::NoError);
        packets.clear();
        QTRY_VERIFY_WITH_TIMEOUT(packets.size() >= 3, 5000);
        AudioStreamDecoder decoder; QString error;
        quint64 previousSequence = 0; qint64 previousTimestamp = -1;
        for (int index = 0; index < 3; ++index) {
            const auto packet = packets.at(index);
            QCOMPARE(packet.at(0).toString(), epoch);
            const auto sequence = packet.at(1).toULongLong();
            const auto timestamp = packet.at(2).toLongLong();
            QVERIFY(sequence > previousSequence); QVERIFY(timestamp > previousTimestamp);
            QVERIFY(std::abs(AudioWorkerClient::nowUs() - timestamp) < 5000000);
            const auto decoded = decoder.decode(packet.at(3).toByteArray(), error);
            QVERIFY2(error.isEmpty(), qPrintable(error));
            QCOMPARE(decoded.size(), qsizetype(960 * 2 * sizeof(float)));
            const auto* samples = reinterpret_cast<const float*>(decoded.constData());
            for (int sample = 0; sample < 960 * 2; ++sample) QVERIFY(std::isfinite(samples[sample]));
            previousSequence = sequence; previousTimestamp = timestamp;
        }
        QTest::qWait(2000);
        QVERIFY(ages.size() >= 3 && !arrivalGaps.isEmpty());
        qint64 sum = 0;
        for (const auto age : ages) sum += age;
        qInfo().nospace() << "native_capture_probe packets=" << ages.size()
            << " age_us_avg=" << (sum / ages.size())
            << " age_us_min=" << *std::min_element(ages.cbegin(), ages.cend())
            << " age_us_max=" << *std::max_element(ages.cbegin(), ages.cend())
            << " arrival_gap_us_max=" << *std::max_element(arrivalGaps.cbegin(), arrivalGaps.cend());
        silence.stop();
        state.clear(); client.stopCapture(); QTRY_VERIFY_WITH_TIMEOUT(!state.isEmpty(), 3000);
        QCOMPARE(state.last().at(0).toBool(), false);
        client.shutdown();
    }
};

int main(int argc, char** argv) {
    if (argc > 1 && QByteArray(argv[1]) == "--audio-worker") {
        if (argc == 4 && qEnvironmentVariableIntValue("MOUFFETTE_TEST_IGNORE_PREVIEW_REGISTRATION")) {
            QCoreApplication app(argc, argv);
            QLocalSocket socket;
            QByteArray input;
            QObject::connect(&socket, &QLocalSocket::connected, &app, [&] {
                AudioWorkerProtocol::sendControl(&socket, {{QStringLiteral("type"), QStringLiteral("hello")},
                    {QStringLiteral("token"), QString::fromLocal8Bit(argv[3])}});
            });
            QObject::connect(&socket, &QLocalSocket::readyRead, &app, [&] {
                input += socket.readAll();
                QCborMap message; bool malformed = false;
                while (AudioWorkerProtocol::take(input, message, malformed)) {
                    const auto type = message.value(QStringLiteral("type")).toString();
                    if (type == QLatin1String("quit")) app.quit();
                    if (type == QLatin1String("capture-stop"))
                        AudioWorkerProtocol::sendControl(&socket, {{QStringLiteral("type"), QStringLiteral("capture-state")},
                            {QStringLiteral("active"), false}});
                }
                if (malformed) app.quit();
            });
            QObject::connect(&socket, &QLocalSocket::disconnected, &app, &QCoreApplication::quit);
            socket.connectToServer(QString::fromLocal8Bit(argv[2]));
            QTimer::singleShot(15000, &app, &QCoreApplication::quit);
            return app.exec();
        }
        return runAudioWorker(argc, argv);
    }
    QGuiApplication app(argc, argv);
    app.setProperty("mouffetteAudioWorkerExecutable", qEnvironmentVariable(
        "MOUFFETTE_TEST_AUDIO_WORKER_EXECUTABLE", QCoreApplication::applicationFilePath()));
    AudioWorkerTest test; return QTest::qExec(&test, argc, argv);
}
#include "tst_AudioWorker.moc"
