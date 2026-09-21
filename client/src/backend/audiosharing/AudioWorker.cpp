#include "backend/audiosharing/AudioWorker.h"
#include "backend/audiosharing/AudioWorkerProtocol.h"
#include "backend/audiosharing/AudioPreviewChannel.h"
#include "backend/audiosharing/AudioStreamCodec.h"
#include "backend/audiosharing/AudioPlaybackTimeline.h"
#include "backend/audiosharing/MediaCaptureClock.h"
#include "backend/audiosharing/SystemAudioCapture.h"
#include <QAudioDevice>
#include <QAudioSink>
#include <QGuiApplication>
#include <QMediaDevices>
#include <QMutex>
#include <QMutexLocker>
#include <QPointer>
#include <QScopeGuard>
#include <QSharedMemory>
#include <QTimer>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <deque>
#include <vector>

namespace {
QCborMap command(const char* type) { return {{QStringLiteral("type"), QString::fromLatin1(type)}}; }
void mixStereo(float* output, int channels, const float* pcm, int count, double position, float gain = 1) {
    const int first = std::clamp(int(position), 0, count - 1), second = std::min(first + 1, count - 1);
    const float fraction = float(std::clamp(position - first, 0.0, 1.0));
    const float left = (pcm[first * 2] + fraction * (pcm[second * 2] - pcm[first * 2])) * gain;
    const float right = (pcm[first * 2 + 1] + fraction * (pcm[second * 2 + 1] - pcm[first * 2 + 1])) * gain;
    if (channels == 1) output[0] += 0.5f * (left + right);
    else { output[0] += left; output[1] += right; }
}
struct AudioBlock { QByteArray pcm; qint64 timestampUs = 0; };
// Slots retain allocations until the GUI producer reuses them. The audio
// callback advances indices only, without allocating or freeing PCM memory.
struct PlaybackQueue {
    std::array<AudioBlock, 7> buffers; // 140 ms, strictly below the 150 ms ceiling.
    size_t first = 0, count = 0;
    bool empty() const { return count == 0; }
    size_t size() const { return count; }
    void clear() { first = 0; count = 0; }
    AudioBlock& front() { return buffers[first]; }
    void pop_front() { if (count) { first = (first + 1) % buffers.size(); --count; } }
    void push_back(AudioBlock block) { if (count < buffers.size()) { buffers[(first + count) % buffers.size()] = std::move(block); ++count; } }
};
struct RemoteSource {
    QString epoch;
    AudioStreamDecoder decoder;
    PlaybackQueue blocks;
    AudioPlaybackTimeline timeline;
    quint64 sequence = 0;
    bool received = false;
    qint64 consumedUs = -1, lastPacketUs = 0;
    int dropped = 0;
};
struct PreviewMapping {
    QSharedMemory memory;
    QByteArray deviceId;
    QByteArray resolvedDeviceId;
    explicit PreviewMapping(const QString& key) : memory(key) {}
    AudioPreviewState* state() { return static_cast<AudioPreviewState*>(memory.data()); }
};
struct OutputDevice {
    QByteArray id;
    QAudioFormat format;
    std::unique_ptr<QAudioSink> sink;
    qint64 anchorUs = 0;
    quint64 frames = 0;
};
// Native threads only write this bounded mailbox. The helper main thread owns
// the codec, IPC and stream state, so native callbacks never wait on a socket.
struct CaptureMailbox {
    QMutex mutex;
    std::deque<AudioBlock> blocks;
    qsizetype bytes = 0;
    bool queued = false, closed = false;
};
class AudioWorker final : public QObject {
public:
    QLocalSocket socket;
    QMediaDevices devices;
    QTimer reports;
    QByteArray input;
    std::unique_ptr<SystemAudioCapture> capture;
    std::shared_ptr<CaptureMailbox> captureMailbox;
    AudioStreamEncoder encoder;
    QString captureEpoch;
    QByteArray pendingPcm;
    qint64 pendingTimestamp = 0;
    quint64 captureSequence = 0;
    bool captureFailed = false;
    int bitrate = 96000;
    QMutex mixerMutex;
    QHash<QString, std::shared_ptr<RemoteSource>> remote;
    QHash<QString, std::shared_ptr<PreviewMapping>> previews;
    std::vector<std::unique_ptr<OutputDevice>> outputs;
    bool remoteMuted = false;
    QByteArray defaultOutputId;
    int reportTicks = 0;

    AudioWorker(const QString& server, const QString& token) {
        connect(&socket, &QLocalSocket::connected, this, [this, token] {
            auto hello = command("hello"); hello.insert(QStringLiteral("token"), token);
            AudioWorkerProtocol::sendControl(&socket, hello);
        });
        connect(&socket, &QLocalSocket::readyRead, this, [this] { readMessages(); });
        connect(&socket, &QLocalSocket::disconnected, this, [] { QCoreApplication::quit(); });
        connect(&socket, &QLocalSocket::errorOccurred, this, [](QLocalSocket::LocalSocketError) { QCoreApplication::quit(); });
        connect(&devices, &QMediaDevices::audioOutputsChanged, this, [this] { rebuildOutputs(); });
        reports.setInterval(20);
        connect(&reports, &QTimer::timeout, this, [this] { reportClocks(); });
        reports.start();
        socket.connectToServer(server);
        QTimer::singleShot(5000, this, [this] { if (socket.state() != QLocalSocket::ConnectedState) QCoreApplication::quit(); });
    }
    ~AudioWorker() override { stopCapture(); outputs.clear(); }
    void error(const QString& value) {
        auto message = command("error"); message.insert(QStringLiteral("error"), value);
        AudioWorkerProtocol::sendControl(&socket, message);
    }
    void state(bool active, const QString& value = {}, const QString& stoppedEpoch = {}) {
        auto message = command("capture-state"); message.insert(QStringLiteral("active"), active);
        message.insert(QStringLiteral("epoch"), stoppedEpoch.isEmpty() ? captureEpoch : stoppedEpoch);
        message.insert(QStringLiteral("error"), value); AudioWorkerProtocol::sendControl(&socket, message);
    }
    void stopCapture() {
        if (captureMailbox) { QMutexLocker lock(&captureMailbox->mutex); captureMailbox->closed = true; captureMailbox->blocks.clear(); }
        captureMailbox.reset();
        if (capture) capture->stop();
        capture.reset(); pendingPcm.clear(); captureEpoch.clear();
    }
    void startCapture(const QString& epoch, int requestedBitrate) {
        if (epoch.isEmpty()) return;
        bitrate = requestedBitrate <= 32000 ? 32000 : 96000;
        if (capture && captureEpoch == epoch && !captureFailed) { encoder.setBitrate(bitrate); return; }
        stopCapture();
        captureEpoch = epoch;
        QString failure;
        if (!encoder.initialize(bitrate, failure)) { state(false, failure); return; }
        encoder.reset(); captureSequence = 0; captureEpoch = epoch; captureFailed = false;
        captureMailbox = std::make_shared<CaptureMailbox>();
        const auto mailbox = captureMailbox;
        capture = createSystemAudioCapture();
        if (!capture) { state(false, QStringLiteral("System audio capture is unavailable")); return; }
        capture->start([this, mailbox](QByteArray pcm, qint64 timestamp) {
            constexpr qsizetype maximumBytes = 4800 * 2 * sizeof(float);
            if (pcm.isEmpty() || pcm.size() % (2 * sizeof(float))) return;
            if (pcm.size() > maximumBytes) {
                const auto skipped = pcm.size() - maximumBytes;
                timestamp += qint64(skipped / (2 * sizeof(float))) * 1000000 / 48000;
                pcm = pcm.right(maximumBytes);
            }
            QMutexLocker lock(&mailbox->mutex);
            if (mailbox->closed) return;
            // At most 100 ms of native buffers. A slow consumer resumes live.
            while (!mailbox->blocks.empty() && mailbox->bytes + pcm.size() > maximumBytes) {
                mailbox->bytes -= mailbox->blocks.front().pcm.size(); mailbox->blocks.pop_front();
            }
            mailbox->bytes += pcm.size();
            mailbox->blocks.push_back({std::move(pcm), timestamp});
            if (mailbox->queued) return;
            mailbox->queued = true;
            QMetaObject::invokeMethod(this, [this, mailbox] { drainCapture(mailbox); }, Qt::QueuedConnection);
        }, [this, mailbox](bool active, QString failure) {
            QMutexLocker lock(&mailbox->mutex);
            if (mailbox->closed) return;
            QMetaObject::invokeMethod(this, [this, mailbox, active, failure] {
                if (captureMailbox == mailbox) { captureFailed = !active; state(active, failure); }
            }, Qt::QueuedConnection);
        });
    }
    void drainCapture(const std::shared_ptr<CaptureMailbox>& mailbox) {
        std::deque<AudioBlock> blocks;
        {
            QMutexLocker lock(&mailbox->mutex); mailbox->queued = false;
            if (mailbox->closed || captureMailbox != mailbox) return;
            blocks.swap(mailbox->blocks); mailbox->bytes = 0;
        }
        constexpr int packetBytes = 960 * 2 * sizeof(float);
        for (auto& block : blocks) {
            if (block.pcm.size() % (2 * sizeof(float)) != 0) continue;
            const qint64 expected = pendingTimestamp + qint64(pendingPcm.size() / (2 * sizeof(float))) * 1000000 / 48000;
            if (pendingPcm.isEmpty() || std::abs(block.timestampUs - expected) > 30000) {
                pendingPcm.clear(); pendingTimestamp = block.timestampUs;
            }
            pendingPcm.append(block.pcm);
            while (pendingPcm.size() >= packetBytes) {
                QString failure;
                const auto packet = encoder.encode(reinterpret_cast<const float*>(pendingPcm.constData()), failure);
                if (!failure.isEmpty()) {
                    const auto failedEpoch = captureEpoch;
                    stopCapture(); state(false, failure, failedEpoch); return;
                }
                auto message = command("packet"); message.insert(QStringLiteral("epoch"), captureEpoch);
                message.insert(QStringLiteral("sequence"), qint64(++captureSequence));
                message.insert(QStringLiteral("timestamp"), pendingTimestamp);
                message.insert(QStringLiteral("opus"), packet); AudioWorkerProtocol::send(&socket, message);
                pendingPcm.remove(0, packetBytes); pendingTimestamp += 20000;
            }
        }
    }
    void readMessages() {
        input += socket.readAll();
        if (input.size() > 2 * AudioWorkerProtocol::MaximumMessage) { socket.abort(); return; }
        QCborMap message;
        bool malformed = false;
        while (AudioWorkerProtocol::take(input, message, malformed)) {
            const auto type = message.value(QStringLiteral("type")).toString();
            if (type == QLatin1String("quit")) { QCoreApplication::quit(); return; }
            if (type == QLatin1String("capture")) startCapture(message.value(QStringLiteral("epoch")).toString(), int(message.value(QStringLiteral("bitrate")).toInteger(96000)));
            else if (type == QLatin1String("capture-stop")) {
                const auto epoch = message.value(QStringLiteral("epoch")).toString();
                stopCapture(); state(false, {}, epoch);
            }
            else if (type == QLatin1String("bitrate")) encoder.setBitrate(int(message.value(QStringLiteral("bitrate")).toInteger(96000)));
            else if (type == QLatin1String("play")) receivePacket(message);
            else if (type == QLatin1String("reset")) {
                { QMutexLocker lock(&mixerMutex); remote.remove(message.value(QStringLiteral("source")).toString()); }
                ensureOutputs();
            } else if (type == QLatin1String("mute")) {
                { QMutexLocker lock(&mixerMutex); remoteMuted = message.value(QStringLiteral("muted")).toBool(); remote.clear(); }
                ensureOutputs();
            } else if (type == QLatin1String("preview-add")) {
                const auto key = message.value(QStringLiteral("key")).toString();
                if (!key.startsWith(QLatin1String("mouffette-pcm-")) || key.size() > 100 || previews.size() >= 256) continue;
                auto mapping = std::make_shared<PreviewMapping>(key);
                if (!mapping->memory.attach() || mapping->memory.size() != sizeof(AudioPreviewState)
                    || mapping->state()->magic != 0x4d415031) continue;
                mapping->deviceId = message.value(QStringLiteral("device")).toByteArray();
                { QMutexLocker lock(&mixerMutex); previews.insert(key, mapping); }
                ensureOutputs();
            } else if (type == QLatin1String("preview-remove")) {
                { QMutexLocker lock(&mixerMutex); previews.remove(message.value(QStringLiteral("key")).toString()); }
                ensureOutputs();
            }
        }
        if (malformed) socket.abort();
    }
    void receivePacket(const QCborMap& message) {
        const auto sourceId = message.value(QStringLiteral("source")).toString();
        const auto epoch = message.value(QStringLiteral("epoch")).toString();
        const auto packet = message.value(QStringLiteral("opus")).toByteArray();
        const qint64 timestamp = message.value(QStringLiteral("timestamp")).toInteger(-1);
        const qint64 sequence = message.value(QStringLiteral("sequence")).toInteger(-1);
        if (sourceId.isEmpty() || sourceId.size() > 256 || epoch.isEmpty() || epoch.size() > 256
            || timestamp < 0 || sequence < 0 || remoteMuted) return;
        std::shared_ptr<RemoteSource> source;
        {
            QMutexLocker lock(&mixerMutex);
            if (!remote.contains(sourceId) && remote.size() >= 64) return;
            auto& existing = remote[sourceId];
            if (!existing || existing->epoch != epoch) { existing = std::make_shared<RemoteSource>(); existing->epoch = epoch; }
            source = existing;
            if (source->received && quint64(sequence) <= source->sequence) return;
            if (source->received && quint64(sequence) > source->sequence + 1)
                source->dropped += int(std::min<quint64>(quint64(sequence) - source->sequence - 1, 1000));
        }
        QString failure;
        auto pcm = source->decoder.decode(packet, failure);
        if (pcm.isEmpty()) return;
        const auto now = MediaCaptureClock::nowUs();
        {
            QMutexLocker lock(&mixerMutex);
            const auto decision = source->timeline.enqueue(timestamp, now, source->blocks.size() >= 7);
            if (decision.rebuffer) {
                source->dropped += int(source->blocks.size());
                source->blocks.clear(); source->consumedUs = -1;
            }
            source->sequence = quint64(sequence); source->received = true; source->lastPacketUs = now;
            if (decision.accept) source->blocks.push_back({std::move(pcm), timestamp});
            else ++source->dropped;
        }
        ensureOutputs();
    }
    QAudioDevice deviceFor(const QByteArray& id) {
        for (const auto& device : QMediaDevices::audioOutputs()) if (device.id() == id) return device;
        return QMediaDevices::defaultAudioOutput();
    }
    void ensureOutputs() {
        QList<QByteArray> ids;
        const auto defaultDevice = QMediaDevices::defaultAudioOutput();
        { QMutexLocker lock(&mixerMutex); defaultOutputId = defaultDevice.id(); }
        if (!remote.isEmpty() && !defaultDevice.isNull()) ids.append(defaultDevice.id());
        for (const auto& preview : previews) {
            const auto device = deviceFor(preview->deviceId);
            { QMutexLocker lock(&mixerMutex); preview->resolvedDeviceId = device.id(); }
            if (!device.isNull() && !ids.contains(device.id())) ids.append(device.id());
        }
        outputs.erase(std::remove_if(outputs.begin(), outputs.end(), [&ids](const auto& output) {
            return !ids.contains(output->id);
        }), outputs.end());
        for (const auto& id : ids) {
            if (std::any_of(outputs.begin(), outputs.end(), [&id](const auto& output) { return output->id == id; })) continue;
            const auto device = deviceFor(id);
            auto output = std::make_unique<OutputDevice>();
            output->id = id; output->format = device.preferredFormat(); output->format.setSampleFormat(QAudioFormat::Float);
            QAudioFormat streamFormat;
            streamFormat.setSampleRate(48000); streamFormat.setChannelCount(2); streamFormat.setSampleFormat(QAudioFormat::Float);
            if (device.isFormatSupported(streamFormat)) output->format = streamFormat;
            if (output->format.channelCount() > 8) output->format.setChannelCount(2);
            if (!device.isFormatSupported(output->format) || output->format.sampleRate() < 8000) {
                error(QStringLiteral("No supported audio output format")); continue;
            }
            output->sink = std::make_unique<QAudioSink>(device, output->format);
            output->sink->setBufferSize(output->format.bytesForDuration(20000));
            auto* pointer = output.get();
            output->sink->start([this, pointer](QSpan<float> samples) { render(*pointer, samples); });
            if (output->sink->error() != QtAudio::NoError) { error(QStringLiteral("Audio output could not start")); continue; }
            outputs.push_back(std::move(output));
        }
        for (const auto& preview : previews) preview->state()->consumerAvailable.store(
            std::any_of(outputs.begin(), outputs.end(), [&preview](const auto& output) {
                return output->id == preview->resolvedDeviceId;
            }), std::memory_order_release);
    }
    void rebuildOutputs() {
        outputs.clear();
        { QMutexLocker lock(&mixerMutex); for (auto& source : remote) { source->blocks.clear(); source->timeline.reset(); source->received = false; source->consumedUs = -1; } }
        ensureOutputs();
    }
    void render(OutputDevice& output, QSpan<float> samples) {
        std::fill(samples.begin(), samples.end(), 0.0f);
        if (!mixerMutex.tryLock()) return;
        const auto unlock = qScopeGuard([this] { mixerMutex.unlock(); });
        const int rate = output.format.sampleRate(), channels = output.format.channelCount();
        const int frames = int(samples.size()) / channels;
        const auto wall = MediaCaptureClock::nowUs();
        if (!output.anchorUs) output.anchorUs = wall;
        qint64 clock = output.anchorUs + qint64(output.frames * 1000000 / rate);
        if (std::abs(clock - wall) > 200000) { output.anchorUs = wall; output.frames = 0; clock = wall; }
        else if (std::abs(clock - wall) > 2000) output.anchorUs += clock < wall ? 1 : -1;
        output.frames += frames;
        const auto& defaultId = defaultOutputId;
        if (!remoteMuted && output.id == defaultId) {
            for (auto& source : remote) {
                const qint64 start = source->timeline.sourceAt(clock);
                bool consumed = false;
                for (int frame = 0; frame < frames; ++frame) {
                    const qint64 time = start + qint64(frame) * 1000000 / rate;
                    while (!source->blocks.empty() && source->blocks.front().timestampUs + 20000 <= time) source->blocks.pop_front();
                    if (source->blocks.empty() || time < source->blocks.front().timestampUs) continue;
                    const auto& block = source->blocks.front();
                    const double position = (start - block.timestampUs) * (48000.0 / 1000000) + frame * (48000.0 / rate);
                    if (position < 0 || position >= 960) continue;
                    const auto* pcm = reinterpret_cast<const float*>(block.pcm.constData());
                    mixStereo(samples.data() + frame * channels, channels, pcm, 960, position);
                    consumed = true;
                }
                if (consumed) source->consumedUs = start + qint64(frames) * 1000000 / rate;
            }
        }
        for (const auto& mapping : previews) {
            if (mapping->resolvedDeviceId != output.id) continue;
            auto* voice = mapping->state();
            const auto generation = voice->generation.load(std::memory_order_acquire);
            const bool playing = voice->playing.load(std::memory_order_acquire);
            const float gain = voice->gain.load(std::memory_order_relaxed);
            const qint64 target = clock + voice->clockOffsetUs.load(std::memory_order_relaxed);
            for (auto& block : voice->blocks) {
                if (block.state.load(std::memory_order_acquire) != 2) continue;
                if (block.generation.load() != generation) { block.state.store(0, std::memory_order_release); continue; }
                if (!playing) continue;
                const qint64 start = qRound64((block.startUs.load() - target) * (double(rate) / 1000000));
                const qint64 duration = qint64(AudioPreviewBlockFrames) * rate / 48000;
                const qint64 first = std::max<qint64>(0, start), last = std::min<qint64>(frames, start + duration);
                if (last > first) voice->presented.store(true, std::memory_order_relaxed);
                for (qint64 frame = first; frame < last; ++frame) {
                    const double position = (frame - start) * (48000.0 / rate);
                    mixStereo(samples.data() + frame * channels, channels, block.samples.data(), AudioPreviewBlockFrames, position, gain);
                }
                if (start + duration <= frames) block.state.store(0, std::memory_order_release);
            }
        }
        for (auto& sample : samples) sample = std::clamp(sample, -1.0f, 1.0f);
    }
    void reportClocks() {
        QList<QCborMap> messages;
        {
            QMutexLocker lock(&mixerMutex);
            const auto now = MediaCaptureClock::nowUs();
            const bool feedbackDue = ++reportTicks % 25 == 0;
            for (auto it = remote.begin(); it != remote.end();) {
                if (now - it.value()->lastPacketUs > 5000000) { it = remote.erase(it); continue; }
                auto& source = *it.value();
                if (feedbackDue) {
                    auto feedback = command("feedback"); feedback.insert(QStringLiteral("source"), it.key());
                    feedback.insert(QStringLiteral("epoch"), source.epoch); feedback.insert(QStringLiteral("dropped"), source.dropped);
                    feedback.insert(QStringLiteral("buffered"), int(source.blocks.size()) * 20);
                    messages.append(feedback); source.dropped = 0;
                }
                if (source.consumedUs >= 0 && now - source.lastPacketUs < 300000) {
                    auto message = command("clock"); message.insert(QStringLiteral("source"), it.key());
                    message.insert(QStringLiteral("epoch"), source.epoch); message.insert(QStringLiteral("timestamp"), source.consumedUs);
                    messages.append(message); source.consumedUs = -1;
                }
                ++it;
            }
        }
        for (const auto& message : messages) AudioWorkerProtocol::send(&socket, message);
        if (reportTicks % 25 == 0) ensureOutputs();
    }
};
}
int runAudioWorker(int argc, char** argv) {
    if (argc != 4) return 64;
    QGuiApplication app(argc, argv);
    initializeAudioWorkerPlatform();
#ifdef Q_OS_MACOS
    QCoreApplication::addLibraryPath(QCoreApplication::applicationDirPath() + QStringLiteral("/../PlugIns"));
#endif
    app.setQuitOnLastWindowClosed(false);
    AudioWorker worker(QString::fromLocal8Bit(argv[2]), QString::fromLocal8Bit(argv[3]));
    return app.exec();
}
