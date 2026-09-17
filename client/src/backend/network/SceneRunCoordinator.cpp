#include "backend/network/SceneRunCoordinator.h"
#include "MediaFormatContract.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QJsonDocument>
#include <QRegularExpression>
#include <QSet>
#include <QStringList>
#include <QUuid>

#include <algorithm>
#include <cmath>
#include <utility>

namespace {
constexpr qsizetype kMaximumManifestItems = 256;
constexpr qsizetype kMaximumChecklistItems = 2048;
constexpr qint64 kMaximumAssetBytes = 16LL * 1024 * 1024 * 1024;
constexpr double kMaximumSafeJsonInteger = 9007199254740991.0;

bool readPositiveSafeInteger(const QJsonValue& value, quint64* result)
{
    if (!result || !value.isDouble()) return false;
    const double raw = value.toDouble(-1.0);
    if (!std::isfinite(raw) || raw < 1.0
        || raw > kMaximumSafeJsonInteger || std::floor(raw) != raw) {
        return false;
    }
    *result = static_cast<quint64>(raw);
    return true;
}

bool readNonNegativeSafeInteger(const QJsonValue& value, qint64* result)
{
    if (!result || !value.isDouble()) return false;
    const double raw = value.toDouble(-1.0);
    if (!std::isfinite(raw) || raw < 0.0
        || raw > kMaximumSafeJsonInteger || std::floor(raw) != raw) {
        return false;
    }
    *result = static_cast<qint64>(raw);
    return true;
}

QJsonValue normalizeJson(const QJsonValue& value)
{
    if (value.isArray()) {
        QJsonArray normalized;
        const QJsonArray input = value.toArray();
        for (const QJsonValue& item : input) normalized.append(normalizeJson(item));
        return normalized;
    }
    if (value.isObject()) {
        // QJsonObject stores/serializes keys in deterministic lexical order.
        QJsonObject normalized;
        const QJsonObject input = value.toObject();
        QStringList keys = input.keys();
        std::sort(keys.begin(), keys.end());
        for (const QString& key : keys) normalized.insert(key, normalizeJson(input.value(key)));
        return normalized;
    }
    if (value.isDouble()) {
        const double number = value.toDouble();
        if (!std::isfinite(number)) return QJsonValue();
        if (number == 0.0) return QJsonValue(0);
    }
    return value;
}

QString safeChecklistId(QString value, const QString& fallback)
{
    static const QRegularExpression forbidden(QStringLiteral("[^A-Za-z0-9_-]"));
    value.replace(forbidden, QStringLiteral("_"));
    if (value.isEmpty()) value = fallback;
    return value.left(128);
}
}

SceneRunCoordinator::SceneRunCoordinator(QObject* parent)
    : QObject(parent)
    , m_remoteSessions(new RemoteSessionCoordinator(this))
{
}

void SceneRunCoordinator::setLocalEndpointId(const QString& endpointId)
{
    m_localEndpointId = endpointId;
    m_remoteSessions->setLocalEndpointId(endpointId);
}

void SceneRunCoordinator::setPrepareTimeoutMs(int timeoutMs)
{
    if (timeoutMs >= 1'000 && timeoutMs <= 120'000) m_prepareTimeoutMs = timeoutMs;
}

bool SceneRunCoordinator::upsertSession(const QJsonObject& envelope,
                                        quint64 localConnectionGeneration,
                                        QString* validationError)
{
    if (validationError) validationError->clear();
    const QString type = envelope.value(QStringLiteral("type")).toString();
    const QString remoteSessionId =
        envelope.value(QStringLiteral("remoteSessionId")).toString();
    const QString phase = envelope.value(QStringLiteral("phase")).toString();
    quint64 generation = 0;
    if (!readPositiveSafeInteger(
            envelope.value(QStringLiteral("generation")), &generation)) {
        return false;
    }
    const SessionBinding existing = m_remoteSessions->byId(remoteSessionId);

    if (type == QLatin1String("remote_session_offer")
        || type == QLatin1String("remote_session_opening")) {
        const bool offer = type == QLatin1String("remote_session_offer");
        const bool localRoleMatches = offer
            ? envelope.value(QStringLiteral("targetEndpointId")).toString()
                == m_localEndpointId
            : envelope.value(QStringLiteral("ownerEndpointId")).toString()
                == m_localEndpointId;
        const QString requestId =
            envelope.value(QStringLiteral("requestId")).toString();
        if (generation != 1 || phase != QLatin1String("Opening")
            || !localRoleMatches || !isOpaqueId(requestId)
            || !envelope.value(QStringLiteral("resumeToken")).toString().isEmpty()
            || (!existing.remoteSessionId.isEmpty()
                && (existing.generation != generation
                    || existing.phase != QLatin1String("Opening")))) {
            return false;
        }
    } else if (type == QLatin1String("remote_session_opened")) {
        // Open is generation one only. A byte-for-byte-equivalent replay is
        // idempotent, but it must never be used as an alias for Resume.
        if (generation != 1 || phase != QLatin1String("Active")
            || envelope.value(QStringLiteral("resumeToken")).toString().isEmpty()
            || (!existing.remoteSessionId.isEmpty()
                && existing.generation != generation)) {
            return false;
        }
    } else if (type == QLatin1String("remote_session_resumed")) {
        if ((existing.remoteSessionId.isEmpty() && !envelope.value(QStringLiteral("reconciled")).toBool())
            || generation < existing.generation
            || (phase != QLatin1String("Active")
                && phase != QLatin1String("Grace"))) {
            return false;
        }
    } else if (type == QLatin1String("remote_session_lease_state")) {
        if (existing.remoteSessionId.isEmpty()
            || generation != existing.generation
            || (phase != QLatin1String("Active")
                && phase != QLatin1String("Grace"))) {
            return false;
        }
    } else if (type == QLatin1String("remote_session_terminating")) {
        const bool localIsParty =
            envelope.value(QStringLiteral("targetEndpointId")).toString()
                == m_localEndpointId
            || envelope.value(QStringLiteral("ownerEndpointId")).toString()
                == m_localEndpointId;
        const bool recoveryTeardown = existing.remoteSessionId.isEmpty()
            && localIsParty
            && envelope.value(QStringLiteral("resumeToken")).toString().isEmpty()
            && isOpaqueId(
                envelope.value(QStringLiteral("teardownId")).toString());
        if ((!recoveryTeardown && existing.remoteSessionId.isEmpty())
            || (!existing.remoteSessionId.isEmpty()
                && generation < existing.generation)
            || (phase != QLatin1String("Terminating")
                && phase != QLatin1String("CleanupPending"))) {
            return false;
        }
    } else {
        return false;
    }

    if (!m_remoteSessions->upsert(envelope, localConnectionGeneration, validationError)) return false;
    const SessionBinding binding = m_remoteSessions->byId(
        remoteSessionId);
    for (auto it = m_runsById.begin(); it != m_runsById.end(); ++it) {
        if (it->remoteSessionId == binding.remoteSessionId && it->generation <= binding.generation) {
            it->generation = binding.generation;
        }
    }
    return true;
}

bool SceneRunCoordinator::removeSession(const QJsonObject& envelope,
                                        quint64 localConnectionGeneration)
{
    if (envelope.value(QStringLiteral("type")).toString()
            != QLatin1String("remote_session_closed")
        || envelope.value(QStringLiteral("cleanupState")).toString()
            != QLatin1String("confirmed")
        || !m_remoteSessions->canClose(envelope, localConnectionGeneration)) {
        return false;
    }
    const QString remoteSessionId =
        envelope.value(QStringLiteral("remoteSessionId")).toString();
    m_remoteSessions->remove(remoteSessionId, envelope);
    for (auto it = m_runsById.begin(); it != m_runsById.end();) {
        if (it->remoteSessionId == remoteSessionId) it = m_runsById.erase(it);
        else ++it;
    }
    return true;
}

bool SceneRunCoordinator::discardSessionAfterAuthoritativeRejection(
    const QString& remoteSessionId)
{
    if (remoteSessionId.isEmpty()
        || m_remoteSessions->byId(remoteSessionId).remoteSessionId.isEmpty()) {
        return false;
    }
    m_remoteSessions->remove(remoteSessionId);
    for (auto it = m_runsById.begin(); it != m_runsById.end();) {
        if (it->remoteSessionId == remoteSessionId) {
            it = m_runsById.erase(it);
        } else {
            ++it;
        }
    }
    return true;
}

void SceneRunCoordinator::clearSessions()
{
    m_remoteSessions->clear();
    m_runsById.clear();
    m_finishedRunOrder.clear();
}

SceneRunCoordinator::SessionBinding SceneRunCoordinator::sessionForPeer(
    const QString& peerEndpointId) const
{
    return m_remoteSessions->forPeer(peerEndpointId);
}

SceneRunCoordinator::SessionBinding SceneRunCoordinator::sessionById(
    const QString& remoteSessionId) const
{
    return m_remoteSessions->byId(remoteSessionId);
}

QList<SceneRunCoordinator::SessionBinding> SceneRunCoordinator::sessions() const
{
    return m_remoteSessions->all();
}

SceneRunCoordinator::Run SceneRunCoordinator::run(const QString& sceneRunId) const
{
    return m_runsById.value(sceneRunId);
}

bool SceneRunCoordinator::createOutgoingRun(const QString& peerEndpointId,
                                            quint64 revision,
                                            const QJsonArray& manifest,
                                            const QJsonObject& scene,
                                            Run* result,
                                            QString* errorMessage)
{
    if (m_prepareTimeoutMs <= 0) {
        if (errorMessage) {
            *errorMessage = QStringLiteral(
                "Server scene preparation policy is unavailable");
        }
        return false;
    }
    const SessionBinding binding = m_remoteSessions->outgoingForPeer(peerEndpointId);
    if (!binding.active || !binding.commandReady || m_localEndpointId != binding.ownerEndpointId) {
        if (errorMessage) *errorMessage = QStringLiteral("No active outgoing remote session for this device");
        return false;
    }
    if (revision < 1
        || static_cast<double>(revision) > kMaximumSafeJsonInteger
        || !scene.value(QStringLiteral("screens")).isArray()
        || !scene.value(QStringLiteral("media")).isArray()) {
        if (errorMessage) *errorMessage = QStringLiteral("Invalid scene revision or payload");
        return false;
    }
    QString manifestError;
    const QJsonArray normalizedManifest = normalizeManifest(manifest, &manifestError);
    if (!manifestError.isEmpty()) {
        if (errorMessage) *errorMessage = manifestError;
        return false;
    }
    Run created;
    created.remoteSessionId = binding.remoteSessionId;
    created.generation = binding.generation;
    created.sceneRunId = QUuid::createUuid().toString(QUuid::WithoutBraces);
    created.revision = revision;
    created.manifest = normalizedManifest;
    created.scene = normalizeJson(scene).toObject();
    created.digest = computeDigest(revision, created.manifest, created.scene);
    created.ownerEndpointId = binding.ownerEndpointId;
    created.targetEndpointId = binding.targetEndpointId;
    created.phase = Phase::Preparing;
    created.prepareDeadlineEpochMs = QDateTime::currentMSecsSinceEpoch() + m_prepareTimeoutMs;
    m_runsById.insert(created.sceneRunId, created);
    if (result) *result = created;
    emit runChanged(created.sceneRunId, created.phase);
    return true;
}

bool SceneRunCoordinator::acceptInboundEnvelope(const QJsonObject& envelope,
                                                QString* errorMessage)
{
    const QString remoteSessionId = envelope.value(QStringLiteral("remoteSessionId")).toString();
    const QString sceneRunId = envelope.value(QStringLiteral("sceneRunId")).toString();
    quint64 generation = 0;
    quint64 revision = 0;
    if (!readPositiveSafeInteger(
            envelope.value(QStringLiteral("generation")), &generation)
        || !readPositiveSafeInteger(
            envelope.value(QStringLiteral("revision")), &revision)) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("Stale or malformed scene envelope");
        }
        return false;
    }
    const QString digest = envelope.value(QStringLiteral("digest")).toString();
    const SessionBinding binding = sessionById(remoteSessionId);
    const QString type = envelope.value(QStringLiteral("type")).toString();
    const QString ownerEndpointId =
        envelope.value(QStringLiteral("ownerEndpointId")).toString();
    const QString targetEndpointId =
        envelope.value(QStringLiteral("targetEndpointId")).toString();
    if (type == QLatin1String("scene_prepare") && m_prepareTimeoutMs <= 0) {
        if (errorMessage) {
            *errorMessage = QStringLiteral(
                "Server scene preparation policy is unavailable");
        }
        return false;
    }
    const bool terminalDelivery = type == QLatin1String("stop")
        || type == QLatin1String("stopped");
    if (((!binding.active || !binding.commandReady) && !terminalDelivery) || !isOpaqueId(sceneRunId)
        || generation != binding.generation
        || ownerEndpointId != binding.ownerEndpointId
        || targetEndpointId != binding.targetEndpointId
        || revision < 1 || !isSha256(digest)) {
        if (errorMessage) *errorMessage = QStringLiteral("Stale or malformed scene envelope");
        return false;
    }

    auto iterator = m_runsById.find(sceneRunId);
    if (iterator == m_runsById.end()) {
        if (envelope.value(QStringLiteral("type")).toString() != QLatin1String("scene_prepare")) {
            if (errorMessage) *errorMessage = QStringLiteral("Unknown scene run");
            return false;
        }
        QString manifestError;
        const QJsonArray manifest = normalizeManifest(
            envelope.value(QStringLiteral("manifest")).toArray(), &manifestError);
        const QJsonObject scene = normalizeJson(envelope.value(QStringLiteral("scene"))).toObject();
        if (!manifestError.isEmpty() || computeDigest(revision, manifest, scene) != digest) {
            if (errorMessage) *errorMessage = QStringLiteral("Scene digest mismatch");
            return false;
        }
        Run incoming;
        incoming.remoteSessionId = remoteSessionId;
        incoming.generation = generation;
        incoming.sceneRunId = sceneRunId;
        incoming.revision = revision;
        incoming.digest = digest;
        incoming.ownerEndpointId = ownerEndpointId;
        incoming.targetEndpointId = targetEndpointId;
        incoming.manifest = manifest;
        incoming.scene = scene;
        incoming.phase = Phase::Preparing;
        incoming.prepareDeadlineEpochMs = QDateTime::currentMSecsSinceEpoch() + m_prepareTimeoutMs;
        m_runsById.insert(sceneRunId, incoming);
        emit runChanged(sceneRunId, incoming.phase);
        return true;
    }

    Run& current = iterator.value();
    if (current.remoteSessionId != remoteSessionId || current.generation != generation
        || current.revision != revision || current.digest != digest) {
        if (errorMessage) *errorMessage = QStringLiteral("Scene correlation mismatch");
        return false;
    }

    Phase next = current.phase;
    qint64 scheduledEpochMs = current.startEpochMs;
    qint64 scheduledServerMonotonicMs = current.startServerMonotonicMs;
    bool updateSchedule = false;
    if (type == QLatin1String("prepared")) {
        // The first party's acknowledgement is broadcast with
        // allPrepared=false while the authoritative run remains Preparing.
        next = envelope.value(QStringLiteral("allPrepared")).toBool(false)
            ? Phase::Prepared : current.phase;
    }
    else if (type == QLatin1String("armed")) next = Phase::Armed;
    else if (type == QLatin1String("commit")) {
        if (!readNonNegativeSafeInteger(
                envelope.value(QStringLiteral("startEpochMs")),
                &scheduledEpochMs)
            || !readNonNegativeSafeInteger(
                envelope.value(QStringLiteral("startServerMonotonicMs")),
                &scheduledServerMonotonicMs)) {
            if (errorMessage) {
                *errorMessage = QStringLiteral("Invalid scene activation deadline");
            }
            return false;
        }
        next = Phase::Scheduled;
        updateSchedule = true;
    } else if (type == QLatin1String("started")) {
        // A SceneRun is Live only after both first frames were presented.
        next = envelope.value(QStringLiteral("allStarted")).toBool(false)
            ? Phase::Live : Phase::Scheduled;
    }
    else if (type == QLatin1String("stop")) next = Phase::Stopping;
    else if (type == QLatin1String("stopped")) {
        next = envelope.value(QStringLiteral("failed")).toBool(false)
            ? Phase::Failed : Phase::Stopped;
    } else if (type == QLatin1String("state_snapshot")) {
        quint64 sequence = 0;
        if (!readPositiveSafeInteger(
                envelope.value(QStringLiteral("sequence")), &sequence)
            || sequence <= current.lastSnapshotSequence) {
            if (errorMessage) *errorMessage = QStringLiteral("Stale scene snapshot");
            return false;
        }
        current.lastSnapshotSequence = sequence;
        return true;
    } else if (type == QLatin1String("prepare_progress")
               || type == QLatin1String("scene_prepare")) {
        return true;
    }

    if (!isLegalTransition(current.phase, next)) {
        // Repeated server broadcasts are idempotent, backwards transitions are not.
        if (current.phase == next) return true;
        if (errorMessage) *errorMessage = QStringLiteral("Illegal scene state transition");
        return false;
    }
    if (updateSchedule) {
        current.startEpochMs = scheduledEpochMs;
        current.startServerMonotonicMs = scheduledServerMonotonicMs;
    }
    current.phase = next;
    if (next == Phase::Stopped || next == Phase::Failed) rememberFinishedRun(sceneRunId);
    emit runChanged(sceneRunId, next);
    return true;
}

void SceneRunCoordinator::finishRun(const QString& sceneRunId, bool failed)
{
    auto iterator = m_runsById.find(sceneRunId);
    if (iterator == m_runsById.end()) return;
    const Phase terminal = failed ? Phase::Failed : Phase::Stopped;
    iterator->phase = terminal;
    rememberFinishedRun(sceneRunId);
    emit runChanged(sceneRunId, terminal);
}

void SceneRunCoordinator::rememberFinishedRun(const QString& sceneRunId)
{
    if (m_finishedRunOrder.contains(sceneRunId)) return;
    m_finishedRunOrder.append(sceneRunId);
    while (m_finishedRunOrder.size() > 512) {
        const QString oldest = m_finishedRunOrder.takeFirst();
        const auto run = m_runsById.constFind(oldest);
        if (run != m_runsById.cend()
            && (run->phase == Phase::Stopped || run->phase == Phase::Failed))
            m_runsById.remove(oldest);
    }
}

QJsonArray SceneRunCoordinator::normalizeManifest(const QJsonArray& manifest,
                                                  QString* errorMessage)
{
    if (manifest.size() > kMaximumManifestItems) {
        if (errorMessage) *errorMessage = QStringLiteral("Scene manifest exceeds 256 assets");
        return {};
    }
    QList<QJsonObject> normalizedObjects;
    QSet<QString> assetIds;
    for (const QJsonValue& value : manifest) {
        if (!value.isObject()) {
            if (errorMessage) *errorMessage = QStringLiteral("Invalid scene asset");
            return {};
        }
        const QJsonObject source = value.toObject();
        const QString assetId = source.value(QStringLiteral("assetId")).toString();
        const QString fileId = source.value(QStringLiteral("fileId")).toString();
        const QString sha256 = source.value(QStringLiteral("sha256")).toString();
        const QString extension = source.value(QStringLiteral("extension")).toString().toLower();
        const QJsonValue sizeValue = source.value(QStringLiteral("size"));
        const double rawSize = sizeValue.toDouble(-1.0);
        const bool validSize = sizeValue.isDouble()
            && std::isfinite(rawSize) && std::floor(rawSize) == rawSize
            && rawSize >= 1.0
            && rawSize <= static_cast<double>(kMaximumAssetBytes);
        const qint64 size = validSize ? static_cast<qint64>(rawSize) : 0;
        QJsonArray mediaIds = source.value(QStringLiteral("mediaIds")).toArray();
        QStringList ids;
        QSet<QString> uniqueIds;
        for (const QJsonValue& idValue : mediaIds) {
            const QString id = idValue.toString();
            if (!isOpaqueId(id) || uniqueIds.contains(id)) {
                if (errorMessage) *errorMessage = QStringLiteral("Invalid scene media identity");
                return {};
            }
            uniqueIds.insert(id);
            ids.append(id);
        }
        std::sort(ids.begin(), ids.end());
        mediaIds = QJsonArray::fromStringList(ids);
        if (!isOpaqueId(assetId) || assetIds.contains(assetId)
            || !isSha256(fileId) || fileId != sha256
            || !validSize
            || !MediaFormatContract::isCanonicalMediaExtension(extension)) {
            if (errorMessage) *errorMessage = QStringLiteral("Invalid scene asset manifest");
            return {};
        }
        assetIds.insert(assetId);
        normalizedObjects.append(QJsonObject{
            {QStringLiteral("assetId"), assetId},
            {QStringLiteral("extension"), extension},
            {QStringLiteral("fileId"), fileId},
            {QStringLiteral("mediaIds"), mediaIds},
            {QStringLiteral("sha256"), sha256},
            {QStringLiteral("size"), static_cast<double>(size)}
        });
    }
    std::sort(normalizedObjects.begin(), normalizedObjects.end(), [](const QJsonObject& left,
                                                                    const QJsonObject& right) {
        return left.value(QStringLiteral("assetId")).toString()
            < right.value(QStringLiteral("assetId")).toString();
    });
    QJsonArray normalized;
    for (const QJsonObject& object : std::as_const(normalizedObjects)) normalized.append(object);
    if (errorMessage) errorMessage->clear();
    return normalized;
}

QByteArray SceneRunCoordinator::canonicalJson(const QJsonValue& value)
{
    const QJsonValue normalized = normalizeJson(value);
    if (normalized.isObject()) return QJsonDocument(normalized.toObject()).toJson(QJsonDocument::Compact);
    if (normalized.isArray()) return QJsonDocument(normalized.toArray()).toJson(QJsonDocument::Compact);
    QJsonArray wrapper{normalized};
    QByteArray serialized = QJsonDocument(wrapper).toJson(QJsonDocument::Compact);
    return serialized.mid(1, serialized.size() - 2);
}

QString SceneRunCoordinator::computeDigest(quint64 revision,
                                           const QJsonArray& normalizedManifest,
                                           const QJsonObject& scene)
{
    const QJsonObject material{
        {QStringLiteral("manifest"), normalizedManifest},
        {QStringLiteral("revision"), static_cast<double>(revision)},
        {QStringLiteral("scene"), normalizeJson(scene)}
    };
    return QString::fromLatin1(QCryptographicHash::hash(
        canonicalJson(material), QCryptographicHash::Sha256).toHex());
}

QJsonArray SceneRunCoordinator::createLocalChecklist(
    const QJsonObject& scene, QHash<QString, QString>* mediaIdsByItemId)
{
    if (mediaIdsByItemId) mediaIdsByItemId->clear();
    QJsonArray checklist;
    const QJsonArray screens = scene.value(QStringLiteral("screens")).toArray();
    for (qsizetype index = 0; index < screens.size() && checklist.size() < kMaximumChecklistItems; ++index) {
        const QJsonObject screen = screens.at(index).toObject();
        const QString id = safeChecklistId(
            QStringLiteral("screen_%1").arg(screen.value(QStringLiteral("id")).toVariant().toString()),
            QStringLiteral("screen_%1").arg(index));
        checklist.append(QJsonObject{{QStringLiteral("itemId"), id},
                                     {QStringLiteral("stage"), QStringLiteral("screen_render_graph_ready")},
                                     {QStringLiteral("ready"), true}});
    }
    const QJsonArray media = scene.value(QStringLiteral("media")).toArray();
    for (qsizetype index = 0; index < media.size() && checklist.size() < kMaximumChecklistItems; ++index) {
        const QJsonObject item = media.at(index).toObject();
        const QString type = item.value(QStringLiteral("type")).toString();
        const QString base = safeChecklistId(
            item.value(QStringLiteral("mediaId")).toString(),
            QStringLiteral("media_%1").arg(index));
        auto appendStage = [&](const QString& suffix, const QString& stage) {
            if (checklist.size() >= kMaximumChecklistItems) return;
            const QString itemId = safeChecklistId(base + QLatin1Char('_') + suffix, base);
            if (mediaIdsByItemId) {
                mediaIdsByItemId->insert(itemId, item.value(QStringLiteral("mediaId")).toString());
            }
            checklist.append(QJsonObject{
                {QStringLiteral("itemId"), itemId},
                {QStringLiteral("stage"), stage},
                {QStringLiteral("ready"), true}
            });
        };
        if (type != QLatin1String("text")) appendStage(QStringLiteral("file"), QStringLiteral("file_validated"));
        if (type != QLatin1String("text")) appendStage(QStringLiteral("memory"), QStringLiteral("media_memory_ready"));
        if (type == QLatin1String("text")) {
            appendStage(QStringLiteral("glyphs"), QStringLiteral("text_glyphs_ready"));
            appendStage(QStringLiteral("layout"), QStringLiteral("text_layout_ready"));
        } else if (type == QLatin1String("video")) {
            appendStage(QStringLiteral("decode"), QStringLiteral("video_decoded"));
            appendStage(QStringLiteral("frame"), QStringLiteral("first_frame_positioned"));
            appendStage(QStringLiteral("audio"), QStringLiteral("audio_prepared"));
            appendStage(QStringLiteral("graph"), QStringLiteral("render_graph_ready"));
        } else {
            appendStage(QStringLiteral("decode"), QStringLiteral("image_decoded"));
            appendStage(QStringLiteral("texture"), QStringLiteral("image_texture_ready"));
        }
    }
    if (checklist.isEmpty()) {
        checklist.append(QJsonObject{{QStringLiteral("itemId"), QStringLiteral("scene")},
                                     {QStringLiteral("stage"), QStringLiteral("screen_render_graph_ready")},
                                     {QStringLiteral("ready"), true}});
    }
    return checklist;
}

QString SceneRunCoordinator::phaseName(Phase phase)
{
    switch (phase) {
    case Phase::Draft: return QStringLiteral("Draft");
    case Phase::Preparing: return QStringLiteral("Preparing");
    case Phase::Prepared: return QStringLiteral("Prepared");
    case Phase::Armed: return QStringLiteral("Armed");
    case Phase::Scheduled: return QStringLiteral("Scheduled");
    case Phase::Live: return QStringLiteral("Live");
    case Phase::Stopping: return QStringLiteral("Stopping");
    case Phase::Stopped: return QStringLiteral("Stopped");
    case Phase::Failed: return QStringLiteral("Failed");
    }
    return QStringLiteral("Failed");
}

bool SceneRunCoordinator::isOpaqueId(const QString& value)
{
    static const QRegularExpression expression(QStringLiteral("^[A-Za-z0-9_-]{1,128}$"));
    return expression.match(value).hasMatch();
}

bool SceneRunCoordinator::isSha256(const QString& value)
{
    static const QRegularExpression expression(QStringLiteral("^[a-fA-F0-9]{64}$"));
    return expression.match(value).hasMatch();
}

SceneRunCoordinator::Phase SceneRunCoordinator::phaseFromWire(const QString& value,
                                                               Phase fallback)
{
    if (value == QLatin1String("Preparing")) return Phase::Preparing;
    if (value == QLatin1String("Prepared")) return Phase::Prepared;
    if (value == QLatin1String("Armed")) return Phase::Armed;
    if (value == QLatin1String("Scheduled")) return Phase::Scheduled;
    if (value == QLatin1String("Live")) return Phase::Live;
    if (value == QLatin1String("Stopping")) return Phase::Stopping;
    if (value == QLatin1String("Stopped")) return Phase::Stopped;
    if (value == QLatin1String("Failed")) return Phase::Failed;
    return fallback;
}

bool SceneRunCoordinator::isLegalTransition(Phase from, Phase to)
{
    if (from == to) return true;
    if (to == Phase::Stopping || to == Phase::Failed) {
        return from != Phase::Stopped && from != Phase::Failed;
    }
    if (from == Phase::Stopping && to == Phase::Stopped) return true;
    return (from == Phase::Preparing && to == Phase::Prepared)
        || (from == Phase::Prepared && to == Phase::Armed)
        || (from == Phase::Armed && to == Phase::Scheduled)
        || (from == Phase::Scheduled && to == Phase::Live);
}
