#include "backend/media/ResidentVideoPlayer.h"

#include <QAbstractVideoBuffer>
#include <QAudioOutput>
#include <QBuffer>
#include <QVariant>
#include <QVideoSink>
#include <QThreadPool>
#include <QtConcurrent/QtConcurrentRun>
#include <algorithm>
#include <utility>

namespace {
constexpr int PreparationTimeoutMs = 5000;
QThreadPool& scrubPool() {
    static QThreadPool pool;
    static const bool initialized = [] {
        pool.setMaxThreadCount(2);
        pool.setExpiryTimeout(1000);
        return true;
    }();
    Q_UNUSED(initialized);
    return pool;
}
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
    // Keep the newest pointer target, with at most one seek per 16 ms window.
    // Do not restart this timer on movement: continuous drags must still render.
    m_scrubTimer.setSingleShot(true);
    m_scrubTimer.setTimerType(Qt::PreciseTimer);
    m_scrubTimer.setInterval(16);
    connect(&m_scrubTimer, &QTimer::timeout, this, &ResidentVideoPlayer::flushScrubSeek);
    m_preparationTimer.setSingleShot(true);
    connect(&m_preparationTimer, &QTimer::timeout, this, [this] {
        if (preparedAt(m_positionMs)) return;
        fail(QMediaPlayer::FormatError,
             QStringLiteral("The video player could not prepare an image at %1 ms (last decoded image: %2 ms)")
                 .arg(m_positionMs).arg(m_frame.isValid() ? m_frame.startTime() / 1000 : -1));
    });
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
    const bool wasReserved = std::exchange(m_playbackReserved, false);
    const bool wasPrepared = std::exchange(m_playbackPrepared, false);
    const auto releaseReservation = wasReserved && m_asset ? m_asset->releasePlayback
                                                         : std::function<void(bool)>();
    // Join/destroy the decoder before releasing its QIODevice and the budget.
    // An outstanding source load must never read a destroyed buffer.
    if (m_player) disconnect(m_player.get(), nullptr, this, nullptr);
    if (m_decodeSink) disconnect(m_decodeSink.get(), nullptr, this, nullptr);
    m_player.reset();
    m_decodeSink.reset();
    m_source.reset();
    m_loading = false;
    m_scrubTimer.stop();
    m_scrubPending = false;
    m_preparationTimer.stop();
    m_positionTimer.stop();
    m_frame = {};
    ++m_scrubGeneration;
    m_scrubFrame = {};
    m_scrubFrameIndex = -1;
    m_awaitingOriginal = false;
    if (m_videoSink) m_videoSink->setVideoFrame({});
    if (releaseReservation) releaseReservation(wasPrepared);
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
    // Prime the bounded decoder queues while the asset becomes resident, even
    // at zero. The cached poster alone must not defer source/codec setup until
    // the first Play or seek. At zero, budget refusal leaves the poster usable;
    // an explicit playback request can retry admission later. A retained cursor
    // still requires its decoded frame and the normal preparation/error path.
    if (m_positionMs > 0) prepare(m_positionMs);
    else ensurePlayer(false);
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

bool ResidentVideoPlayer::ensurePlayer(bool reportFailure) {
    if (m_player) return true;
    if (!m_asset) return false;
    const auto requestedAsset = m_asset;
    const QPointer<ResidentVideoPlayer> self(this);
    const bool reserved = !requestedAsset->reservePlayback || requestedAsset->reservePlayback();
    if (!self || m_asset != requestedAsset) {
        if (reserved && requestedAsset->releasePlayback) requestedAsset->releasePlayback(false);
        return false;
    }
    if (!reserved) {
        if (reportFailure)
            fail(QMediaPlayer::ResourceError, QStringLiteral("Insufficient available RAM for the video playback buffers"));
        return false;
    }
    m_playbackReserved = true;
    m_playbackPrepared = false;
    m_error = QMediaPlayer::NoError;
    m_errorString.clear();
    emit errorChanged();
    if (!self || m_asset != requestedAsset || !m_playbackReserved) return false;
    m_loading = true;
    m_source = std::make_unique<QBuffer>();
    // QByteArray implicit sharing: one immutable MP4 allocation for every cursor.
    m_source->setData(m_asset->compressedVideo);
    m_source->open(QIODevice::ReadOnly);
    m_decodeSink = std::make_unique<QVideoSink>();
    m_player = std::make_unique<QMediaPlayer>();
    if (!m_player->isAvailable()) {
        fail(QMediaPlayer::ResourceError, QStringLiteral("The video playback engine is unavailable"));
        return false;
    }
    m_player->setLoops(m_loops);
    m_player->setAudioOutput(m_audioOutput);
    m_player->setVideoSink(m_decodeSink.get());
    connect(m_decodeSink.get(), &QVideoSink::videoFrameChanged, this,
            [this, native = QPointer<QMediaPlayer>(m_player.get())](const QVideoFrame& frame) {
        if (!native || native != m_player.get() || !m_asset || !frame.isValid()) return;
        if (m_playbackReserved && !m_playbackPrepared) {
            m_playbackPrepared = true;
            const QPointer<ResidentVideoPlayer> self(this);
            const auto prepared = m_asset->playbackPrepared;
            if (prepared) prepared();
            // Admission notifications can synchronously retire this player.
            if (!self || !native || native != m_player.get() || !m_asset) return;
        }
        m_frame = frame;
        if (preparedAt(m_positionMs) || isPlaying()) m_preparationTimer.stop();
        const QPointer<ResidentVideoPlayer> self(this);
        // Native frames must not replace the responsive proxy with an old seek.
        if (!(m_scrubbing && hasScrubProxy()) && (!m_awaitingOriginal || preparedAt(m_positionMs))) {
            m_awaitingOriginal = false;
            m_scrubFrame = {};
            m_scrubFrameIndex = -1;
            if (m_videoSink) m_videoSink->setVideoFrame(frame);
        }
        if (self && native && native == m_player.get()) emit frameReady(frame.startTime() / 1000);
    });
    connect(m_player.get(), &QMediaPlayer::positionChanged, this, [this](qint64 value) {
        if (m_loading || m_scrubbing || m_awaitingOriginal || !m_asset || m_positionMs == value) return;
        m_positionMs = value;
        emit positionChanged(value);
    });
    connect(m_player.get(), &QMediaPlayer::playbackStateChanged, this, [this](auto state) {
        // A stopped occurrence may own a primed, paused native decoder.
        if (!m_loading) setState(state == QMediaPlayer::PausedState
                && m_requestedState == QMediaPlayer::StoppedState ? m_requestedState : state);
    });
    connect(m_player.get(), &QMediaPlayer::errorOccurred, this,
            [this, native = QPointer<QMediaPlayer>(m_player.get())](auto error, const QString& message) {
        if (native && native == m_player.get()) fail(error, message);
    });
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
                // Select the cursor before pause/play creates decoder threads,
                // so a restored cursor does not first decode from zero and then
                // immediately flush that work for a second seek.
                native->setPosition(target);
                if (m_requestedState == QMediaPlayer::PlayingState) {
                    m_preparationTimer.stop();
                    native->play();
                } else {
                    native->pause();
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
    watchPreparation();
    m_player->setSourceDevice(m_source.get(), QUrl(QStringLiteral("resident:///video.mp4")));
    return true;
}

bool ResidentVideoPlayer::preparedAt(qint64 positionMs) const {
    return preparedFrame(positionMs).isValid();
}
QVideoFrame ResidentVideoPlayer::preparedFrame(qint64 positionMs) const {
    if (!m_player || m_loading || m_error != QMediaPlayer::NoError || !m_asset
        || !m_frame.isValid() || m_frame.startTime() < 0) return {};
    const qint64 target = std::clamp<qint64>(positionMs, 0, std::max<qint64>(0, duration() - 1));
    const qint64 firstUs = m_asset->firstFrame.timestampUs;
    const qint64 startUs = m_frame.startTime();
    // A valid MP4 may start its video after its audio. Hold the verified first
    // image before its PTS without moving either track's presentation clock.
    const bool firstImage = target * 1000 < firstUs && startUs / 1000 == firstUs / 1000;
    // Qt seeks in milliseconds, frames use microseconds. Compare the same
    // millisecond bucket at the start, and an exclusive end for VFR boundaries.
    const bool coversTarget = startUs / 1000 <= target
        && (m_frame.endTime() < 0 ? startUs / 1000 == target : m_frame.endTime() > target * 1000);
    return firstImage || coversTarget ? m_frame : QVideoFrame{};
}
void ResidentVideoPlayer::watchPreparation() {
    // Playback may legitimately traverse a long audio-only interval. The
    // image deadline applies to source loading and paused preparation only.
    if (!m_asset || (m_scrubbing && hasScrubProxy()) || preparedAt(m_positionMs)
        || (!m_loading && m_requestedState == QMediaPlayer::PlayingState)) {
        m_preparationTimer.stop();
        return;
    }
    m_preparationTimer.start(PreparationTimeoutMs);
}
void ResidentVideoPlayer::prepare(qint64 positionMs) {
    if (!m_asset) return;
    if (m_error != QMediaPlayer::NoError) releasePlayer();
    m_requestedState = QMediaPlayer::PausedState;
    setPosition(positionMs);
    if (ensurePlayer() && !m_loading) m_player->pause();
    if (m_player && m_error == QMediaPlayer::NoError) watchPreparation();
}
void ResidentVideoPlayer::presentPoster() {
    if (!m_asset || !m_videoSink) return;
    if (m_scrubFrame.isValid()) m_videoSink->setVideoFrame(m_scrubFrame);
    else if (m_frame.isValid()) m_videoSink->setVideoFrame(m_frame);
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
    m_preparationTimer.stop();
    if (m_player) {
        const QPointer<QMediaPlayer> failedPlayer = m_player.get();
        // A failed source can never produce the frame that retires its future
        // reservation. Destroy its native decoder after the current callback,
        // retaining the error and cached asset for display or an explicit retry.
        QMetaObject::invokeMethod(this, [this, failedPlayer] {
            if (!failedPlayer || failedPlayer != m_player.get()) return;
            const QPointer<ResidentVideoPlayer> self(this);
            releasePlayer();
            if (self) presentPoster();
        }, Qt::QueuedConnection);
    }
    m_error = error;
    m_errorString = message;
    m_requestedState = QMediaPlayer::StoppedState;
    setState(m_requestedState);
    setStatus(QMediaPlayer::InvalidMedia);
    emit errorChanged();
    emit errorOccurred(error, message);
}
void ResidentVideoPlayer::play() {
    setScrubbing(false);
    if (!m_asset) return;
    if (m_error != QMediaPlayer::NoError) releasePlayer();
    if (m_positionMs >= duration()) setPosition(0);
    m_requestedState = QMediaPlayer::PlayingState;
    if (!ensurePlayer()) return;
    if (!m_loading) {
        m_preparationTimer.stop();
        m_player->play();
    }
    setState(m_requestedState);
}
void ResidentVideoPlayer::pause() {
    if (!m_asset) return;
    m_requestedState = QMediaPlayer::PausedState;
    if (m_player && !m_loading) m_player->pause();
    setState(m_requestedState);
}
void ResidentVideoPlayer::stop() {
    setScrubbing(false);
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
    if (m_scrubbing && hasScrubProxy()) {
        m_scrubPending = true;
        requestScrubFrame();
    } else if (m_asset && (m_player || value > 0)) {
        if (ensurePlayer() && !m_loading && changed) {
            if (m_scrubbing) {
                m_scrubPending = true;
                if (!m_scrubTimer.isActive()) m_scrubTimer.start();
            } else {
                watchPreparation();
                m_player->setPosition(value);
            }
            // Qt does not produce a new frame for a stopped native player.
            // Keep paused scrubbing functional after Stop/EndOfMedia as well.
            if (m_player->playbackState() == QMediaPlayer::StoppedState)
                m_player->pause();
        }
    }
    if (changed) emit positionChanged(m_positionMs);
    if (!m_player) presentPoster();
}

bool ResidentVideoPlayer::hasScrubProxy() const {
    return m_asset && !m_asset->scrubFrames.empty();
}

void ResidentVideoPlayer::setScrubbing(bool enabled) {
    if (m_scrubbing == enabled) return;
    ++m_scrubGeneration; // reject a completion from a previous gesture
    m_scrubbing = enabled;
    if (enabled) {
        if (hasScrubProxy()) {
            m_preparationTimer.stop();
            m_awaitingOriginal = false;
        }
        return;
    }
    m_awaitingOriginal = m_scrubFrame.isValid();
    // The last source cursor is exact, even if no timer/worker has run yet.
    flushScrubSeek();
    if (preparedAt(m_positionMs)) {
        m_awaitingOriginal = false;
        m_scrubFrame = {};
        m_scrubFrameIndex = -1;
        if (m_videoSink) m_videoSink->setVideoFrame(m_frame);
    }
}

void ResidentVideoPlayer::flushScrubSeek() {
    m_scrubTimer.stop();
    if (m_scrubbing && hasScrubProxy()) { requestScrubFrame(); return; }
    if (!std::exchange(m_scrubPending, false) || !m_asset) return;
    if (!ensurePlayer() || m_loading) return; // source initialization reads the latest cursor
    watchPreparation();
    m_player->setPosition(m_positionMs);
    if (m_player->playbackState() == QMediaPlayer::StoppedState)
        m_player->pause();
}

void ResidentVideoPlayer::requestScrubFrame() {
    if (!m_scrubbing || !hasScrubProxy() || m_scrubDecode) return;
    const auto& frames = m_asset->scrubFrames;
    const qint64 targetUs = m_positionMs * 1000;
    auto it = std::upper_bound(frames.begin(), frames.end(), targetUs,
        [](qint64 target, const ResidentScrubFrame& frame) { return target < frame.timestampUs; });
    if (it != frames.begin()) --it; // first-frame hold before delayed video; last-frame hold at end
    const auto index = qint64(it - frames.begin());
    if (index == m_scrubFrameIndex) return;
    auto* worker = new QFutureWatcher<QImage>(this);
    m_scrubDecode = worker;
    const quint64 generation = m_scrubGeneration;
    const qint64 start = it->timestampUs;
    const qint64 end = it + 1 == frames.end() ? start + it->durationUs : (it + 1)->timestampUs;
    connect(worker, &QFutureWatcher<QImage>::finished, this, [this, worker, generation, index, start, end] {
        const QImage image = worker->result();
        worker->deleteLater();
        m_scrubDecode = nullptr;
        if (generation == m_scrubGeneration && m_scrubbing && hasScrubProxy() && !image.isNull()) {
            m_scrubFrame = QVideoFrame(image);
            m_scrubFrame.setStartTime(start);
            m_scrubFrame.setEndTime(end);
            m_scrubFrameIndex = index;
            const QPointer<ResidentVideoPlayer> self(this);
            if (m_videoSink) m_videoSink->setVideoFrame(m_scrubFrame);
            if (!self) return;
        }
        // A single in-flight decode and the current cursor replace an unbounded
        // request queue. Publish completed work during motion, then catch up.
        if (!image.isNull()) requestScrubFrame();
    });
    // Copy only this compressed image, not the entire asset: eviction and teardown
    // can reclaim the video while this short, isolated job finishes.
    worker->setFuture(QtConcurrent::run(&scrubPool(), [jpeg = it->jpeg] {
        return QImage::fromData(jpeg, "JPEG");
    }));
}
