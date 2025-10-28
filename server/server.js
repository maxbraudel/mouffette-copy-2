const WebSocket = require('ws');
const { v4: uuidv4 } = require('uuid');

class MouffetteServer {
    constructor(port = 8080) {
        this.port = port;
        this.clients = new Map(); // sessionId -> client info
        this.wss = null;
        // Watching relations: targetId -> Set of watcherIds, and watcherId -> targetId
        this.watchersByTarget = new Map();
        this.watchingByWatcher = new Map();
        
        // PHASE 2: Server-side state tracking
        this.uploads = new Map();        // uploadId -> { sender, target, ideaId, startTime, files: [fileIds] }
        this.clientFiles = new Map();    // persistentClientId -> Map(ideaId -> Set(fileId))
        this.sessionsByPersistent = new Map(); // persistentClientId -> Set(sessionId)
    }

    start() {
        // Bind explicitly to 0.0.0.0 to listen on all IPv4 interfaces (LAN accessible)
        this.wss = new WebSocket.Server({ port: this.port, host: '0.0.0.0' });
        
        console.log(`🎯 Mouffette Server started on ws://0.0.0.0:${this.port}`);
        
        this.wss.on('connection', (ws, req) => {
            // Parse query parameters to detect upload channel connections
            const url = new URL(req.url || '/', 'ws://dummy');
            const channel = url.searchParams.get('channel');
            const isUploadChannel = (channel === 'upload');
            
            if (isUploadChannel) {
                // Upload channel: send welcome but don't register as new client
                // Client will identify itself via messages, we'll route through existing client entry
                const tempId = uuidv4(); // Temporary ID for this socket connection tracking
                console.log(`📤 Upload channel connected (temp ID: ${tempId})`);
                
                ws.send(JSON.stringify({
                    type: 'welcome',
                    clientId: tempId, // Client uses this to track which socket received the response
                    message: 'Upload channel ready'
                }));
                
                // Handle messages from upload channel - they should contain senderClientId
                ws.on('message', (data) => {
                    try {
                        const message = JSON.parse(data.toString());
                        // Extract the real client ID from the message
                        const realClientId = message.senderClientId;
                        if (realClientId) {
                            // Forward to handleMessage with the real client ID
                            this.handleMessage(realClientId, message);
                        }
                    } catch (error) {
                        console.error('❌ Error parsing upload channel message:', error);
                    }
                });
                
                ws.on('close', () => {
                    console.log(`📤 Upload channel disconnected (temp ID: ${tempId})`);
                });
                
                ws.on('error', (error) => {
                    console.error(`❌ Upload channel error (temp ID: ${tempId}):`, error);
                });
                
                // Don't add to clients map or broadcast client list
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
            
            // Send welcome message with client ID
            ws.send(JSON.stringify({
                type: 'welcome',
                clientId: clientId,
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
                this.relayToTarget(clientId, message.targetClientId, message);
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
            // Progress/status notifications from target back to sender
            case 'upload_progress':
                this.relayToSender(clientId, message.senderClientId, message);
                break;
            case 'upload_finished':
                this.relayToSender(clientId, message.senderClientId, message);
                break;
            case 'all_files_removed':
                this.relayToSender(clientId, message.senderClientId, message);
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
                // Relay to target client (like uploads). Expect: targetClientId, scene payload
                console.log(`🎬 Received remote_scene_start from ${clientId} to ${message.targetClientId}`);
                this.relayToTarget(clientId, message.targetClientId, message);
                break;
            case 'remote_scene_stop':
                this.relayToTarget(clientId, message.targetClientId, message);
                break;
            case 'remote_scene_stopped':
                this.relayToTarget(clientId, message.targetClientId, message);
                break;
            case 'remote_scene_validation':
                // Relay validation result back to sender
                this.relayToTarget(clientId, message.targetClientId, message);
                break;
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
        // Include senderId for correlation if not present
        if (!message.senderClientId) message.senderClientId = senderId;
        try {
            if (message.type === 'remote_scene_start' || message.type === 'remote_scene_stop') {
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
    const requestedPersistentId = (typeof message.clientId === 'string') ? message.clientId.trim() : '';
    const requestedSessionId = (typeof message.sessionId === 'string') ? message.sessionId.trim() : '';
    const previousPersistentId = client.persistentId;
    const persistentId = requestedPersistentId || previousPersistentId;
    if (!persistentId) {
        console.warn(`⚠️ register missing clientId for socket ${clientId} - machine: ${message.machineName || 'unknown'}`);
        if (client.ws && client.ws.readyState === WebSocket.OPEN) {
            client.ws.send(JSON.stringify({
                type: 'error',
                message: 'clientId is required for register'
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
            for (const [ideaId, fileIds] of clientIdeas.entries()) {
                if (fileIds.size > 0) {
                    ideas.push({
                        ideaId: ideaId,
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
    handleUploadStart(senderId, message) {
        const { targetClientId, uploadId, ideaId, files } = message;
        
        // Validation
        if (!targetClientId || !uploadId || !ideaId) {
            console.warn(`⚠️ upload_start missing required fields from ${senderId}`);
            return this.sendError(senderId, 'Missing targetClientId, uploadId, or ideaId');
        }
        
        const targetPersistentId = this.getPersistentId(targetClientId);
        const senderPersistentId = this.getPersistentId(senderId);
        // Track upload state
        const fileIds = Array.isArray(files) ? files.map(f => f.fileId).filter(Boolean) : [];
        this.uploads.set(uploadId, {
            senderSession: senderId,
            senderPersistent: senderPersistentId,
            targetSession: targetClientId,
            targetPersistent: targetPersistentId,
            ideaId,
            startTime: Date.now(),
            files: fileIds
        });
        
        console.log(`📤 Upload started: ${senderPersistentId}/${senderId} -> ${targetPersistentId}/${targetClientId} [${uploadId}] idea:${ideaId} files:${fileIds.length}`);
        
        // Relay to target
        this.relayToTarget(senderId, targetClientId, message);
    }
    
    handleUploadComplete(senderId, message) {
        const { targetClientId, uploadId, ideaId } = message;
        const upload = this.uploads.get(uploadId);
        
        if (!upload) {
            console.warn(`⚠️ upload_complete for unknown uploadId: ${uploadId}`);
            // Still relay (backward compatibility)
            return this.relayToTarget(senderId, targetClientId, message);
        }
        if (!ideaId) {
            console.warn(`⚠️ upload_complete missing ideaId for ${uploadId}`);
            return this.sendError(senderId, 'Missing ideaId in upload_complete');
        }
        // Track files received by target
        const effectiveTarget = upload.targetPersistent;
        const effectiveIdea = upload.ideaId;
        
        if (!this.clientFiles.has(effectiveTarget)) {
            this.clientFiles.set(effectiveTarget, new Map());
        }
        const targetIdeas = this.clientFiles.get(effectiveTarget);
        if (!targetIdeas.has(effectiveIdea)) {
            targetIdeas.set(effectiveIdea, new Set());
        }
        
        const ideaFiles = targetIdeas.get(effectiveIdea);
        upload.files.forEach(fileId => ideaFiles.add(fileId));
        
        const duration = ((Date.now() - upload.startTime) / 1000).toFixed(1);
        console.log(`✅ Upload complete: ${uploadId} (${duration}s) - ${upload.files.length} files to ${effectiveTarget}:${effectiveIdea}`);
        
        // Cleanup upload tracking
        this.uploads.delete(uploadId);
        
        // Relay to target
        this.relayToTarget(senderId, targetClientId, message);
    }
    
    handleUploadAbort(senderId, message) {
        const { uploadId } = message;
        if (uploadId && this.uploads.has(uploadId)) {
            console.log(`❌ Upload aborted: ${uploadId}`);
            this.uploads.delete(uploadId);
        }
        this.relayToTarget(senderId, message.targetClientId, message);
    }
    
    handleRemoveAllFiles(senderId, message) {
        const { targetClientId, ideaId } = message;
        const targetPersistentId = this.getPersistentId(targetClientId);
        if (!targetClientId || !ideaId) {
            console.warn(`⚠️ remove_all_files missing required fields from ${senderId}`);
            return this.sendError(senderId, 'Missing targetClientId or ideaId');
        }

        const targetFiles = this.clientFiles.get(targetPersistentId);
        if (targetFiles && targetFiles.has(ideaId)) {
            const count = targetFiles.get(ideaId).size;
            targetFiles.delete(ideaId);
            console.log(`🗑️  Removed ${count} files from ${targetPersistentId}:${ideaId}`);
        }
        
        this.relayToTarget(senderId, targetClientId, message);
    }
    
    handleRemoveFile(senderId, message) {
        const { targetClientId, ideaId, fileId } = message;
        
        if (!targetClientId || !fileId || !ideaId) {
            console.warn(`⚠️ remove_file missing required fields from ${senderId}`);
            return this.sendError(senderId, 'Missing targetClientId, ideaId, or fileId');
        }
        
        const targetPersistentId = this.getPersistentId(targetClientId);
        const targetFiles = this.clientFiles.get(targetPersistentId);
        if (targetFiles) {
            const ideaFiles = targetFiles.get(ideaId);
            if (ideaFiles && ideaFiles.has(fileId)) {
                ideaFiles.delete(fileId);
                console.log(`🗑️  Removed file ${fileId} from ${targetPersistentId}:${ideaId}`);
                
                // Cleanup empty idea sets
                if (ideaFiles.size === 0) {
                    targetFiles.delete(ideaId);
                }
            }
        }
        
        this.relayToTarget(senderId, targetClientId, message);
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
        for (const watcherId of watchers) {
            const watcher = this.clients.get(watcherId);
            if (!watcher || !watcher.ws) continue;
            watcher.ws.send(JSON.stringify({
                type: 'cursor_update',
                targetClientId: targetId,
                x, y
            }));
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

// Start the server
const server = new MouffetteServer(8080);
server.start();

// Display stats every 30 seconds
setInterval(() => {
    const stats = server.getStats();
    console.log(`📊 Stats: ${stats.connectedClients} connected, ${stats.registeredClients} registered`);
}, 30000);

// Graceful shutdown
process.on('SIGINT', () => {
    console.log('\n🛑 Shutting down Mouffette Server...');
    if (server.wss) {
        server.wss.close(() => {
            console.log('✅ Server closed gracefully');
            process.exit(0);
        });
    } else {
        process.exit(0);
    }
});
