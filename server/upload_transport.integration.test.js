const assert = require('node:assert/strict');
const WebSocket = require('ws');
const { MouffetteServer } = require('./server');

const senderId = 'aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa';
const targetId = 'bbbbbbbb-bbbb-4bbb-8bbb-bbbbbbbbbbbb';
const canvasSessionId = `${senderId}_TO_${targetId}_canvas_cccccccc-cccc-4ccc-8ccc-cccccccccccc`;
const fileId = 'd'.repeat(64);

function trackedSocket(url) {
    const ws = new WebSocket(url);
    const queued = [];
    const waiters = [];
    ws.on('message', payload => {
        const message = JSON.parse(payload.toString());
        const waiterIndex = waiters.findIndex(waiter => waiter.predicate(message));
        if (waiterIndex >= 0) {
            const [waiter] = waiters.splice(waiterIndex, 1);
            clearTimeout(waiter.timer);
            waiter.resolve(message);
        } else {
            queued.push(message);
        }
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
            const queuedIndex = queued.findIndex(predicate);
            if (queuedIndex >= 0) {
                return Promise.resolve(queued.splice(queuedIndex, 1)[0]);
            }
            return new Promise((resolve, reject) => {
                const waiter = { predicate, resolve, timer: null };
                waiter.timer = setTimeout(() => {
                    const index = waiters.indexOf(waiter);
                    if (index >= 0) waiters.splice(index, 1);
                    reject(new Error('Timed out waiting for WebSocket protocol message'));
                }, timeoutMs);
                waiters.push(waiter);
            });
        }
    };
}

async function register(peer, id, machineName) {
    peer.ws.send(JSON.stringify({
        type: 'register',
        persistentClientId: id,
        sessionId: id,
        machineName,
        platform: 'test',
        screens: []
    }));
    await peer.next(message => message.type === 'registration_confirmed');
}

async function closeSocket(ws) {
    if (!ws || ws.readyState === WebSocket.CLOSED) return;
    await new Promise(resolve => {
        const timer = setTimeout(() => {
            if (ws.readyState !== WebSocket.CLOSED) ws.terminate();
        }, 1000);
        ws.once('close', () => {
            clearTimeout(timer);
            resolve();
        });
        ws.close();
    });
}

(async () => {
    const server = new MouffetteServer(0);
    server.start();
    await new Promise(resolve => server.wss.once('listening', resolve));
    const port = server.wss.address().port;
    const url = `ws://127.0.0.1:${port}`;
    const sender = trackedSocket(url);
    const target = trackedSocket(url);
    let uploadChannel;

    try {
        await Promise.all([sender.opened(), target.opened()]);
        await Promise.all([
            register(sender, senderId, 'integration-sender'),
            register(target, targetId, 'integration-target')
        ]);

        sender.ws.send(JSON.stringify({ type: 'request_upload_channel' }));
        const tokenMessage = await sender.next(message =>
            message.type === 'upload_channel_token');
        uploadChannel = trackedSocket(`${url}?channel=upload&token=${encodeURIComponent(tokenMessage.token)}`);
        await uploadChannel.opened();
        await uploadChannel.next(message => message.type === 'upload_channel_ready');

        const uploadId = '11111111-2222-4333-8444-555555555555';
        uploadChannel.ws.send(JSON.stringify({
            type: 'upload_start',
            targetPersistentClientId: targetId,
            uploadId,
            canvasSessionId,
            files: [{
                fileId,
                name: 'integration.png',
                extension: 'png',
                sizeBytes: 128,
                mediaIds: ['66666666-7777-4888-8999-aaaaaaaaaaaa']
            }]
        }));
        const start = await target.next(message =>
            message.type === 'upload_start' && message.uploadId === uploadId);
        assert.equal(start.senderPersistentClientId, senderId);

        target.ws.send(JSON.stringify({
            type: 'upload_ready', uploadId, canvasSessionId
        }));
        await sender.next(message => message.type === 'upload_ready'
            && message.uploadId === uploadId);

        uploadChannel.ws.send(JSON.stringify({
            type: 'upload_chunk',
            uploadId,
            canvasSessionId,
            fileId,
            chunkIndex: 0,
            data: Buffer.alloc(128, 0x3c).toString('base64')
        }));
        await target.next(message => message.type === 'upload_chunk'
            && message.uploadId === uploadId);
        target.ws.send(JSON.stringify({
            type: 'upload_progress',
            uploadId,
            percent: 99,
            receivedBytes: 128,
            perFileProgress: [{ fileId, percent: 99 }]
        }));
        const progress = await sender.next(message =>
            message.type === 'upload_progress' && message.uploadId === uploadId);
        assert.equal(progress.receivedBytes, 128);

        uploadChannel.ws.send(JSON.stringify({
            type: 'upload_complete', uploadId, canvasSessionId
        }));
        await target.next(message => message.type === 'upload_complete'
            && message.uploadId === uploadId);
        target.ws.send(JSON.stringify({
            type: 'upload_finished', uploadId, canvasSessionId, fileIds: [fileId]
        }));
        await sender.next(message => message.type === 'upload_finished'
            && message.uploadId === uploadId);

        const removalId = '22222222-3333-4444-8555-666666666666';
        sender.ws.send(JSON.stringify({
            type: 'remove_all_files',
            targetPersistentClientId: targetId,
            removalId,
            canvasSessionId
        }));
        const removal = await target.next(message =>
            message.type === 'remove_all_files' && message.removalId === removalId);
        target.ws.send(JSON.stringify({
            type: 'all_files_removed',
            removalId,
            canvasSessionId,
            senderClientId: removal.senderPersistentClientId
        }));
        await sender.next(message => message.type === 'all_files_removed'
            && message.removalId === removalId);

        const cancelledUploadId = '33333333-4444-4555-8666-777777777777';
        uploadChannel.ws.send(JSON.stringify({
            type: 'upload_start',
            targetPersistentClientId: targetId,
            uploadId: cancelledUploadId,
            canvasSessionId,
            files: [{
                fileId,
                name: 'integration.png',
                extension: 'png',
                sizeBytes: 128,
                mediaIds: ['88888888-9999-4aaa-8bbb-cccccccccccc']
            }]
        }));
        await target.next(message => message.type === 'upload_start'
            && message.uploadId === cancelledUploadId);
        target.ws.send(JSON.stringify({
            type: 'upload_ready', uploadId: cancelledUploadId, canvasSessionId
        }));
        await sender.next(message => message.type === 'upload_ready'
            && message.uploadId === cancelledUploadId);
        uploadChannel.ws.send(JSON.stringify({
            type: 'upload_abort', uploadId: cancelledUploadId, canvasSessionId
        }));
        const abortRequest = await target.next(message =>
            message.type === 'upload_abort' && message.uploadId === cancelledUploadId);
        target.ws.send(JSON.stringify({
            type: 'upload_abort_ack',
            uploadId: cancelledUploadId,
            canvasSessionId,
            senderClientId: abortRequest.senderPersistentClientId
        }));
        await sender.next(message => message.type === 'upload_aborted'
            && message.uploadId === cancelledUploadId);
        assert.equal(server.pendingUploadAborts.size, 0);

        const interruptedUploadId = '44444444-5555-4666-8777-888888888888';
        uploadChannel.ws.send(JSON.stringify({
            type: 'upload_start',
            targetPersistentClientId: targetId,
            uploadId: interruptedUploadId,
            canvasSessionId,
            files: [{
                fileId,
                name: 'integration.png',
                extension: 'png',
                sizeBytes: 128,
                mediaIds: ['99999999-aaaa-4bbb-8ccc-dddddddddddd']
            }]
        }));
        await target.next(message => message.type === 'upload_start'
            && message.uploadId === interruptedUploadId);
        target.ws.send(JSON.stringify({
            type: 'upload_ready', uploadId: interruptedUploadId, canvasSessionId
        }));
        await sender.next(message => message.type === 'upload_ready'
            && message.uploadId === interruptedUploadId);
        uploadChannel.ws.send(JSON.stringify({
            type: 'upload_chunk',
            uploadId: interruptedUploadId,
            canvasSessionId,
            fileId,
            chunkIndex: 0,
            data: Buffer.alloc(64, 0x7e).toString('base64')
        }));
        await target.next(message => message.type === 'upload_chunk'
            && message.uploadId === interruptedUploadId);
        await closeSocket(uploadChannel.ws);
        uploadChannel = null;

        const [rejected, abort] = await Promise.all([
            sender.next(message => message.type === 'upload_rejected'
                && message.uploadId === interruptedUploadId),
            target.next(message => message.type === 'upload_abort'
                && message.uploadId === interruptedUploadId)
        ]);
        assert.match(rejected.reason, /upload connection closed/i);
        assert.equal(abort.protocolRejected, true);
        assert.equal(server.uploads.size, 0);
        assert.equal(server.pendingRemovals.size, 0);

        // Exercise the remote-scene lifecycle over real control WebSockets too.
        // Deliberately bogus response destinations verify that the server routes
        // acknowledgements from its correlated run instead of trusting JSON.
        const sceneInstanceId = '55555555-6666-4777-8888-999999999999';
        sender.ws.send(JSON.stringify({
            type: 'remote_scene_start',
            targetClientId: targetId,
            scene: {
                sceneInstanceId,
                renderSchemaVersion: 2,
                screens: [{ id: 1 }],
                media: [{ mediaId: 'integration-video', type: 'video' }]
            }
        }));
        const remoteStart = await target.next(message =>
            message.type === 'remote_scene_start'
            && message.scene.sceneInstanceId === sceneInstanceId);
        assert.equal(remoteStart.senderClientId, senderId);

        target.ws.send(JSON.stringify({
            type: 'remote_scene_validation',
            targetClientId: 'bogus-destination',
            sceneInstanceId,
            success: true
        }));
        const validation = await sender.next(message =>
            message.type === 'remote_scene_validation'
            && message.sceneInstanceId === sceneInstanceId);
        assert.equal(validation.senderClientId, targetId);

        sender.ws.send(JSON.stringify({
            type: 'remote_scene_activate',
            targetClientId: targetId,
            sceneInstanceId,
            activationEpochMs: Date.now() + 250,
            activationDelayMs: 250
        }));
        await target.next(message => message.type === 'remote_scene_activate'
            && message.sceneInstanceId === sceneInstanceId);
        target.ws.send(JSON.stringify({
            type: 'remote_scene_launched',
            targetClientId: 'bogus-destination',
            sceneInstanceId
        }));
        await sender.next(message => message.type === 'remote_scene_launched'
            && message.sceneInstanceId === sceneInstanceId);

        sender.ws.send(JSON.stringify({
            type: 'remote_scene_video_sync',
            targetClientId: targetId,
            sceneInstanceId,
            sequence: 1,
            sampledEpochMs: Date.now(),
            videos: [{
                mediaId: 'integration-video',
                positionMs: 10,
                durationMs: 1000,
                playing: true,
                muted: false,
                visible: true,
                repeatAvailable: false
            }]
        }));
        await target.next(message => message.type === 'remote_scene_video_sync'
            && message.sequence === 1);

        const remoteStop = {
            type: 'remote_scene_stop',
            targetClientId: targetId,
            sceneInstanceId
        };
        sender.ws.send(JSON.stringify(remoteStop));
        sender.ws.send(JSON.stringify(remoteStop));
        await target.next(message => message.type === 'remote_scene_stop'
            && message.sceneInstanceId === sceneInstanceId);
        target.ws.send(JSON.stringify({
            type: 'remote_scene_stopped',
            targetClientId: 'bogus-destination',
            sceneInstanceId,
            success: true
        }));
        const stopped = await sender.next(message =>
            message.type === 'remote_scene_stopped'
            && message.sceneInstanceId === sceneInstanceId);
        assert.equal(stopped.success, true);
        assert.equal(stopped.senderClientId, targetId);
        assert.equal(server.remoteScenesByTarget.size, 0);

        console.log('upload transport integration tests passed');
    } finally {
        if (server.uploadCleanupInterval) clearInterval(server.uploadCleanupInterval);
        if (uploadChannel) await closeSocket(uploadChannel.ws);
        await Promise.all([closeSocket(sender.ws), closeSocket(target.ws)]);
        await new Promise(resolve => server.wss.close(resolve));
    }
})().catch(error => {
    console.error(error);
    process.exitCode = 1;
});
