#pragma once
#include "backend/audiosharing/AudioPreviewChannel.h"
#include "backend/audiosharing/MediaCaptureClock.h"
#include <QObject>
#include <memory>

// One audio process per application instance. No network sockets, media files,
// or persistent application state are opened by the worker.
class AudioWorkerClient final : public QObject {
    Q_OBJECT
public:
    static AudioWorkerClient* instance();
    static qint64 nowUs() { return MediaCaptureClock::nowUs(); }
    explicit AudioWorkerClient(QObject* parent = nullptr);
    ~AudioWorkerClient() override;
    void startCapture(const QString& epoch, int bitrateBps = 96000);
    void setCaptureBitrate(int bitrateBps);
    void stopCapture();
    void playPacket(const QString& source, const QString& epoch, quint64 sequence,
                    qint64 timestampUs, const QByteArray& opus);
    void resetPlayback(const QString& source);
    void setPlaybackMuted(bool muted);
    std::shared_ptr<AudioPreviewChannel> createPreviewChannel(const QByteArray& deviceId = {});
    void shutdown();
signals:
    void packetReady(const QString& epoch, quint64 sequence, qint64 timestampUs, const QByteArray& opus);
    void captureStateChanged(bool active, const QString& error);
    void playbackClock(const QString& source, const QString& epoch, qint64 timestampUs);
    void playbackFeedback(const QString& source, const QString& epoch, int droppedPackets, int bufferedMs);
    void playbackFailed(const QString& error);
    void failed(const QString& error);
private:
    struct Private;
    std::unique_ptr<Private> d;
    void ensureWorker();
    void readMessages();
    void replayPreviews();
    void removePreview(const QString& key);
    friend class AudioPreviewChannel;
};
