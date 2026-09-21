#include "v1_to_v2.h"
#include "backend/runtime/storage/StorageIO.h"

#include <QJsonArray>
#include <QSet>
#include <QStringList>
#include <QUuid>
#include <cmath>

namespace RuntimeStorage::Migrations {
namespace {
Operation invalid() { return {Failure::InvalidData, QStringLiteral("Invalid version 1 notification history")}; }

// Frozen v1 codec: keep legacy phrases exactly as written. In particular, a
// hostname in prose is not a trustworthy endpoint reference and is never parsed.
bool validEntry(const QJsonObject& entry)
{
    for (const auto* key : {"id", "severity", "category", "message", "correlationId",
                            "projectId", "remoteSessionId", "sceneRunId"}) {
        if (!entry.value(QLatin1String(key)).isString()) return false;
    }
    const QString id = entry.value("id").toString().trimmed();
    const QUuid uuid(id);
    const double timestamp = entry.value("timestampMs").toDouble(-1);
    const QString severity = entry.value("severity").toString();
    return !uuid.isNull()
        && uuid.toString(QUuid::WithoutBraces).compare(id, Qt::CaseInsensitive) == 0
        && entry.value("timestampMs").isDouble() && std::isfinite(timestamp)
        && timestamp >= 0 && timestamp <= 9007199254740991.0 && std::floor(timestamp) == timestamp
        && QStringList{"success", "error", "warning", "info", "loading"}.contains(severity)
        && !entry.value("message").toString().trimmed().isEmpty()
        && entry.value("read").isBool() && entry.value("terminal").isBool()
        && (!entry.value("terminal").toBool() || !entry.value("correlationId").toString().isEmpty());
}
}

Operation historyV1ToV2(const QString& root, const QString& path)
{
    QJsonObject document;
    const auto inspected = readVersionedJson(root, path, 1, 64 * 1024 * 1024, &document);
    if (inspected.state == State::IoError) return {Failure::IoError, inspected.reason};
    if (inspected.state != State::Current || !document.value("entries").isArray()
        || !document.value("terminalCorrelationIds").isArray()) return invalid();

    QSet<QString> terminalIds;
    for (const auto& value : document.value("terminalCorrelationIds").toArray()) {
        const QString id = value.toString().trimmed();
        if (!value.isString() || id.isEmpty() || terminalIds.contains(id)) return invalid();
        terminalIds.insert(id);
    }
    QSet<QString> entryIds;
    QJsonArray entries;
    for (const auto& value : document.value("entries").toArray()) {
        if (!value.isObject() || !validEntry(value.toObject())) return invalid();
        auto entry = value.toObject();
        const QString id = entry.value("id").toString().trimmed();
        if (entryIds.contains(id)
            || (entry.value("terminal").toBool()
                && !terminalIds.contains(entry.value("correlationId").toString()))) return invalid();
        entryIds.insert(id);
        entry.insert("peers", QJsonArray{});
        entries.append(entry);
    }
    document.insert("entries", entries);
    document.insert("schemaVersion", 2);
    return writeJson(root, path, document);
}
}
