'use strict';

const { randomUUID } = require('node:crypto');

const TERMINAL_PHASES = new Set(['Terminating', 'CleanupPending', 'Closed']);

class RemoteSessionRegistry {
    constructor(options = {}) {
        this.leaseTimeoutMs = Number.isSafeInteger(options.leaseTimeoutMs)
            && options.leaseTimeoutMs > 0 ? options.leaseTimeoutMs : 3000;
        this.tombstoneTtlMs = Number.isSafeInteger(options.tombstoneTtlMs)
            && options.tombstoneTtlMs > 0 ? options.tombstoneTtlMs : 5 * 60 * 1000;
        this.openTimeoutMs = Number.isSafeInteger(options.openTimeoutMs)
            && options.openTimeoutMs > 0 ? options.openTimeoutMs : 3000;
        this.openRequestTtlMs = Number.isSafeInteger(options.openRequestTtlMs)
            && options.openRequestTtlMs > 0
            ? options.openRequestTtlMs : this.tombstoneTtlMs;
        this.maximumTombstones = Number.isSafeInteger(options.maximumTombstones)
            && options.maximumTombstones > 0 ? options.maximumTombstones : 4096;
        this.maximumOpenRequests = Number.isSafeInteger(options.maximumOpenRequests)
            && options.maximumOpenRequests > 0 ? options.maximumOpenRequests : 4096;
        // `now` is retained as the deterministic-test alias. Production passes
        // an explicitly monotonic provider so wall-clock corrections cannot
        // extend or shorten a strict network lease.
        this.now = options.monotonicNow || options.now || (() => Date.now());
        this.epochNow = options.epochNow || options.now || (() => Date.now());
        this.idFactory = options.idFactory || (() => randomUUID());
        this.sessions = new Map();
        this.incomingByTarget = new Map(); // targetEndpointId -> Set(remoteSessionId)
        this.outgoingByOwner = new Map();
        this.sessionByOwnerTarget = new Map(); // ownerEndpointId -> Map(targetEndpointId -> id)
        this.openRequests = new Map(); // owner/runtime/request tuple -> bounded replay binding
        this.tombstones = new Map();
    }

    open(binding) {
        const required = ['ownerEndpointId', 'targetEndpointId', 'ownerRuntimeId', 'targetRuntimeId'];
        if (!binding || required.some(key => typeof binding[key] !== 'string' || !binding[key])) {
            return { ok: false, error: 'invalid_remote_session_binding' };
        }
        if (binding.ownerEndpointId === binding.targetEndpointId) {
            return { ok: false, error: 'self_target_not_allowed' };
        }
        const ownerConnectionGeneration = binding.ownerConnectionGeneration ?? 1;
        const targetConnectionGeneration = binding.targetConnectionGeneration ?? 1;
        if (!Number.isSafeInteger(ownerConnectionGeneration)
            || ownerConnectionGeneration <= 0
            || !Number.isSafeInteger(targetConnectionGeneration)
            || targetConnectionGeneration <= 0) {
            return { ok: false, error: 'invalid_connection_generation' };
        }

        const now = this.now();
        this.#trimOpenRequests(now);
        const requestId = typeof binding.requestId === 'string'
            && binding.requestId.length > 0 && binding.requestId.length <= 128
            ? binding.requestId : null;
        const requestKey = requestId ? this.#openRequestKey(binding, requestId) : null;
        const recorded = requestKey ? this.openRequests.get(requestKey) : null;
        if (recorded) {
            if (recorded.ownerEndpointId !== binding.ownerEndpointId
                || recorded.ownerRuntimeId !== binding.ownerRuntimeId
                || recorded.targetEndpointId !== binding.targetEndpointId) {
                return { ok: false, error: 'request_id_conflict' };
            }
            const recordedSession = this.get(recorded.remoteSessionId)
                || this.getTombstone(recorded.remoteSessionId);
            if (recordedSession) {
                return {
                    ok: true,
                    replay: true,
                    requestReplay: true,
                    session: recordedSession,
                };
            }
            this.openRequests.delete(requestKey);
        }

        const existing = this.forOwnerTarget(
            binding.ownerEndpointId, binding.targetEndpointId);
        if (existing) {
            if (existing.ownerRuntimeId !== binding.ownerRuntimeId
                || existing.targetRuntimeId !== binding.targetRuntimeId) {
                return {
                    ok: false,
                    error: TERMINAL_PHASES.has(existing.phase)
                        ? 'session_cleanup_pending' : 'session_runtime_conflict',
                    remoteSessionId: existing.remoteSessionId,
                };
            }
            if (existing.phase === 'Grace') {
                return {
                    ok: false,
                    error: 'session_requires_resume',
                    remoteSessionId: existing.remoteSessionId,
                };
            }
            if (existing.phase === 'Terminating'
                || existing.phase === 'CleanupPending') {
                return {
                    ok: false,
                    error: 'session_cleanup_pending',
                    remoteSessionId: existing.remoteSessionId,
                };
            }
            if (requestKey) this.#rememberOpenRequest(requestKey, binding, existing, now);
            return { ok: true, replay: true, pairReplay: true, session: existing };
        }

        const remoteSessionId = this.idFactory();
        const awaitingTargetAcceptance = binding.awaitTargetAcceptance === true;
        const session = {
            remoteSessionId,
            resumeToken: this.idFactory(),
            phase: awaitingTargetAcceptance ? 'Opening' : 'Active',
            generation: 1,
            ownerKnownGeneration: 1,
            targetKnownGeneration: 1,
            ownerEndpointId: binding.ownerEndpointId,
            targetEndpointId: binding.targetEndpointId,
            ownerRuntimeId: binding.ownerRuntimeId,
            targetRuntimeId: binding.targetRuntimeId,
            ownerConnectionGeneration,
            targetConnectionGeneration,
            lastContact: new Map([
                [binding.ownerEndpointId, now],
                [binding.targetEndpointId, now],
            ]),
            graceDeadlineAt: null,
            graceDeadlineEpochMs: null,
            graceEndpoints: new Set(),
            degradedEndpoints: new Set(),
            teardownId: null,
            teardownReason: null,
            sceneRunId: null,
            activeUploadIds: new Set(),
            openRequestId: requestId,
            openingDeadlineAt: awaitingTargetAcceptance
                ? now + this.openTimeoutMs : null,
            createdAt: now,
            updatedAt: now,
        };
        this.sessions.set(remoteSessionId, session);
        let incoming = this.incomingByTarget.get(binding.targetEndpointId);
        if (!incoming) {
            incoming = new Set();
            this.incomingByTarget.set(binding.targetEndpointId, incoming);
        }
        incoming.add(remoteSessionId);
        let outgoing = this.outgoingByOwner.get(binding.ownerEndpointId);
        if (!outgoing) {
            outgoing = new Set();
            this.outgoingByOwner.set(binding.ownerEndpointId, outgoing);
        }
        outgoing.add(remoteSessionId);
        let targets = this.sessionByOwnerTarget.get(binding.ownerEndpointId);
        if (!targets) {
            targets = new Map();
            this.sessionByOwnerTarget.set(binding.ownerEndpointId, targets);
        }
        targets.set(binding.targetEndpointId, remoteSessionId);
        if (requestKey) this.#rememberOpenRequest(requestKey, binding, session, now);
        return { ok: true, session };
    }

    get(remoteSessionId) {
        return this.sessions.get(remoteSessionId) || null;
    }

    getTombstone(remoteSessionId) {
        return this.tombstones.get(remoteSessionId) || null;
    }

    incomingForTarget(targetEndpointId) {
        const ids = this.incomingByTarget.get(targetEndpointId);
        if (!ids) return [];
        return Array.from(ids, id => this.get(id)).filter(Boolean);
    }

    forOwnerTarget(ownerEndpointId, targetEndpointId) {
        const id = this.sessionByOwnerTarget.get(ownerEndpointId)?.get(targetEndpointId);
        return id ? this.get(id) : null;
    }

    sessionsForEndpoint(endpointId) {
        const ids = new Set(this.outgoingByOwner.get(endpointId) || []);
        for (const id of this.incomingByTarget.get(endpointId) || []) ids.add(id);
        return Array.from(ids, id => this.get(id))
            .filter(session => session && session.phase !== 'Closed');
    }

    accept({ remoteSessionId, targetEndpointId, targetRuntimeId,
             generation, connectionGeneration }, now = this.now()) {
        const session = this.get(remoteSessionId);
        if (!session) return { ok: false, error: 'unknown_remote_session' };
        if (session.targetEndpointId !== targetEndpointId
            || session.targetRuntimeId !== targetRuntimeId) {
            return { ok: false, error: 'not_session_target' };
        }
        if (session.generation !== generation) {
            return { ok: false, error: 'stale_remote_session_generation' };
        }
        if (session.targetConnectionGeneration !== connectionGeneration) {
            return { ok: false, error: 'stale_connection_generation' };
        }
        if (session.phase === 'Active') {
            return { ok: true, replay: true, session };
        }
        if (session.phase !== 'Opening') {
            return { ok: false, error: 'remote_session_not_opening' };
        }
        if (now >= session.openingDeadlineAt) {
            const terminated = this.terminate(remoteSessionId, 'open_timeout', now);
            return {
                ok: false,
                error: 'remote_session_open_timeout',
                session: terminated.session,
                terminalTransition: terminated.ok && !terminated.replay,
            };
        }
        session.phase = 'Active';
        session.openingDeadlineAt = null;
        session.snapshotSequence = 1;
        session.lastContact.set(targetEndpointId, now);
        session.updatedAt = now;
        return { ok: true, replay: false, session };
    }

    validateLease(remoteSessionId, now = this.now()) {
        const session = this.get(remoteSessionId);
        if (!session) return { ok: false, error: 'unknown_remote_session' };
        if (TERMINAL_PHASES.has(session.phase)) {
            return { ok: false, error: 'session_terminal', session };
        }
        if (this.#leaseExpired(session, now)) {
            const terminated = this.terminate(remoteSessionId, 'lease_expired', now);
            return {
                ok: false,
                error: 'lease_expired',
                session: terminated.session,
                terminalTransition: terminated.ok && !terminated.replay,
            };
        }
        return { ok: true, session };
    }

    touch(remoteSessionId, endpointId, connectionGeneration, now = this.now()) {
        const session = this.get(remoteSessionId);
        if (!session || TERMINAL_PHASES.has(session.phase)) return { ok: false, error: 'session_terminal' };
        const role = this.#role(session, endpointId);
        if (!role) return { ok: false, error: 'not_a_session_party' };
        const expected = role === 'owner'
            ? session.ownerConnectionGeneration : session.targetConnectionGeneration;
        if (connectionGeneration !== expected) return { ok: false, error: 'stale_connection_generation' };
        // Once departure placed this party in Grace, ordinary buffered or late
        // heartbeats must not move its fixed lease deadline. Only a signed
        // resume from the same runtime on a newer transport may recover it.
        if (session.graceEndpoints.has(endpointId)) {
            return { ok: false, error: 'resume_required' };
        }
        // A valid heartbeat received at the exact deadline is already too
        // late. Make the transition irreversible before processing its data.
        if (this.#leaseExpired(session, now)) {
            const terminated = this.terminate(remoteSessionId, 'lease_expired', now);
            return {
                ok: false,
                error: 'lease_expired',
                session: terminated.session,
                terminalTransition: terminated.ok && !terminated.replay,
            };
        }
        session.lastContact.set(endpointId, now);
        const recovered = session.degradedEndpoints.delete(endpointId);
        session.updatedAt = now;
        return { ok: true, session, healthChanged: recovered, degraded: false };
    }

    markDegraded(now = this.now(), thresholdMs = Math.floor(this.leaseTimeoutMs / 2)) {
        const transitions = [];
        const threshold = Math.max(1, Math.min(this.leaseTimeoutMs - 1, thresholdMs));
        for (const session of this.sessions.values()) {
            if (TERMINAL_PHASES.has(session.phase)) continue;
            for (const endpointId of [session.ownerEndpointId, session.targetEndpointId]) {
                const lastContact = session.lastContact.get(endpointId) ?? session.createdAt;
                const degraded = now - lastContact >= threshold;
                const known = session.degradedEndpoints.has(endpointId);
                if (degraded && !known) {
                    session.degradedEndpoints.add(endpointId);
                    transitions.push({ session, endpointId, degraded: true });
                } else if (!degraded && known) {
                    session.degradedEndpoints.delete(endpointId);
                    transitions.push({ session, endpointId, degraded: false });
                }
            }
        }
        return transitions;
    }

    markDisconnected(endpointId, now = this.now()) {
        const changed = [];
        for (const session of this.sessionsForEndpoint(endpointId)) {
            if (TERMINAL_PHASES.has(session.phase)) continue;
            if (session.phase === 'Opening') {
                changed.push(this.terminate(
                    session.remoteSessionId, 'open_party_disconnected', now).session);
                continue;
            }
            if (this.#leaseExpired(session, now)) {
                changed.push(this.terminate(session.remoteSessionId, 'lease_expired', now).session);
                continue;
            }
            session.phase = 'Grace';
            session.graceEndpoints.add(endpointId);
            session.degradedEndpoints.add(endpointId);
            this.#refreshGraceDeadline(session, now);
            session.updatedAt = now;
            changed.push(session);
        }
        return changed;
    }

    resume({ remoteSessionId, endpointId, runtimeId, resumeToken, generation,
             connectionGeneration }, now = this.now()) {
        const session = this.get(remoteSessionId);
        if (!session || session.phase !== 'Grace') return { ok: false, error: 'session_not_resumable' };
        if (this.#leaseExpired(session, now)) {
            const terminated = this.terminate(remoteSessionId, 'lease_expired', now);
            return {
                ok: false,
                error: 'lease_expired',
                session: terminated.session,
                terminalTransition: terminated.ok && !terminated.replay,
            };
        }
        const role = this.#role(session, endpointId);
        if (!role) return { ok: false, error: 'not_a_session_party' };
        if (!session.graceEndpoints.has(endpointId)) {
            return { ok: false, error: 'party_not_in_grace' };
        }
        const expectedRuntime = role === 'owner' ? session.ownerRuntimeId : session.targetRuntimeId;
        if (runtimeId !== expectedRuntime || resumeToken !== session.resumeToken) {
            return { ok: false, error: 'invalid_resume_proof' };
        }
        if (!Number.isSafeInteger(generation) || generation < 1
            || generation !== this.knownGenerationFor(session, endpointId)) {
            return { ok: false, error: 'stale_remote_session_generation' };
        }
        if (!Number.isSafeInteger(connectionGeneration) || connectionGeneration <= 0) {
            return { ok: false, error: 'invalid_connection_generation' };
        }
        const previousConnectionGeneration = role === 'owner'
            ? session.ownerConnectionGeneration : session.targetConnectionGeneration;
        if (connectionGeneration <= previousConnectionGeneration) {
            return { ok: false, error: 'stale_connection_generation' };
        }
        if (role === 'owner') session.ownerConnectionGeneration = connectionGeneration;
        else session.targetConnectionGeneration = connectionGeneration;
        session.lastContact.set(endpointId, now);
        session.graceEndpoints.delete(endpointId);
        session.degradedEndpoints.delete(endpointId);
        if (session.graceEndpoints.size === 0) {
            session.phase = 'Active';
            session.graceDeadlineAt = null;
            session.graceDeadlineEpochMs = null;
        } else {
            this.#refreshGraceDeadline(session, now);
        }
        session.generation += 1;
        if (role === 'owner') session.ownerKnownGeneration = session.generation;
        else session.targetKnownGeneration = session.generation;
        session.updatedAt = now;
        return { ok: true, session };
    }

    knownGenerationFor(sessionOrId, endpointId) {
        const session = typeof sessionOrId === 'string'
            ? (this.get(sessionOrId) || this.getTombstone(sessionOrId))
            : sessionOrId;
        if (!session) return null;
        if (session.ownerEndpointId === endpointId) {
            return Number.isSafeInteger(session.ownerKnownGeneration)
                ? session.ownerKnownGeneration : session.generation;
        }
        if (session.targetEndpointId === endpointId) {
            return Number.isSafeInteger(session.targetKnownGeneration)
                ? session.targetKnownGeneration : session.generation;
        }
        return null;
    }

    markGenerationDelivered(remoteSessionId, endpointId, generation) {
        const session = this.get(remoteSessionId);
        if (!session || generation !== session.generation) return false;
        if (session.ownerEndpointId === endpointId) {
            session.ownerKnownGeneration = generation;
            return true;
        }
        if (session.targetEndpointId === endpointId) {
            session.targetKnownGeneration = generation;
            return true;
        }
        return false;
    }

    terminate(remoteSessionId, reason, now = this.now()) {
        const session = this.get(remoteSessionId);
        if (!session) {
            const tombstone = this.tombstones.get(remoteSessionId);
            return tombstone ? { ok: true, replay: true, session: tombstone }
                : { ok: false, error: 'unknown_remote_session' };
        }
        if (TERMINAL_PHASES.has(session.phase)) return { ok: true, replay: true, session };
        session.phase = 'Terminating';
        session.teardownId = this.idFactory();
        session.teardownReason = String(reason || 'closed').slice(0, 128);
        session.graceDeadlineAt = null;
        session.graceDeadlineEpochMs = null;
        session.openingDeadlineAt = null;
        session.graceEndpoints.clear();
        session.degradedEndpoints.clear();
        session.updatedAt = now;
        return { ok: true, replay: false, session };
    }

    markCleanupPending(remoteSessionId, teardownId, now = this.now()) {
        const session = this.get(remoteSessionId);
        if (!session || typeof teardownId !== 'string' || !teardownId
            || session.teardownId !== teardownId
            || (session.phase !== 'Terminating' && session.phase !== 'CleanupPending')) {
            return { ok: false, error: 'invalid_teardown' };
        }
        session.phase = 'CleanupPending';
        session.updatedAt = now;
        return { ok: true, session };
    }

    acknowledgeCleanup(remoteSessionId, teardownId, targetEndpointId, result, now = this.now()) {
        const session = this.get(remoteSessionId);
        if (!session) {
            const tombstone = this.tombstones.get(remoteSessionId);
            if (tombstone && tombstone.teardownId === teardownId
                && tombstone.targetEndpointId === targetEndpointId
                && this.#isCommittedCleanup(result)) {
                return { ok: true, replay: true, session: tombstone };
            }
            return { ok: false, error: 'unknown_remote_session' };
        }
        if (typeof teardownId !== 'string' || !teardownId
            || session.targetEndpointId !== targetEndpointId
            || session.teardownId !== teardownId
            || (session.phase !== 'Terminating' && session.phase !== 'CleanupPending')) {
            return { ok: false, error: 'invalid_teardown_ack' };
        }
        if (!this.#isCommittedCleanup(result)) {
            session.phase = 'CleanupPending';
            session.cleanupError = result && typeof result.errorCode === 'string'
                ? result.errorCode.slice(0, 128) : 'cleanup_error';
            session.updatedAt = now;
            return { ok: false, error: 'cleanup_not_committed', session };
        }
        session.phase = 'Closed';
        session.closedAt = now;
        session.cleanupResult = { ...result };
        delete session.cleanupError;
        session.updatedAt = now;
        const incoming = this.incomingByTarget.get(session.targetEndpointId);
        if (incoming) {
            incoming.delete(remoteSessionId);
            if (incoming.size === 0) this.incomingByTarget.delete(session.targetEndpointId);
        }
        const outgoing = this.outgoingByOwner.get(session.ownerEndpointId);
        if (outgoing) {
            outgoing.delete(remoteSessionId);
            if (outgoing.size === 0) this.outgoingByOwner.delete(session.ownerEndpointId);
        }
        const targets = this.sessionByOwnerTarget.get(session.ownerEndpointId);
        if (targets && targets.get(session.targetEndpointId) === remoteSessionId) {
            targets.delete(session.targetEndpointId);
            if (targets.size === 0) {
                this.sessionByOwnerTarget.delete(session.ownerEndpointId);
            }
        }
        this.sessions.delete(remoteSessionId);
        this.tombstones.set(remoteSessionId, session);
        this.#trimTombstones(now);
        return { ok: true, replay: false, session };
    }

    tick(now = this.now()) {
        const expired = [];
        for (const session of this.sessions.values()) {
            if (TERMINAL_PHASES.has(session.phase)) continue;
            if (session.phase === 'Opening'
                && Number.isFinite(session.openingDeadlineAt)
                && now >= session.openingDeadlineAt) {
                expired.push(this.terminate(
                    session.remoteSessionId, 'open_timeout', now).session);
            } else if (this.#leaseExpired(session, now)) {
                expired.push(this.terminate(
                    session.remoteSessionId, 'lease_expired', now).session);
            }
        }
        this.#trimTombstones(now);
        this.#trimOpenRequests(now);
        return expired;
    }

    #role(session, endpointId) {
        if (session.ownerEndpointId === endpointId) return 'owner';
        if (session.targetEndpointId === endpointId) return 'target';
        return null;
    }

    #leaseExpired(session, now) {
        return [session.ownerEndpointId, session.targetEndpointId].some(endpointId => {
            const lastContact = session.lastContact.get(endpointId) ?? session.createdAt;
            return now >= lastContact + this.leaseTimeoutMs;
        });
    }

    #refreshGraceDeadline(session, now = this.now()) {
        let deadline = null;
        for (const endpointId of session.graceEndpoints) {
            const lastContact = session.lastContact.get(endpointId) ?? session.createdAt;
            const deviceDeadline = lastContact + this.leaseTimeoutMs;
            deadline = deadline === null ? deviceDeadline : Math.min(deadline, deviceDeadline);
        }
        session.graceDeadlineAt = deadline;
        session.graceDeadlineEpochMs = deadline === null ? null
            : this.epochNow() + Math.max(0, deadline - now);
    }

    #isCommittedCleanup(result) {
        return !!result && result.result === 'committed'
            && result.sceneStopped === true
            && result.uploadsAborted === true
            && result.cacheQuarantined === true;
    }

    #openRequestKey(binding, requestId) {
        return JSON.stringify([
            binding.ownerEndpointId,
            binding.ownerRuntimeId,
            requestId,
        ]);
    }

    #rememberOpenRequest(key, binding, session, now) {
        this.openRequests.set(key, {
            ownerEndpointId: binding.ownerEndpointId,
            ownerRuntimeId: binding.ownerRuntimeId,
            targetEndpointId: binding.targetEndpointId,
            remoteSessionId: session.remoteSessionId,
            expiresAt: now + this.openRequestTtlMs,
        });
        this.#trimOpenRequests(now);
    }

    #trimOpenRequests(now) {
        for (const [key, request] of this.openRequests) {
            if (now >= request.expiresAt) this.openRequests.delete(key);
        }
        while (this.openRequests.size > this.maximumOpenRequests) {
            this.openRequests.delete(this.openRequests.keys().next().value);
        }
    }

    #trimTombstones(now) {
        for (const [id, session] of this.tombstones) {
            if (now - session.closedAt >= this.tombstoneTtlMs) this.tombstones.delete(id);
        }
        while (this.tombstones.size > this.maximumTombstones) {
            this.tombstones.delete(this.tombstones.keys().next().value);
        }
    }
}

module.exports = { RemoteSessionRegistry, TERMINAL_PHASES };
