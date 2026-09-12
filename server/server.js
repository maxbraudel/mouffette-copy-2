const WebSocket = require('ws');
const { v4: uuidv4 } = require('uuid');
const crypto = require('node:crypto');
const { loadServerConfig } = require('./config');
const { PROTOCOL_VERSION, createChallenge, verifyAuthResponse } = require('./device_auth');
const { RemoteSessionRegistry } = require('./remote_session_registry');
const { ProtocolMetrics } = require('./protocol_metrics');
const {
    SCENE_PHASES, SceneRunRegistry, computeSceneDigest, isPlainObject,
} = require('./scene_run_registry');

const DEFAULT_CONFIG = loadServerConfig();
const CURSOR_DEBUG = DEFAULT_CONFIG.cursorDebug;
const CANONICAL_UUID_PATTERN = /^[0-9a-f]{8}-[0-9a-f]{4}-[1-5][0-9a-f]{3}-[89ab][0-9a-f]{3}-[0-9a-f]{12}$/i;
const CANVAS_SESSION_ID_PATTERN = /^[A-Za-z0-9_-]{1,512}$/;
const OPAQUE_ID_PATTERN = /^[A-Za-z0-9_-]{1,128}$/;
const SHA256_PATTERN = /^[0-9a-f]{64}$/;
const ALLOWED_MEDIA_EXTENSIONS = new Set(['png', 'jpg', 'jpeg', 'webp', 'avif', 'mp4']);
// Protocol v3 is a hard cut-over. These names are deliberately rejected at
// the envelope boundary rather than translated into their v3 counterparts.
const LEGACY_MESSAGE_TYPES = new Set([
    'register', 'device_register',
    'request_screens', 'watch_screens', 'unwatch_screens', 'cursor_update',
    'canvas_created', 'canvas_deleted',
    'remove_file', 'remove_all_files', 'all_files_removed',
    'remove_all_files_failed', 'removal_rejected',
    'media_share', 'media_update', 'stop_sharing', 'incoming_media',
    'share_initiated', 'stop_media',
    'registration_confirmed', 'state_sync', 'screens_info',
    'watch_status', 'data_request',
]);
const LEGACY_WIRE_FIELDS = Object.freeze([
    'clientId', 'persistentClientId', 'persistentId', 'deviceId', 'sessionId',
    'canvasSessionId', 'targetClientId', 'targetPersistentClientId',
    'senderClientId', 'senderPersistentClientId', 'senderId', 'targetId',
]);

function findLegacyWireField(value) {
    if (!value || typeof value !== 'object') return null;
    const pending = [value];
    const visited = new Set();
    while (pending.length > 0) {
        const current = pending.pop();
        if (!current || typeof current !== 'object' || visited.has(current)) continue;
        visited.add(current);
        for (const [key, nested] of Object.entries(current)) {
            if (LEGACY_WIRE_FIELDS.includes(key)) return key;
            if (nested && typeof nested === 'object') pending.push(nested);
        }
    }
    return null;
}

// ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
// MOUFFETTE SERVER - PROTOCOL V3 IDENTITY BOUNDARY
// ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
//
// Protocol v3 authenticates one installation key, derives one addressable
// endpoint per application instance, and keeps transport runtime identity
// separate. Legacy wire identifiers are never accepted as aliases.
// ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━

class MouffetteServer {
    constructor(portOrOptions) {
        const options = portOrOptions && typeof portOrOptions === 'object'
            ? portOrOptions : {};
        this.config = options.config || DEFAULT_CONFIG;
        this.port = typeof portOrOptions === 'number' ? portOrOptions
            : (Number.isInteger(options.port) ? options.port : this.config.port);
        this.host = options.host || this.config.host;
        this.epochNow = options.epochNow || (() => Date.now());
        this.monotonicNow = options.monotonicNow
            || (() => Number(process.hrtime.bigint() / 1_000_000n));
        this.protocolVersion = PROTOCOL_VERSION;
        this.serverBootId = uuidv4();
        this.protocolLogger = options.protocolLogger === undefined
            ? console.log : options.protocolLogger;
        this.metrics = options.metrics || new ProtocolMetrics({ logger: options.metricLogger });
        this.clients = new Map(); // transport key -> authenticated endpoint state
        this.connectionGenerationByEndpoint = new Map();
        this.wss = null;
        this.uploads = new Map(); // uploadId -> protocol-v3 endpoint/session state
        this.uploadTombstones = new Map(); // uploadId -> bounded terminal v3 result
        this.pendingAssetRemovals = new Map(); // removalId -> immutable session-scoped removal
        this.assetRemovalTombstones = new Map(); // removalId -> bounded committed/error result
        this.connectionsByEndpoint = new Map(); // endpointId -> Set(connectionId)
        this.sessionAssets = new Map(); // remoteSessionId -> Map(assetId -> validated metadata)

        this.UPLOAD_TIMEOUT_MS = this.config.uploadIdleTimeoutMs;
        this.UPLOAD_TARGET_ACK_TIMEOUT_MS = this.config.uploadTargetAckTimeoutMs;
        this.REMOVAL_ACK_TIMEOUT_MS = this.config.removalAckTimeoutMs;
        this.ASSET_REMOVAL_TOMBSTONE_TTL_MS = Math.max(
            60_000, this.REMOVAL_ACK_TIMEOUT_MS * 2);
        this.MAX_UPLOAD_FILES = 256;
        this.MAX_UPLOAD_FILE_BYTES = 16 * 1024 * 1024 * 1024;
        this.MAX_UPLOAD_TOTAL_BYTES = 64 * 1024 * 1024 * 1024;
        this.MAX_UPLOAD_CHUNK_BASE64_LENGTH = Math.ceil((128 * 1024) / 3) * 4;
        this.MAX_TARGET_BUFFERED_UPLOAD_BYTES = 8 * 1024 * 1024;
        this.MAX_PENDING_REMOVALS = 4096;
        this.MAX_REMOTE_SCENE_BYTES = 8 * 1024 * 1024;
        this.MAX_REMOTE_SCENE_SYNC_BYTES = 256 * 1024;
        this.MAX_REMOTE_SCENE_STATE_SNAPSHOT_BYTES = 8 * 1024 * 1024;
        this.MAX_REMOTE_SCENE_SCREENS = 64;
        this.MAX_REMOTE_SCENE_MEDIA = 512;
        this.MAX_REMOTE_SCENE_SYNC_ITEMS = 512;
        this.MAX_REMOTE_SCENE_BUFFERED_BYTES = 1024 * 1024;
        this.MAX_REMOTE_SCENE_TOMBSTONES = 4096;
        this.MAX_SCENE_PROGRESS_ITEMS = 2048;
        this.UPLOAD_CHANNEL_TOKEN_TTL_MS = 30 * 1000;
        this.uploadChannelTokens = new Map();
        this.uploadSocketsByClient = new Map();
        this.uploadChannelMessageTypes = new Set([
            'upload_start', 'upload_resume', 'upload_chunk', 'upload_complete', 'upload_abort'
        ]);
        this.uploadCleanupInterval = null;
        this.leaseSweepInterval = null;
        this.remoteSessions = new RemoteSessionRegistry({
            leaseTimeoutMs: this.config.leaseTimeoutMs,
            maximumTombstones: this.MAX_REMOTE_SCENE_TOMBSTONES,
            monotonicNow: this.monotonicNow,
            epochNow: this.epochNow,
        });
        this.sceneRuns = new SceneRunRegistry({
            prepareTimeoutMs: this.config.scenePrepareTimeoutMs,
            activationLeadMs: this.config.sceneActivationLeadMs,
            startedAckTimeoutMs: this.config.sceneStartedAckTimeoutMs ?? 5_000,
            maximumClockUncertaintyMs: this.config.sceneMaxClockSkewMs,
            maximumStartSkewMs: this.config.sceneMaxStartSkewMs ?? 750,
            stopTimeoutMs: this.config.leaseTimeoutMs,
            maximumTombstones: this.MAX_REMOTE_SCENE_TOMBSTONES,
            epochNow: this.epochNow,
            monotonicNow: this.monotonicNow,
        });
        for (const warning of this.config.warnings) console.warn(`⚠️ ${warning}`);
    }

    logProtocolEvent(event, fields = {}) {
        const record = {
            timestamp: new Date().toISOString(),
            event: String(event || 'protocol_event').slice(0, 128),
            serverBootId: this.serverBootId,
        };
        for (const [key, value] of Object.entries(fields)) {
            if (!/^[A-Za-z][A-Za-z0-9_]{0,63}$/.test(key)) continue;
            if (typeof value === 'string') record[key] = value.slice(0, 256);
            else if (typeof value === 'number' && Number.isFinite(value)) record[key] = value;
            else if (typeof value === 'boolean') record[key] = value;
        }
        if (typeof this.protocolLogger === 'function') {
            this.protocolLogger(JSON.stringify(record));
        }
    }

    start() {
        // Bind explicitly to 0.0.0.0 to listen on all IPv4 interfaces (LAN accessible)
        this.wss = new WebSocket.Server({
            port: this.port,
            host: this.host,
            // Upload chunks are much smaller, while legitimate scene-control
            // payloads can exceed 512 KiB on complex canvases.
            maxPayload: 16 * 1024 * 1024,
            perMessageDeflate: false
        });
        
        console.log(`🎯 Mouffette Server v${this.protocolVersion} started on ws://${this.host}:${this.port}`);
        console.log(`🔁 Server boot id: ${this.serverBootId}`);
        
        // Sweep frequently so stalled partial state is released promptly.
        this.uploadCleanupInterval = setInterval(() => {
            this.cleanupStalledUploadsV3();
        }, 5000);
        this.leaseSweepInterval = setInterval(() => this.sweepRemoteSessionLeases(), 100);
        console.log(`🧹 Upload timeout cleanup started (timeout: ${this.UPLOAD_TIMEOUT_MS}ms)`);
        
        this.wss.on('connection', (ws, req) => {
            this.decorateProtocolSocket(ws);
            // Parse query parameters to detect upload channel connections
            const url = new URL(req.url || '/', 'ws://dummy');
            const channel = url.searchParams.get('channel');
            const isUploadChannel = (channel === 'upload');
            
            if (isUploadChannel) {
                const boundClient = this.consumeUploadChannelToken(url.searchParams.get('token'));
                if (!boundClient) {
                    console.warn('⚠️ Rejected unauthenticated upload channel');
                    ws.send(JSON.stringify({
                        type: 'error',
                        message: 'Invalid or expired upload channel token'
                    }));
                    ws.close(1008, 'Upload channel authentication failed');
                    return;
                }

                ws.mouffetteClient = boundClient;
                this.registerUploadSocket(boundClient, ws);
                console.log(`📤 Authenticated upload channel connected for ${boundClient.id}`);

                ws.on('message', (data) => {
                    try {
                        const message = JSON.parse(data.toString());
                        this.handleUploadChannelMessage(boundClient, ws, message);
                    } catch (error) {
                        console.error('❌ Error parsing upload channel message:', error);
                        ws.send(JSON.stringify({
                            type: 'error',
                            message: 'Invalid JSON format'
                        }));
                    }
                });
                
                ws.on('close', () => {
                    if (!ws.mouffettePreserveSessionUploads) {
                        this.abortUploadsForUploadSocketV3(boundClient.id, ws,
                            'Dedicated upload connection closed');
                    }
                    this.unregisterUploadSocket(boundClient, ws);
                    console.log(`📤 Upload channel disconnected for ${boundClient.id}`);
                });
                
                ws.on('error', (error) => {
                    if (!ws.mouffettePreserveSessionUploads) {
                        this.abortUploadsForUploadSocketV3(boundClient.id, ws,
                            'Dedicated upload connection failed');
                    }
                    this.unregisterUploadSocket(boundClient, ws);
                    console.error(`❌ Upload channel error for ${boundClient.id}:`, error);
                });

                ws.send(JSON.stringify({
                    type: 'upload_channel_ready',
                    protocolVersion: this.protocolVersion,
                    serverBootId: this.serverBootId,
                    messageId: uuidv4(),
                    endpointId: boundClient.endpointId,
                    connectionGeneration: boundClient.connectionGeneration,
                }));
                return;
            }
            
            // Regular control channel connection
            const clientId = uuidv4();
            const clientInfo = {
                id: clientId,
                installationId: null,
                endpointId: null,
                instanceId: null,
                instanceOrdinal: null,
                runtimeId: null,
                connectionGeneration: 1,
                authenticated: false,
                ws: ws,
                machineName: null,
                screens: [],
                systemUI: [], // array of system UI element rects
                volumePercent: -1,
                status: 'connected',
                connectedAt: new Date().toISOString(),
                socketLabel: clientId
            };
            ws.mouffetteClient = clientInfo;
            
            this.clients.set(clientId, clientInfo);
            console.log(`📱 New client connected: ${clientId}`);
            
            clientInfo.authChallenge = createChallenge(this.serverBootId);
            ws.send(JSON.stringify({
                type: 'auth_challenge',
                messageId: uuidv4(),
                ...clientInfo.authChallenge,
            }));
            clientInfo.authTimer = setTimeout(() => {
                if (!clientInfo.authenticated && ws.readyState === WebSocket.OPEN) {
                    ws.close(1008, 'Authentication timeout');
                }
            }, 10_000);
            
            ws.on('message', (data) => {
                try {
                    const message = JSON.parse(data.toString());
                    this.handleControlSocketMessage(clientInfo, ws, message);
                } catch (error) {
                    console.error('❌ Error parsing message:', error);
                    ws.send(JSON.stringify({
                        type: 'error',
                        message: 'Invalid JSON format'
                    }));
                }
            });
            
            ws.on('close', () => {
                if (clientInfo.authTimer) clearTimeout(clientInfo.authTimer);
                if (clientInfo.replaced) {
                    return;
                }
                const finalId = clientInfo.id;
                console.log(`📱 Client disconnected: ${finalId}`);
                const protectedByLease = this.handleRemoteSessionDeparture(clientInfo);
                this.revokeUploadChannelsForClient(clientInfo, protectedByLease);
                if (!protectedByLease) {
                    this.abortUploadsForClientV3(finalId);
                }
                if (clientInfo.endpointId) {
                    this.unregisterConnectionForEndpoint(clientInfo.endpointId, finalId);
                }
                this.clients.delete(finalId);
                this.broadcastClientList();
            });
            
            ws.on('error', (error) => {
                if (clientInfo.authTimer) clearTimeout(clientInfo.authTimer);
                if (clientInfo.replaced) {
                    return;
                }
                const finalId = clientInfo.id;
                console.error(`❌ WebSocket error for client ${finalId}:`, error);
                const protectedByLease = this.handleRemoteSessionDeparture(clientInfo);
                this.revokeUploadChannelsForClient(clientInfo, protectedByLease);
                if (!protectedByLease) {
                    this.abortUploadsForClientV3(finalId);
                }
                if (clientInfo.endpointId) {
                    this.unregisterConnectionForEndpoint(clientInfo.endpointId, finalId);
                }
                this.clients.delete(finalId);
                this.broadcastClientList();
            });
            
            // Discovery begins only after the signed identity has been verified
            // and device metadata has been registered.
        });
    }

    decorateProtocolSocket(ws) {
        if (!ws || ws.mouffetteProtocolDecorated === true) return;
        const rawSend = ws.send.bind(ws);
        ws.send = (data, ...arguments_) => {
            let encoded = data;
            if (typeof data === 'string') {
                try {
                    const payload = JSON.parse(data);
                    if (isPlainObject(payload)) {
                        const client = ws.mouffetteClient;
                        const envelope = {
                            ...payload,
                            protocolVersion: this.protocolVersion,
                            serverBootId: this.serverBootId,
                            messageId: CANONICAL_UUID_PATTERN.test(payload.messageId || '')
                                ? payload.messageId : uuidv4(),
                        };
                        if (client && client.authenticated
                            && Number.isSafeInteger(client.connectionGeneration)
                            && client.connectionGeneration > 0
                            && !Object.hasOwn(envelope, 'connectionGeneration')) {
                            // Server-originated frames default to the recipient
                            // transport. Relayed upload/removal frames carry an
                            // explicit source generation and must retain it.
                            envelope.connectionGeneration = client.connectionGeneration;
                        }
                        encoded = JSON.stringify(envelope);
                    }
                } catch (_) {
                    // Non-JSON strings are left byte-for-byte unchanged.
                }
            }
            return rawSend(encoded, ...arguments_);
        };
        ws.mouffetteProtocolDecorated = true;
    }

    handleControlSocketMessage(client, sourceSocket, message) {
        if (!client || !sourceSocket || this.clients.get(client.id) !== client
            || client.ws !== sourceSocket) {
            if (sourceSocket && sourceSocket.readyState === WebSocket.OPEN) {
                sourceSocket.close(1008, 'Stale control channel');
            }
            return false;
        }
        this.handleMessage(client.id, message);
        return true;
    }

    removeExpiredUploadChannelTokens(now = Date.now()) {
        for (const [token, binding] of this.uploadChannelTokens) {
            if (!binding || binding.expiresAt <= now) {
                this.uploadChannelTokens.delete(token);
            }
        }
    }

    revokeUploadTokensForClient(client) {
        if (!client) return;
        for (const [token, binding] of this.uploadChannelTokens) {
            if (binding && binding.client === client) {
                this.uploadChannelTokens.delete(token);
            }
        }
    }

    issueUploadChannelToken(clientId) {
        const client = this.clients.get(clientId);
        if (!client || !client.endpointId || !client.ws
            || client.ws.readyState !== WebSocket.OPEN) {
            return null;
        }

        this.removeExpiredUploadChannelTokens();
        this.revokeUploadTokensForClient(client);

        let token;
        do {
            token = crypto.randomBytes(32).toString('base64url');
        } while (this.uploadChannelTokens.has(token));

        const expiresAt = Date.now() + this.UPLOAD_CHANNEL_TOKEN_TTL_MS;
        this.uploadChannelTokens.set(token, { client, expiresAt });
        client.ws.send(JSON.stringify({
            type: 'upload_channel_token',
            protocolVersion: this.protocolVersion,
            serverBootId: this.serverBootId,
            messageId: uuidv4(),
            connectionGeneration: client.connectionGeneration,
            token,
            expiresAt
        }));
        return token;
    }

    consumeUploadChannelToken(token) {
        if (typeof token !== 'string' || token.length < 32 || token.length > 128) {
            return null;
        }

        const binding = this.uploadChannelTokens.get(token);
        this.uploadChannelTokens.delete(token);
        this.removeExpiredUploadChannelTokens();
        if (!binding || binding.expiresAt <= Date.now()) return null;

        const client = binding.client;
        if (!client || this.clients.get(client.id) !== client || !client.ws
            || client.ws.readyState !== WebSocket.OPEN) {
            return null;
        }
        return client;
    }

    registerUploadSocket(client, ws) {
        let sockets = this.uploadSocketsByClient.get(client);
        if (!sockets) {
            sockets = new Set();
            this.uploadSocketsByClient.set(client, sockets);
        }
        // A control client owns one high-throughput channel. Replacing it is
        // explicit and closes any transfer pinned to the obsolete socket.
        for (const existing of Array.from(sockets)) {
            if (existing === ws) continue;
            sockets.delete(existing);
            if (existing && (existing.readyState === WebSocket.OPEN
                || existing.readyState === WebSocket.CONNECTING)) {
                existing.close(1008, 'Upload channel replaced');
            }
        }
        sockets.add(ws);
    }

    unregisterUploadSocket(client, ws) {
        const sockets = this.uploadSocketsByClient.get(client);
        if (!sockets) return;
        sockets.delete(ws);
        if (sockets.size === 0) {
            this.uploadSocketsByClient.delete(client);
        }
    }

    revokeUploadChannelsForClient(client, preserveSessionUploads = false) {
        if (!client) return;
        this.revokeUploadTokensForClient(client);
        const sockets = this.uploadSocketsByClient.get(client);
        this.uploadSocketsByClient.delete(client);
        if (!sockets) return;
        for (const socket of sockets) {
            if (preserveSessionUploads && socket) socket.mouffettePreserveSessionUploads = true;
            if (socket && (socket.readyState === WebSocket.OPEN
                || socket.readyState === WebSocket.CONNECTING)) {
                socket.close(1008, 'Control channel disconnected');
            }
        }
    }

    handleUploadChannelMessage(boundClient, ws, message) {
        if (!boundClient || this.clients.get(boundClient.id) !== boundClient
            || !boundClient.ws || boundClient.ws.readyState !== WebSocket.OPEN) {
            ws.send(JSON.stringify({
                type: 'error',
                message: 'Upload channel is no longer authenticated'
            }));
            ws.close(1008, 'Control channel unavailable');
            return;
        }

        if (!message || typeof message !== 'object' || Array.isArray(message)
            || !this.uploadChannelMessageTypes.has(message.type)) {
            console.warn(`⚠️ Rejected ${message && message.type ? message.type : 'invalid message'} on upload channel`);
            ws.send(JSON.stringify({
                type: 'error',
                message: 'Message type is not allowed on the upload channel'
            }));
            return;
        }

        // Authenticated socket binding is authoritative in v3. Do not add the
        // sender aliases used by the v1 relay protocol to the wire envelope.
        this.handleMessage(boundClient.id, message, ws);
    }

    serializedJsonWithinLimit(value, byteLimit) {
        try {
            const serialized = JSON.stringify(value);
            if (typeof serialized !== 'string'
                || Buffer.byteLength(serialized, 'utf8') > byteLimit) {
                return null;
            }
            return serialized;
        } catch (_error) {
            return null;
        }
    }

    isValidOpaqueId(value) {
        return typeof value === 'string' && OPAQUE_ID_PATTERN.test(value);
    }

    scenePayload(run, type, extra = {}) {
        return {
            type,
            protocolVersion: this.protocolVersion,
            serverBootId: this.serverBootId,
            messageId: uuidv4(),
            remoteSessionId: run.remoteSessionId,
            generation: run.generation,
            sceneRunId: run.sceneRunId,
            revision: run.revision,
            digest: run.digest,
            phase: run.phase,
            ownerEndpointId: run.ownerEndpointId,
            targetEndpointId: run.targetEndpointId,
            ...extra,
        };
    }

    sendSceneError(clientId, code, message, run = null) {
        const payload = {
            type: 'error',
            protocolVersion: this.protocolVersion,
            serverBootId: this.serverBootId,
            messageId: uuidv4(),
            scope: 'scene',
            code,
            message: String(message || code).slice(0, 512),
        };
        if (run) {
            payload.remoteSessionId = run.remoteSessionId;
            payload.generation = run.generation;
            payload.sceneRunId = run.sceneRunId;
            payload.digest = run.digest;
        }
        if (run && code === 'clock_uncertainty_too_high') {
            this.metrics.incrementOnce('scene_clock_uncertainty_rejected_total',
                run.sceneRunId);
        }
        if (run && (run.failed === true
            || code.includes('prepare') || code.includes('checklist')
            || code.includes('digest') || code.includes('asset'))) {
            this.metrics.incrementOnce('scene_prepare_failed_total', run.sceneRunId);
        }
        this.sendToEndpoint(this.getEndpointId(clientId), payload);
    }

    countScenePreparationFailure(message) {
        const correlationId = this.isValidOpaqueId(message && message.sceneRunId)
            ? message.sceneRunId
            : (CANONICAL_UUID_PATTERN.test(message && message.messageId || '')
                ? message.messageId : null);
        if (correlationId) {
            this.metrics.incrementOnce('scene_prepare_failed_total', correlationId);
        }
    }

    validateSessionMessage(clientId, message, options = {}) {
        const client = this.clients.get(clientId);
        const session = client && this.remoteSessions.get(message.remoteSessionId);
        if (!client || !session) return { ok: false, error: 'unknown_remote_session' };
        const role = session.ownerEndpointId === client.endpointId ? 'owner'
            : session.targetEndpointId === client.endpointId ? 'target' : null;
        if (!role) return { ok: false, error: 'not_a_session_party' };
        if (options.ownerOnly && role !== 'owner') return { ok: false, error: 'not_session_owner' };
        if (message.connectionGeneration !== client.connectionGeneration) {
            return { ok: false, error: 'stale_connection_generation' };
        }
        const boundConnectionGeneration = role === 'owner'
            ? session.ownerConnectionGeneration : session.targetConnectionGeneration;
        if (boundConnectionGeneration !== client.connectionGeneration) {
            return { ok: false, error: 'stale_connection_generation' };
        }
        if (message.generation !== session.generation) {
            return { ok: false, error: 'stale_remote_session_generation' };
        }
        if (!['Active', 'Grace'].includes(session.phase)) {
            return { ok: false, error: options.allowGrace
                ? 'remote_session_terminal' : 'remote_session_not_active' };
        }
        const lease = this.remoteSessions.validateLease(session.remoteSessionId);
        if (!lease.ok) {
            if (lease.terminalTransition && lease.session) {
                this.metrics.incrementOnce('remote_session_lease_expired_total',
                    lease.session.remoteSessionId);
                this.beginRemoteSessionTeardown(lease.session);
            }
            return { ok: false, error: lease.error };
        }
        if (!options.allowGrace && session.phase !== 'Active') {
            return { ok: false, error: 'remote_session_not_active' };
        }
        return { ok: true, client, session, role };
    }

    normalizeSceneManifest(manifest) {
        if (!Array.isArray(manifest) || manifest.length > this.MAX_UPLOAD_FILES) return null;
        const normalized = [];
        const assetIds = new Set();
        for (const entry of manifest) {
            if (!isPlainObject(entry) || !this.isValidOpaqueId(entry.assetId)
                || assetIds.has(entry.assetId) || !SHA256_PATTERN.test(entry.fileId || '')
                || !SHA256_PATTERN.test(entry.sha256 || '') || entry.fileId !== entry.sha256
                || !Number.isSafeInteger(entry.size) || entry.size < 1
                || entry.size > this.MAX_UPLOAD_FILE_BYTES
                || typeof entry.extension !== 'string'
                || !ALLOWED_MEDIA_EXTENSIONS.has(entry.extension.toLowerCase())) return null;
            const mediaIds = Array.isArray(entry.mediaIds) ? entry.mediaIds.slice() : [];
            if (mediaIds.length > 4096 || mediaIds.some(id => !this.isValidOpaqueId(id))
                || new Set(mediaIds).size !== mediaIds.length) return null;
            normalized.push({
                assetId: entry.assetId,
                extension: entry.extension.toLowerCase(),
                fileId: entry.fileId,
                mediaIds: mediaIds.sort(),
                sha256: entry.sha256,
                size: entry.size,
            });
            assetIds.add(entry.assetId);
        }
        return normalized.sort((left, right) => left.assetId.localeCompare(right.assetId));
    }

    validateSceneInventory(session, manifest) {
        const assets = this.sessionAssets.get(session.remoteSessionId);
        for (const entry of manifest) {
            const stored = assets && assets.get(entry.assetId);
            if (!stored || stored.remoteSessionId !== session.remoteSessionId
                || stored.generation !== session.generation
                || stored.ownerEndpointId !== session.ownerEndpointId
                || stored.targetEndpointId !== session.targetEndpointId
                || stored.fileId !== entry.fileId || stored.sha256 !== entry.sha256
                || stored.size !== entry.size || stored.extension !== entry.extension) {
                return { ok: false, assetId: entry.assetId };
            }
        }
        return { ok: true };
    }

    validateSceneMediaBindings(scene, manifest) {
        const assets = new Map(manifest.map(asset => [asset.assetId, asset]));
        const media = new Map();
        for (const item of scene.media) {
            if (!isPlainObject(item) || !this.isValidOpaqueId(item.mediaId)
                || media.has(item.mediaId) || !['image', 'video', 'text'].includes(item.type)) {
                return false;
            }
            if (item.type !== 'text') {
                const asset = assets.get(item.assetId);
                if (!asset || !asset.mediaIds.includes(item.mediaId)) return false;
            }
            media.set(item.mediaId, item);
        }
        for (const asset of manifest) {
            if (asset.mediaIds.some(mediaId => {
                const item = media.get(mediaId);
                return !item || item.assetId !== asset.assetId;
            })) return false;
        }
        return true;
    }

    handleScenePrepare(clientId, message) {
        const validated = this.validateSessionMessage(clientId, message, { ownerOnly: true });
        if (!validated.ok) return this.sendSceneError(clientId, validated.error, validated.error);
        const { client, session } = validated;
        if (Array.from(this.pendingAssetRemovals.values()).some(removal =>
            removal.remoteSessionId === session.remoteSessionId)) {
            this.countScenePreparationFailure(message);
            return this.sendSceneError(clientId, 'asset_removal_pending',
                'A remote asset removal must settle before scene preparation');
        }
        if (session.activeUploadIds.size > 0) {
            this.countScenePreparationFailure(message);
            return this.sendSceneError(clientId, 'uploads_still_active',
                'Every upload must be validated before scene preparation');
        }
        const manifest = this.normalizeSceneManifest(message.manifest);
        const scene = message.scene;
        if (!this.isValidOpaqueId(message.sceneRunId)
            || !Number.isSafeInteger(message.revision) || message.revision < 1
            || !manifest || !isPlainObject(scene)
            || !Array.isArray(scene.screens) || scene.screens.length > this.MAX_REMOTE_SCENE_SCREENS
            || !Array.isArray(scene.media) || scene.media.length > this.MAX_REMOTE_SCENE_MEDIA
            || !this.validateSceneMediaBindings(scene, manifest)
            || !this.serializedJsonWithinLimit(scene, this.MAX_REMOTE_SCENE_BYTES)) {
            this.countScenePreparationFailure(message);
            return this.sendSceneError(clientId, 'invalid_scene_manifest', 'Invalid scene revision, manifest, or payload');
        }
        if (!this.requiredSceneChecklist({ manifest, scene })) {
            this.countScenePreparationFailure(message);
            return this.sendSceneError(clientId, 'invalid_scene_manifest',
                'Scene requires more preparation checks than the protocol permits');
        }
        const digest = computeSceneDigest(message.revision, manifest, scene);
        if (message.digest !== digest) {
            this.countScenePreparationFailure(message);
            return this.sendSceneError(clientId, 'scene_digest_mismatch', 'Scene digest does not match the canonical payload');
        }
        const inventory = this.validateSceneInventory(session, manifest);
        if (!inventory.ok) {
            this.countScenePreparationFailure(message);
            return this.sendSceneError(clientId, 'scene_asset_not_validated',
                `Asset ${inventory.assetId} is not validated for this session generation`);
        }
        const prepared = this.sceneRuns.prepare({
            remoteSessionId: session.remoteSessionId,
            generation: session.generation,
            sceneRunId: message.sceneRunId,
            revision: message.revision,
            digest,
            manifest,
            scene,
            ownerEndpointId: session.ownerEndpointId,
            targetEndpointId: session.targetEndpointId,
        });
        if (!prepared.ok) {
            this.countScenePreparationFailure(message);
            return this.sendSceneError(clientId, prepared.error, prepared.error);
        }
        const run = prepared.run;
        session.sceneRunId = run.sceneRunId;
        if (prepared.replay) {
            this.sendToEndpoint(client.endpointId, this.scenePayload(run, 'prepare_progress', {
                aggregate: true,
                replay: true,
                percent: run.phase === SCENE_PHASES.PREPARING ? 0 : 100,
            }));
            return;
        }
        const delivered = this.sendToEndpoint(session.targetEndpointId,
            this.scenePayload(run, 'scene_prepare', { manifest, scene }));
        if (!delivered) {
            this.initiateSceneStop(run, 'scene_target_unavailable', true);
            return this.sendSceneError(clientId, 'scene_target_unavailable',
                'Scene target is unavailable', run);
        }
        this.sendToEndpoint(client.endpointId, this.scenePayload(run, 'prepare_progress', {
            aggregate: true,
            percent: 0,
            stage: 'accepted',
        }));
    }

    safeChecklistId(value, fallback) {
        const normalized = String(value === undefined || value === null ? '' : value)
            .replace(/[^A-Za-z0-9_-]/g, '_');
        return (normalized || fallback).slice(0, 128);
    }

    requiredSceneChecklist(run) {
        if (!run || !Array.isArray(run.manifest) || !isPlainObject(run.scene)
            || !Array.isArray(run.scene.screens) || !Array.isArray(run.scene.media)) {
            return null;
        }
        const manifest = new Map(run.manifest.map(asset => [asset.assetId, asset]));
        const required = [];
        const append = (itemId, stage) => {
            if (required.length >= this.MAX_SCENE_PROGRESS_ITEMS) return false;
            required.push({ itemId, stage });
            return true;
        };
        for (let index = 0; index < run.scene.screens.length; ++index) {
            const screen = run.scene.screens[index];
            if (!isPlainObject(screen)) return null;
            const rawId = ['string', 'number', 'boolean'].includes(typeof screen.id)
                ? String(screen.id) : '';
            const itemId = this.safeChecklistId(
                `screen_${rawId}`, `screen_${index}`);
            if (!append(itemId, 'screen_render_graph_ready')) return null;
        }
        for (let index = 0; index < run.scene.media.length; ++index) {
            const media = run.scene.media[index];
            if (!isPlainObject(media) || !this.isValidOpaqueId(media.mediaId)
                || !['image', 'video', 'text'].includes(media.type)) return null;
            if (media.type !== 'text') {
                const asset = manifest.get(media.assetId);
                if (!asset || !asset.mediaIds.includes(media.mediaId)) return null;
            }
            const base = this.safeChecklistId(media.mediaId, `media_${index}`);
            const appendStage = (suffix, stage) => append(
                this.safeChecklistId(`${base}_${suffix}`, base), stage);
            if (media.type !== 'text'
                && !appendStage('file', 'file_validated')) return null;
            if (media.type === 'text') {
                if (!appendStage('glyphs', 'text_glyphs_ready')
                    || !appendStage('layout', 'text_layout_ready')) return null;
            } else if (media.type === 'video') {
                if (!appendStage('decode', 'video_decoded')
                    || !appendStage('frame', 'first_frame_positioned')
                    || !appendStage('audio', 'audio_prepared')
                    || !appendStage('graph', 'render_graph_ready')) return null;
            } else if (!appendStage('decode', 'image_decoded')
                       || !appendStage('texture', 'image_texture_ready')) return null;
        }
        if (required.length === 0) {
            required.push({ itemId: 'scene', stage: 'screen_render_graph_ready' });
        }
        return required;
    }

    validateChecklist(run, checklist) {
        const required = this.requiredSceneChecklist(run);
        if (!required || !Array.isArray(checklist)
            || checklist.length !== required.length
            || checklist.length < 1
            || checklist.length > this.MAX_SCENE_PROGRESS_ITEMS) return false;
        const allowedKeys = new Set(['itemId', 'stage', 'ready']);
        return checklist.every((item, index) => isPlainObject(item)
            && Object.keys(item).every(key => allowedKeys.has(key))
            && this.isValidOpaqueId(item.itemId)
            && item.itemId === required[index].itemId
            && item.stage === required[index].stage
            && typeof item.ready === 'boolean');
    }

    handleSceneProgress(clientId, message) {
        const validated = this.validateSessionMessage(clientId, message);
        const run = this.sceneRuns.get(message.sceneRunId);
        if (!validated.ok || !run || run.remoteSessionId !== message.remoteSessionId) {
            const code = validated.ok ? 'unknown_scene_run' : validated.error;
            return this.sendSceneError(clientId, code, code, run);
        }
        if (!Number.isInteger(message.percent) || message.percent < 0 || message.percent > 100
            || !this.validateChecklist(run, message.checklist)
            || message.percent !== Math.floor(
                message.checklist.filter(item => item.ready).length * 100
                    / message.checklist.length)
            || !this.serializedJsonWithinLimit(message.checklist, this.MAX_REMOTE_SCENE_SYNC_BYTES)) {
            return this.sendSceneError(clientId, 'invalid_prepare_progress', 'Invalid scene preparation progress', run);
        }
        const result = this.sceneRuns.recordProgress(run.sceneRunId,
            validated.client.endpointId, { percent: message.percent, checklist: message.checklist });
        if (!result.ok) return this.sendSceneError(clientId, result.error, result.error, run);
        this.sendToEndpoint(run.ownerEndpointId, this.scenePayload(run, 'prepare_progress', {
            reporterEndpointId: validated.client.endpointId,
            percent: message.percent,
            checklist: message.checklist,
        }));
    }

    handleScenePrepared(clientId, message) {
        const validated = this.validateSessionMessage(clientId, message);
        const run = this.sceneRuns.get(message.sceneRunId);
        if (!validated.ok || !run || run.remoteSessionId !== message.remoteSessionId) {
            const code = validated.ok ? 'unknown_scene_run' : validated.error;
            return this.sendSceneError(clientId, code, code, run);
        }
        if (message.success !== true) {
            const code = typeof message.errorCode === 'string'
                ? message.errorCode.slice(0, 128) : 'scene_prepare_failed';
            this.initiateSceneStop(run, code, true);
            return this.sendSceneError(this.resolveClientId(run.ownerEndpointId), code,
                typeof message.message === 'string' ? message.message : 'Scene preparation failed', run);
        }
        if (!this.validateChecklist(run, message.checklist)
            || message.checklist.some(item => item.ready !== true)) {
            this.initiateSceneStop(run, 'scene_checklist_incomplete', true);
            return this.sendSceneError(clientId, 'scene_checklist_incomplete',
                'Every preparation checklist item must be ready', run);
        }
        const result = this.sceneRuns.markPrepared(run.sceneRunId,
            validated.client.endpointId, message.digest);
        if (!result.ok) {
            // PREPARED is the fail-closed boundary: a malformed or mismatched
            // acknowledgement means neither peer may retain a prepared graph.
            // Scene teardown deliberately leaves the RemoteSession and its
            // validated upload inventory intact for an explicit retry.
            this.initiateSceneStop(run, result.error, true);
            return this.sendSceneError(clientId, result.error, result.error, run);
        }
        const payload = this.scenePayload(run, 'prepared', {
            reporterEndpointId: validated.client.endpointId,
            allPrepared: result.ready,
            checklist: message.checklist,
        });
        this.sendToEndpoint(run.ownerEndpointId, payload);
        this.sendToEndpoint(run.targetEndpointId, payload);
    }

    handleSceneArmed(clientId, message) {
        const validated = this.validateSessionMessage(clientId, message);
        const run = this.sceneRuns.get(message.sceneRunId);
        if (!validated.ok || !run || run.remoteSessionId !== message.remoteSessionId) {
            const code = validated.ok ? 'unknown_scene_run' : validated.error;
            return this.sendSceneError(clientId, code, code, run);
        }
        const result = this.sceneRuns.arm(run.sceneRunId, validated.client.endpointId,
            message.digest, message.clockUncertaintyMs);
        if (!result.ok) {
            // At this point both the authenticated socket and RemoteSession
            // envelope are current and correlated to the run. Any invalid
            // ARMED acknowledgement makes the prepared barrier unusable.
            // Validation failures above intentionally remain non-destructive
            // so stale/foreign packets cannot tear down a valid run.
            this.initiateSceneStop(run, result.error, true);
            return this.sendSceneError(clientId, result.error, result.error, run);
        }
        if (!result.scheduled) {
            const payload = this.scenePayload(run, 'armed', {
                reporterEndpointId: validated.client.endpointId,
                allArmed: false,
            });
            this.sendToEndpoint(run.ownerEndpointId, payload);
            this.sendToEndpoint(run.targetEndpointId, payload);
            return;
        }
        const commit = this.scenePayload(run, 'commit', {
            allArmed: true,
            startEpochMs: run.startEpochMs,
            startServerMonotonicMs: run.startServerMonotonicMs,
            activationLeadMs: this.config.sceneActivationLeadMs,
            maximumClockUncertaintyMs: this.config.sceneMaxClockSkewMs,
        });
        const ownerDelivered = this.sendToEndpoint(run.ownerEndpointId, commit);
        const targetDelivered = this.sendToEndpoint(run.targetEndpointId, commit);
        if (!ownerDelivered || !targetDelivered) {
            this.initiateSceneStop(run, 'scene_commit_delivery_failed', true);
        }
    }

    handleSceneStarted(clientId, message) {
        const validated = this.validateSessionMessage(clientId, message, { allowGrace: true });
        const run = this.sceneRuns.get(message.sceneRunId);
        if (!validated.ok || !run || run.remoteSessionId !== message.remoteSessionId) {
            const code = validated.ok ? 'unknown_scene_run' : validated.error;
            return this.sendSceneError(clientId, code, code, run);
        }
        const result = this.sceneRuns.markStarted(run.sceneRunId, validated.client.endpointId,
            message.digest, message.firstFramePresented,
            message.presentedServerMonotonicMs);
        if (!result.ok) {
            if (result.error === 'scene_start_skew_too_high') {
                this.metrics.incrementOnce('scene_start_skew_rejected_total',
                    run.sceneRunId);
            }
            this.initiateSceneStop(run, result.error, true);
            return this.sendSceneError(clientId, result.error, result.error, run);
        }
        const payload = this.scenePayload(run, 'started', {
            reporterEndpointId: validated.client.endpointId,
            allStarted: result.live,
            firstFramePresented: true,
            presentedServerMonotonicMs: message.presentedServerMonotonicMs,
            startSkewMs: result.live ? result.startSkewMs : undefined,
        });
        this.sendToEndpoint(run.ownerEndpointId, payload);
        this.sendToEndpoint(run.targetEndpointId, payload);
    }

    handleSceneStateSnapshot(clientId, message) {
        const validated = this.validateSessionMessage(clientId, message,
            { ownerOnly: true, allowGrace: true });
        const run = this.sceneRuns.get(message.sceneRunId);
        if (!validated.ok || !run || run.remoteSessionId !== message.remoteSessionId) {
            const code = validated.ok ? 'unknown_scene_run' : validated.error;
            return this.sendSceneError(clientId, code, code, run);
        }
        const sampledServerMonotonicMs = message.sampledServerMonotonicMs;
        const currentServerMonotonicMs = this.monotonicNow();
        if (!Number.isSafeInteger(sampledServerMonotonicMs)
            || sampledServerMonotonicMs < 0
            || sampledServerMonotonicMs
                > currentServerMonotonicMs + this.config.sceneMaxClockSkewMs) {
            return this.sendSceneError(clientId, 'invalid_state_snapshot_timestamp',
                'Invalid scene state snapshot timestamp', run);
        }
        if (!isPlainObject(message.snapshot)
            || !this.serializedJsonWithinLimit(
                message.snapshot, this.MAX_REMOTE_SCENE_STATE_SNAPSHOT_BYTES)) {
            return this.sendSceneError(clientId, 'invalid_state_snapshot', 'Invalid scene state snapshot', run);
        }
        const result = this.sceneRuns.acceptSnapshot(run.sceneRunId,
            validated.client.endpointId, message.digest, message.sequence);
        if (!result.ok) return this.sendSceneError(clientId, result.error, result.error, run);
        this.sendToEndpoint(run.targetEndpointId, this.scenePayload(run, 'state_snapshot', {
            sequence: message.sequence,
            sampledServerMonotonicMs,
            snapshot: message.snapshot,
        }));
    }

    handleSceneStop(clientId, message) {
        const validated = this.validateSessionMessage(clientId, message,
            { ownerOnly: true, allowGrace: true });
        const run = this.sceneRuns.get(message.sceneRunId);
        if (!validated.ok) return this.sendSceneError(clientId, validated.error, validated.error, run);
        if (!run || run.remoteSessionId !== message.remoteSessionId) {
            const tombstone = this.sceneRuns.tombstones.get(message.sceneRunId);
            if (tombstone && tombstone.remoteSessionId === message.remoteSessionId) {
                return this.sendToEndpoint(validated.client.endpointId,
                    this.scenePayload(tombstone, 'stopped', { success: true, replay: true }));
            }
            return this.sendSceneError(clientId, 'unknown_scene_run', 'Unknown scene run');
        }
        if (message.digest !== run.digest) {
            return this.sendSceneError(clientId, 'scene_digest_mismatch', 'Scene digest mismatch', run);
        }
        this.initiateSceneStop(run,
            typeof message.reason === 'string' ? message.reason : 'owner_stop', false);
    }

    initiateSceneStop(run, reason, failed) {
        if (!run) return;
        const result = this.sceneRuns.stop(run.sceneRunId, reason, { failed });
        if (!result.ok || result.completed) return;
        const payload = this.scenePayload(result.run, 'stop', {
            reason: result.run.stopReason,
            failed: result.run.failed,
            replay: result.replay === true,
        });
        this.sendToEndpoint(result.run.ownerEndpointId, payload);
        this.sendToEndpoint(result.run.targetEndpointId, payload);
    }

    handleSceneStopped(clientId, message) {
        const validated = this.validateSessionMessage(clientId, message, { allowGrace: true });
        const run = this.sceneRuns.get(message.sceneRunId);
        if (!validated.ok || !run || run.remoteSessionId !== message.remoteSessionId) {
            const code = validated.ok ? 'unknown_scene_run' : validated.error;
            return this.sendSceneError(clientId, code, code, run);
        }
        const result = this.sceneRuns.acknowledgeStopped(run.sceneRunId,
            validated.client.endpointId, message.success);
        if (!result.ok) return this.sendSceneError(clientId, result.error,
            typeof message.message === 'string' ? message.message : result.error, run);
        if (!result.completed) return;
        const session = this.remoteSessions.get(result.run.remoteSessionId);
        if (session && session.sceneRunId === result.run.sceneRunId) session.sceneRunId = null;
        const payload = this.scenePayload(result.run, 'stopped', {
            success: true,
            failed: result.run.phase === SCENE_PHASES.FAILED,
            reason: result.run.stopReason,
        });
        this.sendToEndpoint(result.run.ownerEndpointId, payload);
        this.sendToEndpoint(result.run.targetEndpointId, payload);
        this.dispatchReadyAssetRemovalsForSession(result.run.remoteSessionId);
    }

    sweepSceneRuns(now = this.epochNow(), nowMonotonic = this.monotonicNow()) {
        for (const action of this.sceneRuns.tick(now, nowMonotonic)) {
            if (action.type === 'stop') {
                const payload = this.scenePayload(action.run, 'stop', {
                    reason: action.code,
                    failed: true,
                });
                this.sendToEndpoint(action.run.ownerEndpointId, payload);
                this.sendToEndpoint(action.run.targetEndpointId, payload);
                this.sendSceneError(this.resolveClientId(action.run.ownerEndpointId),
                    action.code, action.code, action.run);
            } else if (action.type === 'finalized') {
                const session = this.remoteSessions.get(action.run.remoteSessionId);
                if (session && session.sceneRunId === action.run.sceneRunId) session.sceneRunId = null;
                const payload = this.scenePayload(action.run, 'stopped', {
                    success: false,
                    failed: action.run.phase === SCENE_PHASES.FAILED,
                    reason: action.code,
                });
                this.sendToEndpoint(action.run.ownerEndpointId, payload);
                this.sendToEndpoint(action.run.targetEndpointId, payload);
                if (!this.failAssetRemovalsWaitingForSceneV3(
                    action.run, 'scene_stop_timeout', now)) {
                    this.dispatchReadyAssetRemovalsForSession(action.run.remoteSessionId);
                }
            }
        }
    }

    handleMessage(clientId, message, uploadTransportSocket = null) {
        const client = this.clients.get(clientId);
        if (!client) return;

        if (!message || message.protocolVersion !== this.protocolVersion) {
            this.sendError(clientId, 'Protocol version mismatch', 'protocol_version_mismatch');
            if (client.ws && client.ws.readyState === WebSocket.OPEN) {
                client.ws.close(1002, 'Protocol version mismatch');
            }
            return;
        }
        if (!this.isValidOpaqueId(message.type)) {
            this.sendError(clientId, 'Invalid protocol v3 message type',
                'invalid_message_type');
            return;
        }
        if (LEGACY_MESSAGE_TYPES.has(message.type)
            || (typeof message.type === 'string' && message.type.startsWith('remote_scene_'))) {
            this.sendError(clientId,
                `Legacy message type is not supported by protocol v3: ${message.type}`,
                'legacy_message_type');
            return;
        }
        const legacyField = findLegacyWireField(message);
        if (legacyField) {
            this.sendError(clientId,
                `Legacy field is not supported by protocol v3: ${legacyField}`,
                'legacy_protocol_field');
            return;
        }
        if (message.type === 'auth_response') {
            if (!CANONICAL_UUID_PATTERN.test(message.messageId || '')) {
                this.sendError(clientId,
                    'Missing or invalid message identifier', 'invalid_message_id');
                if (client.ws && client.ws.readyState === WebSocket.OPEN) {
                    client.ws.close(1002, 'Invalid protocol envelope');
                }
                return;
            }
            this.handleAuthResponse(clientId, message);
            return;
        }
        if (!client.authenticated) {
            this.sendError(clientId, 'authentication_required', 'authentication_required');
            if (client.ws && client.ws.readyState === WebSocket.OPEN) {
                client.ws.close(1008, 'Authentication required');
            }
            return;
        }
        if (message.serverBootId !== this.serverBootId) {
            this.sendError(clientId, 'Server boot identifier mismatch', 'server_boot_mismatch');
            return;
        }
        if (!CANONICAL_UUID_PATTERN.test(message.messageId || '')) {
            this.sendError(clientId, 'Missing or invalid message identifier', 'invalid_message_id');
            return;
        }
        if (!Number.isSafeInteger(message.connectionGeneration)
            || message.connectionGeneration !== client.connectionGeneration) {
            this.sendError(clientId, 'Stale connection generation',
                'stale_connection_generation');
            return;
        }
        const receivedAt = this.monotonicNow();
        if (this.clientLeaseExpired(client, receivedAt)) {
            this.sendError(clientId, 'Heartbeat lease expired', 'lease_expired');
            this.expireRemoteSessionsForClient(client, receivedAt);
            this.retireClientTransport(
                client, false, 1001, 'Heartbeat lease expired');
            this.broadcastClientList();
            return;
        }
        
        if (message.type !== 'upload_chunk' && message.type !== 'upload_progress'
            && message.type !== 'prepare_progress' && message.type !== 'state_snapshot') {
            this.logProtocolEvent('protocol_message_received', {
                connectionId: clientId,
                endpointId: client.endpointId,
                messageId: message.messageId,
                type: message.type,
            });
        }
        
        switch (message.type) {
            case 'endpoint_snapshot':
                this.handleEndpointSnapshot(clientId, message);
                break;
            case 'request_client_list':
                this.sendClientList(clientId);
                break;
            case 'request_upload_channel':
                if (!this.issueUploadChannelToken(clientId)) {
                    this.sendError(clientId, 'Upload channel requires a registered control connection');
                }
                break;
            // Upload flow: track state and relay
            case 'upload_start':
                this.handleUploadStartV3(clientId, message, uploadTransportSocket);
                break;
            case 'upload_resume':
                this.handleUploadResumeV3(clientId, message, uploadTransportSocket);
                break;
            case 'upload_chunk':
                this.handleUploadChunkV3(clientId, message, uploadTransportSocket);
                break;
            case 'upload_complete':
                this.handleUploadCompleteV3(clientId, message, uploadTransportSocket);
                break;
            case 'upload_abort':
                this.handleUploadAbortV3(clientId, message);
                break;
            // Progress/status notifications from target back to sender
            case 'upload_progress':
                this.handleUploadProgressV3(clientId, message);
                break;
            case 'upload_ready':
                this.handleUploadReadyV3(clientId, message);
                break;
            case 'upload_finished':
                this.handleUploadFinishedV3(clientId, message);
                break;
            case 'upload_rejected':
                this.handleUploadRejectedV3(clientId, message);
                break;
            case 'upload_abort_ack':
                this.handleUploadAbortAcknowledgementV3(clientId, message);
                break;
            case 'upload_remove':
                this.handleUploadRemoveV3(clientId, message);
                break;
            case 'upload_removed':
                this.handleUploadRemovedV3(clientId, message);
                break;
            case 'scene_prepare':
                this.handleScenePrepare(clientId, message);
                break;
            case 'prepare_progress':
                this.handleSceneProgress(clientId, message);
                break;
            case 'prepared':
                this.handleScenePrepared(clientId, message);
                break;
            case 'armed':
                this.handleSceneArmed(clientId, message);
                break;
            case 'started':
                this.handleSceneStarted(clientId, message);
                break;
            case 'state_snapshot':
                this.handleSceneStateSnapshot(clientId, message);
                break;
            case 'stop':
                this.handleSceneStop(clientId, message);
                break;
            case 'stopped':
                this.handleSceneStopped(clientId, message);
                break;
            case 'heartbeat':
                this.handleHeartbeat(clientId, message);
                break;
            case 'remote_session_open':
                this.handleRemoteSessionOpen(clientId, message);
                break;
            case 'remote_session_resume':
                this.handleRemoteSessionResume(clientId, message);
                break;
            case 'remote_session_close':
                this.handleRemoteSessionClose(clientId, message);
                break;
            case 'remote_session_teardown_ack':
                this.handleRemoteSessionTeardownAck(clientId, message);
                break;
            default:
                this.sendError(clientId, 'Unknown protocol v3 message type', 'unknown_message_type');
        }
    }

    clientLeaseExpired(client, now = this.monotonicNow()) {
        const lastContact = Number.isFinite(client && client.lastHeartbeatMonotonicAt)
            ? client.lastHeartbeatMonotonicAt : client && client.lastHeartbeatAt;
        return !!client && client.authenticated
            && Number.isFinite(lastContact)
            && now >= lastContact + this.config.leaseTimeoutMs;
    }

    expireRemoteSessionsForClient(client, now = this.monotonicNow()) {
        if (!client || !client.endpointId) return 0;
        let transitions = 0;
        for (const session of this.remoteSessions.sessionsForEndpoint(client.endpointId)) {
            const result = this.remoteSessions.terminate(
                session.remoteSessionId, 'lease_expired', now);
            if (!result.ok || result.replay) continue;
            ++transitions;
            this.metrics.incrementOnce('remote_session_lease_expired_total',
                result.session.remoteSessionId);
            this.beginRemoteSessionTeardown(result.session);
        }
        return transitions;
    }

    retireClientTransport(client, preserveSessionUploads, code, reason) {
        if (!client) return false;
        const current = this.clients.get(client.id);
        if (current !== client) return false;
        client.replaced = true;
        this.revokeUploadChannelsForClient(client, preserveSessionUploads);
        if (!preserveSessionUploads) this.abortUploadsForClientV3(client.id);
        if (client.endpointId) {
            this.unregisterConnectionForEndpoint(client.endpointId, client.id);
        }
        this.clients.delete(client.id);
        if (client.ws && (client.ws.readyState === WebSocket.OPEN
            || client.ws.readyState === WebSocket.CONNECTING)) {
            client.ws.close(code, reason);
        }
        return true;
    }

    sweepExpiredClientTransports(now = this.monotonicNow()) {
        let removed = 0;
        for (const client of Array.from(this.clients.values())) {
            if (!this.clientLeaseExpired(client, now)) continue;
            this.expireRemoteSessionsForClient(client, now);
            if (this.retireClientTransport(
                client, false, 1001, 'Heartbeat lease expired')) ++removed;
        }
        if (removed > 0) this.broadcastClientList();
        return removed;
    }

    handleAuthResponse(clientId, message, timing = null) {
        const epochNow = typeof timing === 'number' ? timing
            : timing && Number.isFinite(timing.epochMs)
                ? timing.epochMs : this.epochNow();
        const monotonicNow = typeof timing === 'number' ? timing
            : timing && Number.isFinite(timing.monotonicMs)
                ? timing.monotonicMs : this.monotonicNow();
        const client = this.clients.get(clientId);
        if (!client) return;
        if (client.authenticated || !client.authChallenge) {
            this.sendError(clientId, 'Authentication challenge already consumed',
                'auth_challenge_already_consumed');
            if (client.ws && client.ws.readyState === WebSocket.OPEN) {
                client.ws.close(1008, 'Authentication challenge already consumed');
            }
            return;
        }
        // A challenge is a one-shot capability. Consume it before doing any
        // crypto so a failed or duplicated response cannot be retried while
        // the WebSocket close handshake is still in progress.
        const challenge = client.authChallenge;
        client.authChallenge = null;
        const verified = verifyAuthResponse(challenge, message, epochNow);
        if (!verified.ok) {
            this.sendError(clientId, verified.error, verified.error);
            if (client.ws && client.ws.readyState === WebSocket.OPEN) {
                client.ws.close(1008, 'Identity verification failed');
            }
            return;
        }

        for (const existing of this.clients.values()) {
            if (existing === client || !existing.authenticated
                || existing.endpointId !== verified.endpointId) continue;
            if (existing.runtimeId !== verified.runtimeId) {
                if (!this.clientLeaseExpired(existing, monotonicNow)) {
                    this.sendError(clientId, 'This endpoint is already online', 'endpoint_already_connected');
                    client.ws.close(1008, 'Endpoint already connected');
                    return;
                }
                this.expireRemoteSessionsForClient(existing, monotonicNow);
                this.retireClientTransport(
                    existing, false, 1001, 'Superseded after heartbeat lease expiry');
                continue;
            }
            const protectedByLease = this.handleRemoteSessionDeparture(
                existing, monotonicNow);
            this.retireClientTransport(
                existing, protectedByLease, 1000, 'Connection rebound');
        }

        const generation = (this.connectionGenerationByEndpoint.get(verified.endpointId) || 0) + 1;
        this.connectionGenerationByEndpoint.set(verified.endpointId, generation);
        client.authenticated = true;
        client.installationId = verified.installationId;
        client.endpointId = verified.endpointId;
        client.instanceId = verified.instanceId;
        client.runtimeId = verified.runtimeId;
        client.connectionGeneration = generation;
        if (client.authTimer) clearTimeout(client.authTimer);
        client.authTimer = null;
        client.lastHeartbeatAt = epochNow;
        client.lastHeartbeatMonotonicAt = monotonicNow;

        client.ws.send(JSON.stringify({
            type: 'welcome',
            protocolVersion: this.protocolVersion,
            serverBootId: this.serverBootId,
            messageId: uuidv4(),
            connectionId: client.id,
            installationId: client.installationId,
            endpointId: client.endpointId,
            instanceId: client.instanceId,
            runtimeId: client.runtimeId,
            connectionGeneration: generation,
            policy: {
                policyVersion: this.config.policyVersion,
                heartbeatIntervalMs: this.config.heartbeatIntervalMs,
                leaseTimeoutMs: this.config.leaseTimeoutMs,
                scenePrepareTimeoutMs: this.config.scenePrepareTimeoutMs,
                sceneActivationLeadMs: this.config.sceneActivationLeadMs,
                sceneMaxClockSkewMs: this.config.sceneMaxClockSkewMs,
                sceneStartedAckTimeoutMs: this.sceneRuns.startedAckTimeoutMs,
                sceneMaxStartSkewMs: this.sceneRuns.maximumStartSkewMs,
                uploadIdleTimeoutMs: this.config.uploadIdleTimeoutMs,
                uploadTargetAckTimeoutMs: this.config.uploadTargetAckTimeoutMs,
                removalAckTimeoutMs: this.config.removalAckTimeoutMs,
            },
            serverMonotonicMs: Number(process.hrtime.bigint() / 1_000_000n),
        }));
    }

    handleHeartbeat(clientId, message, timing = null) {
        const epochNow = typeof timing === 'number' ? timing
            : timing && Number.isFinite(timing.epochMs)
                ? timing.epochMs : this.epochNow();
        const monotonicNow = typeof timing === 'number' ? timing
            : timing && Number.isFinite(timing.monotonicMs)
                ? timing.monotonicMs : this.monotonicNow();
        const sessionNow = typeof timing === 'number' ? timing
            : timing && Number.isFinite(timing.monotonicMs)
                ? timing.monotonicMs : this.remoteSessions.now();
        const client = this.clients.get(clientId);
        if (!client || message.connectionGeneration !== client.connectionGeneration) {
            return this.sendError(clientId, 'Stale connection generation', 'stale_connection_generation');
        }
        if (this.clientLeaseExpired(client, monotonicNow)) {
            this.sendError(clientId, 'Heartbeat lease expired', 'lease_expired');
            this.expireRemoteSessionsForClient(client, monotonicNow);
            this.retireClientTransport(
                client, false, 1001, 'Heartbeat lease expired');
            this.broadcastClientList();
            return;
        }
        client.lastHeartbeatAt = epochNow;
        client.lastHeartbeatMonotonicAt = monotonicNow;
        for (const session of this.remoteSessions.sessionsForEndpoint(client.endpointId)) {
            const contact = this.remoteSessions.touch(
                session.remoteSessionId, client.endpointId,
                client.connectionGeneration, sessionNow);
            if (!contact.ok && contact.terminalTransition && contact.session) {
                this.beginRemoteSessionTeardown(contact.session);
            }
            if (contact.ok && contact.healthChanged) {
                const payload = this.remoteSessionPayload(session,
                    'remote_session_lease_state');
                payload.state = session.phase === 'Grace' ? 'Grace' : 'Active';
                payload.degradedEndpointId = client.endpointId;
                payload.degraded = false;
                this.sendToEndpoint(session.ownerEndpointId, payload);
                this.sendToEndpoint(session.targetEndpointId, payload);
            }
        }
        client.ws.send(JSON.stringify({
            type: 'heartbeat_ack',
            protocolVersion: this.protocolVersion,
            serverBootId: this.serverBootId,
            messageId: uuidv4(),
            connectionGeneration: client.connectionGeneration,
            sequence: message.sequence,
            clientMonotonicMs: message.clientMonotonicMs,
            serverMonotonicMs: Number(process.hrtime.bigint() / 1_000_000n),
            serverEpochMs: epochNow,
        }));
    }

    handleRemoteSessionOpen(ownerId, message) {
        const owner = this.clients.get(ownerId);
        const targetId = this.resolveClientId(message.targetEndpointId);
        const target = targetId ? this.clients.get(targetId) : null;
        if (!owner || message.connectionGeneration !== owner.connectionGeneration) {
            return this.sendRemoteSessionError(ownerId,
                'Stale connection generation', 'stale_connection_generation', message);
        }
        if (!owner || !target || !target.authenticated || !target.machineName) {
            return this.sendRemoteSessionError(ownerId,
                'Target client is offline', 'target_offline', message);
        }
        // Discovery is not an availability lease. A target whose last
        // authenticated contact reached the strict boundary must be retired
        // before an open can create fresh session contact timestamps for it.
        const commandNow = this.monotonicNow();
        if (this.clientLeaseExpired(target, commandNow)) {
            this.expireRemoteSessionsForClient(target, commandNow);
            this.retireClientTransport(
                target, false, 1001, 'Heartbeat lease expired');
            this.broadcastClientList();
            return this.sendRemoteSessionError(ownerId,
                'Target client is offline', 'target_offline', message,
                message.targetEndpointId);
        }

        // A previous controller may have reached its own strict lease while B
        // is still healthy. Terminalize that incoming session synchronously;
        // B remains unavailable until its cleanup commit removes the registry
        // reservation, so no open can race the 100 ms background sweep.
        const incoming = this.remoteSessions.activeIncomingFor(target.endpointId);
        if (incoming && (incoming.phase === 'Active' || incoming.phase === 'Grace')) {
            const lease = this.remoteSessions.validateLease(
                incoming.remoteSessionId, commandNow);
            if (!lease.ok && lease.terminalTransition && lease.session) {
                this.metrics.incrementOnce('remote_session_lease_expired_total',
                    lease.session.remoteSessionId);
                this.beginRemoteSessionTeardown(lease.session);
            }
        }
        const opened = this.remoteSessions.open({
            ownerEndpointId: owner.endpointId,
            targetEndpointId: target.endpointId,
            ownerRuntimeId: owner.runtimeId,
            targetRuntimeId: target.runtimeId,
            ownerConnectionGeneration: owner.connectionGeneration,
            targetConnectionGeneration: target.connectionGeneration,
        });
        if (!opened.ok) {
            return this.sendRemoteSessionError(
                ownerId, opened.error, opened.error, message, target.endpointId);
        }

        const payload = this.remoteSessionPayload(opened.session, 'remote_session_opened');
        payload.requestId = this.isValidOpaqueId(message.requestId)
            ? message.requestId : undefined;
        payload.resumeToken = opened.session.resumeToken;
        this.sendToEndpoint(owner.endpointId, payload);
        this.sendToEndpoint(target.endpointId, payload);
        this.broadcastClientList();
    }

    handleRemoteSessionResume(clientId, message) {
        const client = this.clients.get(clientId);
        if (!client) return;
        const resumed = this.remoteSessions.resume({
            remoteSessionId: message.remoteSessionId,
            endpointId: client.endpointId,
            runtimeId: client.runtimeId,
            resumeToken: message.resumeToken,
            generation: message.generation,
            connectionGeneration: client.connectionGeneration,
        });
        if (!resumed.ok) {
            if (resumed.terminalTransition && resumed.session) {
                // The lease can become terminal while a same-process RESUME is
                // being handled on its replacement transport. Bind the
                // terminal result to that transport before first dispatch.
                this.bindTerminalDelivery(resumed.session, client);
                this.beginRemoteSessionTeardown(resumed.session);
            }
            if (resumed.error === 'session_not_resumable') {
                const terminal = this.remoteSessions.get(message.remoteSessionId)
                    || this.remoteSessions.getTombstone(message.remoteSessionId);
                const expectedGeneration = this.remoteSessions.knownGenerationFor(
                    terminal, client.endpointId);
                if (expectedGeneration !== null
                    && message.generation !== expectedGeneration) {
                    return this.sendRemoteSessionError(clientId,
                        'Stale remote session generation',
                        'stale_remote_session_generation', message);
                }
                if (this.replayTerminalStateForClient(
                        client, message.remoteSessionId) > 0) return;
            }
            return this.sendRemoteSessionError(
                clientId, resumed.error, resumed.error, message);
        }
        this.metrics.increment('remote_session_resumed_total', 1,
            resumed.session.remoteSessionId);
        this.rebindSessionGeneration(resumed.session);
        const payload = this.remoteSessionPayload(resumed.session, 'remote_session_resumed');
        const liveRun = this.sceneRuns.getForSession(resumed.session.remoteSessionId);
        payload.requestStateSnapshot = !!liveRun && liveRun.phase === SCENE_PHASES.LIVE;
        for (const endpointId of [resumed.session.ownerEndpointId,
                                resumed.session.targetEndpointId]) {
            if (this.sendToEndpoint(endpointId, payload)) {
                this.remoteSessions.markGenerationDelivered(
                    resumed.session.remoteSessionId, endpointId,
                    resumed.session.generation);
            }
        }
        this.dispatchReadyAssetRemovalsForSession(resumed.session.remoteSessionId);
        this.broadcastClientList();
    }

    handleRemoteSessionClose(clientId, message) {
        const client = this.clients.get(clientId);
        const session = this.remoteSessions.get(message.remoteSessionId);
        const tombstone = !session
            ? this.remoteSessions.getTombstone(message.remoteSessionId) : null;
        const tombstoneParty = client && tombstone
            && (tombstone.ownerEndpointId === client.endpointId
                || tombstone.targetEndpointId === client.endpointId);
        if (tombstoneParty) {
            if (message.connectionGeneration !== client.connectionGeneration) {
                return this.sendRemoteSessionError(clientId,
                    'Stale connection generation', 'stale_connection_generation', message);
            }
            if (message.generation !== tombstone.generation) {
                return this.sendRemoteSessionError(clientId,
                    'Stale remote session generation',
                    'stale_remote_session_generation', message);
            }
            if (!this.terminalRuntimeMatches(tombstone, client)) {
                return this.sendRemoteSessionError(clientId,
                    'Terminal session belongs to another process',
                    'invalid_resume_proof', message);
            }
            this.bindTerminalDelivery(tombstone, client);
            this.sendRemoteSessionStateToEndpoint(
                tombstone, 'remote_session_terminating', client.endpointId, {
                    phase: 'CleanupPending',
                    replay: true,
                });
            this.sendRemoteSessionStateToEndpoint(
                tombstone, 'remote_session_closed', client.endpointId, {
                    cleanupState: 'confirmed',
                    replay: true,
                    requestId: this.isValidOpaqueId(message.requestId)
                        ? message.requestId : undefined,
                });
            return;
        }
        const isParty = client && session
            && (session.ownerEndpointId === client.endpointId
                || session.targetEndpointId === client.endpointId);
        if (!isParty) {
            return this.sendRemoteSessionError(clientId,
                'Only an authenticated session party may close it',
                'not_a_session_party', message);
        }
        if (message.connectionGeneration !== client.connectionGeneration) {
            return this.sendRemoteSessionError(clientId,
                'Stale connection generation', 'stale_connection_generation', message);
        }
        if (message.generation !== session.generation) {
            return this.sendRemoteSessionError(clientId,
                'Stale remote session generation',
                'stale_remote_session_generation', message);
        }
        if ((session.phase === 'Terminating' || session.phase === 'CleanupPending')
            && !this.terminalRuntimeMatches(session, client)) {
            return this.sendRemoteSessionError(clientId,
                'Terminal session belongs to another process',
                'invalid_resume_proof', message);
        }
        const roleConnectionGeneration = client.endpointId === session.ownerEndpointId
            ? session.ownerConnectionGeneration : session.targetConnectionGeneration;
        if (session.phase !== 'Terminating' && session.phase !== 'CleanupPending'
            && roleConnectionGeneration !== client.connectionGeneration) {
            return this.sendRemoteSessionError(clientId,
                'Stale session transport generation',
                'stale_connection_generation', message);
        }
        if (session.phase !== 'Terminating' && session.phase !== 'CleanupPending') {
            const lease = this.remoteSessions.validateLease(session.remoteSessionId);
            if (!lease.ok) {
                if (lease.terminalTransition && lease.session) {
                    this.metrics.incrementOnce('remote_session_lease_expired_total',
                        lease.session.remoteSessionId);
                    this.beginRemoteSessionTeardown(lease.session);
                }
                return this.sendRemoteSessionError(
                    clientId, lease.error, lease.error, message);
            }
        }
        const teardownReason = message.reason === 'clean_shutdown'
            ? 'clean_shutdown'
            : (client.endpointId === session.ownerEndpointId
                ? 'explicit_disconnect' : 'peer_close');
        const terminating = this.remoteSessions.terminate(
            session.remoteSessionId, teardownReason);
        if (!terminating.ok) {
            return this.sendRemoteSessionError(
                clientId, terminating.error, terminating.error, message);
        }
        if (!terminating.replay) {
            this.beginRemoteSessionTeardown(terminating.session,
                this.isValidOpaqueId(message.requestId) ? message.requestId : undefined);
        } else {
            this.bindTerminalDelivery(terminating.session, client);
            for (const endpointId of [terminating.session.ownerEndpointId,
                                    terminating.session.targetEndpointId]) {
                this.sendRemoteSessionStateToEndpoint(
                    terminating.session, 'remote_session_terminating', endpointId, {
                        replay: true,
                        requestId: this.isValidOpaqueId(message.requestId)
                            ? message.requestId : undefined,
                    });
            }
        }
    }

    handleRemoteSessionTeardownAck(clientId, message) {
        const client = this.clients.get(clientId);
        if (!client) return;
        const pendingSession = this.remoteSessions.get(message.remoteSessionId);
        const correlatedSession = pendingSession
            || this.remoteSessions.getTombstone(message.remoteSessionId);
        const expectedTargetConnectionGeneration = correlatedSession
            && Number.isSafeInteger(
                correlatedSession.targetTerminalConnectionGeneration)
            ? correlatedSession.targetTerminalConnectionGeneration
            : correlatedSession && correlatedSession.targetConnectionGeneration;
        const expectedTargetRuntimeId = correlatedSession
            && (correlatedSession.targetTerminalRuntimeId
                || correlatedSession.targetRuntimeId);
        if (correlatedSession && (message.generation !== correlatedSession.generation
            || message.connectionGeneration !== client.connectionGeneration
            || correlatedSession.targetEndpointId !== client.endpointId
            || expectedTargetConnectionGeneration !== client.connectionGeneration
            || expectedTargetRuntimeId !== client.runtimeId)) {
            return this.sendRemoteSessionError(clientId,
                'Stale cleanup acknowledgement',
                'stale_remote_session_generation', message);
        }
        const acknowledged = this.remoteSessions.acknowledgeCleanup(
            message.remoteSessionId,
            message.teardownId,
            client.endpointId,
            {
                result: message.result,
                sceneStopped: message.sceneStopped === true,
                uploadsAborted: message.uploadsAborted === true,
                cacheQuarantined: message.cacheQuarantined === true,
                removedFileCount: Number.isSafeInteger(message.removedFileCount)
                    ? Math.max(0, message.removedFileCount) : 0,
                errorCode: typeof message.errorCode === 'string'
                    ? message.errorCode.slice(0, 128) : undefined,
                quarantinedBytes: Number.isSafeInteger(message.quarantinedBytes)
                    ? Math.max(0, message.quarantinedBytes) : 0,
            });
        if (!acknowledged.ok) {
            if (acknowledged.error === 'cleanup_not_committed') {
                this.metrics.incrementOnce('remote_session_cleanup_error_total',
                    `${message.remoteSessionId}:${message.teardownId}`);
                this.updateCleanupPendingMetric(message.remoteSessionId);
            }
            return this.sendRemoteSessionError(
                clientId, acknowledged.error, acknowledged.error, message);
        }
        if (!acknowledged.replay && acknowledged.session.cleanupResult.quarantinedBytes > 0) {
            this.metrics.increment('remote_cache_quarantined_bytes_total',
                acknowledged.session.cleanupResult.quarantinedBytes,
                acknowledged.session.teardownId);
        }
        this.purgeServerSessionState(acknowledged.session);
        this.sendRemoteSessionClosedToParties(acknowledged.session);
        this.broadcastClientList();
        this.updateCleanupPendingMetric(acknowledged.session.remoteSessionId);
    }

    bindTerminalDelivery(session, client, options = {}) {
        if (!session || !client || !client.authenticated
            || !Number.isSafeInteger(client.connectionGeneration)
            || client.connectionGeneration < 1) return null;
        if (session.ownerEndpointId === client.endpointId) {
            const expectedRuntime = session.ownerTerminalRuntimeId
                || session.ownerRuntimeId;
            if (expectedRuntime !== client.runtimeId) return null;
            session.ownerTerminalConnectionGeneration = client.connectionGeneration;
            session.ownerTerminalRuntimeId = client.runtimeId;
            return 'owner';
        }
        if (session.targetEndpointId === client.endpointId) {
            const expectedRuntime = session.targetTerminalRuntimeId
                || session.targetRuntimeId;
            if (expectedRuntime !== client.runtimeId
                && options.allowTargetRuntimeRestart !== true) return null;
            session.targetTerminalConnectionGeneration = client.connectionGeneration;
            session.targetTerminalRuntimeId = client.runtimeId;
            return 'target';
        }
        return null;
    }

    terminalRuntimeMatches(session, client) {
        if (!session || !client) return false;
        if (session.ownerEndpointId === client.endpointId) {
            const expected = session.ownerTerminalRuntimeId || session.ownerRuntimeId;
            return expected === client.runtimeId;
        }
        if (session.targetEndpointId === client.endpointId) {
            const expected = session.targetTerminalRuntimeId || session.targetRuntimeId;
            return expected === client.runtimeId;
        }
        return false;
    }

    remoteSessionPayload(session, type, recipientEndpointId = null) {
        let ownerConnectionGeneration = session.ownerConnectionGeneration;
        let targetConnectionGeneration = session.targetConnectionGeneration;
        // A terminal cleanup may be delivered on a replacement transport, but
        // the active-session tuple is immutable. Tailor only the recipient's
        // local transport generation; the peer generation remains the value
        // that recipient last accepted while the session was command-capable.
        if (recipientEndpointId === session.ownerEndpointId
            && Number.isSafeInteger(session.ownerTerminalConnectionGeneration)) {
            ownerConnectionGeneration = session.ownerTerminalConnectionGeneration;
        }
        if (recipientEndpointId === session.targetEndpointId
            && Number.isSafeInteger(session.targetTerminalConnectionGeneration)) {
            targetConnectionGeneration = session.targetTerminalConnectionGeneration;
        }
        return {
            type,
            protocolVersion: this.protocolVersion,
            serverBootId: this.serverBootId,
            messageId: uuidv4(),
            remoteSessionId: session.remoteSessionId,
            generation: session.generation,
            ownerConnectionGeneration,
            targetConnectionGeneration,
            phase: session.phase,
            ownerEndpointId: session.ownerEndpointId,
            targetEndpointId: session.targetEndpointId,
            teardownId: session.teardownId || undefined,
            reason: session.teardownReason || undefined,
            sceneRunId: session.sceneRunId || undefined,
        };
    }

    sendRemoteSessionStateToEndpoint(session, type, endpointId, extra = {}) {
        const resolvedId = this.resolveClientId(endpointId);
        const client = resolvedId ? this.clients.get(resolvedId) : null;
        // Terminal state belongs to the process that held the session, not
        // merely to any later process presenting the same installation key.
        // A restarted target is rebound explicitly by the startup-cleanup
        // replay path before reaching this helper.
        if (!client || !this.terminalRuntimeMatches(session, client)) return false;
        this.bindTerminalDelivery(session, client);
        return this.sendToEndpoint(endpointId, {
            ...this.remoteSessionPayload(session, type, endpointId),
            ...extra,
        });
    }

    sendRemoteSessionClosedToParties(session, extra = {}) {
        for (const endpointId of [session.ownerEndpointId, session.targetEndpointId]) {
            this.sendRemoteSessionStateToEndpoint(
                session, 'remote_session_closed', endpointId, {
                    cleanupState: 'confirmed',
                    ...extra,
                });
        }
    }

    replayTerminalStateForClient(client, remoteSessionId = null) {
        if (!client || !client.authenticated || !client.endpointId) return 0;
        let replayed = 0;
        for (const session of this.remoteSessions.sessionsForEndpoint(client.endpointId)) {
            if (remoteSessionId !== null
                && session.remoteSessionId !== remoteSessionId) continue;
            if (session.phase !== 'Terminating' && session.phase !== 'CleanupPending') continue;
            const isOwner = session.ownerEndpointId === client.endpointId;
            // A new owner process has no resumable in-memory session. A target
            // process is still allowed to finish startup cleanup for the old
            // sender/session cache, even when its runtimeId changed.
            if (isOwner && session.ownerRuntimeId !== client.runtimeId) continue;
            if (!this.bindTerminalDelivery(session, client, {
                allowTargetRuntimeRestart: !isOwner,
            })) continue;
            if (this.sendRemoteSessionStateToEndpoint(
                    session, 'remote_session_terminating', client.endpointId,
                    { replay: true })) ++replayed;
        }

        for (const tombstone of this.remoteSessions.tombstones.values()) {
            if (remoteSessionId !== null
                && tombstone.remoteSessionId !== remoteSessionId) continue;
            // Closed is useful only to the same process that may still retain
            // the binding. When startup cleanup was rebound to a replacement
            // target process, that terminal runtime becomes authoritative for
            // subsequent tombstone replay.
            if (!this.terminalRuntimeMatches(tombstone, client)) continue;
            if (!this.bindTerminalDelivery(tombstone, client)) continue;
            const terminalDelivered = this.sendRemoteSessionStateToEndpoint(
                tombstone, 'remote_session_terminating', client.endpointId, {
                    phase: 'CleanupPending',
                    replay: true,
                });
            const closedDelivered = this.sendRemoteSessionStateToEndpoint(
                tombstone, 'remote_session_closed', client.endpointId, {
                    cleanupState: 'confirmed',
                    replay: true,
                });
            if (terminalDelivered || closedDelivered) ++replayed;
        }
        return replayed;
    }

    sendRemoteSessionError(clientId, errorMessage, code, message = {}, targetEndpointId) {
        const correlation = { scope: 'remote_session' };
        const copyOpaque = (field, value) => {
            if (typeof value === 'string' && value.length <= 128
                && this.isValidOpaqueId(value)) {
                correlation[field] = value;
            }
        };
        copyOpaque('requestId', message.requestId);
        copyOpaque('remoteSessionId', message.remoteSessionId);
        copyOpaque('targetEndpointId', targetEndpointId || message.targetEndpointId);
        // Never reflect resumeToken or any other proof material.
        return this.sendError(clientId, errorMessage, code, correlation);
    }

    sendToEndpoint(endpointId, payload) {
        const resolvedId = this.resolveClientId(endpointId);
        const client = resolvedId ? this.clients.get(resolvedId) : null;
        if (!client || !client.authenticated || !client.ws
            || client.ws.readyState !== WebSocket.OPEN) return false;
        try {
            const envelope = {
                ...payload,
                protocolVersion: this.protocolVersion,
                serverBootId: this.serverBootId,
                messageId: CANONICAL_UUID_PATTERN.test(payload.messageId || '')
                    ? payload.messageId : uuidv4(),
            };
            if (!Object.hasOwn(envelope, 'connectionGeneration')) {
                envelope.connectionGeneration = client.connectionGeneration;
            }
            client.ws.send(JSON.stringify(envelope));
            return true;
        } catch (error) {
            console.error('❌ Device relay failed:', error);
            return false;
        }
    }

    beginRemoteSessionTeardown(session, requestId) {
        if (!session || !session.teardownId || session.teardownDispatchStarted) return false;
        session.teardownDispatchStarted = true;
        const run = this.sceneRuns.getForSession(session.remoteSessionId);
        if (run) this.initiateSceneStop(run, session.teardownReason || 'session_terminating', true);
        this.abortAssetRemovalsForRemoteSession(
            session, session.teardownReason || 'session_terminating');
        this.abortUploadsForRemoteSession(session, session.teardownReason || 'session_terminating');
        for (const endpointId of [session.ownerEndpointId, session.targetEndpointId]) {
            this.sendRemoteSessionStateToEndpoint(
                session, 'remote_session_terminating', endpointId, {
                    requestId: this.isValidOpaqueId(requestId) ? requestId : undefined,
                });
        }
        this.remoteSessions.markCleanupPending(session.remoteSessionId, session.teardownId);
        this.updateCleanupPendingMetric(session.remoteSessionId);
        this.broadcastClientList();
        return true;
    }

    updateCleanupPendingMetric(correlationId = '') {
        let count = 0;
        for (const session of this.remoteSessions.sessions.values()) {
            if (session.phase === 'Terminating' || session.phase === 'CleanupPending') ++count;
        }
        this.metrics.setGauge('remote_session_cleanup_pending', count, correlationId);
    }

    sweepRemoteSessionLeases(now = this.remoteSessions.now()) {
        const degradedAfterMs = Math.min(this.config.leaseTimeoutMs - 1,
            this.config.heartbeatIntervalMs * 2);
        for (const transition of this.remoteSessions.markDegraded(now, degradedAfterMs)) {
            const payload = this.remoteSessionPayload(
                transition.session, 'remote_session_lease_state');
            payload.state = transition.degraded ? 'Degraded'
                : (transition.session.phase === 'Grace' ? 'Grace' : 'Active');
            payload.degradedEndpointId = transition.endpointId;
            payload.degraded = transition.degraded;
            this.sendToEndpoint(transition.session.ownerEndpointId, payload);
            this.sendToEndpoint(transition.session.targetEndpointId, payload);
        }
        for (const session of this.remoteSessions.tick(now)) {
            if (session.teardownReason === 'lease_expired') {
                this.metrics.incrementOnce('remote_session_lease_expired_total',
                    session.remoteSessionId);
            }
            this.beginRemoteSessionTeardown(session);
        }
        this.sweepExpiredClientTransports(now);
        this.sweepSceneRuns(this.epochNow());
    }

    handleRemoteSessionDeparture(client, now = this.remoteSessions.now()) {
        if (!client || !client.endpointId) return false;
        const changed = this.remoteSessions.markDisconnected(client.endpointId, now);
        for (const session of changed) {
            if (session.phase === 'Terminating') {
                this.beginRemoteSessionTeardown(session);
                continue;
            }
            const payload = this.remoteSessionPayload(session, 'remote_session_lease_state');
            payload.state = 'Grace';
            payload.deadlineEpochMs = session.graceDeadlineEpochMs;
            const peer = session.ownerEndpointId === client.endpointId
                ? session.targetEndpointId : session.ownerEndpointId;
            this.sendToEndpoint(peer, payload);
        }
        if (changed.length > 0) this.broadcastClientList();
        return changed.length > 0;
    }

    rebindSessionGeneration(session) {
        const assets = this.sessionAssets.get(session.remoteSessionId);
        if (assets) {
            for (const asset of assets.values()) asset.generation = session.generation;
        }
        const ownerId = this.resolveClientId(session.ownerEndpointId);
        const owner = ownerId ? this.clients.get(ownerId) : null;
        const targetId = this.resolveClientId(session.targetEndpointId);
        const target = targetId ? this.clients.get(targetId) : null;
        for (const upload of this.uploads.values()) {
            if (upload.remoteSessionId === session.remoteSessionId) {
                upload.generation = session.generation;
                if (owner && owner.runtimeId === upload.ownerRuntimeId) {
                    upload.ownerConnectionGeneration = owner.connectionGeneration;
                }
            }
        }
        for (const result of this.uploadTombstones.values()) {
            if (result.remoteSessionId !== session.remoteSessionId) continue;
            result.generation = session.generation;
            if (result.payload) result.payload.generation = session.generation;
            if (owner && owner.runtimeId === result.ownerRuntimeId) {
                result.ownerConnectionGeneration = owner.connectionGeneration;
                if (result.payload) {
                    result.payload.connectionGeneration = owner.connectionGeneration;
                }
            }
        }
        for (const removal of this.pendingAssetRemovals.values()) {
            if (removal.remoteSessionId !== session.remoteSessionId) continue;
            removal.generation = session.generation;
            if (owner) removal.ownerConnectionGeneration = owner.connectionGeneration;
            if (target) removal.targetConnectionGeneration = target.connectionGeneration;
        }
        for (const removal of this.assetRemovalTombstones.values()) {
            if (removal.remoteSessionId !== session.remoteSessionId) continue;
            removal.generation = session.generation;
            if (owner) removal.ownerConnectionGeneration = owner.connectionGeneration;
            if (target) removal.targetConnectionGeneration = target.connectionGeneration;
        }
        const run = this.sceneRuns.getForSession(session.remoteSessionId);
        if (run) run.generation = session.generation;
    }

    abortUploadsForRemoteSession(session, reason) {
        for (const [uploadId, upload] of Array.from(this.uploads.entries())) {
            if (!upload || upload.remoteSessionId !== session.remoteSessionId) continue;
            const payload = {
                type: 'upload_abort',
                protocolVersion: this.protocolVersion,
                serverBootId: this.serverBootId,
                messageId: uuidv4(),
                remoteSessionId: session.remoteSessionId,
                generation: session.generation,
                connectionGeneration: upload.ownerConnectionGeneration,
                uploadId,
                ownerEndpointId: upload.ownerEndpointId,
                targetEndpointId: upload.targetEndpointId,
                reason: String(reason || 'remote_session_terminating').slice(0, 128),
            };
            this.sendToEndpoint(session.ownerEndpointId, payload);
            this.sendToEndpoint(session.targetEndpointId, payload);
            this.uploads.delete(uploadId);
            session.activeUploadIds.delete(uploadId);
        }
    }

    purgeServerSessionState(session) {
        const run = this.sceneRuns.getForSession(session.remoteSessionId);
        if (run) this.sceneRuns.forceFinalize(run.sceneRunId, true);
        this.abortUploadsForRemoteSession(session, 'remote_session_closed');
        this.sessionAssets.delete(session.remoteSessionId);
        for (const [uploadId, result] of this.uploadTombstones) {
            if (result.remoteSessionId === session.remoteSessionId) {
                this.uploadTombstones.delete(uploadId);
            }
        }
        for (const [removalId, removal] of this.pendingAssetRemovals) {
            if (removal.remoteSessionId === session.remoteSessionId) {
                this.pendingAssetRemovals.delete(removalId);
            }
        }
        for (const [removalId, removal] of this.assetRemovalTombstones) {
            if (removal.remoteSessionId === session.remoteSessionId) {
                this.assetRemovalTombstones.delete(removalId);
            }
        }
    }

    resolveClientId(targetEndpointId) {
        if (typeof targetEndpointId !== 'string' || !targetEndpointId) return null;
        for (const [transportKey, clientInfo] of this.clients.entries()) {
            if (clientInfo.authenticated && clientInfo.endpointId === targetEndpointId) {
                return transportKey;
            }
        }
        return null;
    }

    getEndpointId(clientId) {
        const client = this.clients.get(clientId);
        return client && client.endpointId ? client.endpointId : clientId;
    }

    registerConnectionForEndpoint(endpointId, connectionId) {
        if (!endpointId || !connectionId) return;
        let sessions = this.connectionsByEndpoint.get(endpointId);
        if (!sessions) {
            sessions = new Set();
            this.connectionsByEndpoint.set(endpointId, sessions);
        }
        sessions.add(connectionId);
    }

    unregisterConnectionForEndpoint(endpointId, connectionId) {
        if (!endpointId || !connectionId) return;
        const sessions = this.connectionsByEndpoint.get(endpointId);
        if (!sessions) return;
        sessions.delete(connectionId);
        if (sessions.size === 0) {
            this.connectionsByEndpoint.delete(endpointId);
        }
    }

    handleEndpointSnapshot(clientId, message) {
        const client = this.clients.get(clientId);
        if (!client || !client.authenticated || !client.endpointId || !client.runtimeId) return;

        // endpoint_snapshot is an authoritative replacement, never a partial
        // patch. Validate the complete snapshot before mutating advertised state.
        const invalidSnapshot = (() => {
            if (typeof message.machineName !== 'string'
                || message.machineName.trim().length === 0
                || message.machineName.length > 255) {
                return 'machineName must be a non-empty string of at most 255 characters';
            }
            if (typeof message.platform !== 'string'
                || message.platform.trim().length === 0
                || message.platform.length > 128) {
                return 'platform must be a non-empty string of at most 128 characters';
            }
            if (!Number.isSafeInteger(message.instanceOrdinal)
                || message.instanceOrdinal < 1
                || (client.instanceId === 'primary' && message.instanceOrdinal !== 1)
                || (client.instanceId !== 'primary' && message.instanceOrdinal < 2)) {
                return 'instanceOrdinal is inconsistent with the authenticated instance';
            }
            if (!Array.isArray(message.screens)
                || message.screens.length > this.MAX_REMOTE_SCENE_SCREENS
                || message.screens.some(screen => !isPlainObject(screen))) {
                return `screens must be an array of at most ${this.MAX_REMOTE_SCENE_SCREENS} objects`;
            }
            if (message.volumePercent !== null
                && (typeof message.volumePercent !== 'number'
                    || !Number.isFinite(message.volumePercent)
                    || message.volumePercent < 0
                    || message.volumePercent > 100)) {
                return 'volumePercent must be a finite number from 0 to 100 or null';
            }
            if (Object.prototype.hasOwnProperty.call(message, 'systemUI')
                && !Array.isArray(message.systemUI)) {
                return 'systemUI must be an array when present';
            }
            return null;
        })();
        if (invalidSnapshot) {
            const correlation = this.isValidOpaqueId(message.requestId)
                ? { requestId: message.requestId }
                : null;
            this.sendError(clientId, `Invalid endpoint snapshot: ${invalidSnapshot}`,
                'invalid_endpoint_snapshot', correlation);
            return;
        }

        client.socketLabel = client.socketLabel || clientId;
        // The map key is the immutable transport connection id. runtimeId is
        // scoped by the authenticated device and must never become a global
        // key: two different installations may legitimately choose the same
        // random runtime value without being able to evict one another.
        this.registerConnectionForEndpoint(client.endpointId, client.id);

        client.machineName = message.machineName;
        client.platform = message.platform;
        client.instanceOrdinal = message.instanceOrdinal;
        client.screens = message.screens;
        client.systemUI = Array.isArray(message.systemUI) ? message.systemUI : [];
        client.volumePercent = message.volumePercent;

        this.logProtocolEvent('endpoint_snapshot_applied', {
            connectionId: client.id,
            endpointId: client.endpointId,
            messageId: message.messageId,
            screenCount: client.screens.length,
        });

        const applied = {
            type: 'endpoint_snapshot_applied',
            protocolVersion: this.protocolVersion,
            serverBootId: this.serverBootId,
            messageId: uuidv4(),
            connectionGeneration: client.connectionGeneration,
            snapshot: {
                installationId: client.installationId,
                endpointId: client.endpointId,
                instanceId: client.instanceId,
                instanceOrdinal: client.instanceOrdinal,
                runtimeId: client.runtimeId,
                machineName: client.machineName,
                screens: client.screens,
                platform: client.platform,
                systemUI: client.systemUI || [],
                volumePercent: client.volumePercent
            }
        };
        if (this.isValidOpaqueId(message.requestId)) applied.requestId = message.requestId;
        client.ws.send(JSON.stringify(applied));

        // Broadcast updated client list
        this.broadcastClientList();
        // A terminal result may have been emitted while either party's control
        // transport was absent. Replay it after authoritative registration.
        // The target may use a new runtime solely to finish startup cache
        // cleanup; an owner receives catch-up only in its original process.
        this.replayTerminalStateForClient(client);
    }

    sendClientList(clientId) {
        const client = this.clients.get(clientId);
        if (!client) return;

        const clientList = Array.from(this.clients.values())
            .filter(c => c.id !== clientId && c.machineName && c.endpointId) // Don't include self and only registered clients with stable identity
            .map(c => {
                const incoming = this.remoteSessions.activeIncomingFor(c.endpointId);
                let remoteSessionState = 'Available';
                if (incoming) {
                    if (incoming.ownerEndpointId === client.endpointId) {
                        if (incoming.phase === 'Terminating'
                            || incoming.phase === 'CleanupPending') {
                            remoteSessionState = 'Disconnecting';
                        } else {
                            remoteSessionState = incoming.phase === 'Grace'
                                ? 'Reconnecting' : 'Connected';
                        }
                    } else {
                        remoteSessionState = incoming.phase === 'CleanupPending'
                            || incoming.phase === 'Terminating' ? 'Unavailable' : 'In use';
                    }
                }
                return {
                    installationId: c.installationId,
                    endpointId: c.endpointId,
                    instanceId: c.instanceId,
                    instanceOrdinal: c.instanceOrdinal,
                    runtimeId: c.runtimeId,
                    machineName: c.machineName,
                    screens: c.screens,
                    platform: c.platform,
                    systemUI: c.systemUI || [],
                    volumePercent: c.volumePercent,
                    status: c.status,
                    lastSeenAt: c.lastHeartbeatAt || null,
                    remoteSessionState,
                };
            });

        client.ws.send(JSON.stringify({
            type: 'client_list',
            protocolVersion: this.protocolVersion,
            serverBootId: this.serverBootId,
            messageId: uuidv4(),
            connectionGeneration: client.connectionGeneration,
            clients: clientList
        }));
    }

    broadcastClientList() {
        for (const [clientId, client] of this.clients) {
            if (client.machineName) { // Only send to registered clients
                this.sendClientList(clientId);
            }
        }
    }

    // Protocol v3 upload state. A transfer is immutable and belongs to one
    // RemoteSession generation; authenticated socket identity supplies both
    // parties, so client-provided sender/target aliases are never consulted.
    uploadPayloadV3(upload, type, extra = {}) {
        return {
            type,
            protocolVersion: this.protocolVersion,
            serverBootId: this.serverBootId,
            messageId: uuidv4(),
            remoteSessionId: upload.remoteSessionId,
            generation: upload.generation,
            connectionGeneration: upload.ownerConnectionGeneration,
            uploadId: upload.uploadId,
            ownerEndpointId: upload.ownerEndpointId,
            targetEndpointId: upload.targetEndpointId,
            ...extra,
        };
    }

    uploadOffsetsV3(upload) {
        return Array.from(upload.assetStates.values()).map(asset => ({
            assetId: asset.assetId,
            offset: asset.durableOffset,
            size: asset.size,
            sha256: asset.sha256,
        })).sort((left, right) => left.assetId.localeCompare(right.assetId));
    }

    uploadCompletionInventoryV3(upload) {
        return Array.from(upload.assetStates.values()).map(asset => ({
            assetId: asset.assetId,
            offset: asset.nextOffset,
            size: asset.size,
            sha256: asset.sha256,
        })).sort((left, right) => left.assetId.localeCompare(right.assetId));
    }

    validateUploadInventoryV3(upload, entries, validateOffset) {
        if (!upload || !Array.isArray(entries)
            || entries.length !== upload.assetStates.size) return false;
        const seen = new Set();
        for (const entry of entries) {
            if (!isPlainObject(entry) || !this.isValidOpaqueId(entry.assetId)
                || seen.has(entry.assetId)) return false;
            const asset = upload.assetStates.get(entry.assetId);
            if (!asset || entry.size !== asset.size || entry.sha256 !== asset.sha256
                || !Number.isSafeInteger(entry.offset)
                || !validateOffset(asset, entry.offset)) return false;
            seen.add(entry.assetId);
        }
        return seen.size === upload.assetStates.size;
    }

    sendUploadRejectedV3(clientId, uploadId, code, detail = '', upload = null) {
        const client = this.clients.get(clientId);
        if (!client) return;
        const payload = {
            type: 'upload_rejected',
            protocolVersion: this.protocolVersion,
            serverBootId: this.serverBootId,
            messageId: uuidv4(),
            uploadId: typeof uploadId === 'string' ? uploadId : '',
            code,
            reason: String(detail || code).slice(0, 512),
        };
        if (upload) {
            payload.remoteSessionId = upload.remoteSessionId;
            payload.generation = upload.generation;
            payload.ownerEndpointId = upload.ownerEndpointId;
            payload.targetEndpointId = upload.targetEndpointId;
        }
        this.sendToEndpoint(client.endpointId, payload);
    }

    normalizeUploadFilesV3(files) {
        if (!Array.isArray(files) || files.length < 1 || files.length > this.MAX_UPLOAD_FILES) {
            return { ok: false, error: 'invalid_upload_file_count' };
        }
        const assets = [];
        const assetIds = new Set();
        const mediaIds = new Set();
        let totalSize = 0;
        for (const file of files) {
            if (!isPlainObject(file) || !this.isValidOpaqueId(file.assetId)
                || assetIds.has(file.assetId) || !SHA256_PATTERN.test(file.fileId || '')
                || !SHA256_PATTERN.test(file.sha256 || '') || file.fileId !== file.sha256
                || typeof file.name !== 'string' || file.name.length < 1 || file.name.length > 255
                || /[\\/\x00-\x1f\x7f]/.test(file.name)
                || typeof file.extension !== 'string'
                || !ALLOWED_MEDIA_EXTENSIONS.has(file.extension.toLowerCase())
                || !Number.isSafeInteger(file.size) || file.size < 1
                || file.size > this.MAX_UPLOAD_FILE_BYTES
                || !Array.isArray(file.mediaIds) || file.mediaIds.length < 1
                || file.mediaIds.length > 4096) {
                return { ok: false, error: 'invalid_upload_asset_metadata' };
            }
            const extension = file.extension.toLowerCase();
            const lastDot = file.name.lastIndexOf('.');
            const filenameExtension = lastDot > 0 && lastDot < file.name.length - 1
                ? file.name.slice(lastDot + 1).toLowerCase() : '';
            if (filenameExtension !== extension) {
                return { ok: false, error: 'upload_extension_mismatch' };
            }
            for (const mediaId of file.mediaIds) {
                if (!this.isValidOpaqueId(mediaId) || mediaIds.has(mediaId)) {
                    return { ok: false, error: 'invalid_or_duplicate_media_id' };
                }
                mediaIds.add(mediaId);
            }
            if (totalSize > this.MAX_UPLOAD_TOTAL_BYTES - file.size) {
                return { ok: false, error: 'upload_total_size_exceeded' };
            }
            totalSize += file.size;
            assetIds.add(file.assetId);
            assets.push({
                assetId: file.assetId,
                extension,
                fileId: file.fileId,
                mediaIds: file.mediaIds.slice().sort(),
                name: file.name,
                sha256: file.sha256,
                size: file.size,
            });
        }
        assets.sort((left, right) => left.assetId.localeCompare(right.assetId));
        return { ok: true, assets, totalSize };
    }

    validateUploadPartyV3(clientId, message, role, allowGrace = false) {
        const validated = this.validateSessionMessage(clientId, message, {
            ownerOnly: role === 'owner',
            allowGrace,
        });
        if (!validated.ok) return validated;
        if (validated.role !== role) return { ok: false, error: `not_upload_${role}` };
        return validated;
    }

    removeUploadV3(upload) {
        if (!upload) return;
        this.uploads.delete(upload.uploadId);
        const session = this.remoteSessions.get(upload.remoteSessionId);
        if (session) session.activeUploadIds.delete(upload.uploadId);
    }

    rememberUploadResultV3(upload, status, extra = {}, now = Date.now()) {
        this.uploadTombstones.delete(upload.uploadId);
        this.uploadTombstones.set(upload.uploadId, {
            uploadId: upload.uploadId,
            remoteSessionId: upload.remoteSessionId,
            generation: upload.generation,
            ownerEndpointId: upload.ownerEndpointId,
            targetEndpointId: upload.targetEndpointId,
            ownerRuntimeId: upload.ownerRuntimeId,
            ownerConnectionGeneration: upload.ownerConnectionGeneration,
            manifestDigest: upload.manifestDigest,
            status,
            expiresAt: now + 60_000,
            ...extra,
        });
        this.pruneUploadTombstonesV3(now);
    }

    pruneUploadTombstonesV3(now = Date.now()) {
        for (const [uploadId, result] of this.uploadTombstones) {
            if (!result || result.expiresAt <= now) this.uploadTombstones.delete(uploadId);
        }
        while (this.uploadTombstones.size > this.MAX_REMOTE_SCENE_TOMBSTONES) {
            this.uploadTombstones.delete(this.uploadTombstones.keys().next().value);
        }
    }

    assetRemovalPayloadV3(removal, type = 'upload_removed', extra = {}) {
        return {
            type,
            protocolVersion: this.protocolVersion,
            serverBootId: this.serverBootId,
            messageId: uuidv4(),
            remoteSessionId: removal.remoteSessionId,
            generation: removal.generation,
            connectionGeneration: removal.ownerConnectionGeneration,
            removalId: removal.removalId,
            uploadId: removal.uploadId,
            assetId: removal.assetId,
            offset: removal.offset,
            size: removal.size,
            sha256: removal.sha256,
            fileId: removal.fileId,
            extension: removal.extension,
            ownerEndpointId: removal.ownerEndpointId,
            targetEndpointId: removal.targetEndpointId,
            ...extra,
        };
    }

    sendAssetRemovalProtocolErrorV3(clientId, message, code, detail = code) {
        const client = this.clients.get(clientId);
        if (!client) return false;
        const payload = {
            type: 'error',
            protocolVersion: this.protocolVersion,
            serverBootId: this.serverBootId,
            messageId: uuidv4(),
            scope: 'upload_remove',
            code,
            message: String(detail || code).slice(0, 512),
            connectionGeneration: client.connectionGeneration,
        };
        for (const field of ['remoteSessionId', 'removalId', 'uploadId', 'assetId']) {
            if (this.isValidOpaqueId(message && message[field])) payload[field] = message[field];
        }
        for (const field of ['generation', 'offset', 'size']) {
            if (Number.isSafeInteger(message && message[field])
                && message[field] >= 0) payload[field] = message[field];
        }
        if (SHA256_PATTERN.test(message && message.sha256 || '')) {
            payload.sha256 = message.sha256;
        }
        const session = message && typeof message.remoteSessionId === 'string'
            ? this.remoteSessions.get(message.remoteSessionId) : null;
        if (session && (client.endpointId === session.ownerEndpointId
            || client.endpointId === session.targetEndpointId)) {
            payload.ownerEndpointId = session.ownerEndpointId;
            payload.targetEndpointId = session.targetEndpointId;
        }
        return this.sendToEndpoint(client.endpointId, payload);
    }

    assetRemovalMatchesMessageV3(removal, message, includeDerived = false) {
        if (!removal || !message
            || message.remoteSessionId !== removal.remoteSessionId
            || message.generation !== removal.generation
            || message.removalId !== removal.removalId
            || message.uploadId !== removal.uploadId
            || message.assetId !== removal.assetId
            || message.offset !== removal.offset
            || message.size !== removal.size
            || message.sha256 !== removal.sha256) return false;
        return !includeDerived
            || (message.fileId === removal.fileId
                && message.extension === removal.extension);
    }

    sceneRunUsesAssetV3(run, assetId) {
        return !!run && Array.isArray(run.manifest)
            && run.manifest.some(entry => entry && entry.assetId === assetId);
    }

    dispatchAssetRemovalV3(removal, forceReplay = false) {
        if (!removal || !this.pendingAssetRemovals.has(removal.removalId)) return false;
        const session = this.remoteSessions.get(removal.remoteSessionId);
        if (!session || !['Active', 'Grace'].includes(session.phase)) return false;
        const run = this.sceneRuns.getForSession(removal.remoteSessionId);
        if (this.sceneRunUsesAssetV3(run, removal.assetId)) {
            removal.waitingForSceneRunId = run.sceneRunId;
            this.initiateSceneStop(run, 'asset_removed', false);
            return false;
        }
        removal.waitingForSceneRunId = null;
        if (session.phase !== 'Active') return false;
        const targetId = this.resolveClientId(removal.targetEndpointId);
        const target = targetId ? this.clients.get(targetId) : null;
        if (target) removal.targetConnectionGeneration = target.connectionGeneration;
        const delivered = this.sendToEndpoint(removal.targetEndpointId,
            this.assetRemovalPayloadV3(removal, 'upload_remove', {
                replay: forceReplay || removal.dispatchAttempts > 0,
                reason: removal.reason,
            }));
        if (delivered) {
            removal.dispatchAttempts += 1;
            removal.lastDispatchedAt = Date.now();
            removal.phase = 'awaiting_target_commit';
        }
        return delivered;
    }

    dispatchReadyAssetRemovalsForSession(remoteSessionId) {
        for (const removal of this.pendingAssetRemovals.values()) {
            if (removal.remoteSessionId !== remoteSessionId) continue;
            this.dispatchAssetRemovalV3(removal);
        }
    }

    failAssetRemovalsWaitingForSceneV3(run, code, now = Date.now()) {
        if (!run) return false;
        let failed = false;
        for (const removal of Array.from(this.pendingAssetRemovals.values())) {
            if (removal.remoteSessionId !== run.remoteSessionId
                || removal.waitingForSceneRunId !== run.sceneRunId) continue;
            failed = true;
            this.settleAssetRemovalV3(removal, {
                success: false,
                result: 'cleanup_error',
                code,
                reason: 'The scene render graph did not stop before asset cleanup',
            }, now);
            this.terminateSessionAfterAssetRemovalFailureV3(removal, code, now);
        }
        return failed;
    }

    rememberAssetRemovalResultV3(removal, result, now = Date.now()) {
        const terminal = {
            ...removal,
            phase: 'terminal',
            status: result.success === true ? 'committed' : 'failed',
            success: result.success === true,
            result: result.result,
            code: result.code,
            reason: result.reason,
            cacheQuarantined: result.cacheQuarantined === true,
            removedFileCount: result.removedFileCount,
            quarantinedBytes: result.quarantinedBytes,
            settledAt: now,
            expiresAt: now + this.ASSET_REMOVAL_TOMBSTONE_TTL_MS,
        };
        this.assetRemovalTombstones.delete(removal.removalId);
        this.assetRemovalTombstones.set(removal.removalId, terminal);
        this.pruneAssetRemovalTombstonesV3(now);
        return terminal;
    }

    pruneAssetRemovalTombstonesV3(now = Date.now()) {
        for (const [removalId, result] of this.assetRemovalTombstones) {
            if (!result || result.expiresAt <= now) {
                this.assetRemovalTombstones.delete(removalId);
            }
        }
        while (this.assetRemovalTombstones.size > this.MAX_REMOTE_SCENE_TOMBSTONES) {
            this.assetRemovalTombstones.delete(
                this.assetRemovalTombstones.keys().next().value);
        }
    }

    invalidateUploadReplayAfterAssetRemovalV3(removal, now = Date.now()) {
        const previous = this.uploadTombstones.get(removal.uploadId);
        const invalidated = {
            uploadId: removal.uploadId,
            remoteSessionId: removal.remoteSessionId,
            generation: removal.generation,
            ownerEndpointId: removal.ownerEndpointId,
            targetEndpointId: removal.targetEndpointId,
            manifestDigest: previous && previous.manifestDigest,
            status: 'asset_removed',
            removedAssetId: removal.assetId,
            expiresAt: Math.max(previous && previous.expiresAt || 0, now + 60_000),
        };
        this.uploadTombstones.delete(removal.uploadId);
        this.uploadTombstones.set(removal.uploadId, invalidated);
        this.pruneUploadTombstonesV3(now);
    }

    settleAssetRemovalV3(removal, result, now = Date.now()) {
        if (!removal || !this.pendingAssetRemovals.has(removal.removalId)) return null;
        let finalResult = { ...result };
        if (finalResult.success === true) {
            const inventory = this.sessionAssets.get(removal.remoteSessionId);
            const stored = inventory && inventory.get(removal.assetId);
            const exactInventory = stored
                && stored.uploadId === removal.uploadId
                && stored.fileId === removal.fileId
                && stored.sha256 === removal.sha256
                && stored.size === removal.size
                && stored.extension === removal.extension
                && stored.generation === removal.generation
                && stored.ownerEndpointId === removal.ownerEndpointId
                && stored.targetEndpointId === removal.targetEndpointId;
            if (!exactInventory) {
                finalResult = {
                    success: false,
                    result: 'rejected',
                    code: 'asset_inventory_changed',
                    reason: 'The validated session inventory changed before removal commit',
                };
            } else {
                inventory.delete(removal.assetId);
                if (inventory.size === 0) this.sessionAssets.delete(removal.remoteSessionId);
                this.invalidateUploadReplayAfterAssetRemovalV3(removal, now);
            }
        }
        this.pendingAssetRemovals.delete(removal.removalId);
        const terminal = this.rememberAssetRemovalResultV3(removal, finalResult, now);
        this.sendToEndpoint(removal.ownerEndpointId,
            this.assetRemovalPayloadV3(terminal, 'upload_removed', {
                success: terminal.success,
                result: terminal.result,
                code: terminal.code,
                reason: terminal.reason,
                cacheQuarantined: terminal.cacheQuarantined,
                removedFileCount: terminal.removedFileCount,
                quarantinedBytes: terminal.quarantinedBytes,
            }));
        return terminal;
    }

    abortAssetRemovalsForRemoteSession(session, reason) {
        if (!session) return;
        for (const removal of Array.from(this.pendingAssetRemovals.values())) {
            if (removal.remoteSessionId !== session.remoteSessionId) continue;
            this.settleAssetRemovalV3(removal, {
                success: false,
                result: 'rejected',
                code: 'remote_session_terminating',
                reason: String(reason || 'remote_session_terminating').slice(0, 128),
            });
        }
    }

    terminateSessionAfterAssetRemovalFailureV3(removal, reason, now = Date.now()) {
        const session = removal && this.remoteSessions.get(removal.remoteSessionId);
        if (!session || !['Active', 'Grace'].includes(session.phase)) return false;
        const terminated = this.remoteSessions.terminate(
            session.remoteSessionId, String(reason || 'asset_removal_failed').slice(0, 128), now);
        if (!terminated.ok || terminated.replay) return false;
        this.beginRemoteSessionTeardown(terminated.session);
        return true;
    }

    sweepAssetRemovalsV3(now = Date.now()) {
        this.pruneAssetRemovalTombstonesV3(now);
        for (const removal of Array.from(this.pendingAssetRemovals.values())) {
            if (now < removal.deadlineAt) continue;
            this.settleAssetRemovalV3(removal, {
                success: false,
                result: 'timeout',
                code: 'asset_removal_ack_timeout',
                reason: 'Remote asset removal was not committed before its fixed deadline',
            }, now);
            this.terminateSessionAfterAssetRemovalFailureV3(
                removal, 'asset_removal_timeout', now);
        }
    }

    handleUploadRemoveV3(senderId, message) {
        this.sweepAssetRemovalsV3();
        const validated = this.validateUploadPartyV3(senderId, message, 'owner');
        if (!validated.ok) {
            return this.sendAssetRemovalProtocolErrorV3(
                senderId, message, validated.error, validated.error);
        }
        const { client: owner, session } = validated;
        if (!CANONICAL_UUID_PATTERN.test(message.removalId || '')
            || !this.isValidOpaqueId(message.uploadId)
            || !this.isValidOpaqueId(message.assetId)
            || !Number.isSafeInteger(message.size) || message.size < 1
            || !Number.isSafeInteger(message.offset) || message.offset !== message.size
            || !SHA256_PATTERN.test(message.sha256 || '')) {
            return this.sendAssetRemovalProtocolErrorV3(senderId, message,
                'invalid_asset_removal', 'Invalid remote asset removal fields');
        }
        this.pruneAssetRemovalTombstonesV3();
        const tombstone = this.assetRemovalTombstones.get(message.removalId);
        if (tombstone) {
            if (tombstone.ownerEndpointId !== owner.endpointId
                || !this.assetRemovalMatchesMessageV3(tombstone, message)) {
                return this.sendAssetRemovalProtocolErrorV3(senderId, message,
                    'removal_id_reused', 'A terminal removal identifier cannot be reused');
            }
            return this.sendToEndpoint(owner.endpointId,
                this.assetRemovalPayloadV3(tombstone, 'upload_removed', {
                    success: tombstone.success,
                    result: tombstone.result,
                    code: tombstone.code,
                    reason: tombstone.reason,
                    cacheQuarantined: tombstone.cacheQuarantined,
                    removedFileCount: tombstone.removedFileCount,
                    quarantinedBytes: tombstone.quarantinedBytes,
                    replay: true,
                }));
        }
        const pending = this.pendingAssetRemovals.get(message.removalId);
        if (pending) {
            if (pending.ownerEndpointId !== owner.endpointId
                || !this.assetRemovalMatchesMessageV3(pending, message)) {
                return this.sendAssetRemovalProtocolErrorV3(senderId, message,
                    'removal_id_reused', 'A pending removal identifier cannot be reused');
            }
            this.dispatchAssetRemovalV3(pending, true);
            return;
        }
        if (this.pendingAssetRemovals.size >= this.MAX_PENDING_REMOVALS) {
            return this.sendAssetRemovalProtocolErrorV3(senderId, message,
                'too_many_pending_removals', 'Too many asset removals are pending');
        }
        const inventory = this.sessionAssets.get(session.remoteSessionId);
        const stored = inventory && inventory.get(message.assetId);
        if (!stored || stored.remoteSessionId !== session.remoteSessionId
            || stored.generation !== session.generation
            || stored.ownerEndpointId !== session.ownerEndpointId
            || stored.targetEndpointId !== session.targetEndpointId
            || stored.uploadId !== message.uploadId
            || stored.size !== message.size
            || stored.sha256 !== message.sha256
            || stored.fileId !== message.sha256) {
            return this.sendAssetRemovalProtocolErrorV3(senderId, message,
                'asset_inventory_mismatch',
                'The asset does not exactly match this session inventory');
        }
        for (const other of this.pendingAssetRemovals.values()) {
            if (other.remoteSessionId === session.remoteSessionId
                && other.assetId === message.assetId) {
                return this.sendAssetRemovalProtocolErrorV3(senderId, message,
                    'asset_removal_pending', 'This asset already has a pending removal');
            }
        }
        for (const upload of Array.from(this.uploads.values())) {
            if (upload.remoteSessionId === session.remoteSessionId
                && upload.assetStates && upload.assetStates.has(message.assetId)) {
                this.rejectTrackedUploadV3(upload, 'source_asset_removed',
                    'The source asset was removed while its upload was active');
            }
        }
        const now = Date.now();
        const removal = {
            protocolVersion: 3,
            removalId: message.removalId,
            remoteSessionId: session.remoteSessionId,
            generation: session.generation,
            ownerEndpointId: session.ownerEndpointId,
            targetEndpointId: session.targetEndpointId,
            ownerConnectionGeneration: owner.connectionGeneration,
            targetConnectionGeneration: session.targetConnectionGeneration,
            uploadId: stored.uploadId,
            assetId: stored.assetId,
            offset: stored.size,
            size: stored.size,
            sha256: stored.sha256,
            fileId: stored.fileId,
            extension: stored.extension,
            reason: typeof message.reason === 'string'
                ? message.reason.slice(0, 128) : 'source_removed',
            phase: 'accepted',
            waitingForSceneRunId: null,
            dispatchAttempts: 0,
            lastDispatchedAt: null,
            createdAt: now,
            deadlineAt: now + this.REMOVAL_ACK_TIMEOUT_MS,
        };
        this.pendingAssetRemovals.set(removal.removalId, removal);
        this.dispatchAssetRemovalV3(removal);
    }

    handleUploadRemovedV3(targetId, message) {
        this.sweepAssetRemovalsV3();
        const validated = this.validateUploadPartyV3(targetId, message, 'target', true);
        if (!validated.ok) {
            return this.sendAssetRemovalProtocolErrorV3(
                targetId, message, validated.error, validated.error);
        }
        const removal = this.pendingAssetRemovals.get(message.removalId);
        if (!removal) {
            const tombstone = this.assetRemovalTombstones.get(message.removalId);
            if (tombstone
                && tombstone.targetEndpointId === validated.client.endpointId
                && this.assetRemovalMatchesMessageV3(tombstone, message, true)) {
                if (tombstone.success) {
                    this.sendToEndpoint(tombstone.ownerEndpointId,
                        this.assetRemovalPayloadV3(tombstone, 'upload_removed', {
                            success: true,
                            result: 'committed',
                            cacheQuarantined: true,
                            removedFileCount: tombstone.removedFileCount,
                            quarantinedBytes: tombstone.quarantinedBytes,
                            replay: true,
                        }));
                }
                return;
            }
            return this.sendAssetRemovalProtocolErrorV3(targetId, message,
                'unknown_asset_removal', 'Unknown remote asset removal');
        }
        if (removal.targetEndpointId !== validated.client.endpointId
            || !this.assetRemovalMatchesMessageV3(removal, message, true)) {
            return this.sendAssetRemovalProtocolErrorV3(targetId, message,
                'asset_removal_ack_mismatch',
                'Remote asset removal acknowledgement does not match the request');
        }
        if (removal.phase !== 'awaiting_target_commit') {
            return this.sendAssetRemovalProtocolErrorV3(targetId, message,
                'asset_removal_not_dispatched',
                'Remote asset removal is not awaiting a target commit');
        }
        const committed = message.result === 'committed'
            && message.cacheQuarantined === true
            && message.success !== false;
        if (!committed) {
            const terminal = this.settleAssetRemovalV3(removal, {
                success: false,
                result: 'cleanup_error',
                code: typeof message.errorCode === 'string'
                    ? message.errorCode.slice(0, 128) : 'asset_removal_not_committed',
                reason: typeof message.reason === 'string'
                    ? message.reason.slice(0, 512)
                    : 'The target did not commit the remote asset removal',
            });
            this.terminateSessionAfterAssetRemovalFailureV3(
                removal, 'asset_removal_cleanup_error');
            return terminal;
        }
        const removedFileCount = Number.isSafeInteger(message.removedFileCount)
            ? Math.max(0, message.removedFileCount) : 1;
        const quarantinedBytes = Number.isSafeInteger(message.quarantinedBytes)
            ? Math.max(0, message.quarantinedBytes) : removal.size;
        const terminal = this.settleAssetRemovalV3(removal, {
            success: true,
            result: 'committed',
            cacheQuarantined: true,
            removedFileCount,
            quarantinedBytes,
        });
        if (terminal && !terminal.success) {
            this.terminateSessionAfterAssetRemovalFailureV3(
                removal, 'asset_removal_inventory_changed');
        }
        return terminal;
    }

    rejectTrackedUploadV3(upload, code, detail = code, notifyTarget = true) {
        if (!upload) return;
        if (notifyTarget) {
            this.sendToEndpoint(upload.targetEndpointId,
                this.uploadPayloadV3(upload, 'upload_abort', { code, reason: detail }));
        }
        const ownerId = this.resolveClientId(upload.ownerEndpointId);
        if (ownerId) this.sendUploadRejectedV3(ownerId, upload.uploadId, code, detail, upload);
        this.rememberUploadResultV3(upload, 'rejected', { code, detail });
        this.removeUploadV3(upload);
    }

    handleUploadStartV3(senderId, message, transportSocket = null) {
        const validated = this.validateUploadPartyV3(senderId, message, 'owner');
        if (!validated.ok) {
            return this.sendUploadRejectedV3(senderId, message.uploadId,
                validated.error, validated.error);
        }
        const { client: owner, session } = validated;
        if (!this.isValidOpaqueId(message.uploadId)) {
            return this.sendUploadRejectedV3(senderId, message.uploadId,
                'invalid_upload_id', 'Invalid upload identifier');
        }
        const normalized = this.normalizeUploadFilesV3(message.files);
        if (!normalized.ok) {
            return this.sendUploadRejectedV3(senderId, message.uploadId,
                normalized.error, normalized.error);
        }
        const manifestDigest = crypto.createHash('sha256')
            .update(JSON.stringify(normalized.assets), 'utf8').digest('hex');
        this.pruneUploadTombstonesV3();
        const terminal = this.uploadTombstones.get(message.uploadId);
        if (terminal) {
            if (terminal.status === 'finished'
                && terminal.remoteSessionId === session.remoteSessionId
                && terminal.ownerEndpointId === owner.endpointId
                && terminal.targetEndpointId === session.targetEndpointId
                && terminal.ownerRuntimeId === session.ownerRuntimeId
                && terminal.ownerConnectionGeneration === owner.connectionGeneration
                && terminal.manifestDigest === manifestDigest) {
                return this.sendToEndpoint(owner.endpointId, {
                    ...terminal.payload,
                    messageId: uuidv4(),
                    replay: true,
                });
            }
            return this.sendUploadRejectedV3(senderId, message.uploadId,
                'upload_id_reused', 'A terminal upload identifier cannot be reused');
        }
        if (this.sceneRuns.getForSession(session.remoteSessionId)) {
            return this.sendUploadRejectedV3(senderId, message.uploadId,
                'scene_run_active', 'Uploads are locked while a scene run exists');
        }
        if (Array.from(this.pendingAssetRemovals.values()).some(removal =>
            removal.remoteSessionId === session.remoteSessionId)) {
            return this.sendUploadRejectedV3(senderId, message.uploadId,
                'asset_removal_pending',
                'Uploads are locked while a remote asset removal is pending');
        }
        const inventory = this.sessionAssets.get(session.remoteSessionId) || new Map();
        const resultingAssets = new Map(inventory);
        for (const asset of normalized.assets) resultingAssets.set(asset.assetId, asset);
        const resultingBytes = Array.from(resultingAssets.values())
            .reduce((total, asset) => total + asset.size, 0);
        if (resultingAssets.size > this.MAX_UPLOAD_FILES
            || resultingBytes > this.MAX_UPLOAD_TOTAL_BYTES) {
            return this.sendUploadRejectedV3(senderId, message.uploadId,
                'session_asset_limit_exceeded',
                'The remote session asset inventory would exceed its hard limit');
        }
        const duplicate = this.uploads.get(message.uploadId);
        if (duplicate) {
            if (duplicate.remoteSessionId === session.remoteSessionId
                && duplicate.generation === session.generation
                && duplicate.ownerEndpointId === session.ownerEndpointId
                && duplicate.targetEndpointId === session.targetEndpointId
                && duplicate.ownerRuntimeId === session.ownerRuntimeId
                && duplicate.ownerConnectionGeneration === owner.connectionGeneration
                && duplicate.manifestDigest === manifestDigest) {
                if (duplicate.awaitingTargetValidation) {
                    // B may already have promoted every asset while its final
                    // ACK was lost. Ask it to replay only that exact terminal
                    // validation; never reopen staging or rewind the upload.
                    this.sendToEndpoint(duplicate.targetEndpointId,
                        this.uploadPayloadV3(duplicate, 'upload_complete', {
                            replay: true,
                            assets: this.uploadCompletionInventoryV3(duplicate),
                        }));
                }
                return this.sendToEndpoint(owner.endpointId,
                    this.uploadPayloadV3(duplicate, 'upload_resume_ready', {
                        replay: true,
                        assets: this.uploadOffsetsV3(duplicate),
                    }));
            }
            return this.sendUploadRejectedV3(senderId, message.uploadId,
                'upload_id_reused',
                'An active upload identifier is already bound to a different immutable transfer');
        }
        const ownerUploads = Array.from(this.uploads.values())
            .filter(upload => upload && upload.protocolVersion === 3
                && upload.ownerEndpointId === owner.endpointId);
        if (ownerUploads.some(upload => upload.remoteSessionId === session.remoteSessionId)) {
            return this.sendUploadRejectedV3(senderId, message.uploadId,
                'upload_session_busy', 'Only one upload per remote session is allowed');
        }
        if (ownerUploads.length >= 2) {
            return this.sendUploadRejectedV3(senderId, message.uploadId,
                'upload_concurrency_exceeded', 'At most two outgoing uploads are allowed');
        }
        const assetStates = new Map(normalized.assets.map(asset => [asset.assetId, {
            ...asset,
            nextOffset: 0,
            durableOffset: 0,
        }]));
        const now = Date.now();
        const upload = {
            protocolVersion: 3,
            uploadId: message.uploadId,
            remoteSessionId: session.remoteSessionId,
            generation: session.generation,
            ownerEndpointId: session.ownerEndpointId,
            targetEndpointId: session.targetEndpointId,
            ownerRuntimeId: session.ownerRuntimeId,
            ownerConnectionGeneration: owner.connectionGeneration,
            manifestDigest,
            assets: normalized.assets,
            assetStates,
            totalSize: normalized.totalSize,
            relayedBytes: 0,
            durableBytes: 0,
            awaitingTargetReady: true,
            awaitingTargetValidation: false,
            transportSocket,
            startTime: now,
            lastActivity: now,
        };
        this.uploads.set(upload.uploadId, upload);
        session.activeUploadIds.add(upload.uploadId);
        const delivered = this.sendToEndpoint(upload.targetEndpointId,
            this.uploadPayloadV3(upload, 'upload_start', {
                connectionGeneration: owner.connectionGeneration,
                files: upload.assets,
                totalSize: upload.totalSize,
            }));
        if (!delivered) {
            this.rejectTrackedUploadV3(upload, 'upload_target_unavailable',
                'Upload target is unavailable', false);
        }
    }

    handleUploadResumeV3(senderId, message, transportSocket = null) {
        const upload = this.uploads.get(message.uploadId);
        // Keep the immutable binding even when lease validation synchronously
        // tears the session down and removes it from the live upload map.
        const validated = this.validateUploadPartyV3(senderId, message, 'owner');
        if (!validated.ok || !upload || upload.protocolVersion !== 3
            || upload.remoteSessionId !== message.remoteSessionId
            || upload.ownerEndpointId !== validated.client?.endpointId) {
            const code = validated.ok ? 'unknown_upload' : validated.error;
            return this.sendUploadRejectedV3(senderId, message.uploadId, code, code, upload);
        }
        upload.generation = validated.session.generation;
        upload.ownerConnectionGeneration = validated.client.connectionGeneration;
        upload.transportSocket = transportSocket;
        upload.transportDisconnectedAt = null;
        upload.awaitingTargetReady = true;
        upload.awaitingTargetValidation = false;
        upload.relayedBytes = 0;
        upload.durableBytes = 0;
        for (const asset of upload.assetStates.values()) {
            asset.nextOffset = asset.durableOffset;
            upload.relayedBytes += asset.durableOffset;
            upload.durableBytes += asset.durableOffset;
        }
        upload.lastActivity = Date.now();
        this.sendToEndpoint(upload.targetEndpointId,
            this.uploadPayloadV3(upload, 'upload_resume', {
                connectionGeneration: validated.client.connectionGeneration,
                assets: this.uploadOffsetsV3(upload),
            }));
        this.sendToEndpoint(upload.ownerEndpointId,
            this.uploadPayloadV3(upload, 'upload_resume_ready', {
                replay: false,
                assets: this.uploadOffsetsV3(upload),
            }));
    }

    handleUploadReadyV3(targetId, message) {
        const validated = this.validateUploadPartyV3(targetId, message, 'target');
        const upload = this.uploads.get(message.uploadId);
        if (!validated.ok || !upload || upload.protocolVersion !== 3
            || upload.remoteSessionId !== message.remoteSessionId
            || upload.awaitingTargetValidation || !upload.awaitingTargetReady) return;
        if (upload.generation !== validated.session.generation
            || upload.targetEndpointId !== validated.client.endpointId
            || !this.validateUploadInventoryV3(
                upload, message.assets,
                (asset, offset) => offset === asset.durableOffset)) {
            return this.rejectTrackedUploadV3(upload,
                'invalid_upload_ready_inventory');
        }
        upload.awaitingTargetReady = false;
        upload.lastActivity = Date.now();
        this.sendToEndpoint(upload.ownerEndpointId,
            this.uploadPayloadV3(upload, 'upload_ready', {
                assets: this.uploadOffsetsV3(upload),
            }));
    }

    decodeUploadChunkV3(message) {
        if (typeof message.data !== 'string' || message.data.length < 1
            || message.data.length > this.MAX_UPLOAD_CHUNK_BASE64_LENGTH
            || message.data.length % 4 !== 0
            || !/^[A-Za-z0-9+/]*={0,2}$/.test(message.data)) return null;
        const decoded = Buffer.from(message.data, 'base64');
        if (decoded.length < 1 || decoded.length > 128 * 1024
            || decoded.toString('base64') !== message.data) return null;
        return decoded;
    }

    handleUploadChunkV3(senderId, message, transportSocket = null) {
        const upload = this.uploads.get(message.uploadId);
        const validated = this.validateUploadPartyV3(senderId, message, 'owner');
        if (!validated.ok || !upload || upload.protocolVersion !== 3
            || upload.remoteSessionId !== message.remoteSessionId
            || upload.ownerEndpointId !== validated.client?.endpointId) {
            const code = validated.ok ? 'unknown_upload' : validated.error;
            return this.sendUploadRejectedV3(senderId, message.uploadId, code, code, upload);
        }
        if (upload.transportSocket !== transportSocket) {
            return this.rejectTrackedUploadV3(upload, 'upload_transport_changed');
        }
        const asset = upload.assetStates.get(message.assetId);
        const decoded = this.decodeUploadChunkV3(message);
        if (upload.awaitingTargetReady || upload.awaitingTargetValidation || !asset || !decoded
            || message.offset !== asset.nextOffset || message.size !== decoded.length
            || message.sha256 !== asset.sha256
            || decoded.length > asset.size - asset.nextOffset) {
            return this.rejectTrackedUploadV3(upload, 'invalid_upload_chunk');
        }
        const targetId = this.resolveClientId(upload.targetEndpointId);
        const target = targetId ? this.clients.get(targetId) : null;
        if (!target || !target.ws || target.ws.readyState !== WebSocket.OPEN) return;
        if ((Number(target.ws.bufferedAmount) || 0) > this.MAX_TARGET_BUFFERED_UPLOAD_BYTES) {
            return this.rejectTrackedUploadV3(upload, 'upload_target_backpressure');
        }
        const delivered = this.sendToEndpoint(upload.targetEndpointId,
            this.uploadPayloadV3(upload, 'upload_chunk', {
                connectionGeneration: validated.client.connectionGeneration,
                assetId: asset.assetId,
                offset: message.offset,
                size: message.size,
                sha256: asset.sha256,
                data: message.data,
            }));
        if (!delivered) return;
        asset.nextOffset += decoded.length;
        upload.relayedBytes += decoded.length;
        upload.lastActivity = Date.now();
    }

    handleUploadProgressV3(targetId, message) {
        const validated = this.validateUploadPartyV3(targetId, message, 'target');
        const upload = this.uploads.get(message.uploadId);
        if (!validated.ok || !upload || upload.protocolVersion !== 3
            || upload.remoteSessionId !== message.remoteSessionId
            || !Array.isArray(message.assets)) return;
        if (upload.generation !== validated.session.generation
            || upload.targetEndpointId !== validated.client.endpointId
            || !this.validateUploadInventoryV3(
                upload, message.assets,
                (asset, offset) => offset >= asset.durableOffset
                    && offset <= asset.nextOffset)) {
            return this.rejectTrackedUploadV3(upload,
                'invalid_upload_progress_inventory');
        }
        let progressed = false;
        for (const entry of message.assets) {
            const asset = upload.assetStates.get(entry.assetId);
            if (entry.offset > asset.durableOffset) {
                asset.durableOffset = entry.offset;
                progressed = true;
            }
        }
        if (progressed) {
            upload.durableBytes = Array.from(upload.assetStates.values())
                .reduce((total, asset) => total + asset.durableOffset, 0);
            upload.lastActivity = Date.now();
        }
        this.sendToEndpoint(upload.ownerEndpointId,
            this.uploadPayloadV3(upload, 'upload_progress', {
                durableBytes: upload.durableBytes,
                totalSize: upload.totalSize,
                assets: this.uploadOffsetsV3(upload),
            }));
    }

    handleUploadCompleteV3(senderId, message, transportSocket = null) {
        const upload = this.uploads.get(message.uploadId);
        const validated = this.validateUploadPartyV3(senderId, message, 'owner');
        if (!validated.ok || !upload || upload.protocolVersion !== 3
            || upload.remoteSessionId !== message.remoteSessionId
            || upload.ownerEndpointId !== validated.client?.endpointId) {
            const code = validated.ok ? 'unknown_upload' : validated.error;
            return this.sendUploadRejectedV3(senderId, message.uploadId, code, code, upload);
        }
        if (upload.transportSocket !== transportSocket || upload.awaitingTargetReady) {
            return this.rejectTrackedUploadV3(upload, 'invalid_upload_completion_state');
        }
        if (!this.validateUploadInventoryV3(
            upload, message.assets, (asset, offset) => offset === asset.size)) {
            return this.rejectTrackedUploadV3(
                upload, 'invalid_upload_completion_inventory');
        }
        if (upload.awaitingTargetValidation) return;
        if (upload.relayedBytes !== upload.totalSize
            || Array.from(upload.assetStates.values()).some(asset => asset.nextOffset !== asset.size)) {
            return this.rejectTrackedUploadV3(upload, 'upload_incomplete');
        }
        upload.awaitingTargetValidation = true;
        upload.awaitingTargetValidationSince = Date.now();
        upload.lastActivity = upload.awaitingTargetValidationSince;
        this.sendToEndpoint(upload.targetEndpointId,
            this.uploadPayloadV3(upload, 'upload_complete', {
                connectionGeneration: validated.client.connectionGeneration,
                assets: this.uploadCompletionInventoryV3(upload),
            }));
    }

    handleUploadFinishedV3(targetId, message) {
        const validated = this.validateUploadPartyV3(targetId, message, 'target');
        const upload = this.uploads.get(message.uploadId);
        if (!validated.ok || !upload || upload.protocolVersion !== 3
            || upload.remoteSessionId !== message.remoteSessionId
            || !upload.awaitingTargetValidation || !Array.isArray(message.assets)) return;
        const acknowledgements = new Map();
        for (const entry of message.assets) {
            if (!isPlainObject(entry) || acknowledgements.has(entry.assetId)) {
                return this.rejectTrackedUploadV3(upload, 'invalid_upload_ack');
            }
            acknowledgements.set(entry.assetId, entry);
        }
        const invalid = acknowledgements.size !== upload.assetStates.size
            || Array.from(upload.assetStates.values()).some(asset => {
                const entry = acknowledgements.get(asset.assetId);
                return !entry || entry.offset !== asset.size || entry.size !== asset.size
                    || entry.sha256 !== asset.sha256;
            });
        if (invalid) return this.rejectTrackedUploadV3(upload, 'invalid_upload_ack');
        let inventory = this.sessionAssets.get(upload.remoteSessionId);
        if (!inventory) {
            inventory = new Map();
            this.sessionAssets.set(upload.remoteSessionId, inventory);
        }
        for (const asset of upload.assets) {
            inventory.set(asset.assetId, {
                ...asset,
                uploadId: upload.uploadId,
                remoteSessionId: upload.remoteSessionId,
                generation: upload.generation,
                ownerEndpointId: upload.ownerEndpointId,
                targetEndpointId: upload.targetEndpointId,
                validatedAt: Date.now(),
            });
        }
        const finished = this.uploadPayloadV3(upload, 'upload_finished', {
            assets: upload.assets.map(asset => ({
                assetId: asset.assetId,
                offset: asset.size,
                size: asset.size,
                sha256: asset.sha256,
            })),
        });
        this.rememberUploadResultV3(upload, 'finished', { payload: finished });
        this.removeUploadV3(upload);
        this.sendToEndpoint(upload.ownerEndpointId, finished);
    }

    handleUploadRejectedV3(targetId, message) {
        const validated = this.validateUploadPartyV3(targetId, message, 'target');
        const upload = this.uploads.get(message.uploadId);
        if (!validated.ok || !upload || upload.protocolVersion !== 3
            || upload.remoteSessionId !== message.remoteSessionId) return;
        this.rejectTrackedUploadV3(upload,
            typeof message.code === 'string' ? message.code.slice(0, 128) : 'upload_target_rejected',
            typeof message.reason === 'string' ? message.reason.slice(0, 512) : '', false);
    }

    handleUploadAbortV3(senderId, message) {
        const validated = this.validateUploadPartyV3(senderId, message, 'owner', true);
        const upload = this.uploads.get(message.uploadId);
        if (!validated.ok || !upload || upload.protocolVersion !== 3
            || upload.remoteSessionId !== message.remoteSessionId) return;
        this.sendToEndpoint(upload.targetEndpointId,
            this.uploadPayloadV3(upload, 'upload_abort', {
                reason: typeof message.reason === 'string'
                    ? message.reason.slice(0, 128) : 'owner_abort',
            }));
        this.rememberUploadResultV3(upload, 'aborted');
        this.removeUploadV3(upload);
        this.sendToEndpoint(upload.ownerEndpointId,
            this.uploadPayloadV3(upload, 'upload_aborted', { success: true }));
    }

    handleUploadAbortAcknowledgementV3(targetId, message) {
        // No state is recreated by a late acknowledgement. This is deliberately
        // an idempotent no-op after the server has committed an abort.
        void targetId;
        void message;
    }

    abortUploadsForUploadSocketV3(senderId, socket, reason) {
        void senderId;
        for (const upload of this.uploads.values()) {
            if (!upload || upload.protocolVersion !== 3 || upload.transportSocket !== socket) continue;
            upload.transportSocket = null;
            upload.transportDisconnectedAt = Date.now();
            upload.lastActivity = Date.now();
            upload.pauseReason = String(reason || 'upload_transport_lost').slice(0, 128);
        }
    }

    abortUploadsForClientV3(clientId) {
        const client = this.clients.get(clientId);
        if (!client) return;
        for (const upload of Array.from(this.uploads.values())) {
            if (!upload || upload.protocolVersion !== 3
                || (upload.ownerEndpointId !== client.endpointId
                    && upload.targetEndpointId !== client.endpointId)) continue;
            const peer = upload.ownerEndpointId === client.endpointId
                ? upload.targetEndpointId : upload.ownerEndpointId;
            this.sendToEndpoint(peer, this.uploadPayloadV3(upload, 'upload_abort', {
                reason: 'remote_session_unavailable',
            }));
            this.removeUploadV3(upload);
        }
    }

    cleanupStalledUploadsV3(now = Date.now()) {
        this.pruneUploadTombstonesV3(now);
        this.sweepAssetRemovalsV3(now);
        for (const upload of Array.from(this.uploads.values())) {
            if (!upload || upload.protocolVersion !== 3) continue;
            const waitingForValidation = upload.awaitingTargetValidation === true;
            const timeout = waitingForValidation
                ? this.UPLOAD_TARGET_ACK_TIMEOUT_MS : this.UPLOAD_TIMEOUT_MS;
            const activity = waitingForValidation
                ? upload.awaitingTargetValidationSince : upload.lastActivity;
            if (now - activity < timeout) continue;
            this.rejectTrackedUploadV3(upload,
                waitingForValidation ? 'upload_validation_timeout' : 'upload_idle_timeout');
        }
        this.sweepSceneRuns(now);
    }

    sendError(clientId, errorMessage, code = 'protocol_error', correlation = null) {
        const client = this.clients.get(clientId);
        if (client && client.ws) {
            const payload = {
                type: 'error',
                protocolVersion: this.protocolVersion,
                serverBootId: this.serverBootId,
                messageId: uuidv4(),
                connectionGeneration: client.authenticated
                    ? client.connectionGeneration : undefined,
                code,
                message: errorMessage
            };
            if (correlation && typeof correlation === 'object') {
                if (correlation.scope === 'remote_session') {
                    payload.scope = 'remote_session';
                }
                for (const field of ['requestId', 'remoteSessionId', 'targetEndpointId']) {
                    if (typeof correlation[field] === 'string'
                        && correlation[field].length <= 128) {
                        payload[field] = correlation[field];
                    }
                }
            }
            client.ws.send(JSON.stringify(payload));
        }
    }
    
    getStats() {
        return {
            connectedClients: this.clients.size,
            registeredClients: Array.from(this.clients.values()).filter(c => c.machineName).length
        };
    }
}

module.exports = { MouffetteServer };

if (require.main === module) {
    const server = new MouffetteServer();
    server.start();

    setInterval(() => {
        const stats = server.getStats();
        console.log(`📊 Stats: ${stats.connectedClients} connected, ${stats.registeredClients} registered`);
    }, 30000);

    process.on('SIGINT', () => {
        console.log('\n🛑 Shutting down Mouffette Server...');
        if (server.uploadCleanupInterval) {
            clearInterval(server.uploadCleanupInterval);
            console.log('🧹 Upload cleanup interval stopped');
        }
        if (server.leaseSweepInterval) clearInterval(server.leaseSweepInterval);

        if (server.wss) {
            server.wss.close(() => {
                console.log('✅ Server closed gracefully');
                process.exit(0);
            });
        } else {
            process.exit(0);
        }
    });
}
