#include "backend/domain/project/ProjectModel.h"
#include "backend/security/DeviceIdentityStore.h"

#include <QJsonValue>
#include <QSet>

#include <cmath>
#include <limits>

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
        QStringLiteral("residencyready"),
        QStringLiteral("residencystate"),
        QStringLiteral("residencyprogress"),
        QStringLiteral("residencyerror"),
        QStringLiteral("residentframesource"),
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

bool ProjectTargetReference::isValid() const
{
    return !endpointId.trimmed().isEmpty();
}

QJsonObject ProjectTargetReference::toJson() const
{
    QJsonObject result{
        {QStringLiteral("endpointId"), endpointId},
        {QStringLiteral("machineName"), machineName},
        {QStringLiteral("platform"), platform}
    };
    if (instanceOrdinal > 0 && !installationId.isEmpty()
        && instanceId == DeviceIdentityStore::instanceIdForOrdinal(instanceOrdinal)) {
        result.insert(QStringLiteral("installationId"), installationId);
        result.insert(QStringLiteral("instanceId"), instanceId);
        result.insert(QStringLiteral("instanceOrdinal"), instanceOrdinal);
    }
    return result;
}

bool ProjectTargetReference::fromJson(const QJsonObject& json,
                                      ProjectTargetReference* target,
                                      QString* error)
{
    if (!target) {
        setError(error, QStringLiteral("Missing ProjectTargetReference output"));
        return false;
    }

    ProjectTargetReference parsed;
    parsed.endpointId = json.value(QStringLiteral("endpointId")).toString().trimmed();
    parsed.machineName = json.value(QStringLiteral("machineName")).toString();
    parsed.platform = json.value(QStringLiteral("platform")).toString();
    const qint64 ordinal = jsonInteger(json, "instanceOrdinal");
    const QString installationId = json.value(QStringLiteral("installationId")).toString();
    const QString instanceId = json.value(QStringLiteral("instanceId")).toString();
    // The descriptive tuple is optional for existing project files. Never
    // guess a primary instance or discard the user's project when it is absent.
    if (ordinal > 0 && ordinal <= std::numeric_limits<int>::max()
        && !installationId.isEmpty()
        && instanceId == DeviceIdentityStore::instanceIdForOrdinal(static_cast<int>(ordinal))) {
        parsed.installationId = installationId;
        parsed.instanceId = instanceId;
        parsed.instanceOrdinal = static_cast<int>(ordinal);
    }

    if (!parsed.isValid()) {
        setError(error, QStringLiteral("ProjectTargetReference.endpointId is required"));
        return false;
    }
    *target = parsed;
    return true;
}

ProjectTargetReference ProjectTargetReference::fromClientInfo(const ClientInfo& client)
{
    ProjectTargetReference target;
    target.endpointId = client.endpointId().trimmed();
    target.machineName = client.getMachineName();
    target.platform = client.getPlatform();
    if (client.instanceOrdinal() > 0 && !client.installationId().isEmpty()
        && client.instanceId() == DeviceIdentityStore::instanceIdForOrdinal(client.instanceOrdinal())) {
        target.installationId = client.installationId();
        target.instanceId = client.instanceId();
        target.instanceOrdinal = client.instanceOrdinal();
    }
    return target;
}

ClientInfo ProjectTargetReference::toClientInfo(bool online) const
{
    ClientInfo client(endpointId, machineName, platform);
    client.setEndpointId(endpointId);
    client.setInstallationId(installationId);
    client.setInstanceId(instanceId);
    client.setInstanceOrdinal(instanceOrdinal);
    client.setStatus(online ? QStringLiteral("Available")
                            : QStringLiteral("Disconnected"));
    client.setAvailabilityStatus(client.getStatus());
    client.setVolumePercent(-1);
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
        {QStringLiteral("mediaType"), mediaType},
        {QStringLiteral("pendingImport"), pendingImport}
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
    parsed.pendingImport = json.value(QStringLiteral("pendingImport")).toBool(false);
    if (parsed.mediaId.isEmpty()) {
        setError(error, QStringLiteral("Project media reference is missing mediaId"));
        return false;
    }
    *reference = parsed;
    return true;
}

bool ProjectRecord::isValid() const
{
    const bool identityValid = !projectId.trimmed().isEmpty()
        && !targetEndpointId.trimmed().isEmpty()
        && target.isValid()
        && target.endpointId == targetEndpointId
        && state != ProjectLifecycleState::Deleted
        && createdAtMs >= 0
        && updatedAtMs >= 0;
    if (!identityValid || savedVolumePercent < -1 || savedVolumePercent > 100
        || snapshotRevision == 0 || snapshotCapturedAtMs < 1) {
        return false;
    }
    for (const ScreenInfo& screen : savedScreens) {
        if (!screen.isValid()) return false;
    }
    return true;
}

QJsonObject ProjectRecord::toJson() const
{
    QJsonArray references;
    for (const ProjectMediaReference& reference : mediaReferences) {
        references.append(reference.toJson());
    }

    QJsonObject json;
    json.insert(QStringLiteral("projectId"), projectId);
    json.insert(QStringLiteral("targetEndpointId"), targetEndpointId);
    json.insert(QStringLiteral("target"), target.toJson());
    QJsonArray screens;
    for (const ScreenInfo& screen : savedScreens) {
        screens.append(screen.toJson());
    }
    json.insert(QStringLiteral("savedScreens"), screens);
    json.insert(QStringLiteral("savedVolumePercent"), savedVolumePercent);
    json.insert(QStringLiteral("snapshotRevision"),
                static_cast<double>(snapshotRevision));
    json.insert(QStringLiteral("snapshotCapturedAtMs"),
                static_cast<double>(snapshotCapturedAtMs));
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
    parsed.targetEndpointId = json.value(QStringLiteral("targetEndpointId")).toString().trimmed();
    parsed.createdAtMs = jsonInteger(json, "createdAtMs");
    parsed.updatedAtMs = jsonInteger(json, "updatedAtMs");
    parsed.hiddenAtMs = jsonInteger(json, "hiddenAtMs");
    parsed.lastCheckpointAtMs = jsonInteger(json, "lastCheckpointAtMs");
    const qint64 revision = jsonInteger(json, "snapshotRevision", 0);
    parsed.savedVolumePercent =
        json.value(QStringLiteral("savedVolumePercent")).toInt(-2);
    parsed.snapshotRevision = revision > 0
        ? static_cast<quint64>(revision) : 0;
    parsed.snapshotCapturedAtMs =
        jsonInteger(json, "snapshotCapturedAtMs");
    if (parsed.savedVolumePercent < -1 || parsed.savedVolumePercent > 100
        || parsed.snapshotRevision == 0 || parsed.snapshotCapturedAtMs < 1) {
        setError(error, QStringLiteral("Project saved volume is invalid"));
        return false;
    }
    if (!projectLifecycleStateFromString(json.value(QStringLiteral("state")).toString(), &parsed.state)) {
        setError(error, QStringLiteral("Project contains an invalid lifecycle state"));
        return false;
    }
    if (!json.value(QStringLiteral("target")).isObject()
        || !ProjectTargetReference::fromJson(
            json.value(QStringLiteral("target")).toObject(), &parsed.target, error)) {
        return false;
    }
    const QJsonValue screensValue = json.value(QStringLiteral("savedScreens"));
    if (!screensValue.isArray()) {
        setError(error, QStringLiteral("Project.savedScreens must be an array"));
        return false;
    }
    if (screensValue.toArray().size() > 64) {
        setError(error, QStringLiteral("Project has too many saved screens"));
        return false;
    }
    QSet<int> screenIds;
    for (const QJsonValue& value : screensValue.toArray()) {
        if (!value.isObject()) {
            setError(error, QStringLiteral("Project contains an invalid saved screen"));
            return false;
        }
        const ScreenInfo screen = ScreenInfo::fromJson(value.toObject());
        if (!screen.isValid() || screenIds.contains(screen.id)) {
            setError(error, QStringLiteral("Project contains an invalid or duplicate saved screen"));
            return false;
        }
        screenIds.insert(screen.id);
        parsed.savedScreens.append(screen);
    }
    if (!json.value(QStringLiteral("canvasState")).isObject()) {
        setError(error, QStringLiteral("Project.canvasState must be an object"));
        return false;
    }
    const QJsonObject storedCanvasState =
        json.value(QStringLiteral("canvasState")).toObject();
    const QJsonObject sanitizedCanvasState =
        sanitizeDurableValue(storedCanvasState).toObject();
    if (storedCanvasState != sanitizedCanvasState) {
        setError(error, QStringLiteral(
            "Project.canvasState contains transient protocol state"));
        return false;
    }
    parsed.canvasState = sanitizedCanvasState;

    const QJsonValue referencesValue = json.value(QStringLiteral("mediaReferences"));
    if (!referencesValue.isArray()) {
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
    QJsonObject restored =
        prepareRestoredValue(sanitizeDurableValue(canvasState)).toObject();
    QJsonArray screens;
    for (const ScreenInfo& screen : savedScreens) {
        screens.append(screen.toJson());
    }
    restored.insert(QStringLiteral("screens"), screens);
    return restored;
}
