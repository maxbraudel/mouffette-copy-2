const WebSocket = require('ws');
const { v4: uuidv4 } = require('uuid');
const crypto = require('node:crypto');

const CURSOR_DEBUG = !!process.env.MOUFFETTE_CURSOR_DEBUG;
const CANONICAL_UUID_PATTERN = /^[0-9a-f]{8}-[0-9a-f]{4}-[1-5][0-9a-f]{3}-[89ab][0-9a-f]{3}-[0-9a-f]{12}$/i;
const CANVAS_SESSION_ID_PATTERN = /^[A-Za-z0-9_-]{1,512}$/;
const REMOTE_SCENE_INSTANCE_ID_PATTERN = /^[A-Za-z0-9_-]{1,128}$/;

// ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
// 🔐 MOUFFETTE SERVER - IDENTIFICATION SYSTEM & TERMINOLOGY FIX
// ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
//
// This server implements the fix for the "clientId" terminology confusion.
//
// TERMINOLOGY (Explicit Field Names):
//
//   Field Name              | Source        | Meaning                    | Lifetime
//   ----------------------- | ------------- | -------------------------- | -------------------
//   persistentClientId      | Client        | Stable device identity     | Permanent (persisted)
//   sessionId               | Client        | Connection identifier      | Temporary (per launch)
//   socketId (internal)     | Server        | Raw WebSocket ID           | Per connection
//   canvasSessionId         | Client        | Logical scene/project      | Until canvas deleted
//
// SERVER INTERNAL NAMING:
//   - client.persistentId: Maps to "persistentClientId" from protocol
//   - client.sessionId: Maps to "sessionId" from protocol
//   - client.id: Internal session identifier (used as Map key)
//
// BACKWARD COMPATIBILITY:
//   - Reads "persistentClientId" first (new explicit field)
//   - Falls back to "clientId" (legacy field) if not present
//   - Old clients continue to work during transition
//
// MIGRATION STATUS:
//   Phase 1: ✅ Accept both "clientId" and "persistentClientId" (COMPLETE)
//   Phase 2: ✅ Send explicit field names in responses (COMPLETE)
//   Phase 3: 🔄 Canvas lifecycle validation (IN PROGRESS)
//   Phase 4: ⏳ Deprecate "clientId" field (future)
// ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━

class MouffetteServer {
    constructor(port = 8080) {
        this.port = port;
        this.clients = new Map(); // sessionId -> client info (see terminology above)
        this.wss = null;
        // Watching relations: targetId -> Set of watcherIds, and watcherId -> targetId
        this.watchersByTarget = new Map();
        this.watchingByWatcher = new Map();
        
        // PHASE 2: Server-side state tracking
        this.uploads = new Map();        // uploadId -> { sender, target, canvasSessionId, startTime, files: [fileIds] }
        this.clientFiles = new Map();    // persistentClientId -> Map(canvasSessionId -> Set(fileId))
        // Mirrors clientFiles and records which authenticated sender session
        // created each inventory entry. Removal requests must match this owner
        // before either server state or the target filesystem is touched.
        this.clientFileOwners = new Map(); // persistentClientId -> Map(canvasSessionId -> Map(fileId -> senderPersistentId))
        this.clientFileGenerations = new Map(); // same shape, fileId -> validated uploadId
        this.pendingRemovals = new Map(); // removalId -> authenticated sender/target/canvas correlation
        this.pendingUploadAborts = new Map(); // uploadId -> correlated target cleanup acknowledgement
        this.sessionsByPersistent = new Map(); // persistentClientId -> Set(sessionId)
        // target session -> authoritative scene lifecycle state. Remote-scene
        // messages are accepted only when their authenticated socket and phase
        // match this entry.
        this.remoteScenesByTarget = new Map();
        // Successful STOP results remain briefly replayable so a retry never
        // asks the target to tear down the same multimedia graph twice.
        this.remoteSceneStopTombstones = new Map();
        this.remoteSceneAuxiliaryReplyRates = new Map();
        
        // PHASE 2: Active canvas tracking (CRITICAL for canvasSessionId validation)
        this.activeCanvases = new Map(); // persistentClientId -> Set(canvasSessionId)
        
        // PHASE 1: Upload timeout configuration
        this.UPLOAD_TIMEOUT_MS = 45 * 1000; // inactivity timeout, refreshed by chunks and acknowledgements
        this.UPLOAD_TARGET_ACK_TIMEOUT_MS = 30 * 1000;
        this.REMOVAL_ACK_TIMEOUT_MS = 30 * 1000;
        this.MAX_UPLOAD_FILES = 256;
        this.MAX_UPLOAD_FILE_BYTES = 16 * 1024 * 1024 * 1024;
        this.MAX_UPLOAD_TOTAL_BYTES = 64 * 1024 * 1024 * 1024;
        this.MAX_UPLOAD_CHUNK_BASE64_LENGTH = Math.ceil((128 * 1024) / 3) * 4;
        this.MAX_TARGET_BUFFERED_UPLOAD_BYTES = 8 * 1024 * 1024;
        this.MAX_PENDING_REMOVALS = 4096;
        this.MAX_REMOTE_SCENE_BYTES = 8 * 1024 * 1024;
        this.MAX_REMOTE_SCENE_SYNC_BYTES = 256 * 1024;
        this.MAX_REMOTE_SCENE_SCREENS = 64;
        this.MAX_REMOTE_SCENE_MEDIA = 512;
        this.MAX_REMOTE_SCENE_SYNC_ITEMS = 512;
        this.MAX_REMOTE_SCENE_BUFFERED_BYTES = 1024 * 1024;
        this.MAX_REMOTE_SCENE_TOMBSTONES = 4096;
        this.MAX_REMOTE_SCENE_AUXILIARY_REPLIES_PER_SECOND = 20;
        this.REMOTE_SCENE_SYNC_MIN_INTERVAL_MS = 50;
        this.REMOTE_SCENE_PREPARE_TIMEOUT_MS = 45 * 1000;
        this.REMOTE_SCENE_ACTIVATE_TIMEOUT_MS = 15 * 1000;
        this.REMOTE_SCENE_STOP_TIMEOUT_MS = 30 * 1000;
        this.REMOTE_SCENE_TOMBSTONE_TTL_MS = 60 * 1000;
        this.UPLOAD_CHANNEL_TOKEN_TTL_MS = 30 * 1000;
        this.uploadChannelTokens = new Map();
        this.uploadSocketsByClient = new Map();
        this.uploadChannelMessageTypes = new Set([
            'upload_start', 'upload_chunk', 'upload_complete', 'upload_abort'
        ]);
        this.uploadCleanupInterval = null;
    }

    start() {
        // Bind explicitly to 0.0.0.0 to listen on all IPv4 interfaces (LAN accessible)
        this.wss = new WebSocket.Server({
            port: this.port,
            host: '0.0.0.0',
            // Upload chunks are much smaller, while legitimate scene-control
            // payloads can exceed 512 KiB on complex canvases.
            maxPayload: 16 * 1024 * 1024,
            perMessageDeflate: false
        });
        
        console.log(`🎯 Mouffette Server started on ws://0.0.0.0:${this.port}`);
        
        // Sweep frequently so stalled partial state is released promptly.
        this.uploadCleanupInterval = setInterval(() => {
            this.cleanupStalledUploads();
        }, 5000);
        console.log(`🧹 Upload timeout cleanup started (timeout: ${this.UPLOAD_TIMEOUT_MS}ms)`);
        
        this.wss.on('connection', (ws, req) => {
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
                    this.abortUploadsForUploadSocket(boundClient.id, ws,
                        'Dedicated upload connection closed');
                    this.unregisterUploadSocket(boundClient, ws);
                    console.log(`📤 Upload channel disconnected for ${boundClient.id}`);
                });
                
                ws.on('error', (error) => {
                    this.abortUploadsForUploadSocket(boundClient.id, ws,
                        'Dedicated upload connection failed');
                    this.unregisterUploadSocket(boundClient, ws);
                    console.error(`❌ Upload channel error for ${boundClient.id}:`, error);
                });

                ws.send(JSON.stringify({
                    type: 'upload_channel_ready',
                    clientId: boundClient.id
                }));
                return;
            }
            
            // Regular control channel connection
            const clientId = uuidv4();
            const clientInfo = {
                id: clientId,
                sessionId: clientId,
                persistentId: null,
                ws: ws,
                machineName: null,
                screens: [],
                systemUI: [], // array of system UI element rects
                volumePercent: -1,
                status: 'connected',
                connectedAt: new Date().toISOString(),
                socketLabel: clientId
            };
            
            this.clients.set(clientId, clientInfo);
            console.log(`📱 New client connected: ${clientId}`);
            
            // Send welcome message with socket ID (note: this is NOT persistentClientId)
            ws.send(JSON.stringify({
                type: 'welcome',
                socketId: clientId,        // ✅ NEW: Explicit name for raw socket ID
                clientId: clientId,        // ⚠️ LEGACY: For backward compatibility
                message: 'Connected to Mouffette Server'
            }));
            
            ws.on('message', (data) => {
                try {
                    const message = JSON.parse(data.toString());
                    // Always route using the client's current ID (session reassignment can occur during register)
                    this.handleMessage(clientInfo.id, message);
                } catch (error) {
                    console.error('❌ Error parsing message:', error);
                    ws.send(JSON.stringify({
                        type: 'error',
                        message: 'Invalid JSON format'
                    }));
                }
            });
            
            ws.on('close', () => {
                if (clientInfo.replaced) {
                    return;
                }
                const finalId = clientInfo.id;
                console.log(`📱 Client disconnected: ${finalId}`);
                this.revokeUploadChannelsForClient(clientInfo);
                this.handleRemoteSceneClientDeparture(finalId);
                this.abortUploadsForClient(finalId);
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
                    this.unregisterSessionForPersistent(clientInfo.persistentId, clientInfo.sessionId || finalId);
                }
                this.clients.delete(finalId);
                this.broadcastClientList();
            });
            
            ws.on('error', (error) => {
                if (clientInfo.replaced) {
                    return;
                }
                const finalId = clientInfo.id;
                console.error(`❌ WebSocket error for client ${finalId}:`, error);
                this.revokeUploadChannelsForClient(clientInfo);
                this.handleRemoteSceneClientDeparture(finalId);
                this.abortUploadsForClient(finalId);
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
                    this.unregisterSessionForPersistent(clientInfo.persistentId, clientInfo.sessionId || finalId);
                }
                this.clients.delete(finalId);
                this.broadcastClientList();
            });
            
            // Send current client list to new client
            this.sendClientList(clientId);
            // Broadcast updated client list to all clients
            this.broadcastClientList();
        });
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

    revokeUploadChannelsForClient(client) {
        if (!client) return;
        this.revokeUploadTokensForClient(client);
        const sockets = this.uploadSocketsByClient.get(client);
        this.uploadSocketsByClient.delete(client);
        if (!sockets) return;
        for (const socket of sockets) {
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

        this.handleMessage(boundClient.id, {
            ...message,
            senderClientId: boundClient.id,
            senderPersistentClientId: boundClient.persistentId
        }, ws);
    }

    isValidRemoteSceneInstanceId(sceneInstanceId) {
        return typeof sceneInstanceId === 'string'
            && REMOTE_SCENE_INSTANCE_ID_PATTERN.test(sceneInstanceId);
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

    sendRemoteSceneMessage(clientId, message, maximumBufferedBytes = null) {
        const client = this.clients.get(clientId);
        if (!client || !client.ws || client.ws.readyState !== WebSocket.OPEN) {
            return false;
        }
        const bufferedAmount = Number(client.ws.bufferedAmount) || 0;
        if (Number.isFinite(maximumBufferedBytes)
            && bufferedAmount > maximumBufferedBytes) {
            return false;
        }
        const encoded = this.serializedJsonWithinLimit(
            message, this.MAX_REMOTE_SCENE_BYTES + 4096);
        if (!encoded) return false;
        try {
            client.ws.send(encoded);
            return true;
        } catch (error) {
            console.error('❌ Remote-scene send failed:', error);
            return false;
        }
    }

    sendRemoteSceneValidation(ownerId, targetId, sceneInstanceId, success, error = '') {
        const result = {
            type: 'remote_scene_validation',
            senderClientId: typeof targetId === 'string' ? targetId.slice(0, 512) : '',
            targetClientId: ownerId,
            sceneInstanceId: this.isValidRemoteSceneInstanceId(sceneInstanceId)
                ? sceneInstanceId : '',
            success: success === true
        };
        if (!result.success && error) result.error = String(error).slice(0, 512);
        return this.sendRemoteSceneMessage(ownerId, result);
    }

    sendRemoteSceneStopped(ownerId, targetId, sceneInstanceId, success, error = '') {
        const result = {
            type: 'remote_scene_stopped',
            senderClientId: typeof targetId === 'string' ? targetId.slice(0, 512) : '',
            targetClientId: ownerId,
            sceneInstanceId: this.isValidRemoteSceneInstanceId(sceneInstanceId)
                ? sceneInstanceId : '',
            success: success === true
        };
        if (!result.success && error) result.error = String(error).slice(0, 512);
        return this.sendRemoteSceneMessage(ownerId, result);
    }

    sendRemoteSceneLaunched(ownerId, targetId, sceneInstanceId) {
        return this.sendRemoteSceneMessage(ownerId, {
            type: 'remote_scene_launched',
            senderClientId: targetId,
            targetClientId: ownerId,
            sceneInstanceId
        });
    }

    remoteSceneTombstoneKey(ownerId, targetId, sceneInstanceId) {
        return JSON.stringify([ownerId, targetId, sceneInstanceId]);
    }

    pruneRemoteSceneStopTombstones(now = Date.now()) {
        for (const [key, tombstone] of this.remoteSceneStopTombstones) {
            if (!tombstone || tombstone.expiresAt <= now) {
                this.remoteSceneStopTombstones.delete(key);
            }
        }
        while (this.remoteSceneStopTombstones.size > this.MAX_REMOTE_SCENE_TOMBSTONES) {
            const oldestKey = this.remoteSceneStopTombstones.keys().next().value;
            if (oldestKey === undefined) break;
            this.remoteSceneStopTombstones.delete(oldestKey);
        }
    }

    rememberRemoteSceneStopped(run, now = Date.now()) {
        this.pruneRemoteSceneStopTombstones(now);
        const key = this.remoteSceneTombstoneKey(
            run.ownerId, run.targetId, run.sceneInstanceId);
        this.remoteSceneStopTombstones.delete(key);
        this.remoteSceneStopTombstones.set(key, {
            ownerId: run.ownerId,
            targetId: run.targetId,
            sceneInstanceId: run.sceneInstanceId,
            lastReplayedAt: 0,
            expiresAt: now + this.REMOTE_SCENE_TOMBSTONE_TTL_MS
        });
        this.pruneRemoteSceneStopTombstones(now);
    }

    getRemoteSceneStopTombstone(ownerId, targetId, sceneInstanceId, now = Date.now()) {
        this.pruneRemoteSceneStopTombstones(now);
        return this.remoteSceneStopTombstones.get(
            this.remoteSceneTombstoneKey(ownerId, targetId, sceneInstanceId)) || null;
    }

    allowRemoteSceneAuxiliaryReply(ownerId, now = Date.now()) {
        let rate = this.remoteSceneAuxiliaryReplyRates.get(ownerId);
        if (!rate || now - rate.windowStartedAt >= 1000) {
            rate = { windowStartedAt: now, count: 0 };
            this.remoteSceneAuxiliaryReplyRates.set(ownerId, rate);
        }
        if (rate.count >= this.MAX_REMOTE_SCENE_AUXILIARY_REPLIES_PER_SECOND) {
            return false;
        }
        rate.count++;
        return true;
    }

    rejectRemoteSceneStart(ownerId, targetId, sceneInstanceId, reason) {
        if (!this.allowRemoteSceneAuxiliaryReply(ownerId)) return;
        this.sendRemoteSceneValidation(
            ownerId, targetId || '', sceneInstanceId, false, reason);
    }

    handleRemoteSceneStart(ownerId, message) {
        const requestedTargetId = typeof message.targetClientId === 'string'
            ? message.targetClientId : '';
        const targetId = this.resolveClientId(requestedTargetId);
        const scene = message.scene;
        const sceneInstanceId = scene && typeof scene === 'object' && !Array.isArray(scene)
            ? scene.sceneInstanceId : '';
        const owner = this.clients.get(ownerId);
        const target = targetId ? this.clients.get(targetId) : null;
        const reject = reason => this.rejectRemoteSceneStart(
            ownerId, targetId || requestedTargetId, sceneInstanceId, reason);

        if (!owner || !owner.persistentId || !target || !target.persistentId
            || ownerId === targetId || !this.isValidRemoteSceneInstanceId(sceneInstanceId)
            || !scene || typeof scene !== 'object' || Array.isArray(scene)
            || !Array.isArray(scene.screens)
            || scene.screens.length > this.MAX_REMOTE_SCENE_SCREENS
            || !Array.isArray(scene.media)
            || scene.media.length > this.MAX_REMOTE_SCENE_MEDIA
            || !this.serializedJsonWithinLimit(scene, this.MAX_REMOTE_SCENE_BYTES)) {
            reject('Invalid remote scene target, identifier, or payload');
            return;
        }

        const activeRun = this.remoteScenesByTarget.get(targetId);
        if (activeRun) {
            if (activeRun.ownerId === ownerId
                && activeRun.sceneInstanceId === sceneInstanceId) {
                // The first START owns the immutable payload. Replays never ask
                // Qt multimedia to prepare the same graph a second time.
                return;
            }
            reject('Remote scene target is already busy');
            return;
        }
        if (this.getRemoteSceneStopTombstone(ownerId, targetId, sceneInstanceId)) {
            reject('Remote scene instance has already been stopped');
            return;
        }

        const now = Date.now();
        const run = {
            ownerId,
            targetId,
            sceneInstanceId,
            phase: 'preparing',
            createdAt: now,
            lastActivity: now,
            lastVideoSequence: 0,
            lastVideoRelayAt: 0,
            previousPhase: null
        };
        this.remoteScenesByTarget.set(targetId, run);
        const relayed = this.sendRemoteSceneMessage(targetId, {
            type: 'remote_scene_start',
            targetClientId: targetId,
            senderClientId: ownerId,
            scene
        }, this.MAX_REMOTE_SCENE_BUFFERED_BYTES);
        if (!relayed) {
            this.remoteScenesByTarget.delete(targetId);
            reject('Remote scene target is unavailable or congested');
        }
    }

    handleRemoteSceneValidation(targetId, message) {
        const run = this.remoteScenesByTarget.get(targetId);
        if (!run || !this.isValidRemoteSceneInstanceId(message.sceneInstanceId)
            || run.sceneInstanceId !== message.sceneInstanceId
            || typeof message.success !== 'boolean') {
            return;
        }
        if (run.phase !== 'preparing') {
            // A delayed or duplicate validation result must not make the owner
            // issue another ACTIVATE command.
            return;
        }

        const error = typeof message.error === 'string'
            ? message.error.slice(0, 512) : '';
        if (!message.success) {
            this.remoteScenesByTarget.delete(targetId);
            this.sendRemoteSceneValidation(
                run.ownerId, targetId, run.sceneInstanceId, false,
                error || 'Remote scene validation failed');
            return;
        }

        run.phase = 'prepared';
        run.lastActivity = Date.now();
        this.sendRemoteSceneValidation(
            run.ownerId, targetId, run.sceneInstanceId, true);
    }

    handleRemoteSceneActivate(ownerId, message) {
        const targetId = typeof message.targetClientId === 'string'
            ? this.resolveClientId(message.targetClientId) : null;
        const run = targetId ? this.remoteScenesByTarget.get(targetId) : null;
        const activationEpochMs = message.activationEpochMs;
        const activationDelayMs = message.activationDelayMs;
        if (!run || run.ownerId !== ownerId
            || run.sceneInstanceId !== message.sceneInstanceId
            || !this.isValidRemoteSceneInstanceId(message.sceneInstanceId)
            || !Number.isSafeInteger(activationEpochMs) || activationEpochMs <= 0
            || !Number.isSafeInteger(activationDelayMs)
            || activationDelayMs < 0 || activationDelayMs > 5000) {
            return;
        }
        if (run.phase === 'running') {
            // Converge an owner that retried after losing LAUNCHED without
            // reactivating the target.
            this.sendRemoteSceneLaunched(ownerId, targetId, run.sceneInstanceId);
            return;
        }
        if (run.phase === 'activating') return;
        if (run.phase !== 'prepared') return;

        run.phase = 'activating';
        run.lastActivity = Date.now();
        const relayed = this.sendRemoteSceneMessage(targetId, {
            type: 'remote_scene_activate',
            targetClientId: targetId,
            senderClientId: ownerId,
            sceneInstanceId: run.sceneInstanceId,
            activationEpochMs,
            activationDelayMs
        }, this.MAX_REMOTE_SCENE_BUFFERED_BYTES);
        if (!relayed) {
            this.remoteScenesByTarget.delete(targetId);
            this.sendRemoteSceneValidation(
                ownerId, targetId, run.sceneInstanceId, false,
                'Remote scene activation could not be delivered');
        }
    }

    handleRemoteSceneLaunched(targetId, message) {
        const run = this.remoteScenesByTarget.get(targetId);
        if (!run || run.sceneInstanceId !== message.sceneInstanceId
            || run.phase !== 'activating') {
            return;
        }
        run.phase = 'running';
        run.lastActivity = Date.now();
        this.sendRemoteSceneLaunched(run.ownerId, targetId, run.sceneInstanceId);
    }

    isValidRemoteSceneVideoSync(message) {
        if (!this.isValidRemoteSceneInstanceId(message.sceneInstanceId)
            || !Number.isSafeInteger(message.sequence) || message.sequence <= 0
            || !Number.isSafeInteger(message.sampledEpochMs) || message.sampledEpochMs <= 0
            || !Array.isArray(message.videos)
            || message.videos.length > this.MAX_REMOTE_SCENE_SYNC_ITEMS
            || !this.serializedJsonWithinLimit(message.videos, this.MAX_REMOTE_SCENE_SYNC_BYTES)) {
            return false;
        }
        const maximumPositionMs = 7 * 24 * 60 * 60 * 1000;
        return message.videos.every(state => state && typeof state === 'object'
            && !Array.isArray(state)
            && typeof state.mediaId === 'string'
            && state.mediaId.length >= 1 && state.mediaId.length <= 128
            && Number.isFinite(state.positionMs)
            && state.positionMs >= 0 && state.positionMs <= maximumPositionMs
            && Number.isFinite(state.durationMs)
            && state.durationMs >= 0 && state.durationMs <= maximumPositionMs
            && typeof state.playing === 'boolean'
            && typeof state.muted === 'boolean'
            && typeof state.visible === 'boolean'
            && typeof state.repeatAvailable === 'boolean');
    }

    handleRemoteSceneVideoSync(ownerId, message) {
        const targetId = typeof message.targetClientId === 'string'
            ? this.resolveClientId(message.targetClientId) : null;
        const run = targetId ? this.remoteScenesByTarget.get(targetId) : null;
        if (!run || run.ownerId !== ownerId || run.phase !== 'running'
            || run.sceneInstanceId !== message.sceneInstanceId
            || !this.isValidRemoteSceneVideoSync(message)
            || message.sequence <= run.lastVideoSequence) {
            return;
        }

        const now = Date.now();
        run.lastVideoSequence = message.sequence;
        if (now - run.lastVideoRelayAt < this.REMOTE_SCENE_SYNC_MIN_INTERVAL_MS) {
            return;
        }
        const target = this.clients.get(targetId);
        if (!target || !target.ws || target.ws.readyState !== WebSocket.OPEN
            || (Number(target.ws.bufferedAmount) || 0) > this.MAX_REMOTE_SCENE_BUFFERED_BYTES) {
            return;
        }

        if (this.sendRemoteSceneMessage(targetId, {
            type: 'remote_scene_video_sync',
            targetClientId: targetId,
            senderClientId: ownerId,
            sceneInstanceId: run.sceneInstanceId,
            sequence: message.sequence,
            sampledEpochMs: message.sampledEpochMs,
            videos: message.videos
        }, this.MAX_REMOTE_SCENE_BUFFERED_BYTES)) {
            run.lastVideoRelayAt = now;
            run.lastActivity = now;
        }
    }

    handleRemoteSceneStop(ownerId, message) {
        const requestedTargetId = typeof message.targetClientId === 'string'
            ? message.targetClientId : '';
        const targetId = this.resolveClientId(requestedTargetId);
        if (!targetId || ownerId === targetId) return;

        const run = this.remoteScenesByTarget.get(targetId);
        let sceneInstanceId = typeof message.sceneInstanceId === 'string'
            ? message.sceneInstanceId : '';
        if (!sceneInstanceId && run && run.ownerId === ownerId) {
            // Compatibility for old clients: correlation is supplied only from
            // authenticated server state, never from another client's payload.
            sceneInstanceId = run.sceneInstanceId;
        }
        if (!this.isValidRemoteSceneInstanceId(sceneInstanceId)) return;

        let tombstone = this.getRemoteSceneStopTombstone(
            ownerId, targetId, sceneInstanceId);
        if (!run) {
            const now = Date.now();
            if (!this.allowRemoteSceneAuxiliaryReply(ownerId, now)) return;
            if (!tombstone) {
                // No active run also means there is nothing left to tear down:
                // this is the safe, idempotent answer after a server restart or
                // a target reconnect, both of which close the target socket and
                // make its controller clear the scene locally.
                this.rememberRemoteSceneStopped({
                    ownerId,
                    targetId,
                    sceneInstanceId
                }, now);
                tombstone = this.getRemoteSceneStopTombstone(
                    ownerId, targetId, sceneInstanceId, now);
            }
            if (tombstone && now - tombstone.lastReplayedAt >= 1000) {
                tombstone.lastReplayedAt = now;
                this.sendRemoteSceneStopped(
                    ownerId, targetId, sceneInstanceId, true);
            }
            return;
        }
        if (run.ownerId !== ownerId || run.sceneInstanceId !== sceneInstanceId) {
            return;
        }
        if (run.phase === 'stopping') return;

        run.previousPhase = run.phase;
        run.phase = 'stopping';
        run.lastActivity = Date.now();
        const relayed = this.sendRemoteSceneMessage(targetId, {
            type: 'remote_scene_stop',
            targetClientId: targetId,
            senderClientId: ownerId,
            sceneInstanceId
        });
        if (!relayed) {
            this.remoteScenesByTarget.delete(targetId);
            const target = this.clients.get(targetId);
            const targetTransportLost = !target || !target.ws
                || target.ws.readyState !== WebSocket.OPEN;
            if (targetTransportLost) {
                this.rememberRemoteSceneStopped(run);
                this.sendRemoteSceneStopped(
                    ownerId, targetId, sceneInstanceId, true);
            } else {
                this.sendRemoteSceneStopped(
                    ownerId, targetId, sceneInstanceId, false,
                    'Remote scene stop could not be delivered');
            }
        }
    }

    handleRemoteSceneStopped(targetId, message) {
        const run = this.remoteScenesByTarget.get(targetId);
        if (!run || run.phase !== 'stopping'
            || run.sceneInstanceId !== message.sceneInstanceId
            || typeof message.success !== 'boolean') {
            return;
        }
        const error = typeof message.error === 'string'
            ? message.error.slice(0, 512) : '';
        if (!message.success) {
            const resumablePhases = new Set([
                'preparing', 'prepared', 'activating', 'running'
            ]);
            run.phase = resumablePhases.has(run.previousPhase)
                ? run.previousPhase : 'running';
            run.previousPhase = null;
            run.lastActivity = Date.now();
            this.sendRemoteSceneStopped(
                run.ownerId, targetId, run.sceneInstanceId, false,
                error || 'Remote client failed to stop the scene');
            return;
        }

        this.remoteScenesByTarget.delete(targetId);
        this.rememberRemoteSceneStopped(run);
        this.sendRemoteSceneStopped(
            run.ownerId, targetId, run.sceneInstanceId, true);
    }

    cleanupRemoteScenes(now = Date.now()) {
        this.pruneRemoteSceneStopTombstones(now);
        for (const [targetId, run] of Array.from(this.remoteScenesByTarget.entries())) {
            if (!run || run.phase === 'running') continue;
            const timeoutMs = run.phase === 'stopping'
                ? this.REMOTE_SCENE_STOP_TIMEOUT_MS
                : run.phase === 'activating'
                    ? this.REMOTE_SCENE_ACTIVATE_TIMEOUT_MS
                    : this.REMOTE_SCENE_PREPARE_TIMEOUT_MS;
            if (now - (run.lastActivity || run.createdAt || now) <= timeoutMs) continue;

            const cleanupDelivered = this.sendRemoteSceneMessage(targetId, {
                type: 'remote_scene_stop',
                targetClientId: targetId,
                senderClientId: run.ownerId,
                sceneInstanceId: run.sceneInstanceId,
                protocolTimeout: true
            });
            this.remoteScenesByTarget.delete(targetId);
            if (run.phase === 'stopping') {
                const target = this.clients.get(targetId);
                const targetTransportLost = !target || !target.ws
                    || target.ws.readyState !== WebSocket.OPEN;
                if (!cleanupDelivered && targetTransportLost) {
                    this.rememberRemoteSceneStopped(run, now);
                    this.sendRemoteSceneStopped(
                        run.ownerId, targetId, run.sceneInstanceId, true);
                } else {
                    this.sendRemoteSceneStopped(
                        run.ownerId, targetId, run.sceneInstanceId, false,
                        'Remote scene stop acknowledgement timed out');
                }
            } else {
                this.sendRemoteSceneValidation(
                    run.ownerId, targetId, run.sceneInstanceId, false,
                    'Remote scene launch timed out');
            }
        }
    }

    handleRemoteSceneClientDeparture(clientId) {
        if (!clientId) return;

        const targetRun = this.remoteScenesByTarget.get(clientId);
        if (targetRun) {
            if (targetRun.phase === 'preparing'
                || targetRun.phase === 'prepared'
                || targetRun.phase === 'activating') {
                this.sendRemoteSceneValidation(
                    targetRun.ownerId, clientId, targetRun.sceneInstanceId, false,
                    'Remote target connection was lost');
            } else {
                const completedRun = { ...targetRun, targetId: clientId };
                this.rememberRemoteSceneStopped(completedRun);
                this.sendRemoteSceneStopped(
                    targetRun.ownerId, clientId, targetRun.sceneInstanceId, true);
            }
            this.remoteScenesByTarget.delete(clientId);
        }

        // An owner's departure triggers exactly one server-authored cleanup.
        for (const [targetId, run] of Array.from(this.remoteScenesByTarget.entries())) {
            if (!run || run.ownerId !== clientId) continue;
            this.sendRemoteSceneMessage(targetId, {
                type: 'remote_scene_stop',
                targetClientId: targetId,
                senderClientId: clientId,
                sceneInstanceId: run.sceneInstanceId,
                ownerDisconnected: true
            });
            this.remoteScenesByTarget.delete(targetId);
        }

        for (const [key, tombstone] of this.remoteSceneStopTombstones) {
            // Keep a departed target's terminal result: if it reconnects under
            // the same logical session, the still-connected owner may retry
            // STOP and must receive the cached convergence result. An owner
            // departure has no consumer left, so its entries can go now.
            if (tombstone && tombstone.ownerId === clientId) {
                this.remoteSceneStopTombstones.delete(key);
            }
        }
        this.remoteSceneAuxiliaryReplyRates.delete(clientId);
    }
    
    handleMessage(clientId, message, uploadTransportSocket = null) {
        const client = this.clients.get(clientId);
        if (!client) return;
        
        if (message.type !== 'upload_chunk' && message.type !== 'upload_progress'
            && message.type !== 'cursor_update'
            && message.type !== 'remote_scene_video_sync') {
            console.log(`📨 Message from ${clientId}:`, message.type);
        }
        
        switch (message.type) {
            case 'register':
                this.handleRegister(clientId, message);
                break;
            case 'request_client_list':
                this.sendClientList(clientId);
                break;
            case 'request_upload_channel':
                if (!this.issueUploadChannelToken(clientId)) {
                    this.sendError(clientId, 'Upload channel requires a registered control connection');
                }
                break;
            case 'request_screens':
                this.handleRequestScreens(clientId, message);
                break;
            case 'watch_screens':
                this.handleWatchScreens(clientId, message);
                break;
            case 'unwatch_screens':
                this.handleUnwatchScreens(clientId, message);
                break;
            // Upload flow: track state and relay
            case 'upload_start':
                this.handleUploadStart(clientId, message, uploadTransportSocket);
                break;
            case 'upload_chunk':
                this.handleUploadChunk(clientId, message, uploadTransportSocket);
                break;
            case 'upload_complete':
                this.handleUploadComplete(clientId, message, uploadTransportSocket);
                break;
            case 'upload_abort':
                this.handleUploadAbort(clientId, message);
                break;
            case 'remove_all_files':
                this.handleRemoveAllFiles(clientId, message);
                break;
            case 'remove_file':
                this.handleRemoveFile(clientId, message);
                break;
            // PHASE 2: Canvas lifecycle tracking (CRITICAL for canvasSessionId validation)
            case 'canvas_created':
                this.handleCanvasCreated(clientId, message);
                break;
            case 'canvas_deleted':
                this.handleCanvasDeleted(clientId, message);
                break;
            // Progress/status notifications from target back to sender
            case 'upload_progress':
                this.handleUploadProgress(clientId, message);
                break;
            case 'upload_ready':
                this.handleUploadReady(clientId, message);
                break;
            case 'upload_finished':
                this.handleUploadFinished(clientId, message);
                break;
            case 'upload_rejected':
                this.handleUploadRejected(clientId, message);
                break;
            case 'upload_abort_ack':
                this.handleUploadAbortAcknowledgement(clientId, message);
                break;
            case 'all_files_removed':
                this.handleAllFilesRemoved(clientId, message);
                break;
            case 'remove_all_files_failed':
                this.handleAllFilesRemovalFailed(clientId, message);
                break;
            case 'media_share':
                this.handleMediaShare(clientId, message);
                break;
            case 'media_update':
                this.handleMediaUpdate(clientId, message);
                break;
            case 'stop_sharing':
                this.handleStopSharing(clientId, message);
                break;
            case 'cursor_update':
                this.handleCursorUpdate(clientId, message);
                break;
            case 'remote_scene_start':
                this.handleRemoteSceneStart(clientId, message);
                break;
            case 'remote_scene_activate':
                this.handleRemoteSceneActivate(clientId, message);
                break;
            case 'remote_scene_video_sync':
                this.handleRemoteSceneVideoSync(clientId, message);
                break;
            case 'remote_scene_stop':
                this.handleRemoteSceneStop(clientId, message);
                break;
            case 'remote_scene_stopped':
                this.handleRemoteSceneStopped(clientId, message);
                break;
            case 'remote_scene_validation':
                this.handleRemoteSceneValidation(clientId, message);
                break;
            case 'remote_scene_launched':
                this.handleRemoteSceneLaunched(clientId, message);
                break;
            default:
                console.log(`⚠️ Unknown message type: ${message.type}`);
        }
    }

    // Helper to relay a message from sender -> target
    // Phase 3 cleanup: resolve targetClientId (accepts both sessionId and persistentId)
    resolveClientId(targetClientId) {
        // Try direct lookup (sessionId)
        if (this.clients.has(targetClientId)) {
            return targetClientId;
        }
        // Try persistent → session lookup
        for (const [sessionId, clientInfo] of this.clients.entries()) {
            if (clientInfo.persistentId === targetClientId) {
                return sessionId;
            }
        }
        return null; // not found
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
        // Scene-control ownership must come from the authenticated control
        // socket, never from a client-provided JSON field. Other protocols keep
        // their existing compatibility behavior.
        const isRemoteSceneMessage = typeof message.type === 'string'
            && message.type.startsWith('remote_scene_');
        const isUploadMessage = typeof message.type === 'string'
            && (message.type.startsWith('upload_') || message.type === 'remove_file'
                || message.type === 'remove_all_files');
        if (isRemoteSceneMessage || isUploadMessage) {
            message.senderClientId = senderId;
            if (isUploadMessage) {
                message.senderPersistentClientId = this.getPersistentId(senderId);
            }
        } else if (!message.senderClientId) {
            message.senderClientId = senderId;
        }
        try {
            if (message.type === 'remote_scene_start'
                || message.type === 'remote_scene_activate'
                || message.type === 'remote_scene_stop') {
                console.log(`🎯 Relaying ${message.type} from ${senderId} -> ${targetClientId}`);
            }
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
        const targetId = message.targetClientId;
        const requester = this.clients.get(requesterId);
        const target = this.clients.get(targetId);
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
            clientInfo: {
                id: target.id,
                sessionId: target.sessionId || target.id,
                persistentClientId: target.persistentId,
                machineName: target.machineName,
                platform: target.platform,
                screens: target.screens,
                systemUI: target.systemUI || [],
                volumePercent: target.volumePercent
            }
        }));
    }
    
    handleRegister(clientId, message) {
        const client = this.clients.get(clientId);
        if (!client) return;
    
    // ✅ ID TERMINOLOGY FIX: Read explicit field names with backward compatibility
    // - "persistentClientId": NEW explicit field (recommended)
    // - "clientId": LEGACY field (for old client versions)
    const requestedPersistentId = (typeof message.persistentClientId === 'string') 
        ? message.persistentClientId.trim() 
        : (typeof message.clientId === 'string') 
            ? message.clientId.trim() 
            : '';
    
    const requestedSessionId = (typeof message.sessionId === 'string') ? message.sessionId.trim() : '';
    const previousPersistentId = client.persistentId;
    const persistentId = requestedPersistentId || previousPersistentId;
    
    // Validation: persistentClientId is mandatory for registration
    if (!persistentId) {
        console.warn(`⚠️ Registration rejected: missing persistentClientId for socket ${clientId} - machine: ${message.machineName || 'unknown'}`);
        if (client.ws && client.ws.readyState === WebSocket.OPEN) {
            client.ws.send(JSON.stringify({
                type: 'error',
                message: 'persistentClientId is required for registration'
            }));
        }
        return;
    }
    const sessionId = requestedSessionId || client.sessionId || client.id;

        client.socketLabel = client.socketLabel || clientId;

        // Detect reconnection of the same logical session
        const existingSession = this.clients.get(sessionId);
        if (existingSession && existingSession !== client) {
            console.log(`🔄 Client session reconnecting: ${persistentId} (session ${sessionId}, old socket: ${existingSession.socketLabel || existingSession.id})`);
            client.machineName = existingSession.machineName;
            client.platform = existingSession.platform;
            client.screens = existingSession.screens;
            client.systemUI = existingSession.systemUI || [];
            client.volumePercent = existingSession.volumePercent;
            client.status = existingSession.status;
            client.connectedAt = existingSession.connectedAt;
            client.persistentId = existingSession.persistentId || persistentId;
            if (existingSession.persistentId) {
                this.unregisterSessionForPersistent(existingSession.persistentId, sessionId);
            }
            // The old socket's close/error callbacks intentionally return once
            // marked as replaced, so terminate its protocol state explicitly.
            this.abortUploadsForClient(existingSession.id);
            this.handleRemoteSceneClientDeparture(existingSession.id);
            this.revokeUploadChannelsForClient(existingSession);
            this.clients.delete(existingSession.id);
            existingSession.replaced = true;
            existingSession.id = `${sessionId}::replaced::${Date.now()}`;
            if (existingSession.ws && existingSession.ws.readyState === WebSocket.OPEN) {
                existingSession.ws.close();
            }
        }

        if (client.id !== sessionId) {
            this.cleanupWatcherReferences(client.id, sessionId);
            this.clients.delete(client.id);
            client.id = sessionId;
            client.sessionId = sessionId;
            this.clients.set(sessionId, client);
        } else {
            client.sessionId = sessionId;
        }

        client.persistentId = persistentId;
        if (previousPersistentId && previousPersistentId !== persistentId) {
            this.unregisterSessionForPersistent(previousPersistentId, sessionId);
        }
        this.registerSessionForPersistent(persistentId, sessionId);

        // Only update fields that are explicitly provided to avoid clobbering identity on partial updates
        if (Object.prototype.hasOwnProperty.call(message, 'machineName')) {
            client.machineName = message.machineName || `Client-${client.id.slice(0, 8)}`;
        } else if (!client.machineName) {
            client.machineName = `Client-${client.id.slice(0, 8)}`;
        }
        if (Object.prototype.hasOwnProperty.call(message, 'screens')) {
            client.screens = message.screens || [];
        }
        if (Object.prototype.hasOwnProperty.call(message, 'systemUI') && Array.isArray(message.systemUI)) {
            client.systemUI = message.systemUI;
        }
        if (typeof message.volumePercent === 'number') {
            client.volumePercent = Math.max(0, Math.min(100, Math.round(message.volumePercent)));
        }
        if (Object.prototype.hasOwnProperty.call(message, 'platform')) {
            client.platform = message.platform || 'unknown';
        } else if (!client.platform) {
            client.platform = 'unknown';
        }
        
    console.log(`✅ Client registered: ${client.machineName} (${client.platform}) with ${client.screens.length} screen(s) [session: ${client.sessionId}] [persistent: ${client.persistentId}]`);
        
        // Send confirmation (use actual client.id which may have changed to persistent ID)
        client.ws.send(JSON.stringify({
            type: 'registration_confirmed',
            clientInfo: {
                id: client.id,  // Send the actual (possibly persistent) ID
                machineName: client.machineName,
                screens: client.screens,
                platform: client.platform,
                systemUI: client.systemUI || [],
                volumePercent: client.volumePercent,
                persistentClientId: client.persistentId
            }
        }));
        
        // PHASE 2: Send state sync if client has known files
        const stateKey = client.persistentId;
        if (this.clientFiles.has(stateKey)) {
            const clientIdeas = this.clientFiles.get(stateKey);
            const ideas = [];
            for (const [canvasSessionId, fileIds] of clientIdeas.entries()) {
                if (fileIds.size > 0) {
                    ideas.push({
                        canvasSessionId: canvasSessionId,
                        fileIds: Array.from(fileIds)
                    });
                }
            }
            
            if (ideas.length > 0) {
                console.log(`🔄 Sending state_sync to ${client.id}: ${ideas.length} idea(s), ${ideas.reduce((sum, i) => sum + i.fileIds.length, 0)} file(s)`);
                client.ws.send(JSON.stringify({
                    type: 'state_sync',
                    ideas: ideas
                }));
            }
        }
        
        // Broadcast updated client list
        this.broadcastClientList();
    // Notify watchers of this target with fresh screens
    this.notifyWatchersOfTarget(client.id);  // Use actual client.id
    }
    
    sendClientList(clientId) {
        const client = this.clients.get(clientId);
        if (!client) return;
        
        const clientList = Array.from(this.clients.values())
            .filter(c => c.id !== clientId && c.machineName && c.persistentId) // Don't include self and only registered clients with stable identity
            .map(c => ({
                id: c.id,
                sessionId: c.sessionId || c.id,
                persistentClientId: c.persistentId,
                machineName: c.machineName,
                screens: c.screens,
                platform: c.platform,
                systemUI: c.systemUI || [],
                volumePercent: c.volumePercent,
                status: c.status
            }));
        
        client.ws.send(JSON.stringify({
            type: 'client_list',
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

        this.cleanupRemoteScenes(now);
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
    
    sendError(clientId, errorMessage) {
        const client = this.clients.get(clientId);
        if (client && client.ws) {
            client.ws.send(JSON.stringify({
                type: 'error',
                message: errorMessage
            }));
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
        const targetId = message.targetClientId;
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
        const targetId = message.targetClientId || this.watchingByWatcher.get(watcherId);
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
                clientInfo: {
                    id: target.id,
                    sessionId: target.sessionId || target.id,
                    persistentClientId: target.persistentId || target.id,
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
    const server = new MouffetteServer(8080);
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
