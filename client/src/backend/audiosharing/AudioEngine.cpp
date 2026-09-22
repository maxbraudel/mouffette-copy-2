#include "AudioEngine.h"
#include "AudioCaptureMix.h"
#include "AudioCaptureResampler.h"
#include "AudioOutputRouting.h"
#include "AudioOutputTiming.h"
#include "SceneAudioTap.h"
#include "backend/audiosharing/AudioStreamCodec.h"
#include "backend/audiosharing/AudioPlaybackTimeline.h"
#include "backend/audiosharing/AudioPlaybackBuffer.h"
#include "backend/audiosharing/MediaCaptureClock.h"
#include "backend/audiosharing/SystemAudioCapture.h"
#include <QAudioDevice>
#include <QAudioSink>
#include <QCoreApplication>
#include <QHash>
#include <QLoggingCategory>
#include <QMediaDevices>
#include <QMutex>
#include <QMutexLocker>
#include <QPointer>
#include <QScopeGuard>
#include <QTimer>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <deque>
#include <vector>
#include <opus.h>

Q_LOGGING_CATEGORY(audioEngineLog, "mouffette.audio.engine", QtWarningMsg)

namespace {
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
    bool received = false, discontinuityPending = false;
    qint64 lastTimestampUs = -1, lastPacketUs = 0, reportedLocalUs = -1;
    int dropped = 0, concealed = 0, rebuffers = 0;
};
struct MixerSnapshot {
    std::vector<std::shared_ptr<RemoteSource>> remote;
};
struct OutputDevice {
    QByteArray id;
    QAudioFormat format;
    AudioStereoOutputMapping routing;
    std::unique_ptr<QAudioSink> sink;
    std::atomic<const MixerSnapshot*> snapshot{nullptr};
    std::atomic<bool> rendering{false};
    std::atomic<const MixerSnapshot*> reader{nullptr};
    std::unique_ptr<MixerSnapshot> current;
    std::vector<std::unique_ptr<MixerSnapshot>> retired;
    quint64 revision = 0;
    qint64 anchorUs = 0;
    quint64 frames = 0;
    std::atomic<qint64> maximumQuantumUs{20000};
    std::array<float, 2> softClipMemory{};
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
// Native threads only write this bounded mailbox. The application thread owns
// the codec and stream state; native callbacks never wait on network I/O.
struct CaptureMailbox {
    QMutex mutex;
    std::deque<AudioBlock> blocks;
    qsizetype bytes = 0;
    bool queued = false, closed = false;
};
}
struct AudioEngine::Private : public QObject {
    AudioEngine* owner;
    CaptureFactory factory;
    bool closing = false, capturing = false;
    QTimer captureTimer;
    AudioCaptureMix captureMix;
    AudioCaptureResampler nativeSamples;
    QMediaDevices devices;
    QTimer reports;
    std::unique_ptr<SystemAudioCapture> capture;
    std::shared_ptr<CaptureMailbox> captureMailbox;
    AudioStreamEncoder encoder;
    QString captureEpoch, captureSequenceEpoch;
    quint64 captureSequence = 0;
    bool captureFailed = false;
    int bitrate = 96000;
    QHash<QString, std::shared_ptr<RemoteSource>> remote;
    QHash<QString, QPair<QString, qint64>> playbackErrors;
    std::vector<std::unique_ptr<OutputDevice>> outputs;
    bool remoteMuted = false;
    QByteArray defaultOutputId;
    int reportTicks = 0;
    quint64 mixerRevision = 1;
    QHash<QByteArray, qint64> outputRetryUs;

    Private(AudioEngine* parent, CaptureFactory sourceFactory) : QObject(parent), owner(parent), factory(std::move(sourceFactory)) {
        connect(&devices, &QMediaDevices::audioOutputsChanged, this, [this] { rebuildOutputs(); });
        reports.setInterval(20);
        connect(&reports, &QTimer::timeout, this, [this] { reportClocks(); });
        reports.start();
        captureTimer.setTimerType(Qt::PreciseTimer);
        captureTimer.setInterval(10);
        connect(&captureTimer, &QTimer::timeout, this, [this] { publishCapture(); });
    }
    ~Private() override { stopCapture(); outputs.clear(); }
    void error(const QString& value, const QString& sourceId, const QString& epoch) {
        const auto key = epoch + QLatin1Char('\n') + value;
        const auto now = MediaCaptureClock::nowUs();
        const auto previous = playbackErrors.constFind(sourceId);
        if (previous != playbackErrors.cend() && previous->first == key && now - previous->second < 1000000) return;
        if (previous == playbackErrors.cend() && playbackErrors.size() >= 64)
            playbackErrors.erase(playbackErrors.begin());
        playbackErrors.insert(sourceId, {key, now});
        QTimer::singleShot(0, this, [this, sourceId, epoch, value] {
            if (!closing) emit owner->playbackFailed(sourceId, epoch, value);
        });
    }
    void outputError(const QString& value, const QByteArray& outputId) {
        // Playback failures stay scoped to the remote source and stream epoch.
        if (outputId != QMediaDevices::defaultAudioOutput().id()) return;
        for (auto it = remote.cbegin(); it != remote.cend(); ++it)
            error(value, it.key(), it.value()->epoch);
    }
    void state(bool active, const QString& value = {}) {
        emit owner->captureStateChanged(active, value);
    }
    void stopCapture() {
        captureTimer.stop(); capturing = false;
        if (captureMailbox) SceneAudioBus::instance().setEnabled(false);
        captureMix.clear(); nativeSamples.reset();
        if (captureMailbox) { QMutexLocker lock(&captureMailbox->mutex); captureMailbox->closed = true; captureMailbox->blocks.clear(); }
        captureMailbox.reset();
        if (capture) capture->stop();
        capture.reset(); captureEpoch.clear();
    }
    void startCapture(const QString& epoch, int requestedBitrate) {
        if (epoch.isEmpty() || closing) return;
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
        capture = factory ? factory() : createSystemAudioCapture();
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
                if (captureMailbox != mailbox) return;
                captureFailed = !active;
                if (active && !capturing) {
                    captureMix.reset(MediaCaptureClock::nowUs());
                    SceneAudioBus::instance().setEnabled(true);
                    capturing = true; captureTimer.start();
                } else if (!active) {
                    capturing = false; captureTimer.stop(); captureMix.clear();
                    SceneAudioBus::instance().setEnabled(false);
                }
                state(active, failure);
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
        if (!capturing) return;
        for (const auto& block : blocks) {
            const auto age = MediaCaptureClock::nowUs() - block.timestampUs;
            if (age > 100000 || age < -250000) continue;
            auto samples = nativeSamples.append(block.pcm, block.timestampUs, block.discontinuity);
            const auto failure = nativeSamples.errorString();
            if (!failure.isEmpty()) { stopCapture(); state(false, failure); return; }
            if (samples) captureMix.appendSystem(reinterpret_cast<const float*>(samples->pcm.constData()),
                int(samples->pcm.size() / (2 * sizeof(float))), samples->timestampUs, samples->discontinuity);
        }
    }
    void publishCapture() {
        if (!capturing || captureEpoch.isEmpty()) return;
        const auto activeEpoch = captureEpoch;
        // Drain native work before the deadline timer: Qt may deliver a timer
        // before an already queued native notification after a GUI stall.
        drainCapture(captureMailbox);
        if (!capturing || captureEpoch != activeEpoch) return;
        for (const auto& tap : SceneAudioBus::instance().sources()) {
            QByteArray pcm; qint64 timestamp = 0; bool transition = false; QString failure;
            while (tap->takeForCapture(pcm, timestamp, transition, failure))
                captureMix.appendScene(reinterpret_cast<const float*>(pcm.constData()),
                    int(pcm.size() / (2 * sizeof(float))), timestamp, transition);
            if (!failure.isEmpty()) { stopCapture(); state(false, failure); return; }
        }
        const auto now = MediaCaptureClock::nowUs();
        while (auto chunk = captureMix.take(now)) {
            // Codec prediction stays continuous within a wire epoch. A local
            // backlog drop is a PCM fade/gap, not an unannounced encoder reset.
            QString failure;
            const auto packet = encoder.encode(reinterpret_cast<const float*>(chunk->pcm.constData()), failure);
            if (!failure.isEmpty()) { stopCapture(); state(false, failure); return; }
            emit owner->packetReady(activeEpoch, ++captureSequence,
                std::max<qint64>(0, chunk->timestampUs - encoder.lookaheadUs()), packet);
            // Consumers may stop sharing synchronously from the signal.
            if (!capturing || captureEpoch != activeEpoch) return;
        }
    }
    void receivePacket(const QString& sourceId, const QString& epoch, quint64 sequence,
                       qint64 timestamp, const QByteArray& packet, qint64 presentation) {
        if (closing || sourceId.isEmpty() || sourceId.size() > 256 || epoch.isEmpty() || epoch.size() > 256
            || timestamp < 0 || remoteMuted) return;
        if (!remote.contains(sourceId) && remote.size() >= 64) return;
        auto& source = remote[sourceId];
        if (!source || source->epoch != epoch) {
            if (source) source->enabled.store(false, std::memory_order_release);
            source = std::make_shared<RemoteSource>(); source->epoch = epoch;
            source->lastPacketUs = MediaCaptureClock::nowUs();
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
        source->timeline.setOutputQuantumUs(owner->outputQuantumUs());
        auto queuePcm = [&](const QByteArray& pcm, qint64 time, qint64 deadline) {
            const auto now = MediaCaptureClock::nowUs();
            const bool full = source->blocks.size() >= AudioPlaybackBuffer::Capacity;
            const auto decision = source->timeline.enqueue(time, now, full, deadline);
            if (decision.rebuffer && source->received) ++source->rebuffers;
            if (full) {
                source->blocks.requestDiscardQueued();
                source->discontinuityPending = true;
                ++source->dropped; return;
            }
            if (!decision.accept) { ++source->dropped; return; }
            const bool transition = source->discontinuityPending || decision.rebuffer;
            if (source->blocks.push(reinterpret_cast<const float*>(pcm.constData()),
                    time, source->timeline.presentationAt(time), transition))
                source->discontinuityPending = false;
            else { source->discontinuityPending = true; ++source->dropped; }
        };
        if (source->received && (elapsed > 140000 || std::abs(elapsed - (missing + 1) * 20000) > 2000)) {
            source->decoder.reset(); source->discontinuityPending = true;
        }
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
        if (pcm.isEmpty()) {
            error(QStringLiteral("Remote system audio could not be decoded: %1").arg(failure), sourceId, epoch);
            return;
        }
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
            outputError(QStringLiteral("Audio output stopped; retrying the device"), output->id);
            outputRetryUs.insert(output->id, now + 1000000); restart = true;
        }
        if (restart) {
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
        if (!remote.isEmpty() && defaultDevice.isNull())
            outputError(QStringLiteral("No audio output device is available on this device."), defaultDevice.id());
        if (defaultOutputId != defaultDevice.id()) {
            if (!defaultOutputId.isEmpty()) {
                // Stop all callbacks before transferring remote voices,
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
        outputs.erase(std::remove_if(outputs.begin(), outputs.end(), [&ids](const auto& output) {
            return !ids.contains(output->id);
        }), outputs.end());
        for (const auto& id : ids) {
            if (std::any_of(outputs.begin(), outputs.end(), [&id](const auto& output) { return output->id == id; })) continue;
            if (outputRetryUs.value(id) > now) continue;
            const auto device = deviceFor(id);
            auto output = std::make_unique<OutputDevice>();
            output->id = id; output->format = chooseStereoOutputFormat(device);
            output->routing = AudioStereoOutputMapping(output->format);
            if (!output->format.isValid() || !output->routing.isValid() || output->format.sampleRate() < 8000) {
                outputRetryUs.insert(id, now + 1000000);
                outputError(QStringLiteral("No supported audio output format"), id); continue;
            }
            output->sink = std::make_unique<QAudioSink>(device, output->format);
            // Qt's callback API bypasses its software ringbuffer; setBufferSize
            // is ignored in this mode. The device determines the callback size.
            auto* pointer = output.get();
            output->sink->start([this, pointer](QSpan<float> samples) { render(*pointer, samples); });
            if (output->sink->error() != QtAudio::NoError) {
                outputRetryUs.insert(id, now + 1000000);
                outputError(QStringLiteral("Audio output could not start"), id); continue;
            }
            outputRetryUs.remove(id);
            qCDebug(audioEngineLog) << "output opened rate=" << output->format.sampleRate()
                << "device_channels=" << output->format.channelCount() << "shared_channels=2";
            outputs.push_back(std::move(output));
        }
        publishMixers();
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
        const int rate = output.format.sampleRate(), channels = output.format.channelCount();
        const int frames = int(samples.size()) / channels;
        const auto quantumUs = qint64(frames) * 1000000 / rate;
        const auto measuredQuantum = AudioOutputTiming::normalizeQuantumUs(quantumUs);
        if (measuredQuantum > output.maximumQuantumUs.load(std::memory_order_relaxed))
            output.maximumQuantumUs.store(measuredQuantum, std::memory_order_release);
        if (!snapshot) return;
        const auto wall = MediaCaptureClock::nowUs();
        // Public Qt callbacks expose neither the native DAC timestamp nor the
        // device's transport latency. One callback quantum is the explicit
        // presentation estimate; a frame counter filters callback wakeup jitter.
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
        // Mix stereo independently of the device layout. Scratch storage is
        // fixed; mono downmix/speaker routing happens only at the final write.
        for (int offset = 0; offset < frames; offset += 1024) {
            const int count = std::min(1024, frames - offset);
            std::array<float, 1024 * 2> stereo{};
            const auto chunkClock = clock + qint64(offset) * 1000000 / rate;
            for (const auto& source : snapshot->remote) {
                if (!source->enabled.load(std::memory_order_acquire)) continue;
                const auto clockSample = source->renderer.render(source->blocks, stereo.data(), count, 2, rate, chunkClock, quantumUs);
                if (clockSample.sourceUs >= 0) source->clock.publish(clockSample);
            }
            for (auto& sample : stereo) if (!std::isfinite(sample)) sample = 0;
            opus_pcm_soft_clip(stereo.data(), count, 2, output.softClipMemory.data());
            for (int frame = 0; frame < count; ++frame)
                output.routing.add(samples.data() + (offset + frame) * channels,
                    stereo[size_t(frame * 2)], stereo[size_t(frame * 2 + 1)]);
        }
    }
    void reportClocks() {
        const auto now = MediaCaptureClock::nowUs();
        const bool feedbackDue = ++reportTicks % 25 == 0;
        for (auto it = remote.begin(); it != remote.end();) {
            if (now - it.value()->lastPacketUs > 5000000) {
                it.value()->enabled.store(false, std::memory_order_release);
                playbackErrors.remove(it.key());
                it = remote.erase(it); ++mixerRevision; continue;
            }
            auto& source = *it.value();
            if (feedbackDue) {
                const auto sourceId = it.key(), epoch = source.epoch;
                const int dropped = source.dropped, buffered = int(source.blocks.size()) * 20;
                QTimer::singleShot(0, this, [this, sourceId, epoch, dropped, buffered] {
                    const auto current = remote.value(sourceId);
                    if (!closing && current && current->epoch == epoch)
                        emit owner->playbackFeedback(sourceId, epoch, dropped, buffered);
                });
                const int underruns = source.renderer.takeUnderruns(), recoveries = source.renderer.takeRebuffers();
                if (underruns || recoveries || dropped)
                    qCDebug(audioEngineLog) << "playout interval_ms=500 underruns=" << underruns
                        << "recoveries=" << recoveries << "dropped=" << dropped
                        << "concealed=" << source.concealed << "buffered_ms=" << buffered
                        << "output_quantum_us=" << owner->outputQuantumUs();
                source.concealed = 0; source.rebuffers = 0; source.dropped = 0;
            }
            const auto clock = source.clock.read();
            if (clock.sourceUs >= 0 && clock.localUs != source.reportedLocalUs && now - source.lastPacketUs < 300000) {
                const auto sourceId = it.key(), epoch = source.epoch;
                QTimer::singleShot(0, this, [this, sourceId, epoch, clock] {
                    const auto current = remote.value(sourceId);
                    if (!closing && current && current->epoch == epoch)
                        emit owner->playbackClock(sourceId, epoch, clock.sourceUs, clock.localUs);
                });
                source.reportedLocalUs = clock.localUs;
            }
            ++it;
        }
        if (feedbackDue) ensureOutputs();
        else publishMixers();
    }
};
namespace { QPointer<AudioEngine> singleton; }
AudioEngine* AudioEngine::instance() {
    if (!singleton) singleton = new AudioEngine(QCoreApplication::instance());
    return singleton;
}
AudioEngine::AudioEngine(QObject* parent, CaptureFactory factory)
    : QObject(parent), d(std::make_unique<Private>(this, std::move(factory))) {}
AudioEngine::~AudioEngine() = default;
void AudioEngine::startCapture(const QString& epoch, int bitrate) { d->startCapture(epoch, bitrate); }
void AudioEngine::setCaptureBitrate(int bitrate) { d->encoder.setBitrate(bitrate); }
void AudioEngine::stopCapture() { d->stopCapture(); }
qint64 AudioEngine::outputQuantumUs() const {
    qint64 quantum = 20000;
    for (const auto& output : d->outputs)
        quantum = std::max(quantum, output->maximumQuantumUs.load(std::memory_order_acquire));
    return quantum;
}
void AudioEngine::playPacket(const QString& source, const QString& epoch, quint64 sequence,
                            qint64 timestamp, const QByteArray& opus, qint64 presentation) {
    d->receivePacket(source, epoch, sequence, timestamp, opus, presentation);
}
void AudioEngine::resetPlayback(const QString& sourceId) {
    const auto source = d->remote.take(sourceId);
    if (source) source->enabled.store(false, std::memory_order_release);
    ++d->mixerRevision; d->ensureOutputs();
}
void AudioEngine::setPlaybackMuted(bool muted) {
    if (d->remoteMuted == muted) return;
    d->remoteMuted = muted;
    for (const auto& source : d->remote) source->enabled.store(false, std::memory_order_release);
    d->remote.clear(); d->playbackErrors.clear(); ++d->mixerRevision; d->ensureOutputs();
}
void AudioEngine::shutdown() {
    d->closing = true; d->reports.stop(); d->stopCapture();
    d->outputs.clear(); d->remote.clear();
}
