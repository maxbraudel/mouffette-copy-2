#include "backend/media/ResidentVideoPlayer.h"

#include <QAbstractVideoBuffer>
#include <QAudioDevice>
#include <QAudioOutput>
#include <QAudioSink>
#include <QIODevice>
#include <QMediaDevices>
#include <QVariant>
#include <QVideoSink>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <limits>

namespace {
class PresentationFrameBuffer final : public QAbstractVideoBuffer {
public:
    explicit PresentationFrameBuffer(QVideoFrame source) : m_source(std::move(source)) {}
    ~PresentationFrameBuffer() override { unmap(); }
    QVideoFrameFormat format() const override { return m_source.surfaceFormat(); }
    MapData map(QVideoFrame::MapMode mode) override {
        if (mode != QVideoFrame::ReadOnly || !m_source.map(QVideoFrame::ReadOnly)) return {};
        m_mapped = true;
        MapData data;
        data.planeCount = m_source.planeCount();
        for (int plane = 0; plane < data.planeCount; ++plane) {
            data.data[plane] = m_source.bits(plane);
            data.bytesPerLine[plane] = m_source.bytesPerLine(plane);
            data.dataSize[plane] = m_source.mappedBytes(plane);
        }
        return data;
    }
    void unmap() override {
        if (m_mapped) { m_source.unmap(); m_mapped = false; }
    }
private:
    QVideoFrame m_source;
    bool m_mapped = false;
};
} // namespace

// Pull-only PCM presentation. Resampling/channel conversion here operates on
// already decoded float samples; this class never opens a file or a codec.
class ResidentPcmDevice final : public QIODevice
{
public:
    ResidentPcmDevice(std::shared_ptr<const ResidentMediaAsset> asset,
                      QAudioFormat outputFormat, qint64 positionUs)
        : m_asset(std::move(asset)), m_outputFormat(outputFormat), m_startUs(positionUs) {
        open(QIODevice::ReadOnly);
    }
    bool isSequential() const override { return true; }
    qint64 bytesAvailable() const override {
        const qint64 frames = std::max<qint64>(0, (m_asset->durationUs - m_startUs)
            * m_outputFormat.sampleRate() / 1000000 - m_outputFrames);
        return frames * m_outputFormat.bytesPerFrame() + QIODevice::bytesAvailable();
    }
protected:
    qint64 readData(char* data, qint64 maxSize) override {
        const int frameBytes = m_outputFormat.bytesPerFrame();
        if (!frameBytes) return 0;
        const qint64 frames = std::min(maxSize / frameBytes, bytesAvailable() / frameBytes);
        const int channels = m_outputFormat.channelCount();
        const int sourceChannels = m_asset->audioFormat.channelCount();
        const double sourceRate = m_asset->audioFormat.sampleRate();
        const int sampleBytes = m_outputFormat.bytesPerSample();
        for (qint64 f = 0; f < frames; ++f) {
            const double timeUs = m_startUs + (m_outputFrames + f) * 1000000.0 / m_outputFormat.sampleRate();
            while (m_chunkIndex < m_asset->audio.size()) {
                const auto& chunk = m_asset->audio[m_chunkIndex];
                if (timeUs < chunk.timestampUs + chunk.sampleFrames * 1000000.0 / sourceRate) break;
                ++m_chunkIndex;
            }
            for (int channel = 0; channel < channels; ++channel) {
                float value = 0;
                if (m_chunkIndex < m_asset->audio.size()) {
                    const auto& chunk = m_asset->audio[m_chunkIndex];
                    if (timeUs >= chunk.timestampUs) {
                        const double position = (timeUs - chunk.timestampUs) * sourceRate / 1000000.0;
                        const qint64 index = std::clamp<qint64>(qint64(position), 0, chunk.sampleFrames - 1);
                        const float fraction = float(position - std::floor(position));
                        auto sample = [&](qint64 frame, int ch) -> float {
                            const ResidentAudioChunk* current = &chunk;
                            if (frame >= current->sampleFrames) {
                                if (m_chunkIndex + 1 < m_asset->audio.size()) {
                                    const auto& next = m_asset->audio[m_chunkIndex + 1];
                                    const double endUs = chunk.timestampUs + chunk.sampleFrames * 1000000.0 / sourceRate;
                                    if (std::abs(next.timestampUs - endUs) < 2000) { current = &next; frame = 0; }
                                    else frame = chunk.sampleFrames - 1;
                                } else frame = chunk.sampleFrames - 1;
                            }
                            float sampleValue = 0;
                            const char* bytes = current->pcm.constData()
                                + (frame * sourceChannels + ch) * sizeof(float);
                            std::memcpy(&sampleValue, bytes, sizeof(sampleValue));
                            return std::isfinite(sampleValue) ? sampleValue : 0;
                        };
                        auto interpolated = [&](int ch) {
                            return sample(index, ch) * (1 - fraction) + sample(index + 1, ch) * fraction;
                        };
                        if (channels == sourceChannels) value = interpolated(channel);
                        else if (sourceChannels == 1) value = interpolated(0);
                        else if (channels == 1) {
                            for (int ch = 0; ch < sourceChannels; ++ch) value += interpolated(ch) / sourceChannels;
                        } else {
                            value = channel < sourceChannels ? interpolated(channel) : 0;
                            // A stereo-only output retains center/surround data.
                            if (channels == 2 && sourceChannels > 2) {
                                float extra = 0;
                                for (int ch = 2; ch < sourceChannels; ++ch) extra += interpolated(ch);
                                value = (value + extra / (sourceChannels - 2)) * 0.5f;
                            }
                        }
                    }
                }
                char* destination = data + f * frameBytes + channel * sampleBytes;
                value = std::clamp(value, -1.0f, 1.0f);
                switch (m_outputFormat.sampleFormat()) {
                case QAudioFormat::Float: std::memcpy(destination, &value, sizeof(value)); break;
                case QAudioFormat::Int16: {
                    const qint16 converted = qint16(std::lround(value * 32767.0f));
                    std::memcpy(destination, &converted, sizeof(converted)); break;
                }
                case QAudioFormat::Int32: {
                    const qint32 converted = qint32(std::llround(double(value) * 2147483647.0));
                    std::memcpy(destination, &converted, sizeof(converted)); break;
                }
                case QAudioFormat::UInt8: *destination = char(std::clamp(int(std::lround((value + 1) * 127.5f)), 0, 255)); break;
                default: std::memset(destination, 0, sampleBytes); break;
                }
            }
        }
        m_outputFrames += frames;
        return frames * frameBytes;
    }
    qint64 writeData(const char*, qint64) override { return -1; }
private:
    std::shared_ptr<const ResidentMediaAsset> m_asset;
    QAudioFormat m_outputFormat;
    qint64 m_startUs;
    std::atomic<qint64> m_outputFrames{0};
    size_t m_chunkIndex = 0;
};

ResidentVideoPlayer::ResidentVideoPlayer(QObject* parent) : QObject(parent) {
    m_timer.setTimerType(Qt::PreciseTimer);
    m_timer.setInterval(5);
    connect(&m_timer, &QTimer::timeout, this, &ResidentVideoPlayer::tick);
}

ResidentVideoPlayer::~ResidentVideoPlayer() {
    m_timer.stop();
    stopAudio();
    if (m_videoSink) m_videoSink->setVideoFrame(QVideoFrame());
}

QVideoFrame ResidentVideoPlayer::presentationFrame(const QVideoFrame& source) {
    if (!source.isValid()) return {};
    QVideoFrame presentation(std::make_unique<PresentationFrameBuffer>(source));
    presentation.setStartTime(source.startTime());
    presentation.setEndTime(source.endTime());
    presentation.setRotation(source.rotation());
    presentation.setMirrored(source.mirrored());
    return presentation;
}

void ResidentVideoPlayer::setAsset(std::shared_ptr<const ResidentMediaAsset> asset) {
    if (m_asset == asset) return;
    if (!asset || !asset->video || asset->frames.empty()) { clearAsset(); return; }
    m_timer.stop();
    stopAudio();
    setState(QMediaPlayer::StoppedState);
    m_asset = std::move(asset);
    m_positionUs = std::clamp<qint64>(m_positionUs, 0, m_asset->durationUs);
    m_presentedIndex = -1;
    m_completedLoops = 0;
    m_error = QMediaPlayer::NoError;
    m_errorString.clear();
    emit errorChanged();
    emit durationChanged(duration());
    emit seekableChanged(true);
    presentFrame();
    emit positionChanged(position());
    setStatus(QMediaPlayer::LoadedMedia);
}

void ResidentVideoPlayer::clearAsset() {
    m_timer.stop();
    stopAudio();
    setState(QMediaPlayer::StoppedState);
    if (m_videoSink) m_videoSink->setVideoFrame(QVideoFrame());
    m_asset.reset();
    m_presentedIndex = -1;
    setStatus(QMediaPlayer::NoMedia);
    emit durationChanged(0);
    emit seekableChanged(false);
}

void ResidentVideoPlayer::setLoops(int loops) {
    if (loops != QMediaPlayer::Infinite && loops < 1) loops = 1;
    if (m_loops == loops) return;
    m_loops = loops;
    m_completedLoops = 0;
    emit loopsChanged();
}

QAudioOutput* ResidentVideoPlayer::audioOutput() const { return m_audioOutput; }
QVideoSink* ResidentVideoPlayer::videoSink() const { return m_videoSink; }

void ResidentVideoPlayer::setAudioOutput(QAudioOutput* output) {
    if (m_audioOutput == output) return;
    stopAudio();
    if (m_audioOutput) disconnect(m_audioOutput, nullptr, this, nullptr);
    m_audioOutput = output;
    if (output) {
        connect(output, &QAudioOutput::volumeChanged, this, &ResidentVideoPlayer::updateAudioVolume);
        connect(output, &QAudioOutput::mutedChanged, this, &ResidentVideoPlayer::updateAudioVolume);
        connect(output, &QAudioOutput::deviceChanged, this, [this] {
            stopAudio();
            if (isPlaying()) startAudio();
        });
        connect(output, &QObject::destroyed, this, [this] { stopAudio(); });
    }
    if (isPlaying()) startAudio();
}

void ResidentVideoPlayer::setVideoSink(QVideoSink* sink) { setVideoOutput(sink); }

void ResidentVideoPlayer::setVideoOutput(QObject* output) {
    QVideoSink* sink = qobject_cast<QVideoSink*>(output);
    if (output && !sink) {
        const QVariant candidate = output->property("videoSink");
        sink = qvariant_cast<QVideoSink*>(candidate);
        if (!sink) sink = qobject_cast<QVideoSink*>(candidate.value<QObject*>());
    }
    if (m_videoOutput == output && m_videoSink == sink) return;
    if (m_videoSink && m_videoSink != sink) m_videoSink->setVideoFrame(QVideoFrame());
    m_videoOutput = output;
    m_videoSink = sink;
    m_presentedIndex = -1;
    presentFrame();
    emit videoOutputChanged();
}

void ResidentVideoPlayer::setState(QMediaPlayer::PlaybackState state) {
    if (m_state == state) return;
    m_state = state;
    emit playbackStateChanged(state);
}

void ResidentVideoPlayer::setStatus(QMediaPlayer::MediaStatus status) {
    if (m_status == status) return;
    m_status = status;
    emit mediaStatusChanged(status);
}

void ResidentVideoPlayer::play() {
    if (!m_asset || isPlaying()) return;
    if (m_positionUs >= m_asset->durationUs) {
        m_completedLoops = 0;
        setPosition(0);
    }
    m_clockOriginUs = m_positionUs;
    m_clock.start();
    setStatus(QMediaPlayer::BufferedMedia);
    setState(QMediaPlayer::PlayingState);
    if (!m_asset || !isPlaying()) return;
    startAudio();
    m_timer.start();
    m_presentedIndex = -1;
    presentFrame();
}

void ResidentVideoPlayer::pause() {
    if (!m_asset) return;
    if (isPlaying()) tick();
    m_timer.stop();
    stopAudio();
    setState(QMediaPlayer::PausedState);
}

void ResidentVideoPlayer::stop() {
    m_timer.stop();
    stopAudio();
    m_completedLoops = 0;
    setState(QMediaPlayer::StoppedState);
    setPosition(0);
    if (m_asset) setStatus(QMediaPlayer::LoadedMedia);
}

void ResidentVideoPlayer::setPosition(qint64 positionMs) {
    positionMs = std::clamp<qint64>(positionMs, 0, std::numeric_limits<qint64>::max() / 1000);
    const qint64 nextUs = m_asset ? std::min(positionMs * 1000, m_asset->durationUs) : positionMs * 1000;
    const qint64 previousMs = position();
    m_positionUs = nextUs;
    m_presentedIndex = -1;
    m_clockOriginUs = nextUs;
    m_clock.restart();
    if (isPlaying()) { stopAudio(); startAudio(); }
    if (m_asset && m_status == QMediaPlayer::EndOfMedia) setStatus(QMediaPlayer::LoadedMedia);
    presentFrame();
    if (previousMs != position()) emit positionChanged(position());
}

void ResidentVideoPlayer::tick() {
    if (!m_asset || !isPlaying()) return;
    const qint64 previousMs = position();
    // The audio hardware is the clock when present, preventing cumulative
    // A/V drift on long resident videos. Silent/video-only playback uses the
    // monotonic clock, with the same cursor and remote seek semantics.
    const bool audioClock = m_audioSink && m_audioSink->error() == QAudio::NoError
        && (m_audioSink->state() == QAudio::ActiveState || m_audioSink->state() == QAudio::IdleState);
    const qint64 clockUs = audioClock ? m_audioOriginUs + m_audioSink->processedUSecs()
        : m_clockOriginUs + m_clock.nsecsElapsed() / 1000;
    const bool audioFinished = audioClock && m_audioSink->state() == QAudio::IdleState
        && m_pcm && m_pcm->bytesAvailable() == 0;
    const qint64 next = audioFinished ? m_asset->durationUs : std::min(m_asset->durationUs, clockUs);
    m_positionUs = next;
    presentFrame();
    if (previousMs != position()) emit positionChanged(position());
    // Markers and scene synchronization can seek or stop synchronously above.
    if (!m_asset || !isPlaying() || m_positionUs != next || m_positionUs < m_asset->durationUs) return;
    ++m_completedLoops;
    if (m_loops == QMediaPlayer::Infinite || m_completedLoops < m_loops) {
        setPosition(0);
        return;
    }
    m_timer.stop();
    stopAudio();
    setState(QMediaPlayer::StoppedState);
    if (m_asset && !isPlaying() && m_positionUs >= m_asset->durationUs)
        setStatus(QMediaPlayer::EndOfMedia);
}

void ResidentVideoPlayer::presentFrame() {
    if (!m_asset || !m_videoSink || m_asset->frames.empty()) return;
    const auto found = std::upper_bound(m_asset->frames.begin(), m_asset->frames.end(), m_positionUs,
        [](qint64 time, const ResidentVideoFrame& frame) { return time < frame.timestampUs; });
    const qsizetype index = found == m_asset->frames.begin() ? 0 : std::distance(m_asset->frames.begin(), found) - 1;
    if (index == m_presentedIndex) return;
    m_presentedIndex = index;
    m_videoSink->setVideoFrame(presentationFrame(m_asset->frames[size_t(index)].frame));
}

void ResidentVideoPlayer::startAudio() {
    if (!m_asset || m_asset->audio.empty() || !m_audioOutput) return;
    QAudioDevice device = m_audioOutput->device();
    if (device.isNull()) device = QMediaDevices::defaultAudioOutput();
    if (device.isNull()) return;
    const QAudioFormat format = device.isFormatSupported(m_asset->audioFormat)
        ? m_asset->audioFormat : device.preferredFormat();
    if (!format.isValid()) { failAudio(QStringLiteral("The audio output has no usable PCM format")); return; }
    m_audioOriginUs = m_positionUs;
    m_pcm = std::make_unique<ResidentPcmDevice>(m_asset, format, m_positionUs);
    m_audioSink = std::make_unique<QAudioSink>(device, format);
    // Keep seeks and scene starts responsive without duplicating long PCM spans.
    m_audioSink->setBufferSize(format.bytesForDuration(40000));
    updateAudioVolume();
    m_audioSink->start(m_pcm.get());
    if (m_audioSink->error() != QAudio::NoError)
        failAudio(QStringLiteral("Unable to start the resident audio output"));
}

void ResidentVideoPlayer::stopAudio() {
    // stop() drains the device on some platforms. Seek, pause and eviction
    // must discard queued PCM immediately and release the resident asset.
    if (m_audioSink) m_audioSink->reset();
    m_audioSink.reset();
    m_pcm.reset();
}

void ResidentVideoPlayer::updateAudioVolume() {
    if (m_audioSink) m_audioSink->setVolume(!m_audioOutput || m_audioOutput->isMuted() ? 0 : m_audioOutput->volume());
}

void ResidentVideoPlayer::failAudio(const QString& message) {
    m_error = QMediaPlayer::ResourceError;
    m_errorString = message;
    emit errorChanged();
    emit errorOccurred(m_error, m_errorString);
}
