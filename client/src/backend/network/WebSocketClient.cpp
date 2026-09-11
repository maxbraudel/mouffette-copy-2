#include "backend/network/WebSocketClient.h"
#include <QJsonArray>
#include <QDebug>
#include <QUrlQuery>
#include <QUuid>
#include <QPoint>
#include <algorithm>
#include <cmath>

namespace {
bool cursorDebugEnabled() {
    static const bool enabled = qEnvironmentVariableIsSet("MOUFFETTE_CURSOR_DEBUG");
    return enabled;
}
}

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
    if (type == "upload_channel_ready") {
        m_uploadClientId = obj.value("clientId").toString();
        m_uploadChannelAuthenticated = !m_uploadClientId.isEmpty()
            && m_uploadClientId == m_clientId;
        if (!m_uploadChannelAuthenticated) {
            qWarning() << "Upload channel identity does not match the control connection";
            closeUploadChannel();
            return;
        }
        qDebug() << "Upload channel authenticated for client:" << m_uploadClientId;
        return;
    }
    if (type == "welcome") {
        // A legacy upload-channel welcome is not proof of identity. Keep the
        // dedicated socket unauthenticated and let the upload fall back to the
        // already authenticated control connection.
        qWarning() << "Upload channel server did not provide authenticated readiness";
        return;
    }
    if (type == "error" && !m_uploadChannelAuthenticated) {
        qWarning() << "Upload channel authentication failed:"
                   << obj.value("message").toString();
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
    closeUploadChannel();
    if (m_webSocket) {
        QWebSocket* const obsoleteSocket = m_webSocket;
        m_webSocket = nullptr;
        QObject::disconnect(obsoleteSocket, nullptr, this, nullptr);
        if (obsoleteSocket->state() == QAbstractSocket::ConnectedState
            || obsoleteSocket->state() == QAbstractSocket::ConnectingState) {
            obsoleteSocket->close();
        }
        obsoleteSocket->deleteLater();
    }
    
    m_serverUrl = serverUrl;
    m_webSocket = new QWebSocket();
    QWebSocket* const controlSocket = m_webSocket;

    // Pin every callback to the socket that installed it. An already queued Qt
    // signal from an obsolete socket must not mutate or close its replacement.
    connect(controlSocket, &QWebSocket::connected, this, [this, controlSocket]() {
        if (m_webSocket == controlSocket) onConnected();
    });
    connect(controlSocket, &QWebSocket::disconnected, this, [this, controlSocket]() {
        if (m_webSocket == controlSocket) onDisconnected();
    });
    connect(controlSocket, &QWebSocket::textMessageReceived, this,
            [this, controlSocket](const QString& message) {
        if (m_webSocket == controlSocket) onTextMessageReceived(message);
    });
    connect(controlSocket, &QWebSocket::errorOccurred, this,
            [this, controlSocket](QAbstractSocket::SocketError error) {
        if (m_webSocket == controlSocket) onError(error);
    });
    connect(controlSocket, &QWebSocket::bytesWritten, this,
            [this, controlSocket](qint64 bytes) {
        if (m_webSocket == controlSocket && m_uploadSessionActive
            && !m_useUploadSocketForSession) {
            emit uploadTransportBytesWritten(bytes);
        }
    });
    
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
    m_uploadChannelAuthenticated = false;
    qDebug() << "Upload channel connected";
}

void WebSocketClient::onUploadDisconnected() {
    const bool selectedTransportWasLost = m_uploadSessionActive
        && m_useUploadSocketForSession;
    m_uploadChannelAuthenticated = false;
    m_uploadChannelTokenRequested = false;
    m_uploadChannelToken.clear();
    m_uploadClientId.clear();
    qDebug() << "Upload channel disconnected";
    if (selectedTransportWasLost) {
        reportSelectedUploadTransportLost(
            QStringLiteral("Dedicated upload connection was lost"));
    } else if (isConnected()) {
        // Recreate the optional channel in the background. An upload already
        // pinned to the control socket is deliberately never switched mid-flow.
        QTimer::singleShot(0, this, [this]() { ensureUploadChannel(); });
    }
}

void WebSocketClient::onUploadError(QAbstractSocket::SocketError error) {
    m_uploadChannelAuthenticated = false;
    QString errorString;
    switch (error) {
        case QAbstractSocket::ConnectionRefusedError: errorString = "Connection refused"; break;
        case QAbstractSocket::RemoteHostClosedError: errorString = "Remote host closed connection"; break;
        case QAbstractSocket::HostNotFoundError: errorString = "Host not found"; break;
        case QAbstractSocket::SocketTimeoutError: errorString = "Connection timeout"; break;
        default: errorString = QString("Socket error: %1").arg(error);
    }
    qWarning() << "Upload WebSocket error:" << errorString;
    if (m_uploadSessionActive && m_useUploadSocketForSession) {
        reportSelectedUploadTransportLost(errorString);
    }
}

bool WebSocketClient::isConnected() const {
    return m_webSocket && m_webSocket->state() == QAbstractSocket::ConnectedState;
}

bool WebSocketClient::isUploadChannelConnected() const {
    return m_uploadChannelAuthenticated && m_uploadSocket
        && m_uploadSocket->state() == QAbstractSocket::ConnectedState;
}

bool WebSocketClient::isUploadSessionTransportAvailable() const {
    if (!m_uploadSessionActive) return false;
    if (m_useUploadSocketForSession) return isUploadChannelConnected();
    return isConnected();
}

qint64 WebSocketClient::uploadTransportBytesToWrite() const {
    if (!isUploadSessionTransportAvailable()) return -1;
    const QWebSocket* channel = m_useUploadSocketForSession ? m_uploadSocket : m_webSocket;
    return channel ? channel->bytesToWrite() : -1;
}

bool WebSocketClient::beginUploadSession(bool preferUploadChannel) {
    if (m_uploadSessionActive) {
        qWarning() << "Cannot begin a second upload session";
        return false;
    }
    if (!isConnected()) return false;

    m_uploadSessionActive = true;
    m_canceledUploads.clear();
    m_uploadTransportLossReported = false;
    m_useUploadSocketForSession = preferUploadChannel
        && isUploadChannelConnected();
    if (preferUploadChannel && !m_useUploadSocketForSession) {
        // Warm the fast path asynchronously without delaying or re-entering the
        // UI. This transfer stays on the control socket from START to COMPLETE.
        ensureUploadChannel();
    }
    return isUploadSessionTransportAvailable();
}

void WebSocketClient::endUploadSession() {
    m_uploadSessionActive = false;
    m_useUploadSocketForSession = false;
    m_uploadTransportLossReported = false;
    m_canceledUploads.clear();
}

void WebSocketClient::reportSelectedUploadTransportLost(const QString& reason) {
    if (!m_uploadSessionActive || m_uploadTransportLossReported) return;
    m_uploadTransportLossReported = true;
    emit uploadTransportLost(reason);
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
        QWebSocket* const uploadSocket = m_uploadSocket;
        connect(uploadSocket, &QWebSocket::connected, this, [this, uploadSocket]() {
            if (m_uploadSocket == uploadSocket) onUploadConnected();
        });
        connect(uploadSocket, &QWebSocket::disconnected, this, [this, uploadSocket]() {
            if (m_uploadSocket == uploadSocket) onUploadDisconnected();
        });
        connect(uploadSocket, &QWebSocket::errorOccurred, this,
                [this, uploadSocket](QAbstractSocket::SocketError error) {
            if (m_uploadSocket == uploadSocket) onUploadError(error);
        });
        connect(uploadSocket, &QWebSocket::textMessageReceived, this,
                [this, uploadSocket](const QString& message) {
            if (m_uploadSocket == uploadSocket) onUploadTextMessageReceived(message);
        });
        connect(uploadSocket, &QWebSocket::bytesWritten, this,
                [this, uploadSocket](qint64 bytes) {
            if (m_uploadSocket == uploadSocket && m_uploadSessionActive
                && m_useUploadSocketForSession) {
                emit uploadTransportBytesWritten(bytes);
            }
        });
    }

    if (m_uploadSocket->state() == QAbstractSocket::ConnectingState
        || m_uploadSocket->state() == QAbstractSocket::ConnectedState) {
        return true;
    }

    if (m_uploadChannelToken.isEmpty()) {
        if (!m_uploadChannelTokenRequested) {
            m_uploadChannelTokenRequested = true;
            QJsonObject request;
            request["type"] = "request_upload_channel";
            sendMessage(request);
        }
        return true;
    }

    QUrl url(m_serverUrl);
    QUrlQuery q(url);
    q.removeAllQueryItems("channel");
    q.removeAllQueryItems("token");
    q.addQueryItem("channel", "upload");
    q.addQueryItem("token", m_uploadChannelToken);
    url.setQuery(q);
    m_uploadChannelToken.clear(); // server tokens are deliberately one-shot
    m_uploadChannelAuthenticated = false;
    m_uploadSocket->open(url);
    return true;
}

void WebSocketClient::closeUploadChannel() {
    m_uploadChannelAuthenticated = false;
    m_uploadChannelTokenRequested = false;
    m_uploadChannelToken.clear();
    m_uploadClientId.clear();
    if (!m_uploadSocket) {
        return;
    }
    QWebSocket* const obsoleteSocket = m_uploadSocket;
    m_uploadSocket = nullptr;
    QObject::disconnect(obsoleteSocket, nullptr, this, nullptr);
    if (obsoleteSocket->state() != QAbstractSocket::UnconnectedState) {
        obsoleteSocket->close();
    }
    obsoleteSocket->deleteLater();
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

void WebSocketClient::sendCursorUpdate(int globalX, int globalY, int screenId, qreal normalizedX, qreal normalizedY) {
    if (!isConnected()) return;
    QJsonObject msg;
    msg["type"] = "cursor_update";
    msg["x"] = globalX;
    msg["y"] = globalY;
    if (screenId >= 0) {
        msg["screenId"] = screenId;
    }
    if (normalizedX >= 0.0 && normalizedY >= 0.0) {
        msg["normalizedX"] = normalizedX;
        msg["normalizedY"] = normalizedY;
    }
    if (cursorDebugEnabled()) {
        qDebug() << "[CursorDebug][WS][Send]"
                 << "global=" << QPoint(globalX, globalY)
                 << "screenId=" << screenId
                 << "norm=" << normalizedX << normalizedY;
    }
    sendMessage(msg);
}

bool WebSocketClient::sendUploadStart(const QString& targetClientId, const QJsonArray& filesManifest, const QString& uploadId, const QString& canvasSessionId) {
    if (!m_uploadSessionActive) return false;
    m_canceledUploads.remove(uploadId);
    
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
    return sendMessageUpload(msg);
}

bool WebSocketClient::sendUploadChunk(const QString& targetClientId, const QString& uploadId, const QString& fileId, int chunkIndex, const QByteArray& dataBase64, const QString& canvasSessionId) {
    if (!m_uploadSessionActive || m_canceledUploads.contains(uploadId)) return false;
    
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
    return sendMessageUpload(msg);
}

bool WebSocketClient::sendUploadComplete(const QString& targetClientId, const QString& uploadId, const QString& canvasSessionId) {
    if (!m_uploadSessionActive || m_canceledUploads.contains(uploadId)) return false;
    
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
    return sendMessageUpload(msg);
}

bool WebSocketClient::sendUploadAbort(const QString& targetClientId, const QString& uploadId, const QString& reason, const QString& canvasSessionId) {
    if (!(isConnected() || isUploadChannelConnected())) return false;
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
    // Preserve START/CHUNK/ABORT ordering on the pinned socket whenever it is
    // still usable. The authenticated control connection is only the emergency
    // path after a dedicated payload transport has already failed.
    if (m_uploadSessionActive && isUploadSessionTransportAvailable()) {
        return sendMessageUpload(msg);
    }
    return sendControlMessage(msg);
}

bool WebSocketClient::sendRemoveAllFiles(const QString& targetClientId,
                                         const QString& canvasSessionId,
                                         const QString& removalId) {
    if (!isConnected()) return false;
    
    QJsonObject msg;
    msg["type"] = "remove_all_files";
    msg["targetClientId"] = targetClientId;           // Legacy (backward compat)
    msg["targetPersistentClientId"] = targetClientId;  // PHASE 2: Explicit field
    msg["canvasSessionId"] = canvasSessionId;
    msg["removalId"] = removalId;
    return sendControlMessage(msg);
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

void WebSocketClient::notifyUploadReadyToSender(const QString& senderClientId,
                                                const QString& uploadId,
                                                const QString& canvasSessionId) {
    if (!isConnected()) return;
    QJsonObject msg;
    msg["type"] = "upload_ready";
    msg["senderClientId"] = senderClientId;
    msg["uploadId"] = uploadId;
    msg["canvasSessionId"] = canvasSessionId;
    sendMessage(msg);
}

void WebSocketClient::notifyUploadProgressToSender(const QString& senderClientId,
                                                   const QString& uploadId,
                                                   int percent,
                                                   int filesCompleted,
                                                   int totalFiles,
                                                   qint64 receivedBytes,
                                                   const QStringList& completedFileIds,
                                                   const QJsonArray& perFileProgress) {
    if (!isConnected()) return;
    QJsonObject msg;
    msg["type"] = "upload_progress";
    msg["senderClientId"] = senderClientId;
    msg["uploadId"] = uploadId;
    msg["percent"] = percent;
    msg["filesCompleted"] = filesCompleted;
    msg["totalFiles"] = totalFiles;
    msg["receivedBytes"] = static_cast<double>(std::max<qint64>(0, receivedBytes));
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

void WebSocketClient::notifyUploadFinishedToSender(const QString& senderClientId,
                                                   const QString& uploadId,
                                                   const QString& canvasSessionId,
                                                   const QStringList& validatedFileIds) {
    if (!isConnected()) return;
    QJsonObject msg;
    msg["type"] = "upload_finished";
    msg["senderClientId"] = senderClientId;
    msg["uploadId"] = uploadId;
    msg["canvasSessionId"] = canvasSessionId;
    QJsonArray fileIds;
    for (const QString& fileId : validatedFileIds) {
        fileIds.append(fileId);
    }
    msg["fileIds"] = fileIds;
    sendMessage(msg);
}

void WebSocketClient::notifyUploadRejectedToSender(const QString& senderClientId,
                                                   const QString& uploadId,
                                                   const QString& reason,
                                                   const QString& canvasSessionId) {
    if (!isConnected() || senderClientId.isEmpty() || uploadId.isEmpty()) return;
    QJsonObject msg;
    msg["type"] = "upload_rejected";
    msg["senderClientId"] = senderClientId;
    msg["uploadId"] = uploadId;
    msg["reason"] = reason.left(512);
    if (!canvasSessionId.isEmpty()) {
        msg["canvasSessionId"] = canvasSessionId;
    }
    sendMessage(msg);
}

void WebSocketClient::notifyUploadAbortAcknowledgedToSender(
    const QString& senderClientId,
    const QString& uploadId,
    const QString& canvasSessionId) {
    if (!isConnected() || senderClientId.isEmpty() || uploadId.isEmpty()) return;
    QJsonObject msg;
    msg["type"] = "upload_abort_ack";
    msg["senderClientId"] = senderClientId;
    msg["uploadId"] = uploadId;
    msg["canvasSessionId"] = canvasSessionId;
    sendMessage(msg);
}

void WebSocketClient::notifyAllFilesRemovedToSender(const QString& senderClientId,
                                                    const QString& removalId,
                                                    const QString& canvasSessionId) {
    if (!isConnected()) return;
    QJsonObject msg;
    msg["type"] = "all_files_removed";
    msg["senderClientId"] = senderClientId;
    msg["removalId"] = removalId;
    msg["canvasSessionId"] = canvasSessionId;
    sendMessage(msg);
}

void WebSocketClient::notifyAllFilesRemovalFailedToSender(
    const QString& senderClientId,
    const QString& removalId,
    const QString& canvasSessionId,
    const QString& reason) {
    if (!isConnected()) return;
    QJsonObject msg;
    msg["type"] = "remove_all_files_failed";
    msg["senderClientId"] = senderClientId;
    msg["removalId"] = removalId;
    msg["canvasSessionId"] = canvasSessionId;
    msg["reason"] = reason.left(512);
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

void WebSocketClient::sendRemoteSceneActivate(const QString& targetClientId,
                                              const QString& sceneInstanceId,
                                              qint64 activationEpochMs,
                                              int activationDelayMs) {
    if (!isConnected() || targetClientId.isEmpty() || sceneInstanceId.isEmpty()) return;
    QJsonObject msg;
    msg["type"] = "remote_scene_activate";
    msg["targetClientId"] = targetClientId;
    msg["sceneInstanceId"] = sceneInstanceId;
    msg["activationEpochMs"] = static_cast<double>(activationEpochMs);
    msg["activationDelayMs"] = std::max(0, activationDelayMs);
    if (!m_clientId.isEmpty()) msg["senderClientId"] = m_clientId;
    sendMessage(msg);
}

void WebSocketClient::sendRemoteSceneVideoSync(const QString& targetClientId,
                                               const QString& sceneInstanceId,
                                               qint64 sequence,
                                               qint64 sampledEpochMs,
                                               const QJsonArray& videos) {
    if (!isConnected() || targetClientId.isEmpty() || sceneInstanceId.isEmpty()
        || sequence <= 0 || sampledEpochMs <= 0) {
        return;
    }
    QJsonObject msg;
    msg["type"] = "remote_scene_video_sync";
    msg["targetClientId"] = targetClientId;
    msg["sceneInstanceId"] = sceneInstanceId;
    msg["sequence"] = static_cast<double>(sequence);
    msg["sampledEpochMs"] = static_cast<double>(sampledEpochMs);
    msg["videos"] = videos;
    if (!m_clientId.isEmpty()) msg["senderClientId"] = m_clientId;
    sendMessage(msg);
}

void WebSocketClient::sendRemoteSceneStop(const QString& targetClientId,
                                          const QString& sceneInstanceId) {
    if (!isConnected()) return;
    QJsonObject msg;
    msg["type"] = "remote_scene_stop";
    msg["targetClientId"] = targetClientId;
    if (!sceneInstanceId.isEmpty()) msg["sceneInstanceId"] = sceneInstanceId;
    if (!m_clientId.isEmpty()) msg["senderClientId"] = m_clientId;
    sendMessage(msg);
}

void WebSocketClient::sendRemoteSceneStopResult(const QString& senderClientId,
                                                const QString& sceneInstanceId,
                                                bool success,
                                                const QString& errorMessage) {
    if (!isConnected()) return;
    QJsonObject msg;
    msg["type"] = "remote_scene_stopped";
    msg["targetClientId"] = senderClientId;
    msg["sceneInstanceId"] = sceneInstanceId;
    msg["success"] = success;
    if (!success && !errorMessage.isEmpty()) {
        msg["error"] = errorMessage;
    }
    if (!m_clientId.isEmpty()) msg["senderClientId"] = m_clientId;
    sendMessage(msg);
}

void WebSocketClient::sendRemoteSceneValidationResult(const QString& senderClientId,
                                                      const QString& sceneInstanceId,
                                                      bool success,
                                                      const QString& errorMessage) {
    if (!isConnected()) return;
    QJsonObject msg;
    msg["type"] = "remote_scene_validation";
    msg["targetClientId"] = senderClientId; // Send back to the sender
    msg["sceneInstanceId"] = sceneInstanceId;
    msg["success"] = success;
    if (!success && !errorMessage.isEmpty()) {
        msg["error"] = errorMessage;
    }
    if (!m_clientId.isEmpty()) msg["senderClientId"] = m_clientId;
    sendMessage(msg);
}

void WebSocketClient::sendRemoteSceneLaunched(const QString& senderClientId,
                                              const QString& sceneInstanceId) {
    if (!isConnected()) return;
    QJsonObject msg;
    msg["type"] = "remote_scene_launched";
    msg["targetClientId"] = senderClientId; // Send back to the sender
    msg["sceneInstanceId"] = sceneInstanceId;
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
    closeUploadChannel();
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
    if (type != "upload_progress" && type != "upload_chunk"
        && type != "cursor_update") {
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
        // Keep the optional high-throughput channel ready before the first
        // upload. Failure is harmless; beginUploadSession() pins the control
        // channel immediately when this fast path is unavailable.
        QTimer::singleShot(0, this, [this]() { ensureUploadChannel(); });
    }
    else if (type == "upload_channel_token") {
        const QString token = message.value("token").toString();
        m_uploadChannelTokenRequested = false;
        if (token.size() >= 32 && token.size() <= 128) {
            m_uploadChannelToken = token;
            QTimer::singleShot(0, this, [this]() { ensureUploadChannel(); });
        } else {
            m_uploadChannelToken.clear();
            qWarning() << "Server returned an invalid upload channel token";
        }
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
        const int screenId = message.value("screenId").toInt(-1);
        const qreal normalizedX = message.value("normalizedX").toDouble(-1.0);
        const qreal normalizedY = message.value("normalizedY").toDouble(-1.0);
        if (cursorDebugEnabled()) {
            qDebug() << "[CursorDebug][WS][Recv]"
                     << "targetId=" << targetId
                     << "global=" << QPoint(x, y)
                     << "screenId=" << screenId
                     << "norm=" << normalizedX << normalizedY;
        }
        emit cursorPositionReceived(targetId, x, y, screenId, normalizedX, normalizedY);
    }
    else if (type == "upload_ready") {
        emit uploadReadyReceived(message.value("uploadId").toString(),
                                 message.value("canvasSessionId").toString());
    }
    else if (type == "upload_progress") {
        const QString uploadId = message.value("uploadId").toString();
        const int percent = message.value("percent").toInt();
        const int filesCompleted = message.value("filesCompleted").toInt();
        const int totalFiles = message.value("totalFiles").toInt();
        emit uploadProgressReceived(uploadId, percent, filesCompleted, totalFiles);
        const double rawReceivedBytes = message.value("receivedBytes").toDouble(-1.0);
        if (std::isfinite(rawReceivedBytes) && rawReceivedBytes >= 0.0
            && rawReceivedBytes <= static_cast<double>(64LL * 1024 * 1024 * 1024)) {
            emit uploadBytesAcknowledgedReceived(
                uploadId, static_cast<qint64>(std::llround(rawReceivedBytes)));
        }
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
    else if (type == "upload_rejected" || type == "upload_timeout") {
        const QString uploadId = message.value("uploadId").toString();
        QString reason = message.value("reason").toString();
        if (reason.isEmpty()) reason = message.value("message").toString();
        if (reason.isEmpty()) reason = QStringLiteral("Remote client rejected the upload");
        emit uploadRejectedReceived(uploadId, reason);
    }
    else if (type == "upload_aborted") {
        emit uploadAbortedReceived(message.value("uploadId").toString(),
                                   message.value("canvasSessionId").toString());
    }
    else if (type == "all_files_removed") {
        const QString removalId = message.value("removalId").toString();
        const QString targetClientId = message.value("targetPersistentClientId").toString(
            message.value("targetClientId").toString());
        const QString canvasSessionId = message.value("canvasSessionId").toString();
        emit allFilesRemovedReceived(removalId, targetClientId, canvasSessionId);
    }
    else if (type == "removal_rejected") {
        emit removalRejectedReceived(message.value("removalId").toString(),
                                     message.value("reason").toString(
                                         QStringLiteral("Remote removal was rejected")));
    }
    else if (type == "remote_scene_start") {
        const QString sender = message.value("senderClientId").toString();
        const QJsonObject scene = message.value("scene").toObject();
        emit remoteSceneStartReceived(sender, scene);
    }
    else if (type == "remote_scene_activate") {
        const QString sender = message.value("senderClientId").toString();
        const QString sceneInstanceId = message.value("sceneInstanceId").toString();
        const qint64 activationEpochMs = static_cast<qint64>(
            std::llround(message.value("activationEpochMs").toDouble(0.0)));
        const int activationDelayMs = message.value("activationDelayMs").toInt(1000);
        emit remoteSceneActivateReceived(sender, sceneInstanceId, activationEpochMs, activationDelayMs);
    }
    else if (type == "remote_scene_video_sync") {
        const QString sender = message.value("senderClientId").toString();
        const QString sceneInstanceId = message.value("sceneInstanceId").toString();
        const qint64 sequence = static_cast<qint64>(
            std::llround(message.value("sequence").toDouble(0.0)));
        const qint64 sampledEpochMs = static_cast<qint64>(
            std::llround(message.value("sampledEpochMs").toDouble(0.0)));
        const QJsonArray videos = message.value("videos").toArray();
        emit remoteSceneVideoSyncReceived(
            sender, sceneInstanceId, sequence, sampledEpochMs, videos);
    }
    else if (type == "remote_scene_stop") {
        const QString sender = message.value("senderClientId").toString();
        const QString sceneInstanceId = message.value("sceneInstanceId").toString();
        emit remoteSceneStopReceived(sender, sceneInstanceId);
    }
    else if (type == "remote_scene_stopped") {
        const QString sender = message.value("senderClientId").toString();
        const QString sceneInstanceId = message.value("sceneInstanceId").toString();
        const bool success = message.value("success").toBool(false);
        const QString error = message.value("error").toString();
        emit remoteSceneStoppedReceived(sender, sceneInstanceId, success, error);
    }
    else if (type == "remote_scene_validation") {
        const QString sender = message.value("senderClientId").toString();
        const QString sceneInstanceId = message.value("sceneInstanceId").toString();
        const bool success = message.value("success").toBool();
        const QString error = message.value("error").toString();
        emit remoteSceneValidationReceived(sender, sceneInstanceId, success, error);
    }
    else if (type == "remote_scene_launched") {
        const QString sender = message.value("senderClientId").toString();
        const QString sceneInstanceId = message.value("sceneInstanceId").toString();
        emit remoteSceneLaunchedReceived(sender, sceneInstanceId);
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
    sendControlMessage(message);
}

bool WebSocketClient::sendControlMessage(const QJsonObject& message) {
    if (!isConnected() || !m_webSocket) {
        qWarning() << "Cannot send message: not connected";
        return false;
    }

    const QJsonDocument doc(message);
    return m_webSocket->sendTextMessage(doc.toJson(QJsonDocument::Compact)) >= 0;
}

bool WebSocketClient::sendMessageUpload(const QJsonObject& message) {
    if (!m_uploadSessionActive) {
        qWarning() << "Cannot send upload payload before the session transport is locked";
        return false;
    }

    QWebSocket* channel = m_useUploadSocketForSession ? m_uploadSocket : m_webSocket;
    const bool connected = m_useUploadSocketForSession
        ? isUploadChannelConnected()
        : (channel && channel->state() == QAbstractSocket::ConnectedState);
    if (!channel || !connected) {
        qWarning() << "Upload session transport was lost; refusing to switch sockets";
        return false;
    }

    QJsonDocument doc(message);
    return channel->sendTextMessage(doc.toJson(QJsonDocument::Compact)) >= 0;
}

void WebSocketClient::setConnectionStatus(const QString& status) {
    if (m_connectionStatus != status) {
        m_connectionStatus = status;
        qDebug() << "Connection status changed to:" << status;
        emit connectionStatusChanged(m_connectionStatus);
    }
}
