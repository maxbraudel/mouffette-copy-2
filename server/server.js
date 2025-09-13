const WebSocket = require('ws');
const { v4: uuidv4 } = require('uuid');

class MouffetteServer {
    constructor(port = 8080) {
        this.port = port;
        this.clients = new Map(); // clientId -> client info
        this.wss = null;
    // Watching relations: targetId -> Set of watcherIds, and watcherId -> targetId
    this.watchersByTarget = new Map();
    this.watchingByWatcher = new Map();
    }

    start() {
        // Bind explicitly to 0.0.0.0 to listen on all IPv4 interfaces (LAN accessible)
        this.wss = new WebSocket.Server({ port: this.port, host: '0.0.0.0' });
        
        console.log(`🎯 Mouffette Server started on ws://0.0.0.0:${this.port}`);
        
        this.wss.on('connection', (ws, req) => {
            const clientId = uuidv4();
            const clientInfo = {
                id: clientId,
                ws: ws,
                machineName: null,
                screens: [],
                volumePercent: -1,
                status: 'connected',
                connectedAt: new Date().toISOString()
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
                    this.handleMessage(clientId, message);
                } catch (error) {
                    console.error('❌ Error parsing message:', error);
                    ws.send(JSON.stringify({
                        type: 'error',
                        message: 'Invalid JSON format'
                    }));
                }
            });
            
            ws.on('close', () => {
                console.log(`📱 Client disconnected: ${clientId}`);
                // Clean up watching relationships
                const targetId = this.watchingByWatcher.get(clientId);
                if (targetId) {
                    this.watchingByWatcher.delete(clientId);
                    const set = this.watchersByTarget.get(targetId);
                    if (set) {
                        set.delete(clientId);
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
                const watchers = this.watchersByTarget.get(clientId);
                if (watchers) {
                    this.watchersByTarget.delete(clientId);
                    // Notify this (now targetless) client it's no longer watched
                    const targetClient = this.clients.get(clientId);
                    if (targetClient && targetClient.ws) {
                        targetClient.ws.send(JSON.stringify({ type: 'watch_status', watched: false }));
                    }
                }
                this.clients.delete(clientId);
                this.broadcastClientList();
            });
            
            ws.on('error', (error) => {
                console.error(`❌ WebSocket error for client ${clientId}:`, error);
                // Similar cleanup on error
                const targetId = this.watchingByWatcher.get(clientId);
                if (targetId) {
                    this.watchingByWatcher.delete(clientId);
                    const set = this.watchersByTarget.get(targetId);
                    if (set) {
                        set.delete(clientId);
                        if (set.size === 0) {
                            this.watchersByTarget.delete(targetId);
                            const targetClient = this.clients.get(targetId);
                            if (targetClient && targetClient.ws) {
                                targetClient.ws.send(JSON.stringify({ type: 'watch_status', watched: false }));
                            }
                        }
                    }
                }
                const watchers = this.watchersByTarget.get(clientId);
                if (watchers) {
                    this.watchersByTarget.delete(clientId);
                    const targetClient = this.clients.get(clientId);
                    if (targetClient && targetClient.ws) {
                        targetClient.ws.send(JSON.stringify({ type: 'watch_status', watched: false }));
                    }
                }
                this.clients.delete(clientId);
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
            default:
                console.log(`⚠️ Unknown message type: ${message.type}`);
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
        // Reply with current known screens info for the target
        requester.ws.send(JSON.stringify({
            type: 'screens_info',
            clientInfo: {
                id: target.id,
                machineName: target.machineName,
                platform: target.platform,
                screens: target.screens,
                volumePercent: target.volumePercent
            }
        }));
    }
    
    handleRegister(clientId, message) {
        const client = this.clients.get(clientId);
        if (!client) return;
        
        // Only update fields that are explicitly provided to avoid clobbering identity on partial updates
        if (Object.prototype.hasOwnProperty.call(message, 'machineName')) {
            client.machineName = message.machineName || `Client-${clientId.slice(0, 8)}`;
        } else if (!client.machineName) {
            client.machineName = `Client-${clientId.slice(0, 8)}`;
        }
        if (Object.prototype.hasOwnProperty.call(message, 'screens')) {
            client.screens = message.screens || [];
        }
        if (typeof message.volumePercent === 'number') {
            client.volumePercent = Math.max(0, Math.min(100, Math.round(message.volumePercent)));
        }
        if (Object.prototype.hasOwnProperty.call(message, 'platform')) {
            client.platform = message.platform || 'unknown';
        } else if (!client.platform) {
            client.platform = 'unknown';
        }
        
        console.log(`✅ Client registered: ${client.machineName} (${client.platform}) with ${client.screens.length} screen(s)`);
        
        // Send confirmation
        client.ws.send(JSON.stringify({
            type: 'registration_confirmed',
            clientInfo: {
                id: clientId,
                machineName: client.machineName,
                screens: client.screens,
                platform: client.platform,
                volumePercent: client.volumePercent
            }
        }));
        
        // Broadcast updated client list
        this.broadcastClientList();
    // Notify watchers of this target with fresh screens
    this.notifyWatchersOfTarget(clientId);
    }
    
    sendClientList(clientId) {
        const client = this.clients.get(clientId);
        if (!client) return;
        
        const clientList = Array.from(this.clients.values())
            .filter(c => c.id !== clientId && c.machineName) // Don't include self and only registered clients
            .map(c => ({
                id: c.id,
                machineName: c.machineName,
                screens: c.screens,
                platform: c.platform,
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
                    machineName: target.machineName,
                    platform: target.platform,
                    screens: target.screens,
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
