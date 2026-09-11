const WebSocket = require('ws');
const { v4: uuidv4 } = require('uuid');
const crypto = require('node:crypto');

const CURSOR_DEBUG = !!process.env.MOUFFETTE_CURSOR_DEBUG;
const CANONICAL_UUID_PATTERN = /^[0-9a-f]{8}-[0-9a-f]{4}-[1-5][0-9a-f]{3}-[89ab][0-9a-f]{3}-[0-9a-f]{12}$/i;

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
        this.sessionsByPersistent = new Map(); // persistentClientId -> Set(sessionId)
        // target session -> { ownerId, sceneInstanceId }. This is intentionally
        // small protocol state used only to terminate orphaned remote scenes
        // when their authenticated control owner disconnects.
        this.remoteScenesByTarget = new Map();
        
        // PHASE 2: Active canvas tracking (CRITICAL for canvasSessionId validation)
        this.activeCanvases = new Map(); // persistentClientId -> Set(canvasSessionId)
        
        // PHASE 1: Upload timeout configuration
        this.UPLOAD_TIMEOUT_MS = 30 * 60 * 1000; // 30 minutes
        this.UPLOAD_TARGET_ACK_TIMEOUT_MS = 30 * 1000;
        this.REMOVAL_ACK_TIMEOUT_MS = 30 * 1000;
        this.MAX_UPLOAD_FILES = 256;
        this.MAX_UPLOAD_FILE_BYTES = 16 * 1024 * 1024 * 1024;
        this.MAX_UPLOAD_TOTAL_BYTES = 64 * 1024 * 1024 * 1024;
        this.MAX_UPLOAD_CHUNK_BASE64_LENGTH = Math.ceil((128 * 1024) / 3) * 4;
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
        this.wss = new WebSocket.Server({ port: this.port, host: '0.0.0.0' });
        
        console.log(`🎯 Mouffette Server started on ws://0.0.0.0:${this.port}`);
        
        // PHASE 1: Start upload cleanup interval (every minute)
        this.uploadCleanupInterval = setInterval(() => {
            this.cleanupStalledUploads();
        }, 60000); // 1 minute
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
                    this.unregisterUploadSocket(boundClient, ws);
                    console.log(`📤 Upload channel disconnected for ${boundClient.id}`);
                });
                
                ws.on('error', (error) => {
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
        });
    }

    handleRemoteSceneClientDeparture(clientId) {
        if (!clientId) return;

        // Notify the owner before removing a departing/replaced target. In a
        // logical-session replacement there is no client-list gap, so this
        // correlated failure is what prevents the host retaining a ghost run.
        const targetRun = this.remoteScenesByTarget.get(clientId);
        if (targetRun) {
            const owner = this.clients.get(targetRun.ownerId);
            if (owner && owner.ws && owner.ws.readyState === WebSocket.OPEN) {
                owner.ws.send(JSON.stringify({
                    type: 'remote_scene_stopped',
                    senderClientId: clientId,
                    targetClientId: targetRun.ownerId,
                    sceneInstanceId: targetRun.sceneInstanceId,
                    success: false,
                    error: 'Remote target connection was lost'
                }));
            }
        }
        this.remoteScenesByTarget.delete(clientId);

        // If the authenticated owner disappears, issue the same correlated
        // STOP the owner would have sent. This message is server-authored from
        // the recorded run, so a client-provided senderClientId is never used.
        for (const [targetId, run] of this.remoteScenesByTarget) {
            if (!run || run.ownerId !== clientId) continue;

            const target = this.clients.get(targetId);
            if (target && target.ws && target.ws.readyState === WebSocket.OPEN) {
                try {
                    target.ws.send(JSON.stringify({
                        type: 'remote_scene_stop',
                        targetClientId: targetId,
                        senderClientId: clientId,
                        sceneInstanceId: run.sceneInstanceId,
                        ownerDisconnected: true
                    }));
                    console.log(`🛑 Stopped orphaned remote scene ${run.sceneInstanceId} on ${targetId}`);
                } catch (error) {
                    console.error('❌ Failed to stop orphaned remote scene:', error);
                }
            }
            this.remoteScenesByTarget.delete(targetId);
        }
    }
    
    handleMessage(clientId, message) {
        const client = this.clients.get(clientId);
        if (!client) return;
        
        console.log(`📨 Message from ${clientId}:`, message.type);
        
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
                this.handleUploadStart(clientId, message);
                break;
            case 'upload_chunk':
                this.handleUploadChunk(clientId, message);
                break;
            case 'upload_complete':
                this.handleUploadComplete(clientId, message);
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
            case 'upload_finished':
                this.handleUploadFinished(clientId, message);
                break;
            case 'upload_rejected':
                this.handleUploadRejected(clientId, message);
                break;
            case 'all_files_removed':
                this.handleAllFilesRemoved(clientId, message);
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
            case 'remote_scene_start': {
                // Relay to target client (like uploads). Expect: targetClientId, scene payload
                console.log(`🎬 Received remote_scene_start from ${clientId} to ${message.targetClientId}`);
                const targetId = this.resolveClientId(message.targetClientId);
                const sceneInstanceId = message.scene && typeof message.scene === 'object'
                    ? message.scene.sceneInstanceId : null;
                if (targetId && typeof sceneInstanceId === 'string'
                    && sceneInstanceId.length >= 1 && sceneInstanceId.length <= 128) {
                    const activeRun = this.remoteScenesByTarget.get(targetId);
                    if (!activeRun
                        || (activeRun.ownerId === clientId
                            && activeRun.sceneInstanceId === sceneInstanceId)) {
                        this.remoteScenesByTarget.set(targetId, {
                            ownerId: clientId,
                            sceneInstanceId
                        });
                    }
                }
                this.relayToTarget(clientId, message.targetClientId, message);
                break;
            }
            case 'remote_scene_activate':
                this.relayToTarget(clientId, message.targetClientId, message);
                break;
            case 'remote_scene_video_sync':
                this.relayToTarget(clientId, message.targetClientId, message);
                break;
            case 'remote_scene_stop': {
                const targetId = this.resolveClientId(message.targetClientId);
                const activeRun = targetId ? this.remoteScenesByTarget.get(targetId) : null;
                if (activeRun && activeRun.ownerId === clientId
                    && (typeof message.sceneInstanceId !== 'string'
                        || message.sceneInstanceId.length === 0)) {
                    // Upgrade the legacy generic STOP to the recorded correlated
                    // run whenever possible.
                    message.sceneInstanceId = activeRun.sceneInstanceId;
                }
                this.relayToTarget(clientId, message.targetClientId, message);
                break;
            }
            case 'remote_scene_stopped': {
                const activeRun = this.remoteScenesByTarget.get(clientId);
                if (message.success === true && activeRun
                    && activeRun.sceneInstanceId === message.sceneInstanceId) {
                    this.remoteScenesByTarget.delete(clientId);
                }
                this.relayToTarget(clientId, message.targetClientId, message);
                break;
            }
            case 'remote_scene_validation': {
                // Relay validation result back to sender
                const activeRun = this.remoteScenesByTarget.get(clientId);
                if (message.success === false && activeRun
                    && activeRun.sceneInstanceId === message.sceneInstanceId) {
                    this.remoteScenesByTarget.delete(clientId);
                }
                this.relayToTarget(clientId, message.targetClientId, message);
                break;
            }
            case 'remote_scene_launched':
                // Relay launched confirmation back to sender
                this.relayToTarget(clientId, message.targetClientId, message);
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
        
        if (!targetClient || !targetClient.ws) {
            const senderClient = this.clients.get(senderId);
            if (senderClient && senderClient.ws) {
                senderClient.ws.send(JSON.stringify({
                    type: 'error',
                    message: 'Target client not found',
                }));
            }
            return;
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
        } catch (e) {
            console.error('❌ Relay to target failed:', e);
        }
    }

    // Helper to relay a message from target -> sender
    relayToSender(targetId, senderClientId, message) {
        const senderClient = this.clients.get(senderClientId);
        if (!senderClient || !senderClient.ws) return;
        if (!message.targetClientId) message.targetClientId = targetId;
        try {
            senderClient.ws.send(JSON.stringify(message));
        } catch (e) {
            console.error('❌ Relay to sender failed:', e);
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
            if (removal && (removal.senderSession === clientId
                || removal.targetSession === clientId)) {
                this.pendingRemovals.delete(removalId);
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

    handleUploadStart(senderId, message) {
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
        if (this.uploads.has(uploadId)) {
            rejectStart('Upload identifier is already active');
            return;
        }

        const fileIds = [];
        const seenFileIds = new Set();
        const seenMediaIds = new Set();
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
        }

        const targetPersistentId = this.getPersistentId(targetClientId);
        const senderPersistentId = this.getPersistentId(senderId);
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
            totalSize,
            awaitingTargetValidation: false
        });

        console.log(`   Files: ${fileIds.length} file(s)`);
        const relayed = { ...message, senderClientId: senderId };
        this.relayToTarget(senderId, resolvedTarget, relayed);
    }

    handleUploadChunk(senderId, message) {
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
        if (upload.awaitingTargetValidation
            || message.canvasSessionId !== upload.canvasSessionId
            || typeof message.fileId !== 'string' || !upload.fileSet.has(message.fileId)
            || !Number.isInteger(message.chunkIndex) || message.chunkIndex < 0
            || typeof message.data !== 'string' || message.data.length < 1
            || message.data.length > this.MAX_UPLOAD_CHUNK_BASE64_LENGTH) {
            this.rejectTrackedUpload(uploadId, 'Invalid upload chunk');
            return;
        }

        const relayed = {
            ...message,
            senderClientId: upload.senderSession,
            targetClientId: upload.targetPersistent,
            targetPersistentClientId: upload.targetPersistent
        };
        upload.lastActivity = Date.now();
        this.relayToTarget(upload.senderSession, upload.targetSession, relayed);
    }

    handleUploadComplete(senderId, message) {
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
        if (canvasSessionId !== upload.canvasSessionId) {
            this.rejectTrackedUpload(uploadId, 'Upload session identifier mismatch');
            return;
        }
        if (upload.awaitingTargetValidation) {
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
        this.relayToTarget(upload.senderSession, upload.targetSession, relayed);
    }

    handleUploadProgress(targetId, message) {
        const upload = this.uploads.get(message.uploadId);
        if (!upload || !this.isExpectedUploadTarget(upload, targetId)) return;
        upload.lastActivity = Date.now();

        const percent = Number.isInteger(message.percent) ? Math.max(0, Math.min(99, message.percent)) : 0;
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
        this.relayToTarget(senderId, upload.targetSession, {
            ...message,
            senderClientId: upload.senderSession,
            canvasSessionId: upload.canvasSessionId
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

        for (const [removalId, removal] of this.pendingRemovals) {
            if (!removal || now - removal.createdAt > this.REMOVAL_ACK_TIMEOUT_MS) {
                this.pendingRemovals.delete(removalId);
            }
        }
    }
    
    handleRemoveAllFiles(senderId, message) {
        // PHASE 2: Extract targetClientId with fallback
        const targetClientId = message.targetPersistentClientId || message.targetClientId;
        const { canvasSessionId, removalId } = message;
        if (!targetClientId || !canvasSessionId
            || typeof removalId !== 'string' || !CANONICAL_UUID_PATTERN.test(removalId)) {
            console.warn(`⚠️ remove_all_files missing or invalid required fields from ${senderId}`);
            return this.sendError(senderId,
                'Missing or invalid targetClientId, canvasSessionId, or removalId');
        }
        if (this.pendingRemovals.has(removalId)) {
            return this.sendError(senderId, 'Removal identifier is already active');
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
        const entries = [];

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

        if (entries.length === 0) {
            console.warn(`⚠️ Unauthorized remove_all_files from ${senderId} for ${targetPersistentId}:${canvasSessionId}`);
            return this.sendError(senderId, 'Not authorized to remove files from this canvas');
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
        console.log(`🗑️  Requested removal of ${entries.length} owned file(s) from ${targetPersistentId}:${canvasSessionId}`);
        this.relayToTarget(senderId, resolvedTarget, message);
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
