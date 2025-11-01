#include "backend/network/WebSocketClient.h"
#include <QJsonArray>
#include <QDebug>
#include <QUrlQuery>
#include <QElapsedTimer>
#include <QCoreApplication>
#include <QThread>
#include <QUuid>

// ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
// 🔐 MOUFFETTE IDENTIFICATION SYSTEM - TERMINOLOGY FIX
// ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
//
// This file implements the fix for the critical "clientId" terminology confusion.
//
// PROBLEM:
// The term "clientId" was ambiguously used to mean 3 different things:
//   1. In protocol messages: persistentClientId (stable device ID)
//   2. In server code: temporary socket ID (sessionId)
//   3. Mixed usage caused bugs and maintenance issues
//
// SOLUTION (Implemented):
// Use EXPLICIT field names in all protocol messages:
//
//   Field Name              | Meaning                    | Lifetime
//   ----------------------- | -------------------------- | -------------------
//   persistentClientId      | Stable device identity     | Permanent (persisted)
//   sessionId               | Connection identifier      | Temporary (per launch)
//   socketId                | Raw WebSocket ID           | Per connection
//   canvasSessionId         | Logical scene/project      | Until canvas deleted
//   fileId                  | File deduplication key     | While file referenced
//   mediaId                 | Canvas item instance       | While item exists
//
// BACKWARD COMPATIBILITY:
// - Client sends BOTH "clientId" (legacy) and "persistentClientId" (new)
// - Server reads "persistentClientId" first, falls back to "clientId"
// - Old clients continue to work during transition
//
// MIGRATION STATUS:
// Phase 1: ✅ Client sends both fields (COMPLETE)
// Phase 2: ✅ Server reads both fields (COMPLETE)
// Phase 3: 🔄 Update all message handlers (IN PROGRESS)
// Phase 4: ⏳ Deprecate "clientId" field (future)
// Phase 5: ⏳ Remove legacy support (future v2.0)
// ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━

WebSocketClient::WebSocketClient(QObject *parent)
    : QObject(parent)
    , m_webSocket(nullptr)
    , m_connectionStatus("Disconnected")
    , m_reconnectTimer(new QTimer(this))
    , m_reconnectAttempts(0)
    , m_sessionId(QUuid::createUuid().toString(QUuid::WithoutBraces))
{
    m_reconnectTimer->setSingleShot(true);
    connect(m_reconnectTimer, &QTimer::timeout, this, &WebSocketClient::attemptReconnect);
    m_clientId = m_sessionId;
    qDebug() << "WebSocketClient: Initialized sessionId" << m_sessionId;
}

void WebSocketClient::onUploadTextMessageReceived(const QString& message) {
    QJsonParseError error;
    QJsonDocument doc = QJsonDocument::fromJson(message.toUtf8(), &error);
    if (error.error != QJsonParseError::NoError) {
        qWarning() << "Failed to parse JSON message on upload channel:" << error.errorString();
        return;
    }
    QJsonObject obj = doc.object();
    const QString type = obj.value("type").toString();
    if (type == "welcome") {
        // Keep a separate client id for the upload channel; do not override control id
        m_uploadClientId = obj.value("clientId").toString();
        qDebug() << "Upload channel received client ID:" << m_uploadClientId;
        return;
    }
    // Reuse the same message handler for upload progress/finished/all_files_removed
    handleMessage(obj);
}

WebSocketClient::~WebSocketClient() {
    if (m_webSocket) {
        m_webSocket->disconnect();  // Disconnect all signals first
        m_webSocket->close();
        m_webSocket->deleteLater();
    }
    if (m_uploadSocket) {
        m_uploadSocket->disconnect();  // Disconnect all signals first
        m_uploadSocket->close();
        m_uploadSocket->deleteLater();
        m_uploadSocket = nullptr;
    }
}

void WebSocketClient::connectToServer(const QString& serverUrl) {
    if (m_webSocket) {
        if (m_webSocket->state() == QAbstractSocket::ConnectedState || m_webSocket->state() == QAbstractSocket::ConnectingState) {
            m_webSocket->close();
        }
        m_webSocket->deleteLater();
    }
    
    m_serverUrl = serverUrl;
    m_webSocket = new QWebSocket();
    
    connect(m_webSocket, &QWebSocket::connected, this, &WebSocketClient::onConnected);
    connect(m_webSocket, &QWebSocket::disconnected, this, &WebSocketClient::onDisconnected);
    connect(m_webSocket, &QWebSocket::textMessageReceived, this, &WebSocketClient::onTextMessageReceived);
    connect(m_webSocket, &QWebSocket::errorOccurred, this, &WebSocketClient::onError);
    
    setConnectionStatus("Connecting...");
    qDebug() << "Connecting to server:" << serverUrl;
    m_webSocket->open(QUrl(serverUrl));
}

void WebSocketClient::disconnect() {
    // Mark this as a user-initiated disconnect so auto-reconnect is suppressed
    m_userInitiatedDisconnect = true;
    m_reconnectTimer->stop();
    m_reconnectAttempts = 0;
    if (m_webSocket) {
        if (m_webSocket->state() == QAbstractSocket::ConnectedState || m_webSocket->state() == QAbstractSocket::ConnectingState) {
            m_webSocket->close();
        }
    }
    if (m_uploadSocket) {
        if (m_uploadSocket->state() == QAbstractSocket::ConnectedState || m_uploadSocket->state() == QAbstractSocket::ConnectingState) {
            m_uploadSocket->close();
        }
    }
}

void WebSocketClient::onUploadConnected() {
    qDebug() << "Upload channel connected";
}

void WebSocketClient::onUploadDisconnected() {
    qDebug() << "Upload channel disconnected";
}

void WebSocketClient::onUploadError(QAbstractSocket::SocketError error) {
    QString errorString;
    switch (error) {
        case QAbstractSocket::ConnectionRefusedError: errorString = "Connection refused"; break;
        case QAbstractSocket::RemoteHostClosedError: errorString = "Remote host closed connection"; break;
        case QAbstractSocket::HostNotFoundError: errorString = "Host not found"; break;
        case QAbstractSocket::SocketTimeoutError: errorString = "Connection timeout"; break;
        default: errorString = QString("Socket error: %1").arg(error);
    }
    qWarning() << "Upload WebSocket error:" << errorString;
}

bool WebSocketClient::isConnected() const {
    return m_webSocket && m_webSocket->state() == QAbstractSocket::ConnectedState;
}

bool WebSocketClient::isUploadChannelConnected() const {
    return m_uploadSocket && m_uploadSocket->state() == QAbstractSocket::ConnectedState;
}

bool WebSocketClient::prepareUploadChannel(int timeoutMs) {
    if (isUploadChannelConnected()) {
        return true;
    }

    if (!ensureUploadChannel()) {
        return false;
    }

    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < timeoutMs) {
        if (isUploadChannelConnected()) {
            return true;
        }
        QCoreApplication::processEvents();
        QThread::msleep(20);
    }

    qWarning() << "Upload channel did not connect within timeout";
    return false;
}

void WebSocketClient::beginUploadSession(bool preferUploadChannel) {
    if (m_uploadSessionActive) {
        if (preferUploadChannel && !m_useUploadSocketForSession) {
            if (prepareUploadChannel()) {
                m_useUploadSocketForSession = true;
            }
        }
        return;
    }

    m_uploadSessionActive = true;
    m_useUploadSocketForSession = false;

    if (!preferUploadChannel) {
        return;
    }

    static QElapsedTimer throttleTimer;
    static bool timerInitialized = false;
    if (!timerInitialized) {
        throttleTimer.start();
        timerInitialized = true;
    }

    if (!isUploadChannelConnected()) {
        if (!ensureUploadChannel()) {
            qWarning() << "Failed to initiate dedicated upload channel";
        }
    }

    if (prepareUploadChannel()) {
        m_useUploadSocketForSession = true;
    } else {
        if (!timerInitialized || throttleTimer.elapsed() > 2000) {
            qWarning() << "Falling back to control channel for this upload session";
            throttleTimer.restart();
        }
    }
}

void WebSocketClient::endUploadSession() {
    m_uploadSessionActive = false;
    m_useUploadSocketForSession = false;
    closeUploadChannel();
}

bool WebSocketClient::ensureUploadChannel() {
    if (isUploadChannelConnected()) {
        return true;
    }

    if (!isConnected()) {
        qWarning() << "Cannot open upload channel without control connection";
        return false;
    }

    if (!m_uploadSocket) {
        m_uploadSocket = new QWebSocket();
        connect(m_uploadSocket, &QWebSocket::connected, this, &WebSocketClient::onUploadConnected);
        connect(m_uploadSocket, &QWebSocket::disconnected, this, &WebSocketClient::onUploadDisconnected);
        connect(m_uploadSocket, &QWebSocket::errorOccurred, this, &WebSocketClient::onUploadError);
        connect(m_uploadSocket, &QWebSocket::textMessageReceived, this, &WebSocketClient::onUploadTextMessageReceived);
    }

    if (m_uploadSocket->state() == QAbstractSocket::ConnectingState) {
        return true;
    }

    if (m_uploadSocket->state() == QAbstractSocket::ConnectedState) {
        return true;
    }

    QUrl url(m_serverUrl);
    QUrlQuery q(url);
    q.addQueryItem("channel", "upload");
    url.setQuery(q);
    m_uploadSocket->open(url);
    return true;
}

void WebSocketClient::closeUploadChannel() {
    if (!m_uploadSocket) {
        return;
    }
    if (m_uploadSocket->state() != QAbstractSocket::UnconnectedState) {
        m_uploadSocket->close();
    }
    m_uploadSocket->deleteLater();
    m_uploadSocket = nullptr;
}

void WebSocketClient::registerClient(const QString& machineName, const QString& platform, const QList<ScreenInfo>& screens, int volumePercent) {
    if (!isConnected()) {
        qWarning() << "Cannot register client: not connected to server";
        return;
    }
    
    QJsonObject message;
    message["type"] = "register";
    message["machineName"] = machineName;
    message["platform"] = platform;
    if (volumePercent >= 0) message["volumePercent"] = volumePercent;
    message["sessionId"] = m_sessionId;
    
    // ✅ ID TERMINOLOGY FIX: Send both fields for server compatibility
    // - "persistentClientId": NEW explicit field name (recommended)
    // - "clientId": LEGACY field (backward compatibility, will be deprecated)
    // The ambiguous "clientId" terminology is being phased out in favor of explicit naming.
    if (!m_persistentClientId.isEmpty()) {
        message["clientId"] = m_persistentClientId;           // ⚠️ LEGACY: For old server versions
        message["persistentClientId"] = m_persistentClientId;  // ✅ NEW: Explicit and unambiguous
    }
    
    if (!screens.isEmpty()) {
        QJsonArray screensArray;
        for (const auto& screen : screens) {
            screensArray.append(screen.toJson());
        }
        message["screens"] = screensArray;
    }
    // Legacy systemUI field removed; per-screen uiZones now embedded in screens
    
    sendMessage(message);
    qDebug() << "Registering client:" << machineName << "(" << platform << ") with persistentId:" << m_persistentClientId;
}

void WebSocketClient::requestScreens(const QString& targetClientId) {
    if (!isConnected()) {
        qWarning() << "Cannot request screens: not connected to server";
        return;
    }
    QJsonObject message;
    message["type"] = "request_screens";
    message["targetClientId"] = targetClientId;
    sendMessage(message);
}

void WebSocketClient::watchScreens(const QString& targetClientId) {
    if (!isConnected()) {
        qWarning() << "Cannot watch screens: not connected to server";
        return;
    }
    QJsonObject message;
    message["type"] = "watch_screens";
    message["targetClientId"] = targetClientId;
    sendMessage(message);
}

void WebSocketClient::unwatchScreens(const QString& targetClientId) {
    if (!isConnected()) {
    qWarning() << "Cannot unwatch screens: not connected to server";
    return;
    }
    QJsonObject message;
    message["type"] = "unwatch_screens";
    message["targetClientId"] = targetClientId;
    sendMessage(message);
}

void WebSocketClient::sendStateSnapshot(const QList<ScreenInfo>& screens, int volumePercent) {
    if (!isConnected()) return;
    QJsonObject msg;
    msg["type"] = "register"; // reuse register payload to update server-side cache
    // Keep last known identity fields if available from previous registration
    // Identity fields are expected to be sent via syncRegistration by MainWindow
    QJsonArray arr;
    for (const auto& s : screens) arr.append(s.toJson());
    msg["screens"] = arr;
    if (volumePercent >= 0) msg["volumePercent"] = volumePercent;
    // Legacy systemUI omitted
    sendMessage(msg);
}

void WebSocketClient::sendCursorUpdate(int globalX, int globalY) {
    if (!isConnected()) return;
    QJsonObject msg;
    msg["type"] = "cursor_update";
    msg["x"] = globalX;
    msg["y"] = globalY;
    sendMessage(msg);
}

void WebSocketClient::sendUploadStart(const QString& targetClientId, const QJsonArray& filesManifest, const QString& uploadId, const QString& canvasSessionId) {
    if (!(isConnected() || isUploadChannelConnected())) return;
    
    QJsonObject msg;
    msg["type"] = "upload_start";
    msg["targetClientId"] = targetClientId;           // PHASE 2: Still called targetClientId (refers to persistentClientId)
    msg["targetPersistentClientId"] = targetClientId;  // PHASE 2: Explicit field name for clarity
    msg["uploadId"] = uploadId;
    msg["files"] = filesManifest;
    msg["canvasSessionId"] = canvasSessionId;
    if (!m_clientId.isEmpty()) {
        msg["senderClientId"] = m_clientId;           // Legacy (backward compat)
        msg["senderPersistentClientId"] = m_clientId;  // PHASE 2: Explicit field
    }
    sendMessageUpload(msg);
}

void WebSocketClient::sendUploadChunk(const QString& targetClientId, const QString& uploadId, const QString& fileId, int chunkIndex, const QByteArray& dataBase64, const QString& canvasSessionId) {
    if (!(isConnected() || isUploadChannelConnected())) return;
    if (m_canceledUploads.contains(uploadId)) return; // drop silently
    
    QJsonObject msg;
    msg["type"] = "upload_chunk";
    msg["targetClientId"] = targetClientId;           // Legacy (backward compat)
    msg["targetPersistentClientId"] = targetClientId;  // PHASE 2: Explicit field
    msg["uploadId"] = uploadId;
    msg["fileId"] = fileId;
    msg["chunkIndex"] = chunkIndex;
    // Ensure Base64 encoded string (if caller passed raw bytes, encode here)
    QByteArray payload = dataBase64;
    // Heuristic: contains non-base64 characters? encode
    if (!payload.isEmpty() && (payload.contains('\n') || payload.contains('{') || payload.contains('\0'))) {
        payload = payload.toBase64();
    }
    msg["data"] = QString::fromUtf8(payload);
    msg["canvasSessionId"] = canvasSessionId; // now mandatory
    if (!m_clientId.isEmpty()) {
        msg["senderClientId"] = m_clientId;           // Legacy (backward compat)
        msg["senderPersistentClientId"] = m_clientId;  // PHASE 2: Explicit field
    }
    sendMessageUpload(msg);
}

void WebSocketClient::sendUploadComplete(const QString& targetClientId, const QString& uploadId, const QString& canvasSessionId) {
    if (!(isConnected() || isUploadChannelConnected())) return;
    if (m_canceledUploads.contains(uploadId)) return; // already canceled
    
    QJsonObject msg;
    msg["type"] = "upload_complete";
    msg["targetClientId"] = targetClientId;           // Legacy (backward compat)
    msg["targetPersistentClientId"] = targetClientId;  // PHASE 2: Explicit field
    msg["uploadId"] = uploadId;
    msg["canvasSessionId"] = canvasSessionId;
    if (!m_clientId.isEmpty()) {
        msg["senderClientId"] = m_clientId;           // Legacy (backward compat)
        msg["senderPersistentClientId"] = m_clientId;  // PHASE 2: Explicit field
    }
    sendMessageUpload(msg);
}

void WebSocketClient::sendUploadAbort(const QString& targetClientId, const QString& uploadId, const QString& reason, const QString& canvasSessionId) {
    if (!(isConnected() || isUploadChannelConnected())) return;
    m_canceledUploads.insert(uploadId);
    
    QJsonObject msg;
    msg["type"] = "upload_abort";
    msg["targetClientId"] = targetClientId;           // Legacy (backward compat)
    msg["targetPersistentClientId"] = targetClientId;  // PHASE 2: Explicit field
    msg["uploadId"] = uploadId;
    msg["canvasSessionId"] = canvasSessionId;
    if (!reason.isEmpty()) msg["reason"] = reason;
    if (!m_clientId.isEmpty()) {
        msg["senderClientId"] = m_clientId;           // Legacy (backward compat)
        msg["senderPersistentClientId"] = m_clientId;  // PHASE 2: Explicit field
    }
    sendMessageUpload(msg);
}

void WebSocketClient::sendRemoveAllFiles(const QString& targetClientId, const QString& canvasSessionId) {
    if (!isConnected()) return;
    
    QJsonObject msg;
    msg["type"] = "remove_all_files";
    msg["targetClientId"] = targetClientId;           // Legacy (backward compat)
    msg["targetPersistentClientId"] = targetClientId;  // PHASE 2: Explicit field
    msg["canvasSessionId"] = canvasSessionId;
    sendMessage(msg);
}

void WebSocketClient::sendRemoveFile(const QString& targetClientId, const QString& canvasSessionId, const QString& fileId) {
    if (!isConnected()) return;
    
    QJsonObject msg;
    msg["type"] = "remove_file";
    msg["targetClientId"] = targetClientId;           // Legacy (backward compat)
    msg["targetPersistentClientId"] = targetClientId;  // PHASE 2: Explicit field
    msg["fileId"] = fileId;
    msg["canvasSessionId"] = canvasSessionId;
    if (!m_clientId.isEmpty()) {
        msg["senderClientId"] = m_clientId;           // Legacy (backward compat)
        msg["senderPersistentClientId"] = m_clientId;  // PHASE 2: Explicit field
    }
    qDebug() << "Sending remove_file command for fileId:" << fileId << "idea:" << canvasSessionId << "to client:" << targetClientId;
    sendMessage(msg);
}

// PHASE 2: Canvas lifecycle notifications (CRITICAL for canvasSessionId validation)
void WebSocketClient::sendCanvasCreated(const QString& persistentClientId, const QString& canvasSessionId) {
    if (!isConnected()) return;
    
    QJsonObject msg;
    msg["type"] = "canvas_created";
    msg["persistentClientId"] = persistentClientId;
    msg["canvasSessionId"] = canvasSessionId;
    
    sendMessage(msg);
    qDebug() << "Notified server: canvas created for client:" << persistentClientId << "canvasSessionId:" << canvasSessionId;
}

void WebSocketClient::sendCanvasDeleted(const QString& persistentClientId, const QString& canvasSessionId) {
    if (!isConnected()) return;
    
    QJsonObject msg;
    msg["type"] = "canvas_deleted";
    msg["persistentClientId"] = persistentClientId;
    msg["canvasSessionId"] = canvasSessionId;
    
    sendMessage(msg);
    qDebug() << "Notified server: canvas deleted for client:" << persistentClientId << "canvasSessionId:" << canvasSessionId;
}

void WebSocketClient::notifyUploadProgressToSender(const QString& senderClientId, const QString& uploadId, int percent, int filesCompleted, int totalFiles, const QStringList& completedFileIds, const QJsonArray& perFileProgress) {
    if (!isConnected()) return;
    QJsonObject msg;
    msg["type"] = "upload_progress";
    msg["senderClientId"] = senderClientId;
    msg["uploadId"] = uploadId;
    msg["percent"] = percent;
    msg["filesCompleted"] = filesCompleted;
    msg["totalFiles"] = totalFiles;
    if (!completedFileIds.isEmpty()) {
        QJsonArray arr;
        for (const QString& fid : completedFileIds) arr.append(fid);
        msg["completedFileIds"] = arr;
    }
    if (!perFileProgress.isEmpty()) {
        msg["perFileProgress"] = perFileProgress;
    }
    sendMessage(msg);
}

void WebSocketClient::notifyUploadFinishedToSender(const QString& senderClientId, const QString& uploadId) {
    if (!isConnected()) return;
    QJsonObject msg;
    msg["type"] = "upload_finished";
    msg["senderClientId"] = senderClientId;
    msg["uploadId"] = uploadId;
    sendMessage(msg);
}

void WebSocketClient::notifyAllFilesRemovedToSender(const QString& senderClientId) {
    if (!isConnected()) return;
    QJsonObject msg;
    msg["type"] = "all_files_removed";
    msg["senderClientId"] = senderClientId;
    sendMessage(msg);
}

void WebSocketClient::sendRemoteSceneStart(const QString& targetClientId, const QJsonObject& scenePayload) {
    if (!isConnected()) return;
    QJsonObject msg;
    msg["type"] = "remote_scene_start";
    msg["targetClientId"] = targetClientId;
    msg["scene"] = scenePayload; // contains screens + media arrays
    if (!m_clientId.isEmpty()) msg["senderClientId"] = m_clientId;
    sendMessage(msg);
}

void WebSocketClient::sendRemoteSceneStop(const QString& targetClientId) {
    if (!isConnected()) return;
    QJsonObject msg;
    msg["type"] = "remote_scene_stop";
    msg["targetClientId"] = targetClientId;
    if (!m_clientId.isEmpty()) msg["senderClientId"] = m_clientId;
    sendMessage(msg);
}

void WebSocketClient::sendRemoteSceneStopResult(const QString& senderClientId, bool success, const QString& errorMessage) {
    if (!isConnected()) return;
    QJsonObject msg;
    msg["type"] = "remote_scene_stopped";
    msg["targetClientId"] = senderClientId;
    msg["success"] = success;
    if (!success && !errorMessage.isEmpty()) {
        msg["error"] = errorMessage;
    }
    if (!m_clientId.isEmpty()) msg["senderClientId"] = m_clientId;
    sendMessage(msg);
}

void WebSocketClient::sendRemoteSceneValidationResult(const QString& senderClientId, bool success, const QString& errorMessage) {
    if (!isConnected()) return;
    QJsonObject msg;
    msg["type"] = "remote_scene_validation";
    msg["targetClientId"] = senderClientId; // Send back to the sender
    msg["success"] = success;
    if (!success && !errorMessage.isEmpty()) {
        msg["error"] = errorMessage;
    }
    if (!m_clientId.isEmpty()) msg["senderClientId"] = m_clientId;
    sendMessage(msg);
}

void WebSocketClient::sendRemoteSceneLaunched(const QString& senderClientId) {
    if (!isConnected()) return;
    QJsonObject msg;
    msg["type"] = "remote_scene_launched";
    msg["targetClientId"] = senderClientId; // Send back to the sender
    if (!m_clientId.isEmpty()) msg["senderClientId"] = m_clientId;
    sendMessage(msg);
}

void WebSocketClient::onConnected() {
    qDebug() << "Connected to server";
    setConnectionStatus("Connected");
    // Clear user-initiated flag upon successful connection
    m_userInitiatedDisconnect = false;
    m_reconnectAttempts = 0;
    m_reconnectTimer->stop();
    emit connected();
}

void WebSocketClient::onDisconnected() {
    qDebug() << "Disconnected from server";
    // If user initiated, keep status as Disconnected (no error, no reconnect)
    setConnectionStatus("Disconnected");
    emit disconnected();
    
    // Attempt to reconnect if we haven't reached the max attempts and user didn't disconnect
    if (!m_userInitiatedDisconnect && m_reconnectAttempts < MAX_RECONNECT_ATTEMPTS) {
        m_reconnectAttempts++;
        setConnectionStatus(QString("Reconnecting... (attempt %1/%2)").arg(m_reconnectAttempts).arg(MAX_RECONNECT_ATTEMPTS));
        m_reconnectTimer->start(RECONNECT_INTERVAL);
    } else {
        if (!m_userInitiatedDisconnect) {
            setConnectionStatus("Connection failed");
            emit connectionError("Failed to reconnect after multiple attempts");
        }
    }
}

void WebSocketClient::onTextMessageReceived(const QString& message) {
    QJsonParseError error;
    QJsonDocument doc = QJsonDocument::fromJson(message.toUtf8(), &error);
    
    if (error.error != QJsonParseError::NoError) {
        qWarning() << "Failed to parse JSON message:" << error.errorString();
        return;
    }
    
    QJsonObject messageObj = doc.object();
    handleMessage(messageObj);
}

void WebSocketClient::onError(QAbstractSocket::SocketError error) {
    QString errorString;
    int reconnectDelayMs = 5000; // Default 5 seconds
    
    // PHASE 1: Differentiated error handling with appropriate retry strategies
    switch (error) {
        case QAbstractSocket::ConnectionRefusedError:
            errorString = "Connection refused";
            reconnectDelayMs = 10000; // Server down, retry after 10 seconds
            break;
            
        case QAbstractSocket::RemoteHostClosedError:
            errorString = "Remote host closed connection";
            reconnectDelayMs = 3000; // Quick retry, might be server restart
            break;
            
        case QAbstractSocket::HostNotFoundError:
            errorString = "Host not found";
            reconnectDelayMs = 2000; // DNS issue, quick retry
            break;
            
        case QAbstractSocket::SocketTimeoutError:
            errorString = "Connection timeout";
            reconnectDelayMs = 5000; // Network issue, moderate retry
            break;
            
        case QAbstractSocket::NetworkError:
            errorString = "Network error";
            reconnectDelayMs = 2000; // Temporary network issue, quick retry
            break;
            
        case QAbstractSocket::SslHandshakeFailedError:
            errorString = "SSL handshake failed";
            reconnectDelayMs = -1; // Fatal error, don't retry automatically
            emit fatalError("SSL/TLS error - check certificates");
            break;
            
        default:
            errorString = QString("Socket error: %1").arg(error);
            reconnectDelayMs = 5000;
    }
    
    // Suppress error status if this is a user-initiated disconnect flow
    if (m_userInitiatedDisconnect) {
        qDebug() << "Ignoring socket error due to user-initiated disconnect:" << errorString;
        return;
    }
    
    qWarning() << "WebSocket error:" << errorString;
    setConnectionStatus("Error: " + errorString);
    emit connectionError(errorString);
    
    // PHASE 1: Schedule reconnection with appropriate delay (if not fatal)
    if (reconnectDelayMs > 0 && !m_reconnectTimer->isActive()) {
        qDebug() << "Will retry connection in" << reconnectDelayMs << "ms";
        m_reconnectTimer->start(reconnectDelayMs);
    }
}

void WebSocketClient::attemptReconnect() {
    if (m_reconnectAttempts <= MAX_RECONNECT_ATTEMPTS) {
        qDebug() << "Attempting to reconnect..." << m_reconnectAttempts << "/" << MAX_RECONNECT_ATTEMPTS;
        connectToServer(m_serverUrl);
    }
}

void WebSocketClient::handleMessage(const QJsonObject& message) {
    QString type = message["type"].toString();
    // Suppress noisy logs for high-frequency message types
    if (type != "upload_progress" && type != "cursor_update") {
        qDebug() << "Received message type:" << type;
    }
    
    if (type == "welcome") {
        // ✅ ID TERMINOLOGY FIX: Read explicit "socketId" field (new) with fallback to "clientId" (legacy)
        m_socketClientId = message.contains("socketId") 
            ? message["socketId"].toString() 
            : message["clientId"].toString();
        qDebug() << "Received welcome with socket ID:" << m_socketClientId;
    }
    else if (type == "error") {
        const QString err = message.value("message").toString();
        qWarning() << "❌ Server error:" << err;
        
        // CRITICAL FIX: Distinguish between connection errors and business logic errors
        // "Target client not found" is expected when a remote client disconnects
        // It should NOT trigger a reconnection or show "Disconnected" status
        if (err.contains("Target client not found", Qt::CaseInsensitive)) {
            // Business logic error - remote client is offline, this is normal
            qDebug() << "Remote client is offline, ignoring error (not a connection issue)";
            // Don't emit connectionError - this would trigger reconnection
            return;
        }
        
        // For other errors, emit signal so UI can show the error to user
        emit connectionError(err);
    }
    else if (type == "registration_confirmed") {
        QJsonObject clientInfoObj = message["clientInfo"].toObject();
        ClientInfo clientInfo = ClientInfo::fromJson(clientInfoObj);
        if (!clientInfo.getId().isEmpty()) {
            m_clientId = clientInfo.getId();
        }
        qDebug() << "Registration confirmed for session" << m_clientId << "persistent" << clientInfo.clientId();
        emit registrationConfirmed(clientInfo);
    }
    else if (type == "client_list") {
        QJsonArray clientsArray = message["clients"].toArray();
        QList<ClientInfo> clients;
        
        for (const auto& clientValue : clientsArray) {
            ClientInfo client = ClientInfo::fromJson(clientValue.toObject());
            clients.append(client);
        }
        
        emit clientListReceived(clients);
    }
    else if (type == "screens_info") {
        QJsonObject clientInfoObj = message["clientInfo"].toObject();
        ClientInfo clientInfo = ClientInfo::fromJson(clientInfoObj);
        emit screensInfoReceived(clientInfo);
    }
    else if (type == "watch_status") {
        bool watched = message["watched"].toBool(false);
        emit watchStatusChanged(watched);
    }
    else if (type == "data_request") {
        emit dataRequestReceived();
    }
    else if (type == "cursor_update") {
        // Forward to UI with target id context
        const QString targetId = message.value("targetClientId").toString();
        const int x = message.value("x").toInt();
        const int y = message.value("y").toInt();
        emit cursorPositionReceived(targetId, x, y);
    }
    else if (type == "upload_progress") {
        const QString uploadId = message.value("uploadId").toString();
        const int percent = message.value("percent").toInt();
        const int filesCompleted = message.value("filesCompleted").toInt();
        const int totalFiles = message.value("totalFiles").toInt();
        emit uploadProgressReceived(uploadId, percent, filesCompleted, totalFiles);
        if (message.contains("completedFileIds") && message.value("completedFileIds").isArray()) {
            QStringList ids;
            const QJsonArray arr = message.value("completedFileIds").toArray();
            ids.reserve(arr.size());
            for (const auto& v : arr) ids.append(v.toString());
            emit uploadCompletedFileIdsReceived(uploadId, ids);
        }
        if (message.contains("perFileProgress") && message.value("perFileProgress").isArray()) {
            const QJsonArray arr = message.value("perFileProgress").toArray();
            QHash<QString,int> map;
            for (const auto& v : arr) {
                const QJsonObject o = v.toObject();
                const QString fid = o.value("fileId").toString();
                const int p = o.value("percent").toInt();
                if (!fid.isEmpty()) map.insert(fid, p);
            }
            if (!map.isEmpty()) emit uploadPerFileProgressReceived(uploadId, map);
        }
    }
    else if (type == "upload_finished") {
        const QString uploadId = message.value("uploadId").toString();
        emit uploadFinishedReceived(uploadId);
    }
    else if (type == "all_files_removed") {
        emit allFilesRemovedReceived();
    }
    else if (type == "remote_scene_start") {
        const QString sender = message.value("senderClientId").toString();
        const QJsonObject scene = message.value("scene").toObject();
        emit remoteSceneStartReceived(sender, scene);
    }
    else if (type == "remote_scene_stop") {
        const QString sender = message.value("senderClientId").toString();
        emit remoteSceneStopReceived(sender);
    }
    else if (type == "remote_scene_stopped") {
        const QString sender = message.value("senderClientId").toString();
        const bool success = message.value("success").toBool(false);
        const QString error = message.value("error").toString();
        emit remoteSceneStoppedReceived(sender, success, error);
    }
    else if (type == "remote_scene_validation") {
        const QString sender = message.value("senderClientId").toString();
        const bool success = message.value("success").toBool();
        const QString error = message.value("error").toString();
        emit remoteSceneValidationReceived(sender, success, error);
    }
    else if (type == "remote_scene_launched") {
        const QString sender = message.value("senderClientId").toString();
        emit remoteSceneLaunchedReceived(sender);
    }
    else if (type == "state_sync") {
        // PHASE 2: Handle server state synchronization after reconnection
        qDebug() << "WebSocketClient: Received state_sync from server";
        // Forward to MainWindow for processing
        emit messageReceived(message);
    }
    else {
        // Forward unknown messages
        emit messageReceived(message);
    }
}

void WebSocketClient::sendMessage(const QJsonObject& message) {
    if (!isConnected()) {
        qWarning() << "Cannot send message: not connected";
        return;
    }
    
    QJsonDocument doc(message);
    QString jsonString = doc.toJson(QJsonDocument::Compact);
    m_webSocket->sendTextMessage(jsonString);
}

void WebSocketClient::sendMessageUpload(const QJsonObject& message) {
    const QString type = message.value("type").toString();
    QWebSocket* channel = nullptr;
    bool attemptedUploadChannel = false;

    if (m_useUploadSocketForSession) {
        attemptedUploadChannel = true;
        if (isUploadChannelConnected()) {
            channel = m_uploadSocket;
        }
    }

    if (!channel && isConnected()) {
        channel = m_webSocket;
        if (attemptedUploadChannel) {
            qWarning() << "Falling back to control channel for upload session";
            m_useUploadSocketForSession = false;
            closeUploadChannel();
        }
    }

    if (!channel || channel->state() != QAbstractSocket::ConnectedState) {
        qWarning() << "Cannot send upload message: no connected websocket available";
        return;
    }

    QJsonDocument doc(message);
    channel->sendTextMessage(doc.toJson(QJsonDocument::Compact));
}

void WebSocketClient::setConnectionStatus(const QString& status) {
    if (m_connectionStatus != status) {
        m_connectionStatus = status;
        qDebug() << "Connection status changed to:" << status;
        emit connectionStatusChanged(m_connectionStatus);
    }
}
