#include "WebSocketClient.h"
#include <QJsonArray>
#include <QDebug>

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

WebSocketClient::~WebSocketClient() {
    if (m_webSocket) {
        m_webSocket->close();
        m_webSocket->deleteLater();
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
}

bool WebSocketClient::isConnected() const {
    return m_webSocket && m_webSocket->state() == QAbstractSocket::ConnectedState;
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
    
    sendMessage(message);
    qDebug() << "Registering client:" << machineName << "(" << platform << ")";
}

void WebSocketClient::requestClientList() {
    if (!isConnected()) {
        qWarning() << "Cannot request client list: not connected to server";
        return;
    }
    
    QJsonObject message;
    message["type"] = "request_client_list";
    sendMessage(message);
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
    qDebug() << "Received message type:" << type;
    
    if (type == "welcome") {
        m_clientId = message["clientId"].toString();
        qDebug() << "Received client ID:" << m_clientId;
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

void WebSocketClient::setConnectionStatus(const QString& status) {
    if (m_connectionStatus != status) {
        m_connectionStatus = status;
        qDebug() << "Connection status changed to:" << status;
    }
}
