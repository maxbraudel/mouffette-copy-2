#include "WebSocketClient.h"
#include <QJsonArray>
#include <QDebug>
#include <QUrlQuery>

WebSocketClient::WebSocketClient(QObject *parent)
    : QObject(parent)
    , m_webSocket(nullptr)
    , m_connectionStatus("Disconnected")
    , m_reconnectTimer(new QTimer(this))
    , m_reconnectAttempts(0)
{
    m_reconnectTimer->setSingleShot(true);
    connect(m_reconnectTimer, &QTimer::timeout, this, &WebSocketClient::attemptReconnect);
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
        m_webSocket->close();
        m_webSocket->deleteLater();
    }
    if (m_uploadSocket) {
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

bool WebSocketClient::ensureUploadChannel() {
    if (isUploadChannelConnected()) return true;
    if (!m_uploadSocket) {
        m_uploadSocket = new QWebSocket();
        connect(m_uploadSocket, &QWebSocket::connected, this, &WebSocketClient::onUploadConnected);
        connect(m_uploadSocket, &QWebSocket::disconnected, this, &WebSocketClient::onUploadDisconnected);
        connect(m_uploadSocket, &QWebSocket::errorOccurred, this, &WebSocketClient::onUploadError);
        // Use a dedicated slot so the upload channel's 'welcome' doesn't override m_clientId
        connect(m_uploadSocket, &QWebSocket::textMessageReceived, this, &WebSocketClient::onUploadTextMessageReceived);
    }
    // Open same server URL with a hint that this is upload channel
    QUrl url(m_serverUrl);
    QUrlQuery q(url);
    q.addQueryItem("channel", "upload");
    url.setQuery(q);
    m_uploadSocket->open(url);
    return true;
}

void WebSocketClient::closeUploadChannel() {
    if (!m_uploadSocket) return;
    if (m_uploadSocket->state() == QAbstractSocket::ConnectedState || m_uploadSocket->state() == QAbstractSocket::ConnectingState) {
        m_uploadSocket->close();
    }
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
    
    if (!screens.isEmpty()) {
        QJsonArray screensArray;
        for (const auto& screen : screens) {
            screensArray.append(screen.toJson());
        }
        message["screens"] = screensArray;
    }
    // Legacy systemUI field removed; per-screen uiZones now embedded in screens
    
    sendMessage(message);
    qDebug() << "Registering client:" << machineName << "(" << platform << ")";
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

void WebSocketClient::sendUploadStart(const QString& targetClientId, const QJsonArray& filesManifest, const QString& uploadId) {
    if (!(isConnected() || isUploadChannelConnected())) return;
    QJsonObject msg;
    msg["type"] = "upload_start";
    msg["targetClientId"] = targetClientId;
    msg["uploadId"] = uploadId;
    msg["files"] = filesManifest;
    if (!m_clientId.isEmpty()) msg["senderClientId"] = m_clientId;
    sendMessageUpload(msg);
}

void WebSocketClient::sendUploadChunk(const QString& targetClientId, const QString& uploadId, const QString& fileId, int chunkIndex, const QByteArray& dataBase64) {
    if (!(isConnected() || isUploadChannelConnected())) return;
    if (m_canceledUploads.contains(uploadId)) return; // drop silently
    QJsonObject msg;
    msg["type"] = "upload_chunk";
    msg["targetClientId"] = targetClientId;
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
    if (!m_clientId.isEmpty()) msg["senderClientId"] = m_clientId;
    sendMessageUpload(msg);
}

void WebSocketClient::sendUploadComplete(const QString& targetClientId, const QString& uploadId) {
    if (!(isConnected() || isUploadChannelConnected())) return;
    if (m_canceledUploads.contains(uploadId)) return; // already canceled
    QJsonObject msg;
    msg["type"] = "upload_complete";
    msg["targetClientId"] = targetClientId;
    msg["uploadId"] = uploadId;
    if (!m_clientId.isEmpty()) msg["senderClientId"] = m_clientId;
    sendMessageUpload(msg);
}

void WebSocketClient::sendUploadAbort(const QString& targetClientId, const QString& uploadId, const QString& reason) {
    if (!(isConnected() || isUploadChannelConnected())) return;
    m_canceledUploads.insert(uploadId);
    QJsonObject msg;
    msg["type"] = "upload_abort";
    msg["targetClientId"] = targetClientId;
    msg["uploadId"] = uploadId;
    if (!reason.isEmpty()) msg["reason"] = reason;
    if (!m_clientId.isEmpty()) msg["senderClientId"] = m_clientId;
    sendMessageUpload(msg);
}

void WebSocketClient::sendRemoveAllFiles(const QString& targetClientId) {
    if (!isConnected()) return;
    QJsonObject msg;
    msg["type"] = "remove_all_files";
    msg["targetClientId"] = targetClientId;
    sendMessage(msg);
}

void WebSocketClient::sendRemoveFile(const QString& targetClientId, const QString& fileId) {
    if (!isConnected()) return;
    QJsonObject msg;
    msg["type"] = "remove_file";
    msg["targetClientId"] = targetClientId;
    msg["fileId"] = fileId;
    if (!m_clientId.isEmpty()) msg["senderClientId"] = m_clientId;
    qDebug() << "Sending remove_file command for fileId:" << fileId << "to client:" << targetClientId;
    sendMessage(msg);
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
    switch (error) {
        case QAbstractSocket::ConnectionRefusedError:
            errorString = "Connection refused";
            break;
        case QAbstractSocket::RemoteHostClosedError:
            errorString = "Remote host closed connection";
            break;
        case QAbstractSocket::HostNotFoundError:
            errorString = "Host not found";
            break;
        case QAbstractSocket::SocketTimeoutError:
            errorString = "Connection timeout";
            break;
        default:
            errorString = QString("Socket error: %1").arg(error);
    }
    // Suppress error status if this is a user-initiated disconnect flow
    if (m_userInitiatedDisconnect) {
        qDebug() << "Ignoring socket error due to user-initiated disconnect:" << errorString;
        return;
    }
    qWarning() << "WebSocket error:" << errorString;
    setConnectionStatus("Error: " + errorString);
    emit connectionError(errorString);
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
        m_clientId = message["clientId"].toString();
        qDebug() << "Received client ID:" << m_clientId;
    }
    else if (type == "error") {
        const QString err = message.value("message").toString();
        qWarning() << "Server error:" << err;
    }
    else if (type == "registration_confirmed") {
        QJsonObject clientInfoObj = message["clientInfo"].toObject();
        ClientInfo clientInfo = ClientInfo::fromJson(clientInfoObj);
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
    // Prefer upload channel if connected; otherwise use control channel as fallback
    if (isUploadChannelConnected()) {
        QJsonDocument doc(message);
        QString jsonString = doc.toJson(QJsonDocument::Compact);
        m_uploadSocket->sendTextMessage(jsonString);
        return;
    }
    // Fallback
    sendMessage(message);
}

void WebSocketClient::setConnectionStatus(const QString& status) {
    if (m_connectionStatus != status) {
        m_connectionStatus = status;
        qDebug() << "Connection status changed to:" << status;
        emit connectionStatusChanged(m_connectionStatus);
    }
}
