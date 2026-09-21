#pragma once

#include <QObject>
#include <QByteArray>
#include <memory>

class WebSocketClient;

// Authenticated ephemeral audio. All methods/signals run on the GUI thread;
// capture, codec, and output devices belong to the caller's audio worker.
class AudioTransport final : public QObject {
    Q_OBJECT
public:
    explicit AudioTransport(WebSocketClient* network, QObject* parent = nullptr);
    ~AudioTransport() override;
    void setSharingEnabled(bool enabled);
    void setSubscription(const QString& remoteSessionId, quint64 generation, bool enabled);
    void setSuspended(bool suspended);
    void setSourceBudget(int totalBps);
    bool sendPacket(const QByteArray& opus, qint64 timestampUs);
    void sendStatus(const QString& reason);
    void sendPlaybackFeedback(int droppedPackets, int bufferedMs);
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

private:
    struct Private;
    std::unique_ptr<Private> d;
};
