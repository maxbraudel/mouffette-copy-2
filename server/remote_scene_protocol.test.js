const assert = require('node:assert/strict');
const WebSocket = require('ws');
const { MouffetteServer } = require('./server');

function fakeSocket({ throwOnSend = false } = {}) {
    return {
        readyState: WebSocket.OPEN,
        bufferedAmount: 0,
        messages: [],
        send(payload) {
            if (throwOnSend) throw new Error('simulated send failure');
            this.messages.push(JSON.parse(payload));
        }
    };
}

function addClient(server, id, options = {}) {
    const ws = fakeSocket(options);
    server.clients.set(id, {
        id,
        sessionId: id,
        persistentId: `persistent-${id}`,
        machineName: id,
        ws
    });
    return ws;
}

function messagesOf(socket, type) {
    return socket.messages.filter(message => message.type === type);
}

function scene(sceneInstanceId) {
    return {
        sceneInstanceId,
        renderSchemaVersion: 2,
        screens: [{ id: 1 }],
        media: [{ mediaId: 'media-1', type: 'video' }]
    };
}

function videoSync(targetClientId, sceneInstanceId, sequence) {
    return {
        type: 'remote_scene_video_sync',
        targetClientId,
        sceneInstanceId,
        sequence,
        sampledEpochMs: Date.now(),
        videos: [{
            mediaId: 'media-1',
            positionMs: 100,
            durationMs: 1000,
            playing: true,
            muted: false,
            visible: true,
            repeatAvailable: false
        }]
    };
}

const ownerId = 'owner-session';
const targetId = 'target-session';
const attackerId = 'attacker-session';
const sceneInstanceId = '11111111-2222-4333-8444-555555555555';

// Full lifecycle: state transitions are authoritative, response routing is
// derived from server state, and every destructive transition is idempotent.
{
    const server = new MouffetteServer(0);
    const owner = addClient(server, ownerId);
    const target = addClient(server, targetId);
    const attacker = addClient(server, attackerId);

    const start = {
        type: 'remote_scene_start',
        targetClientId: targetId,
        scene: scene(sceneInstanceId)
    };
    server.handleMessage(ownerId, structuredClone(start));
    assert.equal(messagesOf(target, 'remote_scene_start').length, 1);
    assert.equal(server.remoteScenesByTarget.get(targetId).phase, 'preparing');

    for (let i = 0; i < 100; ++i) {
        server.handleRemoteSceneStart(ownerId, structuredClone(start));
    }
    assert.equal(messagesOf(target, 'remote_scene_start').length, 1,
        'duplicate START must never rebuild the target multimedia graph');

    server.handleRemoteSceneStart(attackerId, {
        ...structuredClone(start),
        scene: scene('aaaaaaaa-bbbb-4ccc-8ddd-eeeeeeeeeeee')
    });
    assert.equal(messagesOf(target, 'remote_scene_start').length, 1,
        'a competing owner must not reach an occupied target');
    assert.equal(messagesOf(attacker, 'remote_scene_validation').at(-1).success, false);

    server.handleRemoteSceneValidation(attackerId, {
        sceneInstanceId,
        targetClientId: ownerId,
        success: true
    });
    assert.equal(server.remoteScenesByTarget.get(targetId).phase, 'preparing',
        'a third party cannot validate another target run');

    server.handleRemoteSceneStopped(targetId, {
        sceneInstanceId,
        targetClientId: attackerId,
        success: true
    });
    assert.equal(server.remoteScenesByTarget.get(targetId).phase, 'preparing',
        'STOPPED before STOP must not mutate the run');

    server.handleRemoteSceneValidation(targetId, {
        sceneInstanceId,
        targetClientId: attackerId,
        success: true
    });
    assert.equal(server.remoteScenesByTarget.get(targetId).phase, 'prepared');
    assert.equal(messagesOf(owner, 'remote_scene_validation').length, 1);
    assert.equal(messagesOf(owner, 'remote_scene_validation')[0].senderClientId, targetId);
    assert.equal(messagesOf(attacker, 'remote_scene_validation').length, 1,
        'the target-provided destination must not receive a correlated response');

    server.handleRemoteSceneValidation(targetId, {
        sceneInstanceId,
        targetClientId: ownerId,
        success: true
    });
    assert.equal(messagesOf(owner, 'remote_scene_validation').length, 1,
        'duplicate validation must not make the owner activate twice');

    const activate = {
        type: 'remote_scene_activate',
        targetClientId: targetId,
        sceneInstanceId,
        activationEpochMs: Date.now() + 1000,
        activationDelayMs: 1000
    };
    server.handleRemoteSceneActivate(attackerId, activate);
    assert.equal(messagesOf(target, 'remote_scene_activate').length, 0);
    server.handleRemoteSceneActivate(ownerId, activate);
    server.handleRemoteSceneActivate(ownerId, activate);
    assert.equal(messagesOf(target, 'remote_scene_activate').length, 1);
    assert.equal(server.remoteScenesByTarget.get(targetId).phase, 'activating');

    server.handleRemoteSceneLaunched(targetId, {
        sceneInstanceId,
        targetClientId: attackerId
    });
    assert.equal(server.remoteScenesByTarget.get(targetId).phase, 'running');
    assert.equal(messagesOf(owner, 'remote_scene_launched').length, 1);
    assert.equal(messagesOf(attacker, 'remote_scene_launched').length, 0,
        'LAUNCHED destination must be derived from the run owner');
    server.handleRemoteSceneLaunched(targetId, { sceneInstanceId, targetClientId: ownerId });
    assert.equal(messagesOf(owner, 'remote_scene_launched').length, 1);

    const targetStopsBefore = messagesOf(target, 'remote_scene_stop').length;
    server.handleRemoteSceneStop(attackerId, {
        targetClientId: targetId,
        sceneInstanceId
    });
    server.handleRemoteSceneStop(ownerId, {
        targetClientId: targetId,
        sceneInstanceId: '99999999-2222-4333-8444-555555555555'
    });
    assert.equal(messagesOf(target, 'remote_scene_stop').length, targetStopsBefore,
        'foreign and mismatched STOP commands must not reach the target');
    const attackerStopResultsBeforeTargetAck =
        messagesOf(attacker, 'remote_scene_stopped').length;

    target.bufferedAmount = server.MAX_REMOTE_SCENE_BUFFERED_BYTES + 1;
    server.handleRemoteSceneStop(ownerId, { targetClientId: targetId, sceneInstanceId });
    target.bufferedAmount = 0;
    for (let i = 0; i < 100; ++i) {
        server.handleRemoteSceneStop(ownerId, { targetClientId: targetId, sceneInstanceId });
    }
    assert.equal(messagesOf(target, 'remote_scene_stop').length, targetStopsBefore + 1,
        'STOP spam must produce one target teardown');
    assert.equal(server.remoteScenesByTarget.get(targetId).phase, 'stopping');

    server.handleRemoteSceneVideoSync(ownerId, videoSync(targetId, sceneInstanceId, 1));
    assert.equal(messagesOf(target, 'remote_scene_video_sync').length, 0,
        'video sync must stop as soon as STOP begins');

    server.handleRemoteSceneStopped(targetId, {
        sceneInstanceId,
        targetClientId: attackerId,
        success: true
    });
    assert.equal(server.remoteScenesByTarget.has(targetId), false);
    assert.equal(messagesOf(owner, 'remote_scene_stopped').at(-1).success, true);
    assert.equal(messagesOf(attacker, 'remote_scene_stopped').length,
        attackerStopResultsBeforeTargetAck,
        'STOPPED destination must be derived from the run owner');

    const stopCountAfterSuccess = messagesOf(target, 'remote_scene_stop').length;
    server.handleRemoteSceneStop(ownerId, { targetClientId: targetId, sceneInstanceId });
    assert.equal(messagesOf(target, 'remote_scene_stop').length, stopCountAfterSuccess,
        'a completed STOP retry must be answered from the tombstone');
    assert.equal(messagesOf(owner, 'remote_scene_stopped').at(-1).success, true);
}

// STOP is idempotently successful when server state is already empty (for
// example after reconnect), without forwarding an unowned teardown to B or
// amplifying a burst into an acknowledgement storm.
{
    const server = new MouffetteServer(0);
    const owner = addClient(server, ownerId);
    const target = addClient(server, targetId);
    for (let i = 0; i < 100; ++i) {
        server.handleRemoteSceneStop(ownerId, {
            targetClientId: targetId,
            sceneInstanceId
        });
    }
    assert.equal(messagesOf(target, 'remote_scene_stop').length, 0);
    assert.equal(messagesOf(owner, 'remote_scene_stopped').length, 1);
    assert.equal(messagesOf(owner, 'remote_scene_stopped')[0].success, true);
    assert.equal(server.remoteSceneStopTombstones.size, 1);

    server.remoteSceneAuxiliaryReplyRates.clear();
    owner.messages.length = 0;
    for (let i = 0; i < 100; ++i) {
        server.handleRemoteSceneStop(ownerId, {
            targetClientId: targetId,
            sceneInstanceId: `unknown-run-${i}`
        });
    }
    assert.equal(messagesOf(owner, 'remote_scene_stopped').length,
        server.MAX_REMOTE_SCENE_AUXILIARY_REPLIES_PER_SECOND,
        'unique unknown STOP spam must have a bounded acknowledgement rate');
}

// Losing the target closes its own controller and therefore converges an
// already running/stopping owner as a successful correlated stop.
{
    const server = new MouffetteServer(0);
    const owner = addClient(server, ownerId);
    addClient(server, targetId);
    server.remoteScenesByTarget.set(targetId, {
        ownerId,
        targetId,
        sceneInstanceId,
        phase: 'running',
        createdAt: Date.now(),
        lastActivity: Date.now()
    });
    server.handleRemoteSceneClientDeparture(targetId);
    assert.equal(server.remoteScenesByTarget.has(targetId), false);
    assert.equal(messagesOf(owner, 'remote_scene_stopped').at(-1).success, true);
    assert.equal(server.remoteSceneStopTombstones.size, 1);
}

// A STOP racing the target socket's close converges too; it must not report a
// failure that would make A resume video synchronization toward a dead B.
{
    const server = new MouffetteServer(0);
    const owner = addClient(server, ownerId);
    const target = addClient(server, targetId);
    server.remoteScenesByTarget.set(targetId, {
        ownerId,
        targetId,
        sceneInstanceId,
        phase: 'running',
        createdAt: Date.now(),
        lastActivity: Date.now()
    });
    target.readyState = WebSocket.CLOSING;
    server.handleRemoteSceneStop(ownerId, { targetClientId: targetId, sceneInstanceId });
    assert.equal(server.remoteScenesByTarget.has(targetId), false);
    assert.equal(messagesOf(owner, 'remote_scene_stopped').at(-1).success, true);
}

// VIDEO_SYNC accepts only the authenticated owner/current run, monotonically
// increasing data, and never grows a slow target's WebSocket queue.
{
    const server = new MouffetteServer(0);
    addClient(server, ownerId);
    const target = addClient(server, targetId);
    addClient(server, attackerId);
    server.remoteScenesByTarget.set(targetId, {
        ownerId,
        targetId,
        sceneInstanceId,
        phase: 'running',
        createdAt: Date.now(),
        lastActivity: Date.now(),
        lastVideoSequence: 0,
        lastVideoRelayAt: 0,
        previousPhase: null
    });

    server.handleRemoteSceneVideoSync(attackerId, videoSync(targetId, sceneInstanceId, 1));
    assert.equal(messagesOf(target, 'remote_scene_video_sync').length, 0);
    server.handleRemoteSceneVideoSync(ownerId, videoSync(targetId, sceneInstanceId, 1));
    assert.equal(messagesOf(target, 'remote_scene_video_sync').length, 1);
    server.handleRemoteSceneVideoSync(ownerId, videoSync(targetId, sceneInstanceId, 1));
    assert.equal(messagesOf(target, 'remote_scene_video_sync').length, 1,
        'duplicate sequences must be dropped');

    const run = server.remoteScenesByTarget.get(targetId);
    run.lastVideoRelayAt = 0;
    target.bufferedAmount = server.MAX_REMOTE_SCENE_BUFFERED_BYTES + 1;
    server.handleRemoteSceneVideoSync(ownerId, videoSync(targetId, sceneInstanceId, 2));
    assert.equal(messagesOf(target, 'remote_scene_video_sync').length, 1,
        'a congested target must not accumulate sync snapshots');

    run.lastVideoRelayAt = 0;
    target.bufferedAmount = 0;
    server.handleRemoteSceneVideoSync(ownerId, videoSync(targetId, sceneInstanceId, 3));
    assert.deepEqual(
        messagesOf(target, 'remote_scene_video_sync').map(message => message.sequence),
        [1, 3]);

    run.lastVideoRelayAt = 0;
    const oversized = videoSync(targetId, sceneInstanceId, 4);
    oversized.videos = Array.from(
        { length: server.MAX_REMOTE_SCENE_SYNC_ITEMS + 1 },
        () => oversized.videos[0]);
    server.handleRemoteSceneVideoSync(ownerId, oversized);
    assert.equal(messagesOf(target, 'remote_scene_video_sync').length, 2);
}

// Invalid/undeliverable starts never create ghost server state.
{
    const server = new MouffetteServer(0);
    const owner = addClient(server, ownerId);
    const target = addClient(server, targetId, { throwOnSend: true });

    const originalConsoleError = console.error;
    console.error = () => {};
    try {
        server.handleRemoteSceneStart(ownerId, {
            type: 'remote_scene_start',
            targetClientId: targetId,
            scene: scene(sceneInstanceId)
        });
    } finally {
        console.error = originalConsoleError;
    }
    assert.equal(server.remoteScenesByTarget.has(targetId), false);
    assert.equal(messagesOf(owner, 'remote_scene_validation').at(-1).success, false);

    server.handleRemoteSceneStart(ownerId, {
        type: 'remote_scene_start',
        targetClientId: targetId,
        scene: { sceneInstanceId, screens: 'invalid', media: [] }
    });
    assert.equal(server.remoteScenesByTarget.has(targetId), false);
    assert.equal(messagesOf(owner, 'remote_scene_validation').at(-1).success, false);
}

// Timeout cleanup and tombstones are bounded, so stalled peers and historical
// retries cannot leak state indefinitely.
{
    const server = new MouffetteServer(0);
    const owner = addClient(server, ownerId);
    const target = addClient(server, targetId);
    server.remoteScenesByTarget.set(targetId, {
        ownerId,
        targetId,
        sceneInstanceId,
        phase: 'stopping',
        createdAt: 1,
        lastActivity: 1,
        previousPhase: 'running'
    });
    server.cleanupRemoteScenes(server.REMOTE_SCENE_STOP_TIMEOUT_MS + 2);
    assert.equal(server.remoteScenesByTarget.has(targetId), false);
    assert.equal(messagesOf(target, 'remote_scene_stop').length, 1);
    assert.equal(messagesOf(owner, 'remote_scene_stopped').at(-1).success, false);

    server.MAX_REMOTE_SCENE_TOMBSTONES = 2;
    for (let i = 0; i < 3; ++i) {
        server.rememberRemoteSceneStopped({
            ownerId,
            targetId,
            sceneInstanceId: `tombstone-${i}`
        }, 100 + i);
    }
    assert.equal(server.remoteSceneStopTombstones.size, 2);
    server.pruneRemoteSceneStopTombstones(
        103 + server.REMOTE_SCENE_TOMBSTONE_TTL_MS);
    assert.equal(server.remoteSceneStopTombstones.size, 0);
}

console.log('remote scene protocol tests passed');
