#include "backend/network/RemoteSessionCoordinator.h"
#include "backend/network/NetworkDiagnostics.h"

#include <QRegularExpression>
#include <QJsonArray>

#include <cmath>

namespace {
constexpr double kMaximumSafeJsonInteger = 9007199254740991.0;
constexpr qsizetype kMaximumSessionIdentities = 4096;

bool readPositiveGeneration(const QJsonValue& value, quint64* result)
{
    if (!result || !value.isDouble()) return false;
    const double raw = value.toDouble(-1.0);
    if (!std::isfinite(raw) || raw < 1.0 || raw > kMaximumSafeJsonInteger
        || std::floor(raw) != raw) {
        return false;
    }
    *result = static_cast<quint64>(raw);
    return true;
}

bool isBoundedInteger(const QJsonValue& value, qint64 minimum, qint64 maximum)
{
    if (!value.isDouble()) return false;
    const double raw = value.toDouble();
    return std::isfinite(raw) && std::floor(raw) == raw
        && raw >= static_cast<double>(minimum)
        && raw <= static_cast<double>(maximum);
}

bool hasOnlyKeys(const QJsonObject& object,
                 const QSet<QString>& required,
                 const QSet<QString>& optional = {})
{
    for (const QString& key : required) {
        if (!object.contains(key)) return false;
    }
    for (auto it = object.constBegin(); it != object.constEnd(); ++it) {
        if (!required.contains(it.key()) && !optional.contains(it.key())) {
            return false;
        }
    }
    return true;
}

bool isValidUiZone(const QJsonValue& value)
{
    if (!value.isObject()) return false;
    const QJsonObject zone = value.toObject();
    static const QSet<QString> keys = {
        QStringLiteral("type"), QStringLiteral("x"), QStringLiteral("y"),
        QStringLiteral("width"), QStringLiteral("height")
    };
    static const QSet<QString> types = {
        QStringLiteral("taskbar"), QStringLiteral("menu_bar"),
        QStringLiteral("dock")
    };
    return hasOnlyKeys(zone, keys)
        && zone.value(QStringLiteral("type")).isString()
        && types.contains(zone.value(QStringLiteral("type")).toString())
        && isBoundedInteger(zone.value(QStringLiteral("x")), -1'000'000, 1'000'000)
        && isBoundedInteger(zone.value(QStringLiteral("y")), -1'000'000, 1'000'000)
        && isBoundedInteger(zone.value(QStringLiteral("width")), 0, 100'000)
        && isBoundedInteger(zone.value(QStringLiteral("height")), 0, 100'000);
}

bool isValidScreen(const QJsonValue& value)
{
    if (!value.isObject()) return false;
    const QJsonObject screen = value.toObject();
    static const QSet<QString> required = {
        QStringLiteral("id"), QStringLiteral("width"),
        QStringLiteral("height"), QStringLiteral("x"),
        QStringLiteral("y"), QStringLiteral("primary")
    };
    static const QSet<QString> optional = {QStringLiteral("uiZones")};
    if (!hasOnlyKeys(screen, required, optional)
        || !isBoundedInteger(screen.value(QStringLiteral("id")), 0, 1'000'000)
        || !isBoundedInteger(screen.value(QStringLiteral("width")), 1, 100'000)
        || !isBoundedInteger(screen.value(QStringLiteral("height")), 1, 100'000)
        || !isBoundedInteger(screen.value(QStringLiteral("x")), -1'000'000, 1'000'000)
        || !isBoundedInteger(screen.value(QStringLiteral("y")), -1'000'000, 1'000'000)
        || !screen.value(QStringLiteral("primary")).isBool()) {
        return false;
    }
    const QJsonValue zones = screen.value(QStringLiteral("uiZones"));
    if (zones.isUndefined()) return true;
    if (!zones.isArray() || zones.toArray().size() > 16) return false;
    for (const QJsonValue& zone : zones.toArray()) {
        if (!isValidUiZone(zone)) return false;
    }
    return true;
}
}

RemoteSessionCoordinator::RemoteSessionCoordinator(QObject* parent)
    : QObject(parent)
{
}

void RemoteSessionCoordinator::setLocalEndpointId(const QString& endpointId)
{
    if (m_localEndpointId == endpointId) return;
    clear();
    m_localEndpointId = endpointId;
}

bool RemoteSessionCoordinator::upsert(const QJsonObject& envelope,
                                      quint64 localConnectionGeneration,
                                      QString* validationError)
{
    if (validationError) validationError->clear();
    Binding binding;
    binding.remoteSessionId = envelope.value(QStringLiteral("remoteSessionId")).toString();
    const bool versioned = envelope.contains(QStringLiteral("protocolVersion"));
    if (versioned && !readPositiveGeneration(envelope.value(QStringLiteral("stateRevision")),
                                             &binding.stateRevision)) return false;
    if (!versioned) readPositiveGeneration(envelope.value(QStringLiteral("stateRevision")),
                                           &binding.stateRevision);
    const QJsonValue deadline = envelope.value(QStringLiteral("validUntilServerMonotonicMs"));
    if (deadline.isDouble() && isBoundedInteger(deadline, 0, 9007199254740991LL))
        binding.validUntilServerMonotonicMs = static_cast<qint64>(deadline.toDouble());
    if (!readPositiveGeneration(envelope.value(QStringLiteral("generation")),
                                &binding.generation)
        || !readPositiveGeneration(
            envelope.value(QStringLiteral("ownerConnectionGeneration")),
            &binding.ownerConnectionGeneration)
        || !readPositiveGeneration(
            envelope.value(QStringLiteral("targetConnectionGeneration")),
            &binding.targetConnectionGeneration)) {
        return false;
    }
    binding.ownerEndpointId = envelope.value(QStringLiteral("ownerEndpointId")).toString();
    binding.targetEndpointId = envelope.value(QStringLiteral("targetEndpointId")).toString();
    binding.resumeToken = envelope.value(QStringLiteral("resumeToken")).toString();
    binding.teardownId = envelope.value(QStringLiteral("teardownId")).toString();
    binding.phase = envelope.value(QStringLiteral("phase")).toString();
    // Grace keeps the binding and its resume token alive, but it is not an
    // admissible command state. New scene/upload commands must wait until the
    // signed resume has moved the session back to Active.
    binding.degraded = envelope.value(QStringLiteral("degraded")).toBool()
        || envelope.value(QStringLiteral("state")).toString() == QLatin1String("Degraded");
    binding.active = binding.phase == QLatin1String("Active") && !binding.degraded;
    binding.commandReady = envelope.value(QStringLiteral("commandReady")).toBool(!versioned);
    const bool terminalPhase = binding.phase == QLatin1String("Terminating")
        || binding.phase == QLatin1String("CleanupPending")
        || binding.phase == QLatin1String("Closed");
    if (versioned && !terminalPhase && binding.validUntilServerMonotonicMs < 0) return false;
    if (!isOpaqueId(binding.remoteSessionId)
        || m_closedSessionIds.contains(binding.remoteSessionId)
        || !isEndpointId(binding.ownerEndpointId) || !isEndpointId(binding.targetEndpointId)
        || binding.ownerEndpointId == binding.targetEndpointId
        || !isAllowedPhase(binding.phase)
        || (!binding.resumeToken.isEmpty() && !isOpaqueId(binding.resumeToken))
        || (!binding.teardownId.isEmpty() && !isOpaqueId(binding.teardownId))
        || (terminalPhase && binding.teardownId.isEmpty())
        || (terminalPhase && !binding.resumeToken.isEmpty())
        || (!terminalPhase && !binding.teardownId.isEmpty())
        || (m_localEndpointId != binding.ownerEndpointId
            && m_localEndpointId != binding.targetEndpointId)) return false;

    const quint64 envelopeLocalConnectionGeneration =
        m_localEndpointId == binding.ownerEndpointId
        ? binding.ownerConnectionGeneration : binding.targetConnectionGeneration;
    if (localConnectionGeneration > 0
        && envelopeLocalConnectionGeneration != localConnectionGeneration) {
        return false;
    }

    const auto previous = m_byId.constFind(binding.remoteSessionId);
    if (previous == m_byId.cend()
        && m_closedSessionIds.size() + m_byId.size() >= kMaximumSessionIdentities) {
        if (validationError) *validationError = QStringLiteral("session_history_capacity");
        NetworkDiagnostics::record(QStringLiteral("session_rejected"), {
            {"remoteSessionId", binding.remoteSessionId}, {"reason", "session_history_capacity"}}, true);
        return false;
    }
    if (previous != m_byId.cend()) {
        if (previous->ownerEndpointId != binding.ownerEndpointId
            || previous->targetEndpointId != binding.targetEndpointId
            || previous->generation > binding.generation
            || (binding.stateRevision && previous->stateRevision > binding.stateRevision)
            || (!previous->resumeToken.isEmpty() && !binding.resumeToken.isEmpty()
                && previous->resumeToken != binding.resumeToken)) {
            return false;
        }
        if (!previous->teardownId.isEmpty()
            && previous->teardownId != binding.teardownId) {
            return false;
        }
        if (previous->generation == binding.generation) {
            const bool sameTransportTuple =
                previous->ownerConnectionGeneration
                    == binding.ownerConnectionGeneration
                && previous->targetConnectionGeneration
                    == binding.targetConnectionGeneration;
            // Either party can reconnect after the server made the session
            // terminal. It may therefore miss Terminating entirely. Accept a
            // terminal replay only when the authenticated local side alone is
            // rebound to this transport; the remote transport, session
            // generation and (once known) teardown id remain immutable.
            const bool localIsOwner = m_localEndpointId == binding.ownerEndpointId;
            const quint64 previousLocalTransport = localIsOwner
                ? previous->ownerConnectionGeneration
                : previous->targetConnectionGeneration;
            const quint64 currentLocalTransport = localIsOwner
                ? binding.ownerConnectionGeneration
                : binding.targetConnectionGeneration;
            const quint64 previousRemoteTransport = localIsOwner
                ? previous->targetConnectionGeneration
                : previous->ownerConnectionGeneration;
            const quint64 currentRemoteTransport = localIsOwner
                ? binding.targetConnectionGeneration
                : binding.ownerConnectionGeneration;
            const bool localTerminalCatchup = localConnectionGeneration > 0
                && terminalPhase
                && binding.resumeToken.isEmpty()
                && previousRemoteTransport == currentRemoteTransport
                && currentLocalTransport > previousLocalTransport
                && currentLocalTransport == localConnectionGeneration;
            if ((!sameTransportTuple && !localTerminalCatchup)
                || !isAllowedSameGenerationTransition(previous->phase,
                                                       binding.phase)) {
                return false;
            }
        } else {
            // A RemoteSession generation advances only when a party resumes
            // on a newer authenticated transport. Skipped intermediate
            // envelopes are acceptable, but neither transport generation may
            // move backwards and at least one must advance.
            if ((!terminalPhase && (previous->phase == QLatin1String("Terminating")
                || previous->phase == QLatin1String("CleanupPending")
                || previous->phase == QLatin1String("Closed")))
                || binding.ownerConnectionGeneration
                    < previous->ownerConnectionGeneration
                || binding.targetConnectionGeneration
                    < previous->targetConnectionGeneration
                || (!terminalPhase && binding.ownerConnectionGeneration
                        == previous->ownerConnectionGeneration
                    && binding.targetConnectionGeneration
                        == previous->targetConnectionGeneration)) {
                return false;
            }
        }
    } else if (binding.phase == QLatin1String("Terminating")
               || binding.phase == QLatin1String("CleanupPending")
               || binding.phase == QLatin1String("Closed")) {
        // After either party restarts, the server can replay a terminal session
        // once that installation authenticates. This grants no command state;
        // target-side cache cleanup still has to commit before Closed.
        if (binding.phase == QLatin1String("Closed")
            || !binding.resumeToken.isEmpty()
            || !isOpaqueId(
                envelope.value(QStringLiteral("teardownId")).toString())) {
            return false;
        }
    }

    const bool localIsOwner = m_localEndpointId == binding.ownerEndpointId;
    const QString peer = m_localEndpointId == binding.ownerEndpointId
        ? binding.targetEndpointId : binding.ownerEndpointId;
    QHash<QString, QString>& directionalIndex = localIsOwner
        ? m_outgoingIdByPeer : m_incomingIdByPeer;
    const QString existingForPeer = directionalIndex.value(peer);
    if (!terminalPhase && !existingForPeer.isEmpty() && existingForPeer != binding.remoteSessionId) {
        const Binding indexed = m_byId.value(existingForPeer);
        if (indexed.phase != QLatin1String("Terminating")
            && indexed.phase != QLatin1String("CleanupPending")
            && indexed.phase != QLatin1String("Closed")) return false;
    }
    // Snapshot validation comes after the complete identity/transition checks,
    // but before the binding becomes observable or command-capable.
    const QString envelopeType = envelope.value(QStringLiteral("type")).toString();
    const bool requiresOwnerSnapshot = binding.ownerEndpointId == m_localEndpointId
        && (envelopeType == QLatin1String("remote_session_opened")
            || (envelopeType == QLatin1String("remote_session_resumed")
                && (previous == m_byId.cend() || envelope.contains(QStringLiteral("snapshot")))));
    if (versioned && requiresOwnerSnapshot
        && (!validateSnapshot(envelope.value(QStringLiteral("snapshot")).toObject())
            || !isBoundedInteger(envelope.value(QStringLiteral("snapshotSequence")), 1, 9007199254740991LL))) {
        if (validationError) *validationError = QStringLiteral("invalid_initial_snapshot");
        return false;
    }
    if (!terminalPhase && binding.resumeToken.isEmpty()
        && previous != m_byId.cend()) {
        binding.resumeToken = previous->resumeToken;
    }
    if (binding.teardownId.isEmpty() && previous != m_byId.cend()) {
        binding.teardownId = previous->teardownId;
    }
    // Readiness is monotonic within a revision: an idempotent replay of the
    // pre-ACK OPEN/RESUME cannot revoke an already applied two-party barrier.
    if (previous != m_byId.cend() && binding.stateRevision > 0
        && binding.stateRevision == previous->stateRevision
        && binding.generation == previous->generation && binding.phase == previous->phase
        && binding.ownerConnectionGeneration == previous->ownerConnectionGeneration
        && binding.targetConnectionGeneration == previous->targetConnectionGeneration
        && previous->commandReady && !binding.degraded)
        binding.commandReady = true;
    if (previous != m_byId.cend() && binding.stateRevision > 0
        && binding.stateRevision == previous->stateRevision
        && binding.generation == previous->generation && binding.phase == previous->phase
        && binding.ownerConnectionGeneration == previous->ownerConnectionGeneration
        && binding.targetConnectionGeneration == previous->targetConnectionGeneration
        && binding.validUntilServerMonotonicMs == previous->validUntilServerMonotonicMs
        && binding.degraded == previous->degraded
        && binding.commandReady == previous->commandReady) return true;
    m_byId.insert(binding.remoteSessionId, binding);
    if (!terminalPhase || existingForPeer.isEmpty() || existingForPeer == binding.remoteSessionId)
        directionalIndex.insert(peer, binding.remoteSessionId);
    emit sessionChanged(binding.remoteSessionId, binding.generation, binding.phase);
    return true;
}

bool RemoteSessionCoordinator::acceptSnapshot(
    const QJsonObject& envelope, quint64 localConnectionGeneration,
    bool allowInitialReplay)
{
    const QString remoteSessionId =
        envelope.value(QStringLiteral("remoteSessionId")).toString();
    const Binding binding = byId(remoteSessionId);
    quint64 generation = 0;
    quint64 sequence = 0;
    const auto reject = [&](const QString& reason) {
        NetworkDiagnostics::record(QStringLiteral("snapshot_rejected"), {
            {QStringLiteral("remoteSessionId"), remoteSessionId},
            {QStringLiteral("generation"), static_cast<qint64>(generation)},
            {QStringLiteral("sequence"), static_cast<qint64>(sequence)},
            {QStringLiteral("reason"), reason}});
        return false;
    };
    const QJsonValue snapshotValue = envelope.value(QStringLiteral("snapshot"));
    if (binding.remoteSessionId.isEmpty() || !binding.active
        || binding.ownerEndpointId != m_localEndpointId)
        return reject(QStringLiteral("session_inactive"));
    if (!readPositiveGeneration(envelope.value(QStringLiteral("generation")),
                                   &generation)
        || !readPositiveGeneration(
            envelope.value(QStringLiteral("snapshotSequence")), &sequence)
        || !snapshotValue.isObject())
        return reject(QStringLiteral("invalid_structure"));
    if (generation != binding.generation
        || (localConnectionGeneration != 0
            && binding.ownerConnectionGeneration != localConnectionGeneration))
        return reject(QStringLiteral("generation_mismatch"));
    const QJsonObject snapshot = snapshotValue.toObject();
    if (!validateSnapshot(snapshot)) return reject(QStringLiteral("invalid_structure"));
    const quint64 snapshotRevision = static_cast<quint64>(snapshot.value(QStringLiteral("revision")).toDouble());
    if (sequence <= m_lastSnapshotSequenceBySession.value(remoteSessionId, 0)
        || snapshotRevision <= m_lastSnapshotRevisionBySession.value(remoteSessionId, 0)) {
        if (allowInitialReplay && (snapshot == m_initialSnapshotBySession.value(remoteSessionId)
            || (sequence == m_lastSnapshotSequenceBySession.value(remoteSessionId)
                && snapshot == m_latestSnapshotBySession.value(remoteSessionId)))) return true;
        return reject(QStringLiteral("old_sequence"));
    }
    if (!m_initialSnapshotBySession.contains(remoteSessionId))
        m_initialSnapshotBySession.insert(remoteSessionId, snapshot);
    m_latestSnapshotBySession.insert(remoteSessionId, snapshot);
    m_lastSnapshotSequenceBySession.insert(remoteSessionId, sequence);
    m_lastSnapshotRevisionBySession.insert(remoteSessionId, snapshotRevision);
    return true;
}

bool RemoteSessionCoordinator::validateSnapshot(const QJsonObject& snapshot)
{
    const QJsonValue screens = snapshot.value(QStringLiteral("screens"));
    const QJsonValue systemUI = snapshot.value(QStringLiteral("systemUI"));
    const QJsonValue volume = snapshot.value(QStringLiteral("volumePercent"));
    quint64 snapshotRevision = 0;
    quint64 capturedAt = 0;
    static const QSet<QString> snapshotKeys = {
        QStringLiteral("screens"), QStringLiteral("systemUI"),
        QStringLiteral("volumePercent"), QStringLiteral("revision"),
        QStringLiteral("capturedAtEpochMs")
    };
    if (!hasOnlyKeys(snapshot, snapshotKeys)
        || !screens.isArray() || !systemUI.isArray()
        || screens.toArray().size() > 64
        || systemUI.toArray().size() > 64
        || !readPositiveGeneration(snapshot.value(QStringLiteral("revision")),
                                   &snapshotRevision)
        || !readPositiveGeneration(
            snapshot.value(QStringLiteral("capturedAtEpochMs")), &capturedAt)
        || !(volume.isNull()
             || (volume.isDouble() && std::isfinite(volume.toDouble())
                 && volume.toDouble() >= 0.0 && volume.toDouble() <= 100.0))) {
        return false;
    }
    for (const QJsonValue& screen : screens.toArray()) {
        if (!isValidScreen(screen)) return false;
    }
    for (const QJsonValue& zone : systemUI.toArray()) {
        if (!isValidUiZone(zone)) return false;
    }
    return true;
}

void RemoteSessionCoordinator::suspend(const QString& remoteSessionId)
{
    auto it = m_byId.find(remoteSessionId);
    if (it == m_byId.end() || (it->phase != QLatin1String("Active")
        && it->phase != QLatin1String("Grace"))) return;
    it->active = false;
    it->commandReady = false;
    it->degraded = true;
    emit sessionChanged(remoteSessionId, it->generation, it->phase);
}

bool RemoteSessionCoordinator::isClosedDuplicate(const QJsonObject& envelope) const
{
    const auto found = m_closedBindings.constFind(envelope.value(QStringLiteral("remoteSessionId")).toString());
    if (found == m_closedBindings.cend()) return false;
    quint64 generation = 0;
    return readPositiveGeneration(envelope.value(QStringLiteral("generation")), &generation)
        && generation == found->generation
        && envelope.value(QStringLiteral("ownerEndpointId")).toString() == found->ownerEndpointId
        && envelope.value(QStringLiteral("targetEndpointId")).toString() == found->targetEndpointId
        && envelope.value(QStringLiteral("teardownId")).toString() == found->teardownId;
}

bool RemoteSessionCoordinator::canClose(
    const QJsonObject& envelope, quint64 localConnectionGeneration) const
{
    const QString remoteSessionId =
        envelope.value(QStringLiteral("remoteSessionId")).toString();
    const auto iterator = m_byId.constFind(remoteSessionId);

    quint64 generation = 0;
    quint64 ownerConnectionGeneration = 0;
    quint64 targetConnectionGeneration = 0;
    if (!readPositiveGeneration(envelope.value(QStringLiteral("generation")),
                                &generation)
        || !readPositiveGeneration(
            envelope.value(QStringLiteral("ownerConnectionGeneration")),
            &ownerConnectionGeneration)
        || !readPositiveGeneration(
            envelope.value(QStringLiteral("targetConnectionGeneration")),
            &targetConnectionGeneration)) {
        return false;
    }

    const QString ownerEndpointId =
        envelope.value(QStringLiteral("ownerEndpointId")).toString();
    const QString targetEndpointId =
        envelope.value(QStringLiteral("targetEndpointId")).toString();
    const QString teardownId =
        envelope.value(QStringLiteral("teardownId")).toString();
    if (!isOpaqueId(remoteSessionId) || !isEndpointId(ownerEndpointId)
        || !isEndpointId(targetEndpointId) || ownerEndpointId == targetEndpointId
        || !isOpaqueId(teardownId)
        || envelope.value(QStringLiteral("phase")).toString()
            != QLatin1String("Closed")
        || (m_localEndpointId != ownerEndpointId
            && m_localEndpointId != targetEndpointId)) {
        return false;
    }
    const bool localIsOwner = m_localEndpointId == ownerEndpointId;
    const quint64 envelopeLocalConnectionGeneration = localIsOwner
        ? ownerConnectionGeneration : targetConnectionGeneration;
    if (localConnectionGeneration > 0
        && envelopeLocalConnectionGeneration != localConnectionGeneration) {
        return false;
    }

    quint64 revision = 0;
    const bool revisionPresent = readPositiveGeneration(envelope.value(QStringLiteral("stateRevision")), &revision);
    if (envelope.contains(QStringLiteral("protocolVersion")) && !revisionPresent) return false;

    // A restarted owner may receive the committed Closed tombstone before any
    // terminal state replay. It owns no receiver cache, so accepting this
    // complete authenticated tuple merely resolves stale local UI/state.
    if (iterator == m_byId.cend()) {
        return localIsOwner && !m_closedSessionIds.contains(remoteSessionId)
            && m_closedSessionIds.size() + m_byId.size() < kMaximumSessionIdentities;
    }

    const Binding& binding = iterator.value();
    if (revisionPresent && revision < binding.stateRevision) return false;
    if (generation != binding.generation
        && (!revisionPresent || revision <= binding.stateRevision)) return false;
    if (ownerEndpointId != binding.ownerEndpointId
        || targetEndpointId != binding.targetEndpointId
        || generation < binding.generation) {
        return false;
    }
    if (!localIsOwner) {
        return (binding.phase == QLatin1String("Terminating")
                || binding.phase == QLatin1String("CleanupPending"))
            && teardownId == binding.teardownId
            && !binding.teardownId.isEmpty()
            && ownerConnectionGeneration >= binding.ownerConnectionGeneration
            && targetConnectionGeneration == envelopeLocalConnectionGeneration;
    }

    const bool terminalOrMissedTerminal =
        binding.phase == QLatin1String("Active")
        || binding.phase == QLatin1String("Grace")
        || binding.phase == QLatin1String("Terminating")
        || binding.phase == QLatin1String("CleanupPending");
    const bool teardownMatches = binding.teardownId.isEmpty()
        || binding.teardownId == teardownId;
    const bool ownerTransportMatches =
        ownerConnectionGeneration == binding.ownerConnectionGeneration
        || (localConnectionGeneration > 0
            && ownerConnectionGeneration > binding.ownerConnectionGeneration
            && ownerConnectionGeneration == localConnectionGeneration);
    return terminalOrMissedTerminal && teardownMatches
        && ownerTransportMatches
        && targetConnectionGeneration >= binding.targetConnectionGeneration;
}

void RemoteSessionCoordinator::remove(const QString& remoteSessionId, const QJsonObject& finalEnvelope)
{
    Binding binding = m_byId.take(remoteSessionId);
    if (!finalEnvelope.isEmpty()) {
        binding.remoteSessionId = remoteSessionId;
        binding.ownerEndpointId = finalEnvelope.value(QStringLiteral("ownerEndpointId")).toString();
        binding.targetEndpointId = finalEnvelope.value(QStringLiteral("targetEndpointId")).toString();
        binding.teardownId = finalEnvelope.value(QStringLiteral("teardownId")).toString();
        readPositiveGeneration(finalEnvelope.value(QStringLiteral("generation")), &binding.generation);
        readPositiveGeneration(finalEnvelope.value(QStringLiteral("stateRevision")), &binding.stateRevision);
        binding.phase = QStringLiteral("Closed");
        binding.active = false;
    }
    if (isOpaqueId(remoteSessionId) && !m_closedSessionIds.contains(remoteSessionId)
        && m_closedSessionIds.size() < kMaximumSessionIdentities) {
        m_closedSessionIds.insert(remoteSessionId);
        m_closedSessionOrder.append(remoteSessionId);
        m_closedBindings.insert(remoteSessionId, binding);
        while (m_closedSessionOrder.size() > 2048) {
            const QString oldest = m_closedSessionOrder.takeFirst();
            // Keep the small tombstone after detailed cleanup evidence is
            // retired. Forgetting its ID would make an old OPEN admissible.
            m_closedBindings.remove(oldest);
        }
    }
    m_initialSnapshotBySession.remove(remoteSessionId);
    m_latestSnapshotBySession.remove(remoteSessionId);
    m_lastSnapshotSequenceBySession.remove(remoteSessionId);
    m_lastSnapshotRevisionBySession.remove(remoteSessionId);
    if (binding.remoteSessionId.isEmpty()) return;
    const QString peer = m_localEndpointId == binding.ownerEndpointId
        ? binding.targetEndpointId : binding.ownerEndpointId;
    QHash<QString, QString>& directionalIndex =
        m_localEndpointId == binding.ownerEndpointId
        ? m_outgoingIdByPeer : m_incomingIdByPeer;
    if (directionalIndex.value(peer) == remoteSessionId) {
        directionalIndex.remove(peer);
    }
    emit sessionRemoved(remoteSessionId);
}

void RemoteSessionCoordinator::clear()
{
    const QStringList ids = m_byId.keys();
    m_byId.clear();
    m_outgoingIdByPeer.clear();
    m_incomingIdByPeer.clear();
    m_lastSnapshotSequenceBySession.clear();
    m_lastSnapshotRevisionBySession.clear();
    m_closedSessionIds.clear();
    m_closedSessionOrder.clear();
    m_closedBindings.clear();
    m_initialSnapshotBySession.clear();
    m_latestSnapshotBySession.clear();
    for (const QString& id : ids) emit sessionRemoved(id);
}

RemoteSessionCoordinator::Binding RemoteSessionCoordinator::forPeer(
    const QString& peerEndpointId) const
{
    const Binding outgoing = outgoingForPeer(peerEndpointId);
    return outgoing.remoteSessionId.isEmpty()
        ? incomingForPeer(peerEndpointId) : outgoing;
}

RemoteSessionCoordinator::Binding RemoteSessionCoordinator::outgoingForPeer(
    const QString& peerEndpointId) const
{
    return byId(m_outgoingIdByPeer.value(peerEndpointId));
}

RemoteSessionCoordinator::Binding RemoteSessionCoordinator::incomingForPeer(
    const QString& peerEndpointId) const
{
    return byId(m_incomingIdByPeer.value(peerEndpointId));
}

RemoteSessionCoordinator::Binding RemoteSessionCoordinator::byId(
    const QString& remoteSessionId) const
{
    return m_byId.value(remoteSessionId);
}

QList<RemoteSessionCoordinator::Binding> RemoteSessionCoordinator::all() const
{
    return m_byId.values();
}

bool RemoteSessionCoordinator::isEndpointId(const QString& value)
{
    static const QRegularExpression expression(QStringLiteral("^[A-Za-z0-9_-]{43}$"));
    return expression.match(value).hasMatch();
}

bool RemoteSessionCoordinator::isOpaqueId(const QString& value)
{
    static const QRegularExpression expression(QStringLiteral("^[A-Za-z0-9_-]{1,128}$"));
    return expression.match(value).hasMatch();
}

bool RemoteSessionCoordinator::isAllowedPhase(const QString& phase)
{
    return phase == QLatin1String("Opening")
        || phase == QLatin1String("Active")
        || phase == QLatin1String("Grace")
        || phase == QLatin1String("Terminating")
        || phase == QLatin1String("CleanupPending")
        || phase == QLatin1String("Closed");
}

bool RemoteSessionCoordinator::isAllowedSameGenerationTransition(
    const QString& from, const QString& to)
{
    if (from == to) return true;
    if (from == QLatin1String("Opening")) {
        return to == QLatin1String("Active")
            || to == QLatin1String("Grace")
            || to == QLatin1String("Terminating")
            || to == QLatin1String("CleanupPending")
            || to == QLatin1String("Closed");
    }
    if (from == QLatin1String("Active")) {
        return to == QLatin1String("Grace")
            || to == QLatin1String("Terminating")
            || to == QLatin1String("CleanupPending")
            || to == QLatin1String("Closed");
    }
    if (from == QLatin1String("Grace")) {
        return to == QLatin1String("Terminating")
            || to == QLatin1String("CleanupPending")
            || to == QLatin1String("Closed");
    }
    if (from == QLatin1String("Terminating")) {
        return to == QLatin1String("CleanupPending")
            || to == QLatin1String("Closed");
    }
    if (from == QLatin1String("CleanupPending")) {
        return to == QLatin1String("Closed");
    }
    return false;
}
