#ifndef NOTIFICATIONCENTER_H
#define NOTIFICATIONCENTER_H

#include "backend/notifications/HistoryStore.h"

#include <QObject>
#include <QSet>
#include <memory>

struct NotificationRequest {
    NotificationSeverity severity = NotificationSeverity::Info;
    QString category = QStringLiteral("General");
    QString message;
    int toastDurationMs = -1;
    QString correlationId;
    QString projectId;
    QString remoteSessionId;
    QString sceneRunId;
    bool terminal = false;
    qint64 timestampMs = -1;
    QList<NotificationPeer> peers;
};

// Keep terminal identities scoped by protocol object. Uploads, SceneRuns and
// teardowns are independent UUID domains and must not suppress one another if
// an identifier is ever reused across those domains.
namespace NotificationCorrelation {
QString upload(const QString& uploadId);
QString sceneRun(const QString& sceneRunId);
QString teardown(const QString& teardownId);
}

/** Single source of truth for user-facing messages and durable history. */
class NotificationCenter final : public QObject {
    Q_OBJECT

public:
    explicit NotificationCenter(QObject* parent = nullptr);
    explicit NotificationCenter(HistoryStore* store, QObject* parent = nullptr);
    ~NotificationCenter() override = default;

    bool load();
    QString publish(const NotificationRequest& request);

    QList<NotificationEntry> entries() const { return m_history.entries; }
    int unreadCount() const;
    bool isHistoryVisible() const { return m_historyVisible; }
    QString lastError() const { return m_lastError; }

    bool setHistoryVisible(bool visible);
    bool markAllRead();
    bool clearHistory();

signals:
    void entryAdded(const NotificationEntry& entry);
    void historyChanged();
    void unreadCountChanged(int count);
    void historyCleared();
    void toastRequested(const NotificationEntry& entry, int durationMs);
    void persistenceError(const QString& message);

private:
    bool persist();
    void rebuildTerminalIndex();
    void emitUnreadIfChanged(int previousUnread);

    std::unique_ptr<HistoryStore> m_ownedStore;
    HistoryStore* m_store = nullptr;
    NotificationHistoryData m_history;
    QSet<QString> m_terminalCorrelations;
    bool m_historyVisible = false;
    QString m_lastError;
};

#endif // NOTIFICATIONCENTER_H
