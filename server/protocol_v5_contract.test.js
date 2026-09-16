'use strict';

const assert = require('node:assert/strict');
const crypto = require('node:crypto');
const WebSocket = require('ws');
const { MouffetteServer } = require('./server');

function socket() {
    return {
        readyState: WebSocket.OPEN,
        messages: [],
        closes: [],
        send(encoded) { this.messages.push(JSON.parse(encoded)); },
        close(code, reason) {
            this.closes.push({ code, reason });
            this.readyState = WebSocket.CLOSED;
        },
    };
}

function addClient(server, connectionId, endpointId, options = {}) {
    const ws = socket();
    const client = {
        id: connectionId,
        installationId: options.installationId || `installation-${endpointId}`,
        endpointId,
        instanceId: options.instanceId || 'primary',
        instanceOrdinal: options.instanceOrdinal || 1,
        runtimeId: options.runtimeId || `runtime-${endpointId}`,
        connectionGeneration: 1,
        authenticated: options.authenticated !== false,
        machineName: options.registered === false ? null : endpointId,
        platform: 'test',
        screens: [],
        systemUI: [],
        volumePercent: 50,
        ws,
    };
    server.clients.set(connectionId, client);
    return { client, ws };
}

function envelope(server, type, extra = {}) {
    return {
        type,
        protocolVersion: 5,
        serverBootId: server.serverBootId,
        messageId: crypto.randomUUID(),
        connectionGeneration: 1,
        ...(type === 'endpoint_snapshot' ? { instanceOrdinal: 1 } : {}),
        ...extra,
    };
}

function lastMessage(ws, type) {
    return ws.messages.filter(message => message.type === type).at(-1);
}

function screen(id = 1) {
    return { id, width: 1920, height: 1080, x: 0, y: 0,
        primary: id === 1 };
}

function containsRemovedWireKey(value, forbidden) {
    if (!value || typeof value !== 'object') return false;
    if (Array.isArray(value)) {
        return value.some(item => containsRemovedWireKey(item, forbidden));
    }
    return Object.entries(value).some(([key, nested]) =>
        forbidden.has(key) || containsRemovedWireKey(nested, forbidden));
}

// Envelope type is a bounded opaque scalar. In particular, an attacker cannot
// smuggle proof material inside an object that the generic protocol log would
// otherwise inspect.
{
    const server = new MouffetteServer(0);
    const owner = addClient(server, 'invalid-type-connection', 'A');
    server.handleMessage('invalid-type-connection', envelope(server, {
        resumeToken: 'must-never-reach-a-log',
    }));
    assert.equal(lastMessage(owner.ws, 'error').code, 'invalid_message_type');
    assert.equal(owner.ws.readyState, WebSocket.OPEN);
}

// Structured protocol logs carry bounded correlation fields only. Proof
// material supplied alongside an otherwise valid message is never serialized.
{
    const records = [];
    const server = new MouffetteServer({
        port: 0,
        protocolLogger: line => records.push(JSON.parse(line)),
    });
    addClient(server, 'logged-connection', 'logged-device');
    server.handleMessage('logged-connection', envelope(server,
        'request_client_list', { resumeToken: 'must-never-be-logged' }));
    assert.equal(records.length, 1);
    assert.equal(records[0].event, 'protocol_message_received');
    assert.equal(records[0].type, 'request_client_list');
    assert.equal(records[0].endpointId, 'logged-device');
    assert.equal(JSON.stringify(records).includes('must-never-be-logged'), false);
}

// The socket boundary completes every post-authentication server envelope.
// An explicit source generation on a relay is immutable; only a missing field
// defaults to the recipient's current transport generation.
{
    const server = new MouffetteServer(0);
    const recipient = addClient(server, 'recipient-connection', 'recipient-device');
    server.decorateProtocolSocket(recipient.ws);
    recipient.ws.mouffetteClient = recipient.client;
    recipient.client.connectionGeneration = 7;
    recipient.ws.send(JSON.stringify({
        type: 'test_outbound',
        protocolVersion: 1,
        serverBootId: crypto.randomUUID(),
        messageId: 'invalid-message-id',
        connectionGeneration: 99,
    }));
    const outbound = lastMessage(recipient.ws, 'test_outbound');
    assert.equal(outbound.protocolVersion, 5);
    assert.equal(outbound.serverBootId, server.serverBootId);
    assert.equal(outbound.connectionGeneration, 99);
    assert.match(outbound.messageId,
        /^[0-9a-f]{8}-[0-9a-f]{4}-4[0-9a-f]{3}-[89ab][0-9a-f]{3}-[0-9a-f]{12}$/i);
    recipient.ws.send(JSON.stringify({ type: 'test_server_originated' }));
    assert.equal(lastMessage(recipient.ws, 'test_server_originated')
        .connectionGeneration, 7);
}

// Version negotiation precedes authentication: an old client always receives
// the single explicit mismatch result and the server closes its connection.
{
    const server = new MouffetteServer(0);
    const old = addClient(server, 'old-connection', null, {
        authenticated: false,
        registered: false,
        runtimeId: null,
    });
    server.handleMessage('old-connection', {
        type: 'device_register',
        protocolVersion: 1,
    });
    const mismatch = lastMessage(old.ws, 'error');
    assert.equal(mismatch.code, 'protocol_version_mismatch');
    assert.equal(mismatch.protocolVersion, 5);
    assert.equal(mismatch.serverBootId, server.serverBootId);
    assert.equal(typeof mismatch.messageId, 'string');
    assert.deepEqual(old.ws.closes, [{ code: 1002, reason: 'Protocol version mismatch' }]);
}

// Device discovery uses only the v4 endpoint_snapshot family. It no longer
// emits the registration/state-sync aliases or identity aliases in snapshots.
{
    const server = new MouffetteServer(0);
    const owner = addClient(server, 'owner-connection', 'A', { registered: false });
    addClient(server, 'target-connection', 'B');

    server.handleMessage('owner-connection', envelope(server, 'endpoint_snapshot', {
        requestId: 'snapshot-request',
        machineName: 'Owner device',
        platform: 'test-platform',
        screens: [screen()],
        systemUI: [],
        volumePercent: 72,
    }));
    const applied = lastMessage(owner.ws, 'endpoint_snapshot_applied');
    assert.ok(applied);
    assert.equal(applied.requestId, 'snapshot-request');
    assert.equal(applied.snapshot.endpointId, 'A');
    assert.equal(applied.snapshot.machineName, 'Owner device');
    assert.equal(owner.ws.messages.some(message =>
        message.type === 'registration_confirmed' || message.type === 'state_sync'), false);

    const currentOwnerId = owner.client.id;
    server.handleMessage(currentOwnerId, envelope(server, 'request_client_list'));
    const list = lastMessage(owner.ws, 'client_list');
    assert.equal(list.clients.some(device => device.endpointId === 'B'), true);
    assert.equal(Object.hasOwn(list.clients.find(device => device.endpointId === 'B'), 'id'), false);
    assert.deepEqual(Object.keys(list.clients.find(device => device.endpointId === 'B')).sort(),
        ['endpointId', 'lastSeenAt', 'machineName', 'platform', 'status']);
    assert.equal(list.clients.find(device => device.endpointId === 'B').status, 'Available');
    const forbiddenOutputFields = new Set([
        'clientId', 'persistentClientId', 'persistentId', 'deviceId', 'sessionId',
        'canvasSessionId', 'targetClientId', 'targetPersistentClientId',
        'senderClientId', 'senderPersistentClientId', 'senderId', 'targetId',
    ]);
    assert.equal(containsRemovedWireKey(applied, forbiddenOutputFields), false);
    assert.equal(containsRemovedWireKey(list, forbiddenOutputFields), false);
}

// Discovery distinguishes two endpoints belonging to the same installation.
{
    const server = new MouffetteServer(0);
    const installationId = 'shared-installation';
    const primary = addClient(server, 'primary-connection', 'primary-endpoint', {
        installationId,
        instanceId: 'primary',
        instanceOrdinal: 1,
    });
    addClient(server, 'secondary-connection', 'secondary-endpoint', {
        installationId,
        instanceId: crypto.randomUUID(),
        instanceOrdinal: 2,
    });

    server.sendClientList('primary-connection');
    const discovered = lastMessage(primary.ws, 'client_list').clients;
    assert.equal(discovered.length, 1);
    assert.equal(discovered[0].endpointId, 'secondary-endpoint');
    assert.equal(Object.hasOwn(discovered[0], 'installationId'), false);
    assert.equal(Object.hasOwn(discovered[0], 'instanceOrdinal'), false);
}

// A device snapshot is a complete replacement. Required fields may not be
// omitted, and an explicitly empty topology clears the previous one.
{
    const requiredFields = ['machineName', 'platform', 'screens', 'systemUI', 'volumePercent'];
    const complete = {
        machineName: 'Complete device',
        platform: 'test-platform',
        screens: [screen(7)], systemUI: [],
        volumePercent: 41,
    };
    for (const field of requiredFields) {
        const server = new MouffetteServer(0);
        const owner = addClient(server, 'owner-connection', 'A', { registered: false });
        const incomplete = { ...complete };
        delete incomplete[field];
        server.handleMessage('owner-connection', envelope(server, 'endpoint_snapshot', incomplete));
        const rejected = lastMessage(owner.ws, 'error');
        assert.equal(rejected.code, 'invalid_endpoint_snapshot', field);
        assert.equal(lastMessage(owner.ws, 'endpoint_snapshot_applied'), undefined, field);
        assert.equal(owner.client.id, 'owner-connection', `${field} must fail before re-keying`);
        assert.equal(owner.client.machineName, null, `${field} must not partially mutate state`);
    }
}

{
    const server = new MouffetteServer(0);
    const owner = addClient(server, 'owner-connection', 'A', { registered: false });
    server.handleMessage('owner-connection', envelope(server, 'endpoint_snapshot', {
        machineName: 'Before', platform: 'before-platform',
        screens: [{ ...screen(), width: 2560, height: 1440 }],
        systemUI: [], volumePercent: 75,
    }));
    assert.equal(lastMessage(owner.ws, 'endpoint_snapshot_applied').snapshot.screens.length, 1);

    server.handleMessage(owner.client.id, envelope(server, 'endpoint_snapshot', {
        machineName: 'After', platform: 'after-platform',
        screens: [], systemUI: [], volumePercent: null,
    }));
    const applied = lastMessage(owner.ws, 'endpoint_snapshot_applied');
    assert.equal(applied.snapshot.machineName, 'After');
    assert.equal(applied.snapshot.platform, 'after-platform');
    assert.deepEqual(applied.snapshot.screens, []);
    assert.equal(applied.snapshot.volumePercent, null);
    assert.deepEqual(owner.client.screens, []);
    assert.equal(owner.client.volumePercent, null);
}

{
    for (const invalidVolume of [-1, 101, Number.NaN, '50']) {
        const server = new MouffetteServer(0);
        const owner = addClient(server, 'owner-connection', 'A', { registered: false });
        server.handleMessage('owner-connection', envelope(server, 'endpoint_snapshot', {
            machineName: 'Device', platform: 'test', screens: [], systemUI: [],
            volumePercent: invalidVolume,
        }));
        assert.equal(lastMessage(owner.ws, 'error').code, 'invalid_endpoint_snapshot');
        assert.equal(owner.client.machineName, null);
    }
}

// Every post-auth application command is bound to the current transport
// generation, including endpoint_snapshot (which used to be an unguarded path).
{
    const server = new MouffetteServer(0);
    const owner = addClient(server, 'owner-connection', 'A', { registered: false });
    server.handleMessage('owner-connection', envelope(server, 'endpoint_snapshot', {
        connectionGeneration: 2,
        machineName: 'Stale mutation', platform: 'test', screens: [],
        systemUI: [], volumePercent: 10,
    }));
    assert.equal(lastMessage(owner.ws, 'error').code, 'stale_connection_generation');
    assert.equal(owner.client.machineName, null);
}

// runtimeId is scoped by endpointId. Distinct signed installations that happen
// to use the same runtime UUID keep immutable, independent connection keys.
{
    const server = new MouffetteServer(0);
    const runtimeId = 'shared-runtime-id';
    const first = addClient(server, 'first-connection', 'A', {
        registered: false, runtimeId,
    });
    const second = addClient(server, 'second-connection', 'C', {
        registered: false, runtimeId,
    });
    server.handleMessage('first-connection', envelope(server, 'endpoint_snapshot', {
        machineName: 'First', platform: 'test', screens: [], systemUI: [], volumePercent: 10,
    }));
    server.handleMessage('second-connection', envelope(server, 'endpoint_snapshot', {
        machineName: 'Second', platform: 'test', screens: [], systemUI: [], volumePercent: 20,
    }));
    assert.equal(server.clients.get('first-connection'), first.client);
    assert.equal(server.clients.get('second-connection'), second.client);
    assert.equal(first.client.machineName, 'First');
    assert.equal(second.client.machineName, 'Second');
    assert.equal(first.ws.readyState, WebSocket.OPEN);
    assert.equal(second.ws.readyState, WebSocket.OPEN);
}

// A callback retained by a replaced socket cannot address the new client by
// spoofing its generation; neither resume nor snapshot reaches the dispatcher.
{
    const server = new MouffetteServer(0);
    const old = addClient(server, 'old-connection', 'A', { runtimeId: 'runtime-A' });
    addClient(server, 'target-connection', 'B');
    const session = server.remoteSessions.open({
        ownerEndpointId: 'A', targetEndpointId: 'B',
        ownerRuntimeId: 'runtime-A', targetRuntimeId: 'runtime-B',
        ownerConnectionGeneration: 1, targetConnectionGeneration: 1,
    }).session;
    server.remoteSessions.markDisconnected('A');
    server.clients.delete('old-connection');
    const replacement = addClient(server, 'new-connection', 'A', {
        runtimeId: 'runtime-A',
    });
    replacement.client.connectionGeneration = 2;

    assert.equal(server.handleControlSocketMessage(old.client, old.ws,
        envelope(server, 'remote_session_resume', {
            connectionGeneration: 2,
            remoteSessionId: session.remoteSessionId,
            generation: session.generation,
            resumeToken: session.resumeToken,
        })), false);
    assert.equal(session.phase, 'Grace');
    assert.equal(session.generation, 1);
    assert.equal(server.handleControlSocketMessage(old.client, old.ws,
        envelope(server, 'endpoint_snapshot', {
            connectionGeneration: 2,
            machineName: 'Spoofed', platform: 'test', screens: [], volumePercent: 0,
        })), false);
    assert.equal(replacement.client.machineName, 'A');
    assert.equal(old.ws.readyState, WebSocket.CLOSED);
}

// Every removed v1 family is rejected before any command handler can
// mutate state or relay a translated message. The healthy v4 socket stays up.
{
    const server = new MouffetteServer(0);
    const owner = addClient(server, 'owner-connection', 'A');
    const target = addClient(server, 'target-connection', 'B');
    const removedTypes = [
        'register', 'device_register',
        'request_screens', 'watch_screens', 'unwatch_screens', 'cursor_update',
        'canvas_created', 'canvas_deleted',
        'remove_file', 'remove_all_files', 'all_files_removed',
        'remove_all_files_failed', 'removal_rejected',
        'media_share', 'media_update', 'stop_sharing', 'incoming_media',
        'share_initiated', 'stop_media',
        'registration_confirmed', 'state_sync', 'screens_info',
        'watch_status', 'data_request',
        'remote_scene_start', 'remote_scene_update', 'remote_scene_stop',
    ];
    for (const type of removedTypes) {
        const targetMessageCount = target.ws.messages.length;
        server.handleMessage('owner-connection', envelope(server, type, {
            persistentClientId: 'B',
            canvasSessionId: 'removed-canvas',
            targetClientId: 'target-connection',
        }));
        assert.equal(lastMessage(owner.ws, 'error').code, 'removed_message_type', type);
        assert.equal(target.ws.messages.length, targetMessageCount,
            `${type} must not be relayed`);
    }
    assert.equal(owner.ws.readyState, WebSocket.OPEN);
}

// Canonical message names do not opt into field aliases. Any removed v1
// identity/routing alias rejects the whole request instead of being ignored.
{
    const removedFields = [
        'clientId', 'persistentClientId', 'persistentId', 'deviceId', 'sessionId',
        'canvasSessionId', 'targetClientId', 'targetPersistentClientId',
        'senderClientId', 'senderPersistentClientId', 'senderId', 'targetId',
    ];
    for (const field of removedFields) {
        const server = new MouffetteServer(0);
        const owner = addClient(server, 'owner-connection', 'A');
        addClient(server, 'target-connection', 'B');
        server.handleMessage('owner-connection', envelope(server, 'remote_session_open', {
            targetEndpointId: 'B',
            requestId: `request-${field}`,
            [field]: 'removed-alias',
        }));
        const rejected = lastMessage(owner.ws, 'error');
        assert.equal(rejected.code, 'removed_protocol_field', field);
        assert.match(rejected.message, new RegExp(`${field}$`));
        assert.equal(server.remoteSessions.sessions.size, 0,
            `${field} must not be translated into a v4 session open`);
        assert.equal(owner.ws.readyState, WebSocket.OPEN);
    }

    const server = new MouffetteServer(0);
    const owner = addClient(server, 'owner-connection', 'A');
    server.handleMessage('owner-connection', envelope(server, 'endpoint_snapshot', {
        machineName: 'Changed by removed payload', platform: 'test',
        screens: [{ id: 'screen-1', targetClientId: 'removed-nested-alias' }],
    }));
    assert.equal(lastMessage(owner.ws, 'error').code, 'removed_protocol_field');
    assert.equal(owner.client.machineName, 'A',
        'nested removed routing fields reject the entire snapshot');
}

// Even when placed in a canonical field, an internal transport/session key is
// not accepted as a device identity alias.
{
    const server = new MouffetteServer(0);
    const owner = addClient(server, 'owner-connection', 'A');
    addClient(server, 'target-connection', 'B');
    server.handleMessage('owner-connection', envelope(server, 'remote_session_open', {
        targetEndpointId: 'target-connection',
        requestId: 'transport-alias-open',
    }));
    const rejected = lastMessage(owner.ws, 'error');
    assert.equal(rejected.code, 'target_offline');
    assert.equal(rejected.targetEndpointId, 'target-connection');
    assert.equal(server.remoteSessions.sessions.size, 0);
}

// Disable is a server-authoritative drain: the endpoint disappears from
// discovery before its transport closes, no new session can target it, and
// every existing session enters exact-session teardown.
{
    const server = new MouffetteServer(0);
    const owner = addClient(server, 'owner-connection', 'A');
    const target = addClient(server, 'target-connection', 'B');
    const session = server.remoteSessions.open({
        ownerEndpointId: 'A', targetEndpointId: 'B',
        ownerRuntimeId: owner.client.runtimeId,
        targetRuntimeId: target.client.runtimeId,
        ownerConnectionGeneration: 1,
        targetConnectionGeneration: 1,
    }).session;

    server.handleMessage('target-connection',
        envelope(server, 'endpoint_disable'));
    assert.equal(target.client.draining, true);
    assert.ok(lastMessage(target.ws, 'endpoint_disable_started'));
    assert.ok(['Terminating', 'CleanupPending'].includes(session.phase));
    assert.ok(lastMessage(target.ws, 'remote_session_terminating'));

    server.handleMessage('owner-connection',
        envelope(server, 'request_client_list'));
    assert.equal(lastMessage(owner.ws, 'client_list').clients
        .some(client => client.endpointId === 'B'), false);

    server.handleMessage('owner-connection',
        envelope(server, 'remote_session_open', {
            targetEndpointId: 'B', requestId: 'open-draining-target',
        }));
    assert.equal(lastMessage(owner.ws, 'error').code, 'target_offline');
}

console.log('protocol v5 cut-over tests passed');
