#pragma once

#include <QObject>
#include <QByteArray>
#include <memory>

class WebSocketClient;

// Authenticated ephemeral audio. All methods/signals run on the GUI thread;
// capture, codec, and output devices belong to the in-process AudioEngine.
class AudioTransport final : public QObject {
    Q_OBJECT
public:
    explicit AudioTransport(WebSocketClient* network, QObject* parent = nullptr);
    ~AudioTransport() override;
    void setSharingEnabled(bool enabled);
    void setSubscription(const QString& remoteSessionId, quint64 generation, bool enabled);
    void setSuspended(bool suspended);
    void setSourceBudget(int totalBps);
    // Video and audio use the same source clock; receipt uses MediaCaptureClock.
    void observeVideoTimestamp(qint64 sourceUs, qint64 receivedAtUs);
    qint64 playbackTimeUs(qint64 sourceUs) const;
    void setOutputQuantumUs(qint64 quantumUs);
    // Preserve capture sequence gaps through admission and the relay so
    // the receiver can conceal loss and report it. Zero allocates locally.
    bool sendPacket(const QByteArray& opus, qint64 timestampUs, quint64 captureSequence = 0);
    void sendStatus(const QString& reason);
    void sendPlaybackFeedback(int droppedPackets, int bufferedMs);
    // A replacement capture session loses codec and capture sequence state.
    // Rotate its publication so old packets cannot alias that new incarnation.
    void restartPublication();
    void stop();
    bool isSupported() const;
    bool isPublishing() const;
    int audioBitrateBps() const;
    int reservedSourceBps() const;
    QString publicationId() const;
    QString viewerStreamId() const;

signals:
    void publicationChanged(bool enabled, int bitrateBps);
    void playbackStreamChanged(const QString& streamId);
    void packetReceived(const QByteArray& opus, qint64 timestampUs, quint64 sequence);
    void remoteStateChanged(const QString& reason);
    void sourceReservationChanged(int bitsPerSecond);
    void issue(const QString& message);
    void publicationIssue(const QString& message);

private:
    struct Private;
    std::unique_ptr<Private> d;
};
