'use strict';

const { randomUUID } = require('node:crypto');

const TERMINAL_PHASES = new Set(['Terminating', 'CleanupPending', 'Closed']);

class RemoteSessionRegistry {
    constructor(options = {}) {
        this.leaseTimeoutMs = Number.isSafeInteger(options.leaseTimeoutMs)
            && options.leaseTimeoutMs > 0 ? options.leaseTimeoutMs : 3000;
        this.tombstoneTtlMs = Number.isSafeInteger(options.tombstoneTtlMs)
            && options.tombstoneTtlMs > 0 ? options.tombstoneTtlMs : 5 * 60 * 1000;
        this.maximumTombstones = Number.isSafeInteger(options.maximumTombstones)
            && options.maximumTombstones > 0 ? options.maximumTombstones : 4096;
        // `now` is retained as the deterministic-test alias. Production passes
        // an explicitly monotonic provider so wall-clock corrections cannot
        // extend or shorten a strict network lease.
        this.now = options.monotonicNow || options.now || (() => Date.now());
        this.epochNow = options.epochNow || options.now || (() => Date.now());
        this.idFactory = options.idFactory || (() => randomUUID());
        this.sessions = new Map();
        this.incomingByTarget = new Map();
        this.outgoingByOwner = new Map();
        this.tombstones = new Map();
    }

    open(binding) {
        const required = ['ownerDeviceId', 'targetDeviceId', 'ownerRuntimeId', 'targetRuntimeId'];
        if (!binding || required.some(key => typeof binding[key] !== 'string' || !binding[key])) {
            return { ok: false, error: 'invalid_remote_session_binding' };
        }
        if (binding.ownerDeviceId === binding.targetDeviceId) {
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
        const existingId = this.incomingByTarget.get(binding.targetDeviceId);
        const existing = existingId && this.sessions.get(existingId);
        if (existing && existing.phase !== 'Closed') {
            return { ok: false, error: 'target_in_use', remoteSessionId: existing.remoteSessionId };
        }

        const now = this.now();
        const remoteSessionId = this.idFactory();
        const session = {
            remoteSessionId,
            resumeToken: this.idFactory(),
            phase: 'Active',
            generation: 1,
            ownerKnownGeneration: 1,
            targetKnownGeneration: 1,
            ownerDeviceId: binding.ownerDeviceId,
            targetDeviceId: binding.targetDeviceId,
            ownerRuntimeId: binding.ownerRuntimeId,
            targetRuntimeId: binding.targetRuntimeId,
            ownerConnectionGeneration,
            targetConnectionGeneration,
            lastContact: new Map([
                [binding.ownerDeviceId, now],
                [binding.targetDeviceId, now],
            ]),
            graceDeadlineAt: null,
            graceDeadlineEpochMs: null,
            graceDevices: new Set(),
            degradedDevices: new Set(),
            teardownId: null,
            teardownReason: null,
            sceneRunId: null,
            activeUploadIds: new Set(),
            createdAt: now,
            updatedAt: now,
        };
        this.sessions.set(remoteSessionId, session);
        this.incomingByTarget.set(binding.targetDeviceId, remoteSessionId);
        let outgoing = this.outgoingByOwner.get(binding.ownerDeviceId);
        if (!outgoing) {
            outgoing = new Set();
            this.outgoingByOwner.set(binding.ownerDeviceId, outgoing);
        }
        outgoing.add(remoteSessionId);
        return { ok: true, session };
    }

    get(remoteSessionId) {
        return this.sessions.get(remoteSessionId) || null;
    }

    getTombstone(remoteSessionId) {
        return this.tombstones.get(remoteSessionId) || null;
    }

    activeIncomingFor(targetDeviceId) {
        const id = this.incomingByTarget.get(targetDeviceId);
        return id ? this.get(id) : null;
    }

    sessionsForDevice(deviceId) {
        return Array.from(this.sessions.values()).filter(session =>
            session.phase !== 'Closed'
            && (session.ownerDeviceId === deviceId || session.targetDeviceId === deviceId));
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

    touch(remoteSessionId, deviceId, connectionGeneration, now = this.now()) {
        const session = this.get(remoteSessionId);
        if (!session || TERMINAL_PHASES.has(session.phase)) return { ok: false, error: 'session_terminal' };
        const role = this.#role(session, deviceId);
        if (!role) return { ok: false, error: 'not_a_session_party' };
        const expected = role === 'owner'
            ? session.ownerConnectionGeneration : session.targetConnectionGeneration;
        if (connectionGeneration !== expected) return { ok: false, error: 'stale_connection_generation' };
        // Once departure placed this party in Grace, ordinary buffered or late
        // heartbeats must not move its fixed lease deadline. Only a signed
        // resume from the same runtime on a newer transport may recover it.
        if (session.graceDevices.has(deviceId)) {
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
        session.lastContact.set(deviceId, now);
        const recovered = session.degradedDevices.delete(deviceId);
        session.updatedAt = now;
        return { ok: true, session, healthChanged: recovered, degraded: false };
    }

    markDegraded(now = this.now(), thresholdMs = Math.floor(this.leaseTimeoutMs / 2)) {
        const transitions = [];
        const threshold = Math.max(1, Math.min(this.leaseTimeoutMs - 1, thresholdMs));
        for (const session of this.sessions.values()) {
            if (TERMINAL_PHASES.has(session.phase)) continue;
            for (const deviceId of [session.ownerDeviceId, session.targetDeviceId]) {
                const lastContact = session.lastContact.get(deviceId) ?? session.createdAt;
                const degraded = now - lastContact >= threshold;
                const known = session.degradedDevices.has(deviceId);
                if (degraded && !known) {
                    session.degradedDevices.add(deviceId);
                    transitions.push({ session, deviceId, degraded: true });
                } else if (!degraded && known) {
                    session.degradedDevices.delete(deviceId);
                    transitions.push({ session, deviceId, degraded: false });
                }
            }
        }
        return transitions;
    }

    markDisconnected(deviceId, now = this.now()) {
        const changed = [];
        for (const session of this.sessionsForDevice(deviceId)) {
            if (TERMINAL_PHASES.has(session.phase)) continue;
            if (this.#leaseExpired(session, now)) {
                changed.push(this.terminate(session.remoteSessionId, 'lease_expired', now).session);
                continue;
            }
            session.phase = 'Grace';
            session.graceDevices.add(deviceId);
            session.degradedDevices.add(deviceId);
            this.#refreshGraceDeadline(session, now);
            session.updatedAt = now;
            changed.push(session);
        }
        return changed;
    }

    resume({ remoteSessionId, deviceId, runtimeId, resumeToken, generation,
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
        const role = this.#role(session, deviceId);
        if (!role) return { ok: false, error: 'not_a_session_party' };
        if (!session.graceDevices.has(deviceId)) {
            return { ok: false, error: 'party_not_in_grace' };
        }
        const expectedRuntime = role === 'owner' ? session.ownerRuntimeId : session.targetRuntimeId;
        if (runtimeId !== expectedRuntime || resumeToken !== session.resumeToken) {
            return { ok: false, error: 'invalid_resume_proof' };
        }
        if (!Number.isSafeInteger(generation) || generation < 1
            || generation !== this.knownGenerationFor(session, deviceId)) {
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
        session.lastContact.set(deviceId, now);
        session.graceDevices.delete(deviceId);
        session.degradedDevices.delete(deviceId);
        if (session.graceDevices.size === 0) {
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

    knownGenerationFor(sessionOrId, deviceId) {
        const session = typeof sessionOrId === 'string'
            ? (this.get(sessionOrId) || this.getTombstone(sessionOrId))
            : sessionOrId;
        if (!session) return null;
        if (session.ownerDeviceId === deviceId) {
            return Number.isSafeInteger(session.ownerKnownGeneration)
                ? session.ownerKnownGeneration : session.generation;
        }
        if (session.targetDeviceId === deviceId) {
            return Number.isSafeInteger(session.targetKnownGeneration)
                ? session.targetKnownGeneration : session.generation;
        }
        return null;
    }

    markGenerationDelivered(remoteSessionId, deviceId, generation) {
        const session = this.get(remoteSessionId);
        if (!session || generation !== session.generation) return false;
        if (session.ownerDeviceId === deviceId) {
            session.ownerKnownGeneration = generation;
            return true;
        }
        if (session.targetDeviceId === deviceId) {
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
        session.graceDevices.clear();
        session.degradedDevices.clear();
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

    acknowledgeCleanup(remoteSessionId, teardownId, targetDeviceId, result, now = this.now()) {
        const session = this.get(remoteSessionId);
        if (!session) {
            const tombstone = this.tombstones.get(remoteSessionId);
            if (tombstone && tombstone.teardownId === teardownId
                && tombstone.targetDeviceId === targetDeviceId
                && this.#isCommittedCleanup(result)) {
                return { ok: true, replay: true, session: tombstone };
            }
            return { ok: false, error: 'unknown_remote_session' };
        }
        if (typeof teardownId !== 'string' || !teardownId
            || session.targetDeviceId !== targetDeviceId
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
        this.incomingByTarget.delete(session.targetDeviceId);
        const outgoing = this.outgoingByOwner.get(session.ownerDeviceId);
        if (outgoing) {
            outgoing.delete(remoteSessionId);
            if (outgoing.size === 0) this.outgoingByOwner.delete(session.ownerDeviceId);
        }
        this.sessions.delete(remoteSessionId);
        this.tombstones.set(remoteSessionId, session);
        this.#trimTombstones(now);
        return { ok: true, replay: false, session };
    }

    tick(now = this.now()) {
        const expired = [];
        for (const session of this.sessions.values()) {
            if (!TERMINAL_PHASES.has(session.phase) && this.#leaseExpired(session, now)) {
                expired.push(this.terminate(session.remoteSessionId, 'lease_expired', now).session);
            }
        }
        this.#trimTombstones(now);
        return expired;
    }

    #role(session, deviceId) {
        if (session.ownerDeviceId === deviceId) return 'owner';
        if (session.targetDeviceId === deviceId) return 'target';
        return null;
    }

    #leaseExpired(session, now) {
        return [session.ownerDeviceId, session.targetDeviceId].some(deviceId => {
            const lastContact = session.lastContact.get(deviceId) ?? session.createdAt;
            return now >= lastContact + this.leaseTimeoutMs;
        });
    }

    #refreshGraceDeadline(session, now = this.now()) {
        let deadline = null;
        for (const deviceId of session.graceDevices) {
            const lastContact = session.lastContact.get(deviceId) ?? session.createdAt;
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
