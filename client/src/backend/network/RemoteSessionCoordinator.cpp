#include "backend/network/RemoteSessionCoordinator.h"

#include <QRegularExpression>

#include <cmath>

namespace {
constexpr double kMaximumSafeJsonInteger = 9007199254740991.0;

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
}

RemoteSessionCoordinator::RemoteSessionCoordinator(QObject* parent)
    : QObject(parent)
{
}

void RemoteSessionCoordinator::setLocalDeviceId(const QString& deviceId)
{
    if (m_localDeviceId == deviceId) return;
    clear();
    m_localDeviceId = deviceId;
}

bool RemoteSessionCoordinator::upsert(const QJsonObject& envelope,
                                      quint64 localConnectionGeneration)
{
    Binding binding;
    binding.remoteSessionId = envelope.value(QStringLiteral("remoteSessionId")).toString();
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
    binding.ownerDeviceId = envelope.value(QStringLiteral("ownerDeviceId")).toString();
    binding.targetDeviceId = envelope.value(QStringLiteral("targetDeviceId")).toString();
    binding.resumeToken = envelope.value(QStringLiteral("resumeToken")).toString();
    binding.teardownId = envelope.value(QStringLiteral("teardownId")).toString();
    binding.phase = envelope.value(QStringLiteral("phase")).toString();
    // Grace keeps the binding and its resume token alive, but it is not an
    // admissible command state. New scene/upload commands must wait until the
    // signed resume has moved the session back to Active.
    binding.active = binding.phase == QLatin1String("Active");
    const bool terminalPhase = binding.phase == QLatin1String("Terminating")
        || binding.phase == QLatin1String("CleanupPending")
        || binding.phase == QLatin1String("Closed");
    if (!isOpaqueId(binding.remoteSessionId)
        || m_closedSessionIds.contains(binding.remoteSessionId)
        || !isDeviceId(binding.ownerDeviceId) || !isDeviceId(binding.targetDeviceId)
        || binding.ownerDeviceId == binding.targetDeviceId
        || !isAllowedPhase(binding.phase)
        || (!binding.resumeToken.isEmpty() && !isOpaqueId(binding.resumeToken))
        || (!binding.teardownId.isEmpty() && !isOpaqueId(binding.teardownId))
        || (terminalPhase && binding.teardownId.isEmpty())
        || (terminalPhase && !binding.resumeToken.isEmpty())
        || (!terminalPhase && !binding.teardownId.isEmpty())
        || (m_localDeviceId != binding.ownerDeviceId
            && m_localDeviceId != binding.targetDeviceId)) return false;

    const quint64 envelopeLocalConnectionGeneration =
        m_localDeviceId == binding.ownerDeviceId
        ? binding.ownerConnectionGeneration : binding.targetConnectionGeneration;
    if (localConnectionGeneration > 0
        && envelopeLocalConnectionGeneration != localConnectionGeneration) {
        return false;
    }

    const auto previous = m_byId.constFind(binding.remoteSessionId);
    if (previous != m_byId.cend()) {
        if (previous->ownerDeviceId != binding.ownerDeviceId
            || previous->targetDeviceId != binding.targetDeviceId
            || previous->generation > binding.generation
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
            const bool localIsOwner = m_localDeviceId == binding.ownerDeviceId;
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
            if (previous->phase == QLatin1String("Terminating")
                || previous->phase == QLatin1String("CleanupPending")
                || previous->phase == QLatin1String("Closed")
                || binding.ownerConnectionGeneration
                    < previous->ownerConnectionGeneration
                || binding.targetConnectionGeneration
                    < previous->targetConnectionGeneration
                || (binding.ownerConnectionGeneration
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

    const bool localIsOwner = m_localDeviceId == binding.ownerDeviceId;
    const QString peer = m_localDeviceId == binding.ownerDeviceId
        ? binding.targetDeviceId : binding.ownerDeviceId;
    QHash<QString, QString>& directionalIndex = localIsOwner
        ? m_outgoingIdByPeer : m_incomingIdByPeer;
    const QString existingForPeer = directionalIndex.value(peer);
    if (!existingForPeer.isEmpty() && existingForPeer != binding.remoteSessionId) {
        return false;
    }
    // B accepts one incoming controller at a time. An outgoing session to the
    // same peer is intentionally independent and does not participate here.
    if (!localIsOwner && existingForPeer.isEmpty()
        && !m_incomingIdByPeer.isEmpty()) {
        return false;
    }
    if (!terminalPhase && binding.resumeToken.isEmpty()
        && previous != m_byId.cend()) {
        binding.resumeToken = previous->resumeToken;
    }
    if (binding.teardownId.isEmpty() && previous != m_byId.cend()) {
        binding.teardownId = previous->teardownId;
    }
    m_byId.insert(binding.remoteSessionId, binding);
    directionalIndex.insert(peer, binding.remoteSessionId);
    emit sessionChanged(binding.remoteSessionId, binding.generation, binding.phase);
    return true;
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

    const QString ownerDeviceId =
        envelope.value(QStringLiteral("ownerDeviceId")).toString();
    const QString targetDeviceId =
        envelope.value(QStringLiteral("targetDeviceId")).toString();
    const QString teardownId =
        envelope.value(QStringLiteral("teardownId")).toString();
    if (!isOpaqueId(remoteSessionId) || !isDeviceId(ownerDeviceId)
        || !isDeviceId(targetDeviceId) || ownerDeviceId == targetDeviceId
        || !isOpaqueId(teardownId)
        || envelope.value(QStringLiteral("phase")).toString()
            != QLatin1String("Closed")
        || (m_localDeviceId != ownerDeviceId
            && m_localDeviceId != targetDeviceId)) {
        return false;
    }
    const bool localIsOwner = m_localDeviceId == ownerDeviceId;
    const quint64 envelopeLocalConnectionGeneration = localIsOwner
        ? ownerConnectionGeneration : targetConnectionGeneration;
    if (localConnectionGeneration > 0
        && envelopeLocalConnectionGeneration != localConnectionGeneration) {
        return false;
    }

    // A restarted owner may receive the committed Closed tombstone before any
    // terminal state replay. It owns no receiver cache, so accepting this
    // complete authenticated tuple merely resolves stale local UI/state.
    if (iterator == m_byId.cend()) {
        return localIsOwner && !m_closedSessionIds.contains(remoteSessionId);
    }

    const Binding& binding = iterator.value();
    if (ownerDeviceId != binding.ownerDeviceId
        || targetDeviceId != binding.targetDeviceId
        || generation != binding.generation) {
        return false;
    }
    if (!localIsOwner) {
        return (binding.phase == QLatin1String("Terminating")
                || binding.phase == QLatin1String("CleanupPending"))
            && teardownId == binding.teardownId
            && !binding.teardownId.isEmpty()
            && ownerConnectionGeneration == binding.ownerConnectionGeneration
            && targetConnectionGeneration == binding.targetConnectionGeneration;
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
        && targetConnectionGeneration == binding.targetConnectionGeneration;
}

void RemoteSessionCoordinator::remove(const QString& remoteSessionId)
{
    if (isOpaqueId(remoteSessionId)) m_closedSessionIds.insert(remoteSessionId);
    const Binding binding = m_byId.take(remoteSessionId);
    if (binding.remoteSessionId.isEmpty()) return;
    const QString peer = m_localDeviceId == binding.ownerDeviceId
        ? binding.targetDeviceId : binding.ownerDeviceId;
    QHash<QString, QString>& directionalIndex =
        m_localDeviceId == binding.ownerDeviceId
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
    m_closedSessionIds.clear();
    for (const QString& id : ids) emit sessionRemoved(id);
}

RemoteSessionCoordinator::Binding RemoteSessionCoordinator::forPeer(
    const QString& peerDeviceId) const
{
    const Binding outgoing = outgoingForPeer(peerDeviceId);
    return outgoing.remoteSessionId.isEmpty()
        ? incomingForPeer(peerDeviceId) : outgoing;
}

RemoteSessionCoordinator::Binding RemoteSessionCoordinator::outgoingForPeer(
    const QString& peerDeviceId) const
{
    return byId(m_outgoingIdByPeer.value(peerDeviceId));
}

RemoteSessionCoordinator::Binding RemoteSessionCoordinator::incomingForPeer(
    const QString& peerDeviceId) const
{
    return byId(m_incomingIdByPeer.value(peerDeviceId));
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

bool RemoteSessionCoordinator::isDeviceId(const QString& value)
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
