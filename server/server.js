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
// Protocol v2 is a hard cut-over. These names are deliberately rejected at
// the envelope boundary rather than translated into their v2 counterparts.
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
    'clientId', 'persistentClientId', 'persistentId', 'sessionId',
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
// MOUFFETTE SERVER - PROTOCOL V2 IDENTITY BOUNDARY
// ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
//
// Protocol v2 derives installation and transport identity exclusively from the
// authenticated socket. Legacy wire identifiers are never accepted as aliases.
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
        this.clients = new Map(); // current transport key -> authenticated device state
        this.connectionGenerationByDevice = new Map();
        this.wss = null;
        // Watching relations: targetId -> Set of watcherIds, and watcherId -> targetId
        this.watchersByTarget = new Map();
        this.watchingByWatcher = new Map();

        // PHASE 2: Server-side state tracking
        this.uploads = new Map();        // uploadId -> { sender, target, canvasSessionId, startTime, files: [fileIds] }
        this.uploadTombstones = new Map(); // uploadId -> bounded terminal v2 result
        this.pendingAssetRemovals = new Map(); // removalId -> immutable session-scoped removal
        this.assetRemovalTombstones = new Map(); // removalId -> bounded committed/error result
        this.clientFiles = new Map();    // persistentClientId -> Map(canvasSessionId -> Set(fileId))
        // Mirrors clientFiles and records which authenticated sender session
        // created each inventory entry. Removal requests must match this owner
        // before either server state or the target filesystem is touched.
        this.clientFileOwners = new Map(); // persistentClientId -> Map(canvasSessionId -> Map(fileId -> senderPersistentId))
        this.clientFileGenerations = new Map(); // same shape, fileId -> validated uploadId
        this.pendingRemovals = new Map(); // removalId -> authenticated sender/target/canvas correlation
        this.pendingUploadAborts = new Map(); // uploadId -> correlated target cleanup acknowledgement
        this.sessionsByPersistent = new Map(); // persistentClientId -> Set(sessionId)
        this.sessionAssets = new Map(); // remoteSessionId -> Map(assetId -> validated metadata)
        
        // PHASE 2: Active canvas tracking (CRITICAL for canvasSessionId validation)
        this.activeCanvases = new Map(); // persistentClientId -> Set(canvasSessionId)
        
        // PHASE 1: Upload timeout configuration
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
            maximumClockUncertaintyMs: this.config.sceneMaxClockSkewMs,
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
            this.cleanupStalledUploadsV2();
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
                        this.abortUploadsForUploadSocketV2(boundClient.id, ws,
                            'Dedicated upload connection closed');
                    }
                    this.unregisterUploadSocket(boundClient, ws);
                    console.log(`📤 Upload channel disconnected for ${boundClient.id}`);
                });
                
                ws.on('error', (error) => {
                    if (!ws.mouffettePreserveSessionUploads) {
                        this.abortUploadsForUploadSocketV2(boundClient.id, ws,
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
                    deviceId: boundClient.deviceId,
                    connectionGeneration: boundClient.connectionGeneration,
                }));
                return;
            }
            
            // Regular control channel connection
            const clientId = uuidv4();
            const clientInfo = {
                id: clientId,
                sessionId: null,
                persistentId: null,
                deviceId: null,
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
                    this.abortUploadsForClientV2(finalId);
                }
                // Clean up watching relationships
                const targetId = this.watchingByWatcher.get(finalId);
                if (targetId) {
                    this.watchingByWatcher.delete(finalId);
                    const set = this.watchersByTarget.get(targetId);
                    if (set) {
                        set.delete(finalId);
                        if (set.size === 0) {
                            this.watchersByTarget.delete(targetId);
                            // Notify target they are no longer watched
                            const targetClient = this.clients.get(targetId);
                            if (targetClient && targetClient.ws) {
                                targetClient.ws.send(JSON.stringify({ type: 'watch_status', watched: false }));
                            }
                        }
                    }
                }
                // Also remove as target from any watchers set
                const watchers = this.watchersByTarget.get(finalId);
                if (watchers) {
                    this.watchersByTarget.delete(finalId);
                    // Notify this (now targetless) client it's no longer watched
                    const targetClient = this.clients.get(finalId);
                    if (targetClient && targetClient.ws) {
                        targetClient.ws.send(JSON.stringify({ type: 'watch_status', watched: false }));
                    }
                }
                if (clientInfo.persistentId) {
                    this.unregisterSessionForPersistent(clientInfo.persistentId, finalId);
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
                    this.abortUploadsForClientV2(finalId);
                }
                // Similar cleanup on error
                const targetId = this.watchingByWatcher.get(finalId);
                if (targetId) {
                    this.watchingByWatcher.delete(finalId);
                    const set = this.watchersByTarget.get(targetId);
                    if (set) {
                        set.delete(finalId);
                        if (set.size === 0) {
                            this.watchersByTarget.delete(targetId);
                            const targetClient = this.clients.get(targetId);
                            if (targetClient && targetClient.ws) {
                                targetClient.ws.send(JSON.stringify({ type: 'watch_status', watched: false }));
                            }
                        }
                    }
                }
                const watchers = this.watchersByTarget.get(finalId);
                if (watchers) {
                    this.watchersByTarget.delete(finalId);
                    const targetClient = this.clients.get(finalId);
                    if (targetClient && targetClient.ws) {
                        targetClient.ws.send(JSON.stringify({ type: 'watch_status', watched: false }));
                    }
                }
                if (clientInfo.persistentId) {
                    this.unregisterSessionForPersistent(clientInfo.persistentId, finalId);
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
        if (!client || !client.persistentId || !client.ws
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

        // Authenticated socket binding is authoritative in v2. Do not add the
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
            ownerDeviceId: run.ownerDeviceId,
            targetDeviceId: run.targetDeviceId,
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
        this.sendToDevice(this.getPersistentId(clientId), payload);
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
        const role = session.ownerDeviceId === client.deviceId ? 'owner'
            : session.targetDeviceId === client.deviceId ? 'target' : null;
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
                || stored.ownerDeviceId !== session.ownerDeviceId
                || stored.targetDeviceId !== session.targetDeviceId
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
            ownerDeviceId: session.ownerDeviceId,
            targetDeviceId: session.targetDeviceId,
        });
        if (!prepared.ok) {
            this.countScenePreparationFailure(message);
            return this.sendSceneError(clientId, prepared.error, prepared.error);
        }
        const run = prepared.run;
        session.sceneRunId = run.sceneRunId;
        if (prepared.replay) {
            this.sendToDevice(client.deviceId, this.scenePayload(run, 'prepare_progress', {
                aggregate: true,
                replay: true,
                percent: run.phase === SCENE_PHASES.PREPARING ? 0 : 100,
            }));
            return;
        }
        const delivered = this.sendToDevice(session.targetDeviceId,
            this.scenePayload(run, 'scene_prepare', { manifest, scene }));
        if (!delivered) {
            this.initiateSceneStop(run, 'scene_target_unavailable', true);
            return this.sendSceneError(clientId, 'scene_target_unavailable',
                'Scene target is unavailable', run);
        }
        this.sendToDevice(client.deviceId, this.scenePayload(run, 'prepare_progress', {
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
            validated.client.deviceId, { percent: message.percent, checklist: message.checklist });
        if (!result.ok) return this.sendSceneError(clientId, result.error, result.error, run);
        this.sendToDevice(run.ownerDeviceId, this.scenePayload(run, 'prepare_progress', {
            reporterDeviceId: validated.client.deviceId,
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
            return this.sendSceneError(this.resolveClientId(run.ownerDeviceId), code,
                typeof message.message === 'string' ? message.message : 'Scene preparation failed', run);
        }
        if (!this.validateChecklist(run, message.checklist)
            || message.checklist.some(item => item.ready !== true)) {
            this.initiateSceneStop(run, 'scene_checklist_incomplete', true);
            return this.sendSceneError(clientId, 'scene_checklist_incomplete',
                'Every preparation checklist item must be ready', run);
        }
        const result = this.sceneRuns.markPrepared(run.sceneRunId,
            validated.client.deviceId, message.digest);
        if (!result.ok) {
            // PREPARED is the fail-closed boundary: a malformed or mismatched
            // acknowledgement means neither peer may retain a prepared graph.
            // Scene teardown deliberately leaves the RemoteSession and its
            // validated upload inventory intact for an explicit retry.
            this.initiateSceneStop(run, result.error, true);
            return this.sendSceneError(clientId, result.error, result.error, run);
        }
        const payload = this.scenePayload(run, 'prepared', {
            reporterDeviceId: validated.client.deviceId,
            allPrepared: result.ready,
            checklist: message.checklist,
        });
        this.sendToDevice(run.ownerDeviceId, payload);
        this.sendToDevice(run.targetDeviceId, payload);
    }

    handleSceneArmed(clientId, message) {
        const validated = this.validateSessionMessage(clientId, message);
        const run = this.sceneRuns.get(message.sceneRunId);
        if (!validated.ok || !run || run.remoteSessionId !== message.remoteSessionId) {
            const code = validated.ok ? 'unknown_scene_run' : validated.error;
            return this.sendSceneError(clientId, code, code, run);
        }
        const result = this.sceneRuns.arm(run.sceneRunId, validated.client.deviceId,
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
                reporterDeviceId: validated.client.deviceId,
                allArmed: false,
            });
            this.sendToDevice(run.ownerDeviceId, payload);
            this.sendToDevice(run.targetDeviceId, payload);
            return;
        }
        const commit = this.scenePayload(run, 'commit', {
            allArmed: true,
            startEpochMs: run.startEpochMs,
            startServerMonotonicMs: run.startServerMonotonicMs,
            activationLeadMs: this.config.sceneActivationLeadMs,
            maximumClockUncertaintyMs: this.config.sceneMaxClockSkewMs,
        });
        const ownerDelivered = this.sendToDevice(run.ownerDeviceId, commit);
        const targetDelivered = this.sendToDevice(run.targetDeviceId, commit);
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
        const result = this.sceneRuns.markStarted(run.sceneRunId, validated.client.deviceId,
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
            reporterDeviceId: validated.client.deviceId,
            allStarted: result.live,
            firstFramePresented: true,
            presentedServerMonotonicMs: message.presentedServerMonotonicMs,
            startSkewMs: result.live ? result.startSkewMs : undefined,
        });
        this.sendToDevice(run.ownerDeviceId, payload);
        this.sendToDevice(run.targetDeviceId, payload);
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
            validated.client.deviceId, message.digest, message.sequence);
        if (!result.ok) return this.sendSceneError(clientId, result.error, result.error, run);
        this.sendToDevice(run.targetDeviceId, this.scenePayload(run, 'state_snapshot', {
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
                return this.sendToDevice(validated.client.deviceId,
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
        this.sendToDevice(result.run.ownerDeviceId, payload);
        this.sendToDevice(result.run.targetDeviceId, payload);
    }

    handleSceneStopped(clientId, message) {
        const validated = this.validateSessionMessage(clientId, message, { allowGrace: true });
        const run = this.sceneRuns.get(message.sceneRunId);
        if (!validated.ok || !run || run.remoteSessionId !== message.remoteSessionId) {
            const code = validated.ok ? 'unknown_scene_run' : validated.error;
            return this.sendSceneError(clientId, code, code, run);
        }
        const result = this.sceneRuns.acknowledgeStopped(run.sceneRunId,
            validated.client.deviceId, message.success);
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
        this.sendToDevice(result.run.ownerDeviceId, payload);
        this.sendToDevice(result.run.targetDeviceId, payload);
        this.dispatchReadyAssetRemovalsForSession(result.run.remoteSessionId);
    }

    sweepSceneRuns(now = this.epochNow(), nowMonotonic = this.monotonicNow()) {
        for (const action of this.sceneRuns.tick(now, nowMonotonic)) {
            if (action.type === 'stop') {
                const payload = this.scenePayload(action.run, 'stop', {
                    reason: action.code,
                    failed: true,
                });
                this.sendToDevice(action.run.ownerDeviceId, payload);
                this.sendToDevice(action.run.targetDeviceId, payload);
                this.sendSceneError(this.resolveClientId(action.run.ownerDeviceId),
                    action.code, action.code, action.run);
            } else if (action.type === 'finalized') {
                const session = this.remoteSessions.get(action.run.remoteSessionId);
                if (session && session.sceneRunId === action.run.sceneRunId) session.sceneRunId = null;
                const payload = this.scenePayload(action.run, 'stopped', {
                    success: false,
                    failed: action.run.phase === SCENE_PHASES.FAILED,
                    reason: action.code,
                });
                this.sendToDevice(action.run.ownerDeviceId, payload);
                this.sendToDevice(action.run.targetDeviceId, payload);
                if (!this.failAssetRemovalsWaitingForSceneV2(
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
            this.sendError(clientId, 'Invalid protocol v2 message type',
                'invalid_message_type');
            return;
        }
        if (LEGACY_MESSAGE_TYPES.has(message.type)
            || (typeof message.type === 'string' && message.type.startsWith('remote_scene_'))) {
            this.sendError(clientId,
                `Legacy message type is not supported by protocol v2: ${message.type}`,
                'legacy_message_type');
            return;
        }
        const legacyField = findLegacyWireField(message);
        if (legacyField) {
            this.sendError(clientId,
                `Legacy field is not supported by protocol v2: ${legacyField}`,
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
                deviceId: client.deviceId,
                messageId: message.messageId,
                type: message.type,
            });
        }
        
        switch (message.type) {
            case 'device_snapshot':
                this.handleDeviceSnapshot(clientId, message);
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
                this.handleUploadStartV2(clientId, message, uploadTransportSocket);
                break;
            case 'upload_resume':
                this.handleUploadResumeV2(clientId, message, uploadTransportSocket);
                break;
            case 'upload_chunk':
                this.handleUploadChunkV2(clientId, message, uploadTransportSocket);
                break;
            case 'upload_complete':
                this.handleUploadCompleteV2(clientId, message, uploadTransportSocket);
                break;
            case 'upload_abort':
                this.handleUploadAbortV2(clientId, message);
                break;
            // Progress/status notifications from target back to sender
            case 'upload_progress':
                this.handleUploadProgressV2(clientId, message);
                break;
            case 'upload_ready':
                this.handleUploadReadyV2(clientId, message);
                break;
            case 'upload_finished':
                this.handleUploadFinishedV2(clientId, message);
                break;
            case 'upload_rejected':
                this.handleUploadRejectedV2(clientId, message);
                break;
            case 'upload_abort_ack':
                this.handleUploadAbortAcknowledgementV2(clientId, message);
                break;
            case 'upload_remove':
                this.handleUploadRemoveV2(clientId, message);
                break;
            case 'upload_removed':
                this.handleUploadRemovedV2(clientId, message);
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
                this.sendError(clientId, 'Unknown protocol v2 message type', 'unknown_message_type');
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
        if (!client || !client.deviceId) return 0;
        let transitions = 0;
        for (const session of this.remoteSessions.sessionsForDevice(client.deviceId)) {
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
        if (!preserveSessionUploads) this.abortUploadsForClientV2(client.id);
        if (client.persistentId) {
            this.unregisterSessionForPersistent(client.persistentId, client.id);
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
                || existing.deviceId !== verified.deviceId) continue;
            if (existing.runtimeId !== verified.runtimeId) {
                if (!this.clientLeaseExpired(existing, monotonicNow)) {
                    this.sendError(clientId, 'This device identity is already online', 'device_already_connected');
                    client.ws.close(1008, 'Device already connected');
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

        const generation = (this.connectionGenerationByDevice.get(verified.deviceId) || 0) + 1;
        this.connectionGenerationByDevice.set(verified.deviceId, generation);
        client.authenticated = true;
        client.deviceId = verified.deviceId;
        client.persistentId = verified.deviceId;
        client.runtimeId = verified.runtimeId;
        client.sessionId = verified.runtimeId;
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
            deviceId: client.deviceId,
            runtimeId: client.runtimeId,
            connectionGeneration: generation,
            policy: {
                policyVersion: this.config.policyVersion,
                heartbeatIntervalMs: this.config.heartbeatIntervalMs,
                leaseTimeoutMs: this.config.leaseTimeoutMs,
                scenePrepareTimeoutMs: this.config.scenePrepareTimeoutMs,
                sceneActivationLeadMs: this.config.sceneActivationLeadMs,
                sceneMaxClockSkewMs: this.config.sceneMaxClockSkewMs,
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
        for (const session of this.remoteSessions.sessionsForDevice(client.deviceId)) {
            const contact = this.remoteSessions.touch(
                session.remoteSessionId, client.deviceId,
                client.connectionGeneration, sessionNow);
            if (!contact.ok && contact.terminalTransition && contact.session) {
                this.beginRemoteSessionTeardown(contact.session);
            }
            if (contact.ok && contact.healthChanged) {
                const payload = this.remoteSessionPayload(session,
                    'remote_session_lease_state');
                payload.state = session.phase === 'Grace' ? 'Grace' : 'Active';
                payload.degradedDeviceId = client.deviceId;
                payload.degraded = false;
                this.sendToDevice(session.ownerDeviceId, payload);
                this.sendToDevice(session.targetDeviceId, payload);
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
        const targetId = this.resolveClientId(message.targetDeviceId);
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
                message.targetDeviceId);
        }

        // A previous controller may have reached its own strict lease while B
        // is still healthy. Terminalize that incoming session synchronously;
        // B remains unavailable until its cleanup commit removes the registry
        // reservation, so no open can race the 100 ms background sweep.
        const incoming = this.remoteSessions.activeIncomingFor(target.deviceId);
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
            ownerDeviceId: owner.deviceId,
            targetDeviceId: target.deviceId,
            ownerRuntimeId: owner.runtimeId,
            targetRuntimeId: target.runtimeId,
            ownerConnectionGeneration: owner.connectionGeneration,
            targetConnectionGeneration: target.connectionGeneration,
        });
        if (!opened.ok) {
            return this.sendRemoteSessionError(
                ownerId, opened.error, opened.error, message, target.deviceId);
        }

        const payload = this.remoteSessionPayload(opened.session, 'remote_session_opened');
        payload.requestId = this.isValidOpaqueId(message.requestId)
            ? message.requestId : undefined;
        payload.resumeToken = opened.session.resumeToken;
        this.sendToDevice(owner.deviceId, payload);
        this.sendToDevice(target.deviceId, payload);
        this.broadcastClientList();
    }

    handleRemoteSessionResume(clientId, message) {
        const client = this.clients.get(clientId);
        if (!client) return;
        const resumed = this.remoteSessions.resume({
            remoteSessionId: message.remoteSessionId,
            deviceId: client.deviceId,
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
                    terminal, client.deviceId);
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
        for (const deviceId of [resumed.session.ownerDeviceId,
                                resumed.session.targetDeviceId]) {
            if (this.sendToDevice(deviceId, payload)) {
                this.remoteSessions.markGenerationDelivered(
                    resumed.session.remoteSessionId, deviceId,
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
            && (tombstone.ownerDeviceId === client.deviceId
                || tombstone.targetDeviceId === client.deviceId);
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
            this.sendRemoteSessionStateToDevice(
                tombstone, 'remote_session_terminating', client.deviceId, {
                    phase: 'CleanupPending',
                    replay: true,
                });
            this.sendRemoteSessionStateToDevice(
                tombstone, 'remote_session_closed', client.deviceId, {
                    cleanupState: 'confirmed',
                    replay: true,
                    requestId: this.isValidOpaqueId(message.requestId)
                        ? message.requestId : undefined,
                });
            return;
        }
        const isParty = client && session
            && (session.ownerDeviceId === client.deviceId
                || session.targetDeviceId === client.deviceId);
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
        const roleConnectionGeneration = client.deviceId === session.ownerDeviceId
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
            : (client.deviceId === session.ownerDeviceId
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
            for (const deviceId of [terminating.session.ownerDeviceId,
                                    terminating.session.targetDeviceId]) {
                this.sendRemoteSessionStateToDevice(
                    terminating.session, 'remote_session_terminating', deviceId, {
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
            || correlatedSession.targetDeviceId !== client.deviceId
            || expectedTargetConnectionGeneration !== client.connectionGeneration
            || expectedTargetRuntimeId !== client.runtimeId)) {
            return this.sendRemoteSessionError(clientId,
                'Stale cleanup acknowledgement',
                'stale_remote_session_generation', message);
        }
        const acknowledged = this.remoteSessions.acknowledgeCleanup(
            message.remoteSessionId,
            message.teardownId,
            client.deviceId,
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
        if (session.ownerDeviceId === client.deviceId) {
            const expectedRuntime = session.ownerTerminalRuntimeId
                || session.ownerRuntimeId;
            if (expectedRuntime !== client.runtimeId) return null;
            session.ownerTerminalConnectionGeneration = client.connectionGeneration;
            session.ownerTerminalRuntimeId = client.runtimeId;
            return 'owner';
        }
        if (session.targetDeviceId === client.deviceId) {
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
        if (session.ownerDeviceId === client.deviceId) {
            const expected = session.ownerTerminalRuntimeId || session.ownerRuntimeId;
            return expected === client.runtimeId;
        }
        if (session.targetDeviceId === client.deviceId) {
            const expected = session.targetTerminalRuntimeId || session.targetRuntimeId;
            return expected === client.runtimeId;
        }
        return false;
    }

    remoteSessionPayload(session, type, recipientDeviceId = null) {
        let ownerConnectionGeneration = session.ownerConnectionGeneration;
        let targetConnectionGeneration = session.targetConnectionGeneration;
        // A terminal cleanup may be delivered on a replacement transport, but
        // the active-session tuple is immutable. Tailor only the recipient's
        // local transport generation; the peer generation remains the value
        // that recipient last accepted while the session was command-capable.
        if (recipientDeviceId === session.ownerDeviceId
            && Number.isSafeInteger(session.ownerTerminalConnectionGeneration)) {
            ownerConnectionGeneration = session.ownerTerminalConnectionGeneration;
        }
        if (recipientDeviceId === session.targetDeviceId
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
            ownerDeviceId: session.ownerDeviceId,
            targetDeviceId: session.targetDeviceId,
            teardownId: session.teardownId || undefined,
            reason: session.teardownReason || undefined,
            sceneRunId: session.sceneRunId || undefined,
        };
    }

    sendRemoteSessionStateToDevice(session, type, deviceId, extra = {}) {
        const resolvedId = this.resolveClientId(deviceId);
        const client = resolvedId ? this.clients.get(resolvedId) : null;
        // Terminal state belongs to the process that held the session, not
        // merely to any later process presenting the same installation key.
        // A restarted target is rebound explicitly by the startup-cleanup
        // replay path before reaching this helper.
        if (!client || !this.terminalRuntimeMatches(session, client)) return false;
        this.bindTerminalDelivery(session, client);
        return this.sendToDevice(deviceId, {
            ...this.remoteSessionPayload(session, type, deviceId),
            ...extra,
        });
    }

    sendRemoteSessionClosedToParties(session, extra = {}) {
        for (const deviceId of [session.ownerDeviceId, session.targetDeviceId]) {
            this.sendRemoteSessionStateToDevice(
                session, 'remote_session_closed', deviceId, {
                    cleanupState: 'confirmed',
                    ...extra,
                });
        }
    }

    replayTerminalStateForClient(client, remoteSessionId = null) {
        if (!client || !client.authenticated || !client.deviceId) return 0;
        let replayed = 0;
        for (const session of this.remoteSessions.sessionsForDevice(client.deviceId)) {
            if (remoteSessionId !== null
                && session.remoteSessionId !== remoteSessionId) continue;
            if (session.phase !== 'Terminating' && session.phase !== 'CleanupPending') continue;
            const isOwner = session.ownerDeviceId === client.deviceId;
            // A new owner process has no resumable in-memory session. A target
            // process is still allowed to finish startup cleanup for the old
            // sender/session cache, even when its runtimeId changed.
            if (isOwner && session.ownerRuntimeId !== client.runtimeId) continue;
            if (!this.bindTerminalDelivery(session, client, {
                allowTargetRuntimeRestart: !isOwner,
            })) continue;
            if (this.sendRemoteSessionStateToDevice(
                    session, 'remote_session_terminating', client.deviceId,
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
            const terminalDelivered = this.sendRemoteSessionStateToDevice(
                tombstone, 'remote_session_terminating', client.deviceId, {
                    phase: 'CleanupPending',
                    replay: true,
                });
            const closedDelivered = this.sendRemoteSessionStateToDevice(
                tombstone, 'remote_session_closed', client.deviceId, {
                    cleanupState: 'confirmed',
                    replay: true,
                });
            if (terminalDelivered || closedDelivered) ++replayed;
        }
        return replayed;
    }

    sendRemoteSessionError(clientId, errorMessage, code, message = {}, targetDeviceId) {
        const correlation = { scope: 'remote_session' };
        const copyOpaque = (field, value) => {
            if (typeof value === 'string' && value.length <= 128
                && this.isValidOpaqueId(value)) {
                correlation[field] = value;
            }
        };
        copyOpaque('requestId', message.requestId);
        copyOpaque('remoteSessionId', message.remoteSessionId);
        copyOpaque('targetDeviceId', targetDeviceId || message.targetDeviceId);
        // Never reflect resumeToken or any other proof material.
        return this.sendError(clientId, errorMessage, code, correlation);
    }

    sendToDevice(deviceId, payload) {
        const resolvedId = this.resolveClientId(deviceId);
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
        for (const deviceId of [session.ownerDeviceId, session.targetDeviceId]) {
            this.sendRemoteSessionStateToDevice(
                session, 'remote_session_terminating', deviceId, {
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
            payload.degradedDeviceId = transition.deviceId;
            payload.degraded = transition.degraded;
            this.sendToDevice(transition.session.ownerDeviceId, payload);
            this.sendToDevice(transition.session.targetDeviceId, payload);
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
        if (!client || !client.deviceId) return false;
        const changed = this.remoteSessions.markDisconnected(client.deviceId, now);
        for (const session of changed) {
            if (session.phase === 'Terminating') {
                this.beginRemoteSessionTeardown(session);
                continue;
            }
            const payload = this.remoteSessionPayload(session, 'remote_session_lease_state');
            payload.state = 'Grace';
            payload.deadlineEpochMs = session.graceDeadlineEpochMs;
            const peer = session.ownerDeviceId === client.deviceId
                ? session.targetDeviceId : session.ownerDeviceId;
            this.sendToDevice(peer, payload);
        }
        if (changed.length > 0) this.broadcastClientList();
        return changed.length > 0;
    }

    rebindSessionGeneration(session) {
        const assets = this.sessionAssets.get(session.remoteSessionId);
        if (assets) {
            for (const asset of assets.values()) asset.generation = session.generation;
        }
        const ownerId = this.resolveClientId(session.ownerDeviceId);
        const owner = ownerId ? this.clients.get(ownerId) : null;
        const targetId = this.resolveClientId(session.targetDeviceId);
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
                ownerDeviceId: upload.ownerDeviceId,
                targetDeviceId: upload.targetDeviceId,
                reason: String(reason || 'remote_session_terminating').slice(0, 128),
            };
            this.sendToDevice(session.ownerDeviceId, payload);
            this.sendToDevice(session.targetDeviceId, payload);
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

    resolveClientId(targetDeviceId) {
        if (typeof targetDeviceId !== 'string' || !targetDeviceId) return null;
        for (const [transportKey, clientInfo] of this.clients.entries()) {
            if (clientInfo.authenticated && clientInfo.deviceId === targetDeviceId) {
                return transportKey;
            }
        }
        return null;
    }

    relayToTarget(senderId, targetClientId, message) {
        // Phase 3: resolve targetClientId (can be sessionId or persistentId)
        const resolvedId = this.resolveClientId(targetClientId);
        const targetClient = resolvedId ? this.clients.get(resolvedId) : null;

        if (!targetClient || !targetClient.ws
            || targetClient.ws.readyState !== WebSocket.OPEN) {
            const senderClient = this.clients.get(senderId);
            if (senderClient && senderClient.ws) {
                senderClient.ws.send(JSON.stringify({
                    type: 'error',
                    message: 'Target client not found',
                }));
            }
            return false;
        }
        const isUploadMessage = typeof message.type === 'string'
            && (message.type.startsWith('upload_') || message.type === 'remove_file'
                || message.type === 'remove_all_files');
        if (isUploadMessage) {
            message.senderClientId = senderId;
            message.senderPersistentClientId = this.getPersistentId(senderId);
        } else if (!message.senderClientId) {
            message.senderClientId = senderId;
        }
        try {
            targetClient.ws.send(JSON.stringify(message));
            return true;
        } catch (e) {
            console.error('❌ Relay to target failed:', e);
            return false;
        }
    }

    // Helper to relay a message from target -> sender
    relayToSender(targetId, senderClientId, message) {
        const senderClient = this.clients.get(senderClientId);
        if (!senderClient || !senderClient.ws
            || senderClient.ws.readyState !== WebSocket.OPEN) return false;
        if (!message.targetClientId) message.targetClientId = targetId;
        try {
            senderClient.ws.send(JSON.stringify(message));
            return true;
        } catch (e) {
            console.error('❌ Relay to sender failed:', e);
            return false;
        }
    }

    getPersistentId(clientId) {
        const client = this.clients.get(clientId);
        return client && client.persistentId ? client.persistentId : clientId;
    }

    registerSessionForPersistent(persistentId, sessionId) {
        if (!persistentId || !sessionId) return;
        let sessions = this.sessionsByPersistent.get(persistentId);
        if (!sessions) {
            sessions = new Set();
            this.sessionsByPersistent.set(persistentId, sessions);
        }
        sessions.add(sessionId);
    }

    unregisterSessionForPersistent(persistentId, sessionId) {
        if (!persistentId || !sessionId) return;
        const sessions = this.sessionsByPersistent.get(persistentId);
        if (!sessions) return;
        sessions.delete(sessionId);
        if (sessions.size === 0) {
            this.sessionsByPersistent.delete(persistentId);
        }
    }

    cleanupWatcherReferences(oldId, newId) {
        if (!oldId || !newId || oldId === newId) return;
        if (this.watchersByTarget.has(oldId)) {
            const watchers = this.watchersByTarget.get(oldId);
            this.watchersByTarget.delete(oldId);
            if (this.watchersByTarget.has(newId)) {
                const merged = this.watchersByTarget.get(newId);
                watchers.forEach(watcher => merged.add(watcher));
            } else {
                this.watchersByTarget.set(newId, watchers);
            }
        }
        for (const [targetId, watchers] of this.watchersByTarget.entries()) {
            if (watchers.has(oldId)) {
                watchers.delete(oldId);
                watchers.add(newId);
            }
        }
        if (this.watchingByWatcher.has(oldId)) {
            const target = this.watchingByWatcher.get(oldId);
            this.watchingByWatcher.delete(oldId);
            this.watchingByWatcher.set(newId, target);
        }
    }

    handleRequestScreens(requesterId, message) {
        const targetId = this.resolveClientId(message.targetDeviceId);
        const requester = this.clients.get(requesterId);
        const target = targetId ? this.clients.get(targetId) : null;
        if (!requester) return;
        if (!target || !target.machineName) {
            requester.ws.send(JSON.stringify({
                type: 'error',
                message: 'Target client not found or not registered'
            }));
            return;
        }
        if (!target.persistentId) {
            console.warn(`⚠️ request_screens target ${targetId} missing persistentId`);
            requester.ws.send(JSON.stringify({
                type: 'error',
                message: 'Target client missing persistent identity'
            }));
            return;
        }
        // Reply with current known screens info for the target
        requester.ws.send(JSON.stringify({
                type: 'screens_info',
                protocolVersion: this.protocolVersion,
                serverBootId: this.serverBootId,
                clientInfo: {
                id: target.deviceId,
                deviceId: target.deviceId,
                runtimeId: target.runtimeId,
                machineName: target.machineName,
                platform: target.platform,
                screens: target.screens,
                systemUI: target.systemUI || [],
                volumePercent: target.volumePercent
            }
        }));
    }

    handleDeviceSnapshot(clientId, message) {
        const client = this.clients.get(clientId);
        if (!client || !client.authenticated || !client.deviceId || !client.runtimeId) return;

        // device_snapshot is an authoritative replacement, never a partial
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
            this.sendError(clientId, `Invalid device snapshot: ${invalidSnapshot}`,
                'invalid_device_snapshot', correlation);
            return;
        }

        client.socketLabel = client.socketLabel || clientId;
        // The map key is the immutable transport connection id. runtimeId is
        // scoped by the authenticated device and must never become a global
        // key: two different installations may legitimately choose the same
        // random runtime value without being able to evict one another.
        client.sessionId = client.runtimeId;
        client.persistentId = client.deviceId;
        this.registerSessionForPersistent(client.deviceId, client.id);

        client.machineName = message.machineName;
        client.platform = message.platform;
        client.screens = message.screens;
        client.systemUI = Array.isArray(message.systemUI) ? message.systemUI : [];
        client.volumePercent = message.volumePercent;

        this.logProtocolEvent('device_snapshot_applied', {
            connectionId: client.id,
            deviceId: client.deviceId,
            messageId: message.messageId,
            screenCount: client.screens.length,
        });

        const applied = {
            type: 'device_snapshot_applied',
            protocolVersion: this.protocolVersion,
            serverBootId: this.serverBootId,
            messageId: uuidv4(),
            connectionGeneration: client.connectionGeneration,
            snapshot: {
                deviceId: client.deviceId,
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
            .filter(c => c.id !== clientId && c.machineName && c.persistentId) // Don't include self and only registered clients with stable identity
            .map(c => {
                const incoming = this.remoteSessions.activeIncomingFor(c.deviceId);
                let remoteSessionState = 'Available';
                if (incoming) {
                    if (incoming.ownerDeviceId === client.deviceId) {
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
                    deviceId: c.deviceId,
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

    // Protocol v2 upload state. A transfer is immutable and belongs to one
    // RemoteSession generation; authenticated socket identity supplies both
    // parties, so client-provided sender/target aliases are never consulted.
    uploadPayloadV2(upload, type, extra = {}) {
        return {
            type,
            protocolVersion: this.protocolVersion,
            serverBootId: this.serverBootId,
            messageId: uuidv4(),
            remoteSessionId: upload.remoteSessionId,
            generation: upload.generation,
            connectionGeneration: upload.ownerConnectionGeneration,
            uploadId: upload.uploadId,
            ownerDeviceId: upload.ownerDeviceId,
            targetDeviceId: upload.targetDeviceId,
            ...extra,
        };
    }

    uploadOffsetsV2(upload) {
        return Array.from(upload.assetStates.values()).map(asset => ({
            assetId: asset.assetId,
            offset: asset.durableOffset,
            size: asset.size,
            sha256: asset.sha256,
        })).sort((left, right) => left.assetId.localeCompare(right.assetId));
    }

    uploadCompletionInventoryV2(upload) {
        return Array.from(upload.assetStates.values()).map(asset => ({
            assetId: asset.assetId,
            offset: asset.nextOffset,
            size: asset.size,
            sha256: asset.sha256,
        })).sort((left, right) => left.assetId.localeCompare(right.assetId));
    }

    validateUploadInventoryV2(upload, entries, validateOffset) {
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

    sendUploadRejectedV2(clientId, uploadId, code, detail = '', upload = null) {
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
            payload.ownerDeviceId = upload.ownerDeviceId;
            payload.targetDeviceId = upload.targetDeviceId;
        }
        this.sendToDevice(client.deviceId, payload);
    }

    normalizeUploadFilesV2(files) {
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

    validateUploadPartyV2(clientId, message, role, allowGrace = false) {
        const validated = this.validateSessionMessage(clientId, message, {
            ownerOnly: role === 'owner',
            allowGrace,
        });
        if (!validated.ok) return validated;
        if (validated.role !== role) return { ok: false, error: `not_upload_${role}` };
        return validated;
    }

    removeUploadV2(upload) {
        if (!upload) return;
        this.uploads.delete(upload.uploadId);
        const session = this.remoteSessions.get(upload.remoteSessionId);
        if (session) session.activeUploadIds.delete(upload.uploadId);
    }

    rememberUploadResultV2(upload, status, extra = {}, now = Date.now()) {
        this.uploadTombstones.delete(upload.uploadId);
        this.uploadTombstones.set(upload.uploadId, {
            uploadId: upload.uploadId,
            remoteSessionId: upload.remoteSessionId,
            generation: upload.generation,
            ownerDeviceId: upload.ownerDeviceId,
            targetDeviceId: upload.targetDeviceId,
            ownerRuntimeId: upload.ownerRuntimeId,
            ownerConnectionGeneration: upload.ownerConnectionGeneration,
            manifestDigest: upload.manifestDigest,
            status,
            expiresAt: now + 60_000,
            ...extra,
        });
        this.pruneUploadTombstonesV2(now);
    }

    pruneUploadTombstonesV2(now = Date.now()) {
        for (const [uploadId, result] of this.uploadTombstones) {
            if (!result || result.expiresAt <= now) this.uploadTombstones.delete(uploadId);
        }
        while (this.uploadTombstones.size > this.MAX_REMOTE_SCENE_TOMBSTONES) {
            this.uploadTombstones.delete(this.uploadTombstones.keys().next().value);
        }
    }

    assetRemovalPayloadV2(removal, type = 'upload_removed', extra = {}) {
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
            ownerDeviceId: removal.ownerDeviceId,
            targetDeviceId: removal.targetDeviceId,
            ...extra,
        };
    }

    sendAssetRemovalProtocolErrorV2(clientId, message, code, detail = code) {
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
        if (session && (client.deviceId === session.ownerDeviceId
            || client.deviceId === session.targetDeviceId)) {
            payload.ownerDeviceId = session.ownerDeviceId;
            payload.targetDeviceId = session.targetDeviceId;
        }
        return this.sendToDevice(client.deviceId, payload);
    }

    assetRemovalMatchesMessageV2(removal, message, includeDerived = false) {
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

    sceneRunUsesAssetV2(run, assetId) {
        return !!run && Array.isArray(run.manifest)
            && run.manifest.some(entry => entry && entry.assetId === assetId);
    }

    dispatchAssetRemovalV2(removal, forceReplay = false) {
        if (!removal || !this.pendingAssetRemovals.has(removal.removalId)) return false;
        const session = this.remoteSessions.get(removal.remoteSessionId);
        if (!session || !['Active', 'Grace'].includes(session.phase)) return false;
        const run = this.sceneRuns.getForSession(removal.remoteSessionId);
        if (this.sceneRunUsesAssetV2(run, removal.assetId)) {
            removal.waitingForSceneRunId = run.sceneRunId;
            this.initiateSceneStop(run, 'asset_removed', false);
            return false;
        }
        removal.waitingForSceneRunId = null;
        if (session.phase !== 'Active') return false;
        const targetId = this.resolveClientId(removal.targetDeviceId);
        const target = targetId ? this.clients.get(targetId) : null;
        if (target) removal.targetConnectionGeneration = target.connectionGeneration;
        const delivered = this.sendToDevice(removal.targetDeviceId,
            this.assetRemovalPayloadV2(removal, 'upload_remove', {
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
            this.dispatchAssetRemovalV2(removal);
        }
    }

    failAssetRemovalsWaitingForSceneV2(run, code, now = Date.now()) {
        if (!run) return false;
        let failed = false;
        for (const removal of Array.from(this.pendingAssetRemovals.values())) {
            if (removal.remoteSessionId !== run.remoteSessionId
                || removal.waitingForSceneRunId !== run.sceneRunId) continue;
            failed = true;
            this.settleAssetRemovalV2(removal, {
                success: false,
                result: 'cleanup_error',
                code,
                reason: 'The scene render graph did not stop before asset cleanup',
            }, now);
            this.terminateSessionAfterAssetRemovalFailureV2(removal, code, now);
        }
        return failed;
    }

    rememberAssetRemovalResultV2(removal, result, now = Date.now()) {
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
        this.pruneAssetRemovalTombstonesV2(now);
        return terminal;
    }

    pruneAssetRemovalTombstonesV2(now = Date.now()) {
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

    invalidateUploadReplayAfterAssetRemovalV2(removal, now = Date.now()) {
        const previous = this.uploadTombstones.get(removal.uploadId);
        const invalidated = {
            uploadId: removal.uploadId,
            remoteSessionId: removal.remoteSessionId,
            generation: removal.generation,
            ownerDeviceId: removal.ownerDeviceId,
            targetDeviceId: removal.targetDeviceId,
            manifestDigest: previous && previous.manifestDigest,
            status: 'asset_removed',
            removedAssetId: removal.assetId,
            expiresAt: Math.max(previous && previous.expiresAt || 0, now + 60_000),
        };
        this.uploadTombstones.delete(removal.uploadId);
        this.uploadTombstones.set(removal.uploadId, invalidated);
        this.pruneUploadTombstonesV2(now);
    }

    settleAssetRemovalV2(removal, result, now = Date.now()) {
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
                && stored.ownerDeviceId === removal.ownerDeviceId
                && stored.targetDeviceId === removal.targetDeviceId;
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
                this.invalidateUploadReplayAfterAssetRemovalV2(removal, now);
            }
        }
        this.pendingAssetRemovals.delete(removal.removalId);
        const terminal = this.rememberAssetRemovalResultV2(removal, finalResult, now);
        this.sendToDevice(removal.ownerDeviceId,
            this.assetRemovalPayloadV2(terminal, 'upload_removed', {
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
            this.settleAssetRemovalV2(removal, {
                success: false,
                result: 'rejected',
                code: 'remote_session_terminating',
                reason: String(reason || 'remote_session_terminating').slice(0, 128),
            });
        }
    }

    terminateSessionAfterAssetRemovalFailureV2(removal, reason, now = Date.now()) {
        const session = removal && this.remoteSessions.get(removal.remoteSessionId);
        if (!session || !['Active', 'Grace'].includes(session.phase)) return false;
        const terminated = this.remoteSessions.terminate(
            session.remoteSessionId, String(reason || 'asset_removal_failed').slice(0, 128), now);
        if (!terminated.ok || terminated.replay) return false;
        this.beginRemoteSessionTeardown(terminated.session);
        return true;
    }

    sweepAssetRemovalsV2(now = Date.now()) {
        this.pruneAssetRemovalTombstonesV2(now);
        for (const removal of Array.from(this.pendingAssetRemovals.values())) {
            if (now < removal.deadlineAt) continue;
            this.settleAssetRemovalV2(removal, {
                success: false,
                result: 'timeout',
                code: 'asset_removal_ack_timeout',
                reason: 'Remote asset removal was not committed before its fixed deadline',
            }, now);
            this.terminateSessionAfterAssetRemovalFailureV2(
                removal, 'asset_removal_timeout', now);
        }
    }

    handleUploadRemoveV2(senderId, message) {
        this.sweepAssetRemovalsV2();
        const validated = this.validateUploadPartyV2(senderId, message, 'owner');
        if (!validated.ok) {
            return this.sendAssetRemovalProtocolErrorV2(
                senderId, message, validated.error, validated.error);
        }
        const { client: owner, session } = validated;
        if (!CANONICAL_UUID_PATTERN.test(message.removalId || '')
            || !this.isValidOpaqueId(message.uploadId)
            || !this.isValidOpaqueId(message.assetId)
            || !Number.isSafeInteger(message.size) || message.size < 1
            || !Number.isSafeInteger(message.offset) || message.offset !== message.size
            || !SHA256_PATTERN.test(message.sha256 || '')) {
            return this.sendAssetRemovalProtocolErrorV2(senderId, message,
                'invalid_asset_removal', 'Invalid remote asset removal fields');
        }
        this.pruneAssetRemovalTombstonesV2();
        const tombstone = this.assetRemovalTombstones.get(message.removalId);
        if (tombstone) {
            if (tombstone.ownerDeviceId !== owner.deviceId
                || !this.assetRemovalMatchesMessageV2(tombstone, message)) {
                return this.sendAssetRemovalProtocolErrorV2(senderId, message,
                    'removal_id_reused', 'A terminal removal identifier cannot be reused');
            }
            return this.sendToDevice(owner.deviceId,
                this.assetRemovalPayloadV2(tombstone, 'upload_removed', {
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
            if (pending.ownerDeviceId !== owner.deviceId
                || !this.assetRemovalMatchesMessageV2(pending, message)) {
                return this.sendAssetRemovalProtocolErrorV2(senderId, message,
                    'removal_id_reused', 'A pending removal identifier cannot be reused');
            }
            this.dispatchAssetRemovalV2(pending, true);
            return;
        }
        if (this.pendingAssetRemovals.size >= this.MAX_PENDING_REMOVALS) {
            return this.sendAssetRemovalProtocolErrorV2(senderId, message,
                'too_many_pending_removals', 'Too many asset removals are pending');
        }
        const inventory = this.sessionAssets.get(session.remoteSessionId);
        const stored = inventory && inventory.get(message.assetId);
        if (!stored || stored.remoteSessionId !== session.remoteSessionId
            || stored.generation !== session.generation
            || stored.ownerDeviceId !== session.ownerDeviceId
            || stored.targetDeviceId !== session.targetDeviceId
            || stored.uploadId !== message.uploadId
            || stored.size !== message.size
            || stored.sha256 !== message.sha256
            || stored.fileId !== message.sha256) {
            return this.sendAssetRemovalProtocolErrorV2(senderId, message,
                'asset_inventory_mismatch',
                'The asset does not exactly match this session inventory');
        }
        for (const other of this.pendingAssetRemovals.values()) {
            if (other.remoteSessionId === session.remoteSessionId
                && other.assetId === message.assetId) {
                return this.sendAssetRemovalProtocolErrorV2(senderId, message,
                    'asset_removal_pending', 'This asset already has a pending removal');
            }
        }
        for (const upload of Array.from(this.uploads.values())) {
            if (upload.remoteSessionId === session.remoteSessionId
                && upload.assetStates && upload.assetStates.has(message.assetId)) {
                this.rejectTrackedUploadV2(upload, 'source_asset_removed',
                    'The source asset was removed while its upload was active');
            }
        }
        const now = Date.now();
        const removal = {
            protocolVersion: 2,
            removalId: message.removalId,
            remoteSessionId: session.remoteSessionId,
            generation: session.generation,
            ownerDeviceId: session.ownerDeviceId,
            targetDeviceId: session.targetDeviceId,
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
        this.dispatchAssetRemovalV2(removal);
    }

    handleUploadRemovedV2(targetId, message) {
        this.sweepAssetRemovalsV2();
        const validated = this.validateUploadPartyV2(targetId, message, 'target', true);
        if (!validated.ok) {
            return this.sendAssetRemovalProtocolErrorV2(
                targetId, message, validated.error, validated.error);
        }
        const removal = this.pendingAssetRemovals.get(message.removalId);
        if (!removal) {
            const tombstone = this.assetRemovalTombstones.get(message.removalId);
            if (tombstone
                && tombstone.targetDeviceId === validated.client.deviceId
                && this.assetRemovalMatchesMessageV2(tombstone, message, true)) {
                if (tombstone.success) {
                    this.sendToDevice(tombstone.ownerDeviceId,
                        this.assetRemovalPayloadV2(tombstone, 'upload_removed', {
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
            return this.sendAssetRemovalProtocolErrorV2(targetId, message,
                'unknown_asset_removal', 'Unknown remote asset removal');
        }
        if (removal.targetDeviceId !== validated.client.deviceId
            || !this.assetRemovalMatchesMessageV2(removal, message, true)) {
            return this.sendAssetRemovalProtocolErrorV2(targetId, message,
                'asset_removal_ack_mismatch',
                'Remote asset removal acknowledgement does not match the request');
        }
        if (removal.phase !== 'awaiting_target_commit') {
            return this.sendAssetRemovalProtocolErrorV2(targetId, message,
                'asset_removal_not_dispatched',
                'Remote asset removal is not awaiting a target commit');
        }
        const committed = message.result === 'committed'
            && message.cacheQuarantined === true
            && message.success !== false;
        if (!committed) {
            const terminal = this.settleAssetRemovalV2(removal, {
                success: false,
                result: 'cleanup_error',
                code: typeof message.errorCode === 'string'
                    ? message.errorCode.slice(0, 128) : 'asset_removal_not_committed',
                reason: typeof message.reason === 'string'
                    ? message.reason.slice(0, 512)
                    : 'The target did not commit the remote asset removal',
            });
            this.terminateSessionAfterAssetRemovalFailureV2(
                removal, 'asset_removal_cleanup_error');
            return terminal;
        }
        const removedFileCount = Number.isSafeInteger(message.removedFileCount)
            ? Math.max(0, message.removedFileCount) : 1;
        const quarantinedBytes = Number.isSafeInteger(message.quarantinedBytes)
            ? Math.max(0, message.quarantinedBytes) : removal.size;
        const terminal = this.settleAssetRemovalV2(removal, {
            success: true,
            result: 'committed',
            cacheQuarantined: true,
            removedFileCount,
            quarantinedBytes,
        });
        if (terminal && !terminal.success) {
            this.terminateSessionAfterAssetRemovalFailureV2(
                removal, 'asset_removal_inventory_changed');
        }
        return terminal;
    }

    rejectTrackedUploadV2(upload, code, detail = code, notifyTarget = true) {
        if (!upload) return;
        if (notifyTarget) {
            this.sendToDevice(upload.targetDeviceId,
                this.uploadPayloadV2(upload, 'upload_abort', { code, reason: detail }));
        }
        const ownerId = this.resolveClientId(upload.ownerDeviceId);
        if (ownerId) this.sendUploadRejectedV2(ownerId, upload.uploadId, code, detail, upload);
        this.rememberUploadResultV2(upload, 'rejected', { code, detail });
        this.removeUploadV2(upload);
    }

    handleUploadStartV2(senderId, message, transportSocket = null) {
        const validated = this.validateUploadPartyV2(senderId, message, 'owner');
        if (!validated.ok) {
            return this.sendUploadRejectedV2(senderId, message.uploadId,
                validated.error, validated.error);
        }
        const { client: owner, session } = validated;
        if (!this.isValidOpaqueId(message.uploadId)) {
            return this.sendUploadRejectedV2(senderId, message.uploadId,
                'invalid_upload_id', 'Invalid upload identifier');
        }
        const normalized = this.normalizeUploadFilesV2(message.files);
        if (!normalized.ok) {
            return this.sendUploadRejectedV2(senderId, message.uploadId,
                normalized.error, normalized.error);
        }
        const manifestDigest = crypto.createHash('sha256')
            .update(JSON.stringify(normalized.assets), 'utf8').digest('hex');
        this.pruneUploadTombstonesV2();
        const terminal = this.uploadTombstones.get(message.uploadId);
        if (terminal) {
            if (terminal.status === 'finished'
                && terminal.remoteSessionId === session.remoteSessionId
                && terminal.ownerDeviceId === owner.deviceId
                && terminal.targetDeviceId === session.targetDeviceId
                && terminal.ownerRuntimeId === session.ownerRuntimeId
                && terminal.ownerConnectionGeneration === owner.connectionGeneration
                && terminal.manifestDigest === manifestDigest) {
                return this.sendToDevice(owner.deviceId, {
                    ...terminal.payload,
                    messageId: uuidv4(),
                    replay: true,
                });
            }
            return this.sendUploadRejectedV2(senderId, message.uploadId,
                'upload_id_reused', 'A terminal upload identifier cannot be reused');
        }
        if (this.sceneRuns.getForSession(session.remoteSessionId)) {
            return this.sendUploadRejectedV2(senderId, message.uploadId,
                'scene_run_active', 'Uploads are locked while a scene run exists');
        }
        if (Array.from(this.pendingAssetRemovals.values()).some(removal =>
            removal.remoteSessionId === session.remoteSessionId)) {
            return this.sendUploadRejectedV2(senderId, message.uploadId,
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
            return this.sendUploadRejectedV2(senderId, message.uploadId,
                'session_asset_limit_exceeded',
                'The remote session asset inventory would exceed its hard limit');
        }
        const duplicate = this.uploads.get(message.uploadId);
        if (duplicate) {
            if (duplicate.remoteSessionId === session.remoteSessionId
                && duplicate.generation === session.generation
                && duplicate.ownerDeviceId === session.ownerDeviceId
                && duplicate.targetDeviceId === session.targetDeviceId
                && duplicate.ownerRuntimeId === session.ownerRuntimeId
                && duplicate.ownerConnectionGeneration === owner.connectionGeneration
                && duplicate.manifestDigest === manifestDigest) {
                if (duplicate.awaitingTargetValidation) {
                    // B may already have promoted every asset while its final
                    // ACK was lost. Ask it to replay only that exact terminal
                    // validation; never reopen staging or rewind the upload.
                    this.sendToDevice(duplicate.targetDeviceId,
                        this.uploadPayloadV2(duplicate, 'upload_complete', {
                            replay: true,
                            assets: this.uploadCompletionInventoryV2(duplicate),
                        }));
                }
                return this.sendToDevice(owner.deviceId,
                    this.uploadPayloadV2(duplicate, 'upload_resume_ready', {
                        replay: true,
                        assets: this.uploadOffsetsV2(duplicate),
                    }));
            }
            return this.sendUploadRejectedV2(senderId, message.uploadId,
                'upload_id_reused',
                'An active upload identifier is already bound to a different immutable transfer');
        }
        const ownerUploads = Array.from(this.uploads.values())
            .filter(upload => upload && upload.protocolVersion === 2
                && upload.ownerDeviceId === owner.deviceId);
        if (ownerUploads.some(upload => upload.remoteSessionId === session.remoteSessionId)) {
            return this.sendUploadRejectedV2(senderId, message.uploadId,
                'upload_session_busy', 'Only one upload per remote session is allowed');
        }
        if (ownerUploads.length >= 2) {
            return this.sendUploadRejectedV2(senderId, message.uploadId,
                'upload_concurrency_exceeded', 'At most two outgoing uploads are allowed');
        }
        const assetStates = new Map(normalized.assets.map(asset => [asset.assetId, {
            ...asset,
            nextOffset: 0,
            durableOffset: 0,
        }]));
        const now = Date.now();
        const upload = {
            protocolVersion: 2,
            uploadId: message.uploadId,
            remoteSessionId: session.remoteSessionId,
            generation: session.generation,
            ownerDeviceId: session.ownerDeviceId,
            targetDeviceId: session.targetDeviceId,
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
        const delivered = this.sendToDevice(upload.targetDeviceId,
            this.uploadPayloadV2(upload, 'upload_start', {
                connectionGeneration: owner.connectionGeneration,
                files: upload.assets,
                totalSize: upload.totalSize,
            }));
        if (!delivered) {
            this.rejectTrackedUploadV2(upload, 'upload_target_unavailable',
                'Upload target is unavailable', false);
        }
    }

    handleUploadResumeV2(senderId, message, transportSocket = null) {
        const upload = this.uploads.get(message.uploadId);
        // Keep the immutable binding even when lease validation synchronously
        // tears the session down and removes it from the live upload map.
        const validated = this.validateUploadPartyV2(senderId, message, 'owner');
        if (!validated.ok || !upload || upload.protocolVersion !== 2
            || upload.remoteSessionId !== message.remoteSessionId
            || upload.ownerDeviceId !== validated.client?.deviceId) {
            const code = validated.ok ? 'unknown_upload' : validated.error;
            return this.sendUploadRejectedV2(senderId, message.uploadId, code, code, upload);
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
        this.sendToDevice(upload.targetDeviceId,
            this.uploadPayloadV2(upload, 'upload_resume', {
                connectionGeneration: validated.client.connectionGeneration,
                assets: this.uploadOffsetsV2(upload),
            }));
        this.sendToDevice(upload.ownerDeviceId,
            this.uploadPayloadV2(upload, 'upload_resume_ready', {
                replay: false,
                assets: this.uploadOffsetsV2(upload),
            }));
    }

    handleUploadReadyV2(targetId, message) {
        const validated = this.validateUploadPartyV2(targetId, message, 'target');
        const upload = this.uploads.get(message.uploadId);
        if (!validated.ok || !upload || upload.protocolVersion !== 2
            || upload.remoteSessionId !== message.remoteSessionId
            || upload.awaitingTargetValidation || !upload.awaitingTargetReady) return;
        if (upload.generation !== validated.session.generation
            || upload.targetDeviceId !== validated.client.deviceId
            || !this.validateUploadInventoryV2(
                upload, message.assets,
                (asset, offset) => offset === asset.durableOffset)) {
            return this.rejectTrackedUploadV2(upload,
                'invalid_upload_ready_inventory');
        }
        upload.awaitingTargetReady = false;
        upload.lastActivity = Date.now();
        this.sendToDevice(upload.ownerDeviceId,
            this.uploadPayloadV2(upload, 'upload_ready', {
                assets: this.uploadOffsetsV2(upload),
            }));
    }

    decodeUploadChunkV2(message) {
        if (typeof message.data !== 'string' || message.data.length < 1
            || message.data.length > this.MAX_UPLOAD_CHUNK_BASE64_LENGTH
            || message.data.length % 4 !== 0
            || !/^[A-Za-z0-9+/]*={0,2}$/.test(message.data)) return null;
        const decoded = Buffer.from(message.data, 'base64');
        if (decoded.length < 1 || decoded.length > 128 * 1024
            || decoded.toString('base64') !== message.data) return null;
        return decoded;
    }

    handleUploadChunkV2(senderId, message, transportSocket = null) {
        const upload = this.uploads.get(message.uploadId);
        const validated = this.validateUploadPartyV2(senderId, message, 'owner');
        if (!validated.ok || !upload || upload.protocolVersion !== 2
            || upload.remoteSessionId !== message.remoteSessionId
            || upload.ownerDeviceId !== validated.client?.deviceId) {
            const code = validated.ok ? 'unknown_upload' : validated.error;
            return this.sendUploadRejectedV2(senderId, message.uploadId, code, code, upload);
        }
        if (upload.transportSocket !== transportSocket) {
            return this.rejectTrackedUploadV2(upload, 'upload_transport_changed');
        }
        const asset = upload.assetStates.get(message.assetId);
        const decoded = this.decodeUploadChunkV2(message);
        if (upload.awaitingTargetReady || upload.awaitingTargetValidation || !asset || !decoded
            || message.offset !== asset.nextOffset || message.size !== decoded.length
            || message.sha256 !== asset.sha256
            || decoded.length > asset.size - asset.nextOffset) {
            return this.rejectTrackedUploadV2(upload, 'invalid_upload_chunk');
        }
        const targetId = this.resolveClientId(upload.targetDeviceId);
        const target = targetId ? this.clients.get(targetId) : null;
        if (!target || !target.ws || target.ws.readyState !== WebSocket.OPEN) return;
        if ((Number(target.ws.bufferedAmount) || 0) > this.MAX_TARGET_BUFFERED_UPLOAD_BYTES) {
            return this.rejectTrackedUploadV2(upload, 'upload_target_backpressure');
        }
        const delivered = this.sendToDevice(upload.targetDeviceId,
            this.uploadPayloadV2(upload, 'upload_chunk', {
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

    handleUploadProgressV2(targetId, message) {
        const validated = this.validateUploadPartyV2(targetId, message, 'target');
        const upload = this.uploads.get(message.uploadId);
        if (!validated.ok || !upload || upload.protocolVersion !== 2
            || upload.remoteSessionId !== message.remoteSessionId
            || !Array.isArray(message.assets)) return;
        if (upload.generation !== validated.session.generation
            || upload.targetDeviceId !== validated.client.deviceId
            || !this.validateUploadInventoryV2(
                upload, message.assets,
                (asset, offset) => offset >= asset.durableOffset
                    && offset <= asset.nextOffset)) {
            return this.rejectTrackedUploadV2(upload,
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
        this.sendToDevice(upload.ownerDeviceId,
            this.uploadPayloadV2(upload, 'upload_progress', {
                durableBytes: upload.durableBytes,
                totalSize: upload.totalSize,
                assets: this.uploadOffsetsV2(upload),
            }));
    }

    handleUploadCompleteV2(senderId, message, transportSocket = null) {
        const upload = this.uploads.get(message.uploadId);
        const validated = this.validateUploadPartyV2(senderId, message, 'owner');
        if (!validated.ok || !upload || upload.protocolVersion !== 2
            || upload.remoteSessionId !== message.remoteSessionId
            || upload.ownerDeviceId !== validated.client?.deviceId) {
            const code = validated.ok ? 'unknown_upload' : validated.error;
            return this.sendUploadRejectedV2(senderId, message.uploadId, code, code, upload);
        }
        if (upload.transportSocket !== transportSocket || upload.awaitingTargetReady) {
            return this.rejectTrackedUploadV2(upload, 'invalid_upload_completion_state');
        }
        if (!this.validateUploadInventoryV2(
            upload, message.assets, (asset, offset) => offset === asset.size)) {
            return this.rejectTrackedUploadV2(
                upload, 'invalid_upload_completion_inventory');
        }
        if (upload.awaitingTargetValidation) return;
        if (upload.relayedBytes !== upload.totalSize
            || Array.from(upload.assetStates.values()).some(asset => asset.nextOffset !== asset.size)) {
            return this.rejectTrackedUploadV2(upload, 'upload_incomplete');
        }
        upload.awaitingTargetValidation = true;
        upload.awaitingTargetValidationSince = Date.now();
        upload.lastActivity = upload.awaitingTargetValidationSince;
        this.sendToDevice(upload.targetDeviceId,
            this.uploadPayloadV2(upload, 'upload_complete', {
                connectionGeneration: validated.client.connectionGeneration,
                assets: this.uploadCompletionInventoryV2(upload),
            }));
    }

    handleUploadFinishedV2(targetId, message) {
        const validated = this.validateUploadPartyV2(targetId, message, 'target');
        const upload = this.uploads.get(message.uploadId);
        if (!validated.ok || !upload || upload.protocolVersion !== 2
            || upload.remoteSessionId !== message.remoteSessionId
            || !upload.awaitingTargetValidation || !Array.isArray(message.assets)) return;
        const acknowledgements = new Map();
        for (const entry of message.assets) {
            if (!isPlainObject(entry) || acknowledgements.has(entry.assetId)) {
                return this.rejectTrackedUploadV2(upload, 'invalid_upload_ack');
            }
            acknowledgements.set(entry.assetId, entry);
        }
        const invalid = acknowledgements.size !== upload.assetStates.size
            || Array.from(upload.assetStates.values()).some(asset => {
                const entry = acknowledgements.get(asset.assetId);
                return !entry || entry.offset !== asset.size || entry.size !== asset.size
                    || entry.sha256 !== asset.sha256;
            });
        if (invalid) return this.rejectTrackedUploadV2(upload, 'invalid_upload_ack');
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
                ownerDeviceId: upload.ownerDeviceId,
                targetDeviceId: upload.targetDeviceId,
                validatedAt: Date.now(),
            });
        }
        const finished = this.uploadPayloadV2(upload, 'upload_finished', {
            assets: upload.assets.map(asset => ({
                assetId: asset.assetId,
                offset: asset.size,
                size: asset.size,
                sha256: asset.sha256,
            })),
        });
        this.rememberUploadResultV2(upload, 'finished', { payload: finished });
        this.removeUploadV2(upload);
        this.sendToDevice(upload.ownerDeviceId, finished);
    }

    handleUploadRejectedV2(targetId, message) {
        const validated = this.validateUploadPartyV2(targetId, message, 'target');
        const upload = this.uploads.get(message.uploadId);
        if (!validated.ok || !upload || upload.protocolVersion !== 2
            || upload.remoteSessionId !== message.remoteSessionId) return;
        this.rejectTrackedUploadV2(upload,
            typeof message.code === 'string' ? message.code.slice(0, 128) : 'upload_target_rejected',
            typeof message.reason === 'string' ? message.reason.slice(0, 512) : '', false);
    }

    handleUploadAbortV2(senderId, message) {
        const validated = this.validateUploadPartyV2(senderId, message, 'owner', true);
        const upload = this.uploads.get(message.uploadId);
        if (!validated.ok || !upload || upload.protocolVersion !== 2
            || upload.remoteSessionId !== message.remoteSessionId) return;
        this.sendToDevice(upload.targetDeviceId,
            this.uploadPayloadV2(upload, 'upload_abort', {
                reason: typeof message.reason === 'string'
                    ? message.reason.slice(0, 128) : 'owner_abort',
            }));
        this.rememberUploadResultV2(upload, 'aborted');
        this.removeUploadV2(upload);
        this.sendToDevice(upload.ownerDeviceId,
            this.uploadPayloadV2(upload, 'upload_aborted', { success: true }));
    }

    handleUploadAbortAcknowledgementV2(targetId, message) {
        // No state is recreated by a late acknowledgement. This is deliberately
        // an idempotent no-op after the server has committed an abort.
        void targetId;
        void message;
    }

    abortUploadsForUploadSocketV2(senderId, socket, reason) {
        void senderId;
        for (const upload of this.uploads.values()) {
            if (!upload || upload.protocolVersion !== 2 || upload.transportSocket !== socket) continue;
            upload.transportSocket = null;
            upload.transportDisconnectedAt = Date.now();
            upload.lastActivity = Date.now();
            upload.pauseReason = String(reason || 'upload_transport_lost').slice(0, 128);
        }
    }

    abortUploadsForClientV2(clientId) {
        const client = this.clients.get(clientId);
        if (!client) return;
        for (const upload of Array.from(this.uploads.values())) {
            if (!upload || upload.protocolVersion !== 2
                || (upload.ownerDeviceId !== client.deviceId
                    && upload.targetDeviceId !== client.deviceId)) continue;
            const peer = upload.ownerDeviceId === client.deviceId
                ? upload.targetDeviceId : upload.ownerDeviceId;
            this.sendToDevice(peer, this.uploadPayloadV2(upload, 'upload_abort', {
                reason: 'remote_session_unavailable',
            }));
            this.removeUploadV2(upload);
        }
    }

    cleanupStalledUploadsV2(now = Date.now()) {
        this.pruneUploadTombstonesV2(now);
        this.sweepAssetRemovalsV2(now);
        for (const upload of Array.from(this.uploads.values())) {
            if (!upload || upload.protocolVersion !== 2) continue;
            const waitingForValidation = upload.awaitingTargetValidation === true;
            const timeout = waitingForValidation
                ? this.UPLOAD_TARGET_ACK_TIMEOUT_MS : this.UPLOAD_TIMEOUT_MS;
            const activity = waitingForValidation
                ? upload.awaitingTargetValidationSince : upload.lastActivity;
            if (now - activity < timeout) continue;
            this.rejectTrackedUploadV2(upload,
                waitingForValidation ? 'upload_validation_timeout' : 'upload_idle_timeout');
        }
        this.sweepSceneRuns(now);
    }

    // Legacy upload implementation retained temporarily as unreachable helper
    // code while non-scene media sharing is migrated. The v2 dispatcher above
    // never routes protocol messages into these methods.
    // PHASE 2: Upload state tracking methods
    abortUploadsForUploadSocket(senderId, socket, reason) {
        if (!senderId || !socket) return;
        for (const [uploadId, upload] of Array.from(this.uploads.entries())) {
            if (!upload || upload.senderSession !== senderId
                || upload.transportSocket !== socket) continue;
            const failure = String(reason || 'Dedicated upload transport was lost');
            const target = this.clients.get(upload.targetSession);
            if (target && target.ws && target.ws.readyState === WebSocket.OPEN) {
                target.ws.send(JSON.stringify({
                    type: 'upload_abort',
                    uploadId,
                    canvasSessionId: upload.canvasSessionId,
                    senderClientId: upload.senderSession,
                    senderPersistentClientId: upload.senderPersistent,
                    protocolRejected: true,
                    reason: failure
                }));
            }
            this.sendUploadRejected(upload.senderSession, uploadId, failure);
            this.uploads.delete(uploadId);
        }
    }

    abortUploadsForClient(clientId) {
        if (!clientId) return;

        for (const [uploadId, upload] of Array.from(this.uploads.entries())) {
            if (!upload) continue;

            if (upload.senderSession === clientId) {
                const target = this.clients.get(upload.targetSession);
                if (target && target.ws && target.ws.readyState === WebSocket.OPEN) {
                    target.ws.send(JSON.stringify({
                        type: 'upload_abort',
                        uploadId,
                        canvasSessionId: upload.canvasSessionId,
                        senderClientId: upload.senderSession,
                        senderPersistentClientId: upload.senderPersistent,
                        protocolRejected: true,
                        reason: 'Upload sender disconnected'
                    }));
                }
                this.uploads.delete(uploadId);
                continue;
            }

            if (upload.targetSession === clientId) {
                this.sendUploadRejected(upload.senderSession, uploadId,
                    'Upload target disconnected');
                this.uploads.delete(uploadId);
            }
        }

        for (const [removalId, removal] of Array.from(this.pendingRemovals.entries())) {
            if (!removal) continue;
            if (removal.senderSession === clientId) {
                this.pendingRemovals.delete(removalId);
            } else if (removal.targetSession === clientId) {
                this.pendingRemovals.delete(removalId);
                this.sendRemovalRejected(removal.senderSession, removalId,
                    'Remote removal target disconnected');
            }
        }
        for (const [uploadId, pending] of Array.from(this.pendingUploadAborts.entries())) {
            if (!pending) continue;
            if (pending.senderSession === clientId) {
                this.pendingUploadAborts.delete(uploadId);
            } else if (pending.targetSession === clientId) {
                this.pendingUploadAborts.delete(uploadId);
                this.relayToSender(clientId, pending.senderSession, {
                    type: 'upload_aborted',
                    uploadId,
                    canvasSessionId: pending.canvasSessionId
                });
            }
        }
    }

    sendUploadRejected(senderSession, uploadId, reason) {
        const sender = this.clients.get(senderSession);
        if (!sender || !sender.ws || sender.ws.readyState !== WebSocket.OPEN) return;
        sender.ws.send(JSON.stringify({
            type: 'upload_rejected',
            uploadId: typeof uploadId === 'string' ? uploadId : '',
            reason: String(reason || 'Upload rejected').slice(0, 512)
        }));
    }

    sendRemovalRejected(senderSession, removalId, reason) {
        const sender = this.clients.get(senderSession);
        if (!sender || !sender.ws || sender.ws.readyState !== WebSocket.OPEN) return;
        sender.ws.send(JSON.stringify({
            type: 'removal_rejected',
            removalId: typeof removalId === 'string' ? removalId : '',
            reason: String(reason || 'Remote removal was rejected').slice(0, 512)
        }));
    }

    isExpectedUploadSender(upload, clientId) {
        return !!upload && upload.senderSession === clientId;
    }

    isExpectedUploadTarget(upload, clientId) {
        return !!upload && upload.targetSession === clientId;
    }

    rejectTrackedUpload(uploadId, reason, notifyTarget = true) {
        const upload = this.uploads.get(uploadId);
        if (!upload) return;

        if (notifyTarget) {
            const target = this.clients.get(upload.targetSession);
            if (target && target.ws && target.ws.readyState === WebSocket.OPEN) {
                target.ws.send(JSON.stringify({
                    type: 'upload_abort',
                    uploadId,
                    canvasSessionId: upload.canvasSessionId,
                    senderClientId: upload.senderSession,
                    senderPersistentClientId: upload.senderPersistent,
                    protocolRejected: true,
                    reason: String(reason || 'Upload rejected').slice(0, 512)
                }));
            }
        }

        this.sendUploadRejected(upload.senderSession, uploadId, reason);
        this.uploads.delete(uploadId);
    }

    rejectInvalidFinishedAcknowledgement(uploadId, reason) {
        const upload = this.uploads.get(uploadId);
        if (!upload) return;
        const target = this.clients.get(upload.targetSession);
        if (target && target.ws && target.ws.readyState === WebSocket.OPEN) {
            for (const fileId of upload.files) {
                target.ws.send(JSON.stringify({
                    type: 'remove_file',
                    fileId,
                    canvasSessionId: upload.canvasSessionId,
                    senderClientId: upload.senderSession
                }));
            }
        }
        this.sendUploadRejected(upload.senderSession, uploadId, reason);
        this.uploads.delete(uploadId);
    }

    handleUploadStart(senderId, message, transportSocket = null) {
        const targetClientId = message.targetPersistentClientId || message.targetClientId;
        const { uploadId, canvasSessionId, files } = message;

        const uuidPattern = /^[0-9a-f]{8}-[0-9a-f]{4}-[1-5][0-9a-f]{3}-[89ab][0-9a-f]{3}-[0-9a-f]{12}$/i;
        const fileIdPattern = /^[0-9a-f]{64}$/;
        const extensionPattern = /^[a-z0-9]{1,16}$/;
        const canvasPattern = /^[A-Za-z0-9_-]{1,512}$/;
        const resolvedTarget = typeof targetClientId === 'string' ? this.resolveClientId(targetClientId) : null;

        const rejectStart = reason => {
            console.warn(`⚠️ Rejecting upload_start ${uploadId || '<missing>'} from ${senderId}: ${reason}`);
            this.sendUploadRejected(senderId, uploadId, reason);
        };

        if (!resolvedTarget || !uuidPattern.test(uploadId || '')
            || !canvasPattern.test(canvasSessionId || '')
            || !Array.isArray(files) || files.length < 1 || files.length > this.MAX_UPLOAD_FILES) {
            rejectStart('Invalid upload identifiers, target, or file count');
            return;
        }
        const targetPersistentId = this.getPersistentId(resolvedTarget);
        const senderPersistentId = this.getPersistentId(senderId);
        if (this.uploads.has(uploadId) || this.pendingUploadAborts.has(uploadId)) {
            rejectStart('Upload identifier is already active');
            return;
        }
        for (const pending of this.pendingUploadAborts.values()) {
            if (pending && (pending.senderPersistent === senderPersistentId
                || pending.targetSession === resolvedTarget)) {
                rejectStart('Previous upload cancellation cleanup is still pending');
                return;
            }
        }
        for (const removal of this.pendingRemovals.values()) {
            const sameNamespace = removal
                && removal.senderPersistent === senderPersistentId
                && removal.targetPersistent === targetPersistentId;
            const overlappingCanvas = sameNamespace
                && (removal.canvasSessionId === 'default'
                    || removal.canvasSessionId === canvasSessionId);
            if (overlappingCanvas) {
                rejectStart('Remote removal for this canvas is still pending');
                return;
            }
        }
        for (const upload of this.uploads.values()) {
            if (upload && upload.senderSession === senderId) {
                rejectStart('Another upload from this sender is already active');
                return;
            }
            if (upload && upload.targetSession === resolvedTarget) {
                rejectStart('Remote client is already receiving another upload');
                return;
            }
        }

        const fileIds = [];
        const seenFileIds = new Set();
        const seenMediaIds = new Set();
        const fileStates = new Map();
        let totalSize = 0;
        for (const file of files) {
            if (!file || typeof file !== 'object' || Array.isArray(file)
                || typeof file.fileId !== 'string' || !fileIdPattern.test(file.fileId)
                || seenFileIds.has(file.fileId)
                || typeof file.name !== 'string' || file.name.length < 1 || file.name.length > 255
                || /[\\/\x00-\x1f\x7f]/.test(file.name)
                || typeof file.extension !== 'string' || file.extension !== file.extension.trim()
                || (file.extension.length > 0 && !extensionPattern.test(file.extension.toLowerCase()))
                || !Number.isSafeInteger(file.sizeBytes) || file.sizeBytes < 1
                || file.sizeBytes > this.MAX_UPLOAD_FILE_BYTES
                || !Array.isArray(file.mediaIds) || file.mediaIds.length < 1 || file.mediaIds.length > 4096) {
                rejectStart('Upload manifest contains invalid file metadata');
                return;
            }

            const lastDot = file.name.lastIndexOf('.');
            const filenameExtension = lastDot > 0 && lastDot < file.name.length - 1
                ? file.name.slice(lastDot + 1).toLowerCase() : '';
            if (filenameExtension !== file.extension.toLowerCase()) {
                rejectStart('Upload filename and extension do not match');
                return;
            }
            for (const mediaId of file.mediaIds) {
                if (typeof mediaId !== 'string' || !uuidPattern.test(mediaId)
                    || seenMediaIds.has(mediaId)) {
                    rejectStart('Upload manifest contains an invalid or duplicate media identifier');
                    return;
                }
                seenMediaIds.add(mediaId);
            }
            if (totalSize > this.MAX_UPLOAD_TOTAL_BYTES - file.sizeBytes) {
                rejectStart('Upload manifest exceeds the total size limit');
                return;
            }
            totalSize += file.sizeBytes;
            seenFileIds.add(file.fileId);
            fileIds.push(file.fileId);
            fileStates.set(file.fileId, {
                sizeBytes: file.sizeBytes,
                receivedBytes: 0,
                nextChunkIndex: 0
            });
        }

        console.log(`📤 Upload started: ${senderPersistentId}/${senderId} -> ${targetPersistentId}/${targetClientId} [${uploadId}] directional-idea:${canvasSessionId}`);

        const startedAt = Date.now();
        this.uploads.set(uploadId, {
            senderSession: senderId,
            senderPersistent: senderPersistentId,
            targetSession: resolvedTarget,
            targetPersistent: targetPersistentId,
            canvasSessionId,
            startTime: startedAt,
            lastActivity: startedAt,
            files: fileIds,
            fileSet: seenFileIds,
            fileStates,
            totalSize,
            relayedBytes: 0,
            receivedBytes: 0,
            lastTargetPercent: 0,
            awaitingTargetReady: true,
            awaitingTargetValidation: false,
            transportSocket
        });

        console.log(`   Files: ${fileIds.length} file(s)`);
        const relayed = { ...message, senderClientId: senderId };
        if (!this.relayToTarget(senderId, resolvedTarget, relayed)) {
            this.sendUploadRejected(senderId, uploadId, 'Upload target is unavailable');
            this.uploads.delete(uploadId);
        }
    }

    handleUploadChunk(senderId, message, transportSocket = null) {
        const uploadId = message.uploadId;
        const upload = this.uploads.get(uploadId);
        if (!upload) {
            this.sendUploadRejected(senderId, uploadId, 'Unknown or closed upload session');
            return;
        }
        if (!this.isExpectedUploadSender(upload, senderId)) {
            this.sendUploadRejected(senderId, uploadId, 'Upload sender does not own this session');
            return;
        }
        if (upload.transportSocket !== transportSocket) {
            this.rejectTrackedUpload(uploadId, 'Upload transport changed during transfer');
            return;
        }
        if (upload.awaitingTargetReady || upload.awaitingTargetValidation
            || message.canvasSessionId !== upload.canvasSessionId
            || typeof message.fileId !== 'string' || !upload.fileSet.has(message.fileId)
            || !Number.isInteger(message.chunkIndex) || message.chunkIndex < 0
            || typeof message.data !== 'string' || message.data.length < 1
            || message.data.length > this.MAX_UPLOAD_CHUNK_BASE64_LENGTH) {
            this.rejectTrackedUpload(uploadId, 'Invalid upload chunk');
            return;
        }

        const fileState = upload.fileStates.get(message.fileId);
        const encodedData = message.data;
        let decodedData;
        try {
            decodedData = Buffer.from(encodedData, 'base64');
        } catch (_error) {
            decodedData = null;
        }
        if (!fileState || message.chunkIndex !== fileState.nextChunkIndex
            || encodedData.length % 4 !== 0
            || !/^[A-Za-z0-9+/]*={0,2}$/.test(encodedData)
            || !decodedData || decodedData.length < 1 || decodedData.length > 128 * 1024
            || decodedData.toString('base64') !== encodedData
            || decodedData.length > fileState.sizeBytes - fileState.receivedBytes) {
            this.rejectTrackedUpload(uploadId,
                'Upload chunks are out of order or exceed the declared file size');
            return;
        }

        const target = this.clients.get(upload.targetSession);
        if (!target || !target.ws || target.ws.readyState !== WebSocket.OPEN) {
            this.rejectTrackedUpload(uploadId, 'Upload target disconnected', false);
            return;
        }
        if (target.ws.bufferedAmount > this.MAX_TARGET_BUFFERED_UPLOAD_BYTES) {
            this.rejectTrackedUpload(uploadId,
                'Upload target is not consuming data fast enough');
            return;
        }

        const relayed = {
            ...message,
            senderClientId: upload.senderSession,
            targetClientId: upload.targetPersistent,
            targetPersistentClientId: upload.targetPersistent
        };
        upload.lastActivity = Date.now();
        if (!this.relayToTarget(upload.senderSession, upload.targetSession, relayed)) {
            this.rejectTrackedUpload(uploadId, 'Upload relay failed', false);
            return;
        }
        fileState.receivedBytes += decodedData.length;
        fileState.nextChunkIndex++;
        upload.relayedBytes += decodedData.length;
    }

    handleUploadComplete(senderId, message, transportSocket = null) {
        const { uploadId, canvasSessionId } = message;
        const upload = this.uploads.get(uploadId);

        if (!upload) {
            console.warn(`⚠️ upload_complete for unknown uploadId: ${uploadId}`);
            this.sendUploadRejected(senderId, uploadId, 'Unknown or closed upload session');
            return;
        }
        if (!this.isExpectedUploadSender(upload, senderId)) {
            this.sendUploadRejected(senderId, uploadId, 'Upload sender does not own this session');
            return;
        }
        if (upload.transportSocket !== transportSocket) {
            this.rejectTrackedUpload(uploadId, 'Upload transport changed before completion');
            return;
        }
        if (upload.awaitingTargetReady || canvasSessionId !== upload.canvasSessionId) {
            this.rejectTrackedUpload(uploadId, 'Upload session identifier mismatch');
            return;
        }
        if (upload.awaitingTargetValidation) {
            return;
        }
        if (upload.relayedBytes !== upload.totalSize
            || [...upload.fileStates.values()].some(file =>
                file.receivedBytes !== file.sizeBytes)) {
            this.rejectTrackedUpload(uploadId,
                'Upload completed before every declared byte was relayed');
            return;
        }
        upload.awaitingTargetValidation = true;
        upload.lastActivity = Date.now();
        upload.awaitingTargetValidationSince = upload.lastActivity;
        const relayed = {
            ...message,
            senderClientId: upload.senderSession,
            targetClientId: upload.targetPersistent,
            targetPersistentClientId: upload.targetPersistent
        };
        if (!this.relayToTarget(upload.senderSession, upload.targetSession, relayed)) {
            this.rejectTrackedUpload(uploadId,
                'Upload target disconnected before validation', false);
        }
    }

    handleUploadReady(targetId, message) {
        const upload = this.uploads.get(message.uploadId);
        if (!upload || !this.isExpectedUploadTarget(upload, targetId)
            || message.canvasSessionId !== upload.canvasSessionId) return;
        if (upload.awaitingTargetValidation || !upload.awaitingTargetReady) return;

        upload.awaitingTargetReady = false;
        upload.lastActivity = Date.now();
        this.relayToSender(targetId, upload.senderSession, {
            type: 'upload_ready',
            uploadId: message.uploadId,
            canvasSessionId: upload.canvasSessionId
        });
    }

    handleUploadProgress(targetId, message) {
        const upload = this.uploads.get(message.uploadId);
        if (!upload || !this.isExpectedUploadTarget(upload, targetId)) return;
        let madeProgress = false;

        // Compatibility with clients predating the explicit upload_ready event:
        // their initial zero-progress acknowledgement also proves staging is ready.
        if (upload.awaitingTargetReady && !upload.awaitingTargetValidation) {
            upload.awaitingTargetReady = false;
            madeProgress = true;
            this.relayToSender(targetId, upload.senderSession, {
                type: 'upload_ready',
                uploadId: message.uploadId,
                canvasSessionId: upload.canvasSessionId
            });
        }

        const percent = Number.isInteger(message.percent) ? Math.max(0, Math.min(99, message.percent)) : 0;
        if (percent > upload.lastTargetPercent) {
            upload.lastTargetPercent = percent;
            madeProgress = true;
        }
        if (Number.isSafeInteger(message.receivedBytes)
            && message.receivedBytes > upload.receivedBytes
            && message.receivedBytes <= upload.relayedBytes) {
            upload.receivedBytes = message.receivedBytes;
            madeProgress = true;
        }
        if (madeProgress) upload.lastActivity = Date.now();
        const perFileProgress = [];
        if (Array.isArray(message.perFileProgress)) {
            for (const entry of message.perFileProgress) {
                if (!entry || typeof entry !== 'object' || !upload.fileSet.has(entry.fileId)
                    || !Number.isInteger(entry.percent)) continue;
                perFileProgress.push({
                    fileId: entry.fileId,
                    percent: Math.max(0, Math.min(99, entry.percent))
                });
            }
        }
        this.relayToSender(targetId, upload.senderSession, {
            type: 'upload_progress',
            uploadId: message.uploadId,
            percent,
            filesCompleted: 0,
            totalFiles: upload.files.length,
            receivedBytes: upload.receivedBytes,
            perFileProgress
        });
    }

    handleUploadFinished(targetId, message) {
        const uploadId = message.uploadId;
        const upload = this.uploads.get(uploadId);
        if (!upload || !this.isExpectedUploadTarget(upload, targetId)) {
            this.sendError(targetId, 'Invalid upload_finished acknowledgement');
            return;
        }
        if (!upload.awaitingTargetValidation) {
            this.rejectTrackedUpload(uploadId, 'Target acknowledged the upload before completion');
            return;
        }
        if (message.canvasSessionId !== upload.canvasSessionId
            || !Array.isArray(message.fileIds)) {
            this.rejectInvalidFinishedAcknowledgement(uploadId,
                'Target returned an invalid upload acknowledgement');
            return;
        }

        const acknowledged = new Set(message.fileIds);
        if (acknowledged.size !== message.fileIds.length
            || acknowledged.size !== upload.fileSet.size
            || [...upload.fileSet].some(fileId => !acknowledged.has(fileId))) {
            this.rejectInvalidFinishedAcknowledgement(uploadId,
                'Target acknowledgement does not match the upload manifest');
            return;
        }

        if (!this.clientFiles.has(upload.targetPersistent)) {
            this.clientFiles.set(upload.targetPersistent, new Map());
        }
        const targetIdeas = this.clientFiles.get(upload.targetPersistent);
        if (!targetIdeas.has(upload.canvasSessionId)) {
            targetIdeas.set(upload.canvasSessionId, new Set());
        }
        const ideaFiles = targetIdeas.get(upload.canvasSessionId);
        upload.files.forEach(fileId => ideaFiles.add(fileId));

        if (!this.clientFileOwners.has(upload.targetPersistent)) {
            this.clientFileOwners.set(upload.targetPersistent, new Map());
        }
        const targetOwners = this.clientFileOwners.get(upload.targetPersistent);
        if (!targetOwners.has(upload.canvasSessionId)) {
            targetOwners.set(upload.canvasSessionId, new Map());
        }
        const ideaOwners = targetOwners.get(upload.canvasSessionId);
        upload.files.forEach(fileId => ideaOwners.set(fileId, upload.senderPersistent));

        if (!this.clientFileGenerations.has(upload.targetPersistent)) {
            this.clientFileGenerations.set(upload.targetPersistent, new Map());
        }
        const targetGenerations = this.clientFileGenerations.get(upload.targetPersistent);
        if (!targetGenerations.has(upload.canvasSessionId)) {
            targetGenerations.set(upload.canvasSessionId, new Map());
        }
        const ideaGenerations = targetGenerations.get(upload.canvasSessionId);
        upload.files.forEach(fileId => ideaGenerations.set(fileId, uploadId));

        const duration = ((Date.now() - upload.startTime) / 1000).toFixed(1);
        console.log(`✅ Upload validated: ${uploadId} (${duration}s) - ${upload.files.length} files to ${upload.targetPersistent}:${upload.canvasSessionId}`);
        this.uploads.delete(uploadId);
        this.relayToSender(targetId, upload.senderSession, {
            type: 'upload_finished',
            uploadId,
            canvasSessionId: upload.canvasSessionId,
            fileIds: upload.files
        });
    }

    handleUploadRejected(targetId, message) {
        const uploadId = message.uploadId;
        const upload = this.uploads.get(uploadId);
        if (!upload || !this.isExpectedUploadTarget(upload, targetId)
            || (message.canvasSessionId !== undefined
                && message.canvasSessionId !== upload.canvasSessionId)) return;

        const reason = typeof message.reason === 'string' && message.reason.trim()
            ? message.reason.trim().slice(0, 512) : 'Remote client rejected the upload';
        this.uploads.delete(uploadId);
        this.relayToSender(targetId, upload.senderSession, {
            type: 'upload_rejected',
            uploadId,
            reason,
            canvasSessionId: upload.canvasSessionId
        });
    }

    handleUploadAbort(senderId, message) {
        const { uploadId } = message;
        const upload = this.uploads.get(uploadId);
        if (!upload || !this.isExpectedUploadSender(upload, senderId)) {
            return;
        }
        console.log(`❌ Upload aborted: ${uploadId}`);
        this.uploads.delete(uploadId);
        const abortMessage = {
            ...message,
            type: 'upload_abort',
            senderClientId: upload.senderSession,
            senderPersistentClientId: upload.senderPersistent,
            canvasSessionId: upload.canvasSessionId
        };
        this.pendingUploadAborts.set(uploadId, {
            senderSession: upload.senderSession,
            senderPersistent: upload.senderPersistent,
            targetSession: upload.targetSession,
            canvasSessionId: upload.canvasSessionId,
            abortMessage,
            createdAt: Date.now(),
            lastSentAt: Date.now(),
            attempts: 1
        });
        if (!this.relayToTarget(senderId, upload.targetSession, abortMessage)) {
            // A disconnected target runs its own connection-loss cleanup, so no
            // partial staging can remain usable. Complete the correlated cancel.
            this.pendingUploadAborts.delete(uploadId);
            this.relayToSender(upload.targetSession, upload.senderSession, {
                type: 'upload_aborted',
                uploadId,
                canvasSessionId: upload.canvasSessionId
            });
        }
    }

    handleUploadAbortAcknowledgement(targetId, message) {
        const pending = this.pendingUploadAborts.get(message.uploadId);
        // A retried abort can produce a second acknowledgement after the first
        // one committed. Treat that as an idempotent no-op.
        if (!pending) return;
        if (pending.targetSession !== targetId
            || pending.senderPersistent !== message.senderClientId
            || pending.canvasSessionId !== message.canvasSessionId) {
            return this.sendError(targetId, 'Invalid upload abort acknowledgement');
        }
        this.pendingUploadAborts.delete(message.uploadId);
        this.relayToSender(targetId, pending.senderSession, {
            type: 'upload_aborted',
            uploadId: message.uploadId,
            canvasSessionId: pending.canvasSessionId
        });
    }
    
    // PHASE 1: Cleanup stalled uploads (called periodically)
    cleanupStalledUploads() {
        const now = Date.now();
        let cleanedCount = 0;
        
        for (const [uploadId, upload] of this.uploads) {
            const awaitingTarget = upload.awaitingTargetValidation === true;
            const timeoutMs = awaitingTarget
                ? this.UPLOAD_TARGET_ACK_TIMEOUT_MS : this.UPLOAD_TIMEOUT_MS;
            const activityAt = awaitingTarget
                ? (upload.awaitingTargetValidationSince || upload.lastActivity || upload.startTime)
                : (upload.lastActivity || upload.startTime);
            const age = now - activityAt;
            if (age > timeoutMs) {
                const reason = awaitingTarget
                    ? 'Target validation acknowledgement timed out'
                    : 'Upload timed out';
                console.warn(`⏱️  ${reason}: ${uploadId} after ${(age / 1000).toFixed(1)}s`);

                const target = this.clients.get(upload.targetSession);
                if (target && target.ws && target.ws.readyState === WebSocket.OPEN) {
                    target.ws.send(JSON.stringify({
                        type: 'upload_abort',
                        uploadId,
                        canvasSessionId: upload.canvasSessionId,
                        senderClientId: upload.senderSession,
                        senderPersistentClientId: upload.senderPersistent,
                        protocolRejected: true,
                        reason
                    }));
                }
                this.sendUploadRejected(upload.senderSession, uploadId,
                    `${reason} after ${timeoutMs / 1000}s`);
                this.uploads.delete(uploadId);
                cleanedCount++;
            }
        }
        
        if (cleanedCount > 0) {
            console.log(`🧹 Cleaned up ${cleanedCount} stalled upload(s)`);
        }

        for (const [uploadId, pending] of Array.from(this.pendingUploadAborts.entries())) {
            if (!pending || now - pending.createdAt > this.UPLOAD_TIMEOUT_MS) {
                this.pendingUploadAborts.delete(uploadId);
                continue;
            }
            if (now - pending.lastSentAt >= 2000 && pending.attempts < 5) {
                pending.lastSentAt = now;
                pending.attempts++;
                this.relayToTarget(pending.senderSession, pending.targetSession,
                    { ...pending.abortMessage });
            }
        }

        for (const [removalId, removal] of this.pendingRemovals) {
            if (!removal || now - removal.createdAt > this.REMOVAL_ACK_TIMEOUT_MS) {
                if (removal) {
                    this.sendRemovalRejected(removal.senderSession, removalId,
                        'Remote removal confirmation timed out');
                }
                this.pendingRemovals.delete(removalId);
            }
        }

        this.sweepSceneRuns(now);
    }
    
    handleRemoveAllFiles(senderId, message) {
        // PHASE 2: Extract targetClientId with fallback
        const targetClientId = message.targetPersistentClientId || message.targetClientId;
        const { canvasSessionId, removalId } = message;
        if (!targetClientId || typeof canvasSessionId !== 'string'
            || !CANVAS_SESSION_ID_PATTERN.test(canvasSessionId)
            || typeof removalId !== 'string' || !CANONICAL_UUID_PATTERN.test(removalId)) {
            console.warn(`⚠️ remove_all_files missing or invalid required fields from ${senderId}`);
            return this.sendError(senderId,
                'Missing or invalid targetClientId, canvasSessionId, or removalId');
        }
        if (this.pendingRemovals.has(removalId)) {
            return this.sendRemovalRejected(senderId, removalId,
                'Removal identifier is already active');
        }
        if (this.pendingRemovals.size >= this.MAX_PENDING_REMOVALS) {
            return this.sendRemovalRejected(senderId, removalId,
                'Server has too many pending removal transactions');
        }

        const resolvedTarget = this.resolveClientId(targetClientId);
        if (!resolvedTarget) {
            return this.sendRemovalRejected(senderId, removalId,
                'Target client not found');
        }
        const targetPersistentId = this.getPersistentId(resolvedTarget);
        const targetFiles = this.clientFiles.get(targetPersistentId);
        const targetOwners = this.clientFileOwners.get(targetPersistentId);
        const targetGenerations = this.clientFileGenerations.get(targetPersistentId);
        const senderPersistentId = this.getPersistentId(senderId);
        const entries = [];

        for (const removal of this.pendingRemovals.values()) {
            if (removal && removal.senderPersistent === senderPersistentId) {
                return this.sendRemovalRejected(senderId, removalId,
                    'Another remote removal from this sender is still pending');
            }
        }
        for (const upload of this.uploads.values()) {
            const sameNamespace = upload
                && upload.senderPersistent === senderPersistentId
                && upload.targetPersistent === targetPersistentId;
            const overlappingCanvas = sameNamespace
                && (canvasSessionId === 'default'
                    || upload.canvasSessionId === canvasSessionId);
            if (overlappingCanvas) {
                return this.sendRemovalRejected(senderId, removalId,
                    'Cannot remove files while their upload is active');
            }
        }

        // DEFAULT means the target will remove the authenticated sender's whole
        // cache root, so mirror that operation across all of that sender's
        // inventory entries. A scoped request only affects its named canvas.
        const canvasIds = canvasSessionId === 'default' && targetOwners
            ? Array.from(targetOwners.keys()) : [canvasSessionId];
        for (const candidateCanvasId of canvasIds) {
            const ideaFiles = targetFiles && targetFiles.get(candidateCanvasId);
            const ideaOwners = targetOwners && targetOwners.get(candidateCanvasId);
            const ideaGenerations = targetGenerations && targetGenerations.get(candidateCanvasId);
            if (!ideaFiles || !ideaOwners || !ideaGenerations) continue;

            for (const [fileId, ownerId] of Array.from(ideaOwners.entries())) {
                if (ownerId !== senderPersistentId) continue;
                const generation = ideaGenerations.get(fileId);
                if (!ideaFiles.has(fileId) || typeof generation !== 'string') continue;
                entries.push({
                    canvasSessionId: candidateCanvasId,
                    fileId,
                    ownerPersistent: ownerId,
                    generation
                });
            }
        }

        this.pendingRemovals.set(removalId, {
            senderSession: senderId,
            senderPersistent: senderPersistentId,
            targetSession: resolvedTarget,
            targetPersistent: targetPersistentId,
            canvasSessionId,
            entries,
            createdAt: Date.now()
        });
        console.log(`🗑️  Requested idempotent removal of ${entries.length} tracked file(s) from ${targetPersistentId}:${canvasSessionId}`);
        if (!this.relayToTarget(senderId, resolvedTarget, message)) {
            this.pendingRemovals.delete(removalId);
            this.sendRemovalRejected(senderId, removalId,
                'Remote client is unavailable for removal');
        }
    }

    handleAllFilesRemoved(targetId, message) {
        const { removalId, canvasSessionId } = message;
        const removal = typeof removalId === 'string'
            ? this.pendingRemovals.get(removalId) : null;
        if (!removal || removal.targetSession !== targetId
            || removal.senderPersistent !== message.senderClientId
            || removal.canvasSessionId !== canvasSessionId) {
            console.warn(`⚠️ Ignoring mismatched all_files_removed acknowledgement from ${targetId}`);
            return this.sendError(targetId, 'Invalid removal acknowledgement');
        }

        this.pendingRemovals.delete(removalId);
        const targetFiles = this.clientFiles.get(removal.targetPersistent);
        const targetOwners = this.clientFileOwners.get(removal.targetPersistent);
        const targetGenerations = this.clientFileGenerations.get(removal.targetPersistent);
        for (const entry of removal.entries) {
            const ideaFiles = targetFiles && targetFiles.get(entry.canvasSessionId);
            const ideaOwners = targetOwners && targetOwners.get(entry.canvasSessionId);
            const ideaGenerations = targetGenerations
                && targetGenerations.get(entry.canvasSessionId);
            if (!ideaFiles || !ideaOwners || !ideaGenerations
                || ideaOwners.get(entry.fileId) !== entry.ownerPersistent
                || ideaGenerations.get(entry.fileId) !== entry.generation) {
                continue;
            }

            ideaFiles.delete(entry.fileId);
            ideaOwners.delete(entry.fileId);
            ideaGenerations.delete(entry.fileId);
            if (ideaFiles.size === 0) targetFiles.delete(entry.canvasSessionId);
            if (ideaOwners.size === 0) targetOwners.delete(entry.canvasSessionId);
            if (ideaGenerations.size === 0) targetGenerations.delete(entry.canvasSessionId);
        }
        if (targetFiles && targetFiles.size === 0) {
            this.clientFiles.delete(removal.targetPersistent);
        }
        if (targetOwners && targetOwners.size === 0) {
            this.clientFileOwners.delete(removal.targetPersistent);
        }
        if (targetGenerations && targetGenerations.size === 0) {
            this.clientFileGenerations.delete(removal.targetPersistent);
        }
        this.relayToSender(targetId, removal.senderSession, {
            type: 'all_files_removed',
            removalId,
            canvasSessionId: removal.canvasSessionId,
            targetClientId: removal.targetPersistent,
            targetPersistentClientId: removal.targetPersistent
        });
    }

    handleAllFilesRemovalFailed(targetId, message) {
        const removal = typeof message.removalId === 'string'
            ? this.pendingRemovals.get(message.removalId) : null;
        if (!removal || removal.targetSession !== targetId
            || removal.senderPersistent !== message.senderClientId
            || removal.canvasSessionId !== message.canvasSessionId) {
            console.warn(`⚠️ Ignoring mismatched removal failure from ${targetId}`);
            return this.sendError(targetId, 'Invalid removal failure acknowledgement');
        }

        this.pendingRemovals.delete(message.removalId);
        const reason = typeof message.reason === 'string' && message.reason.trim()
            ? message.reason.trim().slice(0, 512)
            : 'Remote client could not remove every file';
        this.sendRemovalRejected(removal.senderSession, message.removalId, reason);
    }
    
    handleRemoveFile(senderId, message) {
        // PHASE 2: Extract targetClientId with fallback
        const targetClientId = message.targetPersistentClientId || message.targetClientId;
        const { canvasSessionId, fileId } = message;
        
        if (!targetClientId || !fileId || !canvasSessionId) {
            console.warn(`⚠️ remove_file missing required fields from ${senderId}`);
            return this.sendError(senderId, 'Missing targetClientId, canvasSessionId, or fileId');
        }
        
        const resolvedTarget = this.resolveClientId(targetClientId);
        if (!resolvedTarget) {
            return this.sendError(senderId, 'Target client not found');
        }
        const targetPersistentId = this.getPersistentId(resolvedTarget);
        const targetFiles = this.clientFiles.get(targetPersistentId);
        const targetOwners = this.clientFileOwners.get(targetPersistentId);
        const targetGenerations = this.clientFileGenerations.get(targetPersistentId);
        const senderPersistentId = this.getPersistentId(senderId);
        const ideaFiles = targetFiles && targetFiles.get(canvasSessionId);
        const ideaOwners = targetOwners && targetOwners.get(canvasSessionId);
        if (!ideaFiles || !ideaFiles.has(fileId)
            || !ideaOwners || ideaOwners.get(fileId) !== senderPersistentId) {
            console.warn(`⚠️ Unauthorized remove_file from ${senderId} for ${targetPersistentId}:${canvasSessionId}/${fileId}`);
            return this.sendError(senderId, 'Not authorized to remove this file');
        }

        ideaFiles.delete(fileId);
        ideaOwners.delete(fileId);
        const ideaGenerations = targetGenerations && targetGenerations.get(canvasSessionId);
        if (ideaGenerations) ideaGenerations.delete(fileId);
        if (ideaFiles.size === 0) targetFiles.delete(canvasSessionId);
        if (ideaOwners.size === 0) targetOwners.delete(canvasSessionId);
        if (ideaGenerations && ideaGenerations.size === 0) {
            targetGenerations.delete(canvasSessionId);
        }
        if (targetFiles.size === 0) this.clientFiles.delete(targetPersistentId);
        if (targetOwners.size === 0) this.clientFileOwners.delete(targetPersistentId);
        if (targetGenerations && targetGenerations.size === 0) {
            this.clientFileGenerations.delete(targetPersistentId);
        }
        console.log(`🗑️  Removed owned file ${fileId} from ${targetPersistentId}:${canvasSessionId}`);
        this.relayToTarget(senderId, resolvedTarget, message);
    }
    
    // PHASE 2: Canvas lifecycle tracking handlers
    handleCanvasCreated(senderId, message) {
        const { persistentClientId, canvasSessionId } = message;
        
        if (!persistentClientId || !canvasSessionId) {
            console.warn(`⚠️ canvas_created missing required fields from ${senderId}`);
            return this.sendError(senderId, 'Missing persistentClientId or canvasSessionId');
        }
        
        // Track active canvas
        if (!this.activeCanvases.has(persistentClientId)) {
            this.activeCanvases.set(persistentClientId, new Set());
        }
        this.activeCanvases.get(persistentClientId).add(canvasSessionId);
        
        console.log(`🎨 Canvas created: ${persistentClientId}:${canvasSessionId}`);
        console.log(`   Active canvases for ${persistentClientId}:`, Array.from(this.activeCanvases.get(persistentClientId)));
    }
    
    handleCanvasDeleted(senderId, message) {
        const { persistentClientId, canvasSessionId } = message;
        
        if (!persistentClientId || !canvasSessionId) {
            console.warn(`⚠️ canvas_deleted missing required fields from ${senderId}`);
            return this.sendError(senderId, 'Missing persistentClientId or canvasSessionId');
        }
        
        // Remove canvas from tracking
        const canvases = this.activeCanvases.get(persistentClientId);
        if (canvases) {
            canvases.delete(canvasSessionId);
            console.log(`🗑️  Canvas deleted: ${persistentClientId}:${canvasSessionId}`);
            
            // Cleanup empty sets
            if (canvases.size === 0) {
                this.activeCanvases.delete(persistentClientId);
            }
        }
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
                for (const field of ['requestId', 'remoteSessionId', 'targetDeviceId']) {
                    if (typeof correlation[field] === 'string'
                        && correlation[field].length <= 128) {
                        payload[field] = correlation[field];
                    }
                }
            }
            client.ws.send(JSON.stringify(payload));
        }
    }
    
    handleMediaShare(senderId, message) {
        const targetClient = this.clients.get(message.targetClientId);
        if (!targetClient) {
            const senderClient = this.clients.get(senderId);
            if (senderClient) {
                senderClient.ws.send(JSON.stringify({
                    type: 'error',
                    message: 'Target client not found'
                }));
            }
            return;
        }
        
        console.log(`🎬 Media share from ${senderId} to ${message.targetClientId}`);
        
        // Forward media share to target client
        targetClient.ws.send(JSON.stringify({
            type: 'incoming_media',
            senderId: senderId,
            mediaData: message.mediaData,
            screens: message.screens
        }));
        
        // Confirm to sender
        const senderClient = this.clients.get(senderId);
        if (senderClient) {
            senderClient.ws.send(JSON.stringify({
                type: 'share_initiated',
                targetClientId: message.targetClientId
            }));
        }
    }
    
    handleMediaUpdate(senderId, message) {
        const targetClient = this.clients.get(message.targetClientId);
        if (!targetClient) return;
        
        // Forward real-time updates to target client
        targetClient.ws.send(JSON.stringify({
            type: 'media_update',
            senderId: senderId,
            updates: message.updates
        }));
    }
    
    handleStopSharing(senderId, message) {
        const targetClient = this.clients.get(message.targetClientId);
        if (!targetClient) return;
        
        console.log(`🛑 Stop sharing from ${senderId} to ${message.targetClientId}`);
        
        // Tell target client to stop displaying media
        targetClient.ws.send(JSON.stringify({
            type: 'stop_media',
            senderId: senderId
        }));
    }

    handleCursorUpdate(targetId, message) {
        // Forward current cursor position from target to all watchers
        const watchers = this.watchersByTarget.get(targetId);
        if (!watchers || watchers.size === 0) return;
        const x = typeof message.x === 'number' ? Math.round(message.x) : null;
        const y = typeof message.y === 'number' ? Math.round(message.y) : null;
        if (x === null || y === null) return;
        const screenId = Number.isInteger(message.screenId) ? message.screenId : -1;
        const normalizedX = (typeof message.normalizedX === 'number' && Number.isFinite(message.normalizedX))
            ? Math.max(0, Math.min(1, message.normalizedX))
            : null;
        const normalizedY = (typeof message.normalizedY === 'number' && Number.isFinite(message.normalizedY))
            ? Math.max(0, Math.min(1, message.normalizedY))
            : null;
        if (CURSOR_DEBUG) {
            console.log('[CursorDebug][Server][Recv]', {
                targetId,
                watchers: watchers.size,
                x,
                y,
                screenId,
                normalizedX,
                normalizedY,
            });
        }
        for (const watcherId of watchers) {
            const watcher = this.clients.get(watcherId);
            if (!watcher || !watcher.ws) continue;
            const payload = {
                type: 'cursor_update',
                targetClientId: targetId,
                x, y
            };
            if (screenId >= 0 && normalizedX !== null && normalizedY !== null) {
                payload.screenId = screenId;
                payload.normalizedX = normalizedX;
                payload.normalizedY = normalizedY;
            }
            if (CURSOR_DEBUG) {
                console.log('[CursorDebug][Server][Send]', {
                    watcherId,
                    payload,
                });
            }
            watcher.ws.send(JSON.stringify(payload));
        }
    }
    
    handleWatchScreens(watcherId, message) {
        const targetId = this.resolveClientId(message.targetDeviceId);
        const watcher = this.clients.get(watcherId);
        const target = this.clients.get(targetId);
        if (!watcher || !target) return;
        // Update maps
        const prevTarget = this.watchingByWatcher.get(watcherId);
        if (prevTarget && prevTarget !== targetId) {
            const set = this.watchersByTarget.get(prevTarget);
            if (set) {
                set.delete(watcherId);
                if (set.size === 0) {
                    this.watchersByTarget.delete(prevTarget);
                    const prevTargetClient = this.clients.get(prevTarget);
                    if (prevTargetClient && prevTargetClient.ws) {
                        prevTargetClient.ws.send(JSON.stringify({ type: 'watch_status', watched: false }));
                    }
                }
            }
        }
        this.watchingByWatcher.set(watcherId, targetId);
        let set = this.watchersByTarget.get(targetId);
        if (!set) {
            set = new Set();
            this.watchersByTarget.set(targetId, set);
        }
        set.add(watcherId);
    // Notify target that it is watched (start sending updates)
        if (target.ws) {
            target.ws.send(JSON.stringify({ type: 'watch_status', watched: true }));
            // Ask target to send fresh state now
            target.ws.send(JSON.stringify({ type: 'data_request', fields: ['screens', 'volume'] }));
        }
    // Do not send cached info; wait for target to reply so watcher receives fresh data only
    }
    
    handleUnwatchScreens(watcherId, message) {
        const targetId = this.resolveClientId(message.targetDeviceId)
            || this.watchingByWatcher.get(watcherId);
        const set = this.watchersByTarget.get(targetId);
        if (set) {
            set.delete(watcherId);
            if (set.size === 0) {
                this.watchersByTarget.delete(targetId);
                const targetClient = this.clients.get(targetId);
                if (targetClient && targetClient.ws) {
                    targetClient.ws.send(JSON.stringify({ type: 'watch_status', watched: false }));
                }
            }
        }
        this.watchingByWatcher.delete(watcherId);
    }
    
    notifyWatchersOfTarget(targetId) {
        const set = this.watchersByTarget.get(targetId);
        if (!set || set.size === 0) return;
        const target = this.clients.get(targetId);
        if (!target) return;
        for (const watcherId of set) {
            const watcher = this.clients.get(watcherId);
            if (!watcher || !watcher.ws) continue;
            watcher.ws.send(JSON.stringify({
                type: 'screens_info',
                protocolVersion: this.protocolVersion,
                serverBootId: this.serverBootId,
                clientInfo: {
                    id: target.deviceId,
                    deviceId: target.deviceId,
                    runtimeId: target.runtimeId,
                    machineName: target.machineName,
                    platform: target.platform,
                    screens: target.screens,
                    systemUI: target.systemUI || [],
                    volumePercent: target.volumePercent
                }
            }));
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
