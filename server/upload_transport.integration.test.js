'use strict';

const assert = require('node:assert/strict');
const crypto = require('node:crypto');
const WebSocket = require('ws');
const { MouffetteServer } = require('./server');
const {
    challengePayload, installationIdForPublicKey, endpointIdForInstallation,
} = require('./device_auth');
const { computeSceneDigest, SceneRunRegistry } = require('./scene_run_registry');

function trackedSocket(url) {
    const ws = new WebSocket(url);
    const queued = [];
    const waiters = [];
    ws.on('message', payload => {
        const message = JSON.parse(payload.toString());
        const index = waiters.findIndex(waiter => waiter.predicate(message));
        if (index >= 0) {
            const [waiter] = waiters.splice(index, 1);
            clearTimeout(waiter.timer);
            waiter.resolve(message);
        } else queued.push(message);
    });
    return {
        ws,
        async opened() {
            if (ws.readyState === WebSocket.OPEN) return;
            await new Promise((resolve, reject) => {
                ws.once('open', resolve);
                ws.once('error', reject);
            });
        },
        next(predicate, timeoutMs = 3000) {
            const index = queued.findIndex(predicate);
            if (index >= 0) return Promise.resolve(queued.splice(index, 1)[0]);
            return new Promise((resolve, reject) => {
                const waiter = { predicate, resolve, timer: null };
                waiter.timer = setTimeout(() => {
                    const current = waiters.indexOf(waiter);
                    if (current >= 0) waiters.splice(current, 1);
                    reject(new Error('Timed out waiting for WebSocket protocol message'));
                }, timeoutMs);
                waiters.push(waiter);
            });
        },
    };
}

function assertEnvelope(message, context) {
    assert.equal(message.protocolVersion, 4);
    assert.equal(message.serverBootId, context.serverBootId);
    assert.equal(message.connectionGeneration, context.connectionGeneration);
    assert.match(message.messageId,
        /^[0-9a-f]{8}-[0-9a-f]{4}-4[0-9a-f]{3}-[89ab][0-9a-f]{3}-[0-9a-f]{12}$/i);
}

async function closeSocket(ws) {
    if (!ws || ws.readyState === WebSocket.CLOSED) return;
    await new Promise(resolve => {
        const timer = setTimeout(() => ws.terminate(), 1000);
        ws.once('close', () => { clearTimeout(timer); resolve(); });
        ws.close();
    });
}

async function connectDevice(url, machineName) {
    const peer = trackedSocket(url);
    await peer.opened();
    const challenge = await peer.next(message => message.type === 'auth_challenge');
    const keyPair = crypto.generateKeyPairSync('ed25519');
    const publicKey = keyPair.publicKey.export({ type: 'spki', format: 'der' });
    const runtimeId = crypto.randomUUID();
    const instanceId = 'primary';
    const installationId = installationIdForPublicKey(publicKey);
    const endpointId = endpointIdForInstallation(installationId, instanceId);
    peer.ws.send(JSON.stringify({
        type: 'auth_response',
        protocolVersion: 4,
        serverBootId: challenge.serverBootId,
        messageId: crypto.randomUUID(),
        runtimeId,
        instanceId,
        installationId,
        publicKey: publicKey.toString('base64url'),
        signature: crypto.sign(null,
            challengePayload({ ...challenge, runtimeId, instanceId }), keyPair.privateKey)
            .toString('base64url'),
    }));
    const welcome = await peer.next(message => message.type === 'welcome');
    peer.context = {
        endpointId,
        runtimeId,
        serverBootId: welcome.serverBootId,
        connectionGeneration: welcome.connectionGeneration,
    };
    peer.send = (type, body = {}) => peer.ws.send(JSON.stringify({
        type,
        protocolVersion: 4,
        serverBootId: peer.context.serverBootId,
        connectionGeneration: peer.context.connectionGeneration,
        messageId: crypto.randomUUID(),
        ...body,
    }));
    peer.send('endpoint_snapshot', {
        machineName, platform: 'test', instanceOrdinal: 1,
        screens: [], systemUI: [], volumePercent: null,
    });
    await peer.next(message => message.type === 'endpoint_snapshot_applied');
    return peer;
}

(async () => {
    const server = new MouffetteServer(0);
    let sceneEpoch = Date.now();
    let sceneMonotonic = 10_000;
    server.sceneRuns = new SceneRunRegistry({
        epochNow: () => sceneEpoch,
        monotonicNow: () => sceneMonotonic,
        prepareTimeoutMs: 15_000,
        activationLeadMs: 4_000,
        maximumClockUncertaintyMs: 50,
    });
    server.start();
    await new Promise(resolve => server.wss.once('listening', resolve));
    const url = `ws://127.0.0.1:${server.wss.address().port}`;
    const owner = await connectDevice(url, 'owner');
    const target = await connectDevice(url, 'target');
    let uploadChannel = null;

    try {
        owner.send('remote_session_open', {
            targetEndpointId: target.context.endpointId,
            requestId: 'open-1',
        });
        const opening = await owner.next(message =>
            message.type === 'remote_session_opening');
        const offer = await target.next(message =>
            message.type === 'remote_session_offer'
            && message.remoteSessionId === opening.remoteSessionId);
        target.send('remote_session_accept', {
            remoteSessionId: offer.remoteSessionId,
            generation: offer.generation,
            snapshot: {
                screens: [], systemUI: [], volumePercent: null,
                revision: 1, capturedAtEpochMs: Date.now(),
            },
        });
        const opened = await owner.next(message => message.type === 'remote_session_opened');
        await target.next(message => message.type === 'remote_session_opened'
            && message.remoteSessionId === opened.remoteSessionId);
        const session = {
            remoteSessionId: opened.remoteSessionId,
            generation: opened.generation,
        };

        owner.send('request_upload_channel');
        const token = await owner.next(message => message.type === 'upload_channel_token');
        assertEnvelope(token, owner.context);
        uploadChannel = trackedSocket(`${url}?channel=upload&token=${encodeURIComponent(token.token)}`);
        await uploadChannel.opened();
        const ready = await uploadChannel.next(message => message.type === 'upload_channel_ready');
        assertEnvelope(ready, owner.context);

        const bytes = Buffer.alloc(128, 0x4d);
        const sha256 = crypto.createHash('sha256').update(bytes).digest('hex');
        const uploadId = 'upload-integration-1';
        const asset = {
            assetId: 'asset-integration-1', fileId: sha256, sha256,
            name: 'integration.png', extension: 'png', size: bytes.length,
            mediaIds: ['media-integration-1'],
        };
        const uploadEnvelope = body => ({
            protocolVersion: 4,
            serverBootId: owner.context.serverBootId,
            messageId: crypto.randomUUID(),
            connectionGeneration: owner.context.connectionGeneration,
            remoteSessionId: session.remoteSessionId,
            generation: session.generation,
            ...body,
        });
        uploadChannel.ws.send(JSON.stringify(uploadEnvelope({
            type: 'upload_start', uploadId, files: [asset],
        })));
        await target.next(message => message.type === 'upload_start'
            && message.uploadId === uploadId);
        target.send('upload_ready', {
            ...session, uploadId,
            assets: [{ assetId: asset.assetId, offset: 0, size: 128, sha256 }],
        });
        await owner.next(message => message.type === 'upload_ready'
            && message.uploadId === uploadId);

        uploadChannel.ws.send(JSON.stringify(uploadEnvelope({
            type: 'upload_chunk', uploadId, assetId: asset.assetId,
            offset: 0, size: 64, sha256, data: bytes.subarray(0, 64).toString('base64'),
        })));
        await target.next(message => message.type === 'upload_chunk'
            && message.uploadId === uploadId);
        target.send('upload_progress', {
            ...session, uploadId,
            assets: [{ assetId: asset.assetId, offset: 64, size: 128, sha256 }],
        });
        await owner.next(message => message.type === 'upload_progress'
            && message.durableBytes === 64);

        await closeSocket(uploadChannel.ws);
        uploadChannel = null;
        assert.equal(server.uploads.has(uploadId), true,
            'closing only the upload transport must preserve resumable state');
        owner.send('request_upload_channel');
        const replacementToken = await owner.next(message => message.type === 'upload_channel_token');
        assertEnvelope(replacementToken, owner.context);
        uploadChannel = trackedSocket(
            `${url}?channel=upload&token=${encodeURIComponent(replacementToken.token)}`);
        await uploadChannel.opened();
        const replacementReady = await uploadChannel.next(
            message => message.type === 'upload_channel_ready');
        assertEnvelope(replacementReady, owner.context);
        uploadChannel.ws.send(JSON.stringify(uploadEnvelope({
            type: 'upload_resume', uploadId,
        })));
        const resumed = await owner.next(message => message.type === 'upload_resume_ready');
        assert.equal(resumed.assets[0].offset, 64);
        await target.next(message => message.type === 'upload_resume'
            && message.uploadId === uploadId);
        target.send('upload_ready', {
            ...session, uploadId,
            assets: [{ assetId: asset.assetId, offset: 64, size: 128, sha256 }],
        });
        await owner.next(message => message.type === 'upload_ready'
            && message.uploadId === uploadId);

        uploadChannel.ws.send(JSON.stringify(uploadEnvelope({
            type: 'upload_chunk', uploadId, assetId: asset.assetId,
            offset: 64, size: 64, sha256, data: bytes.subarray(64).toString('base64'),
        })));
        await target.next(message => message.type === 'upload_chunk'
            && message.offset === 64);
        target.send('upload_progress', {
            ...session, uploadId,
            assets: [{ assetId: asset.assetId, offset: 128, size: 128, sha256 }],
        });
        await owner.next(message => message.type === 'upload_progress'
            && message.uploadId === uploadId && message.durableBytes === 128);
        uploadChannel.ws.send(JSON.stringify(uploadEnvelope({
            type: 'upload_complete', uploadId,
            assets: [{ assetId: asset.assetId, offset: 128, size: 128, sha256 }],
        })));
        const firstCompletion = await target.next(message => message.type === 'upload_complete'
            && message.uploadId === uploadId);
        assert.deepEqual(firstCompletion.assets,
            [{ assetId: asset.assetId, offset: 128, size: 128, sha256 }]);

        // Simulate B promoting the asset while its upload_finished ACK is lost.
        // Reopening only A's upload channel and replaying the exact upload_start
        // must ask B for the terminal ACK, never restart byte transport.
        await closeSocket(uploadChannel.ws);
        uploadChannel = null;
        owner.send('request_upload_channel');
        const finalAckRetryToken = await owner.next(
            message => message.type === 'upload_channel_token');
        uploadChannel = trackedSocket(
            `${url}?channel=upload&token=${encodeURIComponent(finalAckRetryToken.token)}`);
        await uploadChannel.opened();
        await uploadChannel.next(message => message.type === 'upload_channel_ready');
        uploadChannel.ws.send(JSON.stringify(uploadEnvelope({
            type: 'upload_start', uploadId, files: [asset],
        })));
        const completionReplay = await target.next(message =>
            message.type === 'upload_complete' && message.uploadId === uploadId
            && message.replay === true);
        assert.deepEqual(completionReplay.assets, firstCompletion.assets);
        const resumeReadyAfterPromotion = await owner.next(message =>
            message.type === 'upload_resume_ready' && message.uploadId === uploadId);
        assert.equal(resumeReadyAfterPromotion.replay, true);
        assert.deepEqual(resumeReadyAfterPromotion.assets,
            [{ assetId: asset.assetId, offset: 128, size: 128, sha256 }]);
        target.send('upload_finished', {
            ...session, uploadId,
            assets: [{ assetId: asset.assetId, offset: 128, size: 128, sha256 }],
        });
        await owner.next(message => message.type === 'upload_finished'
            && message.uploadId === uploadId);

        const manifest = [{
            assetId: asset.assetId, extension: asset.extension, fileId: sha256,
            mediaIds: asset.mediaIds, sha256, size: asset.size,
        }];
        const scene = {
            renderSchemaVersion: 2,
            screens: [{ id: 1, x: 0, y: 0, width: 1920, height: 1080,
                primary: true }],
            media: [{
                mediaId: asset.mediaIds[0], assetId: asset.assetId,
                fileId: sha256, fileName: asset.name, type: 'image',
                x: 0, y: 0, width: 1920, height: 1080,
                baseWidth: 1920, baseHeight: 1080, visible: true, z: 1,
                autoDisplay: false, autoDisplayDelayMs: 0,
                autoHide: false, autoHideDelayMs: 0,
                hideWhenVideoEnds: false, fadeInSeconds: 0,
                fadeOutSeconds: 0, contentOpacity: 1,
                spans: [{
                    screenId: 1, normX: 0, normY: 0, normW: 1, normH: 1,
                    spanDestNormX: 0, spanDestNormY: 0,
                    spanDestNormW: 1, spanDestNormH: 1,
                    spanSourceNormX: 0, spanSourceNormY: 0,
                    spanSourceNormW: 1, spanSourceNormH: 1,
                }],
            }],
        };
        const digest = computeSceneDigest(1, manifest, scene);
        const sceneRunId = 'scene-run-integration-1';
        owner.send('scene_prepare', {
            ...session, sceneRunId, revision: 1, digest, manifest, scene,
        });
        await target.next(message => message.type === 'scene_prepare'
            && message.sceneRunId === sceneRunId);
        const checklist = [
            { itemId: 'screen_1', stage: 'screen_render_graph_ready', ready: true },
            { itemId: 'media-integration-1_file', stage: 'file_validated', ready: true },
            { itemId: 'media-integration-1_decode', stage: 'image_decoded', ready: true },
            { itemId: 'media-integration-1_texture', stage: 'image_texture_ready', ready: true },
        ];
        owner.send('prepared', { ...session, sceneRunId, digest, success: true, checklist });
        target.send('prepared', { ...session, sceneRunId, digest, success: true, checklist });
        await owner.next(message => message.type === 'prepared' && message.allPrepared === true);
        owner.send('armed', { ...session, sceneRunId, digest, clockUncertaintyMs: 10 });
        target.send('armed', { ...session, sceneRunId, digest, clockUncertaintyMs: 10 });
        const commit = await owner.next(message => message.type === 'commit');
        assert.ok(commit.startEpochMs - Date.now() > 3000);
        await target.next(message => message.type === 'commit');
        sceneEpoch += 4_000;
        sceneMonotonic += 4_000;
        owner.send('started', {
            ...session, sceneRunId, digest, firstFramePresented: true,
            presentedServerMonotonicMs: sceneMonotonic,
        });
        target.send('started', {
            ...session, sceneRunId, digest, firstFramePresented: true,
            presentedServerMonotonicMs: sceneMonotonic,
        });
        await owner.next(message => message.type === 'started' && message.allStarted === true);
        owner.send('stop', { ...session, sceneRunId, digest, reason: 'test_complete' });
        await target.next(message => message.type === 'stop');
        owner.send('stopped', { ...session, sceneRunId, digest, success: true });
        target.send('stopped', { ...session, sceneRunId, digest, success: true });
        await owner.next(message => message.type === 'stopped' && message.success === true);

        owner.send('remote_session_close', { ...session, requestId: 'close-1' });
        const terminating = await target.next(message => message.type === 'remote_session_terminating');
        target.send('remote_session_teardown_ack', {
            ...session,
            teardownId: terminating.teardownId,
            result: 'committed',
            sceneStopped: true,
            uploadsAborted: true,
            cacheQuarantined: true,
            removedFileCount: 1,
        });
        await owner.next(message => message.type === 'remote_session_closed');
        assert.equal(server.sessionAssets.has(session.remoteSessionId), false);

        for (const peer of [owner, target]) {
            const output = peer.ws === owner.ws ? owner : target;
            // A representative post-auth message proves centralized envelopes.
            const clientList = await output.next(message => message.type === 'client_list');
            assert.equal(clientList.protocolVersion, 4);
            assert.equal(clientList.serverBootId, server.serverBootId);
            assert.equal(clientList.connectionGeneration,
                output.context.connectionGeneration);
            assert.equal(typeof clientList.messageId, 'string');
        }
    } finally {
        await closeSocket(uploadChannel && uploadChannel.ws);
        await Promise.all([closeSocket(owner.ws), closeSocket(target.ws)]);
        clearInterval(server.uploadCleanupInterval);
        clearInterval(server.leaseSweepInterval);
        await new Promise(resolve => server.wss.close(resolve));
    }
})().then(() => {
    console.log('upload transport v4 integration tests passed');
}).catch(error => {
    console.error(error);
    process.exitCode = 1;
});
