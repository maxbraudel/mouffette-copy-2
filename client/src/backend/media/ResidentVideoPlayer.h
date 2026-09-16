#pragma once

#include "backend/media/ResidentMediaAsset.h"
#include <QElapsedTimer>
#include <QMediaPlayer>
#include <QObject>
#include <QPointer>
#include <QTimer>
#include <memory>

class QAudioOutput;
class QAudioSink;
class QVideoSink;
class ResidentPcmDevice;

// QMediaPlayer-shaped presentation layer. No source URL, codec, or file access:
// only a completely decoded, immutable resident asset can be attached.
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
    void clearAsset(); // retain the occurrence's cursor for later re-residency
    // Fresh Qt presentation/cache identity, shared immutable CPU planes. Never
    // call toImage() on a long-lived asset frame: Qt caches that RGBA conversion.
    static QVideoFrame presentationFrame(const QVideoFrame& frame);
    std::shared_ptr<const ResidentMediaAsset> asset() const { return m_asset; }
    qint64 position() const { return m_positionUs / 1000; }
    qint64 duration() const { return m_asset ? (m_asset->durationUs + 999) / 1000 : 0; }
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

private:
    void tick();
    void presentFrame();
    void setState(QMediaPlayer::PlaybackState state);
    void setStatus(QMediaPlayer::MediaStatus status);
    void startAudio();
    void stopAudio();
    void updateAudioVolume();
    void failAudio(const QString& message);

    std::shared_ptr<const ResidentMediaAsset> m_asset;
    QPointer<QAudioOutput> m_audioOutput;
    QPointer<QVideoSink> m_videoSink;
    QPointer<QObject> m_videoOutput;
    std::unique_ptr<ResidentPcmDevice> m_pcm;
    std::unique_ptr<QAudioSink> m_audioSink;
    QTimer m_timer;
    QElapsedTimer m_clock;
    qint64 m_clockOriginUs = 0;
    qint64 m_audioOriginUs = 0;
    qint64 m_positionUs = 0;
    qsizetype m_presentedIndex = -1;
    int m_loops = QMediaPlayer::Once;
    int m_completedLoops = 0;
    QMediaPlayer::PlaybackState m_state = QMediaPlayer::StoppedState;
    QMediaPlayer::MediaStatus m_status = QMediaPlayer::NoMedia;
    QMediaPlayer::Error m_error = QMediaPlayer::NoError;
    QString m_errorString;
};
