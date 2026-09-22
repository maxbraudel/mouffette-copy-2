#include "backend/audiosharing/AudioWorker.h"
#include "backend/audiosharing/AudioWorkerProtocol.h"
#include "backend/audiosharing/AudioPreviewChannel.h"
#include "backend/audiosharing/AudioStreamCodec.h"
#include "backend/audiosharing/AudioPlaybackTimeline.h"
#include "backend/audiosharing/AudioPlaybackBuffer.h"
#include "backend/audiosharing/AudioCapturePacketizer.h"
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
#include <opus.h>

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
struct AudioBlock { QByteArray pcm; qint64 timestampUs = 0; bool discontinuity = false; };
struct RemoteSource {
    QString epoch;
    AudioStreamDecoder decoder;
    AudioPlaybackBuffer blocks;
    AudioPlaybackTimeline timeline; // Producer thread only.
    AudioPlaybackRenderer renderer; // Device callback only.
    AudioPlaybackClock clock;
    std::atomic<bool> enabled{true};
    quint64 sequence = 0;
    bool received = false;
    qint64 lastTimestampUs = -1, lastPacketUs = 0, reportedLocalUs = -1;
    int dropped = 0, concealed = 0, rebuffers = 0;
};
struct PreviewMapping {
    QSharedMemory memory;
    QByteArray deviceId;
    QByteArray resolvedDeviceId;
    explicit PreviewMapping(const QString& key) : memory(key) {}
    AudioPreviewState* state() { return static_cast<AudioPreviewState*>(memory.data()); }
};
struct MixerSnapshot {
    std::vector<std::shared_ptr<RemoteSource>> remote;
    std::vector<std::shared_ptr<PreviewMapping>> previews;
};
struct OutputDevice {
    QByteArray id;
    QAudioFormat format;
    std::unique_ptr<QAudioSink> sink;
    std::atomic<const MixerSnapshot*> snapshot{nullptr};
    std::atomic<bool> rendering{false};
    std::atomic<const MixerSnapshot*> reader{nullptr};
    std::unique_ptr<MixerSnapshot> current;
    std::vector<std::unique_ptr<MixerSnapshot>> retired;
    quint64 revision = 0;
    qint64 anchorUs = 0;
    quint64 frames = 0;
    std::array<float, 8> softClipMemory{};
    ~OutputDevice() { if (sink) sink->stop(); }
    void publish(std::unique_ptr<MixerSnapshot> next) {
        snapshot.store(next.get(), std::memory_order_seq_cst);
        if (current) retired.push_back(std::move(current));
        current = std::move(next);
        collect();
    }
    void collect() {
        // A callback announces entry before reading snapshot. After publication
        // a new reader can only observe current, so old objects can be freed on
        // this main thread as soon as an existing reader has exited.
        if (!rendering.load(std::memory_order_seq_cst)) retired.clear();
        else if (const auto* inUse = reader.load(std::memory_order_seq_cst)) {
            // A long callback needs only its one snapshot, not every intermediate
            // control update. During callback entry reader is null and we defer.
            retired.erase(std::remove_if(retired.begin(), retired.end(), [inUse](const auto& item) {
                return item.get() != inUse;
            }), retired.end());
        }
    }
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
    QString captureEpoch, captureSequenceEpoch;
    AudioCapturePacketizer capturePacketizer;
    quint64 captureSequence = 0;
    bool captureFailed = false;
    int bitrate = 96000;
    QHash<QString, std::shared_ptr<RemoteSource>> remote;
    QHash<QString, std::shared_ptr<PreviewMapping>> previews;
    std::vector<std::unique_ptr<OutputDevice>> outputs;
    bool remoteMuted = false;
    QByteArray defaultOutputId;
    int reportTicks = 0;
    quint64 mixerRevision = 1;
    QHash<QByteArray, qint64> outputRetryUs;

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
    void previewState(const QString& key, const QString& failure = {}) {
        auto message = command("preview-state");
        message.insert(QStringLiteral("key"), key);
        message.insert(QStringLiteral("attached"), failure.isEmpty());
        message.insert(QStringLiteral("error"), failure);
        AudioWorkerProtocol::sendControl(&socket, message);
    }
    void addPreview(const QCborMap& message) {
        const auto key = message.value(QStringLiteral("key")).toString();
        if (!key.startsWith(QLatin1String("mouffette-pcm-")) || key.size() > 100) {
            previewState(key, QStringLiteral("Invalid preview audio shared-memory key")); return;
        }
        if (previews.contains(key)) { previewState(key); return; } // Idempotent replay.
        if (previews.size() >= 256) {
            previewState(key, QStringLiteral("Too many preview audio channels")); return;
        }
        auto mapping = std::make_shared<PreviewMapping>(key);
        if (!mapping->memory.attach()) {
            previewState(key, QStringLiteral("Could not attach preview audio shared memory: %1")
                .arg(mapping->memory.errorString())); return;
        }
        // QSharedMemory::size() may exceed create()'s request (VirtualQuery
        // reports page-rounded capacity on Windows). Validate capacity before
        // reading the header, then validate the logical ABI independently.
        if (mapping->memory.size() < qsizetype(sizeof(AudioPreviewState))) {
            previewState(key, QStringLiteral("Preview audio shared memory is too small: %1 bytes; need %2")
                .arg(mapping->memory.size()).arg(sizeof(AudioPreviewState))); return;
        }
        const auto* shared = mapping->state();
        if (shared->magic != AudioPreviewState::Magic || shared->version != AudioPreviewState::Version
            || shared->byteSize != sizeof(AudioPreviewState)) {
            previewState(key, QStringLiteral("Incompatible preview audio shared-memory format "
                "(magic %1, version %2, size %3)")
                .arg(shared->magic, 0, 16).arg(shared->version).arg(shared->byteSize)); return;
        }
        mapping->deviceId = message.value(QStringLiteral("device")).toByteArray();
        previews.insert(key, mapping); ++mixerRevision;
        ensureOutputs();
        previewState(key);
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
        capture.reset(); capturePacketizer.reset(); captureEpoch.clear();
    }
    void startCapture(const QString& epoch, int requestedBitrate) {
        if (epoch.isEmpty()) return;
        bitrate = requestedBitrate <= 32000 ? 32000 : 96000;
        if (capture && captureEpoch == epoch && !captureFailed) { encoder.setBitrate(bitrate); return; }
        stopCapture();
        captureEpoch = epoch;
        QString failure;
        if (!encoder.initialize(bitrate, failure)) { state(false, failure); return; }
        encoder.reset();
        if (captureSequenceEpoch != epoch) { captureSequence = 0; captureSequenceEpoch = epoch; }
        captureEpoch = epoch; captureFailed = false;
        captureMailbox = std::make_shared<CaptureMailbox>();
        const auto mailbox = captureMailbox;
        capture = createSystemAudioCapture();
        if (!capture) { state(false, QStringLiteral("System audio capture is unavailable")); return; }
        capture->start([this, mailbox](QByteArray pcm, qint64 timestamp, bool discontinuity) {
            constexpr qsizetype maximumBytes = 4800 * 2 * sizeof(float);
            if (pcm.isEmpty() || pcm.size() % (2 * sizeof(float))) return;
            if (pcm.size() > maximumBytes) {
                const auto skipped = pcm.size() - maximumBytes;
                timestamp += qint64(skipped / (2 * sizeof(float))) * 1000000 / 48000;
                pcm = pcm.right(maximumBytes); discontinuity = true;
            }
            QMutexLocker lock(&mailbox->mutex);
            if (mailbox->closed) return;
            // At most 100 ms of native buffers. A slow consumer resumes live.
            while (!mailbox->blocks.empty() && mailbox->bytes + pcm.size() > maximumBytes) {
                mailbox->bytes -= mailbox->blocks.front().pcm.size(); mailbox->blocks.pop_front();
                if (!mailbox->blocks.empty()) mailbox->blocks.front().discontinuity = true;
                else discontinuity = true;
            }
            mailbox->bytes += pcm.size();
            mailbox->blocks.push_back({std::move(pcm), timestamp, discontinuity});
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
        for (auto& block : blocks) {
            const auto age = MediaCaptureClock::nowUs() - block.timestampUs;
            if (age > 100000 || age < -250000) { capturePacketizer.reset(); continue; }
            capturePacketizer.append(std::move(block.pcm), block.timestampUs, block.discontinuity);
            while (auto chunk = capturePacketizer.take()) {
                if (chunk->discontinuity) encoder.reset();
                QString failure;
                const auto packet = encoder.encode(reinterpret_cast<const float*>(chunk->pcm.constData()), failure);
                if (!failure.isEmpty()) {
                    const auto failedEpoch = captureEpoch;
                    stopCapture(); state(false, failure, failedEpoch); return;
                }
                auto message = command("packet"); message.insert(QStringLiteral("epoch"), captureEpoch);
                message.insert(QStringLiteral("sequence"), qint64(++captureSequence));
                message.insert(QStringLiteral("timestamp"), std::max<qint64>(0, chunk->timestampUs - encoder.lookaheadUs()));
                message.insert(QStringLiteral("opus"), packet); AudioWorkerProtocol::send(&socket, message);
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
                const auto source = remote.take(message.value(QStringLiteral("source")).toString());
                if (source) source->enabled.store(false, std::memory_order_release);
                ++mixerRevision;
                ensureOutputs();
            } else if (type == QLatin1String("mute")) {
                remoteMuted = message.value(QStringLiteral("muted")).toBool();
                for (const auto& source : remote) source->enabled.store(false, std::memory_order_release);
                remote.clear(); ++mixerRevision;
                ensureOutputs();
            } else if (type == QLatin1String("preview-add")) {
                addPreview(message);
            } else if (type == QLatin1String("preview-remove")) {
                previews.remove(message.value(QStringLiteral("key")).toString()); ++mixerRevision;
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
        const qint64 receivedAt = message.value(QStringLiteral("receivedAt")).toInteger(-1);
        const qint64 presentation = message.value(QStringLiteral("presentation")).toInteger(-1);
        const qint64 ipcAge = MediaCaptureClock::nowUs() - receivedAt;
        if (sourceId.isEmpty() || sourceId.size() > 256 || epoch.isEmpty() || epoch.size() > 256
            || timestamp < 0 || sequence < 0 || remoteMuted || receivedAt < 0
            || ipcAge < -250000 || ipcAge > 100000) return;
        if (!remote.contains(sourceId) && remote.size() >= 64) return;
        auto& source = remote[sourceId];
        if (!source || source->epoch != epoch) {
            if (source) source->enabled.store(false, std::memory_order_release);
            source = std::make_shared<RemoteSource>(); source->epoch = epoch;
            ++mixerRevision;
        }
        if (source->received && quint64(sequence) <= source->sequence) return;
        // Timestamp gaps also reveal frames discarded before transport assigned
        // a sequence number. Never synthesize across a capture-clock reset.
        const qint64 elapsed = source->received ? timestamp - source->lastTimestampUs : 20000;
        if (source->received && elapsed <= 0) return;
        const int missing = elapsed >= 39000 && elapsed <= 140000
            ? int((elapsed + 1000) / 20000) - 1 : 0;
        const auto sequenceGap = source->received ? quint64(sequence) - source->sequence - 1 : 0;
        source->dropped += int(std::min<quint64>(std::max<quint64>(sequenceGap, missing), 1000));
        QString failure;
        auto queuePcm = [&](const QByteArray& pcm, qint64 time, qint64 deadline) {
            const auto now = MediaCaptureClock::nowUs();
            const auto decision = source->timeline.enqueue(time, now, false, deadline);
            if (decision.rebuffer && source->received) ++source->rebuffers;
            if (!decision.accept || !source->blocks.push(reinterpret_cast<const float*>(pcm.constData()),
                    time, source->timeline.presentationAt(time))) ++source->dropped;
        };
        if (source->received && (elapsed > 140000 || std::abs(elapsed - (missing + 1) * 20000) > 2000))
            source->decoder.reset();
        for (int lost = 0; lost < missing; ++lost) {
            // The immediately following Opus packet may repair its predecessor.
            // For CELT-only/music packets libopus falls back to its native PLC.
            const auto pcm = lost + 1 == missing ? source->decoder.decode(packet, failure, true)
                                               : source->decoder.conceal(failure);
            if (pcm.isEmpty()) break;
            ++source->concealed;
            const auto lostTime = timestamp - qint64(missing - lost) * 20000;
            queuePcm(pcm, lostTime, presentation < 0 ? -1 : presentation - qint64(missing - lost) * 20000);
        }
        const auto pcm = source->decoder.decode(packet, failure);
        if (pcm.isEmpty()) return;
        queuePcm(pcm, timestamp, presentation);
        source->sequence = quint64(sequence); source->received = true;
        source->lastTimestampUs = timestamp; source->lastPacketUs = MediaCaptureClock::nowUs();
        ensureOutputs();
    }
    QAudioDevice deviceFor(const QByteArray& id) {
        for (const auto& device : QMediaDevices::audioOutputs()) if (device.id() == id) return device;
        return QMediaDevices::defaultAudioOutput();
    }
    void ensureOutputs() {
        const auto now = MediaCaptureClock::nowUs();
        bool restart = false;
        for (const auto& output : outputs) {
            if (output->sink->error() == QtAudio::NoError && output->sink->state() != QtAudio::StoppedState) continue;
            outputRetryUs.insert(output->id, now + 1000000); restart = true;
        }
        if (restart) {
            error(QStringLiteral("Audio output stopped; retrying the device"));
            outputs.clear();
            for (auto& source : remote) {
                auto replacement = std::make_shared<RemoteSource>(); replacement->epoch = source->epoch;
                replacement->lastPacketUs = MediaCaptureClock::nowUs();
                source = std::move(replacement);
            }
            ++mixerRevision;
        }
        QList<QByteArray> ids;
        const auto defaultDevice = QMediaDevices::defaultAudioOutput();
        if (defaultOutputId != defaultDevice.id()) {
            if (!defaultOutputId.isEmpty()) {
                // A default-device change can leave the old sink alive for a
                // preview. Stop all callbacks before transferring remote voices,
                // preserving the SPSC queue's single-consumer contract.
                outputs.clear();
                for (auto& source : remote) {
                    auto replacement = std::make_shared<RemoteSource>(); replacement->epoch = source->epoch;
                    replacement->lastPacketUs = MediaCaptureClock::nowUs();
                    source = std::move(replacement);
                }
            }
            defaultOutputId = defaultDevice.id(); ++mixerRevision;
        }
        if (!remote.isEmpty() && !defaultDevice.isNull()) ids.append(defaultDevice.id());
        for (const auto& preview : previews) {
            const auto device = deviceFor(preview->deviceId);
            if (preview->resolvedDeviceId != device.id()) { preview->resolvedDeviceId = device.id(); ++mixerRevision; }
            if (!device.isNull() && !ids.contains(device.id())) ids.append(device.id());
        }
        outputs.erase(std::remove_if(outputs.begin(), outputs.end(), [&ids](const auto& output) {
            return !ids.contains(output->id);
        }), outputs.end());
        for (const auto& id : ids) {
            if (std::any_of(outputs.begin(), outputs.end(), [&id](const auto& output) { return output->id == id; })) continue;
            if (outputRetryUs.value(id) > now) continue;
            const auto device = deviceFor(id);
            auto output = std::make_unique<OutputDevice>();
            output->id = id; output->format = device.preferredFormat(); output->format.setSampleFormat(QAudioFormat::Float);
            QAudioFormat streamFormat;
            streamFormat.setSampleRate(48000); streamFormat.setChannelCount(2); streamFormat.setSampleFormat(QAudioFormat::Float);
            if (device.isFormatSupported(streamFormat)) output->format = streamFormat;
            if (output->format.channelCount() > 8) output->format.setChannelCount(2);
            if (!device.isFormatSupported(output->format) || output->format.sampleRate() < 8000) {
                outputRetryUs.insert(id, now + 1000000);
                error(QStringLiteral("No supported audio output format")); continue;
            }
            output->sink = std::make_unique<QAudioSink>(device, output->format);
            // Qt's callback API bypasses its software ringbuffer; setBufferSize
            // is ignored in this mode. The device determines the callback size.
            auto* pointer = output.get();
            output->sink->start([this, pointer](QSpan<float> samples) { render(*pointer, samples); });
            if (output->sink->error() != QtAudio::NoError) {
                outputRetryUs.insert(id, now + 1000000);
                error(QStringLiteral("Audio output could not start")); continue;
            }
            outputRetryUs.remove(id);
            outputs.push_back(std::move(output));
        }
        publishMixers();
        for (const auto& preview : previews) preview->state()->consumerAvailable.store(
            std::any_of(outputs.begin(), outputs.end(), [&preview](const auto& output) {
                return output->id == preview->resolvedDeviceId;
            }), std::memory_order_release);
    }
    void publishMixers() {
        for (auto& output : outputs) {
            if (output->revision == mixerRevision) { output->collect(); continue; }
            auto snapshot = std::make_unique<MixerSnapshot>();
            if (!remoteMuted && output->id == defaultOutputId)
                for (const auto& source : remote) {
                    // A format change first stops old sinks in ensureOutputs /
                    // rebuildOutputs, so filter preparation never races render.
                    source->renderer.configure(output->format.sampleRate());
                    snapshot->remote.push_back(source);
                }
            for (const auto& preview : previews)
                if (preview->resolvedDeviceId == output->id) snapshot->previews.push_back(preview);
            output->publish(std::move(snapshot)); output->revision = mixerRevision;
        }
    }
    void rebuildOutputs() {
        outputs.clear();
        // Stop old callbacks before assigning a stream to a different device.
        // Recreating its consumer state also discards obsolete hardware time.
        for (auto& source : remote) {
            auto replacement = std::make_shared<RemoteSource>(); replacement->epoch = source->epoch;
            replacement->lastPacketUs = MediaCaptureClock::nowUs();
            source = std::move(replacement);
        }
        ++mixerRevision; ensureOutputs();
    }
    void render(OutputDevice& output, QSpan<float> samples) {
        std::fill(samples.begin(), samples.end(), 0.0f);
        output.rendering.store(true, std::memory_order_seq_cst);
        const auto done = qScopeGuard([&output] {
            output.reader.store(nullptr, std::memory_order_seq_cst);
            output.rendering.store(false, std::memory_order_seq_cst);
        });
        const auto* snapshot = output.snapshot.load(std::memory_order_seq_cst);
        output.reader.store(snapshot, std::memory_order_seq_cst);
        if (!snapshot) return;
        const int rate = output.format.sampleRate(), channels = output.format.channelCount();
        const int frames = int(samples.size()) / channels;
        const auto wall = MediaCaptureClock::nowUs();
        // Public Qt callbacks expose neither the native DAC timestamp nor the
        // device's transport latency. One callback quantum is the explicit
        // presentation estimate; a frame counter filters callback wakeup jitter.
        const auto quantumUs = qint64(frames) * 1000000 / rate;
        const auto observedPresentation = wall + quantumUs;
        if (!output.anchorUs) output.anchorUs = observedPresentation;
        qint64 clock = output.anchorUs + qint64(output.frames * 1000000 / rate);
        if (std::abs(clock - observedPresentation) > 100000) {
            output.anchorUs = observedPresentation; output.frames = 0; clock = observedPresentation;
        } else {
            const auto adjustment = std::clamp<qint64>((observedPresentation - clock) / 128, -100, 100);
            output.anchorUs += adjustment; clock += adjustment;
        }
        output.frames += frames;
        for (const auto& source : snapshot->remote) {
            if (!source->enabled.load(std::memory_order_acquire)) continue;
            const auto clockSample = source->renderer.render(source->blocks, samples.data(), frames, channels, rate, clock);
            if (clockSample.sourceUs >= 0) source->clock.publish(clockSample);
        }
        for (const auto& mapping : snapshot->previews) {
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
        for (auto& sample : samples) if (!std::isfinite(sample)) sample = 0;
        // Stateful libopus soft clipping avoids the harsh discontinuities of
        // hard clipping when several remote sources or previews overlap.
        opus_pcm_soft_clip(samples.data(), frames, channels, output.softClipMemory.data());
    }
    void reportClocks() {
        const auto now = MediaCaptureClock::nowUs();
        const bool feedbackDue = ++reportTicks % 25 == 0;
        for (auto it = remote.begin(); it != remote.end();) {
            if (now - it.value()->lastPacketUs > 5000000) {
                it.value()->enabled.store(false, std::memory_order_release);
                it = remote.erase(it); ++mixerRevision; continue;
            }
            auto& source = *it.value();
            if (feedbackDue) {
                auto feedback = command("feedback"); feedback.insert(QStringLiteral("source"), it.key());
                feedback.insert(QStringLiteral("epoch"), source.epoch); feedback.insert(QStringLiteral("dropped"), source.dropped);
                feedback.insert(QStringLiteral("buffered"), int(source.blocks.size()) * 20);
                feedback.insert(QStringLiteral("concealed"), source.concealed);
                feedback.insert(QStringLiteral("underruns"), source.renderer.takeUnderruns());
                feedback.insert(QStringLiteral("rebuffers"), source.rebuffers + source.renderer.takeRebuffers());
                source.concealed = 0; source.rebuffers = 0;
                AudioWorkerProtocol::send(&socket, feedback); source.dropped = 0;
            }
            const auto clock = source.clock.read();
            if (clock.sourceUs >= 0 && clock.localUs != source.reportedLocalUs && now - source.lastPacketUs < 300000) {
                auto message = command("clock"); message.insert(QStringLiteral("source"), it.key());
                message.insert(QStringLiteral("epoch"), source.epoch); message.insert(QStringLiteral("timestamp"), clock.sourceUs);
                message.insert(QStringLiteral("localUs"), clock.localUs);
                AudioWorkerProtocol::send(&socket, message); source.reportedLocalUs = clock.localUs;
            }
            ++it;
        }
        if (feedbackDue) ensureOutputs();
        else publishMixers();
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
