#include "backend/notifications/HistoryStore.h"
#include "backend/runtime/RuntimeProfile.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QSaveFile>
#include <QSet>
#include <QUuid>
#include <cmath>
#include <utility>

namespace {
qint64 jsonInteger(const QJsonObject& json, const char* key, qint64 fallback = -1)
{
    const QJsonValue value = json.value(QLatin1String(key));
    if (!value.isDouble()) return fallback;
    const double raw = value.toDouble();
    constexpr double maximumSafeJsonInteger = 9007199254740991.0;
    if (!std::isfinite(raw) || raw < 0.0 || raw > maximumSafeJsonInteger
        || std::floor(raw) != raw) {
        return fallback;
    }
    return static_cast<qint64>(raw);
}

void setError(QString* error, const QString& value)
{
    if (error) {
        *error = value;
    }
}

bool isCanonicalUuid(const QString& value)
{
    const QUuid uuid(value);
    return !uuid.isNull()
        && uuid.toString(QUuid::WithoutBraces)
               .compare(value, Qt::CaseInsensitive) == 0;
}

bool isExactSchemaVersion(const QJsonValue& value)
{
    return value.isDouble()
        && value.toDouble(-1.0) == static_cast<double>(HistoryStore::SchemaVersion);
}
}

QString notificationSeverityToString(NotificationSeverity severity)
{
    switch (severity) {
    case NotificationSeverity::Success: return QStringLiteral("success");
    case NotificationSeverity::Error: return QStringLiteral("error");
    case NotificationSeverity::Warning: return QStringLiteral("warning");
    case NotificationSeverity::Info: return QStringLiteral("info");
    case NotificationSeverity::Loading: return QStringLiteral("loading");
    }
    return QStringLiteral("info");
}

bool notificationSeverityFromString(const QString& value, NotificationSeverity* severity)
{
    if (!severity) {
        return false;
    }
    if (value == QStringLiteral("success")) *severity = NotificationSeverity::Success;
    else if (value == QStringLiteral("error")) *severity = NotificationSeverity::Error;
    else if (value == QStringLiteral("warning")) *severity = NotificationSeverity::Warning;
    else if (value == QStringLiteral("info")) *severity = NotificationSeverity::Info;
    else if (value == QStringLiteral("loading")) *severity = NotificationSeverity::Loading;
    else return false;
    return true;
}

bool NotificationEntry::isValid() const
{
    return isCanonicalUuid(id) && timestampMs >= 0 && !message.trimmed().isEmpty();
}

QJsonObject NotificationEntry::toJson() const
{
    QJsonObject json;
    json.insert(QStringLiteral("id"), id);
    json.insert(QStringLiteral("timestampMs"), static_cast<double>(timestampMs));
    json.insert(QStringLiteral("severity"), notificationSeverityToString(severity));
    json.insert(QStringLiteral("category"), category);
    json.insert(QStringLiteral("message"), message);
    json.insert(QStringLiteral("read"), read);
    json.insert(QStringLiteral("correlationId"), correlationId);
    json.insert(QStringLiteral("projectId"), projectId);
    json.insert(QStringLiteral("remoteSessionId"), remoteSessionId);
    json.insert(QStringLiteral("sceneRunId"), sceneRunId);
    json.insert(QStringLiteral("terminal"), terminal);
    return json;
}

bool NotificationEntry::fromJson(const QJsonObject& json, NotificationEntry* entry, QString* error)
{
    if (!entry) {
        setError(error, QStringLiteral("Missing NotificationEntry output"));
        return false;
    }
    NotificationEntry parsed;
    if (!json.value(QStringLiteral("id")).isString()
        || !json.value(QStringLiteral("severity")).isString()
        || !json.value(QStringLiteral("category")).isString()
        || !json.value(QStringLiteral("message")).isString()
        || !json.value(QStringLiteral("read")).isBool()
        || !json.value(QStringLiteral("correlationId")).isString()
        || !json.value(QStringLiteral("projectId")).isString()
        || !json.value(QStringLiteral("remoteSessionId")).isString()
        || !json.value(QStringLiteral("sceneRunId")).isString()
        || !json.value(QStringLiteral("terminal")).isBool()) {
        setError(error, QStringLiteral("Notification contains invalid field types"));
        return false;
    }
    parsed.id = json.value(QStringLiteral("id")).toString().trimmed();
    parsed.timestampMs = jsonInteger(json, "timestampMs");
    if (!notificationSeverityFromString(json.value(QStringLiteral("severity")).toString(),
                                        &parsed.severity)) {
        setError(error, QStringLiteral("Notification contains an invalid severity"));
        return false;
    }
    parsed.category = json.value(QStringLiteral("category")).toString();
    parsed.message = json.value(QStringLiteral("message")).toString();
    parsed.read = json.value(QStringLiteral("read")).toBool(false);
    parsed.correlationId = json.value(QStringLiteral("correlationId")).toString();
    parsed.projectId = json.value(QStringLiteral("projectId")).toString();
    parsed.remoteSessionId = json.value(QStringLiteral("remoteSessionId")).toString();
    parsed.sceneRunId = json.value(QStringLiteral("sceneRunId")).toString();
    parsed.terminal = json.value(QStringLiteral("terminal")).toBool(false);
    if (!parsed.isValid()) {
        setError(error, QStringLiteral("Notification record is incomplete"));
        return false;
    }
    if (parsed.terminal && parsed.correlationId.isEmpty()) {
        setError(error, QStringLiteral("Terminal notification requires a correlationId"));
        return false;
    }
    *entry = parsed;
    return true;
}

HistoryStore::HistoryStore(QString filePath)
    : m_filePath(std::move(filePath))
{
}

QString HistoryStore::defaultFilePath()
{
    return QDir(RuntimeProfile::appDataLocation())
        .filePath(QStringLiteral("notification-history-v1.json"));
}

bool HistoryStore::load(NotificationHistoryData* history)
{
    m_lastError.clear();
    if (!history) {
        m_lastError = QStringLiteral("Missing notification history output");
        return false;
    }
    *history = {};

    QFile file(m_filePath);
    if (!file.exists()) {
        return true;
    }
    if (!file.open(QIODevice::ReadOnly)) {
        m_lastError = QStringLiteral("Cannot open notification history: %1").arg(file.errorString());
        return false;
    }
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(file.readAll(), &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
        m_lastError = QStringLiteral("Invalid notification history JSON: %1").arg(parseError.errorString());
        return false;
    }
    const QJsonObject root = document.object();
    if (!isExactSchemaVersion(root.value(QStringLiteral("schemaVersion")))) {
        m_lastError = QStringLiteral("Unsupported notification history schema version");
        return false;
    }
    if (!root.value(QStringLiteral("entries")).isArray()
        || !root.value(QStringLiteral("terminalCorrelationIds")).isArray()) {
        m_lastError = QStringLiteral("Notification history arrays are missing");
        return false;
    }

    NotificationHistoryData parsed;
    QSet<QString> notificationIds;
    for (const QJsonValue& value : root.value(QStringLiteral("entries")).toArray()) {
        NotificationEntry entry;
        QString error;
        if (!value.isObject() || !NotificationEntry::fromJson(value.toObject(), &entry, &error)) {
            m_lastError = QStringLiteral("Invalid notification: %1").arg(error);
            return false;
        }
        if (notificationIds.contains(entry.id)) {
            m_lastError = QStringLiteral("Notification history contains a duplicate id");
            return false;
        }
        notificationIds.insert(entry.id);
        parsed.entries.append(entry);
        if (parsed.entries.size() >= MaximumEntries) {
            break;
        }
    }

    QSet<QString> terminalIds;
    for (const QJsonValue& value : root.value(QStringLiteral("terminalCorrelationIds")).toArray()) {
        if (!value.isString()) {
            m_lastError = QStringLiteral(
                "Notification history contains an invalid terminal correlation id");
            return false;
        }
        const QString correlationId = value.toString().trimmed();
        if (correlationId.isEmpty() || terminalIds.contains(correlationId)) {
            m_lastError = QStringLiteral(
                "Notification history contains an empty or duplicate terminal correlation id");
            return false;
        }
        terminalIds.insert(correlationId);
        parsed.terminalCorrelationIds.append(correlationId);
    }
    while (parsed.terminalCorrelationIds.size() > MaximumTerminalCorrelations) {
        terminalIds.remove(parsed.terminalCorrelationIds.takeFirst());
    }
    for (const NotificationEntry& entry : std::as_const(parsed.entries)) {
        if (entry.terminal && !terminalIds.contains(entry.correlationId)) {
            m_lastError = QStringLiteral(
                "Terminal notification is missing its replay tombstone");
            return false;
        }
    }

    *history = parsed;
    return true;
}

bool HistoryStore::save(const NotificationHistoryData& history)
{
    m_lastError.clear();
    QJsonArray entries;
    QSet<QString> notificationIds;
    int count = 0;
    for (const NotificationEntry& entry : history.entries) {
        if (count++ >= MaximumEntries) {
            break;
        }
        if (!entry.isValid() || notificationIds.contains(entry.id)
            || (entry.terminal && entry.correlationId.isEmpty())) {
            m_lastError = QStringLiteral("Refusing to persist invalid notification data");
            return false;
        }
        notificationIds.insert(entry.id);
        entries.append(entry.toJson());
    }

    QJsonArray terminalIds;
    QSet<QString> seenTerminalIds;
    const int first = qMax(0, history.terminalCorrelationIds.size() - MaximumTerminalCorrelations);
    for (int index = first; index < history.terminalCorrelationIds.size(); ++index) {
        const QString id = history.terminalCorrelationIds.at(index).trimmed();
        if (!id.isEmpty() && !seenTerminalIds.contains(id)) {
            seenTerminalIds.insert(id);
            terminalIds.append(id);
        }
    }

    const QFileInfo info(m_filePath);
    QDir directory = info.dir();
    if (!directory.exists() && !directory.mkpath(QStringLiteral("."))) {
        m_lastError = QStringLiteral("Cannot create notification history directory");
        return false;
    }

    QJsonObject root;
    root.insert(QStringLiteral("schemaVersion"), SchemaVersion);
    root.insert(QStringLiteral("entries"), entries);
    root.insert(QStringLiteral("terminalCorrelationIds"), terminalIds);

    QSaveFile file(m_filePath);
    file.setDirectWriteFallback(false);
    if (!file.open(QIODevice::WriteOnly)) {
        m_lastError = QStringLiteral("Cannot open notification history for writing: %1").arg(file.errorString());
        return false;
    }
    const QByteArray encoded = QJsonDocument(root).toJson(QJsonDocument::Compact);
    if (file.write(encoded) != encoded.size()) {
        m_lastError = QStringLiteral("Cannot write notification history: %1").arg(file.errorString());
        file.cancelWriting();
        return false;
    }
    if (!file.commit()) {
        m_lastError = QStringLiteral("Cannot atomically commit notification history: %1").arg(file.errorString());
        return false;
    }
    return true;
}
