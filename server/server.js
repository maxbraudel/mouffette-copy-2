const WebSocket = require('ws');
const { v4: uuidv4 } = require('uuid');
const crypto = require('node:crypto');
const { loadServerConfig } = require('./config');
const { monotonicNow } = require('./suspend_inclusive_clock');
const { PROTOCOL_VERSION, createChallenge, verifyAuthResponse } = require('./device_auth');
const { RemoteSessionRegistry, TERMINAL_PHASES } = require('./remote_session_registry');
const { ProtocolMetrics } = require('./protocol_metrics');
const { isAllowedMediaExtension } = require('./media_format_contract');
const {
    SCENE_PHASES, SceneRunRegistry, computeSceneDigest, isPlainObject,
} = require('./scene_run_registry');

const DEFAULT_CONFIG = loadServerConfig();
const CURSOR_DEBUG = DEFAULT_CONFIG.cursorDebug;
const CANONICAL_UUID_PATTERN = /^[0-9a-f]{8}-[0-9a-f]{4}-[1-5][0-9a-f]{3}-[89ab][0-9a-f]{3}-[0-9a-f]{12}$/i;
const OPAQUE_ID_PATTERN = /^[A-Za-z0-9_-]{1,128}$/;
const SHA256_PATTERN = /^[0-9a-f]{64}$/;
// Protocol v7 is a hard cut-over. Obsolete names are rejected at the envelope
// boundary and are never translated.
const REMOVED_MESSAGE_TYPES = new Set([
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
const REMOVED_WIRE_FIELDS = Object.freeze([
    'clientId', 'persistentClientId', 'persistentId', 'deviceId', 'sessionId',
    'canvasSessionId', 'targetClientId', 'targetPersistentClientId',
    'senderClientId', 'senderPersistentClientId', 'senderId', 'targetId',
]);

function findRemovedWireField(value) {
    if (!value || typeof value !== 'object') return null;
    const pending = [value];
    const visited = new Set();
    while (pending.length > 0) {
        const current = pending.pop();
        if (!current || typeof current !== 'object' || visited.has(current)) continue;
        visited.add(current);
        for (const [key, nested] of Object.entries(current)) {
            if (REMOVED_WIRE_FIELDS.includes(key)) return key;
            if (nested && typeof nested === 'object') pending.push(nested);
        }
    }
    return null;
}

function hasOnlyKeys(object, required, optional = []) {
    const allowed = new Set([...required, ...optional]);
    return required.every(key => Object.prototype.hasOwnProperty.call(object, key))
        && Object.keys(object).every(key => allowed.has(key));
}

function isBoundedInteger(value, minimum, maximum) {
    return Number.isSafeInteger(value) && value >= minimum && value <= maximum;
}

function normalizeUiZone(value) {
    const keys = ['type', 'x', 'y', 'width', 'height'];
    const types = new Set(['taskbar', 'menu_bar', 'dock']);
    if (!isPlainObject(value) || !hasOnlyKeys(value, keys)
        || typeof value.type !== 'string' || !types.has(value.type)
        || !isBoundedInteger(value.x, -1_000_000, 1_000_000)
        || !isBoundedInteger(value.y, -1_000_000, 1_000_000)
        || !isBoundedInteger(value.width, 0, 100_000)
        || !isBoundedInteger(value.height, 0, 100_000)) {
        return null;
    }
    return { type: value.type, x: value.x, y: value.y,
        width: value.width, height: value.height };
}

function normalizeScreen(value) {
    const required = ['id', 'width', 'height', 'x', 'y', 'primary'];
    if (!isPlainObject(value) || !hasOnlyKeys(value, required, ['uiZones'])
        || !isBoundedInteger(value.id, 0, 1_000_000)
        || !isBoundedInteger(value.width, 1, 100_000)
        || !isBoundedInteger(value.height, 1, 100_000)
        || !isBoundedInteger(value.x, -1_000_000, 1_000_000)
        || !isBoundedInteger(value.y, -1_000_000, 1_000_000)
        || typeof value.primary !== 'boolean') {
        return null;
    }
    const zones = value.uiZones === undefined ? [] : value.uiZones;
    if (!Array.isArray(zones) || zones.length > 16) return null;
    const normalizedZones = zones.map(normalizeUiZone);
    if (normalizedZones.some(zone => zone === null)) return null;
    const screen = { id: value.id, width: value.width, height: value.height,
        x: value.x, y: value.y, primary: value.primary };
    if (normalizedZones.length > 0) screen.uiZones = normalizedZones;
    return screen;
}

function normalizeScreens(value, maximumScreens) {
    if (!Array.isArray(value) || value.length > maximumScreens) return null;
    const screens = value.map(normalizeScreen);
    if (screens.some(screen => screen === null)) return null;
    const ids = new Set(screens.map(screen => screen.id));
    return ids.size === screens.length ? screens : null;
}

const SCENE_COMMON_MEDIA_KEYS = Object.freeze([
    'mediaId', 'fileId', 'fileName', 'type',
    'x', 'y', 'width', 'height', 'baseWidth', 'baseHeight',
    'visible', 'z', 'autoDisplay', 'autoDisplayDelayMs',
    'autoHide', 'autoHideDelayMs', 'hideWhenVideoEnds',
    'fadeInSeconds', 'fadeOutSeconds', 'contentOpacity', 'spans',
]);
const SCENE_TEXT_MEDIA_KEYS = Object.freeze([
    'text', 'fontFamily', 'fontItalic',
    'fontUnderline', 'fontUppercase', 'fontWeight', 'fontPixelSize',
    'textColor', 'textOutlineWidthPx',
    'textBorderColor', 'textFitToTextEnabled', 'textHighlightEnabled',
    'textHighlightColor', 'horizontalAlignment',
    'verticalAlignment',
]);
const SCENE_VIDEO_MEDIA_KEYS = Object.freeze([
    'assetId', 'autoPlay', 'autoPlayDelayMs', 'autoPause',
    'autoPauseDelayMs', 'muted', 'volume', 'continuousLoop',
    'repeatEnabled', 'repeatCount', 'autoUnmute', 'autoUnmuteDelayMs',
    'autoMute', 'autoMuteDelayMs', 'muteWhenVideoEnds',
    'audioFadeInSeconds', 'audioFadeOutSeconds', 'startPositionMs',
]);
const SCENE_SPAN_KEYS = Object.freeze([
    'screenId', 'normX', 'normY', 'normW', 'normH',
    'spanDestNormX', 'spanDestNormY', 'spanDestNormW', 'spanDestNormH',
    'spanSourceNormX', 'spanSourceNormY', 'spanSourceNormW', 'spanSourceNormH',
]);

function isFiniteInRange(value, minimum, maximum) {
    return Number.isFinite(value) && value >= minimum && value <= maximum;
}

function isUnitRectangle(x, y, width, height) {
    const epsilon = 0.0001;
    return isFiniteInRange(x, -epsilon, 1 + epsilon)
        && isFiniteInRange(y, -epsilon, 1 + epsilon)
        && isFiniteInRange(width, Number.EPSILON, 1 + epsilon)
        && isFiniteInRange(height, Number.EPSILON, 1 + epsilon)
        && x + width <= 1 + epsilon
        && y + height <= 1 + epsilon;
}

function isCanonicalSceneSpan(span, screenIds) {
    if (!isPlainObject(span) || !hasOnlyKeys(span, SCENE_SPAN_KEYS)
        || !isBoundedInteger(span.screenId, 0, 1_000_000)
        || !screenIds.has(span.screenId)
        || !isFiniteInRange(span.normX, -10_000, 10_000)
        || !isFiniteInRange(span.normY, -10_000, 10_000)
        || !isFiniteInRange(span.normW, Number.EPSILON, 10_000)
        || !isFiniteInRange(span.normH, Number.EPSILON, 10_000)
        || !isUnitRectangle(span.spanDestNormX, span.spanDestNormY,
            span.spanDestNormW, span.spanDestNormH)
        || !isUnitRectangle(span.spanSourceNormX, span.spanSourceNormY,
            span.spanSourceNormW, span.spanSourceNormH)) {
        return false;
    }
    return true;
}

function isCanonicalSceneMedia(item, screenIds) {
    if (!isPlainObject(item) || !OPAQUE_ID_PATTERN.test(item.mediaId)
        || !['image', 'video', 'text'].includes(item.type)
        || typeof item.fileId !== 'string' || item.fileId.length > 128
        || typeof item.fileName !== 'string' || item.fileName.length > 1024
        || !isFiniteInRange(item.x, -100_000_000, 100_000_000)
        || !isFiniteInRange(item.y, -100_000_000, 100_000_000)
        || !isFiniteInRange(item.width, Number.EPSILON, 10_000_000)
        || !isFiniteInRange(item.height, Number.EPSILON, 10_000_000)
        || !isBoundedInteger(item.baseWidth, 0, 10_000_000)
        || !isBoundedInteger(item.baseHeight, 0, 10_000_000)
        || typeof item.visible !== 'boolean'
        || !isFiniteInRange(item.z, -100_000_000, 100_000_000)
        || typeof item.autoDisplay !== 'boolean'
        || !isBoundedInteger(item.autoDisplayDelayMs, 0, 604_800_000)
        || typeof item.autoHide !== 'boolean'
        || !isBoundedInteger(item.autoHideDelayMs, -604_800_000, 604_800_000)
        || typeof item.hideWhenVideoEnds !== 'boolean'
        || !isFiniteInRange(item.fadeInSeconds, 0, 3600)
        || !isFiniteInRange(item.fadeOutSeconds, 0, 3600)
        || !isFiniteInRange(item.contentOpacity, 0, 1)
        || !Array.isArray(item.spans)
        || item.spans.length > 64) {
        return false;
    }
    const spanScreenIds = new Set();
    for (const span of item.spans) {
        if (!isCanonicalSceneSpan(span, screenIds)
            || spanScreenIds.has(span.screenId)) return false;
        spanScreenIds.add(span.screenId);
    }

    if (item.type === 'image') {
        return hasOnlyKeys(item, [...SCENE_COMMON_MEDIA_KEYS, 'assetId'])
            && SHA256_PATTERN.test(item.fileId)
            && OPAQUE_ID_PATTERN.test(item.assetId)
            && item.fileName.length > 0;
    }
    if (item.type === 'text') {
        const horizontalAlignments = new Set(['left', 'center', 'right']);
        const verticalAlignments = new Set(['top', 'center', 'bottom']);
        return hasOnlyKeys(item, [...SCENE_COMMON_MEDIA_KEYS, ...SCENE_TEXT_MEDIA_KEYS])
            && item.fileId === '' && item.fileName === ''
            && typeof item.text === 'string' && item.text.length <= 1_000_000
            && typeof item.fontFamily === 'string'
            && item.fontFamily.length > 0 && item.fontFamily.length <= 1024
            && typeof item.fontItalic === 'boolean'
            && typeof item.fontUnderline === 'boolean'
            && typeof item.fontUppercase === 'boolean'
            && isBoundedInteger(item.fontWeight, 1, 900)
            && isBoundedInteger(item.fontPixelSize, 1, 4096)
            && typeof item.textColor === 'string' && item.textColor.length <= 64
            && isFiniteInRange(item.textOutlineWidthPx, 0, 100_000)
            && typeof item.textBorderColor === 'string'
            && item.textBorderColor.length <= 64
            && typeof item.textFitToTextEnabled === 'boolean'
            && typeof item.textHighlightEnabled === 'boolean'
            && typeof item.textHighlightColor === 'string'
            && item.textHighlightColor.length <= 64
            && horizontalAlignments.has(item.horizontalAlignment)
            && verticalAlignments.has(item.verticalAlignment);
    }
    return hasOnlyKeys(item, [...SCENE_COMMON_MEDIA_KEYS, ...SCENE_VIDEO_MEDIA_KEYS], ['endPositionMs'])
        && SHA256_PATTERN.test(item.fileId)
        && OPAQUE_ID_PATTERN.test(item.assetId)
        && item.fileName.length > 0
        && typeof item.autoPlay === 'boolean'
        && isBoundedInteger(item.autoPlayDelayMs, 0, 604_800_000)
        && typeof item.autoPause === 'boolean'
        && isBoundedInteger(item.autoPauseDelayMs, 0, 604_800_000)
        && typeof item.muted === 'boolean'
        && isFiniteInRange(item.volume, 0, 1)
        && typeof item.continuousLoop === 'boolean'
        && typeof item.repeatEnabled === 'boolean'
        && isBoundedInteger(item.repeatCount, 0, 1_000_000)
        && typeof item.autoUnmute === 'boolean'
        && isBoundedInteger(item.autoUnmuteDelayMs, 0, 604_800_000)
        && typeof item.autoMute === 'boolean'
        && isBoundedInteger(item.autoMuteDelayMs, -604_800_000, 604_800_000)
        && typeof item.muteWhenVideoEnds === 'boolean'
        && isFiniteInRange(item.audioFadeInSeconds, 0, 3600)
        && isFiniteInRange(item.audioFadeOutSeconds, 0, 3600)
        && isBoundedInteger(item.startPositionMs, 0, 604_800_000)
        && (item.endPositionMs === undefined
            || (isBoundedInteger(item.endPositionMs, 1, 604_800_000)
                && item.endPositionMs > item.startPositionMs));
}

function isCanonicalScene(scene, maximumScreens, maximumMedia) {
    if (!isPlainObject(scene)
        || !hasOnlyKeys(scene, ['renderSchemaVersion', 'screens', 'media'])
        || scene.renderSchemaVersion !== 2
        || !Array.isArray(scene.screens) || scene.screens.length < 1
        || scene.screens.length > maximumScreens
        || !Array.isArray(scene.media) || scene.media.length < 1
        || scene.media.length > maximumMedia
        || !normalizeScreens(scene.screens, maximumScreens)) {
        return false;
    }
    const screenIds = new Set(scene.screens.map(screen => screen.id));
    const mediaIds = new Set();
    let spanCount = 0;
    for (const item of scene.media) {
        if (!isCanonicalSceneMedia(item, screenIds)
            || mediaIds.has(item.mediaId)) return false;
        mediaIds.add(item.mediaId);
        spanCount += item.spans.length;
        if (spanCount > 4096) return false;
    }
    return true;
}

// ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
// MOUFFETTE SERVER - PROTOCOL V7 IDENTITY BOUNDARY
// ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
//
// Protocol v7 authenticates one installation key, derives one addressable
// endpoint per application instance, and keeps transport runtime identity
// separate. Removed wire identifiers are never accepted as aliases.
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
        this.monotonicNow = options.monotonicNow || monotonicNow;
        this.clockUncertaintyMs = this.monotonicNow.uncertaintyMs || 0;
        this.protocolVersion = PROTOCOL_VERSION;
        this.serverBootId = uuidv4();
        this.protocolLogger = options.protocolLogger === undefined
            ? console.log : options.protocolLogger;
        this.metrics = options.metrics || new ProtocolMetrics({ logger: options.metricLogger });
        this.clients = new Map(); // transport key -> authenticated endpoint state
        this.presenceRevision = 0;
        this.presenceSignature = '';
        this.endpointPresence = new Map();
        // Unique within serverBootId, without retaining one counter per endpoint
        // ever observed. The authority index contains only admitted transports.
        this.connectionGenerationSequence = 0;
        this.currentTransportByEndpoint = new Map();
        this.wss = null;
        this.uploads = new Map(); // uploadId -> protocol-v7 endpoint/session state
        this.uploadTombstones = new Map(); // uploadId -> bounded terminal result
        this.pendingAssetRemovals = new Map(); // removalId -> immutable session-scoped removal
        this.assetRemovalTombstones = new Map(); // removalId -> bounded committed/error result
        this.connectionsByEndpoint = new Map(); // endpointId -> Set(connectionId)
        this.sessionAssets = new Map(); // remoteSessionId -> Map(assetId -> validated metadata)

        this.UPLOAD_TIMEOUT_MS = this.config.uploadIdleTimeoutMs;
        this.UPLOAD_TARGET_ACK_TIMEOUT_MS = this.config.uploadTargetAckTimeoutMs;
        this.UPLOAD_RESULT_TOMBSTONE_TTL_MS =
            this.config.uploadResultTombstoneTtlMs;
        this.REMOVAL_ACK_TIMEOUT_MS = this.config.removalAckTimeoutMs;
        this.ASSET_REMOVAL_TOMBSTONE_TTL_MS =
            this.config.assetRemovalTombstoneTtlMs;
        this.MAX_UPLOAD_FILES = 256;
        this.MAX_UPLOAD_FILE_BYTES = 16 * 1024 * 1024 * 1024;
        this.MAX_UPLOAD_TOTAL_BYTES = 64 * 1024 * 1024 * 1024;
        this.MAX_UPLOAD_CHUNK_BASE64_LENGTH = Math.ceil((128 * 1024) / 3) * 4;
        this.MAX_TARGET_BUFFERED_UPLOAD_BYTES = 8 * 1024 * 1024;
        this.MAX_UPLOAD_UNACKNOWLEDGED_BYTES = 1024 * 1024;
        this.MAX_TARGET_STREAMING_UPLOADS = 8;
        this.MAX_OWNER_BUFFERED_CURSOR_BYTES = 64 * 1024;
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
        this.UPLOAD_CHANNEL_TOKEN_TTL_MS = this.config.uploadChannelTokenTtlMs;
        this.uploadChannelTokens = new Map();
        this.uploadSocketsByClient = new Map();
        this.uploadChannelMessageTypes = new Set([
            'upload_start', 'upload_resume', 'upload_chunk', 'upload_complete', 'upload_abort'
        ]);
        this.uploadCleanupInterval = null;
        this.leaseSweepInterval = null;
        this.remoteSessions = new RemoteSessionRegistry({
            leaseTimeoutMs: this.config.leaseTimeoutMs + this.config.sessionRecoveryTimeoutMs
                - this.clockUncertaintyMs,
            recoveryTimeoutMs: this.config.sessionRecoveryTimeoutMs - this.clockUncertaintyMs,
            openTimeoutMs: this.config.remoteSessionOpenTimeoutMs - this.clockUncertaintyMs,
            openRequestTtlMs: this.config.remoteSessionOpenRequestTtlMs,
            tombstoneTtlMs: this.config.remoteSessionTombstoneTtlMs,
            cleanupRetryInitialMs: this.config.remoteSessionTeardownRetryInitialMs,
            cleanupRetryMaxMs: this.config.remoteSessionTeardownRetryMaxMs,
            // A retained OPEN request must never have a larger cardinality
            // budget than the terminal proof used to answer its replay.
            maximumTombstones: this.MAX_REMOTE_SCENE_TOMBSTONES,
            maximumOpenRequests: this.MAX_REMOTE_SCENE_TOMBSTONES,
            monotonicNow: this.monotonicNow,
            epochNow: this.epochNow,
        });
        this.sceneRuns = new SceneRunRegistry({
            prepareTimeoutMs: this.config.scenePrepareTimeoutMs,
            activationLeadMs: this.config.sceneActivationLeadMs,
            startedAckTimeoutMs: this.config.sceneStartedAckTimeoutMs,
            maximumClockUncertaintyMs: this.config.sceneMaxClockSkewMs,
            maximumStartSkewMs: this.config.sceneMaxStartSkewMs,
            stopTimeoutMs: this.config.sceneStopTimeoutMs,
            tombstoneTtlMs: this.config.sceneRunTombstoneTtlMs,
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
            this.cleanupStalledUploads();
        }, this.config.uploadSweepIntervalMs);
        this.leaseSweepInterval = setInterval(() => this.sweepRemoteSessionLeases(),
            this.config.sessionLeaseSweepIntervalMs);
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
                        this.abortUploadsForUploadSocket(boundClient.id, ws,
                            'Dedicated upload connection closed');
                    }
                    this.unregisterUploadSocket(boundClient, ws);
                    console.log(`📤 Upload channel disconnected for ${boundClient.id}`);
                });
                
                ws.on('error', (error) => {
                    if (!ws.mouffettePreserveSessionUploads) {
                        this.abortUploadsForUploadSocket(boundClient.id, ws,
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
            }, this.config.authChallengeTimeoutMs);
            
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
                if (clientInfo.replaced || this.clients.get(clientInfo.id) !== clientInfo) {
                    return;
                }
                const finalId = clientInfo.id;
                console.log(`📱 Client disconnected: ${finalId}`);
                const protectedByLease = this.handleRemoteSessionDeparture(clientInfo);
                this.revokeUploadChannelsForClient(clientInfo, protectedByLease);
                if (!protectedByLease) {
                    this.abortUploadsForClient(finalId);
                }
                if (clientInfo.endpointId) {
                    this.unregisterConnectionForEndpoint(clientInfo.endpointId, finalId);
                }
                this.clients.delete(finalId);
                this.forgetCurrentTransport(clientInfo);
                this.broadcastClientList();
            });
            
            ws.on('error', (error) => {
                if (clientInfo.authTimer) clearTimeout(clientInfo.authTimer);
                if (clientInfo.replaced || this.clients.get(clientInfo.id) !== clientInfo) {
                    return;
                }
                const finalId = clientInfo.id;
                console.error(`❌ WebSocket error for client ${finalId}:`, error);
                const protectedByLease = this.handleRemoteSessionDeparture(clientInfo);
                this.revokeUploadChannelsForClient(clientInfo, protectedByLease);
                if (!protectedByLease) {
                    this.abortUploadsForClient(finalId);
                }
                if (clientInfo.endpointId) {
                    this.unregisterConnectionForEndpoint(clientInfo.endpointId, finalId);
                }
                this.clients.delete(finalId);
                this.forgetCurrentTransport(clientInfo);
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

    issueUploadChannelToken(clientId, requestId) {
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
            ...(this.isValidOpaqueId(requestId) ? { requestId } : {}),
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

        // Authenticated socket binding is authoritative in v4. Do not add the
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

    sendSceneError(clientId, code, message, run = null, correlation = null) {
        const payload = {
            type: 'error',
            protocolVersion: this.protocolVersion,
            serverBootId: this.serverBootId,
            messageId: uuidv4(),
            scope: 'scene',
            code,
            message: String(message || code).slice(0, 512),
        };
        const source = run || correlation;
        if (source) {
            if (this.isValidOpaqueId(source.remoteSessionId)) {
                payload.remoteSessionId = source.remoteSessionId;
            }
            if (Number.isSafeInteger(source.generation) && source.generation > 0) {
                payload.generation = source.generation;
            }
            if (this.isValidOpaqueId(source.sceneRunId)) {
                payload.sceneRunId = source.sceneRunId;
            }
            if (SHA256_PATTERN.test(source.digest || '')) {
                payload.digest = source.digest;
            }
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
        return this.sendToEndpoint(this.getEndpointId(clientId), payload);
    }

    scenePrepareCorrelation(message, session = null) {
        const correlation = {};
        const remoteSessionId = session ? session.remoteSessionId
            : message && message.remoteSessionId;
        const generation = session ? session.generation : message && message.generation;
        if (this.isValidOpaqueId(remoteSessionId)) {
            correlation.remoteSessionId = remoteSessionId;
        }
        if (Number.isSafeInteger(generation) && generation > 0) {
            correlation.generation = generation;
        }
        if (this.isValidOpaqueId(message && message.sceneRunId)) {
            correlation.sceneRunId = message.sceneRunId;
        }
        if (SHA256_PATTERN.test(message && message.digest || '')) {
            correlation.digest = message.digest;
        }
        return correlation;
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
        if (!options.allowGrace && session.degradedEndpoints.size > 0) {
            return { ok: false, error: 'remote_session_reconnecting' };
        }
        if (!options.allowGrace && !options.allowUnready && !this.remoteSessions.commandReady(session)) {
            return { ok: false, error: 'remote_session_sync_pending' };
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
                || !isAllowedMediaExtension(entry.extension)) return null;
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
        let correlation = this.scenePrepareCorrelation(message);
        const reject = (code, detail) => this.sendSceneError(
            clientId, code, detail, null, correlation);
        const validated = this.validateSessionMessage(clientId, message, { ownerOnly: true });
        if (!validated.ok) return reject(validated.error, validated.error);
        const { client, session } = validated;
        correlation = this.scenePrepareCorrelation(message, session);
        if (Array.from(this.pendingAssetRemovals.values()).some(removal =>
            removal.remoteSessionId === session.remoteSessionId)) {
            this.countScenePreparationFailure(message);
            return reject('asset_removal_pending',
                'A remote asset removal must settle before scene preparation');
        }
        if (session.activeUploadIds.size > 0) {
            this.countScenePreparationFailure(message);
            return reject('uploads_still_active',
                'Every upload must be validated before scene preparation');
        }
        const manifest = this.normalizeSceneManifest(message.manifest);
        const scene = message.scene;
        if (!this.isValidOpaqueId(message.sceneRunId)
            || !Number.isSafeInteger(message.revision) || message.revision < 1
            || !manifest
            || !isCanonicalScene(scene, this.MAX_REMOTE_SCENE_SCREENS,
                this.MAX_REMOTE_SCENE_MEDIA)
            || !this.validateSceneMediaBindings(scene, manifest)
            || !this.serializedJsonWithinLimit(scene, this.MAX_REMOTE_SCENE_BYTES)) {
            this.countScenePreparationFailure(message);
            return reject('invalid_scene_manifest', 'Invalid scene revision, manifest, or payload');
        }
        if (!this.requiredSceneChecklist({ manifest, scene })) {
            this.countScenePreparationFailure(message);
            return reject('invalid_scene_manifest',
                'Scene requires more preparation checks than the protocol permits');
        }
        const digest = computeSceneDigest(message.revision, manifest, scene);
        if (message.digest !== digest) {
            this.countScenePreparationFailure(message);
            return reject('scene_digest_mismatch', 'Scene digest does not match the canonical payload');
        }
        const inventory = this.validateSceneInventory(session, manifest);
        if (!inventory.ok) {
            this.countScenePreparationFailure(message);
            return reject('scene_asset_not_validated',
                'At least one media file in this scene has not been uploaded to the remote client. Upload all media before launching the scene.');
        }
        if (!this.sceneMemoryReady(session, manifest)) {
            this.countScenePreparationFailure(message);
            return reject('scene_memory_unavailable',
                'Every scene asset must be validated and resident in target memory before preparation');
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
            return reject(prepared.error, prepared.error);
        }
        const run = prepared.run;
        session.sceneRunId = run.sceneRunId;
        if (prepared.replay) {
            const delivered = this.sendToEndpoint(client.endpointId,
                this.scenePayload(run, 'prepare_progress', {
                    aggregate: true,
                    replay: true,
                    percent: run.phase === SCENE_PHASES.PREPARING ? 0 : 100,
                    stage: 'accepted',
                }));
            if (!delivered) {
                this.initiateSceneStop(run, 'scene_prepare_ack_delivery_failed', true);
            }
            return;
        }
        const delivered = this.sendToEndpoint(session.targetEndpointId,
            this.scenePayload(run, 'scene_prepare', { manifest, scene }));
        if (!delivered) {
            this.initiateSceneStop(run, 'scene_target_unavailable', true);
            return this.sendSceneError(clientId, 'scene_target_unavailable',
                'Scene target is unavailable', run);
        }
        const acceptedDelivered = this.sendToEndpoint(client.endpointId,
            this.scenePayload(run, 'prepare_progress', {
                aggregate: true,
                percent: 0,
                stage: 'accepted',
            }));
        if (!acceptedDelivered) {
            this.initiateSceneStop(run, 'scene_prepare_ack_delivery_failed', true);
        }
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
            if (media.type !== 'text'
                && !appendStage('memory', 'media_memory_ready')) return null;
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
            activationLeadMs: run.activationLeadMs,
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
        let validated = this.validateSessionMessage(clientId, message, { allowGrace: true });
        const run = this.sceneRuns.get(message.sceneRunId);
        if (!run) {
            const finished = this.sceneRuns.tombstones.get(message.sceneRunId);
            const client = this.clients.get(clientId);
            const session = finished && (this.remoteSessions.get(finished.remoteSessionId)
                || this.remoteSessions.getTombstone(finished.remoteSessionId));
            if (finished && client && session && client.authenticated
                && this.terminalRuntimeMatches(session, client)
                && message.connectionGeneration === client.connectionGeneration
                && message.generation === session.generation
                && message.remoteSessionId === finished.remoteSessionId
                && message.digest === finished.digest) {
                return this.sendToEndpoint(client.endpointId, this.scenePayload(finished, 'stopped', {
                    success: finished.phase === SCENE_PHASES.STOPPED,
                    failed: finished.phase === SCENE_PHASES.FAILED, replay: true,
                }));
            }
        }
        // A STOP result acknowledges a terminal obligation, not a new command.
        // Session command authority may already have been revoked.
        if (!validated.ok && run && run.phase === SCENE_PHASES.STOPPING) {
            const client = this.clients.get(clientId);
            const session = this.remoteSessions.get(run.remoteSessionId)
                || this.remoteSessions.getTombstone(run.remoteSessionId);
            if (client && session && client.authenticated
                && this.terminalRuntimeMatches(session, client)
                && message.connectionGeneration === client.connectionGeneration
                && message.generation === session.generation
                && message.remoteSessionId === session.remoteSessionId
                && message.digest === run.digest) {
                validated = { ok: true, client, session };
            }
        }
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

    sweepSceneRuns(now = this.epochNow(), nowMonotonic = undefined) {
        if (nowMonotonic === undefined) {
            this.monotonicNow.refresh?.();
            nowMonotonic = this.monotonicNow();
        }
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
                if (!this.failAssetRemovalsWaitingForScene(
                    action.run, 'scene_stop_timeout', now)) {
                    this.dispatchReadyAssetRemovalsForSession(action.run.remoteSessionId);
                }
            }
        }
    }

    handleMessage(clientId, message, uploadTransportSocket = null) {
        this.monotonicNow.refresh?.();
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
            this.sendError(clientId, 'Invalid protocol v7 message type',
                'invalid_message_type');
            return;
        }
        if (REMOVED_MESSAGE_TYPES.has(message.type)
            || (typeof message.type === 'string' && message.type.startsWith('remote_scene_'))) {
            this.sendError(clientId,
                `Obsolete message type is not supported by protocol v7: ${message.type}`,
                'removed_message_type');
            return;
        }
        const removedField = findRemovedWireField(message);
        if (removedField) {
            this.sendError(clientId,
                `Obsolete field is not supported by protocol v7: ${removedField}`,
                'removed_protocol_field');
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
                client, true, 1001, 'Transport heartbeat timeout');
            this.broadcastClientList();
            return;
        }
        
        if (message.type !== 'heartbeat' && message.type !== 'remote_session_state_ack'
            && message.type !== 'upload_chunk' && message.type !== 'upload_progress'
            && message.type !== 'prepare_progress' && message.type !== 'state_snapshot'
            && message.type !== 'remote_session_cursor') {
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
            case 'endpoint_disable':
                if (!this.isValidOpaqueId(message.requestId)) {
                    this.sendError(clientId, 'A request identifier is required', 'invalid_request_id');
                    break;
                }
                this.handleEndpointDisable(clientId, message);
                break;
            case 'request_client_list':
                this.sendClientList(clientId);
                break;
            case 'request_upload_channel':
                if (!this.issueUploadChannelToken(clientId, message.requestId)) {
                    this.sendError(clientId, 'Upload channel requires a registered control connection');
                }
                break;
            // Upload flow: track state and relay
            case 'upload_start':
                this.handleUploadStart(clientId, message, uploadTransportSocket);
                break;
            case 'upload_resume':
                this.handleUploadResume(clientId, message, uploadTransportSocket);
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
            case 'upload_remove':
                this.handleUploadRemove(clientId, message);
                break;
            case 'upload_removed':
                this.handleUploadRemoved(clientId, message);
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
                this.handleHeartbeat(clientId, message, {
                    monotonicMs: receivedAt,
                });
                break;
            case 'remote_session_open':
                this.handleRemoteSessionOpen(clientId, message);
                break;
            case 'remote_session_accept':
                this.handleRemoteSessionAccept(clientId, message);
                break;
            case 'media_residency':
                this.handleMediaResidency(clientId, message);
                break;
            case 'remote_session_snapshot':
                this.handleRemoteSessionSnapshot(clientId, message);
                break;
            case 'remote_session_cursor':
                this.handleRemoteSessionCursor(clientId, message);
                break;
            case 'remote_session_resume':
                if (!this.isValidOpaqueId(message.requestId)) {
                    this.sendRemoteSessionError(clientId, 'A request identifier is required', 'invalid_request_id', message);
                    break;
                }
                this.handleRemoteSessionResume(clientId, message);
                break;
            case 'remote_session_state_ack':
                this.handleRemoteSessionStateAck(clientId, message);
                break;
            case 'remote_session_reconcile':
                this.handleRemoteSessionReconcile(clientId, message);
                break;
            case 'remote_session_close':
                this.handleRemoteSessionClose(clientId, message);
                break;
            case 'remote_session_teardown_ack':
                this.handleRemoteSessionTeardownAck(clientId, message);
                break;
            default:
                this.sendError(clientId, 'Unknown protocol v7 message type', 'unknown_message_type');
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
        // A transport timeout fences that socket, not its session. The session
        // retains the fixed interruption deadline, never a fresh recovery period.
        return this.handleRemoteSessionDeparture(client, now) ? 1 : 0;
    }

    retireClientTransport(client, preserveSessionUploads, code, reason) {
        if (!client) return false;
        const current = this.clients.get(client.id);
        if (current !== client) return false;
        client.replaced = true;
        this.rememberEndpointPresence(client);
        this.revokeUploadChannelsForClient(client, preserveSessionUploads);
        if (!preserveSessionUploads) this.abortUploadsForClient(client.id);
        if (client.endpointId) {
            this.unregisterConnectionForEndpoint(client.endpointId, client.id);
        }
        this.clients.delete(client.id);
        this.forgetCurrentTransport(client);
        if (client.ws && (client.ws.readyState === WebSocket.OPEN
            || client.ws.readyState === WebSocket.CONNECTING)) {
            client.ws.close(code, reason);
        }
        return true;
    }

    forgetCurrentTransport(client) {
        if (client && this.currentTransportByEndpoint.get(client.endpointId) === client) {
            this.currentTransportByEndpoint.delete(client.endpointId);
        }
    }

    sweepExpiredClientTransports(now = this.monotonicNow()) {
        let removed = 0;
        for (const client of Array.from(this.clients.values())) {
            if (!this.clientLeaseExpired(client, now)) continue;
            this.expireRemoteSessionsForClient(client, now);
            if (this.retireClientTransport(
                client, true, 1001, 'Transport heartbeat timeout')) ++removed;
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
        const verified = verifyAuthResponse(challenge, message, epochNow,
            this.config.authChallengeTimeoutMs);
        if (!verified.ok) {
            this.sendError(clientId, verified.error, verified.error);
            if (client.ws && client.ws.readyState === WebSocket.OPEN) {
                client.ws.close(1008, 'Identity verification failed');
            }
            return;
        }

        // Exhaustion must fail before retiring any admitted transport. A new
        // server boot obtains a fresh namespace; generations must never wrap.
        if (!Number.isSafeInteger(this.connectionGenerationSequence)
            || this.connectionGenerationSequence >= Number.MAX_SAFE_INTEGER) {
            this.sendError(clientId, 'Transport generation capacity exhausted',
                'connection_generation_exhausted');
            client.ws.close(1011, 'Transport generation capacity exhausted');
            return;
        }

        const existing = this.currentTransportByEndpoint.get(verified.endpointId);
        if (existing && existing !== client) {
            if (existing.runtimeId !== verified.runtimeId) {
                if (!this.clientLeaseExpired(existing, monotonicNow)) {
                    this.sendError(clientId, 'This endpoint is already online', 'endpoint_already_connected');
                    client.ws.close(1008, 'Endpoint already connected');
                    return;
                }
                this.expireRemoteSessionsForClient(existing, monotonicNow);
                this.retireClientTransport(
                    existing, false, 1001, 'Superseded after heartbeat lease expiry');
            } else {
                const protectedByLease = this.handleRemoteSessionDeparture(
                    existing, monotonicNow);
                this.retireClientTransport(
                    existing, protectedByLease, 1000, 'Connection rebound');
            }
        }

        // A departing socket may already have left the live transport index
        // while its sessions retain a recovery lease. Reusing its ordinal in
        // another process must not inherit that process's session authority.
        for (const session of this.remoteSessions.sessionsForEndpoint(verified.endpointId)) {
            const runtimeId = session.ownerEndpointId === verified.endpointId
                ? session.ownerRuntimeId : session.targetRuntimeId;
            if (runtimeId === verified.runtimeId || TERMINAL_PHASES.has(session.phase)) continue;
            const terminated = this.remoteSessions.terminate(
                session.remoteSessionId, 'runtime_restarted', monotonicNow);
            if (terminated.ok && !terminated.replay) this.beginRemoteSessionTeardown(terminated.session);
        }

        const generation = ++this.connectionGenerationSequence;
        client.authenticated = true;
        client.installationId = verified.installationId;
        client.endpointId = verified.endpointId;
        client.instanceId = verified.instanceId;
        client.instanceOrdinal = verified.instanceOrdinal;
        client.runtimeId = verified.runtimeId;
        client.connectionGeneration = generation;
        this.currentTransportByEndpoint.set(client.endpointId, client);
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
            instanceOrdinal: client.instanceOrdinal,
            runtimeId: client.runtimeId,
            connectionGeneration: generation,
            policy: {
                policyVersion: this.config.policyVersion,
                heartbeatIntervalMs: this.config.heartbeatIntervalMs,
                leaseTimeoutMs: this.config.leaseTimeoutMs,
                transportSuspectAfterMs: this.config.leaseTimeoutMs,
                sessionRecoveryTimeoutMs: this.config.sessionRecoveryTimeoutMs,
                remoteSessionOpenTimeoutMs: this.config.remoteSessionOpenTimeoutMs,
                scenePrepareTimeoutMs: this.config.scenePrepareTimeoutMs,
                // Advertise the largest adaptive lead an actual COMMIT may
                // carry. Clients use this policy field as an upper bound;
                // each COMMIT still carries its exact effective deadline.
                sceneActivationLeadMs: this.sceneRuns.activationLeadCeilingMs(),
                sceneMaxClockSkewMs: this.config.sceneMaxClockSkewMs,
                sceneStartedAckTimeoutMs: this.sceneRuns.startedAckTimeoutMs,
                sceneStopTimeoutMs: this.config.sceneStopTimeoutMs,
                sceneMaxStartSkewMs: this.sceneRuns.maximumStartSkewMs,
                uploadIdleTimeoutMs: this.config.uploadIdleTimeoutMs,
                uploadTargetAckTimeoutMs: this.config.uploadTargetAckTimeoutMs,
                removalAckTimeoutMs: this.config.removalAckTimeoutMs,
            },
            // Authentication, leases, SceneRuns and clock synchronization must
            // all live in the same monotonic domain. In production this is
            // suspend-inclusive monotonic time; tests may inject an equivalent.
            serverMonotonicMs: monotonicNow,
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
                client, true, 1001, 'Transport heartbeat timeout');
            this.broadcastClientList();
            return;
        }
        client.lastHeartbeatAt = epochNow;
        client.lastHeartbeatMonotonicAt = monotonicNow;
        client.heartbeatSamples = (client.heartbeatSamples || 0) + 1;
        if (monotonicNow - (client.lastHeartbeatSummaryAt || monotonicNow) >= this.config.statsIntervalMs) {
            this.logProtocolEvent('heartbeat_summary', { endpointId: client.endpointId,
                connectionGeneration: client.connectionGeneration, samples: client.heartbeatSamples });
            client.heartbeatSamples = 0;
            client.lastHeartbeatSummaryAt = monotonicNow;
        }
        if (!Number.isFinite(client.lastHeartbeatSummaryAt)) client.lastHeartbeatSummaryAt = monotonicNow;
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
                payload.degradedEndpointId = client.endpointId;
                this.sendToEndpoint(session.ownerEndpointId, payload);
                this.sendToEndpoint(session.targetEndpointId, payload);
            }
        }
        // Take t2 only after all heartbeat/session work, immediately before
        // serialization. This lets the client remove relay queue/processing
        // time from (t3 - t0) instead of misclassifying it as WAN latency.
        const transmitMonotonicNow = typeof timing === 'number' ? timing
            : timing && Number.isFinite(timing.transmitMonotonicMs)
                ? timing.transmitMonotonicMs : this.monotonicNow();
        client.ws.send(JSON.stringify({
            type: 'heartbeat_ack',
            protocolVersion: this.protocolVersion,
            serverBootId: this.serverBootId,
            messageId: uuidv4(),
            connectionGeneration: client.connectionGeneration,
            sequence: message.sequence,
            clientMonotonicMs: message.clientMonotonicMs,
            // Keep serverMonotonicMs as the transmit-time compatibility field.
            // The explicit pair lets clients subtract relay processing time
            // from RTT and compute a proper four-timestamp/NTP estimate.
            serverMonotonicMs: transmitMonotonicNow,
            serverReceiveMonotonicMs: monotonicNow,
            serverTransmitMonotonicMs: transmitMonotonicNow,
            serverEpochMs: epochNow,
            sessionStates: this.remoteSessions.sessionsForEndpoint(client.endpointId)
                .filter(session => !TERMINAL_PHASES.has(session.phase))
                .map(session => this.remoteSessionPayload(session, 'remote_session_lease_state')),
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
        if (owner.draining === true) {
            return this.sendRemoteSessionError(ownerId,
                'This endpoint is disabled', 'endpoint_draining', message);
        }
        if (!owner || !target || !target.authenticated || !target.machineName
            || target.draining === true) {
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
                target, true, 1001, 'Transport heartbeat timeout');
            this.broadcastClientList();
            return this.sendRemoteSessionError(ownerId,
                'Target client is offline', 'target_offline', message,
                message.targetEndpointId);
        }
        if (!this.isValidOpaqueId(message.requestId)) {
            return this.sendRemoteSessionError(ownerId,
                'A request identifier is required', 'invalid_request_id', message,
                target.endpointId);
        }
        const opened = this.remoteSessions.open({
            ownerEndpointId: owner.endpointId,
            targetEndpointId: target.endpointId,
            ownerRuntimeId: owner.runtimeId,
            targetRuntimeId: target.runtimeId,
            ownerConnectionGeneration: owner.connectionGeneration,
            targetConnectionGeneration: target.connectionGeneration,
            requestId: message.requestId,
            awaitTargetAcceptance: true,
        });
        if (!opened.ok) {
            return this.sendRemoteSessionError(
                ownerId, opened.error, opened.error, message, target.endpointId);
        }

        // An OPEN requestId is idempotent for its whole retention window. If
        // that exact request already reached teardown, replay its authoritative
        // terminal result to the owner; never turn the old transaction into a
        // fresh offer to the target.
        if (opened.replay && TERMINAL_PHASES.has(opened.session.phase)) {
            this.bindTerminalDelivery(opened.session, owner);
            if (opened.session.phase === 'Closed') {
                this.sendRemoteSessionStateToEndpoint(
                    opened.session, 'remote_session_terminating', owner.endpointId, {
                        phase: 'CleanupPending',
                        replay: true,
                        requestId: message.requestId,
                    });
                return this.sendRemoteSessionStateToEndpoint(
                    opened.session, 'remote_session_closed', owner.endpointId, {
                        cleanupState: 'confirmed',
                        replay: true,
                        requestId: message.requestId,
                    });
            }
            return this.sendRemoteSessionStateToEndpoint(
                opened.session, 'remote_session_terminating', owner.endpointId, {
                    replay: true,
                    requestId: message.requestId,
                });
        }

        // OPEN is not a substitute for the proof-bearing Resume transition.
        // Never relay a replay whose owner transport tuple belongs to the
        // displaced socket: Qt (correctly) rejects that envelope, which would
        // otherwise leave a cancelled request fenced forever.
        if (opened.replay && opened.session.phase === 'Grace') {
            return this.sendRemoteSessionError(
                ownerId, 'The existing session requires resume proof',
                'session_requires_resume', {
                    ...message,
                    remoteSessionId: opened.session.remoteSessionId,
                }, target.endpointId);
        }

        if (opened.session.phase === 'Active' && opened.replay) {
            if (opened.session.ownerConnectionGeneration
                !== owner.connectionGeneration) {
                return this.sendRemoteSessionError(
                    ownerId, 'The existing session requires resume proof',
                    'session_requires_resume', {
                        ...message,
                        remoteSessionId: opened.session.remoteSessionId,
                    }, target.endpointId);
            }
            const payload = this.remoteSessionPayload(
                opened.session, opened.session.generation === 1
                    ? 'remote_session_opened' : 'remote_session_resumed');
            payload.requestId = message.requestId;
            payload.resumeToken = opened.session.resumeToken;
            payload.snapshotSequence = opened.session.snapshotSequence || 1;
            payload.snapshot = opened.session.latestTargetSnapshot?.snapshot
                || opened.session.initialSnapshot;
            return this.sendToEndpoint(owner.endpointId, payload);
        }

        if (opened.replay && opened.session.phase === 'Opening'
            && opened.session.ownerConnectionGeneration
                !== owner.connectionGeneration) {
            // Opening has no resume proof and cannot migrate transports. Make
            // its teardown monotonic before answering the replacement socket;
            // a later OPEN remains blocked until target cleanup commits.
            const abandoned = this.remoteSessions.terminate(
                opened.session.remoteSessionId,
                'open_owner_transport_replaced');
            if (abandoned.ok && !abandoned.replay) {
                this.beginRemoteSessionTeardown(
                    abandoned.session, message.requestId);
            }
            return this.sendRemoteSessionError(
                ownerId, 'The previous opening is being cleaned up',
                'session_cleanup_pending', {
                    ...message,
                    remoteSessionId: opened.session.remoteSessionId,
                }, target.endpointId);
        }

        const offer = this.remoteSessionPayload(
            opened.session, 'remote_session_offer');
        offer.requestId = message.requestId;
        if (!this.sendToEndpoint(target.endpointId, offer)) {
            const failed = this.remoteSessions.terminate(
                opened.session.remoteSessionId, 'open_target_unavailable');
            if (failed.ok && !failed.replay) {
                this.beginRemoteSessionTeardown(failed.session, message.requestId);
            }
            return this.sendRemoteSessionError(ownerId,
                'Target client is offline', 'target_offline', message,
                target.endpointId);
        }
        this.sendToEndpoint(owner.endpointId, {
            ...this.remoteSessionPayload(
                opened.session, 'remote_session_opening'),
            requestId: message.requestId,
        });
    }

    normalizeRemoteSessionSnapshot(snapshot) {
        const required = ['screens', 'systemUI', 'volumePercent',
            'revision', 'capturedAtEpochMs'];
        if (!isPlainObject(snapshot) || !hasOnlyKeys(snapshot, required)
            || (snapshot.volumePercent !== null
                && !isBoundedInteger(snapshot.volumePercent, 0, 100))
            || !Number.isSafeInteger(snapshot.revision)
            || snapshot.revision < 1
            || !Number.isSafeInteger(snapshot.capturedAtEpochMs)
            || snapshot.capturedAtEpochMs < 1) {
            return null;
        }
        const screens = normalizeScreens(
            snapshot.screens, this.MAX_REMOTE_SCENE_SCREENS);
        if (!screens || !Array.isArray(snapshot.systemUI)
            || snapshot.systemUI.length > 64) return null;
        const systemUI = snapshot.systemUI.map(normalizeUiZone);
        if (systemUI.some(zone => zone === null)) return null;
        return {
            screens,
            systemUI,
            volumePercent: snapshot.volumePercent,
            revision: snapshot.revision,
            capturedAtEpochMs: snapshot.capturedAtEpochMs,
        };
    }

    handleRemoteSessionAccept(targetId, message) {
        const target = this.clients.get(targetId);
        const session = target && this.remoteSessions.get(message.remoteSessionId);
        const snapshot = this.normalizeRemoteSessionSnapshot(message.snapshot);
        if (!target || !session
            || session.targetEndpointId !== target.endpointId
            || session.targetRuntimeId !== target.runtimeId) {
            return this.sendRemoteSessionError(targetId,
                'Unknown remote session offer', 'unknown_remote_session', message);
        }
        if (!snapshot) {
            const failed = this.remoteSessions.terminate(
                session.remoteSessionId, 'invalid_initial_snapshot');
            this.sendRemoteSessionError(this.resolveClientId(session.ownerEndpointId),
                'The remote client returned an invalid initial snapshot',
                'invalid_initial_snapshot', {
                    requestId: session.openRequestId,
                    remoteSessionId: session.remoteSessionId,
                }, session.targetEndpointId);
            if (failed.ok && !failed.replay) this.beginRemoteSessionTeardown(failed.session);
            return this.sendRemoteSessionError(targetId,
                'Invalid initial snapshot', 'invalid_initial_snapshot', message);
        }
        const accepted = this.remoteSessions.accept({
            remoteSessionId: session.remoteSessionId,
            targetEndpointId: target.endpointId,
            targetRuntimeId: target.runtimeId,
            generation: message.generation,
            connectionGeneration: target.connectionGeneration,
        });
        if (!accepted.ok) {
            if (accepted.terminalTransition && accepted.session) {
                this.beginRemoteSessionTeardown(accepted.session);
            }
            return this.sendRemoteSessionError(targetId,
                accepted.error, accepted.error, message);
        }
        if (accepted.replay) {
            // Replayed ACCEPT cannot reset the sequence or replace the initial
            // snapshot with an old offer's data.
            const payload = this.remoteSessionPayload(accepted.session, 'remote_session_opened');
            payload.requestId = accepted.session.openRequestId;
            payload.resumeToken = accepted.session.resumeToken;
            payload.snapshotSequence = accepted.session.snapshotSequence || 1;
            payload.snapshot = accepted.session.latestTargetSnapshot?.snapshot
                || accepted.session.initialSnapshot;
            this.sendToEndpoint(accepted.session.ownerEndpointId, payload);
            this.sendToEndpoint(accepted.session.targetEndpointId, payload);
            return;
        }
        accepted.session.initialSnapshot = snapshot;
        accepted.session.latestTargetSnapshot = { generation: accepted.session.generation, snapshot };
        accepted.session.snapshotSequence = 1;
        accepted.session.snapshotRevision = snapshot.revision;
        const payload = this.remoteSessionPayload(
            accepted.session, 'remote_session_opened');
        payload.requestId = accepted.session.openRequestId;
        payload.resumeToken = accepted.session.resumeToken;
        payload.snapshotSequence = 1;
        payload.snapshot = snapshot;
        this.sendToEndpoint(accepted.session.ownerEndpointId, payload);
        this.sendToEndpoint(accepted.session.targetEndpointId, payload);
    }

    handleMediaResidency(targetId, message) {
        const validated = this.validateSessionMessage(targetId, message, { allowUnready: true });
        if (!validated.ok || validated.role !== 'target') {
            return this.sendRemoteSessionError(targetId,
                'Only the current target may publish media memory state',
                validated.ok ? 'not_session_target' : validated.error, message);
        }
        const { session } = validated;
        const previous = session.mediaResidency;
        const inventory = this.sessionAssets.get(session.remoteSessionId) || new Map();
        const states = new Set(['analysing', 'queued', 'decoding', 'ready',
            'waiting_for_memory', 'capacity_insufficient', 'error']);
        if (!Number.isSafeInteger(message.sequence) || message.sequence < 1
            || (previous && previous.generation === session.generation
                && message.sequence <= previous.sequence)
            || !Array.isArray(message.assets) || message.assets.length > this.MAX_UPLOAD_FILES
            || !this.serializedJsonWithinLimit(message.assets, this.MAX_REMOTE_SCENE_BYTES)) {
            return this.sendRemoteSessionError(targetId, 'Invalid or stale memory snapshot',
                'invalid_media_residency', message);
        }
        const seen = new Set();
        for (const asset of message.assets) {
            const stored = isPlainObject(asset) && inventory.get(asset.assetId);
            if (!stored || seen.has(asset.assetId) || asset.sha256 !== stored.sha256
                || !states.has(asset.state) || !Number.isFinite(asset.progress)
                || asset.progress < 0 || asset.progress > 1
                || typeof asset.error !== 'string' || asset.error.length > 1024) {
                return this.sendRemoteSessionError(targetId, 'Invalid memory asset identity or state',
                    'invalid_media_residency', message);
            }
            seen.add(asset.assetId);
        }
        session.mediaResidency = { generation: session.generation,
            sequence: message.sequence, assets: message.assets };
        this.sendToEndpoint(session.ownerEndpointId, {
            ...this.remoteSessionPayload(session, 'media_residency'),
            sequence: message.sequence, assets: message.assets,
        });
        const run = session.sceneRunId && this.sceneRuns.get(session.sceneRunId);
        if (run && ![SCENE_PHASES.STOPPED, SCENE_PHASES.FAILED, SCENE_PHASES.STOPPING].includes(run.phase)
            && !this.sceneMemoryReady(session, run.manifest)) {
            this.initiateSceneStop(run, 'scene_memory_unavailable', true);
        }
    }

    sceneMemoryReady(session, manifest) {
        if (manifest.length === 0) return true;
        const snapshot = session.mediaResidency;
        if (!snapshot || snapshot.generation !== session.generation) return false;
        return manifest.every(asset => snapshot.assets.some(state =>
            state.assetId === asset.assetId && state.sha256 === asset.sha256
            && state.state === 'ready'));
    }

    handleRemoteSessionSnapshot(targetId, message) {
        const validated = this.validateSessionMessage(targetId, message, { allowUnready: true });
        if (!validated.ok || validated.role !== 'target') {
            return this.sendRemoteSessionError(targetId,
                validated.ok ? 'Only the target may publish a snapshot' : validated.error,
                validated.ok ? 'not_session_target' : validated.error, message);
        }
        const { session } = validated;
        const snapshot = this.normalizeRemoteSessionSnapshot(message.snapshot);
        if (!snapshot || !Number.isSafeInteger(message.snapshotSequence)
            || message.snapshotSequence <= (session.snapshotSequence || 0)
            || snapshot.revision <= (session.snapshotRevision || 0)) {
            return this.sendRemoteSessionError(targetId,
                'Invalid or stale remote session snapshot',
                'invalid_remote_session_snapshot', message);
        }
        session.snapshotSequence = message.snapshotSequence;
        session.snapshotRevision = snapshot.revision;
        session.updatedAt = this.remoteSessions.now();
        session.latestTargetSnapshot = { generation: session.generation, snapshot };
        // One bounded slot per session; a slow owner receives the latest full
        // replacement, never a backlog of historical display configurations.
        session.pendingTargetSnapshot = {
            generation: session.generation,
            snapshotSequence: message.snapshotSequence,
            snapshot,
        };
        this.flushRemoteSessionSnapshot(session);
    }

    flushRemoteSessionSnapshot(session) {
        const pending = session.pendingTargetSnapshot;
        if (!pending) return true;
        if (pending.generation !== session.generation || session.phase !== 'Active') {
            delete session.pendingTargetSnapshot;
            return false;
        }
        const ownerId = this.resolveClientId(session.ownerEndpointId);
        const owner = ownerId && this.clients.get(ownerId);
        if (!owner || !owner.ws || owner.connectionGeneration !== session.ownerConnectionGeneration
            || owner.runtimeId !== session.ownerRuntimeId
            || Number(owner.ws.bufferedAmount) > this.MAX_OWNER_BUFFERED_CURSOR_BYTES) return false;
        if (!this.sendToEndpoint(session.ownerEndpointId, {
            ...this.remoteSessionPayload(session, 'remote_session_snapshot'),
            snapshotSequence: pending.snapshotSequence,
            snapshot: pending.snapshot,
        })) return false;
        delete session.pendingTargetSnapshot;
        return true;
    }

    handleRemoteSessionCursor(targetId, message) {
        const validated = this.validateSessionMessage(targetId, message, { allowUnready: true });
        if (!validated.ok || validated.role !== 'target') {
            return this.sendRemoteSessionError(targetId,
                validated.ok ? 'Only the target may publish its cursor' : validated.error,
                validated.ok ? 'not_session_target' : validated.error, message);
        }
        const { client, session } = validated;
        const fields = ['type', 'protocolVersion', 'serverBootId', 'messageId',
            'connectionGeneration', 'remoteSessionId', 'generation',
            'sequence', 'visible', 'screenId', 'x', 'y'];
        // Keep the cursor behind its topology, including when the relay had
        // to coalesce snapshots. Connection/session generations still gate it.
        if (!this.flushRemoteSessionSnapshot(session)) return false;
        const latest = session.latestTargetSnapshot;
        const screens = latest?.generation === session.generation
            ? latest.snapshot.screens : client.screens;
        const screen = screens.find(candidate => candidate.id === message.screenId);
        const hidden = message.visible === false && message.screenId === -1
            && message.x === 0 && message.y === 0;
        const onScreen = screen && isBoundedInteger(message.x, 0, screen.width - 1)
            && isBoundedInteger(message.y, 0, screen.height - 1);
        if (!isPlainObject(message) || !hasOnlyKeys(message, fields)
            || !Number.isSafeInteger(message.sequence) || message.sequence < 1
            || typeof message.visible !== 'boolean'
            || !isBoundedInteger(message.screenId, -1, 1_000_000)
            || (message.visible ? !onScreen : !hidden)) {
            return this.sendRemoteSessionError(targetId, 'Invalid remote cursor sample',
                'invalid_remote_session_cursor', message);
        }
        const previous = session.cursorSample;
        if (previous && previous.generation === session.generation
            && message.sequence <= previous.sequence) return false;
        // Cursor positions are ephemeral. Keep only ordering state and drop
        // superseded/congested samples instead of building up a delayed trail.
        // The target periodically repeats its current position with a fresh
        // sequence, including when it is stationary or hidden.
        session.cursorSample = { generation: session.generation, sequence: message.sequence };
        const ownerId = this.resolveClientId(session.ownerEndpointId);
        const owner = ownerId && this.clients.get(ownerId);
        if (!owner || !owner.ws || owner.connectionGeneration !== session.ownerConnectionGeneration
            || owner.runtimeId !== session.ownerRuntimeId
            || Number(owner.ws.bufferedAmount) > this.MAX_OWNER_BUFFERED_CURSOR_BYTES) {
            return false;
        }
        return this.sendToEndpoint(session.ownerEndpointId, {
            ...this.remoteSessionPayload(session, 'remote_session_cursor'),
            sequence: message.sequence,
            visible: message.visible,
            screenId: message.screenId,
            x: message.x,
            y: message.y,
        });
    }

    handleRemoteSessionResume(clientId, message) {
        const client = this.clients.get(clientId);
        if (!client) return;
        if (client.draining) return this.sendRemoteSessionError(clientId,
            'This endpoint is disabled', 'endpoint_draining', message);
        const resumed = this.remoteSessions.resume({
            remoteSessionId: message.remoteSessionId,
            endpointId: client.endpointId,
            runtimeId: client.runtimeId,
            resumeToken: message.resumeToken,
            generation: message.generation,
            connectionGeneration: client.connectionGeneration,
            requestId: message.requestId,
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
                    && message.generation > terminal.generation) {
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
        if (!resumed.replay) this.metrics.increment('remote_session_resumed_total', 1,
            resumed.session.remoteSessionId);
        this.rebindSessionGeneration(resumed.session);
        const payload = this.remoteSessionPayload(resumed.session, 'remote_session_resumed');
        payload.requestId = message.requestId;
        payload.replay = resumed.replay === true;
        payload.resumeToken = resumed.session.resumeToken;
        payload.snapshotSequence = resumed.session.snapshotSequence || 1;
        payload.snapshot = resumed.session.latestTargetSnapshot?.snapshot || resumed.session.initialSnapshot;
        const liveRun = this.sceneRuns.getForSession(resumed.session.remoteSessionId);
        payload.requestStateSnapshot = !!liveRun && liveRun.phase === SCENE_PHASES.LIVE;
        for (const endpointId of [resumed.session.ownerEndpointId,
                                resumed.session.targetEndpointId]) {
            this.sendToEndpoint(endpointId, payload);
        }
        this.dispatchReadyAssetRemovalsForSession(resumed.session.remoteSessionId);
        this.broadcastClientList();
    }

    handleRemoteSessionClose(clientId, message) {
        const client = this.clients.get(clientId);
        if (!client || !client.authenticated) {
            return this.sendRemoteSessionError(clientId,
                'Only an authenticated session party may close it',
                'not_a_session_party', message);
        }
        if (message.connectionGeneration !== client.connectionGeneration
            || this.currentTransportByEndpoint.get(client.endpointId) !== client) {
            return this.sendRemoteSessionError(clientId,
                'Stale connection generation', 'stale_connection_generation', message);
        }

        const session = this.remoteSessions.get(message.remoteSessionId);
        const tombstone = !session
            ? this.remoteSessions.getTombstone(message.remoteSessionId) : null;
        if (!session && !tombstone) {
            return this.sendRemoteSessionError(clientId,
                'Unknown remote session', 'unknown_remote_session', message);
        }
        const correlatedSession = session || tombstone;
        const role = correlatedSession.ownerEndpointId === client.endpointId
            ? 'owner'
            : (correlatedSession.targetEndpointId === client.endpointId ? 'target' : null);
        if (!role) {
            return this.sendRemoteSessionError(clientId,
                'Only an authenticated session party may close it',
                'not_a_session_party', message);
        }
        if (message.generation !== correlatedSession.generation) {
            return this.sendRemoteSessionError(clientId,
                'Stale remote session generation',
                'stale_remote_session_generation', message);
        }

        if (tombstone) {
            if (!this.terminalRuntimeMatches(tombstone, client)) {
                return this.sendRemoteSessionError(clientId,
                    'Terminal session belongs to another process',
                    'invalid_resume_proof', message);
            }
            if (!this.bindTerminalDelivery(tombstone, client)) {
                return this.sendRemoteSessionError(clientId,
                    'Stale terminal delivery generation',
                    'stale_connection_generation', message);
            }
            this.sendRemoteSessionStateToEndpoint(
                tombstone, 'remote_session_terminating', client.endpointId, {
                    phase: 'CleanupPending',
                    replay: true,
                    requestId: this.isValidOpaqueId(message.requestId)
                        ? message.requestId : undefined,
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

        const terminalPhase = session.phase === 'Terminating'
            || session.phase === 'CleanupPending';
        if (terminalPhase) {
            if (!this.terminalRuntimeMatches(session, client)) {
                return this.sendRemoteSessionError(clientId,
                    'Terminal session belongs to another process',
                    'invalid_resume_proof', message);
            }
            if (!this.bindTerminalDelivery(session, client)) {
                return this.sendRemoteSessionError(clientId,
                    'Stale terminal delivery generation',
                    'stale_connection_generation', message);
            }
        } else {
            const expectedRuntimeId = role === 'owner'
                ? session.ownerRuntimeId : session.targetRuntimeId;
            if (client.runtimeId !== expectedRuntimeId) {
                return this.sendRemoteSessionError(clientId,
                    'Remote session belongs to another process',
                    'invalid_resume_proof', message);
            }
            const roleConnectionGeneration = role === 'owner'
                ? session.ownerConnectionGeneration : session.targetConnectionGeneration;
            // CLOSE is the sole non-resume command allowed to advance from a
            // replacement transport. The authenticated endpoint and runtime
            // must match exactly and the server-issued connection generation
            // must move forward; the command-capable binding stays immutable.
            if (client.connectionGeneration < roleConnectionGeneration) {
                return this.sendRemoteSessionError(clientId,
                    'Stale session transport generation',
                    'stale_connection_generation', message);
            }
            if (!this.bindTerminalDelivery(session, client)) {
                return this.sendRemoteSessionError(clientId,
                    'Stale terminal delivery generation',
                    'stale_connection_generation', message);
            }
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
            : (role === 'owner' ? 'explicit_disconnect' : 'peer_close');
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
                this.dispatchRemoteSessionTeardownState(
                    terminating.session, endpointId, {
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
                this.sendRemoteSessionClosedToParties(acknowledged.session);
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
            if (Number.isSafeInteger(session.ownerTerminalConnectionGeneration)
                && client.connectionGeneration
                    < session.ownerTerminalConnectionGeneration) return null;
            session.ownerTerminalConnectionGeneration = client.connectionGeneration;
            session.ownerTerminalRuntimeId = client.runtimeId;
            return 'owner';
        }
        if (session.targetEndpointId === client.endpointId) {
            const expectedRuntime = session.targetTerminalRuntimeId
                || session.targetRuntimeId;
            if (expectedRuntime !== client.runtimeId
                && options.allowTargetRuntimeRestart !== true) return null;
            if (Number.isSafeInteger(session.targetTerminalConnectionGeneration)
                && client.connectionGeneration
                    < session.targetTerminalConnectionGeneration) return null;
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
        const phase = type === 'remote_session_closed' ? 'Closed'
            : type === 'remote_session_terminating' && session.phase === 'Closed'
                ? 'CleanupPending' : session.phase;
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
            stateRevision: session.stateRevision,
            serverMonotonicMs: this.monotonicNow(),
            validUntilServerMonotonicMs: TERMINAL_PHASES.has(session.phase)
                ? undefined : this.remoteSessions.validUntil(session),
            ownerConnectionGeneration,
            targetConnectionGeneration,
            phase,
            state: phase === 'Grace' ? 'Grace'
                : session.graceDeadlineAt !== null || session.degradedEndpoints.size > 0 ? 'Degraded' : phase,
            degraded: session.graceDeadlineAt !== null || session.degradedEndpoints.size > 0,
            commandReady: this.remoteSessions.commandReady(session),
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
        if (!this.bindTerminalDelivery(session, client)) return false;
        return this.sendToEndpoint(endpointId, {
            ...this.remoteSessionPayload(session, type, endpointId),
            ...extra,
        });
    }

    dispatchRemoteSessionTeardownState(session, endpointId, extra = {},
                                       now = this.remoteSessions.now()) {
        const delivered = this.sendRemoteSessionStateToEndpoint(
            session, 'remote_session_terminating', endpointId, extra);
        if (session && endpointId === session.targetEndpointId
            && this.remoteSessions.get(session.remoteSessionId) === session) {
            this.remoteSessions.recordCleanupDispatch(
                session.remoteSessionId, session.teardownId, now);
        }
        return delivered;
    }

    sendRemoteSessionClosedToParties(session, extra = {}) {
        for (const endpointId of [session.ownerEndpointId, session.targetEndpointId]) {
            this.sendRemoteSessionStateToEndpoint(
                session, 'remote_session_closed', endpointId, {
                    phase: 'Closed',
                    cleanupState: session.phase === 'Closed' ? 'confirmed'
                        : (session.cleanupError ? 'error' : 'pending'),
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
            if (this.dispatchRemoteSessionTeardownState(
                    session, client.endpointId,
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
            if (this.remoteSessions.stateApplied(tombstone, client)) continue;
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

    handleRemoteSessionStateAck(clientId, message) {
        const client = this.clients.get(clientId);
        const session = this.remoteSessions.get(message.remoteSessionId)
            || this.remoteSessions.getTombstone(message.remoteSessionId);
        if (!client || !session) return; // Expired terminal receipts are harmless.
        const role = session.ownerEndpointId === client.endpointId ? 'owner'
            : session.targetEndpointId === client.endpointId ? 'target' : null;
        const runtime = role === 'owner' ? session.ownerRuntimeId : session.targetRuntimeId;
        if (!role || (runtime !== client.runtimeId && !this.terminalRuntimeMatches(session, client))) return;
        const wasReady = this.remoteSessions.commandReady(session);
        if (this.remoteSessions.acknowledgeState(message.remoteSessionId,
                client.endpointId, client.connectionGeneration,
                message.generation, message.stateRevision)) {
            this.logProtocolEvent('remote_session_state_applied', {
                endpointId: client.endpointId, remoteSessionId: session.remoteSessionId,
                stateRevision: message.stateRevision, generation: message.generation,
            });
            if (!wasReady && this.remoteSessions.commandReady(session)) {
                const payload = this.remoteSessionPayload(session, 'remote_session_lease_state');
                this.sendToEndpoint(session.ownerEndpointId, payload);
                this.sendToEndpoint(session.targetEndpointId, payload);
            }
        }
    }

    handleRemoteSessionReconcile(clientId, message) {
        const client = this.clients.get(clientId);
        if (!client || !this.isValidOpaqueId(message.requestId)
            || !Array.isArray(message.sessions) || message.sessions.length > 4096
            || message.sessions.some(item => !isPlainObject(item)
                || !this.isValidOpaqueId(item.remoteSessionId)
                || !Number.isSafeInteger(item.generation) || item.generation < 1
                || !Number.isSafeInteger(item.stateRevision) || item.stateRevision < 0)) {
            return this.sendRemoteSessionError(clientId,
                'Invalid reconciliation inventory', 'invalid_reconciliation_inventory', message);
        }
        const requested = new Set(message.sessions.map(item => item.remoteSessionId));
        const candidates = new Map(this.remoteSessions.sessionsForEndpoint(client.endpointId)
            .map(session => [session.remoteSessionId, session]));
        for (const id of requested) {
            const tombstone = this.remoteSessions.getTombstone(id);
            if (tombstone) candidates.set(id, tombstone);
        }
        const sessions = [];
        const encode = (session, type, extra = {}) => ({
            ...this.remoteSessionPayload(session, type, client.endpointId),
            connectionGeneration: client.connectionGeneration,
            ...extra,
        });
        for (const session of candidates.values()) {
            const isOwner = session.ownerEndpointId === client.endpointId;
            const isTarget = session.targetEndpointId === client.endpointId;
            if (!isOwner && !isTarget) continue;
            if (TERMINAL_PHASES.has(session.phase)) {
                if (!this.bindTerminalDelivery(session, client, { allowTargetRuntimeRestart: isTarget })) continue;
                sessions.push(encode(session, 'remote_session_terminating', { phase: 'CleanupPending', replay: true }));
                sessions.push(encode(session, 'remote_session_closed', {
                    phase: 'Closed', replay: true,
                    cleanupState: session.phase === 'Closed' ? 'confirmed'
                        : (session.cleanupError ? 'error' : 'pending'),
                }));
                continue;
            }
            if ((isOwner ? session.ownerRuntimeId : session.targetRuntimeId) !== client.runtimeId) continue;
            sessions.push(encode(session,
                session.phase === 'Opening'
                    ? (isOwner ? 'remote_session_opening' : 'remote_session_offer')
                    : 'remote_session_resumed', {
                    requestId: session.openRequestId,
                    resumeToken: session.phase === 'Opening' ? undefined : session.resumeToken,
                    snapshotSequence: session.snapshotSequence || 1,
                    snapshot: session.latestTargetSnapshot?.snapshot || session.initialSnapshot,
                }));
        }
        this.metrics.increment('remote_session_reconcile_total');
        const visibleIds = new Set(sessions.map(session => session.remoteSessionId));
        this.sendToEndpoint(client.endpointId, {
            type: 'remote_session_reconciled', requestId: message.requestId,
            sessions, complete: true,
            absentSessionIds: [...requested].filter(id => !visibleIds.has(id)),
        });
    }

    sendRemoteSessionError(clientId, errorMessage, code, message = {}, targetEndpointId) {
        const client = this.clients.get(clientId);
        const session = this.remoteSessions.get(message.remoteSessionId)
            || this.remoteSessions.getTombstone(message.remoteSessionId);
        this.metrics.increment('remote_session_rejected_total');
        this.logProtocolEvent('remote_session_rejected', {
            endpointId: client?.endpointId, remoteSessionId: session?.remoteSessionId,
            requestId: this.isValidOpaqueId(message.requestId) ? message.requestId : undefined,
            code, expectedGeneration: session?.generation,
            observedGeneration: message.generation,
            expectedConnectionGeneration: client?.connectionGeneration,
            observedConnectionGeneration: message.connectionGeneration,
            stateRevision: session?.stateRevision,
        });
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
        this.remoteSessions.markCleanupPending(session.remoteSessionId, session.teardownId);
        for (const endpointId of [session.ownerEndpointId, session.targetEndpointId]) {
            this.dispatchRemoteSessionTeardownState(
                session, endpointId, {
                    requestId: this.isValidOpaqueId(requestId) ? requestId : undefined,
                });
        }
        this.sendRemoteSessionClosedToParties(session);
        this.logProtocolEvent('remote_session_closed', {
            remoteSessionId: session.remoteSessionId, generation: session.generation,
            stateRevision: session.stateRevision, reason: session.teardownReason,
            cleanupState: 'pending',
        });
        this.updateCleanupPendingMetric(session.remoteSessionId);
        this.broadcastClientList();
        return true;
    }

    retryPendingRemoteSessionTeardowns(now = this.remoteSessions.now()) {
        // Terminating is intentionally short-lived, but healing an interrupted
        // first dispatch here keeps terminal intent fail-closed and durable for
        // the lifetime of this server process.
        for (const session of [...this.remoteSessions.sessions.values(), ...this.remoteSessions.cleanupJobs.values()]) {
            if ((session.phase === 'Terminating' || session.phase === 'CleanupPending')
                && session.teardownDispatchStarted !== true) {
                this.beginRemoteSessionTeardown(session);
            }
        }

        let attempts = 0;
        for (const session of this.remoteSessions.dueCleanupRetries(now)) {
            ++attempts;
            this.dispatchRemoteSessionTeardownState(
                session, session.targetEndpointId, {
                    phase: 'CleanupPending',
                    replay: true,
                }, now);
        }
        return attempts;
    }

    updateCleanupPendingMetric(correlationId = '') {
        let count = 0;
        let oldestAge = 0;
        for (const session of this.remoteSessions.cleanupJobs.values()) {
            if (session.phase === 'Terminating' || session.phase === 'CleanupPending') ++count;
            oldestAge = Math.max(oldestAge, this.monotonicNow() - session.logicallyClosedAt);
        }
        this.metrics.setGauge('remote_session_cleanup_pending', count, correlationId);
        this.metrics.setGauge('remote_session_cleanup_oldest_age_ms', Math.max(0, Math.floor(oldestAge)), correlationId);
    }

    sweepRemoteSessionLeases(now = undefined) {
        if (now === undefined) {
            this.monotonicNow.refresh?.();
            now = this.remoteSessions.now();
        }
        if (Number.isFinite(this.lastLeaseSweepAt)) {
            const lag = Math.max(0, Math.floor(now - this.lastLeaseSweepAt
                - this.config.sessionLeaseSweepIntervalMs));
            if (lag >= 500) {
                this.metrics.setGauge('server_event_loop_delay_ms', lag);
                this.logProtocolEvent('server_event_loop_delayed', { delayMs: lag });
            }
        }
        this.lastLeaseSweepAt = now;
        for (const transition of this.remoteSessions.markDegraded(now, this.config.leaseTimeoutMs)) {
            const payload = this.remoteSessionPayload(
                transition.session, 'remote_session_lease_state');
            payload.degradedEndpointId = transition.endpointId;
            this.sendToEndpoint(transition.session.ownerEndpointId, payload);
            this.sendToEndpoint(transition.session.targetEndpointId, payload);
        }
        for (const session of this.remoteSessions.tick(now)) {
            if (session.teardownReason === 'lease_expired') {
                this.metrics.incrementOnce('remote_session_lease_expired_total',
                    session.remoteSessionId);
            }
            if (session.teardownReason === 'open_timeout') {
                this.sendRemoteSessionError(
                    this.resolveClientId(session.ownerEndpointId),
                    'The remote client did not accept the session in time',
                    'remote_session_open_timeout', {
                        requestId: session.openRequestId,
                        remoteSessionId: session.remoteSessionId,
                    }, session.targetEndpointId);
            }
            this.beginRemoteSessionTeardown(session);
        }
        for (const session of this.remoteSessions.sessions.values()) {
            this.flushRemoteSessionSnapshot(session);
        }
        this.retryPendingRemoteSessionTeardowns(now);
        this.sweepExpiredClientTransports(now);
        const previousPresenceRevision = this.presenceRevision;
        this.presenceEntries(now);
        if (this.presenceRevision !== previousPresenceRevision) this.broadcastClientList();
        this.sweepSceneRuns(this.epochNow());
    }

    handleRemoteSessionDeparture(client, now = this.remoteSessions.now()) {
        if (!client || !client.endpointId) return false;
        this.rememberEndpointPresence(client);
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
            const run = this.sceneRuns.getForSession(session.remoteSessionId);
            if (run && this.sceneRuns.isPreStart(run)) {
                this.initiateSceneStop(run, 'transport_lost_before_scene_live', true);
            }
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
        this.grantPendingUploadCapacity(session.targetEndpointId);
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
        const client = this.currentTransportByEndpoint.get(targetEndpointId);
        return client && client.authenticated && this.clients.get(client.id) === client
            ? client.id : null;
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
        if (client.draining === true) {
            this.sendError(clientId, 'This endpoint is shutting down',
                'endpoint_draining');
            return;
        }

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
            if (message.instanceOrdinal !== client.instanceOrdinal) {
                return 'instanceOrdinal is inconsistent with the authenticated instance';
            }
            if (!normalizeScreens(message.screens, this.MAX_REMOTE_SCENE_SCREENS))
                return `screens must contain at most ${this.MAX_REMOTE_SCENE_SCREENS} valid, uniquely identified screens`;
            if (message.volumePercent !== null
                && (typeof message.volumePercent !== 'number'
                    || !Number.isFinite(message.volumePercent)
                    || message.volumePercent < 0
                    || message.volumePercent > 100)) {
                return 'volumePercent must be a finite number from 0 to 100 or null';
            }
            if (!Array.isArray(message.systemUI) || message.systemUI.length > 64
                || message.systemUI.map(normalizeUiZone).some(zone => zone === null))
                return 'systemUI must contain at most 64 valid zones';
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
        client.screens = normalizeScreens(message.screens, this.MAX_REMOTE_SCENE_SCREENS);
        client.systemUI = message.systemUI.map(normalizeUiZone);
        client.volumePercent = message.volumePercent;
        this.rememberEndpointPresence(client);

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

        // A terminal result may have been emitted while either party's control
        // transport was absent. Replay it after authoritative registration and
        // before the discovery broadcast. WebSocket ordering makes the
        // subsequent client_list a reconciliation barrier for every terminal
        // state that existed when this snapshot was accepted.
        // The target may use a new runtime solely to finish startup cache
        // cleanup; an owner receives catch-up only in its original process.
        if (client.terminalReconciledGeneration !== client.connectionGeneration) {
            this.replayTerminalStateForClient(client);
            client.terminalReconciledGeneration = client.connectionGeneration;
        }
        // Broadcast updated client list only after terminal catch-up has been
        // enqueued on the registering socket.
        this.broadcastClientList();
    }

    handleEndpointDisable(clientId, message = {}) {
        const client = this.clients.get(clientId);
        if (!client || !client.authenticated || !client.endpointId) return;
        if (!client.draining) {
            client.draining = true;
            this.rememberEndpointPresence(client);
            for (const session of this.remoteSessions.sessionsForEndpoint(
                client.endpointId)) {
                const result = this.remoteSessions.terminate(
                    session.remoteSessionId, 'client_disabled');
                if (result.ok && !result.replay) {
                    this.beginRemoteSessionTeardown(result.session);
                }
            }
            this.broadcastClientList();
        }
        this.sendToEndpoint(client.endpointId, {
            type: 'endpoint_disable_started', requestId: message.requestId,
            desiredMode: 'Disabled', replay: client.disableAcknowledged === true,
        });
        client.disableAcknowledged = true;
    }

    rememberEndpointPresence(client) {
        if (!client || !client.endpointId || !client.machineName) return;
        this.endpointPresence.delete(client.endpointId);
        this.endpointPresence.set(client.endpointId, {
            installationId: client.installationId,
            instanceId: client.instanceId, instanceOrdinal: client.instanceOrdinal,
            runtimeId: client.runtimeId,
            endpointId: client.endpointId, machineName: client.machineName,
            platform: client.platform, lastSeenAt: client.lastHeartbeatAt || null,
            lastContact: client.lastHeartbeatMonotonicAt ?? this.monotonicNow(),
            disabled: client.draining === true,
        });
        while (this.endpointPresence.size > 4096) {
            this.endpointPresence.delete(this.endpointPresence.keys().next().value);
        }
    }

    presenceEntries(now = this.monotonicNow()) {
        const entries = new Map(this.endpointPresence);
        for (const client of this.clients.values()) {
            if (!client.authenticated || !client.machineName || !client.endpointId) continue;
            entries.set(client.endpointId, {
                installationId: client.installationId,
                instanceId: client.instanceId, instanceOrdinal: client.instanceOrdinal,
                runtimeId: client.runtimeId,
                endpointId: client.endpointId, machineName: client.machineName,
                platform: client.platform, lastSeenAt: client.lastHeartbeatAt || null,
                lastContact: client.lastHeartbeatMonotonicAt ?? now,
                disabled: client.draining === true, client,
            });
        }
        const result = [];
        for (const entry of entries.values()) {
            const age = Math.max(0, now - entry.lastContact);
            if (!entry.client && age > this.config.remoteSessionTombstoneTtlMs) {
                this.endpointPresence.delete(entry.endpointId);
                continue;
            }
            const usable = entry.client && entry.client.ws?.readyState === WebSocket.OPEN
                && age < this.config.leaseTimeoutMs && !entry.disabled;
            const recovering = !entry.disabled && !usable
                && this.remoteSessions.sessionsForEndpoint(entry.endpointId)
                    .some(session => !TERMINAL_PHASES.has(session.phase)
                        && now < this.remoteSessions.validUntil(session));
            result.push({
                installationId: entry.installationId,
                instanceId: entry.instanceId, instanceOrdinal: entry.instanceOrdinal,
                runtimeId: entry.runtimeId,
                endpointId: entry.endpointId, machineName: entry.machineName,
                platform: entry.platform, lastSeenAt: entry.lastSeenAt,
                status: usable ? 'Available' : recovering ? 'Degraded' : 'Disconnected',
                canAcceptSession: !!usable,
                reason: entry.disabled ? 'disabled'
                    : usable ? 'enabled' : recovering ? 'transport_lost' : 'offline',
            });
        }
        result.sort((a, b) => a.endpointId.localeCompare(b.endpointId));
        const signature = JSON.stringify(result.map(({ lastSeenAt, ...entry }) => entry));
        if (signature !== this.presenceSignature) {
            this.presenceSignature = signature;
            ++this.presenceRevision;
        }
        return result;
    }

    sendClientList(clientId) {
        const client = this.clients.get(clientId);
        if (!client) return;

        const clientList = this.presenceEntries().filter(entry => entry.endpointId !== client.endpointId);

        client.ws.send(JSON.stringify({
            type: 'client_list',
            protocolVersion: this.protocolVersion,
            serverBootId: this.serverBootId,
            messageId: uuidv4(),
            connectionGeneration: client.connectionGeneration,
            revision: this.presenceRevision,
            observedAtServerMonotonicMs: this.monotonicNow(),
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

    // Protocol v7 upload state. A transfer is immutable and belongs to one
    // RemoteSession generation; authenticated socket identity supplies both
    // parties, so client-provided sender/target aliases are never consulted.
    uploadPayload(upload, type, extra = {}) {
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

    uploadOffsets(upload) {
        return Array.from(upload.assetStates.values()).map(asset => ({
            assetId: asset.assetId,
            offset: asset.durableOffset,
            size: asset.size,
            sha256: asset.sha256,
        })).sort((left, right) => left.assetId.localeCompare(right.assetId));
    }

    uploadCompletionInventory(upload) {
        return Array.from(upload.assetStates.values()).map(asset => ({
            assetId: asset.assetId,
            offset: asset.nextOffset,
            size: asset.size,
            sha256: asset.sha256,
        })).sort((left, right) => left.assetId.localeCompare(right.assetId));
    }

    validateUploadInventory(upload, entries, validateOffset) {
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

    sendUploadRejected(clientId, uploadId, code, detail = '', upload = null) {
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

    normalizeUploadFiles(files) {
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
                || !isAllowedMediaExtension(file.extension)
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

    validateUploadParty(clientId, message, role, allowGrace = false) {
        const validated = this.validateSessionMessage(clientId, message, {
            ownerOnly: role === 'owner',
            allowGrace,
        });
        if (!validated.ok) return validated;
        if (validated.role !== role) return { ok: false, error: `not_upload_${role}` };
        return validated;
    }

    removeUpload(upload) {
        if (!upload) return;
        this.uploads.delete(upload.uploadId);
        const session = this.remoteSessions.get(upload.remoteSessionId);
        if (session) session.activeUploadIds.delete(upload.uploadId);
        this.grantPendingUploadCapacity(upload.targetEndpointId);
    }

    grantPendingUploadCapacity(targetEndpointId) {
        const targetUploads = [...this.uploads.values()]
            .filter(upload => upload.targetEndpointId === targetEndpointId);
        let reserved = targetUploads.filter(upload => upload.relaySlotGranted).length;
        for (const upload of targetUploads) {
            if (upload.relaySlotGranted || !upload.pendingRelayStart) continue;
            const session = this.remoteSessions.get(upload.remoteSessionId);
            if (!session || session.phase !== 'Active') continue;
            if (reserved >= this.MAX_TARGET_STREAMING_UPLOADS) {
                this.sendUploadCapacityWait(upload);
                continue;
            }
            upload.relaySlotGranted = true;
            upload.pendingRelayStart = false;
            ++reserved;
            upload.lastActivity = Date.now();
            const delivered = this.sendToEndpoint(upload.targetEndpointId,
                this.uploadPayload(upload, 'upload_start', {
                    connectionGeneration: upload.ownerConnectionGeneration,
                    files: upload.assets, totalSize: upload.totalSize,
                }));
            if (!delivered) {
                upload.relaySlotGranted = false;
                upload.pendingRelayStart = true;
                --reserved;
            }
        }
    }

    sendUploadCapacityWait(upload) {
        upload.lastActivity = Date.now();
        this.sendToEndpoint(upload.ownerEndpointId,
            this.uploadPayload(upload, 'upload_resume_ready', {
                replay: true, waitingForCapacity: true, assets: this.uploadOffsets(upload),
            }));
    }

    rememberUploadResult(upload, status, extra = {}, now = Date.now()) {
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
            expiresAt: now + this.UPLOAD_RESULT_TOMBSTONE_TTL_MS,
            ...extra,
        });
        this.pruneUploadTombstones(now);
    }

    pruneUploadTombstones(now = Date.now()) {
        for (const [uploadId, result] of this.uploadTombstones) {
            if (!result || result.expiresAt <= now) this.uploadTombstones.delete(uploadId);
        }
        while (this.uploadTombstones.size > this.MAX_REMOTE_SCENE_TOMBSTONES) {
            this.uploadTombstones.delete(this.uploadTombstones.keys().next().value);
        }
    }

    assetRemovalPayload(removal, type = 'upload_removed', extra = {}) {
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

    sendAssetRemovalProtocolError(clientId, message, code, detail = code) {
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

    assetRemovalMatchesMessage(removal, message, includeDerived = false) {
        // Durable ACKs refer to the accepted operation, even if a resume has
        // since advanced the session epoch. The immutable asset identity and
        // authenticated target remain the authority for this one transaction.
        const generationMatches = removal && message && (includeDerived
            ? Number.isSafeInteger(message.generation)
                && message.generation >= (removal.acceptedGeneration || removal.generation)
                && message.generation <= removal.generation
            : message.generation === removal.generation);
        if (!removal || !message
            || message.remoteSessionId !== removal.remoteSessionId
            || !generationMatches
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

    sceneRunUsesAsset(run, assetId) {
        return !!run && Array.isArray(run.manifest)
            && run.manifest.some(entry => entry && entry.assetId === assetId);
    }

    dispatchAssetRemoval(removal, forceReplay = false) {
        if (!removal || !this.pendingAssetRemovals.has(removal.removalId)) return false;
        const session = this.remoteSessions.get(removal.remoteSessionId);
        if (!session || !['Active', 'Grace'].includes(session.phase)) return false;
        const run = this.sceneRuns.getForSession(removal.remoteSessionId);
        if (this.sceneRunUsesAsset(run, removal.assetId)) {
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
            this.assetRemovalPayload(removal, 'upload_remove', {
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
            this.dispatchAssetRemoval(removal);
        }
    }

    failAssetRemovalsWaitingForScene(run, code, now = Date.now()) {
        if (!run) return false;
        let failed = false;
        for (const removal of Array.from(this.pendingAssetRemovals.values())) {
            if (removal.remoteSessionId !== run.remoteSessionId
                || removal.waitingForSceneRunId !== run.sceneRunId) continue;
            failed = true;
            this.settleAssetRemoval(removal, {
                success: false,
                result: 'cleanup_error',
                code,
                reason: 'The scene render graph did not stop before asset cleanup',
            }, now);
            this.terminateSessionAfterAssetRemovalFailure(removal, code, now);
        }
        return failed;
    }

    rememberAssetRemovalResult(removal, result, now = Date.now()) {
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
        this.pruneAssetRemovalTombstones(now);
        return terminal;
    }

    pruneAssetRemovalTombstones(now = Date.now()) {
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

    invalidateUploadReplayAfterAssetRemoval(removal, now = Date.now()) {
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
            expiresAt: Math.max(previous && previous.expiresAt || 0,
                now + this.UPLOAD_RESULT_TOMBSTONE_TTL_MS),
        };
        this.uploadTombstones.delete(removal.uploadId);
        this.uploadTombstones.set(removal.uploadId, invalidated);
        this.pruneUploadTombstones(now);
    }

    settleAssetRemoval(removal, result, now = Date.now()) {
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
                this.invalidateUploadReplayAfterAssetRemoval(removal, now);
            }
        }
        this.pendingAssetRemovals.delete(removal.removalId);
        const terminal = this.rememberAssetRemovalResult(removal, finalResult, now);
        this.sendToEndpoint(removal.ownerEndpointId,
            this.assetRemovalPayload(terminal, 'upload_removed', {
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
            this.settleAssetRemoval(removal, {
                success: false,
                result: 'rejected',
                code: 'remote_session_terminating',
                reason: String(reason || 'remote_session_terminating').slice(0, 128),
            });
        }
    }

    terminateSessionAfterAssetRemovalFailure(removal, reason) {
        const session = removal && this.remoteSessions.get(removal.remoteSessionId);
        if (!session || !['Active', 'Grace'].includes(session.phase)) return false;
        const terminated = this.remoteSessions.terminate(
            session.remoteSessionId, String(reason || 'asset_removal_failed').slice(0, 128),
            this.monotonicNow());
        if (!terminated.ok || terminated.replay) return false;
        this.beginRemoteSessionTeardown(terminated.session);
        return true;
    }

    sweepAssetRemovals(now = Date.now()) {
        this.pruneAssetRemovalTombstones(now);
        for (const removal of Array.from(this.pendingAssetRemovals.values())) {
            if (now < removal.deadlineAt) continue;
            this.settleAssetRemoval(removal, {
                success: false,
                result: 'timeout',
                code: 'asset_removal_ack_timeout',
                reason: 'Remote asset removal was not committed before its fixed deadline',
            }, now);
            this.terminateSessionAfterAssetRemovalFailure(
                removal, 'asset_removal_timeout', now);
        }
    }

    handleUploadRemove(senderId, message) {
        this.sweepAssetRemovals();
        const validated = this.validateUploadParty(senderId, message, 'owner');
        if (!validated.ok) {
            return this.sendAssetRemovalProtocolError(
                senderId, message, validated.error, validated.error);
        }
        const { client: owner, session } = validated;
        if (!CANONICAL_UUID_PATTERN.test(message.removalId || '')
            || !this.isValidOpaqueId(message.uploadId)
            || !this.isValidOpaqueId(message.assetId)
            || !Number.isSafeInteger(message.size) || message.size < 1
            || !Number.isSafeInteger(message.offset) || message.offset !== message.size
            || !SHA256_PATTERN.test(message.sha256 || '')) {
            return this.sendAssetRemovalProtocolError(senderId, message,
                'invalid_asset_removal', 'Invalid remote asset removal fields');
        }
        this.pruneAssetRemovalTombstones();
        const tombstone = this.assetRemovalTombstones.get(message.removalId);
        if (tombstone) {
            if (tombstone.ownerEndpointId !== owner.endpointId
                || !this.assetRemovalMatchesMessage(tombstone, message)) {
                return this.sendAssetRemovalProtocolError(senderId, message,
                    'removal_id_reused', 'A terminal removal identifier cannot be reused');
            }
            return this.sendToEndpoint(owner.endpointId,
                this.assetRemovalPayload(tombstone, 'upload_removed', {
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
                || !this.assetRemovalMatchesMessage(pending, message)) {
                return this.sendAssetRemovalProtocolError(senderId, message,
                    'removal_id_reused', 'A pending removal identifier cannot be reused');
            }
            this.dispatchAssetRemoval(pending, true);
            return;
        }
        if (this.pendingAssetRemovals.size >= this.MAX_PENDING_REMOVALS) {
            return this.sendAssetRemovalProtocolError(senderId, message,
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
            return this.sendAssetRemovalProtocolError(senderId, message,
                'asset_inventory_mismatch',
                'The asset does not exactly match this session inventory');
        }
        for (const other of this.pendingAssetRemovals.values()) {
            if (other.remoteSessionId === session.remoteSessionId
                && other.assetId === message.assetId) {
                return this.sendAssetRemovalProtocolError(senderId, message,
                    'asset_removal_pending', 'This asset already has a pending removal');
            }
        }
        for (const upload of Array.from(this.uploads.values())) {
            if (upload.remoteSessionId === session.remoteSessionId
                && upload.assetStates && upload.assetStates.has(message.assetId)) {
                this.rejectTrackedUpload(upload, 'source_asset_removed',
                    'The source asset was removed while its upload was active');
            }
        }
        const now = Date.now();
        const removal = {
            protocolVersion: this.protocolVersion,
            removalId: message.removalId,
            remoteSessionId: session.remoteSessionId,
            generation: session.generation,
            acceptedGeneration: session.generation,
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
        this.dispatchAssetRemoval(removal);
    }

    handleUploadRemoved(targetId, message) {
        this.sweepAssetRemovals();
        const client = this.clients.get(targetId);
        if (!client || !client.authenticated
            || message.connectionGeneration !== client.connectionGeneration) {
            return this.sendAssetRemovalProtocolError(
                targetId, message, 'stale_connection_generation',
                'The cleanup acknowledgement must use the current authenticated transport');
        }
        const removal = this.pendingAssetRemovals.get(message.removalId);
        if (!removal) {
            const tombstone = this.assetRemovalTombstones.get(message.removalId);
            if (tombstone
                && tombstone.targetEndpointId === client.endpointId
                && this.assetRemovalMatchesMessage(tombstone, message, true)) {
                if (tombstone.success) {
                    this.sendToEndpoint(tombstone.ownerEndpointId,
                        this.assetRemovalPayload(tombstone, 'upload_removed', {
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
            return this.sendAssetRemovalProtocolError(targetId, message,
                'unknown_asset_removal', 'Unknown remote asset removal');
        }
        if (removal.targetEndpointId !== client.endpointId
            || !this.assetRemovalMatchesMessage(removal, message, true)) {
            return this.sendAssetRemovalProtocolError(targetId, message,
                'asset_removal_ack_mismatch',
                'Remote asset removal acknowledgement does not match the request');
        }
        if (removal.phase !== 'awaiting_target_commit') {
            return this.sendAssetRemovalProtocolError(targetId, message,
                'asset_removal_not_dispatched',
                'Remote asset removal is not awaiting a target commit');
        }
        const committed = message.result === 'committed'
            && message.cacheQuarantined === true
            && message.success !== false;
        if (!committed) {
            const terminal = this.settleAssetRemoval(removal, {
                success: false,
                result: 'cleanup_error',
                code: typeof message.errorCode === 'string'
                    ? message.errorCode.slice(0, 128) : 'asset_removal_not_committed',
                reason: typeof message.reason === 'string'
                    ? message.reason.slice(0, 512)
                    : 'The target did not commit the remote asset removal',
            });
            this.terminateSessionAfterAssetRemovalFailure(
                removal, 'asset_removal_cleanup_error');
            return terminal;
        }
        const removedFileCount = Number.isSafeInteger(message.removedFileCount)
            ? Math.max(0, message.removedFileCount) : 1;
        const quarantinedBytes = Number.isSafeInteger(message.quarantinedBytes)
            ? Math.max(0, message.quarantinedBytes) : removal.size;
        const terminal = this.settleAssetRemoval(removal, {
            success: true,
            result: 'committed',
            cacheQuarantined: true,
            removedFileCount,
            quarantinedBytes,
        });
        if (terminal && !terminal.success) {
            this.terminateSessionAfterAssetRemovalFailure(
                removal, 'asset_removal_inventory_changed');
        }
        return terminal;
    }

    rejectTrackedUpload(upload, code, detail = code, notifyTarget = true) {
        if (!upload) return;
        if (notifyTarget) {
            this.sendToEndpoint(upload.targetEndpointId,
                this.uploadPayload(upload, 'upload_abort', { code, reason: detail }));
        }
        const ownerId = this.resolveClientId(upload.ownerEndpointId);
        if (ownerId) this.sendUploadRejected(ownerId, upload.uploadId, code, detail, upload);
        this.rememberUploadResult(upload, 'rejected', { code, detail });
        this.removeUpload(upload);
    }

    handleUploadStart(senderId, message, transportSocket = null) {
        const validated = this.validateUploadParty(senderId, message, 'owner');
        if (!validated.ok) {
            return this.sendUploadRejected(senderId, message.uploadId,
                validated.error, validated.error);
        }
        const { client: owner, session } = validated;
        if (!this.isValidOpaqueId(message.uploadId)) {
            return this.sendUploadRejected(senderId, message.uploadId,
                'invalid_upload_id', 'Invalid upload identifier');
        }
        const normalized = this.normalizeUploadFiles(message.files);
        if (!normalized.ok) {
            return this.sendUploadRejected(senderId, message.uploadId,
                normalized.error, normalized.error);
        }
        const manifestDigest = crypto.createHash('sha256')
            .update(JSON.stringify(normalized.assets), 'utf8').digest('hex');
        this.pruneUploadTombstones();
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
            return this.sendUploadRejected(senderId, message.uploadId,
                'upload_id_reused', 'A terminal upload identifier cannot be reused');
        }
        if (this.sceneRuns.getForSession(session.remoteSessionId)) {
            return this.sendUploadRejected(senderId, message.uploadId,
                'scene_run_active', 'Uploads are locked while a scene run exists');
        }
        if (Array.from(this.pendingAssetRemovals.values()).some(removal =>
            removal.remoteSessionId === session.remoteSessionId)) {
            return this.sendUploadRejected(senderId, message.uploadId,
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
            return this.sendUploadRejected(senderId, message.uploadId,
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
                        this.uploadPayload(duplicate, 'upload_complete', {
                            replay: true,
                            assets: this.uploadCompletionInventory(duplicate),
                        }));
                }
                return this.sendToEndpoint(owner.endpointId,
                    this.uploadPayload(duplicate, 'upload_resume_ready', {
                        replay: true,
                        assets: this.uploadOffsets(duplicate),
                    }));
            }
            return this.sendUploadRejected(senderId, message.uploadId,
                'upload_id_reused',
                'An active upload identifier is already bound to a different immutable transfer');
        }
        const ownerUploads = Array.from(this.uploads.values())
            .filter(upload => upload
                && upload.protocolVersion === this.protocolVersion
                && upload.ownerEndpointId === owner.endpointId);
        if (ownerUploads.some(upload => upload.remoteSessionId === session.remoteSessionId)) {
            return this.sendUploadRejected(senderId, message.uploadId,
                'upload_session_busy', 'Only one upload per remote session is allowed');
        }
        if (ownerUploads.length >= 2) {
            return this.sendUploadRejected(senderId, message.uploadId,
                'upload_concurrency_exceeded', 'At most two outgoing uploads are allowed');
        }
        const assetStates = new Map(normalized.assets.map(asset => [asset.assetId, {
            ...asset,
            nextOffset: 0,
            durableOffset: 0,
        }]));
        const now = Date.now();
        const upload = {
            protocolVersion: this.protocolVersion,
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
            completionRequested: false,
            transportSocket,
            startTime: now,
            lastActivity: now,
            pendingRelayStart: true,
            relaySlotGranted: false,
        };
        this.uploads.set(upload.uploadId, upload);
        session.activeUploadIds.add(upload.uploadId);
        this.grantPendingUploadCapacity(upload.targetEndpointId);
    }

    handleUploadResume(senderId, message, transportSocket = null) {
        const upload = this.uploads.get(message.uploadId);
        // Keep the immutable binding even when lease validation synchronously
        // tears the session down and removes it from the live upload map.
        const validated = this.validateUploadParty(senderId, message, 'owner');
        if (!validated.ok || !upload || upload.protocolVersion !== this.protocolVersion
            || upload.remoteSessionId !== message.remoteSessionId
            || upload.ownerEndpointId !== validated.client?.endpointId) {
            const code = validated.ok ? 'unknown_upload' : validated.error;
            return this.sendUploadRejected(senderId, message.uploadId, code, code, upload);
        }
        upload.generation = validated.session.generation;
        upload.ownerConnectionGeneration = validated.client.connectionGeneration;
        upload.transportSocket = transportSocket;
        upload.transportDisconnectedAt = null;
        upload.awaitingTargetReady = true;
        upload.awaitingTargetValidation = false;
        upload.completionRequested = false;
        upload.relayedBytes = 0;
        upload.durableBytes = 0;
        for (const asset of upload.assetStates.values()) {
            asset.nextOffset = asset.durableOffset;
            upload.relayedBytes += asset.durableOffset;
            upload.durableBytes += asset.durableOffset;
        }
        upload.lastActivity = Date.now();
        if (upload.pendingRelayStart) {
            this.grantPendingUploadCapacity(upload.targetEndpointId);
            this.sendUploadCapacityWait(upload);
            return;
        }
        this.sendToEndpoint(upload.targetEndpointId,
            this.uploadPayload(upload, 'upload_resume', {
                connectionGeneration: validated.client.connectionGeneration,
                assets: this.uploadOffsets(upload),
            }));
        this.sendToEndpoint(upload.ownerEndpointId,
            this.uploadPayload(upload, 'upload_resume_ready', {
                replay: false,
                assets: this.uploadOffsets(upload),
            }));
    }

    handleUploadReady(targetId, message) {
        const validated = this.validateUploadParty(targetId, message, 'target');
        const upload = this.uploads.get(message.uploadId);
        if (!validated.ok || !upload || upload.protocolVersion !== this.protocolVersion
            || upload.remoteSessionId !== message.remoteSessionId
            || upload.awaitingTargetValidation || !upload.awaitingTargetReady
            || upload.pendingRelayStart) return;
        if (upload.generation !== validated.session.generation
            || upload.targetEndpointId !== validated.client.endpointId
            || !this.validateUploadInventory(
                upload, message.assets,
                (asset, offset) => offset === asset.durableOffset)) {
            return this.rejectTrackedUpload(upload,
                'invalid_upload_ready_inventory');
        }
        upload.awaitingTargetReady = false;
        upload.lastActivity = Date.now();
        this.sendToEndpoint(upload.ownerEndpointId,
            this.uploadPayload(upload, 'upload_ready', {
                assets: this.uploadOffsets(upload),
            }));
    }

    decodeUploadChunk(message) {
        if (typeof message.data !== 'string' || message.data.length < 1
            || message.data.length > this.MAX_UPLOAD_CHUNK_BASE64_LENGTH
            || message.data.length % 4 !== 0
            || !/^[A-Za-z0-9+/]*={0,2}$/.test(message.data)) return null;
        const decoded = Buffer.from(message.data, 'base64');
        if (decoded.length < 1 || decoded.length > 128 * 1024
            || decoded.toString('base64') !== message.data) return null;
        return decoded;
    }

    handleUploadChunk(senderId, message, transportSocket = null) {
        const upload = this.uploads.get(message.uploadId);
        const validated = this.validateUploadParty(senderId, message, 'owner');
        if (!validated.ok || !upload || upload.protocolVersion !== this.protocolVersion
            || upload.remoteSessionId !== message.remoteSessionId
            || upload.ownerEndpointId !== validated.client?.endpointId) {
            const code = validated.ok ? 'unknown_upload' : validated.error;
            return this.sendUploadRejected(senderId, message.uploadId, code, code, upload);
        }
        if (upload.transportSocket !== transportSocket) {
            return this.rejectTrackedUpload(upload, 'upload_transport_changed');
        }
        const asset = upload.assetStates.get(message.assetId);
        const decoded = this.decodeUploadChunk(message);
        if (upload.awaitingTargetReady || upload.awaitingTargetValidation || !asset || !decoded
            || message.offset !== asset.nextOffset || message.size !== decoded.length
            || message.sha256 !== asset.sha256
            || decoded.length > asset.size - asset.nextOffset) {
            return this.rejectTrackedUpload(upload, 'invalid_upload_chunk');
        }
        const targetId = this.resolveClientId(upload.targetEndpointId);
        const target = targetId ? this.clients.get(targetId) : null;
        if (!target || !target.ws || target.ws.readyState !== WebSocket.OPEN) return;
        const outstanding = upload.relayedBytes - upload.durableBytes;
        const targetOutstanding = [...this.uploads.values()]
            .filter(other => other.targetEndpointId === upload.targetEndpointId)
            .reduce((total, other) => total + other.relayedBytes - other.durableBytes, 0);
        if (outstanding + decoded.length > this.MAX_UPLOAD_UNACKNOWLEDGED_BYTES
            || targetOutstanding + decoded.length > this.MAX_TARGET_BUFFERED_UPLOAD_BYTES) {
            // Correct v7 senders wait for durable progress at the advertised
            // fixed 1 MiB window. Reject only a sender violating that bound.
            return this.rejectTrackedUpload(upload, 'upload_flow_control_violation');
        }
        const delivered = this.sendToEndpoint(upload.targetEndpointId,
            this.uploadPayload(upload, 'upload_chunk', {
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

    handleUploadProgress(targetId, message) {
        const validated = this.validateUploadParty(targetId, message, 'target');
        const upload = this.uploads.get(message.uploadId);
        if (!validated.ok || !upload || upload.protocolVersion !== this.protocolVersion
            || upload.remoteSessionId !== message.remoteSessionId
            || !Array.isArray(message.assets)) return;
        if (upload.generation !== validated.session.generation
            || upload.targetEndpointId !== validated.client.endpointId
            || !this.validateUploadInventory(
                upload, message.assets,
                (asset, offset) => offset >= asset.durableOffset
                    && offset <= asset.nextOffset)) {
            return this.rejectTrackedUpload(upload,
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
            this.uploadPayload(upload, 'upload_progress', {
                durableBytes: upload.durableBytes,
                totalSize: upload.totalSize,
                assets: this.uploadOffsets(upload),
            }));
        if (upload.completionRequested && this.uploadIsFullyDurable(upload)) {
            this.beginUploadTargetValidation(upload);
        }
    }

    uploadIsFullyDurable(upload) {
        return upload.durableBytes === upload.totalSize
            && Array.from(upload.assetStates.values()).every(asset =>
                asset.durableOffset === asset.size);
    }

    beginUploadTargetValidation(upload) {
        if (!upload || upload.awaitingTargetValidation
            || !this.uploadIsFullyDurable(upload)) return;
        upload.completionRequested = false;
        upload.awaitingTargetValidation = true;
        upload.relaySlotGranted = false;
        this.grantPendingUploadCapacity(upload.targetEndpointId);
        upload.awaitingTargetValidationSince = Date.now();
        upload.lastActivity = upload.awaitingTargetValidationSince;
        this.sendToEndpoint(upload.targetEndpointId,
            this.uploadPayload(upload, 'upload_complete', {
                connectionGeneration: upload.ownerConnectionGeneration,
                assets: this.uploadCompletionInventory(upload),
            }));
    }

    handleUploadComplete(senderId, message, transportSocket = null) {
        const upload = this.uploads.get(message.uploadId);
        const validated = this.validateUploadParty(senderId, message, 'owner');
        if (!validated.ok || !upload || upload.protocolVersion !== this.protocolVersion
            || upload.remoteSessionId !== message.remoteSessionId
            || upload.ownerEndpointId !== validated.client?.endpointId) {
            const code = validated.ok ? 'unknown_upload' : validated.error;
            return this.sendUploadRejected(senderId, message.uploadId, code, code, upload);
        }
        if (upload.transportSocket !== transportSocket || upload.awaitingTargetReady) {
            return this.rejectTrackedUpload(upload, 'invalid_upload_completion_state');
        }
        if (!this.validateUploadInventory(
            upload, message.assets, (asset, offset) => offset === asset.size)) {
            return this.rejectTrackedUpload(
                upload, 'invalid_upload_completion_inventory');
        }
        if (upload.awaitingTargetValidation) return;
        if (upload.relayedBytes !== upload.totalSize
            || Array.from(upload.assetStates.values()).some(asset =>
                asset.nextOffset !== asset.size)) {
            return this.rejectTrackedUpload(upload, 'upload_incomplete');
        }
        if (!this.uploadIsFullyDurable(upload)) {
            // Version-skew tolerant barrier: an older owner may request
            // completion as soon as its send queue drains. Preserve that exact
            // request and release it only after B reports every byte durable.
            upload.completionRequested = true;
            upload.lastActivity = Date.now();
            return;
        }
        this.beginUploadTargetValidation(upload);
    }

    handleUploadFinished(targetId, message) {
        const validated = this.validateUploadParty(targetId, message, 'target');
        const upload = this.uploads.get(message.uploadId);
        if (!validated.ok || !upload || upload.protocolVersion !== this.protocolVersion
            || upload.remoteSessionId !== message.remoteSessionId
            || !upload.awaitingTargetValidation || !Array.isArray(message.assets)) return;
        const acknowledgements = new Map();
        for (const entry of message.assets) {
            if (!isPlainObject(entry) || acknowledgements.has(entry.assetId)) {
                return this.rejectTrackedUpload(upload, 'invalid_upload_ack');
            }
            acknowledgements.set(entry.assetId, entry);
        }
        const invalid = acknowledgements.size !== upload.assetStates.size
            || Array.from(upload.assetStates.values()).some(asset => {
                const entry = acknowledgements.get(asset.assetId);
                return !entry || entry.offset !== asset.size || entry.size !== asset.size
                    || entry.sha256 !== asset.sha256;
            });
        if (invalid) return this.rejectTrackedUpload(upload, 'invalid_upload_ack');
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
        const finished = this.uploadPayload(upload, 'upload_finished', {
            assets: upload.assets.map(asset => ({
                assetId: asset.assetId,
                offset: asset.size,
                size: asset.size,
                sha256: asset.sha256,
            })),
        });
        this.rememberUploadResult(upload, 'finished', { payload: finished });
        this.removeUpload(upload);
        this.sendToEndpoint(upload.ownerEndpointId, finished);
    }

    handleUploadRejected(targetId, message) {
        const validated = this.validateUploadParty(targetId, message, 'target');
        const upload = this.uploads.get(message.uploadId);
        if (!validated.ok || !upload || upload.protocolVersion !== this.protocolVersion
            || upload.remoteSessionId !== message.remoteSessionId) return;
        this.rejectTrackedUpload(upload,
            typeof message.code === 'string' ? message.code.slice(0, 128) : 'upload_target_rejected',
            typeof message.reason === 'string' ? message.reason.slice(0, 512) : '', false);
    }

    handleUploadAbort(senderId, message) {
        const validated = this.validateUploadParty(senderId, message, 'owner', true);
        const upload = this.uploads.get(message.uploadId);
        if (!validated.ok || !upload || upload.protocolVersion !== this.protocolVersion
            || upload.remoteSessionId !== message.remoteSessionId) return;
        this.sendToEndpoint(upload.targetEndpointId,
            this.uploadPayload(upload, 'upload_abort', {
                reason: typeof message.reason === 'string'
                    ? message.reason.slice(0, 128) : 'owner_abort',
            }));
        this.rememberUploadResult(upload, 'aborted');
        this.removeUpload(upload);
        this.sendToEndpoint(upload.ownerEndpointId,
            this.uploadPayload(upload, 'upload_aborted', { success: true }));
    }

    handleUploadAbortAcknowledgement(targetId, message) {
        // No state is recreated by a late acknowledgement. This is deliberately
        // an idempotent no-op after the server has committed an abort.
        void targetId;
        void message;
    }

    abortUploadsForUploadSocket(senderId, socket, reason) {
        void senderId;
        for (const upload of this.uploads.values()) {
            if (!upload || upload.protocolVersion !== this.protocolVersion
                || upload.transportSocket !== socket) continue;
            upload.transportSocket = null;
            upload.transportDisconnectedAt = Date.now();
            upload.lastActivity = Date.now();
            upload.pauseReason = String(reason || 'upload_transport_lost').slice(0, 128);
        }
    }

    abortUploadsForClient(clientId) {
        const client = this.clients.get(clientId);
        if (!client) return;
        for (const upload of Array.from(this.uploads.values())) {
            if (!upload || upload.protocolVersion !== this.protocolVersion
                || (upload.ownerEndpointId !== client.endpointId
                    && upload.targetEndpointId !== client.endpointId)) continue;
            const peer = upload.ownerEndpointId === client.endpointId
                ? upload.targetEndpointId : upload.ownerEndpointId;
            this.sendToEndpoint(peer, this.uploadPayload(upload, 'upload_abort', {
                reason: 'remote_session_unavailable',
            }));
            this.removeUpload(upload);
        }
    }

    cleanupStalledUploads(now = Date.now()) {
        this.pruneUploadTombstones(now);
        this.sweepAssetRemovals(now);
        if (this.remoteSessions.cleanupJobs.size > 0) this.updateCleanupPendingMetric();
        for (const upload of Array.from(this.uploads.values())) {
            if (!upload || upload.protocolVersion !== this.protocolVersion) continue;
            if (upload.pendingRelayStart) {
                this.grantPendingUploadCapacity(upload.targetEndpointId);
                if (upload.pendingRelayStart) this.sendUploadCapacityWait(upload);
                continue;
            }
            const waitingForValidation = upload.awaitingTargetValidation === true;
            const timeout = waitingForValidation
                ? this.UPLOAD_TARGET_ACK_TIMEOUT_MS : this.UPLOAD_TIMEOUT_MS;
            const activity = waitingForValidation
                ? upload.awaitingTargetValidationSince : upload.lastActivity;
            if (now - activity < timeout) continue;
            this.rejectTrackedUpload(upload,
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
    }, server.config.statsIntervalMs);

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
