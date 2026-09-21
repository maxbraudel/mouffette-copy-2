#ifndef HISTORYSTORE_H
#define HISTORYSTORE_H

#include <QList>
#include <QJsonObject>
#include <QString>
#include <QStringList>
#include <QVariantList>
#include "backend/runtime/storage/StorageVersions.h"
#include "backend/runtime/storage/StorageUpgradeEngine.h"

enum class NotificationSeverity {
    Success,
    Error,
    Warning,
    Info,
    Loading
};

QString notificationSeverityToString(NotificationSeverity severity);
bool notificationSeverityFromString(const QString& value, NotificationSeverity* severity);

// Durable references contain technical identity only. Usernames and pictures
// are resolved from the current in-memory profile when the UI displays them.
struct NotificationPeer {
    QString endpointId;
    QString machineName;
    int instanceOrdinal = 1;
    QString role;

    bool isValid() const;
    QJsonObject toJson() const;
    static bool fromJson(const QJsonObject& json, NotificationPeer* peer);
};

QVariantList notificationPeersToVariant(const QList<NotificationPeer>& peers);

struct NotificationEntry {
    QString id;
    qint64 timestampMs = -1;
    NotificationSeverity severity = NotificationSeverity::Info;
    QString category;
    QString message;
    bool read = false;
    QString correlationId;
    QString projectId;
    QString remoteSessionId;
    QString sceneRunId;
    bool terminal = false;
    QList<NotificationPeer> peers;

    bool isValid() const;
    QJsonObject toJson() const;
    static bool fromJson(const QJsonObject& json, NotificationEntry* entry, QString* error = nullptr);
};

struct NotificationHistoryData {
    QList<NotificationEntry> entries; // Newest first.
    QStringList terminalCorrelationIds; // Oldest first, used for replay deduplication.
};

/** Atomic, versioned persistence for the last 100 notifications. */
class HistoryStore {
public:
    static constexpr int SchemaVersion = StorageVersions::History;
    static constexpr int MaximumEntries = 100;
    static constexpr int MaximumTerminalCorrelations = 1024;

    explicit HistoryStore(QString filePath = defaultFilePath());

    static QString defaultFilePath();
    QString filePath() const { return m_filePath; }
    QString lastError() const { return m_lastError; }
    RuntimeStorage::Failure lastReadFailure() const { return m_readFailure; }

    bool load(NotificationHistoryData* history);
    bool save(const NotificationHistoryData& history);

private:
    QString m_filePath;
    QString m_lastError;
    RuntimeStorage::Failure m_readFailure = RuntimeStorage::Failure::None;
};

Q_DECLARE_METATYPE(NotificationEntry)
Q_DECLARE_METATYPE(NotificationSeverity)

#endif // HISTORYSTORE_H
