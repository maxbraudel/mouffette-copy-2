#pragma once

#include "backend/media/ResidentMediaAsset.h"
#include <QMediaPlayer>
#include <QObject>
#include <QPointer>
#include <QTimer>
#include <memory>

class QAudioOutput;
class QBuffer;
class QVideoSink;


// Streams the original resident MP4 through a read-only memory device. Qt owns
// bounded decode queues, clocks and audio output; no file URL is supplied.
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
    // The decoded image for this cursor, including a hold before the first PTS.
    // Returns an invalid frame while loading, on error, or for another cursor.
    QVideoFrame preparedFrame(qint64 positionMs) const;
    void prepare(qint64 positionMs); // prime a paused native frame asynchronously
    void clearAsset(); // retain the occurrence's cursor for later re-residency
    // Fresh Qt presentation/cache identity, shared immutable CPU planes. Never
    // call toImage() on a long-lived asset frame: Qt caches that RGBA conversion.
    static QVideoFrame presentationFrame(const QVideoFrame& frame);
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
    bool isPlaying() const { return m_state == QMediaPlayer::PlayingState; }
    int loops() const { return m_loops; }
    void setLoops(int loops);
    void setAudioOutput(QAudioOutput* output);
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
    void frameReady(qint64 timestampMs); // decoded frame, never the cached poster

private:
    bool ensurePlayer(bool reportFailure = true);
    void releasePlayer();
    void presentPoster();
    void setState(QMediaPlayer::PlaybackState state);
    void setStatus(QMediaPlayer::MediaStatus status);
    void fail(QMediaPlayer::Error error, const QString& message);
    void watchPreparation();

    std::shared_ptr<const ResidentMediaAsset> m_asset;
    QPointer<QAudioOutput> m_audioOutput;
    QPointer<QVideoSink> m_videoSink;
    QPointer<QObject> m_videoOutput;
    std::unique_ptr<QBuffer> m_source;
    std::unique_ptr<QMediaPlayer> m_player;
    std::unique_ptr<QVideoSink> m_decodeSink;
    QVideoFrame m_frame;
    QTimer m_positionTimer;
    QTimer m_preparationTimer;
    qint64 m_positionMs = 0;
    int m_loops = QMediaPlayer::Once;
    bool m_loading = false;
    bool m_playbackReserved = false;
    bool m_playbackPrepared = false;
    QMediaPlayer::PlaybackState m_requestedState = QMediaPlayer::StoppedState;
    QMediaPlayer::PlaybackState m_state = QMediaPlayer::StoppedState;
    QMediaPlayer::MediaStatus m_status = QMediaPlayer::NoMedia;
    QMediaPlayer::Error m_error = QMediaPlayer::NoError;
    QString m_errorString;
};
