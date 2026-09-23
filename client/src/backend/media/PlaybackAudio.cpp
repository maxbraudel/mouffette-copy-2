#include "backend/media/PlaybackAudio.h"
#include "backend/media/IndexedMediaDecoder.h"
#include "backend/audiosharing/AudioOutputRouting.h"
#include <opus.h>
#include "backend/audiosharing/MediaCaptureClock.h"
#include <QAudioDevice>
#include <QAudioOutput>
#include <QAudioSink>
#include <QFutureWatcher>
#include <QMediaDevices>
#include <QPointer>
#include <QTimer>
#include <QThreadPool>
#include <QtConcurrent/QtConcurrentRun>
#include <array>
#include <atomic>
#include <vector>

namespace {
qint64 nowUs() { return MediaCaptureClock::nowUs(); }
std::atomic<quint64> pcmBytes{0};
std::atomic<quint64> decodeRequests{0};
static_assert(std::atomic<qint64>::is_always_lock_free && std::atomic<float>::is_always_lock_free
              && std::atomic<void*>::is_always_lock_free, "Audio callback requires lock-free atomics");
constexpr int BlockCount = 4, MaximumVoices = 256;
constexpr int MixBlockFrames = 1024, MaximumChannels = 8;
struct Block {
    // 0 empty, 1 producer owns it, 2 immutable and published to the callback.
    std::atomic<int> state{0};
    std::atomic<quint64> generation{0};
    std::atomic<qint64> startUs{0};
    std::vector<float> samples;
};
struct Voice {
    int rate, channels, blockFrames;
    std::array<Block, BlockCount> blocks;
    std::atomic<quint64> generation{1};
    std::atomic<bool> playing{false};
    std::atomic<bool> presented{false};
    std::atomic<float> gain{1};
    std::atomic<qint64> clockOffsetUs{0};
    Voice(int sampleRate, int channelCount)
        : rate(sampleRate), channels(channelCount), blockFrames(sampleRate / 20) {
        for (auto& block : blocks) block.samples.resize(size_t(blockFrames) * channels);
        pcmBytes.fetch_add(quint64(BlockCount) * blockFrames * channels * sizeof(float));
    }
    ~Voice() { pcmBytes.fetch_sub(quint64(BlockCount) * blockFrames * channels * sizeof(float)); }
};
bool hasPreparedBlock(const Voice& voice, qint64 positionUs) {
    const auto generation = voice.generation.load();
    for (const auto& block : voice.blocks) {
        if (block.state.load(std::memory_order_acquire) == 2 && block.generation == generation
            && block.startUs <= positionUs && block.startUs + 50000 > positionUs) return true;
    }
    return false;
}
class Mixer : public QObject {
public:
    QAudioFormat format;
    AudioStereoOutputMapping routing;
    int mixChannels = 2;
    std::array<float, MaximumChannels> softClip{};
    std::unique_ptr<QAudioSink> sink;
    std::array<std::atomic<Voice*>, MaximumVoices> voices{};
    std::atomic<int> readers{0};
    std::vector<std::shared_ptr<Voice>> retired;
    QTimer retirement;
    qint64 clockAnchorUs = 0;
    quint64 presentedFrames = 0;
    explicit Mixer(const QAudioDevice& device) {
        auto native = device.preferredFormat();
        native.setSampleFormat(QAudioFormat::Float);
        // Local playback keeps a supported surround device's full layout.
        if (native.channelCount() > 2 && native.channelCount() <= MaximumChannels
            && device.isFormatSupported(native)) format = native;
        else format = chooseStereoOutputFormat(device);
        if (!format.isValid() || format.channelCount() > MaximumChannels
            || format.sampleRate() < 8000) return;
        mixChannels = std::max(2, format.channelCount());
        if (format.channelCount() <= 2) {
            routing = AudioStereoOutputMapping(format);
            if (!routing.isValid()) return;
        }
        sink = std::make_unique<QAudioSink>(device, format);
        sink->start([this](QSpan<float> output) {
            readers.fetch_add(1);
            std::fill(output.begin(), output.end(), 0.0f);
            const qint64 wall = nowUs();
            if (!clockAnchorUs) clockAnchorUs = wall;
            const int channels = format.channelCount(), rate = format.sampleRate();
            const int frames = int(output.size()) / channels;
            qint64 clock = clockAnchorUs + qint64(presentedFrames * 1000000 / rate);
            // Recover the sample-clock epoch after device suspension or a long
            // callback interruption; small drift still uses gradual correction.
            if (std::abs(wall - clock) > 200000) {
                clockAnchorUs = wall; presentedFrames = 0; clock = wall;
            }
            // Continuous device sample clock. Correct sustained drift gradually,
            // without following callback jitter or moving the scene/video clock.
            if (std::abs(wall - clock) > 2000) clockAnchorUs += wall > clock ? 1 : -1;
            presentedFrames += frames;
            for (int offset = 0; offset < frames; offset += MixBlockFrames) {
                const int chunkFrames = std::min(MixBlockFrames, frames - offset);
                const qint64 chunkClock = clock + qint64(offset) * 1000000 / rate;
                auto chunk = output.subspan(offset * channels, chunkFrames * channels);
                std::array<float, MixBlockFrames * MaximumChannels> mixed{};
                for (auto& slot : voices) {
                    Voice* voice = slot.load();
                    if (!voice) continue;
                    const quint64 generation = voice->generation.load(std::memory_order_acquire);
                    const bool playing = voice->playing.load(std::memory_order_acquire);
                    const float gain = voice->gain.load(std::memory_order_relaxed);
                    const qint64 target = chunkClock + voice->clockOffsetUs.load(std::memory_order_relaxed);
                    for (auto& block : voice->blocks) {
                        if (block.state.load(std::memory_order_acquire) != 2) continue;
                        if (block.generation != generation) { block.state.store(0, std::memory_order_release); continue; }
                        if (!playing) continue;
                        const qint64 start = qRound64((block.startUs - target) * (double(rate) / 1000000.0));
                        const qint64 first = std::max<qint64>(0, start);
                        const qint64 last = std::min<qint64>(chunkFrames, start + voice->blockFrames);
                        if (last > first) voice->presented.store(true, std::memory_order_relaxed);
                        for (qint64 frame = first; frame < last; ++frame) {
                            const qint64 source = (frame-start) * mixChannels, destination = frame * mixChannels;
                            for (int channel = 0; channel < mixChannels; ++channel) {
                                const auto value = block.samples[size_t(source+channel)] * gain;
                                mixed[size_t(destination+channel)] += value;
                            }
                        }
                        if (start + voice->blockFrames <= chunkFrames) block.state.store(0, std::memory_order_release);
                    }
                }
                for (int sample = 0; sample < chunkFrames * mixChannels; ++sample)
                    if (!std::isfinite(mixed[size_t(sample)])) mixed[size_t(sample)] = 0;
                opus_pcm_soft_clip(mixed.data(), chunkFrames, mixChannels, softClip.data());
                if (channels > 2) std::copy_n(mixed.data(), chunkFrames * channels, chunk.data());
                else for (int frame = 0; frame < chunkFrames; ++frame)
                    routing.add(chunk.data() + frame * channels, mixed[size_t(frame * 2)], mixed[size_t(frame * 2 + 1)]);
            }
            readers.fetch_sub(1);
        });
        if (sink->error() != QtAudio::NoError) { sink.reset(); return; }
        retirement.setInterval(20);
        connect(&retirement, &QTimer::timeout, this, [this] { if (!readers.load()) retired.clear(); });
        retirement.start();
    }
    ~Mixer() override { if (sink) sink->stop(); }
    int add(const std::shared_ptr<Voice>& voice) {
        for (int i = 0; i < MaximumVoices; ++i) if (!voices[i].load()) { voices[i].store(voice.get(), std::memory_order_release); return i; }
        return -1;
    }
    void remove(int slot, std::shared_ptr<Voice> voice) {
        if (slot >= 0) voices[slot].store(nullptr);
        retired.push_back(std::move(voice));
        if (!readers.load()) retired.clear();
    }
};
QHash<QByteArray, std::weak_ptr<Mixer>>& devices() { static QHash<QByteArray, std::weak_ptr<Mixer>> map; return map; }
QMediaDevices& audioDevices() { static QMediaDevices devices; return devices; }
QThreadPool& audioWorkers() { static QThreadPool pool; static bool initialized = [] { pool.setMaxThreadCount(2); pool.setExpiryTimeout(1000); return true; }(); Q_UNUSED(initialized); return pool; }
}
struct PlaybackAudio::Impl {
    std::shared_ptr<const ResidentMediaAsset> asset;
    QPointer<QAudioOutput> output;
    std::shared_ptr<Mixer> mixer;
    std::shared_ptr<Voice> voice;
    int slot = -1;
    qint64 nextUs = 0, requestedUs = 0;
    bool busy = false;
    bool requestedValid = false;
    bool seeking = false;
    QTimer timer;
    void detach() {
        if (voice) voice->playing.store(false, std::memory_order_release);
        if (mixer && voice) mixer->remove(slot, std::move(voice));
        voice.reset(); mixer.reset(); slot = -1;
    }
};
PlaybackAudio::PlaybackAudio(QObject* parent) : QObject(parent), d(std::make_unique<Impl>()) {
    d->timer.setInterval(10);
    connect(&audioDevices(), &QMediaDevices::audioOutputsChanged, this, &PlaybackAudio::rebuildOutput);
    connect(&d->timer, &QTimer::timeout, this, &PlaybackAudio::refill);
}
PlaybackAudio::~PlaybackAudio() { d->timer.stop(); d->detach(); }
void PlaybackAudio::setAsset(std::shared_ptr<const ResidentMediaAsset> asset) {
    if (d->asset == asset) return;
    pause(); d->detach(); d->asset = std::move(asset); rebuildOutput();
}
void PlaybackAudio::setOutput(QAudioOutput* output) {
    if (d->output == output) return;
    if (d->output) disconnect(d->output, nullptr, this, nullptr);
    d->output = output;
    if (output) {
        connect(output, &QAudioOutput::deviceChanged, this, &PlaybackAudio::rebuildOutput);
        auto gain = [this] { if (d->voice && d->output) d->voice->gain.store(d->output->isMuted() ? 0.0f : float(d->output->volume())); };
        connect(output, &QAudioOutput::volumeChanged, this, gain);
        connect(output, &QAudioOutput::mutedChanged, this, gain);
        connect(output, &QObject::destroyed, this, [this] { pause(); d->timer.stop(); d->detach(); });
    }
    rebuildOutput();
}
void PlaybackAudio::rebuildOutput() {
    d->timer.stop();
    const bool playing = d->voice && d->voice->playing.load();
    const qint64 position = playing ? nowUs() + d->voice->clockOffsetUs.load() : d->requestedUs;
    d->detach(); d->requestedValid = false;
    if (!d->asset || !d->asset->audioPackets.codec || !d->output) return;
    QAudioDevice device = d->output->device();
    if (device.isNull() || !QMediaDevices::audioOutputs().contains(device)) device = QMediaDevices::defaultAudioOutput();
    if (device.isNull()) return; // Video-only/headless preparation stays usable.
    auto& shared = devices()[device.id()];
    d->mixer = shared.lock();
    if (!d->mixer) { d->mixer = std::make_shared<Mixer>(device); shared = d->mixer; }
    if (!d->mixer->sink) { d->mixer.reset(); emit failed(QStringLiteral("No supported floating-point audio output format")); return; }
    d->voice = std::make_shared<Voice>(d->mixer->format.sampleRate(), d->mixer->mixChannels);
    d->slot = d->mixer->add(d->voice);
    if (d->slot < 0) { d->detach(); emit failed(QStringLiteral("Too many simultaneous audio cursors")); return; }
    d->voice->gain.store(d->output->isMuted() ? 0.0f : float(d->output->volume()));
    d->timer.start(); prepare(position);
    if (playing) play(position);
}
void PlaybackAudio::prepare(qint64 positionUs) {
    if (!d->voice) { d->requestedUs = positionUs; return; }
    if (d->requestedValid && d->requestedUs == positionUs && d->seeking) { refill(); return; }
    d->requestedUs = positionUs; d->requestedValid = true;
    if (hasPreparedBlock(*d->voice, positionUs)) { d->seeking = false; return; }
    d->seeking = true;
    d->timer.start();
    d->voice->generation.fetch_add(1, std::memory_order_release);
    d->nextUs = positionUs;
    // Stale published blocks are retired only by the callback, which owns reads.
    refill();
}
bool PlaybackAudio::preparedAt(qint64 positionUs) const {
    if (!d->voice) return true;
    return hasPreparedBlock(*d->voice, positionUs);
}
void PlaybackAudio::play(qint64 positionUs) {
    prepare(positionUs);
    if (!d->voice) return;
    d->timer.start();
    d->voice->presented.store(false, std::memory_order_relaxed);
    d->voice->clockOffsetUs.store(positionUs - nowUs(), std::memory_order_relaxed);
    d->voice->playing.store(true, std::memory_order_release);
}
bool PlaybackAudio::presentedSincePlay() const {
    return !d->voice || d->voice->presented.load(std::memory_order_relaxed);
}
void PlaybackAudio::pause() {
    if (d->voice) d->voice->playing.store(false, std::memory_order_release);
}
void PlaybackAudio::refill() {
    if (!d->voice || d->busy) return;
    auto voice = d->voice;
    const auto generation = voice->generation.load();
    std::vector<int> empty;
    for (int i = 0; i < BlockCount; ++i) {
        int expected = 0;
        if (voice->blocks[i].state.compare_exchange_strong(expected, 1, std::memory_order_acq_rel)) empty.push_back(i);
    }
    if (empty.empty()) {
        if (!voice->playing.load() && !d->seeking) d->timer.stop();
        return;
    }
    // prepare() runs before play() installs the new clock offset. A seek must
    // start at its requested position, even when the previous transport is
    // still playing; catching up to that old clock would skip a backward seek.
    if (voice->playing.load() && !d->seeking)
        d->nextUs = std::max(d->nextUs, nowUs() + voice->clockOffsetUs.load());
    qint64 start = d->nextUs;
    d->nextUs += qint64(empty.size()) * 50000;
    if (start >= 0 && start + qint64(empty.size()) * 50000 <= 200000
        && voice->rate == 48000 && voice->channels == 2 && !d->asset->firstAudioPcm.isEmpty()) {
        for (size_t i = 0; i < empty.size(); ++i) {
            auto& block = voice->blocks[empty[i]];
            const qint64 offset = (start * 48 / 1000 + qint64(i) * voice->blockFrames) * 2 * sizeof(float);
            memcpy(block.samples.data(), d->asset->firstAudioPcm.constData() + offset, block.samples.size() * sizeof(float));
            block.startUs = start + qint64(i) * 50000; block.generation = generation;
            block.state.store(2, std::memory_order_release);
        }
        d->seeking = false;
        emit prepared(); return;
    }
    d->busy = true;
    decodeRequests.fetch_add(1, std::memory_order_relaxed);
    auto* watcher = new QFutureWatcher<QString>(this);
    connect(watcher, &QFutureWatcher<QString>::finished, this, [this, watcher, voice, generation] {
        QString error = watcher->result(); watcher->deleteLater(); d->busy = false;
        if (d->voice == voice && voice->generation.load() == generation) {
            if (!error.isEmpty()) emit failed(error);
            else { d->seeking = false; emit prepared(); }
        }
        refill();
    });
    watcher->setFuture(QtConcurrent::run(&audioWorkers(), [asset = d->asset, voice, generation, empty, start] {
        IndexedMediaDecoder decoder;
        QString error;
        QByteArray pcm = decoder.audio(*asset, start, int(empty.size()) * voice->blockFrames, voice->rate, voice->channels, error);
        for (size_t i = 0; i < empty.size(); ++i) {
            auto& block = voice->blocks[empty[i]];
            if (voice->generation.load(std::memory_order_acquire) != generation || pcm.isEmpty()) { block.state.store(0, std::memory_order_release); continue; }
            memcpy(block.samples.data(), pcm.constData() + i * block.samples.size() * sizeof(float), block.samples.size() * sizeof(float));
            block.startUs = start + qint64(i) * 50000; block.generation = generation;
            block.state.store(2, std::memory_order_release);
        }
        return error;
    }));
}
quint64 PlaybackAudio::bufferBytes() { return pcmBytes.load(); }
quint64 PlaybackAudio::decodeRequestCount() { return decodeRequests.load(std::memory_order_relaxed); }
int PlaybackAudio::deviceCount() { int count = 0; for (const auto& weak : devices()) if (!weak.expired()) ++count; return count; }
