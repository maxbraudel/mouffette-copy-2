#include "network/WatchManager.h"
#include "network/WebSocketClient.h"
#include <QDebug>

WatchManager::WatchManager(QObject* parent) : QObject(parent) {}

void WatchManager::setWebSocketClient(WebSocketClient* ws) {
    if (m_ws == ws) return;
    if (m_ws) {
        disconnect(m_ws, nullptr, this, nullptr);
    }
    m_ws = ws;
    if (m_ws) {
        connect(m_ws, &WebSocketClient::watchStatusChanged, this, &WatchManager::onWatchStatusChanged);
    }
}

void WatchManager::toggleWatch(const QString& targetClientId) {
    if (!m_ws || !m_ws->isConnected()) {
        qWarning() << "WatchManager: Not connected";
        return;
    }
    if (m_watchedClientId == targetClientId && !m_watchedClientId.isEmpty()) {
        stopWatch();
    } else {
        startWatch(targetClientId);
    }
}

void WatchManager::unwatchIfAny() {
    if (!m_watchedClientId.isEmpty()) stopWatch();
}

void WatchManager::startWatch(const QString& targetClientId) {
    if (!m_ws || !m_ws->isConnected()) return;
    if (!m_watchedClientId.isEmpty() && m_watchedClientId == targetClientId) return; // already
    if (!m_watchedClientId.isEmpty()) stopWatch(); // switch
    m_watchedClientId = targetClientId;
    m_ws->watchScreens(targetClientId);
    emit watchStarted(targetClientId);
    emit watchStatusChanged(true, targetClientId);
}

void WatchManager::stopWatch() {
    if (!m_ws || !m_ws->isConnected()) return;
    if (m_watchedClientId.isEmpty()) return;
    const QString prev = m_watchedClientId;
    m_ws->unwatchScreens(m_watchedClientId);
    m_watchedClientId.clear();
    emit watchStopped(prev);
    emit watchStatusChanged(false, QString());
}

void WatchManager::onWatchStatusChanged(bool watched) {
    // This signal indicates whether THIS local client is being watched by someone else.
    emit localWatchedStateChanged(watched);
}
