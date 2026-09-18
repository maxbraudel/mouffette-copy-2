'use strict';

const crypto = require('node:crypto');

const SCENE_PHASES = Object.freeze({
    PREPARING: 'Preparing',
    PREPARED: 'Prepared',
    ARMED: 'Armed',
    SCHEDULED: 'Scheduled',
    LIVE: 'Live',
    STOPPING: 'Stopping',
    STOPPED: 'Stopped',
    FAILED: 'Failed',
});

const PRE_START_PHASES = new Set([
    SCENE_PHASES.PREPARING,
    SCENE_PHASES.PREPARED,
    SCENE_PHASES.ARMED,
    SCENE_PHASES.SCHEDULED,
]);

function isPlainObject(value) {
    if (!value || typeof value !== 'object' || Array.isArray(value)) return false;
    const prototype = Object.getPrototypeOf(value);
    return prototype === Object.prototype || prototype === null;
}

function canonicalize(value) {
    if (value === null || typeof value === 'boolean' || typeof value === 'string') return value;
    if (typeof value === 'number') {
        if (!Number.isFinite(value)) throw new TypeError('non_finite_number');
        return Object.is(value, -0) ? 0 : value;
    }
    if (Array.isArray(value)) return value.map(canonicalize);
    if (!isPlainObject(value)) throw new TypeError('non_json_value');
    const normalized = {};
    for (const key of Object.keys(value).sort()) {
        if (value[key] === undefined) throw new TypeError('undefined_value');
        normalized[key] = canonicalize(value[key]);
    }
    return normalized;
}

function canonicalJson(value) {
    return JSON.stringify(canonicalize(value));
}

function computeSceneDigest(revision, manifest, scene) {
    return crypto.createHash('sha256')
        .update(canonicalJson({ manifest, revision, scene }), 'utf8')
        .digest('hex');
}

class SceneRunRegistry {
    constructor(options = {}) {
        this.prepareTimeoutMs = options.prepareTimeoutMs || 15_000;
        this.activationLeadMs = options.activationLeadMs ?? 500;
        this.maximumActivationLeadMs = options.maximumActivationLeadMs ?? 10_000;
        // First-frame presentation includes real compositor scheduling. It is
        // deliberately distinct from clock-estimation uncertainty: two local
        // macOS/Qt processes can share a precise clock while their visible
        // frame callbacks are hundreds of milliseconds apart under load.
        this.startedAckTimeoutMs = options.startedAckTimeoutMs ?? 5_000;
        this.maximumStartSkewMs = options.maximumStartSkewMs ?? 750;
        this.stopTimeoutMs = options.stopTimeoutMs || 3_000;
        this.maximumClockUncertaintyMs = options.maximumClockUncertaintyMs ?? 50;
        this.tombstoneTtlMs = options.tombstoneTtlMs || 60_000;
        this.maximumTombstones = options.maximumTombstones || 4096;
        this.epochNow = options.epochNow || (() => Date.now());
        this.monotonicNow = options.monotonicNow
            || (() => Number(process.hrtime.bigint() / 1_000_000n));
        this.runs = new Map(); // remoteSessionId -> SceneRun
        this.runToSession = new Map(); // sceneRunId -> remoteSessionId
        this.runByTarget = new Map(); // targetEndpointId -> remoteSessionId
        this.tombstones = new Map(); // sceneRunId -> terminal summary
    }

    activationLeadCeilingMs() {
        return Math.min(this.maximumActivationLeadMs,
            this.activationLeadMs + 2 * this.maximumClockUncertaintyMs);
    }

    getForSession(remoteSessionId) {
        return this.runs.get(remoteSessionId) || null;
    }

    get(sceneRunId) {
        const sessionId = this.runToSession.get(sceneRunId);
        return sessionId ? this.getForSession(sessionId) : null;
    }

    prepare(binding) {
        const current = this.getForSession(binding && binding.remoteSessionId);
        if (current) {
            if (current.sceneRunId === binding.sceneRunId
                && current.digest === binding.digest
                && current.revision === binding.revision) {
                return { ok: true, replay: true, run: current };
            }
            return { ok: false, error: 'scene_run_already_active' };
        }
        const targetSessionId = binding && this.runByTarget.get(binding.targetEndpointId);
        if (targetSessionId && targetSessionId !== binding.remoteSessionId) {
            return { ok: false, error: 'target_scene_already_running' };
        }
        if (this.tombstones.has(binding && binding.sceneRunId)) {
            return { ok: false, error: 'scene_run_id_reused' };
        }
        const requiredStrings = [
            'remoteSessionId', 'sceneRunId', 'ownerEndpointId', 'targetEndpointId', 'digest',
        ];
        if (!binding || requiredStrings.some(key => typeof binding[key] !== 'string'
            || binding[key].length < 1)) {
            return { ok: false, error: 'invalid_scene_binding' };
        }
        if (!Number.isSafeInteger(binding.generation) || binding.generation < 1
            || !Number.isSafeInteger(binding.revision) || binding.revision < 1
            || !Array.isArray(binding.manifest) || !isPlainObject(binding.scene)) {
            return { ok: false, error: 'invalid_scene_binding' };
        }

        const now = this.epochNow();
        const nowMonotonic = this.monotonicNow();
        const run = {
            remoteSessionId: binding.remoteSessionId,
            generation: binding.generation,
            sceneRunId: binding.sceneRunId,
            revision: binding.revision,
            digest: binding.digest,
            manifest: binding.manifest,
            scene: binding.scene,
            ownerEndpointId: binding.ownerEndpointId,
            targetEndpointId: binding.targetEndpointId,
            phase: SCENE_PHASES.PREPARING,
            preparedEndpoints: new Set(),
            armedEndpoints: new Set(),
            armedClockUncertaintyByEndpoint: new Map(),
            startedEndpoints: new Set(),
            presentedServerMonotonicByEndpoint: new Map(),
            stoppedEndpoints: new Set(),
            progressByEndpoint: new Map(),
            lastSnapshotSequence: 0,
            createdAt: now,
            updatedAt: now,
            prepareDeadlineAt: now + this.prepareTimeoutMs,
            prepareDeadlineServerMonotonicMs: nowMonotonic + this.prepareTimeoutMs,
            startEpochMs: null,
            startServerMonotonicMs: null,
            activationLeadMs: null,
            startedDeadlineAt: null,
            startedDeadlineServerMonotonicMs: null,
            startSkewMs: null,
            stopDeadlineAt: null,
            stopDeadlineServerMonotonicMs: null,
            stopReason: null,
            failed: false,
        };
        this.runs.set(run.remoteSessionId, run);
        this.runToSession.set(run.sceneRunId, run.remoteSessionId);
        this.runByTarget.set(run.targetEndpointId, run.remoteSessionId);
        return { ok: true, replay: false, run };
    }

    recordProgress(sceneRunId, endpointId, progress) {
        const run = this.get(sceneRunId);
        if (!run || run.phase !== SCENE_PHASES.PREPARING) {
            return { ok: false, error: 'scene_not_preparing' };
        }
        if (!this.#isParty(run, endpointId)) return { ok: false, error: 'not_a_scene_party' };
        run.progressByEndpoint.set(endpointId, progress);
        run.updatedAt = this.epochNow();
        return { ok: true, run };
    }

    markPrepared(sceneRunId, endpointId, digest) {
        const run = this.get(sceneRunId);
        if (!run) {
            return { ok: false, error: 'scene_not_preparing' };
        }
        if (!this.#isParty(run, endpointId)) return { ok: false, error: 'not_a_scene_party' };
        if (digest !== run.digest) return { ok: false, error: 'scene_digest_mismatch' };
        if (![SCENE_PHASES.PREPARING, SCENE_PHASES.PREPARED].includes(run.phase)) {
            if (run.preparedEndpoints.has(endpointId)
                && [SCENE_PHASES.ARMED, SCENE_PHASES.SCHEDULED, SCENE_PHASES.LIVE]
                    .includes(run.phase)) {
                return { ok: true, replay: true, ready: true, run };
            }
            return { ok: false, error: 'scene_not_preparing' };
        }
        if (this.monotonicNow() >= run.prepareDeadlineServerMonotonicMs) {
            return { ok: false, error: 'scene_prepare_timeout' };
        }
        const replay = run.preparedEndpoints.has(endpointId);
        run.preparedEndpoints.add(endpointId);
        if (run.preparedEndpoints.size === 2) run.phase = SCENE_PHASES.PREPARED;
        run.updatedAt = this.epochNow();
        return { ok: true, replay, ready: run.phase === SCENE_PHASES.PREPARED, run };
    }

    arm(sceneRunId, endpointId, digest, clockUncertaintyMs) {
        const run = this.get(sceneRunId);
        if (!run) {
            return { ok: false, error: 'scene_not_prepared' };
        }
        if (!this.#isParty(run, endpointId)) return { ok: false, error: 'not_a_scene_party' };
        if (digest !== run.digest) return { ok: false, error: 'scene_digest_mismatch' };
        if (!Number.isFinite(clockUncertaintyMs) || clockUncertaintyMs < 0
            || clockUncertaintyMs > this.maximumClockUncertaintyMs) {
            return { ok: false, error: 'clock_uncertainty_too_high' };
        }
        if (![SCENE_PHASES.PREPARED, SCENE_PHASES.ARMED].includes(run.phase)) {
            if (run.armedEndpoints.has(endpointId)
                && [SCENE_PHASES.SCHEDULED, SCENE_PHASES.LIVE].includes(run.phase)) {
                // A resumed transport has acquired a new clock mapping. Keep
                // the immutable COMMIT deadline, but use the new reporter bound
                // when validating any subsequent STARTED timestamp.
                run.armedClockUncertaintyByEndpoint.set(
                    endpointId, clockUncertaintyMs);
                run.updatedAt = this.epochNow();
                return { ok: true, replay: true, scheduled: true, run };
            }
            return { ok: false, error: 'scene_not_prepared' };
        }
        if (this.monotonicNow() >= run.prepareDeadlineServerMonotonicMs) {
            return { ok: false, error: 'scene_prepare_timeout' };
        }
        const replay = run.armedEndpoints.has(endpointId);
        run.armedEndpoints.add(endpointId);
        run.armedClockUncertaintyByEndpoint.set(endpointId, clockUncertaintyMs);
        run.phase = SCENE_PHASES.ARMED;
        run.updatedAt = this.epochNow();
        if (run.armedEndpoints.size === 2) {
            const nowEpoch = this.epochNow();
            const nowMonotonic = this.monotonicNow();
            const maximumReportedUncertaintyMs = Math.max(
                ...run.armedClockUncertaintyByEndpoint.values());
            // clockUncertaintyMs is half the measured network RTT. Reserve up
            // to one complete RTT for the COMMIT to reach either endpoint,
            // then preserve the configured base lead as presentation margin.
            run.activationLeadMs = Math.min(this.maximumActivationLeadMs,
                this.activationLeadMs + 2 * maximumReportedUncertaintyMs);
            run.phase = SCENE_PHASES.SCHEDULED;
            run.startEpochMs = nowEpoch + run.activationLeadMs;
            run.startServerMonotonicMs = nowMonotonic + run.activationLeadMs;
            run.startedDeadlineAt = run.startEpochMs + this.startedAckTimeoutMs;
            run.startedDeadlineServerMonotonicMs =
                run.startServerMonotonicMs + this.startedAckTimeoutMs;
            run.updatedAt = nowEpoch;
        }
        return {
            ok: true,
            replay,
            scheduled: run.phase === SCENE_PHASES.SCHEDULED,
            run,
        };
    }

    markStarted(sceneRunId, endpointId, digest, firstFramePresented,
                presentedServerMonotonicMs) {
        const run = this.get(sceneRunId);
        if (!run || ![SCENE_PHASES.SCHEDULED, SCENE_PHASES.LIVE].includes(run.phase)) {
            return { ok: false, error: 'scene_not_scheduled' };
        }
        if (!this.#isParty(run, endpointId)) return { ok: false, error: 'not_a_scene_party' };
        if (digest !== run.digest) return { ok: false, error: 'scene_digest_mismatch' };
        if (firstFramePresented !== true) return { ok: false, error: 'first_frame_not_presented' };
        if (!Number.isSafeInteger(presentedServerMonotonicMs)
            || presentedServerMonotonicMs < 0) {
            return { ok: false, error: 'invalid_first_frame_timestamp' };
        }
        const previousTimestamp = run.presentedServerMonotonicByEndpoint.get(endpointId);
        if (previousTimestamp !== undefined) {
            if (previousTimestamp !== presentedServerMonotonicMs) {
                return { ok: false, error: 'conflicting_started_ack' };
            }
            return {
                ok: true,
                replay: true,
                live: run.phase === SCENE_PHASES.LIVE,
                startSkewMs: run.startSkewMs,
                run,
            };
        }

        const now = this.epochNow();
        const nowMonotonic = this.monotonicNow();
        const reporterUncertainty = run.armedClockUncertaintyByEndpoint.get(endpointId);
        if (!Number.isFinite(reporterUncertainty)
            || reporterUncertainty < 0
            || reporterUncertainty > this.maximumClockUncertaintyMs) {
            return { ok: false, error: 'clock_uncertainty_too_high' };
        }
        if (run.phase === SCENE_PHASES.SCHEDULED
            && nowMonotonic + this.maximumClockUncertaintyMs
                < run.startServerMonotonicMs) {
            return { ok: false, error: 'first_frame_presented_before_commit' };
        }
        if (run.phase === SCENE_PHASES.SCHEDULED
            && nowMonotonic >= run.startedDeadlineServerMonotonicMs) {
            return { ok: false, error: 'scene_started_timeout' };
        }
        if (presentedServerMonotonicMs
            < run.startServerMonotonicMs - this.maximumClockUncertaintyMs) {
            return { ok: false, error: 'first_frame_presented_before_commit' };
        }
        if (presentedServerMonotonicMs >= run.startedDeadlineServerMonotonicMs) {
            return { ok: false, error: 'scene_started_timeout' };
        }
        if (presentedServerMonotonicMs > nowMonotonic + reporterUncertainty) {
            return { ok: false, error: 'first_frame_timestamp_in_future' };
        }

        const peerTimestamp = Array.from(
            run.presentedServerMonotonicByEndpoint.values())[0];
        const observedSkewMs = peerTimestamp === undefined
            ? null : Math.abs(presentedServerMonotonicMs - peerTimestamp);
        if (observedSkewMs !== null
            && observedSkewMs > this.maximumStartSkewMs) {
            run.startSkewMs = observedSkewMs;
            return {
                ok: false,
                error: 'scene_start_skew_too_high',
                startSkewMs: observedSkewMs,
                run,
            };
        }
        if (presentedServerMonotonicMs > run.startServerMonotonicMs + this.maximumStartSkewMs) {
            return { ok: false, error: 'scene_commit_deadline_missed' };
        }
        const replay = run.startedEndpoints.has(endpointId);
        run.startedEndpoints.add(endpointId);
        run.presentedServerMonotonicByEndpoint.set(endpointId,
            presentedServerMonotonicMs);
        if (run.startedEndpoints.size === 2) {
            run.startSkewMs = observedSkewMs;
            run.phase = SCENE_PHASES.LIVE;
        }
        run.updatedAt = now;
        return {
            ok: true,
            replay,
            live: run.phase === SCENE_PHASES.LIVE,
            startSkewMs: run.startSkewMs,
            run,
        };
    }

    acceptSnapshot(sceneRunId, endpointId, digest, sequence) {
        const run = this.get(sceneRunId);
        if (!run || run.phase !== SCENE_PHASES.LIVE) {
            return { ok: false, error: 'scene_not_live' };
        }
        if (endpointId !== run.ownerEndpointId) return { ok: false, error: 'not_scene_owner' };
        if (digest !== run.digest) return { ok: false, error: 'scene_digest_mismatch' };
        if (!Number.isSafeInteger(sequence) || sequence <= run.lastSnapshotSequence) {
            return { ok: false, error: 'stale_snapshot_sequence' };
        }
        run.lastSnapshotSequence = sequence;
        run.updatedAt = this.epochNow();
        return { ok: true, run };
    }

    stop(sceneRunId, reason = 'stopped', options = {}) {
        const run = this.get(sceneRunId);
        if (!run) {
            const tombstone = this.tombstones.get(sceneRunId);
            return tombstone
                ? { ok: true, replay: true, completed: true, run: tombstone }
                : { ok: false, error: 'unknown_scene_run' };
        }
        if (run.phase === SCENE_PHASES.STOPPING) {
            return { ok: true, replay: true, completed: false, run };
        }
        run.phase = SCENE_PHASES.STOPPING;
        run.stopReason = String(reason || 'stopped').slice(0, 128);
        run.failed = options.failed === true;
        const nowEpoch = Number.isFinite(options.nowEpoch)
            ? options.nowEpoch : this.epochNow();
        const nowMonotonic = Number.isFinite(options.nowMonotonic)
            ? options.nowMonotonic : this.monotonicNow();
        run.updatedAt = nowEpoch;
        run.stopDeadlineAt = run.updatedAt + this.stopTimeoutMs;
        run.stopDeadlineServerMonotonicMs = nowMonotonic + this.stopTimeoutMs;
        return { ok: true, replay: false, completed: false, run };
    }

    acknowledgeStopped(sceneRunId, endpointId, success) {
        const run = this.get(sceneRunId);
        if (!run) {
            const tombstone = this.tombstones.get(sceneRunId);
            return tombstone
                ? { ok: true, replay: true, completed: true, run: tombstone }
                : { ok: false, error: 'unknown_scene_run' };
        }
        if (run.phase !== SCENE_PHASES.STOPPING) {
            return { ok: false, error: 'scene_not_stopping' };
        }
        if (!this.#isParty(run, endpointId)) return { ok: false, error: 'not_a_scene_party' };
        if (success !== true) return { ok: false, error: 'scene_stop_failed', run };
        const replay = run.stoppedEndpoints.has(endpointId);
        run.stoppedEndpoints.add(endpointId);
        run.updatedAt = this.epochNow();
        if (run.stoppedEndpoints.size < 2) {
            return { ok: true, replay, completed: false, run };
        }
        const terminal = this.#finalize(run, run.failed
            ? SCENE_PHASES.FAILED : SCENE_PHASES.STOPPED);
        return { ok: true, replay, completed: true, run: terminal };
    }

    forceFinalize(sceneRunId, failed = false, timing = {}) {
        const run = this.get(sceneRunId);
        if (!run) return this.tombstones.get(sceneRunId) || null;
        run.failed = run.failed || failed;
        return this.#finalize(run,
            run.failed ? SCENE_PHASES.FAILED : SCENE_PHASES.STOPPED, timing);
    }

    tick(nowEpoch = this.epochNow(), nowMonotonic = this.monotonicNow()) {
        void nowEpoch;
        const actions = [];
        for (const run of Array.from(this.runs.values())) {
            if ([SCENE_PHASES.PREPARING, SCENE_PHASES.PREPARED, SCENE_PHASES.ARMED]
                .includes(run.phase)
                && nowMonotonic >= run.prepareDeadlineServerMonotonicMs) {
                this.stop(run.sceneRunId, 'scene_prepare_timeout', {
                    failed: true, nowEpoch, nowMonotonic,
                });
                actions.push({ type: 'stop', code: 'scene_prepare_timeout', run });
            } else if (run.phase === SCENE_PHASES.SCHEDULED
                && nowMonotonic >= run.startedDeadlineServerMonotonicMs) {
                const code = run.recoveryDeadlineServerMonotonicMs
                    ? 'scene_commit_deadline_missed' : 'scene_started_timeout';
                this.stop(run.sceneRunId, code, {
                    failed: true, nowEpoch, nowMonotonic,
                });
                actions.push({ type: 'stop', code, run });
            } else if (run.phase === SCENE_PHASES.STOPPING
                && nowMonotonic >= run.stopDeadlineServerMonotonicMs) {
                const terminal = this.forceFinalize(run.sceneRunId, run.failed, {
                    nowEpoch, nowMonotonic,
                });
                actions.push({ type: 'finalized', code: 'scene_stop_timeout', run: terminal });
            }
        }
        this.#trimTombstones(nowMonotonic);
        return actions;
    }

    isPreStart(run) {
        return !!run && PRE_START_PHASES.has(run.phase);
    }

    #isParty(run, endpointId) {
        return run.ownerEndpointId === endpointId || run.targetEndpointId === endpointId;
    }

    #finalize(run, phase, timing = {}) {
        run.phase = phase;
        run.updatedAt = Number.isFinite(timing.nowEpoch)
            ? timing.nowEpoch : this.epochNow();
        run.closedAt = run.updatedAt;
        run.closedServerMonotonicMs = Number.isFinite(timing.nowMonotonic)
            ? timing.nowMonotonic : this.monotonicNow();
        this.runs.delete(run.remoteSessionId);
        this.runToSession.delete(run.sceneRunId);
        if (this.runByTarget.get(run.targetEndpointId) === run.remoteSessionId) {
            this.runByTarget.delete(run.targetEndpointId);
        }
        this.tombstones.set(run.sceneRunId, run);
        this.#trimTombstones(run.closedServerMonotonicMs);
        return run;
    }

    #trimTombstones(nowMonotonic = this.monotonicNow()) {
        for (const [id, run] of this.tombstones) {
            if (Number.isFinite(run.closedServerMonotonicMs)
                && nowMonotonic - run.closedServerMonotonicMs >= this.tombstoneTtlMs) {
                this.tombstones.delete(id);
            }
        }
        while (this.tombstones.size > this.maximumTombstones) {
            this.tombstones.delete(this.tombstones.keys().next().value);
        }
    }
}

module.exports = {
    PRE_START_PHASES,
    SCENE_PHASES,
    SceneRunRegistry,
    canonicalJson,
    computeSceneDigest,
    isPlainObject,
};
