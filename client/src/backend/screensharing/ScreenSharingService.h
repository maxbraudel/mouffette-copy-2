#pragma once

#include <QObject>
#include <QJsonArray>
#include <QVideoFrame>
#include <memory>

class SystemMonitor;
class WebSocketClient;

// Ephemeral screen streams; no pixels, grants or decoder state enter projects
// or received-media storage. All network and UI calls stay on the GUI thread.
class ScreenSharingService final : public QObject {
    Q_OBJECT
public:
    ScreenSharingService(WebSocketClient* network, SystemMonitor* monitor,
                         QObject* parent = nullptr);
    ~ScreenSharingService() override;
    void setSharingEnabled(bool enabled);
    void setViewedEndpoint(const QString& endpoint);
    void setViewedScreens(const QJsonArray& screens);
    void setSuspended(bool suspended);
    void setAudioReservationBps(int bitrate);
    void setAudioPlaybackClock(const QString& endpoint, const QString& epoch, qint64 sourceUs, qint64 localUs);
    void clearAudioPlaybackClock(const QString& endpoint);
    void refresh();
    void stop();
    QString status() const;
    bool isRemoteScreenAvailable(const QString& endpoint) const;
    bool isRemoteScreenLoading(const QString& endpoint) const;

signals:
    void sourceBudgetChanged(int totalBps);
    void statusChanged();
    void frameReady(const QString& endpoint, int screenId, const QVideoFrame& frame);
    void sourceTimestampObserved(const QString& endpoint, qint64 sourceUs, qint64 receivedAtUs);
    void frameCleared(const QString& endpoint, int screenId);
    void framesCleared(const QString& endpoint);
    void remoteStateChanged(const QString& endpoint);
    void remoteIssue(const QString& endpoint, const QString& message);

private:
    struct Private;
    std::unique_ptr<Private> d;
};
