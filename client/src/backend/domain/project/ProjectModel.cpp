#include "backend/domain/project/ProjectModel.h"

#include <QJsonValue>
#include <QSet>

#include <cmath>

namespace {
constexpr auto kVisible = "visible";
constexpr auto kHidden = "hidden";
constexpr auto kDeleted = "deleted";

qint64 jsonInteger(const QJsonObject& json, const char* key, qint64 fallback = -1)
{
    const QJsonValue value = json.value(QLatin1String(key));
    if (!value.isDouble()) {
        return fallback;
    }
    const double raw = value.toDouble();
    constexpr double maximumSafeJsonInteger = 9007199254740991.0;
    if (!std::isfinite(raw) || raw < 0.0 || raw > maximumSafeJsonInteger
        || std::floor(raw) != raw) {
        return fallback;
    }
    return static_cast<qint64>(raw);
}

QJsonValue sanitizeDurableValue(const QJsonValue& value)
{
    static const QSet<QString> transientKeys = {
        QStringLiteral("canvassessionid"),
        QStringLiteral("remotesessionid"),
        QStringLiteral("scenerunid"),
        QStringLiteral("scenerunstate"),
        QStringLiteral("sceneactive"),
        QStringLiteral("uploadid"),
        QStringLiteral("uploadstatus"),
        QStringLiteral("uploadstate"),
        QStringLiteral("uploadprogress"),
        QStringLiteral("uploaded"),
        QStringLiteral("uploadedbytes"),
        QStringLiteral("uploadbytes"),
        QStringLiteral("connectiongeneration"),
        QStringLiteral("serverbootid"),
        QStringLiteral("knownremotefileids"),
        QStringLiteral("remotefilespresent"),
        QStringLiteral("remotefileid"),
        QStringLiteral("resumetoken"),
        QStringLiteral("uploadchanneltoken"),
        QStringLiteral("authtoken"),
        QStringLiteral("token"),
        QStringLiteral("playing"),
        QStringLiteral("isplaying"),
        QStringLiteral("playbackstate")
    };

    if (value.isArray()) {
        QJsonArray result;
        for (const QJsonValue& child : value.toArray()) {
            result.append(sanitizeDurableValue(child));
        }
        return result;
    }
    if (!value.isObject()) {
        return value;
    }

    QJsonObject result;
    const QJsonObject object = value.toObject();
    for (auto it = object.constBegin(); it != object.constEnd(); ++it) {
        if (!transientKeys.contains(it.key().toLower())) {
            result.insert(it.key(), sanitizeDurableValue(it.value()));
        }
    }
    return result;
}

QJsonValue prepareRestoredValue(const QJsonValue& value)
{
    if (value.isArray()) {
        QJsonArray result;
        for (const QJsonValue& child : value.toArray()) {
            result.append(prepareRestoredValue(child));
        }
        return result;
    }
    if (!value.isObject()) {
        return value;
    }

    QJsonObject result;
    const QJsonObject object = value.toObject();
    for (auto it = object.constBegin(); it != object.constEnd(); ++it) {
        result.insert(it.key(), prepareRestoredValue(it.value()));
    }

    const QString type = result.value(QStringLiteral("type")).toString().toLower();
    if (type == QStringLiteral("video")) {
        result.insert(QStringLiteral("playing"), false);
        result.insert(QStringLiteral("playbackState"), QStringLiteral("paused"));
    }
    if (type == QStringLiteral("video") || type == QStringLiteral("image")) {
        result.insert(QStringLiteral("uploadStatus"), QStringLiteral("not_uploaded"));
    }
    return result;
}

void setError(QString* error, const QString& value)
{
    if (error) {
        *error = value;
    }
}
}

QString projectLifecycleStateToString(ProjectLifecycleState state)
{
    switch (state) {
    case ProjectLifecycleState::Visible: return QString::fromLatin1(kVisible);
    case ProjectLifecycleState::Hidden: return QString::fromLatin1(kHidden);
    case ProjectLifecycleState::Deleted: return QString::fromLatin1(kDeleted);
    }
    return QString::fromLatin1(kDeleted);
}

bool projectLifecycleStateFromString(const QString& value, ProjectLifecycleState* state)
{
    if (!state) {
        return false;
    }
    if (value == QLatin1String(kVisible)) {
        *state = ProjectLifecycleState::Visible;
        return true;
    }
    if (value == QLatin1String(kHidden)) {
        *state = ProjectLifecycleState::Hidden;
        return true;
    }
    if (value == QLatin1String(kDeleted)) {
        *state = ProjectLifecycleState::Deleted;
        return true;
    }
    return false;
}

bool ClientSnapshot::isValid() const
{
    return !deviceId.trimmed().isEmpty();
}

QJsonObject ClientSnapshot::toJson() const
{
    QJsonArray serializedScreens;
    for (const ScreenInfo& screen : screens) {
        serializedScreens.append(screen.toJson());
    }

    QJsonObject json;
    json.insert(QStringLiteral("deviceId"), deviceId);
    json.insert(QStringLiteral("machineName"), machineName);
    json.insert(QStringLiteral("platform"), platform);
    json.insert(QStringLiteral("status"), status);
    json.insert(QStringLiteral("volumePercent"), volumePercent);
    json.insert(QStringLiteral("lastSeenAtMs"), static_cast<double>(lastSeenAtMs));
    json.insert(QStringLiteral("screens"), serializedScreens);
    return json;
}

bool ClientSnapshot::fromJson(const QJsonObject& json, ClientSnapshot* snapshot, QString* error)
{
    if (!snapshot) {
        setError(error, QStringLiteral("Missing ClientSnapshot output"));
        return false;
    }

    ClientSnapshot parsed;
    parsed.deviceId = json.value(QStringLiteral("deviceId")).toString().trimmed();
    // Socket/connection identifiers are runtime-only and are intentionally
    // never restored into a durable Project.
    parsed.serverConnectionId.clear();
    parsed.machineName = json.value(QStringLiteral("machineName")).toString();
    parsed.platform = json.value(QStringLiteral("platform")).toString();
    parsed.status = json.value(QStringLiteral("status")).toString();
    parsed.volumePercent = json.value(QStringLiteral("volumePercent")).toInt(-1);
    parsed.lastSeenAtMs = jsonInteger(json, "lastSeenAtMs");

    const QJsonValue screensValue = json.value(QStringLiteral("screens"));
    if (!screensValue.isUndefined() && !screensValue.isArray()) {
        setError(error, QStringLiteral("ClientSnapshot.screens must be an array"));
        return false;
    }
    for (const QJsonValue& value : screensValue.toArray()) {
        if (!value.isObject()) {
            setError(error, QStringLiteral("ClientSnapshot contains an invalid screen"));
            return false;
        }
        parsed.screens.append(ScreenInfo::fromJson(value.toObject()));
    }

    if (!parsed.isValid()) {
        setError(error, QStringLiteral("ClientSnapshot.deviceId is required"));
        return false;
    }
    *snapshot = parsed;
    return true;
}

ClientSnapshot ClientSnapshot::fromClientInfo(const ClientInfo& client, qint64 seenAtMs)
{
    ClientSnapshot snapshot;
    snapshot.deviceId = client.clientId().trimmed();
    snapshot.serverConnectionId = client.getId();
    snapshot.machineName = client.getMachineName();
    snapshot.platform = client.getPlatform();
    snapshot.status = client.getStatus();
    snapshot.screens = client.getScreens();
    snapshot.volumePercent = client.getVolumePercent();
    snapshot.lastSeenAtMs = seenAtMs;
    return snapshot;
}

ClientInfo ClientSnapshot::toClientInfo(bool online) const
{
    ClientInfo client(serverConnectionId.isEmpty() ? deviceId : serverConnectionId,
                      machineName,
                      platform);
    client.setClientId(deviceId);
    client.setStatus(online ? status : QStringLiteral("offline"));
    client.setScreens(screens);
    client.setVolumePercent(volumePercent);
    client.setOnline(online);
    client.setFromMemory(!online);
    return client;
}

QJsonObject ProjectMediaReference::toJson() const
{
    return {
        {QStringLiteral("mediaId"), mediaId},
        {QStringLiteral("assetId"), assetId},
        {QStringLiteral("canonicalSourcePath"), canonicalSourcePath},
        {QStringLiteral("sourceIdentity"), sourceIdentity},
        {QStringLiteral("sha256"), sha256},
        {QStringLiteral("mediaType"), mediaType}
    };
}

bool ProjectMediaReference::fromJson(const QJsonObject& json,
                                     ProjectMediaReference* reference,
                                     QString* error)
{
    if (!reference) {
        setError(error, QStringLiteral("Missing ProjectMediaReference output"));
        return false;
    }
    ProjectMediaReference parsed;
    parsed.mediaId = json.value(QStringLiteral("mediaId")).toString();
    parsed.assetId = json.value(QStringLiteral("assetId")).toString();
    parsed.canonicalSourcePath = json.value(QStringLiteral("canonicalSourcePath")).toString();
    parsed.sourceIdentity = json.value(QStringLiteral("sourceIdentity")).toString();
    parsed.sha256 = json.value(QStringLiteral("sha256")).toString();
    parsed.mediaType = json.value(QStringLiteral("mediaType")).toString();
    if (parsed.mediaId.isEmpty()) {
        setError(error, QStringLiteral("Project media reference is missing mediaId"));
        return false;
    }
    *reference = parsed;
    return true;
}

bool ProjectRecord::isValid() const
{
    return !projectId.trimmed().isEmpty()
        && !targetDeviceId.trimmed().isEmpty()
        && clientSnapshot.isValid()
        && clientSnapshot.deviceId == targetDeviceId
        && state != ProjectLifecycleState::Deleted
        && createdAtMs >= 0
        && updatedAtMs >= 0;
}

QJsonObject ProjectRecord::toJson() const
{
    QJsonArray references;
    for (const ProjectMediaReference& reference : mediaReferences) {
        references.append(reference.toJson());
    }

    QJsonObject json;
    json.insert(QStringLiteral("projectId"), projectId);
    json.insert(QStringLiteral("targetDeviceId"), targetDeviceId);
    json.insert(QStringLiteral("clientSnapshot"), clientSnapshot.toJson());
    json.insert(QStringLiteral("state"), projectLifecycleStateToString(state));
    json.insert(QStringLiteral("createdAtMs"), static_cast<double>(createdAtMs));
    json.insert(QStringLiteral("updatedAtMs"), static_cast<double>(updatedAtMs));
    json.insert(QStringLiteral("hiddenAtMs"), static_cast<double>(hiddenAtMs));
    json.insert(QStringLiteral("lastCheckpointAtMs"), static_cast<double>(lastCheckpointAtMs));
    json.insert(QStringLiteral("canvasState"), sanitizeDurableValue(canvasState).toObject());
    json.insert(QStringLiteral("mediaReferences"), references);
    return json;
}

bool ProjectRecord::fromJson(const QJsonObject& json, ProjectRecord* project, QString* error)
{
    if (!project) {
        setError(error, QStringLiteral("Missing ProjectRecord output"));
        return false;
    }

    ProjectRecord parsed;
    parsed.projectId = json.value(QStringLiteral("projectId")).toString().trimmed();
    parsed.targetDeviceId = json.value(QStringLiteral("targetDeviceId")).toString().trimmed();
    parsed.createdAtMs = jsonInteger(json, "createdAtMs");
    parsed.updatedAtMs = jsonInteger(json, "updatedAtMs");
    parsed.hiddenAtMs = jsonInteger(json, "hiddenAtMs");
    parsed.lastCheckpointAtMs = jsonInteger(json, "lastCheckpointAtMs");
    if (!projectLifecycleStateFromString(json.value(QStringLiteral("state")).toString(), &parsed.state)) {
        setError(error, QStringLiteral("Project contains an invalid lifecycle state"));
        return false;
    }
    if (!json.value(QStringLiteral("clientSnapshot")).isObject()
        || !ClientSnapshot::fromJson(json.value(QStringLiteral("clientSnapshot")).toObject(),
                                     &parsed.clientSnapshot,
                                     error)) {
        return false;
    }
    if (!json.value(QStringLiteral("canvasState")).isUndefined()
        && !json.value(QStringLiteral("canvasState")).isObject()) {
        setError(error, QStringLiteral("Project.canvasState must be an object"));
        return false;
    }
    parsed.canvasState = sanitizeDurableValue(json.value(QStringLiteral("canvasState"))).toObject();

    const QJsonValue referencesValue = json.value(QStringLiteral("mediaReferences"));
    if (!referencesValue.isUndefined() && !referencesValue.isArray()) {
        setError(error, QStringLiteral("Project.mediaReferences must be an array"));
        return false;
    }
    for (const QJsonValue& value : referencesValue.toArray()) {
        ProjectMediaReference reference;
        if (!value.isObject() || !ProjectMediaReference::fromJson(value.toObject(), &reference, error)) {
            return false;
        }
        parsed.mediaReferences.append(reference);
    }

    if (!parsed.isValid()) {
        setError(error, QStringLiteral("Project record is incomplete or inconsistent"));
        return false;
    }
    *project = parsed;
    return true;
}

QJsonObject ProjectRecord::canvasStateForRestore() const
{
    return prepareRestoredValue(sanitizeDurableValue(canvasState)).toObject();
}
