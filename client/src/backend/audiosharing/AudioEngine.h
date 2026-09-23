#pragma once
#include "SystemAudioCapture.h"
#include "MediaCaptureClock.h"
#include <QObject>

// Capture, encoding and remote monitoring live in the Mouffette process.
// Native capture excludes all application output, including received scenes,
// canvas previews and remote monitoring. Only other applications are published.
class AudioEngine final : public QObject {
    Q_OBJECT
public:
    using CaptureFactory = std::function<std::unique_ptr<SystemAudioCapture>()>;
    static AudioEngine* instance();
    static qint64 nowUs() { return MediaCaptureClock::nowUs(); }
    explicit AudioEngine(QObject* parent = nullptr, CaptureFactory factory = {});
    ~AudioEngine() override;
    void startCapture(const QString& epoch, int bitrateBps = 96000);
    void setCaptureBitrate(int bitrateBps);
    void stopCapture();
    void playPacket(const QString& source, const QString& epoch, quint64 sequence,
                    qint64 timestampUs, const QByteArray& opus, qint64 presentationUs = -1);
    void resetPlayback(const QString& source);
    void setPlaybackMuted(bool muted);
    // Control-thread observation of the actual device callback size.
    qint64 outputQuantumUs() const;
    void shutdown();
signals:
    void packetReady(const QString& epoch, quint64 sequence, qint64 timestampUs, const QByteArray& opus);
    void captureStateChanged(bool active, const QString& error);
    void playbackClock(const QString& source, const QString& epoch, qint64 timestampUs, qint64 localUs);
    void playbackFeedback(const QString& source, const QString& epoch, int droppedPackets, int bufferedMs);
    void playbackFailed(const QString& source, const QString& epoch, const QString& error);
private:
    struct Private;
    std::unique_ptr<Private> d;
};
