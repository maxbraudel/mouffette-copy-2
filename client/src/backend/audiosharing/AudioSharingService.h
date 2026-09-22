#pragma once
#include <QObject>
#include <QSet>
#include <QTimer>

class WebSocketClient;
class AudioTransport;
class AudioWorkerClient;

// One ephemeral audio publication and one foreground listening subscription.
// Monitor topology, project serialization and screen visibility are unrelated.
class AudioSharingService final : public QObject {
    Q_OBJECT
public:
    explicit AudioSharingService(WebSocketClient* network, QObject* parent = nullptr);
    ~AudioSharingService() override;
    void setSharingEnabled(bool enabled);
    void setViewedEndpoint(const QString& endpoint);
    void setMuted(bool muted);
    void setSuspended(bool suspended);
    void setSourceBudget(int totalBps);
    void observeVideoTimestamp(const QString& endpoint, qint64 sourceUs, qint64 receivedAtUs);
    void stop();
    QString state() const { return m_state; }
    QString status() const { return m_status; }
signals:
    void stateChanged();
    void statusChanged();
    void sourceReservationChanged(int bps);
    void playbackClock(const QString& endpoint, const QString& epoch, qint64 sourceUs, qint64 localUs);
    void playbackReset(const QString& endpoint);
    void remoteIssue(const QString& endpoint, const QString& message);
private:
    void refresh();
    void clearPlayback();
    void setState(const QString& state);
    void report(const QString& message);
    WebSocketClient* m_network;
    AudioTransport* m_transport;
    AudioWorkerClient* m_worker;
    QTimer m_timer;
    QString m_endpoint, m_session, m_stream, m_captureEpoch;
    QString m_state = QStringLiteral("unavailable"), m_status;
    quint64 m_generation = 0;
    qint64 m_captureRetryAt = 0;
    bool m_muted = false, m_suspended = false, m_enabled = false, m_stopped = false;
    bool m_playbackAllowed = false;
    QSet<QString> m_reported;
};
