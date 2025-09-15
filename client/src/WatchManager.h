#ifndef WATCHMANAGER_H
#define WATCHMANAGER_H

#include <QObject>
#include <QPointer>
#include <QTimer>
#include <QList>
#include "ClientInfo.h"

class WebSocketClient;

// WatchManager encapsulates logic for starting/stopping a watch session on a target client,
// tracking current watched client id, responding to watch status changes, and periodic
// snapshot/cursor updates while being watched.
class WatchManager : public QObject {
    Q_OBJECT
public:
    explicit WatchManager(QObject* parent = nullptr);

    void setWebSocketClient(WebSocketClient* ws);
    void setLocalClientId(const QString& id) { m_localClientId = id; }

    // Toggle watching a target: if already watching target -> unwatch, else watch new target.
    void toggleWatch(const QString& targetClientId);
    void unwatchIfAny();

    QString watchedClientId() const { return m_watchedClientId; }
    bool isWatching() const { return !m_watchedClientId.isEmpty(); }

signals:
    void watchStarted(const QString& targetClientId);
    void watchStopped(const QString& previousTargetId);
    void watchStatusChanged(bool watching, const QString& targetClientId);

    // Emitted when remote 'watch_status' message indicates this local client is being watched (or not)
    void localWatchedStateChanged(bool watched);

public slots:
    // Forwarded from WebSocketClient watch_status signal
    void onWatchStatusChanged(bool watched);

private:
    void startWatch(const QString& targetClientId);
    void stopWatch();

    QPointer<WebSocketClient> m_ws;
    QString m_watchedClientId;    // target currently being watched by this client
    QString m_localClientId;      // id of this client (used if needed for context)
};

#endif // WATCHMANAGER_H
