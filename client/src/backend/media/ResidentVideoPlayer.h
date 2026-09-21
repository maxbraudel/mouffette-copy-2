#pragma once

#include "backend/media/ResidentMediaAsset.h"
#include <QMediaPlayer>
#include "backend/media/DecodeScheduler.h"
#include "backend/media/PlaybackAudio.h"
#include <QElapsedTimer>
#include <QHash>
#include <QObject>
#include <QPointer>
#include <QTimer>
#include <memory>
#include <limits>

class QAudioOutput;
class QVideoSink;


// Compatible QML facade over a lightweight cursor. Decoding and device output
// are shared by the scheduler and mixer, independent of occurrence count.
class ResidentVideoPlayer final : public QObject
{
    Q_OBJECT
    Q_PROPERTY(QObject* videoOutput READ videoOutput WRITE setVideoOutput NOTIFY videoOutputChanged)
    Q_PROPERTY(qint64 position READ position WRITE setPosition NOTIFY positionChanged)
    Q_PROPERTY(qint64 duration READ duration NOTIFY durationChanged)
    Q_PROPERTY(QMediaPlayer::PlaybackState playbackState READ playbackState NOTIFY playbackStateChanged)
    Q_PROPERTY(QMediaPlayer::MediaStatus mediaStatus READ mediaStatus NOTIFY mediaStatusChanged)
    Q_PROPERTY(bool seekable READ isSeekable NOTIFY seekableChanged)
    Q_PROPERTY(int loops READ loops WRITE setLoops NOTIFY loopsChanged)

public:
    explicit ResidentVideoPlayer(QObject* parent = nullptr);
    ~ResidentVideoPlayer() override;
    void setAsset(std::shared_ptr<const ResidentMediaAsset> asset);
    bool preparedAt(qint64 positionMs) const;
    bool hasPreparedPlayback() const { return m_playbackPrepared; }
    bool waitingForMemory() const { return m_waitingForMemory; }
    // Retry a refused reservation after the residency manager samples memory.
    // This never retries decoding/device failures or starts playback itself.
    void retryPreparation();
    // The decoded image for this cursor, including a hold before the first PTS.
    // Returns an invalid frame while loading, on error, or for another cursor.
    QVideoFrame preparedFrame(qint64 positionMs) const;
    void prepareEntry(qint64 positionMs);
    void prepare(qint64 positionMs); // reuse or request the exact prepared cursor
    void setScrubbing(bool enabled); // latest target; editing previews are refined on release
    void clearAsset(); // retain the occurrence's cursor for later re-residency
    // Fresh Qt presentation/cache identity, shared immutable CPU planes. Never
    // call toImage() on a long-lived asset frame: Qt caches that RGBA conversion.
    static QVideoFrame presentationFrame(const QVideoFrame& frame);
    static quintptr frameIdentity(const QVideoFrame& frame);
    std::shared_ptr<const ResidentMediaAsset> asset() const { return m_asset; }
    qint64 position() const { return m_positionMs; }
    qint64 duration() const {
        const qint64 us = m_asset ? m_asset->durationUs : 0;
        return us > 0 ? us / 1000 + (us % 1000 != 0) : 0;
    }
    QMediaPlayer::PlaybackState playbackState() const { return m_state; }
    QMediaPlayer::MediaStatus mediaStatus() const { return m_status; }
    QMediaPlayer::Error error() const { return m_error; }
    QString errorString() const { return m_errorString; }
    bool isSeekable() const { return bool(m_asset); }
    bool audioPresentedSincePlay() const { return m_audio.presentedSincePlay(); }
    bool isPlaying() const { return m_state == QMediaPlayer::PlayingState; }
    int loops() const { return m_loops; }
    void setLoops(int loops);
    void setAudioOutput(QAudioOutput* output);
    void setAudioRole(PlaybackAudio::Role role) { m_audio.setRole(role); }
    QAudioOutput* audioOutput() const;
    void setVideoSink(QVideoSink* sink);
    QVideoSink* videoSink() const;
    void setVideoOutput(QObject* output);
    QObject* videoOutput() const { return m_videoOutput; }

public slots:
    void play();
    void pause();
    void stop();
    void setPosition(qint64 positionMs);

signals:
    void positionChanged(qint64 position);
    void durationChanged(qint64 duration);
    void playbackStateChanged(QMediaPlayer::PlaybackState state);
    void mediaStatusChanged(QMediaPlayer::MediaStatus status);
    void errorChanged();
    void errorOccurred(QMediaPlayer::Error error, const QString& errorString);
    void seekableChanged(bool seekable);
    void loopsChanged();
    void videoOutputChanged();
    void frameReady(qint64 timestampMs); // A validated native frame (shared preparation may satisfy it).
    void preparationChanged(); // True audiovisual preparation, never a poster/scrub image.

private:
    friend class ResidentMediaTest;
    bool ensurePlayer(bool reportFailure = true);
    void releasePlayer();
    void presentPoster();
    void setState(QMediaPlayer::PlaybackState state);
    void setStatus(QMediaPlayer::MediaStatus status);
    void fail(QMediaPlayer::Error error, const QString& message);
    void requestFrame();
    void scheduleScrubFrame();
    bool acceptsIntermediateScrubFrame(qint64 requestedPosition, qint64 requestedAt,
                                       quint64 directionEpoch, qint64 now) const;
    void prefetch();
    void startPreparedPlayback();
    void tick();

    struct PlaybackCursor {
        quint64 generation = 1;
        qint64 anchorMs = 0;
        QElapsedTimer clock;
        SharedMediaFramePtr frame;
        QHash<int, SharedMediaFramePtr> lookahead;
        bool pending = false;
        int pendingIndex = -1;
        quint64 pendingDirectionEpoch = 0;
        bool starting = false;
    } m_cursor;
    std::shared_ptr<const ResidentMediaAsset> m_asset;
    QPointer<QAudioOutput> m_audioOutput;
    QPointer<QVideoSink> m_videoSink;
    QPointer<QObject> m_videoOutput;
    PlaybackAudio m_audio;
    QObject m_entryOwner;
    SharedMediaFramePtr m_entryFrame;
    qint64 m_entryPositionMs = -1;
    quint64 m_entryGeneration = 0;
    QTimer m_positionTimer;
    QTimer m_scrubTimer;
    QElapsedTimer m_scrubClock;
    qint64 m_lastScrubPresentationMs = 0;
    qint64 m_lastScrubDispatchMs = -16;
    int m_scrubDirection = 0;
    quint64 m_scrubDirectionEpoch = 0;
    quint64 m_scrubPresentationEpoch = std::numeric_limits<quint64>::max();
    QTimer m_preparationTimer;
    qint64 m_positionMs = 0;
    int m_loops = QMediaPlayer::Once;
    int m_completedLoops = 0;
    bool m_scrubbing = false;
    bool m_playbackReserved = false;
    bool m_playbackPrepared = false;
    bool m_waitingForMemory = false;
    QMediaPlayer::PlaybackState m_state = QMediaPlayer::StoppedState;
    QMediaPlayer::MediaStatus m_status = QMediaPlayer::NoMedia;
    QMediaPlayer::Error m_error = QMediaPlayer::NoError;
    QString m_errorString;
};
