const assert = require('node:assert/strict');
const WebSocket = require('ws');
const { MouffetteServer } = require('./server');

function fakeSocket() {
    return {
        readyState: WebSocket.OPEN,
        messages: [],
        send(payload) {
            this.messages.push(JSON.parse(payload));
        },
        close(code, reason) {
            this.readyState = WebSocket.CLOSED;
            this.closeCode = code;
            this.closeReason = reason;
        }
    };
}

function addClient(server, id, persistentId = id) {
    const ws = fakeSocket();
    server.clients.set(id, { id, sessionId: id, persistentId, machineName: id, ws });
    return ws;
}

function manifest(fileId = 'a'.repeat(64), mediaId = '11111111-1111-4111-8111-111111111111') {
    return [{
        fileId,
        name: 'pixel.png',
        extension: 'png',
        sizeBytes: 128,
        mediaIds: [mediaId]
    }];
}

const senderId = 'aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa';
const targetId = 'bbbbbbbb-bbbb-4bbb-8bbb-bbbbbbbbbbbb';
const attackerId = 'cccccccc-cccc-4ccc-8ccc-cccccccccccc';
const canvasSessionId = `${senderId}_TO_${targetId}_canvas_dddddddd-dddd-4ddd-8ddd-dddddddddddd`;

{
    const server = new MouffetteServer(0);
    addClient(server, senderId);
    addClient(server, targetId);
    addClient(server, attackerId);
    const uploadSocket = fakeSocket();
    const token = server.issueUploadChannelToken(senderId);
    const boundClient = server.consumeUploadChannelToken(token);
    const uploadId = '99999999-9999-4999-8999-999999999999';

    assert.equal(boundClient.id, senderId);
    assert.equal(server.consumeUploadChannelToken(token), null,
        'an upload channel token must be one-shot');

    server.handleUploadChannelMessage(boundClient, uploadSocket, {
        type: 'upload_start',
        senderClientId: attackerId,
        senderPersistentClientId: attackerId,
        targetPersistentClientId: targetId,
        uploadId,
        canvasSessionId,
        files: manifest()
    });

    assert.equal(server.uploads.get(uploadId).senderSession, senderId,
        'the authenticated channel binding must override a spoofed sender ID');
    server.handleUploadChannelMessage(boundClient, uploadSocket, {
        type: 'remote_scene_stop',
        senderClientId: attackerId,
        targetClientId: targetId
    });
    assert.equal(uploadSocket.messages.at(-1).type, 'error',
        'non-upload protocols must be rejected on the dedicated upload channel');

    const replacementToken = server.issueUploadChannelToken(senderId);
    server.registerUploadSocket(boundClient, uploadSocket);
    server.revokeUploadChannelsForClient(boundClient);
    assert.equal(server.consumeUploadChannelToken(replacementToken), null,
        'disconnecting or replacing the control client must revoke pending tokens');
    assert.equal(uploadSocket.readyState, WebSocket.CLOSED,
        'disconnecting or replacing the control client must close bound upload sockets');
}

{
    const server = new MouffetteServer(0);
    const sender = addClient(server, senderId);
    addClient(server, targetId);
    addClient(server, attackerId);
    const uploadId = 'eeeeeeee-eeee-4eee-8eee-eeeeeeeeeeee';

    server.handleUploadStart(senderId, {
        targetPersistentClientId: targetId,
        uploadId,
        canvasSessionId,
        files: manifest()
    });
    server.handleUploadComplete(senderId, { uploadId, canvasSessionId });

    assert.equal(server.uploads.has(uploadId), true, 'upload_complete must remain pending');
    assert.equal(server.clientFiles.has(targetId), false, 'unvalidated files must not enter server inventory');

    server.handleUploadFinished(attackerId, {
        uploadId,
        canvasSessionId,
        fileIds: ['a'.repeat(64)]
    });
    assert.equal(server.uploads.has(uploadId), true, 'a third party cannot acknowledge an upload');

    server.handleUploadFinished(targetId, {
        uploadId,
        canvasSessionId,
        fileIds: ['a'.repeat(64)]
    });
    assert.equal(server.uploads.has(uploadId), false, 'validated upload must close');
    assert.equal(server.clientFiles.get(targetId).get(canvasSessionId).has('a'.repeat(64)), true);
    assert.equal(server.clientFileOwners.get(targetId).get(canvasSessionId).get('a'.repeat(64)), senderId,
        'validated inventory must retain its authenticated sender owner');
    assert.equal(sender.messages.at(-1).type, 'upload_finished');
}

{
    const server = new MouffetteServer(0);
    const sender = addClient(server, senderId);
    const target = addClient(server, targetId);
    const attacker = addClient(server, attackerId);
    const uploadId = '13572468-2468-4246-8135-135724681357';
    const fileId = 'd'.repeat(64);

    server.handleUploadStart(senderId, {
        targetPersistentClientId: targetId,
        uploadId,
        canvasSessionId,
        files: manifest(fileId)
    });
    server.handleUploadComplete(senderId, { uploadId, canvasSessionId });
    server.handleUploadFinished(targetId, { uploadId, canvasSessionId, fileIds: [fileId] });

    const targetMessageCount = target.messages.length;
    server.handleRemoveFile(attackerId, {
        type: 'remove_file',
        targetPersistentClientId: targetId,
        canvasSessionId,
        fileId
    });
    assert.equal(server.clientFiles.get(targetId).get(canvasSessionId).has(fileId), true,
        'an attacker remove_file must not mutate server inventory');
    assert.equal(server.clientFileOwners.get(targetId).get(canvasSessionId).get(fileId), senderId,
        'an attacker remove_file must not mutate ownership metadata');
    assert.equal(target.messages.length, targetMessageCount,
        'an attacker remove_file must not be relayed to the target');
    assert.equal(attacker.messages.at(-1).type, 'error');

    server.handleRemoveAllFiles(attackerId, {
        type: 'remove_all_files',
        targetPersistentClientId: targetId,
        canvasSessionId,
        removalId: '24681357-1357-4246-8246-246813572468'
    });
    assert.equal(server.clientFiles.get(targetId).get(canvasSessionId).has(fileId), true,
        'an attacker remove_all_files must not mutate server inventory');
    assert.equal(server.clientFileOwners.get(targetId).get(canvasSessionId).get(fileId), senderId,
        'an attacker remove_all_files must not mutate ownership metadata');
    assert.equal(target.messages.length, targetMessageCount,
        'an attacker remove_all_files must not be relayed to the target');
    assert.equal(attacker.messages.at(-1).type, 'error');

    server.handleRemoveFile(senderId, {
        type: 'remove_file',
        targetPersistentClientId: targetId,
        canvasSessionId,
        fileId
    });
    assert.equal(server.clientFiles.has(targetId), false,
        'the authenticated owner must still be able to remove its file');
    assert.equal(server.clientFileOwners.has(targetId), false,
        'empty ownership metadata must be cleaned');
    assert.equal(target.messages.at(-1).type, 'remove_file');
    assert.equal(sender.messages.some(message => message.type === 'error'), false);
}

{
    const server = new MouffetteServer(0);
    const oldSenderSession = senderId;
    const newSenderSession = 'eeeeeeee-1111-4111-8111-eeeeeeeeeeee';
    const senderPersistentId = '12121212-3434-4567-8901-121212121212';
    addClient(server, oldSenderSession, senderPersistentId);
    const target = addClient(server, targetId);
    const attacker = addClient(server, attackerId);
    const uploadId = '31415926-5358-4979-8323-846264338327';
    const fileId = 'e'.repeat(64);

    server.handleUploadStart(oldSenderSession, {
        targetPersistentClientId: targetId,
        uploadId,
        canvasSessionId,
        files: manifest(fileId)
    });
    server.handleUploadComplete(oldSenderSession, { uploadId, canvasSessionId });
    server.handleUploadFinished(targetId, { uploadId, canvasSessionId, fileIds: [fileId] });
    assert.equal(server.clientFileOwners.get(targetId).get(canvasSessionId).get(fileId),
        senderPersistentId, 'file ownership must survive a new sender session');

    server.clients.delete(oldSenderSession);
    const reconnectedSender = addClient(server, newSenderSession, senderPersistentId);
    const removalId = '27182818-2845-4904-8235-360287471352';
    server.handleRemoveAllFiles(newSenderSession, {
        type: 'remove_all_files',
        targetPersistentClientId: targetId,
        canvasSessionId,
        removalId
    });

    assert.equal(server.clientFiles.get(targetId).get(canvasSessionId).has(fileId), true,
        'inventory must remain committed until the correlated target acknowledgement');
    assert.equal(server.pendingRemovals.has(removalId), true);
    assert.equal(target.messages.at(-1).senderPersistentClientId, senderPersistentId,
        'the target cache namespace must use stable sender identity');

    const senderMessageCount = reconnectedSender.messages.length;
    server.handleAllFilesRemoved(attackerId, {
        removalId,
        canvasSessionId,
        senderClientId: senderPersistentId
    });
    assert.equal(server.pendingRemovals.has(removalId), true,
        'an acknowledgement from the wrong target must not consume the pending removal');
    assert.equal(server.clientFiles.get(targetId).get(canvasSessionId).has(fileId), true,
        'a spoofed acknowledgement must not commit the inventory removal');
    assert.equal(reconnectedSender.messages.length, senderMessageCount);
    assert.equal(attacker.messages.at(-1).type, 'error');

    server.handleAllFilesRemoved(targetId, {
        removalId,
        canvasSessionId,
        senderClientId: senderPersistentId
    });
    assert.equal(server.pendingRemovals.has(removalId), false);
    assert.equal(server.clientFiles.has(targetId), false,
        'a reconnected session with the same persistent identity may unload its files');
    assert.equal(reconnectedSender.messages.at(-1).type, 'all_files_removed');
    assert.equal(reconnectedSender.messages.at(-1).removalId, removalId);
    assert.equal(reconnectedSender.messages.at(-1).targetPersistentClientId, targetId);

    const retryUploadId = '16180339-8874-4989-8482-045868343656';
    server.handleUploadStart(newSenderSession, {
        targetPersistentClientId: targetId,
        uploadId: retryUploadId,
        canvasSessionId,
        files: manifest(fileId)
    });
    server.handleUploadComplete(newSenderSession, {
        uploadId: retryUploadId,
        canvasSessionId
    });
    server.handleUploadFinished(targetId, {
        uploadId: retryUploadId,
        canvasSessionId,
        fileIds: [fileId]
    });
    assert.equal(server.clientFileOwners.get(targetId).get(canvasSessionId).get(fileId),
        senderPersistentId, 're-upload after reconnection must keep stable ownership');
}

{
    const server = new MouffetteServer(0);
    const sender = addClient(server, senderId);
    addClient(server, targetId);
    const uploadId = 'ffffffff-ffff-4fff-8fff-ffffffffffff';

    server.handleUploadStart(senderId, {
        targetPersistentClientId: targetId,
        uploadId,
        canvasSessionId,
        files: manifest()
    });
    server.handleUploadRejected(targetId, {
        uploadId,
        canvasSessionId,
        reason: 'invalid media'
    });

    assert.equal(server.uploads.has(uploadId), false);
    assert.equal(server.clientFiles.has(targetId), false);
    assert.deepEqual(sender.messages.at(-1), {
        type: 'upload_rejected',
        uploadId,
        reason: 'invalid media',
        canvasSessionId,
        targetClientId: targetId
    });
}

{
    const server = new MouffetteServer(0);
    const sender = addClient(server, senderId);
    const target = addClient(server, targetId);
    const uploadId = '87654321-4321-4321-8321-cba987654321';

    server.handleUploadStart(senderId, {
        targetPersistentClientId: targetId,
        uploadId,
        canvasSessionId,
        files: manifest()
    });
    server.handleUploadComplete(senderId, { uploadId, canvasSessionId });
    server.handleUploadFinished(targetId, {
        uploadId,
        canvasSessionId,
        fileIds: ['b'.repeat(64)]
    });

    assert.equal(server.uploads.has(uploadId), false,
        'an invalid target acknowledgement must close the pending upload');
    assert.equal(server.clientFiles.has(targetId), false,
        'an invalid target acknowledgement must not enter server inventory');
    assert.equal(sender.messages.at(-1).type, 'upload_rejected');
    assert.equal(target.messages.some(message => message.type === 'remove_file'
        && message.fileId === 'a'.repeat(64)), true,
    'the target must receive targeted cleanup for files it already validated');
}

{
    const server = new MouffetteServer(0);
    const sender = addClient(server, senderId);
    addClient(server, targetId);
    const uploadId = '12345678-1234-4234-8234-123456789abc';
    const duplicate = manifest()[0];

    server.handleUploadStart(senderId, {
        targetPersistentClientId: targetId,
        uploadId,
        canvasSessionId,
        files: [duplicate, {
            ...duplicate,
            mediaIds: ['22222222-2222-4222-8222-222222222222']
        }]
    });

    assert.equal(server.uploads.has(uploadId), false);
    assert.equal(sender.messages.at(-1).type, 'upload_rejected');
}

{
    const server = new MouffetteServer(0);
    addClient(server, senderId);
    addClient(server, targetId);
    const uploadId = 'abcdefab-cdef-4abc-8def-abcdefabcdef';

    server.handleUploadStart(senderId, {
        targetPersistentClientId: targetId,
        uploadId,
        canvasSessionId,
        files: manifest()
    });
    const upload = server.uploads.get(uploadId);
    server.UPLOAD_TIMEOUT_MS = 1000;
    upload.startTime = Date.now() - 5000;
    upload.lastActivity = Date.now();
    server.cleanupStalledUploads();
    assert.equal(server.uploads.has(uploadId), true,
        'an active long upload must use lastActivity rather than its original start time');

    upload.lastActivity = Date.now() - 5000;
    server.cleanupStalledUploads();
    assert.equal(server.uploads.has(uploadId), false,
        'an upload with no recent activity must still be timed out');
}

{
    const server = new MouffetteServer(0);
    const sender = addClient(server, senderId);
    const target = addClient(server, targetId);
    const uploadId = 'abcdef12-3456-4789-8abc-def123456789';

    server.handleUploadStart(senderId, {
        targetPersistentClientId: targetId,
        uploadId,
        canvasSessionId,
        files: manifest()
    });
    server.handleUploadComplete(senderId, { uploadId, canvasSessionId });
    const upload = server.uploads.get(uploadId);
    upload.awaitingTargetValidationSince = Date.now() - 5000;
    server.UPLOAD_TARGET_ACK_TIMEOUT_MS = 1000;
    server.cleanupStalledUploads();

    assert.equal(server.uploads.has(uploadId), false,
        'a completed upload must not wait indefinitely for its target acknowledgement');
    assert.equal(server.clientFiles.has(targetId), false,
        'a timed-out acknowledgement must not enter server inventory');
    assert.equal(sender.messages.at(-1).type, 'upload_rejected');
    assert.equal(target.messages.at(-1).type, 'upload_abort');
}

{
    const server = new MouffetteServer(0);
    addClient(server, senderId);
    const target = addClient(server, targetId);
    const uploadId = '11112222-3333-4444-8555-666677778888';

    server.handleUploadStart(senderId, {
        targetPersistentClientId: targetId,
        uploadId,
        canvasSessionId,
        files: manifest()
    });
    server.abortUploadsForClient(senderId);

    assert.equal(server.uploads.has(uploadId), false,
        'disconnecting an upload sender must close its server session');
    assert.equal(target.messages.at(-1).type, 'upload_abort');
    assert.equal(target.messages.at(-1).senderClientId, senderId);
}

{
    const server = new MouffetteServer(0);
    const sender = addClient(server, senderId);
    addClient(server, targetId);
    const uploadId = '99998888-7777-4666-8555-444433332222';

    server.handleUploadStart(senderId, {
        targetPersistentClientId: targetId,
        uploadId,
        canvasSessionId,
        files: manifest()
    });
    server.abortUploadsForClient(targetId);

    assert.equal(server.uploads.has(uploadId), false,
        'disconnecting an upload target must close its server session');
    assert.equal(sender.messages.at(-1).type, 'upload_rejected');
    assert.match(sender.messages.at(-1).reason, /target disconnected/i);
}

{
    const server = new MouffetteServer(0);
    const stableOwner = '42424242-4242-4242-8242-424242424242';
    const replacementSocketId = '56565656-5656-4565-8565-565656565656';
    const oldSocket = addClient(server, senderId, stableOwner);
    const target = addClient(server, targetId);
    addClient(server, replacementSocketId, stableOwner);
    const uploadId = '78787878-7878-4787-8787-787878787878';
    const sceneInstanceId = 'replacement-scene';

    server.handleUploadStart(senderId, {
        targetPersistentClientId: targetId,
        uploadId,
        canvasSessionId,
        files: manifest()
    });
    server.remoteScenesByTarget.set(targetId, {
        ownerId: senderId,
        sceneInstanceId
    });
    server.handleRegister(replacementSocketId, {
        persistentClientId: stableOwner,
        sessionId: senderId,
        machineName: 'replacement',
        screens: []
    });

    assert.equal(server.uploads.has(uploadId), false,
        'logical session replacement must explicitly purge the old upload');
    assert.equal(server.remoteScenesByTarget.has(targetId), false,
        'logical session replacement must explicitly purge the old remote scene');
    assert.equal(target.messages.some(message => message.type === 'upload_abort'
        && message.uploadId === uploadId), true);
    assert.equal(target.messages.some(message => message.type === 'remote_scene_stop'
        && message.sceneInstanceId === sceneInstanceId), true);
    assert.equal(server.clients.get(senderId).ws === oldSocket, false,
        'the replacement socket must own the logical session ID');
}

{
    const server = new MouffetteServer(0);
    const owner = addClient(server, senderId);
    const targetPersistentId = '90909090-9090-4090-8090-909090909090';
    const replacementSocketId = '67676767-6767-4676-8676-676767676767';
    addClient(server, targetId, targetPersistentId);
    addClient(server, replacementSocketId, targetPersistentId);
    const sceneInstanceId = 'target-replacement-scene';
    server.remoteScenesByTarget.set(targetId, {
        ownerId: senderId,
        sceneInstanceId
    });

    server.handleRegister(replacementSocketId, {
        persistentClientId: targetPersistentId,
        sessionId: targetId,
        machineName: 'replacement-target',
        screens: []
    });

    assert.equal(server.remoteScenesByTarget.has(targetId), false);
    assert.equal(owner.messages.some(message => message.type === 'remote_scene_stopped'
        && message.senderClientId === targetId
        && message.sceneInstanceId === sceneInstanceId
        && message.success === false), true,
    'atomic target replacement must notify the scene owner of correlated loss');
}

console.log('upload protocol tests passed');
