#include "backend/media/ResidentVideoPlayer.h"
#include "backend/media/IndexedMediaDecoder.h"
#include "backend/media/EditingProxyCache.h"
#include <QAbstractVideoBuffer>
#include <QAudioOutput>
#include <QVariant>
#include <QVideoSink>
#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <utility>
namespace {
class PresentationFrameBuffer final : public QAbstractVideoBuffer {
public:
    explicit PresentationFrameBuffer(QVideoFrame source) : m_source(std::move(source)) {}
    ~PresentationFrameBuffer() override { unmap(); }
    QVideoFrameFormat format() const override { return m_source.surfaceFormat(); }
    quintptr identity() const { return ResidentVideoPlayer::frameIdentity(m_source); }
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
}

ResidentVideoPlayer::ResidentVideoPlayer(QObject* parent) : QObject(parent) {
    m_positionTimer.setTimerType(Qt::PreciseTimer);
    m_positionTimer.setInterval(5);
    connect(&m_positionTimer, &QTimer::timeout, this, &ResidentVideoPlayer::tick);
    m_scrubTimer.setTimerType(Qt::PreciseTimer);
    m_scrubTimer.setSingleShot(true);
    m_scrubTimer.setInterval(16);
    connect(&m_scrubTimer, &QTimer::timeout, this, &ResidentVideoPlayer::requestFrame);
    m_preparationTimer.setSingleShot(true);
    m_preparationTimer.setInterval(5000);
    connect(&m_preparationTimer, &QTimer::timeout, this, [this] {
        if (!preparedAt(m_positionMs)) fail(QMediaPlayer::FormatError,
            QStringLiteral("The video could not prepare its audiovisual cursor at %1 ms").arg(m_positionMs));
    });
    connect(&m_audio, &PlaybackAudio::prepared, this, [this] {
        if (preparedAt(m_positionMs)) {
            m_preparationTimer.stop();
            startPreparedPlayback();
            emit frameReady(m_positionMs);
        }
    });
    connect(&m_audio, &PlaybackAudio::failed, this, [this](const QString& error) { fail(QMediaPlayer::ResourceError, error); });
}
ResidentVideoPlayer::~ResidentVideoPlayer() { releasePlayer(); }
quintptr ResidentVideoPlayer::frameIdentity(const QVideoFrame& frame) {
    auto* wrapper = dynamic_cast<PresentationFrameBuffer*>(frame.videoBuffer());
    return wrapper ? wrapper->identity() : quintptr(frame.videoBuffer());
}
QVideoFrame ResidentVideoPlayer::presentationFrame(const QVideoFrame& source) {
    if (!source.isValid()) return {};
    QVideoFrame frame(std::make_unique<PresentationFrameBuffer>(source));
    frame.setStartTime(source.startTime()); frame.setEndTime(source.endTime());
    frame.setRotation(source.rotation()); frame.setMirrored(source.mirrored());
    return frame;
}
void ResidentVideoPlayer::releasePlayer() {
    EditingProxyCache::instance().release(this);
    DecodeScheduler::instance().cancel(this, m_cursor.generation++);
    DecodeScheduler::instance().cancel(&m_entryOwner, m_entryGeneration++);
    m_entryFrame.reset(); m_entryPositionMs = -1;
    m_cursor.pending = m_cursor.starting = false;
    m_cursor.pendingIndex = -1;
    m_cursor.frame.reset(); m_cursor.lookahead.clear();
    m_positionTimer.stop(); m_scrubTimer.stop(); m_preparationTimer.stop(); m_audio.setAsset({});
    if (m_videoSink) m_videoSink->setVideoFrame({});
    const bool reserved = std::exchange(m_playbackReserved, false);
    const bool prepared = std::exchange(m_playbackPrepared, false);
    if (reserved && m_asset && m_asset->releasePlayback) m_asset->releasePlayback(prepared);
    if (prepared) emit preparationChanged();
}
void ResidentVideoPlayer::setAsset(std::shared_ptr<const ResidentMediaAsset> asset) {
    if (m_asset == asset) return;
    if (!asset || !asset->video || asset->frameIndex.isEmpty() || !asset->firstFrame.frame.isValid()) { clearAsset(); return; }
    releasePlayer(); m_asset = std::move(asset); m_waitingForMemory = false;
    m_positionMs = std::clamp<qint64>(m_positionMs, 0, duration());
    setState(QMediaPlayer::StoppedState);
    m_error = QMediaPlayer::NoError; m_errorString.clear(); emit errorChanged();
    emit durationChanged(duration()); emit seekableChanged(true);
    setStatus(QMediaPlayer::LoadedMedia); emit positionChanged(m_positionMs);
    QPointer<ResidentVideoPlayer> self(this);
    const bool admitted = ensurePlayer(false);
    if (!self) return;
    if (admitted) {
        requestFrame();
        if (self && !m_scrubbing && !preparedAt(m_positionMs) && !m_preparationTimer.isActive())
            m_preparationTimer.start();
    } else presentPoster();
}
void ResidentVideoPlayer::clearAsset() {
    releasePlayer(); m_asset.reset(); m_waitingForMemory = false; setState(QMediaPlayer::StoppedState);
    setStatus(QMediaPlayer::NoMedia); emit durationChanged(0); emit seekableChanged(false);
    emit preparationChanged();
}
bool ResidentVideoPlayer::ensurePlayer(bool reportFailure) {
    if (m_playbackReserved) return true;
    if (!m_asset) return false;
    auto requested = m_asset;
    QPointer<ResidentVideoPlayer> self(this);
    bool admitted = !requested->reservePlayback || requested->reservePlayback();
    if (!self || m_asset != requested) {
        if (admitted && requested->releasePlayback) requested->releasePlayback(false);
        return false;
    }
    if (!admitted) {
        if (!m_waitingForMemory) {
            m_waitingForMemory = true;
            emit preparationChanged();
            if (!self || m_asset != requested) return false;
        }
        if (reportFailure) fail(QMediaPlayer::ResourceError, QStringLiteral("Insufficient available RAM for prepared video buffers"));
        return false;
    }
    m_waitingForMemory = false;
    m_playbackReserved = true;
    EditingProxyCache::instance().acquire(this, m_asset);
    EditingProxyCache::instance().setInteractive(this, m_scrubbing || isPlaying());
    m_error = QMediaPlayer::NoError; m_errorString.clear(); emit errorChanged();
    m_audio.setAsset(m_asset);
    if (!self || m_asset != requested || m_error != QMediaPlayer::NoError) return false;
    if (!m_scrubbing) m_audio.prepare(m_positionMs * 1000);
    return self && m_asset == requested && m_error == QMediaPlayer::NoError;
}
void ResidentVideoPlayer::retryPreparation() {
    if (!m_waitingForMemory || !m_asset) return;
    QPointer<ResidentVideoPlayer> self(this);
    if (m_error != QMediaPlayer::NoError) releasePlayer();
    if (!self || !ensurePlayer(false)) return;
    prepare(m_positionMs);
    if (self) emit preparationChanged();
}
bool ResidentVideoPlayer::preparedAt(qint64 positionMs) const {
    if (!preparedFrame(positionMs).isValid() || (!m_scrubbing && !m_audio.preparedAt(positionMs * 1000))) return false;
    if (!m_scrubbing && m_asset) {
        const int current = IndexedMediaDecoder::frameAt(*m_asset, positionMs * 1000);
        for (int index = current + 1; index <= current + 2 && index < m_asset->frameIndex.size(); ++index)
            if (!m_cursor.lookahead.value(index)) return false;
    }
    return true;
}
QVideoFrame ResidentVideoPlayer::preparedFrame(qint64 positionMs) const {
    if (!m_asset || !m_playbackReserved || m_error != QMediaPlayer::NoError || !m_cursor.frame) return {};
    if (m_cursor.frame->editingPreview && !m_scrubbing) return {};
    qint64 target = std::clamp<qint64>(positionMs, 0, std::max<qint64>(0, duration()-1));
    const auto& frame = m_cursor.frame->frame;
    bool first = target * 1000 < m_asset->frameIndex.first().timestampUs
        && frame.startTime() == m_asset->frameIndex.first().timestampUs;
    bool covers = frame.startTime()/1000 <= target && frame.endTime() > target * 1000;
    // Hold the final video image over a longer audio tail.
    bool last = frame.startTime() == m_asset->frameIndex.last().timestampUs && target * 1000 >= frame.startTime();
    return first || covers || last ? frame : QVideoFrame{};
}
void ResidentVideoPlayer::prepareEntry(qint64 positionMs) {
    if (!m_asset || positionMs == m_entryPositionMs) return;
    DecodeScheduler::instance().cancel(&m_entryOwner, m_entryGeneration++);
    m_entryPositionMs = positionMs;
    m_entryFrame.reset();
    const auto generation = m_entryGeneration;
    DecodeScheduler::instance().request(&m_entryOwner, generation, m_asset, positionMs * 1000,
        DecodeScheduler::Prefetch, 0, [this, generation](auto frame, const QString&) {
            if (generation == m_entryGeneration) m_entryFrame = std::move(frame);
        });
}
void ResidentVideoPlayer::prepare(qint64 positionMs) {
    if (!m_asset) return;
    if (m_error != QMediaPlayer::NoError) releasePlayer();
    setPosition(positionMs);
    if (!isPlaying()) setState(QMediaPlayer::PausedState);
    if (ensurePlayer()) {
        if (!m_scrubbing) m_audio.prepare(m_positionMs * 1000);
        if (m_scrubbing) scheduleScrubFrame(); else requestFrame();
        if (!m_scrubbing && !preparedAt(m_positionMs) && !m_preparationTimer.isActive()) m_preparationTimer.start();
    }
}
void ResidentVideoPlayer::scheduleScrubFrame() {
    if (!m_scrubbing || m_cursor.pending || m_scrubTimer.isActive()) return;
    if (preparedFrame(m_positionMs).isValid()) return;
    // Leading edge at idle, trailing edge only for the remaining frame budget.
    // Always adding 16 ms starts work just before the next pointer event and
    // needlessly makes even a fast cached preview obsolete on arrival.
    const qint64 elapsed = m_scrubClock.elapsed() - m_lastScrubDispatchMs;
    m_scrubTimer.start(int(std::max<qint64>(0, 16 - elapsed)));
}
bool ResidentVideoPlayer::acceptsIntermediateScrubFrame(qint64 requestedPosition, qint64 requestedAt,
                                                       quint64 directionEpoch, qint64 now) const {
    if (directionEpoch != m_scrubDirectionEpoch || preparedFrame(m_positionMs).isValid()
        || now - m_lastScrubPresentationMs < 100 || now - requestedAt > 1000) return false;
    // Source-time distance grows with pointer velocity (often tens of seconds
    // per wall-clock second). Bound wall-clock age, not an arbitrary one-second
    // source distance, and never show a request from the previous direction.
    if (m_scrubDirection > 0 && requestedPosition > m_positionMs) return false;
    if (m_scrubDirection < 0 && requestedPosition < m_positionMs) return false;
    if (!m_scrubDirection) return false;
    if (m_scrubPresentationEpoch != directionEpoch || !m_cursor.frame) return true;
    const qint64 displayed = m_cursor.frame->frame.startTime() / 1000;
    return m_scrubDirection > 0 ? requestedPosition >= displayed : requestedPosition <= displayed;
}
void ResidentVideoPlayer::requestFrame() {
    if (!m_asset || !m_playbackReserved || m_cursor.pending) return;
    if (preparedFrame(m_positionMs).isValid()) {
        QPointer<ResidentVideoPlayer> self(this);
        if (!m_scrubbing) prefetch();
        if (!self) return;
        startPreparedPlayback();
        return;
    }
    const auto generation = m_cursor.generation;
    const int requestedIndex = IndexedMediaDecoder::frameAt(*m_asset, m_positionMs * 1000);
    const qint64 requestedPosition = m_positionMs;
    const qint64 requestedAt = m_scrubbing ? m_scrubClock.elapsed() : 0;
    const quint64 directionEpoch = m_scrubDirectionEpoch;
    if (m_scrubbing) m_lastScrubDispatchMs = requestedAt;
    m_cursor.pending = true;
    m_cursor.pendingIndex = requestedIndex;
    m_cursor.pendingDirectionEpoch = directionEpoch;
    DecodeScheduler::instance().request(this, generation, m_asset, m_positionMs * 1000,
        m_scrubbing ? DecodeScheduler::Scrub : isPlaying() || m_cursor.starting ? DecodeScheduler::Playback : DecodeScheduler::Prepare, 0,
        [this, generation, requestedIndex, requestedPosition, requestedAt, directionEpoch](SharedMediaFramePtr frame, const QString& error) {
            if (generation != m_cursor.generation || !m_asset) return;
            m_cursor.pending = false;
            m_cursor.pendingIndex = -1;
            // Usually only the newest target is presented. A cold long-GOP
            // decode may complete behind a moving pointer: allow bounded,
            // directionally useful progress instead of freezing until release.
            // It never satisfies preparedAt() for a different target.
            if (m_scrubbing && requestedIndex != IndexedMediaDecoder::frameAt(*m_asset, m_positionMs * 1000)) {
                if (!frame || !acceptsIntermediateScrubFrame(requestedPosition, requestedAt, directionEpoch, m_scrubClock.elapsed())) {
                    scheduleScrubFrame(); return;
                }
            }
            if (!frame) { fail(QMediaPlayer::FormatError, error); return; }
            m_cursor.frame = std::move(frame);
            if (m_scrubbing) {
                m_lastScrubPresentationMs = m_scrubClock.elapsed();
                m_scrubPresentationEpoch = directionEpoch;
            }
            QPointer<ResidentVideoPlayer> self(this);
            presentPoster();
            if (!self) return;
            // A matching scrub request can outlive pointer release. A native
            // result is already exact; a proxy must first request that exact
            // frame, before lookahead starts another decode of the same GOP.
            if (!m_scrubbing && m_cursor.frame->editingPreview) {
                requestFrame();
                return;
            }
            if (!m_scrubbing) prefetch();
            if (!self) return;
            if (preparedAt(m_positionMs)) m_preparationTimer.stop();
            startPreparedPlayback();
            if (!self || !m_cursor.frame || generation != m_cursor.generation) return;
            emit frameReady(m_cursor.frame->frame.startTime()/1000);
            if (self && generation == m_cursor.generation) {
                if (m_scrubbing) scheduleScrubFrame(); else requestFrame();
            }
        });
}
void ResidentVideoPlayer::prefetch() {
    if (!m_asset || m_scrubbing) return;
    const bool sequential = !m_asset->allIntra;
    QPointer<ResidentVideoPlayer> self(this);
    int current = IndexedMediaDecoder::frameAt(*m_asset, m_positionMs * 1000);
    for (auto it = m_cursor.lookahead.begin(); it != m_cursor.lookahead.end();) {
        if (it.key() <= current || it.key() > current + 2) it = m_cursor.lookahead.erase(it); else ++it;
    }
    const auto generation = m_cursor.generation;
    for (int index = current+1; index <= current+2 && index < m_asset->frameIndex.size(); ++index) {
        if (m_cursor.lookahead.contains(index)) {
            if (sequential && !m_cursor.lookahead.value(index)) break;
            continue;
        }
        m_cursor.lookahead.insert(index, {});
        const auto now = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
        const auto deadline = now + std::max<qint64>(0, m_asset->frameIndex[index].timestampUs - m_positionMs * 1000) * 1000;
        DecodeScheduler::instance().request(this, generation, m_asset, m_asset->frameIndex[index].timestampUs,
            isPlaying() || m_cursor.starting ? DecodeScheduler::Playback : DecodeScheduler::Prepare, deadline, [this, generation, index, sequential](auto frame, const QString& error) {
                if (generation != m_cursor.generation || !m_cursor.lookahead.contains(index)) return;
                if (!frame) { fail(QMediaPlayer::FormatError, error); return; }
                m_cursor.lookahead[index] = std::move(frame);
                if (sequential) {
                    QPointer<ResidentVideoPlayer> self(this);
                    prefetch();
                    if (!self || generation != m_cursor.generation) return;
                }
                if (preparedAt(m_positionMs)) {
                    m_preparationTimer.stop();
                    QPointer<ResidentVideoPlayer> self(this);
                    startPreparedPlayback();
                    if (self) emit frameReady(m_positionMs);
                }
            });
        if (!self || generation != m_cursor.generation) return;
        // Source GOP history now includes +1 on its worker before +2 is sent.
        // Launching both together can decode the same long GOP on two workers.
        if (sequential) break;
    }
}
void ResidentVideoPlayer::presentPoster() {
    if (!m_videoSink) return;
    if (m_cursor.frame) m_videoSink->setVideoFrame(presentationFrame(m_cursor.frame->frame));
    else if (m_asset && m_positionMs == 0) m_videoSink->setVideoFrame(presentationFrame(m_asset->firstFrame.frame));
}
void ResidentVideoPlayer::startPreparedPlayback() {
    if (m_scrubbing) return; // Preview readiness does not retire the audiovisual reservation.
    if (!preparedAt(m_positionMs)) return;
    if (!m_playbackPrepared) {
        m_playbackPrepared = true;
        QPointer<ResidentVideoPlayer> self(this);
        const auto preparedAsset = m_asset;
        if (preparedAsset->playbackPrepared) preparedAsset->playbackPrepared();
        if (!self || m_asset != preparedAsset) return;
        emit preparationChanged();
        if (!self || m_asset != preparedAsset) return;
    }
    if (!m_cursor.starting) return;
    m_cursor.starting = false; m_cursor.anchorMs = m_positionMs; m_cursor.clock.restart();
    m_audio.play(m_positionMs * 1000); setState(QMediaPlayer::PlayingState);
    setStatus(QMediaPlayer::BufferedMedia); m_preparationTimer.stop(); prefetch();
}
void ResidentVideoPlayer::tick() {
    if (!isPlaying() || !m_asset) return;
    qint64 position = m_cursor.anchorMs + m_cursor.clock.elapsed();
    if (position >= duration()) {
        if (m_loops == QMediaPlayer::Infinite || ++m_completedLoops < m_loops) {
            setPosition(0); m_audio.play(0); return;
        }
        m_positionMs = duration();
        QPointer<ResidentVideoPlayer> self(this);
        const auto generation = m_cursor.generation;
        emit positionChanged(m_positionMs);
        if (!self || generation != m_cursor.generation || m_positionMs != duration()) return;
        m_audio.pause(); setState(QMediaPlayer::StoppedState); setStatus(QMediaPlayer::EndOfMedia); return;
    }
    QPointer<ResidentVideoPlayer> self(this);
    if (m_positionMs != position) { m_positionMs = position; emit positionChanged(position); }
    if (self) requestFrame();
}
void ResidentVideoPlayer::play() {
    setScrubbing(false);
    if (!m_asset) return;
    if (m_error != QMediaPlayer::NoError) releasePlayer();
    if (isPlaying()) return;
    if (m_positionMs >= duration()) setPosition(0);
    if (!ensurePlayer()) return;
    m_cursor.starting = true; m_completedLoops = 0;
    m_audio.prepare(m_positionMs * 1000); requestFrame(); startPreparedPlayback();
    if (m_cursor.starting && !m_preparationTimer.isActive()) m_preparationTimer.start();
}
void ResidentVideoPlayer::pause() {
    if (isPlaying()) tick();
    m_cursor.starting = false; m_audio.pause();
    if (m_asset) {
        if (!m_scrubbing) m_audio.prepare(m_positionMs * 1000);
        setState(QMediaPlayer::PausedState);
    }
}
void ResidentVideoPlayer::stop() {
    m_cursor.starting = false; m_audio.pause(); setState(QMediaPlayer::StoppedState);
    setScrubbing(false); setPosition(0);
    if (m_asset) setStatus(QMediaPlayer::LoadedMedia);
}
void ResidentVideoPlayer::setPosition(qint64 value) {
    value = std::max<qint64>(0, value);
    if (m_asset) value = std::min(value, duration());
    bool changed = value != m_positionMs;
    if (changed && m_scrubbing) {
        const int direction = value > m_positionMs ? 1 : -1;
        if (m_scrubDirection && direction != m_scrubDirection) ++m_scrubDirectionEpoch;
        m_scrubDirection = direction;
    }
    m_positionMs = value;
    m_cursor.anchorMs = value; m_cursor.clock.restart();
    if (changed) {
        if (!m_scrubbing) {
            DecodeScheduler::instance().cancel(this, m_cursor.generation++);
            m_cursor.pending = false; m_cursor.pendingIndex = -1; m_cursor.lookahead.clear();
        }
        if (m_asset && ensurePlayer()) {
            if (m_scrubbing) {
                EditingProxyCache::instance().prioritize(this, IndexedMediaDecoder::frameAt(*m_asset, value * 1000));
                scheduleScrubFrame();
            } else {
                m_audio.prepare(value * 1000);
                if (isPlaying()) m_audio.play(value * 1000);
                requestFrame();
            }
        }
        emit positionChanged(value);
    }
}
void ResidentVideoPlayer::setScrubbing(bool enabled) {
    if (m_scrubbing == enabled) return;
    m_scrubbing = enabled;
    EditingProxyCache::instance().setInteractive(this, m_playbackReserved && (enabled || isPlaying()));
    if (enabled) {
        m_scrubClock.restart();
        m_lastScrubPresentationMs = 0;
        m_lastScrubDispatchMs = -16;
        m_scrubDirection = 0;
        m_scrubDirectionEpoch = 0;
        m_scrubPresentationEpoch = std::numeric_limits<quint64>::max();
        m_audio.pause();
        m_cursor.starting = false;
        if (isPlaying()) {
            QPointer<ResidentVideoPlayer> self(this);
            setState(QMediaPlayer::PausedState);
            if (!self || !m_scrubbing) return;
        }
        m_preparationTimer.stop();
        // Retire preparation/lookahead work before this interactive cursor is
        // scheduled; completed frames remain valid in the shared cache.
        DecodeScheduler::instance().cancel(this, m_cursor.generation++);
        m_cursor.pending = false;
        m_cursor.pendingIndex = -1;
        for (auto it = m_cursor.lookahead.begin(); it != m_cursor.lookahead.end();) {
            if (!it.value()) it = m_cursor.lookahead.erase(it); else ++it;
        }
    }
    if (!enabled) {
        m_scrubTimer.stop();
        // Do not throw away a decode that is already producing the release
        // frame. Long-GOP native requests otherwise start from their keyframe
        // again. Results from another target or drag direction remain obsolete.
        const bool keepPending = m_cursor.pending && m_asset
            && m_cursor.pendingIndex == IndexedMediaDecoder::frameAt(*m_asset, m_positionMs * 1000)
            && m_cursor.pendingDirectionEpoch == m_scrubDirectionEpoch;
        if (!keepPending) {
            DecodeScheduler::instance().cancel(this, m_cursor.generation++);
            m_cursor.pending = false;
            m_cursor.pendingIndex = -1;
        }
        // Empty entries represent in-flight requests from the old generation.
        // Cancellation removes their callbacks: retaining them would prevent
        // prefetch() from ever requesting those required frames again.
        for (auto it = m_cursor.lookahead.begin(); it != m_cursor.lookahead.end();) {
            if (!it.value()) it = m_cursor.lookahead.erase(it); else ++it;
        }
        QPointer<ResidentVideoPlayer> self(this);
        m_audio.prepare(m_positionMs * 1000);
        if (self) requestFrame();
    }
}
void ResidentVideoPlayer::setLoops(int loops) {
    if (loops != QMediaPlayer::Infinite && loops < 1) loops = 1;
    if (m_loops == loops) return;
    m_loops = loops; emit loopsChanged();
}
QAudioOutput* ResidentVideoPlayer::audioOutput() const { return m_audioOutput; }
QVideoSink* ResidentVideoPlayer::videoSink() const { return m_videoSink; }
void ResidentVideoPlayer::setAudioOutput(QAudioOutput* output) { m_audioOutput = output; m_audio.setOutput(output); }
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
    m_videoOutput = output; m_videoSink = sink; presentPoster(); emit videoOutputChanged();
}
void ResidentVideoPlayer::setState(QMediaPlayer::PlaybackState state) {
    if (m_state == state) return;
    m_state = state;
    EditingProxyCache::instance().setInteractive(this, m_playbackReserved && (m_scrubbing || state == QMediaPlayer::PlayingState));
    if (state == QMediaPlayer::PlayingState) m_positionTimer.start(); else m_positionTimer.stop();
    emit playbackStateChanged(state);
}
void ResidentVideoPlayer::setStatus(QMediaPlayer::MediaStatus status) { if (m_status != status) { m_status = status; emit mediaStatusChanged(status); } }
void ResidentVideoPlayer::fail(QMediaPlayer::Error error, const QString& message) {
    m_preparationTimer.stop(); m_cursor.starting = false; m_audio.pause();
    m_error = error; m_errorString = message;
    setState(QMediaPlayer::StoppedState); setStatus(QMediaPlayer::InvalidMedia);
    QPointer<ResidentVideoPlayer> self(this);
    emit errorChanged();
    if (!self) return;
    emit errorOccurred(error, message);
    if (!self) return;
    const auto generation = m_cursor.generation;
    QMetaObject::invokeMethod(this, [this, generation] {
        if (generation != m_cursor.generation || m_error == QMediaPlayer::NoError) return;
        releasePlayer(); presentPoster();
    }, Qt::QueuedConnection);
}
