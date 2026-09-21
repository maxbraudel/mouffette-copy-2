#pragma once

#include <QObject>
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
    void setSuspended(bool suspended);
    void refresh();
    void stop();
    QString status() const;

signals:
    void statusChanged();
    void frameReady(const QString& endpoint, int screenId, const QVideoFrame& frame);
    void framesCleared(const QString& endpoint);
    void remoteStatusChanged(const QString& endpoint, const QString& status);

private:
    struct Private;
    std::unique_ptr<Private> d;
};
