#include "backend/media/ResidentVideoPlayer.h"

#include <QAbstractVideoBuffer>
#include <QAudioOutput>
#include <QBuffer>
#include <QVariant>
#include <QVideoSink>
#include <algorithm>

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


ResidentVideoPlayer::ResidentVideoPlayer(QObject* parent) : QObject(parent) {
    // Preserve marker/automation resolution independently of the platform's
    // coarse positionChanged interval. The decoder/audio clock stays authoritative.
    m_positionTimer.setTimerType(Qt::PreciseTimer);
    m_positionTimer.setInterval(5);
    connect(&m_positionTimer, &QTimer::timeout, this, [this] {
        if (!m_player || m_loading || !isPlaying()) return;
        const qint64 value = m_player->position();
        if (m_positionMs != value) { m_positionMs = value; emit positionChanged(value); }
    });
}
ResidentVideoPlayer::~ResidentVideoPlayer() { releasePlayer(); }

QVideoFrame ResidentVideoPlayer::presentationFrame(const QVideoFrame& source) {
    if (!source.isValid()) return {};
    QVideoFrame frame(std::make_unique<PresentationFrameBuffer>(source));
    frame.setStartTime(source.startTime());
    frame.setEndTime(source.endTime());
    frame.setRotation(source.rotation());
    frame.setMirrored(source.mirrored());
    return frame;
}

void ResidentVideoPlayer::releasePlayer() {
    // Join/destroy the decoder before releasing its QIODevice and the budget.
    // An outstanding source load must never read a destroyed buffer.
    if (m_player) disconnect(m_player.get(), nullptr, this, nullptr);
    if (m_decodeSink) disconnect(m_decodeSink.get(), nullptr, this, nullptr);
    m_player.reset();
    m_decodeSink.reset();
    m_source.reset();
    m_loading = false;
    m_positionTimer.stop();
    m_frame = {};
    if (m_videoSink) m_videoSink->setVideoFrame({});
    if (m_playbackReserved && m_asset && m_asset->releasePlayback) m_asset->releasePlayback();
    m_playbackReserved = false;
}

void ResidentVideoPlayer::setAsset(std::shared_ptr<const ResidentMediaAsset> asset) {
    if (m_asset == asset) return;
    if (!asset || !asset->video || asset->compressedVideo.isEmpty()
        || !asset->firstFrame.frame.isValid()) { clearAsset(); return; }
    releasePlayer();
    m_asset = std::move(asset);
    m_positionMs = std::clamp<qint64>(m_positionMs, 0, duration());
    m_requestedState = QMediaPlayer::StoppedState;
    setState(m_requestedState);
    m_error = QMediaPlayer::NoError;
    m_errorString.clear();
    emit errorChanged();
    emit durationChanged(duration());
    emit seekableChanged(true);
    presentPoster();
    setStatus(QMediaPlayer::LoadedMedia);
    emit positionChanged(position());
    if (m_positionMs > 0) prepare(m_positionMs);
}

void ResidentVideoPlayer::clearAsset() {
    releasePlayer();
    m_asset.reset();
    m_requestedState = QMediaPlayer::StoppedState;
    setState(m_requestedState);
    setStatus(QMediaPlayer::NoMedia);
    emit durationChanged(0);
    emit seekableChanged(false);
}

bool ResidentVideoPlayer::ensurePlayer() {
    if (m_player) return true;
    if (!m_asset) return false;
    if (m_asset->reservePlayback && !m_asset->reservePlayback()) {
        fail(QMediaPlayer::ResourceError, QStringLiteral("Insufficient available RAM for the video playback buffers"));
        return false;
    }
    m_error = QMediaPlayer::NoError;
    m_errorString.clear();
    emit errorChanged();
    m_playbackReserved = true;
    m_loading = true;
    m_source = std::make_unique<QBuffer>();
    // QByteArray implicit sharing: one immutable MP4 allocation for every cursor.
    m_source->setData(m_asset->compressedVideo);
    m_source->open(QIODevice::ReadOnly);
    m_decodeSink = std::make_unique<QVideoSink>();
    m_player = std::make_unique<QMediaPlayer>();
    m_player->setLoops(m_loops);
    m_player->setAudioOutput(m_audioOutput);
    m_player->setVideoSink(m_decodeSink.get());
    connect(m_decodeSink.get(), &QVideoSink::videoFrameChanged, this, [this](const QVideoFrame& frame) {
        if (!m_asset || !frame.isValid()) return;
        m_frame = frame;
        if (m_videoSink) m_videoSink->setVideoFrame(frame);
        emit frameReady(frame.startTime() / 1000);
    });
    connect(m_player.get(), &QMediaPlayer::positionChanged, this, [this](qint64 value) {
        if (m_loading || !m_asset || m_positionMs == value) return;
        m_positionMs = value;
        emit positionChanged(value);
    });
    connect(m_player.get(), &QMediaPlayer::playbackStateChanged, this, [this](auto state) {
        if (!m_loading) setState(state);
    });
    connect(m_player.get(), &QMediaPlayer::errorOccurred, this, &ResidentVideoPlayer::fail);
    connect(m_player.get(), &QMediaPlayer::mediaStatusChanged, this, [this](auto status) {
        if (!m_asset) return;
        if (status == QMediaPlayer::LoadedMedia && m_loading) {
            const QPointer<QMediaPlayer> native = m_player.get();
            // Qt is still completing source initialization while emitting this
            // signal. Prime after that transition, preserving the requested seek
            // across track-selection notifications and rejecting retired players.
            QMetaObject::invokeMethod(this, [this, native] {
                if (!native || native != m_player.get() || !m_asset || !m_loading) return;
                const qint64 target = m_positionMs;
                native->setActiveVideoTrack(m_asset->videoTrack);
                native->setActiveAudioTrack(m_asset->audioTrack);
                native->setActiveSubtitleTrack(-1);
                m_loading = false;
                if (m_requestedState == QMediaPlayer::PlayingState) {
                    native->setPosition(target);
                    native->play();
                } else {
                    native->pause();
                    native->setPosition(target);
                }
                // AVFoundation stays LoadedMedia when primed while paused;
                // it need not emit BufferedMedia. Publish completion so later
                // cursor changes are not mistaken for pending source loads.
                if (native && native == m_player.get() && m_asset)
                    setStatus(native->mediaStatus());
            }, Qt::QueuedConnection);
            return;
        }
        if (status == QMediaPlayer::EndOfMedia) m_requestedState = QMediaPlayer::StoppedState;
        setStatus(status);
    });
    // Synthetic type hint only; no file/network URL. Reads use the memory device.
    m_player->setSourceDevice(m_source.get(), QUrl(QStringLiteral("resident:///video.mp4")));
    return true;
}

bool ResidentVideoPlayer::preparedAt(qint64 positionMs) const {
    const qint64 target = std::clamp<qint64>(positionMs, 0, std::max<qint64>(0, duration() - 1));
    return m_player && !m_loading && m_error == QMediaPlayer::NoError && m_frame.isValid()
        && m_frame.startTime() / 1000 <= target + 1
        && (m_frame.endTime() < 0 ? m_frame.startTime() / 1000 >= target - 1
                                 : m_frame.endTime() / 1000 > target);
}
void ResidentVideoPlayer::prepare(qint64 positionMs) {
    if (!m_asset) return;
    if (m_error != QMediaPlayer::NoError) releasePlayer();
    m_requestedState = QMediaPlayer::PausedState;
    setPosition(positionMs);
    if (ensurePlayer() && !m_loading) m_player->pause();
}
void ResidentVideoPlayer::presentPoster() {
    if (!m_asset || !m_videoSink) return;
    if (m_frame.isValid()) m_videoSink->setVideoFrame(m_frame);
    else if (m_positionMs == 0) m_videoSink->setVideoFrame(presentationFrame(m_asset->firstFrame.frame));
}
void ResidentVideoPlayer::setLoops(int loops) {
    if (loops != QMediaPlayer::Infinite && loops < 1) loops = 1;
    if (m_loops == loops) return;
    m_loops = loops;
    if (m_player) m_player->setLoops(loops);
    emit loopsChanged();
}
QAudioOutput* ResidentVideoPlayer::audioOutput() const { return m_audioOutput; }
QVideoSink* ResidentVideoPlayer::videoSink() const { return m_videoSink; }
void ResidentVideoPlayer::setAudioOutput(QAudioOutput* output) {
    m_audioOutput = output;
    if (m_player) m_player->setAudioOutput(output);
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
    if (m_videoSink && m_videoSink != sink) m_videoSink->setVideoFrame({});
    m_videoOutput = output;
    m_videoSink = sink;
    presentPoster();
    emit videoOutputChanged();
}
void ResidentVideoPlayer::setState(QMediaPlayer::PlaybackState state) {
    if (m_state == state) return;
    m_state = state;
    if (state == QMediaPlayer::PlayingState) m_positionTimer.start();
    else m_positionTimer.stop();
    emit playbackStateChanged(state);
}
void ResidentVideoPlayer::setStatus(QMediaPlayer::MediaStatus status) {
    if (m_status == status) return;
    m_status = status;
    emit mediaStatusChanged(status);
}
void ResidentVideoPlayer::fail(QMediaPlayer::Error error, const QString& message) {
    m_error = error;
    m_errorString = message;
    m_requestedState = QMediaPlayer::StoppedState;
    setState(m_requestedState);
    setStatus(QMediaPlayer::InvalidMedia);
    emit errorChanged();
    emit errorOccurred(error, message);
}
void ResidentVideoPlayer::play() {
    if (!m_asset) return;
    if (m_positionMs >= duration()) setPosition(0);
    m_requestedState = QMediaPlayer::PlayingState;
    if (!ensurePlayer()) return;
    if (!m_loading) m_player->play();
    setState(m_requestedState);
}
void ResidentVideoPlayer::pause() {
    if (!m_asset) return;
    m_requestedState = QMediaPlayer::PausedState;
    if (m_player && !m_loading) m_player->pause();
    setState(m_requestedState);
}
void ResidentVideoPlayer::stop() {
    m_requestedState = QMediaPlayer::StoppedState;
    if (m_player && !m_loading) m_player->stop();
    setState(m_requestedState);
    setPosition(0);
    if (m_asset) setStatus(QMediaPlayer::LoadedMedia);
}
void ResidentVideoPlayer::setPosition(qint64 value) {
    value = std::max<qint64>(0, value);
    if (m_asset) value = std::min(value, duration());
    const bool changed = value != m_positionMs;
    m_positionMs = value;
    if (m_asset && (m_player || value > 0)) {
        if (ensurePlayer() && !m_loading && changed) m_player->setPosition(value);
    }
    if (changed) emit positionChanged(m_positionMs);
    if (!m_player) presentPoster();
}
