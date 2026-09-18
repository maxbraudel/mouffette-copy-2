'use strict';

const assert = require('node:assert/strict');
const fs = require('node:fs/promises');
const os = require('node:os');
const path = require('node:path');
const { MouffetteServer } = require('./server');
const { RemoteSessionRegistry, TERMINAL_PHASES } = require('./remote_session_registry');
const { RemoteCacheStore } = require('./remote_cache_store');
const { ProtocolMetrics } = require('./protocol_metrics');
const { SCENE_PHASES } = require('./scene_run_registry');

// Fixed seed and virtual monotonic time make every injected fault repeatable.
const SEED = 0x6d6f7566;
let randomState = SEED;
function random(bound) {
    randomState ^= randomState << 13;
    randomState ^= randomState >>> 17;
    randomState ^= randomState << 5;
    return (randomState >>> 0) % bound;
}

function protocolSoak() {
    let now = 100000;
    let wallOffset = 1700000000000;
    let serial = 0;
    const statistics = { cycles: 0, departures: 0, resumes: 0, duplicateResumes: 0,
        lostReplies: 0, lostOwnerStateAcks: 0, lostTargetStateAcks: 0,
        lostCleanupAcks: 0, cleanupErrors: 0, expired: 0, wallJumps: 0 };
    const server = new MouffetteServer({ port: 0, monotonicNow: () => now,
        epochNow: () => wallOffset + now, protocolLogger: () => {},
        metrics: new ProtocolMetrics({ logger: () => {}, maximumOnceKeys: 64 }) });
    server.remoteSessions = new RemoteSessionRegistry({ leaseTimeoutMs: 4500, recoveryTimeoutMs: 3000,
        openTimeoutMs: 5000, tombstoneTtlMs: 10000, openRequestTtlMs: 10000,
        maximumTombstones: 32, maximumOpenRequests: 32, maximumSessions: 16,
        monotonicNow: () => now, epochNow: () => wallOffset + now,
        idFactory: () => `soak-${++serial}` });
    server.sceneRuns.maximumTombstones = 32;
    const generations = new Map();
    const attach = id => {
        const generation = (generations.get(id) || 0) + 1;
        generations.set(id, generation);
        const ws = { readyState: 1, bufferedAmount: 0, messages: [], drop: new Set(),
            send(encoded) {
                const message = JSON.parse(encoded);
                if (this.drop.has(message.type)) { ++statistics.lostReplies; return; }
                this.messages.push(message);
                if (this.messages.length > 64) this.messages.shift();
            }, close() { this.readyState = 3; } };
        const client = { id, endpointId: id, installationId: `installation-${id}`,
            instanceId: 'primary', instanceOrdinal: 1, runtimeId: `runtime-${id}`,
            authenticated: true, connectionGeneration: generation, machineName: id,
            platform: 'test', screens: [], ws, lastHeartbeatAt: wallOffset + now,
            lastHeartbeatMonotonicAt: now };
        server.clients.set(id, client);
        server.currentTransportByEndpoint.set(id, client);
        server.connectionGenerationSequence = Math.max(server.connectionGenerationSequence, generation);
        server.handleEndpointSnapshot(id, { machineName: id, platform: 'test',
            instanceOrdinal: 1, screens: [], systemUI: [], volumePercent: 50 });
        return client;
    };
    const send = (id, type, fields = {}) => {
        const client = server.clients.get(id);
        assert.ok(client, `missing sender ${id}`);
        server.handleMessage(id, { type, protocolVersion: 9, serverBootId: server.serverBootId,
            connectionGeneration: client.connectionGeneration,
            messageId: `00000000-0000-4000-8000-${String(++serial).padStart(12, '0')}`,
            ...fields });
    };
    const pulse = () => {
        for (const id of server.clients.keys()) send(id, 'heartbeat', { sequence: ++serial });
    };
    const elapse = duration => {
        const finish = now + duration;
        while (now < finish) {
            now = Math.min(finish, now + 250 + random(250));
            if (random(13) === 0) {
                wallOffset += (random(2) ? 1 : -1) * 86400000;
                ++statistics.wallJumps;
            }
            pulse();
            server.sweepRemoteSessionLeases(now);
        }
    };
    const detach = id => {
        const client = server.clients.get(id);
        server.handleRemoteSessionDeparture(client, now);
        server.retireClientTransport(client, true, 1001, 'soak partition');
        ++statistics.departures;
    };
    const acknowledge = session => {
        const parties = [session.ownerEndpointId, session.targetEndpointId];
        const lostIndex = random(3);
        for (const [index, id] of parties.entries()) {
            if (index === lostIndex) continue;
            send(id, 'remote_session_state_ack', { remoteSessionId: session.remoteSessionId,
                generation: session.generation, stateRevision: session.stateRevision });
        }
        if (lostIndex < 2) {
            ++statistics[lostIndex === 0 ? 'lostOwnerStateAcks' : 'lostTargetStateAcks'];
            assert.equal(server.remoteSessions.commandReady(session), false);
            const revision = session.stateRevision;
            const id = parties[lostIndex];
            send(id, 'heartbeat', { sequence: ++serial });
            const heartbeat = server.clients.get(id).ws.messages
                .filter(message => message.type === 'heartbeat_ack').at(-1);
            const state = heartbeat.sessionStates.find(state =>
                state.remoteSessionId === session.remoteSessionId);
            assert.equal(state.commandReady, false);
            // The client has already applied this exact tuple. Its heartbeat
            // response supplies evidence that its first ACK needs retrying.
            send(id, 'remote_session_state_ack', { remoteSessionId: state.remoteSessionId,
                generation: state.generation, stateRevision: state.stateRevision });
            assert.equal(session.stateRevision, revision, 'readiness ACKs never create an ACK loop');
        }
        assert.equal(server.remoteSessions.commandReady(session), true);
    };
    const makeSession = (owner, target, requestId) => {
        send(owner, 'remote_session_open', { targetEndpointId: target, requestId });
        const session = server.remoteSessions.forOwnerTarget(owner, target);
        assert.ok(session);
        send(target, 'remote_session_accept', { remoteSessionId: session.remoteSessionId,
            generation: session.generation, snapshot: { screens: [], systemUI: [],
                volumePercent: 50, revision: 1, capturedAtEpochMs: wallOffset + now } });
        acknowledge(session);
        return session;
    };
    const settle = session => {
        if (!TERMINAL_PHASES.has(session.phase)) send(session.ownerEndpointId,
            'remote_session_close', { remoteSessionId: session.remoteSessionId,
                generation: session.generation, requestId: `close-${++serial}` });
        assert.equal(server.remoteSessions.sessions.has(session.remoteSessionId), false);
        assert.equal(server.remoteSessions.cleanupJobs.has(session.remoteSessionId), true);
        const teardownId = session.teardownId;
        const ack = { remoteSessionId: session.remoteSessionId, generation: session.generation,
            teardownId, result: 'committed', sceneStopped: true, uploadsAborted: true,
            cacheQuarantined: true, removedFileCount: 0 };
        if (random(2) === 0) {
            ++statistics.lostCleanupAcks;
            const attempts = session.cleanupDispatchAttempts;
            elapse(Math.max(0, session.cleanupNextRetryAt - now) + 1);
            assert.ok(session.cleanupDispatchAttempts > attempts);
            assert.equal(session.teardownId, teardownId, 'retry retains cleanup identity');
        }
        if (random(5) === 0) {
            ++statistics.cleanupErrors;
            send(session.targetEndpointId, 'remote_session_teardown_ack', {
                ...ack, result: 'cleanup_error', cacheQuarantined: false,
                errorCode: 'injected_quarantine_failure' });
            assert.equal(server.remoteSessions.cleanupJobs.has(session.remoteSessionId), true);
        }
        send(session.targetEndpointId, 'remote_session_teardown_ack', ack);
        if (random(2) === 0) send(session.targetEndpointId, 'remote_session_teardown_ack', ack);
        assert.equal(server.remoteSessions.get(session.remoteSessionId), null);
        assert.equal(server.remoteSessions.getTombstone(session.remoteSessionId).phase, 'Closed');
    };
    for (const id of ['A', 'B', 'C']) attach(id);
    const durations = [0, 749, 1500, 2999, 3000, 4000, 4999, 5000, 6000];

    for (let iteration = 0; iteration < 600; ++iteration) {
        ++statistics.cycles;
        pulse();
        const requestId = `open-${iteration}`;
        const session = makeSession('A', 'B', requestId);
        const initialGeneration = session.generation;
        for (let repeat = random(4); repeat > 0; --repeat) {
            send('A', 'remote_session_open', { targetEndpointId: 'B', requestId });
            assert.equal(server.remoteSessions.forOwnerTarget('A', 'B'), session);
        }
        let run = null;
        if (iteration % 3 === 0) {
            run = server.sceneRuns.prepare({ remoteSessionId: session.remoteSessionId,
                generation: session.generation, sceneRunId: `scene-${iteration}`,
                revision: 1, digest: 'digest', manifest: [], scene: {},
                ownerEndpointId: 'A', targetEndpointId: 'B' }).run;
            run.phase = SCENE_PHASES.LIVE;
        }
        const mode = iteration % 5;
        if (mode <= 2) {
            const both = mode === 1;
            detach('A');
            if (both) detach('B');
            const deadline = server.remoteSessions.validUntil(session);
            const duration = durations[(Math.floor(iteration / 5) + random(9)) % durations.length];
            elapse(duration);
            assert.equal(deadline, session.createdAt + 3000);
            const order = both && random(2) ? ['B', 'A'] : both ? ['A', 'B'] : ['A'];
            for (const id of order) attach(id);
            if (duration < 3000) {
                if (run) assert.equal(run.phase, SCENE_PHASES.LIVE);
                // Both sides deliberately retain their pre-loss observation.
                for (const id of order) {
                    const requestId = `resume-${iteration}-${id}`;
                    const resume = { requestId, remoteSessionId: session.remoteSessionId,
                        generation: initialGeneration, resumeToken: session.resumeToken };
                    server.clients.get(id).ws.drop.add('remote_session_resumed');
                    send(id, 'remote_session_resume', resume);
                    ++statistics.resumes;
                    const currentGeneration = session.generation;
                    server.clients.get(id).ws.drop.clear();
                    send(id, 'remote_session_resume', resume);
                    ++statistics.duplicateResumes;
                    assert.equal(session.generation, currentGeneration, 'duplicate resume is not a new epoch');
                }
                acknowledge(session);
                assert.ok(session.generation > initialGeneration);
                const before = session.generation;
                send('A', 'remote_session_close', { remoteSessionId: session.remoteSessionId,
                    generation: initialGeneration, connectionGeneration: 1 });
                assert.equal(session.generation, before, 'stale traffic cannot close a rebound session');
                assert.equal(session.phase, 'Active');
            } else {
                ++statistics.expired;
                assert.equal(session.phase, 'CleanupPending');
                const generation = session.generation;
                send('A', 'remote_session_resume', { requestId: `late-${iteration}`,
                    remoteSessionId: session.remoteSessionId, generation,
                    resumeToken: session.resumeToken });
                assert.equal(session.phase, 'CleanupPending');
                assert.equal(session.generation, generation, 'late proof never revives terminal state');
            }
        } else if (mode === 3) {
            const other = makeSession('C', 'B', `independent-${iteration}`);
            assert.equal(server.remoteSessions.incomingForTarget('B').length, 2);
            settle(other);
            assert.equal(session.phase, 'Active', 'one cleanup leaves the independent controller intact');
        } else {
            // Exact detection threshold fences the silent socket; recover on a new one.
            now += 750;
            send('A', 'heartbeat', { sequence: ++serial });
            send('C', 'heartbeat', { sequence: ++serial });
            now += 750;
            send('A', 'heartbeat', { sequence: ++serial });
            send('C', 'heartbeat', { sequence: ++serial });
            server.sweepRemoteSessionLeases(now);
            assert.equal(session.degradedEndpoints.has('B'), true);
            assert.equal(server.remoteSessions.commandReady(session), false);
            attach('B');
            send('B', 'remote_session_resume', { requestId: `silent-${iteration}`,
                remoteSessionId: session.remoteSessionId, generation: session.generation,
                resumeToken: session.resumeToken });
            acknowledge(session);
        }
        settle(session);
        if (run) {
            // Replay a late STOP acknowledgement after cleanup moved both
            // registries to tombstones. It remains correlated and harmless.
            const client = server.clients.get('A');
            send('A', 'stopped', { remoteSessionId: session.remoteSessionId,
                generation: session.generation, sceneRunId: run.sceneRunId,
                digest: run.digest, success: true });
            assert.equal(client.ws.messages.at(-1).type, 'stopped');
            assert.equal(client.ws.messages.at(-1).replay, true);
        }
        server.sweepRemoteSessionLeases(now);
        assert.equal(server.remoteSessions.sessions.size, 0);
        assert.equal(server.remoteSessions.cleanupJobs.size, 0);
        assert.equal(server.remoteSessions.incomingByTarget.size, 0);
        assert.equal(server.remoteSessions.outgoingByOwner.size, 0);
        assert.equal(server.remoteSessions.sessionByOwnerTarget.size, 0);
        assert.ok(server.remoteSessions.tombstones.size <= 32);
        assert.ok(server.remoteSessions.openRequests.size <= 32);
        assert.ok(server.sceneRuns.tombstones.size <= 32);
        assert.ok(server.metrics.onceKeys.size <= 64);
        assert.equal(server.uploads.size, 0);
        assert.equal(server.sessionAssets.size, 0);
        assert.equal(server.clients.size, 3);
        assert.equal(server.currentTransportByEndpoint.size, 3);
        now += 10 + random(100);
    }
    assert.ok(statistics.resumes > 100 && statistics.expired > 30);
    assert.ok(statistics.lostCleanupAcks > 100 && statistics.wallJumps > 100);
    assert.ok(statistics.lostOwnerStateAcks > 100 && statistics.lostTargetStateAcks > 100);
    return statistics;
}

async function cacheSoak() {
    const temporary = await fs.mkdtemp(path.join(os.tmpdir(), 'mouffette-cache-soak-'));
    const uploadsRoot = path.join(temporary, 'Uploads');
    const scheduled = [];
    const failedOnce = new Set();
    const createStore = () => new RemoteCacheStore({ uploadsRoot,
        scheduler: task => scheduled.push(task),
        faultInjector: (point, transaction) => {
            const index = (transaction.generation - 1) * 8
                + Number(transaction.remoteSessionId.split('-').at(-1));
            if (point === 'before_physical_delete'
                && index % 17 === 0 && !failedOnce.has(index)) {
                failedOnce.add(index);
                const error = new Error('seeded transient delete failure'); error.code = 'EACCES'; throw error;
            }
        } });
    let store = createStore();
    const drain = async () => {
        while (scheduled.length > 0) await scheduled.shift()();
        await store.waitForIdle();
        await store.sweep();
        while (scheduled.length > 0) await scheduled.shift()();
        await store.waitForIdle();
        assert.equal(store.deletionJobs.size, 0);
        assert.equal(store.transactions.size, 0);
        assert.equal(store.activeGenerations.size, 0);
        assert.ok(store.tombstones.size <= 8, 'one durable proof per reused cache scope');
        for (const directory of [store.quarantineRoot, store.transactionsRoot, store.activeStateRoot]) {
            assert.deepEqual(await fs.readdir(directory), [], 'no data/work files remain after drain');
        }
        assert.ok((await fs.readdir(store.tombstonesRoot)).length <= 8);
    };
    try {
        await store.initialize();
        for (let iteration = 0; iteration < 128; ++iteration) {
            const binding = { senderEndpointId: 'cache-sender', remoteSessionId: `scope-${iteration % 8}`,
                generation: 1 + Math.floor(iteration / 8), teardownId: `disk-${iteration}` };
            const active = await store.ensureActiveCache(binding);
            assert.equal(active.ok, true);
            await fs.writeFile(path.join(active.cachePath, 'asset.bin'), Buffer.alloc(256 + random(1024)));
            const committed = await store.commitCleanup(binding);
            assert.equal(committed.logicalCommitted, true);
            const queued = scheduled.length;
            assert.equal((await store.commitCleanup(binding)).replay, true);
            assert.equal(scheduled.length, queued, 'replayed cleanup never duplicates a deletion task');
            assert.ok(scheduled.length <= 8 && store.deletionJobs.size <= 8);
            assert.ok((await fs.readdir(store.quarantineRoot)).length <= 8);
            if (iteration % 8 === 7) await drain();
            if (iteration % 32 === 31) {
                store = createStore();
                assert.equal((await store.initialize()).ok, true);
                await drain();
            }
        }
        await drain();
        assert.equal(failedOnce.size, 8, 'seeded deletion failures were retried');
        return { cycles: 128, restarts: 4, recoveredDeleteFailures: failedOnce.size,
            activeDataFiles: 0, pendingDeletionJobs: 0, retainedScopeProofs: store.tombstones.size };
    } finally {
        await fs.rm(temporary, { recursive: true, force: true });
    }
}

(async () => {
    const protocol = protocolSoak();
    const cache = await cacheSoak();
    console.log(`seeded recovery soak tests passed ${JSON.stringify({ seed: SEED, protocol, cache })}`);
})().catch(error => { console.error(error); process.exitCode = 1; });
