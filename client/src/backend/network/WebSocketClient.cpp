#include "backend/network/WebSocketClient.h"
#include "backend/config/AppConfig.h"
#include "backend/network/SceneRunCoordinator.h"
#include "backend/security/DeviceIdentityStore.h"
#include "MediaFormatContract.h"
#include <QJsonArray>
#include <QDebug>
#include <QUrlQuery>
#include <QUuid>
#include <QRegularExpression>
#include <QDateTime>
#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>

#if defined(Q_OS_MACOS)
#include <mach/mach_time.h>
#elif defined(Q_OS_WIN)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#elif defined(Q_OS_LINUX)
#include <time.h>
#endif

namespace {
constexpr int kClockSyncBurstProbeCount = 5;
constexpr int kClockSyncBurstIntervalMs = 100;
constexpr int kClockSyncBurstCooldownMs = 1000;

qint64 systemSuspendInclusiveMonotonicMs()
{
#if defined(Q_OS_MACOS)
    static mach_timebase_info_data_t timebase = [] {
        mach_timebase_info_data_t value{};
        mach_timebase_info(&value);
        return value;
    }();
    if (timebase.denom == 0) return -1;
    const __uint128_t nanoseconds =
        static_cast<__uint128_t>(mach_continuous_time()) * timebase.numer
        / timebase.denom;
    const __uint128_t milliseconds = nanoseconds / 1'000'000U;
    if (milliseconds
        > static_cast<__uint128_t>(std::numeric_limits<qint64>::max())) {
        return std::numeric_limits<qint64>::max();
    }
    return static_cast<qint64>(milliseconds);
#elif defined(Q_OS_WIN)
    return static_cast<qint64>(GetTickCount64());
#elif defined(Q_OS_LINUX) && defined(CLOCK_BOOTTIME)
    timespec value{};
    if (clock_gettime(CLOCK_BOOTTIME, &value) != 0) return -1;
    if (value.tv_sec < 0 || value.tv_nsec < 0) return -1;
    constexpr qint64 millisecondsPerSecond = 1000;
    if (value.tv_sec
        > std::numeric_limits<qint64>::max() / millisecondsPerSecond) {
        return std::numeric_limits<qint64>::max();
    }
    return static_cast<qint64>(value.tv_sec) * millisecondsPerSecond
        + static_cast<qint64>(value.tv_nsec / 1'000'000L);
#else
    // Fail-closed fallback for an unsupported platform. Wall-clock rollback is
    // treated as an expired lease by leaseElapsedMs(); a forward jump expires
    // early rather than extending remote control past the server deadline.
    return QDateTime::currentMSecsSinceEpoch();
#endif
}

QByteArray base64UrlDecode(const QString& value) {
    return QByteArray::fromBase64(
        value.toLatin1(),
        QByteArray::Base64UrlEncoding | QByteArray::AbortOnBase64DecodingErrors);
}

QString base64UrlEncode(const QByteArray& value) {
    return QString::fromLatin1(value.toBase64(
        QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals));
}

bool isCanonicalUuid(const QString& value) {
    static const QRegularExpression pattern(QStringLiteral(
        "^[0-9a-f]{8}-[0-9a-f]{4}-[1-5][0-9a-f]{3}-[89ab][0-9a-f]{3}-[0-9a-f]{12}$"),
        QRegularExpression::CaseInsensitiveOption);
    return pattern.match(value).hasMatch();
}

bool isUploadOpaqueId(const QString& value) {
    static const QRegularExpression pattern(
        QStringLiteral("^[A-Za-z0-9_-]{1,128}$"));
    return pattern.match(value).hasMatch();
}

bool isUploadSha256(const QString& value) {
    static const QRegularExpression pattern(QStringLiteral("^[0-9a-f]{64}$"));
    return pattern.match(value).hasMatch();
}

bool isSafeJsonInteger(double value, double minimum, double maximum) {
    return std::isfinite(value) && std::floor(value) == value
        && value >= minimum && value <= maximum;
}

bool readPositiveSafeJsonInteger(const QJsonValue& value, quint64* result) {
    constexpr double maximum = 9007199254740991.0;
    if (!result || !value.isDouble()) return false;
    const double raw = value.toDouble(-1.0);
    if (!isSafeJsonInteger(raw, 1.0, maximum)) return false;
    *result = static_cast<quint64>(raw);
    return true;
}

bool isValidUploadAssetStateArray(const QJsonArray& assets) {
    if (assets.isEmpty() || assets.size() > 256) return false;
    QSet<QString> seen;
    for (const QJsonValue& value : assets) {
        if (!value.isObject()) return false;
        const QJsonObject asset = value.toObject();
        const QString assetId = asset.value(QStringLiteral("assetId")).toString();
        const QString digest = asset.value(QStringLiteral("sha256")).toString();
        const double offset = asset.value(QStringLiteral("offset")).toDouble(-1.0);
        const double size = asset.value(QStringLiteral("size")).toDouble(-1.0);
        if (!isUploadOpaqueId(assetId) || seen.contains(assetId)
            || !isUploadSha256(digest)
            || !isSafeJsonInteger(size, 1.0, 16.0 * 1024 * 1024 * 1024)
            || !isSafeJsonInteger(offset, 0.0, size)) return false;
        seen.insert(assetId);
    }
    return true;
}

bool isValidUploadManifest(const QJsonArray& files) {
    if (files.isEmpty() || files.size() > 256) return false;
    QSet<QString> assetIds;
    QSet<QString> mediaIds;
    double totalSize = 0.0;
    for (const QJsonValue& value : files) {
        if (!value.isObject()) return false;
        const QJsonObject file = value.toObject();
        const QString assetId = file.value(QStringLiteral("assetId")).toString();
        const QString fileId = file.value(QStringLiteral("fileId")).toString();
        const QString digest = file.value(QStringLiteral("sha256")).toString();
        const QString name = file.value(QStringLiteral("name")).toString();
        const QString extension = file.value(QStringLiteral("extension")).toString();
        const double size = file.value(QStringLiteral("size")).toDouble(-1.0);
        const QJsonArray ids = file.value(QStringLiteral("mediaIds")).toArray();
        const int dot = name.lastIndexOf(QLatin1Char('.'));
        const QString filenameExtension = dot > 0 && dot < name.size() - 1
            ? name.mid(dot + 1).toLower() : QString();
        if (!isUploadOpaqueId(assetId) || assetIds.contains(assetId)
            || !isUploadSha256(fileId) || fileId != digest
            || name.isEmpty() || name.size() > 255
            || name.contains(QLatin1Char('/')) || name.contains(QLatin1Char('\\'))
            || extension != extension.toLower()
            || !MediaFormatContract::isCanonicalMediaExtension(extension)
            || filenameExtension != extension
            || !isSafeJsonInteger(size, 1.0, 16.0 * 1024 * 1024 * 1024)
            || ids.isEmpty() || ids.size() > 4096
            || totalSize > 64.0 * 1024 * 1024 * 1024 - size) return false;
        for (const QJsonValue& idValue : ids) {
            const QString mediaId = idValue.toString();
            if (!isUploadOpaqueId(mediaId) || mediaIds.contains(mediaId)) return false;
            mediaIds.insert(mediaId);
        }
        assetIds.insert(assetId);
        totalSize += size;
    }
    return true;
}

qint64 boundedInteger(const QJsonValue& value, qint64 minimum, qint64 maximum) {
    const double raw = value.toDouble(std::numeric_limits<double>::quiet_NaN());
    if (!std::isfinite(raw) || std::floor(raw) != raw
        || raw < static_cast<double>(minimum) || raw > static_cast<double>(maximum)) {
        return -1;
    }
    return static_cast<qint64>(raw);
}

bool isRemoteSessionBusinessError(const QString& code) {
    // These are command-level RemoteSession rejections from
    // RemoteSessionRegistry. They say nothing about transport health and must
    // never feed ConnectionManager through connectionError().
    static const QSet<QString> codes = {
        QStringLiteral("target_offline"),
        QStringLiteral("invalid_remote_session_binding"),
        QStringLiteral("self_target_not_allowed"),
        QStringLiteral("unknown_remote_session"),
        QStringLiteral("not_session_owner"),
        QStringLiteral("not_a_session_party"),
        QStringLiteral("session_terminal"),
        QStringLiteral("session_not_resumable"),
        QStringLiteral("remote_session_not_active"),
        QStringLiteral("remote_session_terminal"),
        QStringLiteral("lease_expired"),
        QStringLiteral("invalid_resume_proof"),
        QStringLiteral("invalid_connection_generation"),
        QStringLiteral("stale_remote_session_generation"),
        QStringLiteral("invalid_teardown"),
        QStringLiteral("invalid_teardown_ack"),
        QStringLiteral("cleanup_not_committed"),
        QStringLiteral("party_not_in_grace"),
        QStringLiteral("resume_required"),
        QStringLiteral("stale_connection_generation"),
    };
    return codes.contains(code);
}

bool containsRemovedWireField(const QJsonValue& value) {
    static const QSet<QString> forbidden = {
        QStringLiteral("clientId"), QStringLiteral("persistentClientId"),
        QStringLiteral("persistentId"), QStringLiteral("sessionId"),
        QStringLiteral("deviceId"),
        QStringLiteral("canvasSessionId"), QStringLiteral("targetClientId"),
        QStringLiteral("targetPersistentClientId"),
        QStringLiteral("senderClientId"),
        QStringLiteral("senderPersistentClientId"),
        QStringLiteral("senderId"), QStringLiteral("targetId")
    };
    if (value.isArray()) {
        for (const QJsonValue& child : value.toArray()) {
            if (containsRemovedWireField(child)) return true;
        }
        return false;
    }
    if (!value.isObject()) return false;
    const QJsonObject object = value.toObject();
    for (auto it = object.begin(); it != object.end(); ++it) {
        if (forbidden.contains(it.key()) || containsRemovedWireField(it.value())) {
            return true;
        }
    }
    return false;
}

bool isRemovedWireType(const QString& type) {
    static const QSet<QString> removed = {
        QStringLiteral("device_register"),
        QStringLiteral("registration_confirmed"),
        QStringLiteral("state_sync"),
        QStringLiteral("request_screens"),
        QStringLiteral("watch_screens"),
        QStringLiteral("unwatch_screens"),
        QStringLiteral("screens_info"),
        QStringLiteral("watch_status"),
        QStringLiteral("data_request"),
        QStringLiteral("cursor_update"),
        QStringLiteral("canvas_created"),
        QStringLiteral("canvas_deleted"),
        QStringLiteral("remove_file"),
        QStringLiteral("remove_all_files"),
        QStringLiteral("all_files_removed"),
        QStringLiteral("removal_rejected")
    };
    return removed.contains(type);
}
}

// ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
// MOUFFETTE PROTOCOL  ENDPOINT IDENTITY
// ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
//
// installationId is SHA-256(SPKI), endpointId adds the application instance,
// runtimeId lives for one process, and connectionGeneration changes for every
// authenticated transport.
// ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━

WebSocketClient::WebSocketClient(QObject *parent)
    : WebSocketClient(QString(), true, parent, {}, QStringLiteral("primary"), 1) {}

WebSocketClient::WebSocketClient(const QString& identityFallbackDirectory,
                                 bool preferNativeIdentityVault,
                                 QObject *parent,
                                 SuspendInclusiveClock suspendInclusiveClock,
                                 QString instanceId,
                                 int instanceOrdinal)
    : QObject(parent)
    , m_identityStore(std::make_unique<DeviceIdentityStore>(
          identityFallbackDirectory, preferNativeIdentityVault))
    , m_sceneRuns(std::make_unique<SceneRunCoordinator>())
    , m_webSocket(nullptr)
    , m_connectionStatus("Disconnected")
    , m_heartbeatTimer(new QTimer(this))
    , m_clockSyncBurstTimer(new QTimer(this))
    , m_leaseHealthTimer(new QTimer(this))
    , m_suspendInclusiveClock(std::move(suspendInclusiveClock))
    , m_runtimeId(QUuid::createUuid().toString(QUuid::WithoutBraces))
    , m_instanceId(std::move(instanceId))
    , m_instanceOrdinal(instanceOrdinal)
{
    if (!m_suspendInclusiveClock) {
        m_suspendInclusiveClock = systemSuspendInclusiveMonotonicMs;
    }
    m_processClock.start();
    m_heartbeatTimer->setSingleShot(false);
    connect(m_heartbeatTimer, &QTimer::timeout, this, &WebSocketClient::sendHeartbeat);
    m_clockSyncBurstTimer->setInterval(kClockSyncBurstIntervalMs);
    m_clockSyncBurstTimer->setSingleShot(false);
    connect(m_clockSyncBurstTimer, &QTimer::timeout, this, [this]() {
        if (!isConnected() || m_clockSyncBurstRemaining <= 0) {
            m_clockSyncBurstTimer->stop();
            m_clockSyncBurstRemaining = 0;
            return;
        }
        sendHeartbeat();
        if (--m_clockSyncBurstRemaining <= 0) {
            m_clockSyncBurstTimer->stop();
        }
    });
    m_leaseHealthTimer->setInterval(
        AppConfig::instance().leaseHealthCheckIntervalMs());
    m_leaseHealthTimer->setSingleShot(false);
    connect(m_leaseHealthTimer, &QTimer::timeout, this, &WebSocketClient::checkLeaseHealth);
    connect(remoteSessionCoordinator(), &RemoteSessionCoordinator::sessionRemoved,
            this, [this](const QString& sessionId) {
        m_receivedCursorSequenceBySession.remove(sessionId);
    });

    if (!m_identityStore->initialize(&m_identityInitializationError)) {
        qCritical().noquote() << "Device identity initialization failed:"
                              << m_identityInitializationError;
    } else {
        m_installationId = m_identityStore->installationId();
        m_endpointId = DeviceIdentityStore::endpointIdForInstallation(
            m_installationId, m_instanceId);
        m_sceneRuns->setLocalEndpointId(m_endpointId);
        qInfo() << "Device identity initialized using"
                << m_identityStore->storageBackendName()
                << "installationId" << m_installationId
                << "endpointId" << m_endpointId
                << "instanceId" << m_instanceId;
    }
    qDebug() << "WebSocketClient: Initialized runtimeId" << m_runtimeId;
}

void WebSocketClient::onUploadTextMessageReceived(const QString& message) {
    QJsonParseError error;
    QJsonDocument doc = QJsonDocument::fromJson(message.toUtf8(), &error);
    if (error.error != QJsonParseError::NoError) {
        qWarning() << "Failed to parse JSON message on upload channel:" << error.errorString();
        closeUploadChannel();
        return;
    }
    if (!doc.isObject()) {
        qWarning() << "Rejected non-object message on upload channel";
        closeUploadChannel();
        return;
    }
    QJsonObject obj = doc.object();
    const QString type = obj.value("type").toString();
    if (type == "upload_channel_ready") {
        quint64 generation = 0;
        const QString readyEndpointId = obj.value("endpointId").toString();
        m_uploadChannelAuthenticated =
            boundedInteger(obj.value(QStringLiteral("protocolVersion")),
                           ProtocolVersion, ProtocolVersion) == ProtocolVersion
            && obj.value(QStringLiteral("serverBootId")).toString()
                == m_serverBootId
            && isCanonicalUuid(
                obj.value(QStringLiteral("messageId")).toString())
            && readPositiveSafeJsonInteger(
                obj.value(QStringLiteral("connectionGeneration")), &generation)
            && !readyEndpointId.isEmpty() && readyEndpointId == m_endpointId
            && generation == m_connectionGeneration;
        if (!m_uploadChannelAuthenticated) {
            qWarning() << "Upload channel envelope does not match the authenticated control connection";
            closeUploadChannel();
            return;
        }
        m_uploadClientId = readyEndpointId;
        qDebug() << "Upload channel authenticated for client:" << m_uploadClientId;
        return;
    }
    if (type == "welcome") {
        // The removed upload-channel welcome is not proof of identity. Keep the
        // dedicated socket unauthenticated and let the upload fall back to the
        // already authenticated control connection.
        qWarning() << "Upload channel server did not provide authenticated readiness";
        closeUploadChannel();
        return;
    }
    if (type == "error" && !m_uploadChannelAuthenticated) {
        qWarning() << "Upload channel authentication failed:"
                   << obj.value("message").toString();
        closeUploadChannel();
        return;
    }
    if (!m_uploadChannelAuthenticated) {
        qWarning() << "Rejected application message on an unauthenticated upload channel";
        closeUploadChannel();
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
    if (!m_identityStore || !m_identityStore->isReady()) {
        const QString error = m_identityInitializationError.isEmpty()
            ? QStringLiteral("Device identity is unavailable")
            : m_identityInitializationError;
        setConnectionStatus(QStringLiteral("Identity error"));
        emit fatalError(error);
        emit connectionError(error);
        return;
    }
    if (serverUrl.trimmed().isEmpty()) {
        emit connectionError(QStringLiteral("Server URL is empty"));
        return;
    }

    closeUploadChannel();
    m_authenticated = false;
    m_endpointDraining = false;
    m_heartbeatTimer->stop();
    m_clockSyncBurstTimer->stop();
    m_clockSyncBurstRemaining = 0;
    m_lastClockSyncBurstStartedAtMs = -1;
    m_pendingServerBootId.clear();
    resetSceneClockEstimate();
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
    
    m_disconnectSignalEmitted = false;
    setConnectionStatus("Connecting...");
    qDebug() << "Connecting to server:" << serverUrl;
    m_webSocket->open(QUrl(serverUrl));
}

void WebSocketClient::disconnect() {
    expireLease();
    m_authenticated = false;
    m_heartbeatTimer->stop();
    m_clockSyncBurstTimer->stop();
    m_clockSyncBurstRemaining = 0;
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

void WebSocketClient::abortConnectionAttempt() {
    m_authenticated = false;
    m_heartbeatTimer->stop();
    if (m_webSocket && m_webSocket->state() != QAbstractSocket::UnconnectedState) {
        m_webSocket->abort();
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
    return m_authenticated && isTransportConnected();
}

bool WebSocketClient::isTransportConnected() const {
    return m_webSocket && m_webSocket->state() == QAbstractSocket::ConnectedState;
}

bool WebSocketClient::hasUnexpiredLease() const {
    return m_leaseTimeoutMs > 0 && m_hasEstablishedLease && !m_leaseExpired
        && m_lastServerContactContinuousMs >= 0
        && leaseElapsedMs() < m_leaseTimeoutMs;
}

qint64 WebSocketClient::leaseRemainingMs() const {
    if (m_leaseTimeoutMs <= 0 || !m_hasEstablishedLease || m_leaseExpired
        || m_lastServerContactContinuousMs < 0) {
        return 0;
    }
    return std::max<qint64>(0, static_cast<qint64>(m_leaseTimeoutMs)
                                  - leaseElapsedMs());
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
        if (!isUploadSessionTransportAvailable()) return false;
        ++m_uploadSessionRefCount;
        return true;
    }
    if (!isConnected()) return false;

    m_uploadSessionActive = true;
    m_uploadSessionRefCount = 1;
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
    if (m_uploadSessionRefCount > 1) {
        --m_uploadSessionRefCount;
        return;
    }
    m_uploadSessionRefCount = 0;
    m_uploadSessionActive = false;
    m_useUploadSocketForSession = false;
    m_uploadTransportLossReported = false;
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
    
    if (machineName.trimmed().isEmpty() || platform.trimmed().isEmpty()) {
        qWarning() << "Cannot publish an incomplete device snapshot";
        return;
    }
    m_registeredMachineName = machineName;
    m_registeredPlatform = platform;
    QJsonObject message;
    message["type"] = "endpoint_snapshot";
    message["machineName"] = machineName;
    message["platform"] = platform;
    message["instanceOrdinal"] = m_instanceOrdinal;
    message["volumePercent"] = volumePercent >= 0 && volumePercent <= 100
        ? QJsonValue(volumePercent) : QJsonValue(QJsonValue::Null);

    QJsonArray screensArray;
    for (const auto& screen : screens) {
        screensArray.append(screen.toJson());
    }
    message["screens"] = screensArray;
    message["systemUI"] = QJsonArray();

    m_registeredTargetSnapshot = QJsonObject{
        {QStringLiteral("screens"), screensArray},
        {QStringLiteral("systemUI"), QJsonArray()},
        {QStringLiteral("volumePercent"), message.value(QStringLiteral("volumePercent"))},
        {QStringLiteral("revision"), static_cast<double>(++m_targetSnapshotRevision)},
        {QStringLiteral("capturedAtEpochMs"),
         static_cast<double>(QDateTime::currentMSecsSinceEpoch())}
    };
    
    sendMessage(message);
    qDebug() << "Registering device:" << machineName << "(" << platform
             << ") endpointId:" << m_endpointId << "runtimeId:" << m_runtimeId;
}

bool WebSocketClient::sendUploadStart(const QString& remoteSessionId,
                                      quint64 generation,
                                      const QString& uploadId,
                                      const QJsonArray& filesManifest) {
    if (!m_uploadSessionActive || !m_sceneRuns || !isUploadOpaqueId(uploadId)
        || !isValidUploadManifest(filesManifest)) return false;
    const SceneRunCoordinator::SessionBinding binding =
        m_sceneRuns->sessionById(remoteSessionId);
    if (!binding.active || binding.generation != generation
        || binding.ownerEndpointId != m_endpointId) return false;
    m_canceledUploads.remove(uploadId);

    QJsonObject msg{
        {QStringLiteral("type"), QStringLiteral("upload_start")},
        {QStringLiteral("remoteSessionId"), remoteSessionId},
        {QStringLiteral("generation"), static_cast<double>(generation)},
        {QStringLiteral("uploadId"), uploadId},
        {QStringLiteral("files"), filesManifest}
    };
    return sendMessageUpload(msg);
}

bool WebSocketClient::sendUploadResume(const QString& remoteSessionId,
                                       quint64 generation,
                                       const QString& uploadId) {
    if (!m_uploadSessionActive || !m_sceneRuns || !isUploadOpaqueId(uploadId)) return false;
    const SceneRunCoordinator::SessionBinding binding =
        m_sceneRuns->sessionById(remoteSessionId);
    if (!binding.active || binding.generation != generation
        || binding.ownerEndpointId != m_endpointId) return false;
    return sendMessageUpload(QJsonObject{
        {QStringLiteral("type"), QStringLiteral("upload_resume")},
        {QStringLiteral("remoteSessionId"), remoteSessionId},
        {QStringLiteral("generation"), static_cast<double>(generation)},
        {QStringLiteral("uploadId"), uploadId}
    });
}

bool WebSocketClient::sendUploadChunk(const QString& remoteSessionId,
                                      quint64 generation,
                                      const QString& uploadId,
                                      const QString& assetId,
                                      qint64 offset,
                                      const QString& sha256,
                                      const QByteArray& data) {
    if (!m_uploadSessionActive || m_canceledUploads.contains(uploadId)
        || !m_sceneRuns || !isUploadOpaqueId(uploadId) || !isUploadOpaqueId(assetId)
        || offset < 0 || !isUploadSha256(sha256)
        || data.isEmpty() || data.size() > 128 * 1024) return false;
    const SceneRunCoordinator::SessionBinding binding =
        m_sceneRuns->sessionById(remoteSessionId);
    if (!binding.active || binding.generation != generation
        || binding.ownerEndpointId != m_endpointId) return false;
    const QByteArray encoded = data.toBase64();
    // QByteArray::toBase64 emits the canonical RFC 4648 padded alphabet which
    // the server verifies by decode/re-encode equality.
    QJsonObject msg{
        {QStringLiteral("type"), QStringLiteral("upload_chunk")},
        {QStringLiteral("remoteSessionId"), remoteSessionId},
        {QStringLiteral("generation"), static_cast<double>(generation)},
        {QStringLiteral("uploadId"), uploadId},
        {QStringLiteral("assetId"), assetId},
        {QStringLiteral("offset"), static_cast<double>(offset)},
        {QStringLiteral("size"), data.size()},
        {QStringLiteral("sha256"), sha256},
        {QStringLiteral("data"), QString::fromLatin1(encoded)}
    };
    return sendMessageUpload(msg);
}

bool WebSocketClient::sendUploadComplete(const QString& remoteSessionId,
                                         quint64 generation,
                                         const QString& uploadId,
                                         const QJsonArray& assets) {
    if (!m_uploadSessionActive || m_canceledUploads.contains(uploadId)
        || !m_sceneRuns || !isUploadOpaqueId(uploadId)
        || !isValidUploadAssetStateArray(assets)) return false;
    const SceneRunCoordinator::SessionBinding binding =
        m_sceneRuns->sessionById(remoteSessionId);
    if (!binding.active || binding.generation != generation
        || binding.ownerEndpointId != m_endpointId) return false;
    QJsonObject msg{
        {QStringLiteral("type"), QStringLiteral("upload_complete")},
        {QStringLiteral("remoteSessionId"), remoteSessionId},
        {QStringLiteral("generation"), static_cast<double>(generation)},
        {QStringLiteral("uploadId"), uploadId},
        {QStringLiteral("assets"), assets}
    };
    return sendMessageUpload(msg);
}

bool WebSocketClient::sendUploadAbort(const QString& remoteSessionId,
                                      quint64 generation,
                                      const QString& uploadId,
                                      const QString& reason) {
    if (!(isConnected() || isUploadChannelConnected())) return false;
    if (!m_sceneRuns || !isUploadOpaqueId(uploadId)) return false;
    const SceneRunCoordinator::SessionBinding binding =
        m_sceneRuns->sessionById(remoteSessionId);
    if (binding.remoteSessionId.isEmpty() || binding.generation != generation
        || binding.ownerEndpointId != m_endpointId) return false;
    m_canceledUploads.insert(uploadId);

    QJsonObject msg{
        {QStringLiteral("type"), QStringLiteral("upload_abort")},
        {QStringLiteral("remoteSessionId"), remoteSessionId},
        {QStringLiteral("generation"), static_cast<double>(generation)},
        {QStringLiteral("uploadId"), uploadId}
    };
    if (!reason.isEmpty()) msg["reason"] = reason.left(128);
    // Preserve START/CHUNK/ABORT ordering on the pinned socket whenever it is
    // still usable. The authenticated control connection is only the emergency
    // path after a dedicated payload transport has already failed.
    if (m_uploadSessionActive && isUploadSessionTransportAvailable()) {
        return sendMessageUpload(msg);
    }
    return sendControlMessage(msg);
}

bool WebSocketClient::sendUploadRemove(const QString& remoteSessionId,
                                       quint64 generation,
                                       const QString& removalId,
                                       const QString& uploadId,
                                       const QString& assetId,
                                       qint64 size,
                                       const QString& sha256,
                                       const QString& reason) {
    if (!isConnected() || !m_sceneRuns || !isCanonicalUuid(removalId)
        || !isUploadOpaqueId(uploadId) || !isUploadOpaqueId(assetId)
        || size < 1 || size > 16LL * 1024 * 1024 * 1024
        || !isUploadSha256(sha256)) {
        return false;
    }
    const SceneRunCoordinator::SessionBinding binding =
        m_sceneRuns->sessionById(remoteSessionId);
    if (!binding.active || binding.generation != generation
        || binding.ownerEndpointId != m_endpointId) {
        return false;
    }
    QJsonObject message{
        {QStringLiteral("type"), QStringLiteral("upload_remove")},
        {QStringLiteral("remoteSessionId"), remoteSessionId},
        {QStringLiteral("generation"), static_cast<double>(generation)},
        {QStringLiteral("removalId"), removalId.toLower()},
        {QStringLiteral("uploadId"), uploadId},
        {QStringLiteral("assetId"), assetId},
        {QStringLiteral("offset"), static_cast<double>(size)},
        {QStringLiteral("size"), static_cast<double>(size)},
        {QStringLiteral("sha256"), sha256}
    };
    if (!reason.trimmed().isEmpty()) {
        message.insert(QStringLiteral("reason"), reason.trimmed().left(128));
    }
    return sendControlMessage(message);
}

bool WebSocketClient::sendMediaResidency(const QString& sessionId, quint64 generation,
                                        quint64 sequence, const QJsonArray& assets)
{
    if (!isConnected() || !m_sceneRuns || sequence == 0) return false;
    const auto binding = m_sceneRuns->sessionById(sessionId);
    if (!binding.active || binding.generation != generation
        || binding.targetEndpointId != m_endpointId) return false;
    return sendControlMessage({{QStringLiteral("type"), QStringLiteral("media_residency")},
        {QStringLiteral("remoteSessionId"), sessionId},
        {QStringLiteral("generation"), static_cast<double>(generation)},
        {QStringLiteral("sequence"), static_cast<double>(sequence)},
        {QStringLiteral("assets"), assets}});
}

bool WebSocketClient::sendUploadProtocolResponse(const QJsonObject& response) {
    static const QSet<QString> allowedTypes = {
        QStringLiteral("upload_ready"), QStringLiteral("upload_progress"),
        QStringLiteral("upload_finished"), QStringLiteral("upload_rejected"),
        QStringLiteral("upload_abort_ack"), QStringLiteral("upload_removed")
    };
    QJsonObject message = response;
    const QString type = message.value(QStringLiteral("type")).toString();
    const QString remoteSessionId =
        message.value(QStringLiteral("remoteSessionId")).toString();
    const QString uploadId = message.value(QStringLiteral("uploadId")).toString();
    quint64 generation = 0;
    if (!isConnected() || !m_sceneRuns || !allowedTypes.contains(type)
        || !isUploadOpaqueId(uploadId)
        || !readPositiveSafeJsonInteger(
            message.value(QStringLiteral("generation")), &generation)) {
        return false;
    }
    const SceneRunCoordinator::SessionBinding binding =
        m_sceneRuns->sessionById(remoteSessionId);
    if (binding.remoteSessionId.isEmpty() || binding.generation != generation
        || binding.targetEndpointId != m_endpointId) return false;
    message.remove(QStringLiteral("ownerEndpointId"));
    message.remove(QStringLiteral("targetEndpointId"));
    return sendControlMessage(message);
}

bool WebSocketClient::openRemoteSession(const QString& targetEndpointId,
                                        QString* requestId)
{
    if (!isConnected() || targetEndpointId.isEmpty() || targetEndpointId == m_endpointId) return false;
    const QString correlationId = QUuid::createUuid().toString(QUuid::WithoutBraces);
    if (!replayRemoteSessionOpen(targetEndpointId, correlationId)) return false;
    if (requestId) *requestId = correlationId;
    return true;
}

bool WebSocketClient::replayRemoteSessionOpen(
    const QString& targetEndpointId,
    const QString& requestId)
{
    if (!isConnected() || targetEndpointId.isEmpty()
        || targetEndpointId == m_endpointId
        || !isCanonicalUuid(requestId)) {
        return false;
    }
    QJsonObject message{
        {QStringLiteral("type"), QStringLiteral("remote_session_open")},
        {QStringLiteral("targetEndpointId"), targetEndpointId},
        {QStringLiteral("requestId"), requestId}
    };
    return sendControlMessage(message);
}

bool WebSocketClient::acceptRemoteSessionOffer(const QJsonObject& offer)
{
    if (!isConnected() || !m_sceneRuns || m_registeredTargetSnapshot.isEmpty()) {
        return false;
    }
    const QString remoteSessionId =
        offer.value(QStringLiteral("remoteSessionId")).toString();
    const SceneRunCoordinator::SessionBinding binding =
        m_sceneRuns->sessionById(remoteSessionId);
    if (binding.remoteSessionId.isEmpty()
        || binding.targetEndpointId != m_endpointId
        || binding.phase != QLatin1String("Opening")) {
        return false;
    }

    QJsonObject snapshot = m_registeredTargetSnapshot;
    snapshot.insert(QStringLiteral("revision"),
                    static_cast<double>(++m_targetSnapshotRevision));
    snapshot.insert(QStringLiteral("capturedAtEpochMs"),
                    static_cast<double>(QDateTime::currentMSecsSinceEpoch()));
    return sendControlMessage(QJsonObject{
        {QStringLiteral("type"), QStringLiteral("remote_session_accept")},
        {QStringLiteral("remoteSessionId"), binding.remoteSessionId},
        {QStringLiteral("generation"), static_cast<double>(binding.generation)},
        {QStringLiteral("snapshot"), snapshot}
    });
}

bool WebSocketClient::sendRemoteSessionSnapshot(
    const QString& remoteSessionId,
    quint64 generation,
    const QJsonObject& targetSnapshot)
{
    if (!isConnected() || !m_sceneRuns || targetSnapshot.isEmpty()) return false;
    const SceneRunCoordinator::SessionBinding binding =
        m_sceneRuns->sessionById(remoteSessionId);
    if (!binding.active || binding.generation != generation
        || binding.targetEndpointId != m_endpointId) return false;
    quint64& sequence = m_targetSnapshotSequenceBySession[remoteSessionId];
    sequence = qMax<quint64>(sequence + 1, 2);
    QJsonObject snapshot = targetSnapshot;
    snapshot.insert(QStringLiteral("revision"),
                    static_cast<double>(++m_targetSnapshotRevision));
    snapshot.insert(QStringLiteral("capturedAtEpochMs"),
                    static_cast<double>(QDateTime::currentMSecsSinceEpoch()));
    return sendControlMessage(QJsonObject{
        {QStringLiteral("type"), QStringLiteral("remote_session_snapshot")},
        {QStringLiteral("remoteSessionId"), remoteSessionId},
        {QStringLiteral("generation"), static_cast<double>(generation)},
        {QStringLiteral("snapshotSequence"), static_cast<double>(sequence)},
        {QStringLiteral("snapshot"), snapshot}
    });
}

bool WebSocketClient::sendRemoteCursor(const QString& remoteSessionId,
                                      quint64 generation, quint64 sequence,
                                      bool visible, int screenId,
                                      const QPointF& screenPosition)
{
    if (!isConnected() || !m_sceneRuns || !m_webSocket || m_endpointDraining
        || sequence == 0 || sequence > 9007199254740991ULL
        || m_webSocket->bytesToWrite() > 64 * 1024) return false;
    const auto binding = m_sceneRuns->sessionById(remoteSessionId);
    if (!binding.active || binding.generation != generation
        || binding.targetEndpointId != m_endpointId
        || binding.targetConnectionGeneration != m_connectionGeneration) return false;
    if (visible && (screenId < 0 || screenId > 1000000
        || !std::isfinite(screenPosition.x()) || !std::isfinite(screenPosition.y())
        || screenPosition.x() < 0 || screenPosition.x() >= 100000
        || screenPosition.y() < 0 || screenPosition.y() >= 100000)) return false;
    // Cursor samples are disposable: do not grow a transport backlog while an
    // upload or a slow connection is draining. The next tick sends fresh state.
    return sendControlMessage(QJsonObject{
        {QStringLiteral("type"), QStringLiteral("remote_session_cursor")},
        {QStringLiteral("remoteSessionId"), remoteSessionId},
        {QStringLiteral("generation"), static_cast<double>(generation)},
        {QStringLiteral("sequence"), static_cast<double>(sequence)},
        {QStringLiteral("visible"), visible},
        {QStringLiteral("screenId"), visible ? screenId : -1},
        {QStringLiteral("x"), visible ? static_cast<int>(std::floor(screenPosition.x())) : 0},
        {QStringLiteral("y"), visible ? static_cast<int>(std::floor(screenPosition.y())) : 0}
    });
}

bool WebSocketClient::resumeRemoteSession(const QString& remoteSessionId)
{
    if (!isConnected() || !m_sceneRuns) return false;
    const SceneRunCoordinator::SessionBinding binding =
        m_sceneRuns->sessionById(remoteSessionId);
    if (binding.remoteSessionId.isEmpty() || binding.resumeToken.isEmpty()
        || binding.generation == 0) return false;
    QJsonObject message{
        {QStringLiteral("type"), QStringLiteral("remote_session_resume")},
        {QStringLiteral("remoteSessionId"), binding.remoteSessionId},
        {QStringLiteral("generation"), static_cast<double>(binding.generation)},
        {QStringLiteral("resumeToken"), binding.resumeToken}
    };
    return sendControlMessage(message);
}

void WebSocketClient::resumeAllRemoteSessions()
{
    if (!m_sceneRuns) return;
    // A client can have several outgoing and incoming sessions. Do not derive
    // this list from discovery: an offline project may still own
    // a resumable session during the strict three-second grace window.
    const QList<SceneRunCoordinator::SessionBinding> bindings = m_sceneRuns->sessions();
    for (const SceneRunCoordinator::SessionBinding& binding : bindings) {
        if (!binding.resumeToken.isEmpty()) resumeRemoteSession(binding.remoteSessionId);
    }
}

bool WebSocketClient::beginEndpointDisable()
{
    if (!isConnected() || m_endpointDraining) return false;
    if (!sendControlMessage(QJsonObject{
            {QStringLiteral("type"), QStringLiteral("endpoint_disable")}
        })) {
        return false;
    }
    m_endpointDraining = true;
    return true;
}

bool WebSocketClient::closeRemoteSession(const QString& remoteSessionId,
                                         QString* requestId,
                                         const QString& reason)
{
    if (!isConnected() || !m_sceneRuns) return false;
    const SceneRunCoordinator::SessionBinding binding =
        m_sceneRuns->sessionById(remoteSessionId);
    if (binding.remoteSessionId.isEmpty()
        || (binding.ownerEndpointId != m_endpointId
            && binding.targetEndpointId != m_endpointId)) return false;
    return closeRemoteSessionByIdentity(
        binding.remoteSessionId, binding.generation, requestId, reason);
}

bool WebSocketClient::closeRemoteSessionByIdentity(
    const QString& remoteSessionId,
    quint64 generation,
    QString* requestId,
    const QString& reason)
{
    if (!isConnected() || !isUploadOpaqueId(remoteSessionId)
        || generation < 1 || generation > 9007199254740991ULL) {
        return false;
    }
    const QString correlationId =
        QUuid::createUuid().toString(QUuid::WithoutBraces);
    QJsonObject message{
        {QStringLiteral("type"), QStringLiteral("remote_session_close")},
        {QStringLiteral("remoteSessionId"), remoteSessionId},
        {QStringLiteral("generation"), static_cast<double>(generation)},
        {QStringLiteral("requestId"), correlationId},
        {QStringLiteral("reason"), reason.left(128)}
    };
    if (!sendControlMessage(message)) return false;
    if (requestId) *requestId = correlationId;
    return true;
}

bool WebSocketClient::discardRemoteSessionAfterAuthoritativeRejection(
    const QString& remoteSessionId)
{
    if (!m_sceneRuns) return false;
    const bool removed =
        m_sceneRuns->discardSessionAfterAuthoritativeRejection(
            remoteSessionId);
    if (removed) {
        m_targetSnapshotSequenceBySession.remove(remoteSessionId);
    }
    return removed;
}

bool WebSocketClient::acknowledgeRemoteSessionTeardown(
    const QString& remoteSessionId,
    const QString& teardownId,
    bool sceneStopped,
    bool uploadsAborted,
    bool cacheQuarantined,
    int removedFileCount,
    const QString& errorCode,
    qint64 quarantinedBytes)
{
    if (!isConnected() || !m_sceneRuns || !isUploadOpaqueId(teardownId)) {
        return false;
    }
    const SceneRunCoordinator::SessionBinding binding =
        m_sceneRuns->sessionById(remoteSessionId);
    if (binding.remoteSessionId.isEmpty()
        || binding.targetEndpointId != m_endpointId
        || (binding.phase != QLatin1String("Terminating")
            && binding.phase != QLatin1String("CleanupPending"))
        || binding.teardownId != teardownId
        || binding.targetConnectionGeneration != m_connectionGeneration) {
        return false;
    }
    const bool committed = sceneStopped && uploadsAborted && cacheQuarantined;
    QJsonObject message{
        {QStringLiteral("type"), QStringLiteral("remote_session_teardown_ack")},
        {QStringLiteral("remoteSessionId"), binding.remoteSessionId},
        {QStringLiteral("generation"), static_cast<double>(binding.generation)},
        {QStringLiteral("teardownId"), teardownId},
        {QStringLiteral("result"), committed ? QStringLiteral("committed")
                                              : QStringLiteral("cleanup_error")},
        {QStringLiteral("sceneStopped"), sceneStopped},
        {QStringLiteral("uploadsAborted"), uploadsAborted},
        {QStringLiteral("cacheQuarantined"), cacheQuarantined},
        {QStringLiteral("removedFileCount"), std::max(0, removedFileCount)},
        {QStringLiteral("quarantinedBytes"),
         static_cast<double>(std::max<qint64>(0, quarantinedBytes))}
    };
    if (!committed) {
        message.insert(QStringLiteral("errorCode"), errorCode.isEmpty()
                           ? QStringLiteral("cleanup_error") : errorCode.left(128));
    }
    return sendControlMessage(message);
}

QJsonObject WebSocketClient::sceneMessage(const QString& sceneRunId,
                                          const QString& type) const
{
    if (!m_sceneRuns) return {};
    const SceneRunCoordinator::Run run = m_sceneRuns->run(sceneRunId);
    if (run.sceneRunId.isEmpty()) return {};
    return QJsonObject{
        {QStringLiteral("type"), type},
        {QStringLiteral("remoteSessionId"), run.remoteSessionId},
        {QStringLiteral("generation"), static_cast<double>(run.generation)},
        {QStringLiteral("sceneRunId"), run.sceneRunId},
        {QStringLiteral("revision"), static_cast<double>(run.revision)},
        {QStringLiteral("digest"), run.digest}
    };
}

bool WebSocketClient::sendScenePrepare(const QString& targetEndpointId,
                                       quint64 revision,
                                       const QJsonArray& manifest,
                                       const QJsonObject& scene,
                                       QString* sceneRunId,
                                       QString* digest,
                                       QString* errorMessage)
{
    if (!isConnected() || !m_sceneRuns) {
        if (errorMessage) *errorMessage = QStringLiteral("Server connection is not active");
        return false;
    }
    SceneRunCoordinator::Run run;
    if (!m_sceneRuns->createOutgoingRun(targetEndpointId, revision, manifest, scene,
                                        &run, errorMessage)) {
        return false;
    }
    QJsonObject message = sceneMessage(run.sceneRunId, QStringLiteral("scene_prepare"));
    message.insert(QStringLiteral("manifest"), run.manifest);
    message.insert(QStringLiteral("scene"), run.scene);
    if (!sendControlMessage(message)) {
        m_sceneRuns->finishRun(run.sceneRunId, true);
        if (errorMessage) *errorMessage = QStringLiteral("Could not send scene preparation");
        return false;
    }
    if (sceneRunId) *sceneRunId = run.sceneRunId;
    if (digest) *digest = run.digest;
    return true;
}

bool WebSocketClient::sendScenePrepareProgress(const QString& sceneRunId,
                                               int percent,
                                               const QJsonArray& checklist)
{
    QJsonObject message = sceneMessage(sceneRunId, QStringLiteral("prepare_progress"));
    if (message.isEmpty() || percent < 0 || percent > 100 || checklist.isEmpty()) return false;
    message.insert(QStringLiteral("percent"), percent);
    message.insert(QStringLiteral("checklist"), checklist);
    return sendControlMessage(message);
}

bool WebSocketClient::sendScenePrepared(const QString& sceneRunId,
                                        bool success,
                                        const QJsonArray& checklist,
                                        const QString& errorCode,
                                        const QString& detail)
{
    QJsonObject message = sceneMessage(sceneRunId, QStringLiteral("prepared"));
    if (message.isEmpty()) return false;
    message.insert(QStringLiteral("success"), success);
    message.insert(QStringLiteral("checklist"), checklist);
    if (!success) {
        message.insert(QStringLiteral("errorCode"), errorCode.isEmpty()
                           ? QStringLiteral("scene_prepare_failed") : errorCode.left(128));
        if (!detail.isEmpty()) message.insert(QStringLiteral("message"), detail.left(512));
    }
    return sendControlMessage(message);
}

bool WebSocketClient::sendSceneArmed(const QString& sceneRunId,
                                     qint64 clockUncertaintyMs)
{
    QJsonObject message = sceneMessage(sceneRunId, QStringLiteral("armed"));
    if (message.isEmpty() || clockUncertaintyMs < 0) return false;
    message.insert(QStringLiteral("clockUncertaintyMs"), static_cast<double>(clockUncertaintyMs));
    return sendControlMessage(message);
}

bool WebSocketClient::sendSceneStarted(const QString& sceneRunId,
                                       bool firstFramePresented,
                                       qint64 presentedServerMonotonicMs)
{
    QJsonObject message = sceneMessage(sceneRunId, QStringLiteral("started"));
    if (message.isEmpty() || presentedServerMonotonicMs < 0) return false;
    message.insert(QStringLiteral("firstFramePresented"), firstFramePresented);
    message.insert(QStringLiteral("presentedServerMonotonicMs"),
                   static_cast<double>(presentedServerMonotonicMs));
    return sendControlMessage(message);
}

bool WebSocketClient::sendSceneStateSnapshot(const QString& sceneRunId,
                                             quint64 sequence,
                                             qint64 sampledServerMonotonicMs,
                                             const QJsonObject& snapshot)
{
    QJsonObject message = sceneMessage(sceneRunId, QStringLiteral("state_snapshot"));
    if (message.isEmpty() || sequence < 1 || sampledServerMonotonicMs < 0
        || snapshot.isEmpty()) return false;
    message.insert(QStringLiteral("sequence"), static_cast<double>(sequence));
    message.insert(QStringLiteral("sampledServerMonotonicMs"),
                   static_cast<double>(sampledServerMonotonicMs));
    message.insert(QStringLiteral("snapshot"), snapshot);
    return sendControlMessage(message);
}

bool WebSocketClient::sendSceneStop(const QString& sceneRunId,
                                    const QString& reason)
{
    QJsonObject message = sceneMessage(sceneRunId, QStringLiteral("stop"));
    if (message.isEmpty()) return false;
    message.insert(QStringLiteral("reason"), reason.left(128));
    return sendControlMessage(message);
}

bool WebSocketClient::sendSceneStopped(const QString& sceneRunId,
                                       bool success,
                                       const QString& detail)
{
    QJsonObject message = sceneMessage(sceneRunId, QStringLiteral("stopped"));
    if (message.isEmpty()) return false;
    message.insert(QStringLiteral("success"), success);
    if (!detail.isEmpty()) message.insert(QStringLiteral("message"), detail.left(512));
    return sendControlMessage(message);
}

bool WebSocketClient::hasFreshSceneClockSample() const
{
    if (!m_processClock.isValid() || m_leaseTimeoutMs <= 0
        || m_selectedClockSampleReceivedAtMs < 0) {
        return false;
    }
    const qint64 now = m_processClock.elapsed();
    return now >= m_selectedClockSampleReceivedAtMs
        && now - m_selectedClockSampleReceivedAtMs < m_leaseTimeoutMs;
}

void WebSocketClient::resetSceneClockEstimate()
{
    m_heartbeatSentAt.clear();
    m_clockSamples.clear();
    m_serverMonotonicOffsetMs = 0;
    m_clockUncertaintyMs = std::numeric_limits<qint64>::max();
    m_selectedClockSampleReceivedAtMs = -1;
}

qint64 WebSocketClient::sceneClockUncertaintyMs() const
{
    return hasFreshSceneClockSample()
        ? m_clockUncertaintyMs : std::numeric_limits<qint64>::max();
}

qint64 WebSocketClient::estimatedServerMonotonicMs() const
{
    return hasFreshSceneClockSample()
        ? m_processClock.elapsed() + m_serverMonotonicOffsetMs : -1;
}

bool WebSocketClient::requestSceneClockSynchronization()
{
    if (!isConnected() || !m_processClock.isValid()) return false;
    if (m_clockSyncBurstTimer->isActive()) return true;

    const qint64 now = m_processClock.elapsed();
    if (m_lastClockSyncBurstStartedAtMs >= 0
        && now - m_lastClockSyncBurstStartedAtMs < kClockSyncBurstCooldownMs) {
        return true;
    }

    m_lastClockSyncBurstStartedAtMs = now;
    m_clockSyncBurstRemaining = kClockSyncBurstProbeCount - 1;
    sendHeartbeat();
    if (m_clockSyncBurstRemaining > 0 && isConnected()) {
        m_clockSyncBurstTimer->start();
    }
    return true;
}

RemoteSessionCoordinator* WebSocketClient::remoteSessionCoordinator() const
{
    return m_sceneRuns ? m_sceneRuns->remoteSessions() : nullptr;
}

void WebSocketClient::onConnected() {
    qDebug() << "Control transport connected; waiting for signed authentication challenge";
    m_authenticated = false;
    resetSceneClockEstimate();
    setConnectionStatus("Authenticating...");
    emit transportConnected();
}

void WebSocketClient::onDisconnected() {
    qDebug() << "Control transport disconnected";
    closeUploadChannel();
    m_authenticated = false;
    m_heartbeatTimer->stop();
    m_clockSyncBurstTimer->stop();
    m_clockSyncBurstRemaining = 0;
    resetSceneClockEstimate();
    setConnectionStatus("Disconnected");
    if (!m_disconnectSignalEmitted) {
        m_disconnectSignalEmitted = true;
        emit disconnected();
    }
}

void WebSocketClient::onTextMessageReceived(const QString& message) {
    QJsonParseError error;
    QJsonDocument doc = QJsonDocument::fromJson(message.toUtf8(), &error);
    
    if (error.error != QJsonParseError::NoError) {
        qWarning() << "Failed to parse JSON message:" << error.errorString();
        return;
    }
    
    if (!doc.isObject()) {
        emit fatalError(QStringLiteral("Server sent a non-object protocol message"));
        abortConnectionAttempt();
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
        case QAbstractSocket::NetworkError:
            errorString = "Network error";
            break;
        case QAbstractSocket::SslHandshakeFailedError:
            errorString = "SSL handshake failed";
            emit fatalError("SSL/TLS error - check certificates");
            break;
        default:
            errorString = QString("Socket error: %1").arg(error);
    }

    qWarning() << "WebSocket error:" << errorString;
    setConnectionStatus("Error: " + errorString);
    emit connectionError(errorString);
}

void WebSocketClient::sendHeartbeat() {
    // Timer delivery can be the first event processed after system wake. Apply
    // the suspend-inclusive terminal deadline before writing anything.
    checkLeaseHealth();
    if (!isConnected()) return;
    const quint64 sequence = ++m_heartbeatSequence;
    const qint64 now = m_processClock.elapsed();
    m_heartbeatSentAt.insert(sequence, now);
    while (m_heartbeatSentAt.size() > 16) {
        auto oldest = m_heartbeatSentAt.begin();
        for (auto it = m_heartbeatSentAt.begin(); it != m_heartbeatSentAt.end(); ++it) {
            if (it.value() < oldest.value()) oldest = it;
        }
        m_heartbeatSentAt.erase(oldest);
    }
    QJsonObject heartbeat;
    heartbeat["type"] = "heartbeat";
    heartbeat["sequence"] = static_cast<double>(sequence);
    heartbeat["clientMonotonicMs"] = static_cast<double>(now);
    sendMessage(heartbeat);
}

void WebSocketClient::checkLeaseHealth() {
    if (m_leaseTimeoutMs <= 0 || !m_hasEstablishedLease || m_leaseExpired) {
        return;
    }
    if (m_lastServerContactContinuousMs < 0) {
        expireLease();
        if (isTransportConnected()) {
            setConnectionStatus("Connection lease clock unavailable");
            abortConnectionAttempt();
        }
        return;
    }
    const qint64 elapsed = leaseElapsedMs();
    if (elapsed >= m_leaseTimeoutMs) {
        expireLease();
        if (isTransportConnected()) {
            setConnectionStatus("Connection lease expired");
            abortConnectionAttempt();
        }
        return;
    }
    const bool degraded = elapsed >= std::max(1, m_leaseTimeoutMs / 2);
    if (m_degraded != degraded) {
        m_degraded = degraded;
        emit transportHealthChanged(degraded);
        if (degraded && isConnected()) {
            setConnectionStatus("Degraded");
        } else if (!degraded && isConnected()) {
            setConnectionStatus("Connected");
        }
    }
}

bool WebSocketClient::handleAuthChallenge(const QJsonObject& message) {
    if (m_authenticated || !m_pendingServerBootId.isEmpty()
        || !isTransportConnected()) return false;
    if (boundedInteger(message.value(QStringLiteral("protocolVersion")),
                       ProtocolVersion, ProtocolVersion) != ProtocolVersion) {
        emit fatalError(QStringLiteral("Server requires an incompatible protocol version"));
        abortConnectionAttempt();
        return false;
    }

    const QString serverBootId = message.value("serverBootId").toString();
    const QString nonce = message.value("nonce").toString();
    if (!isCanonicalUuid(
            message.value(QStringLiteral("messageId")).toString())
        || !isCanonicalUuid(serverBootId)
        || base64UrlDecode(nonce).size() != 32
        || boundedInteger(message.value(QStringLiteral("issuedAt")), 0,
                          9007199254740991LL) < 0) {
        emit fatalError(QStringLiteral("Server sent an invalid authentication challenge"));
        abortConnectionAttempt();
        return false;
    }

    const QByteArray payload = QStringLiteral("mouffette-v%1\n%2\n%3\n%4\n%5")
        .arg(ProtocolVersion)
        .arg(serverBootId, nonce, m_runtimeId, m_instanceId)
        .toUtf8();
    QString signatureError;
    const QByteArray signature = m_identityStore->sign(payload, &signatureError);
    if (signature.size() != 64) {
        emit fatalError(signatureError.isEmpty()
                            ? QStringLiteral("Cannot sign authentication challenge")
                            : signatureError);
        abortConnectionAttempt();
        return false;
    }

    m_pendingServerBootId = serverBootId;
    QJsonObject response;
    response["type"] = "auth_response";
    response["protocolVersion"] = ProtocolVersion;
    response["serverBootId"] = serverBootId;
    response["messageId"] = QUuid::createUuid().toString(QUuid::WithoutBraces);
    response["runtimeId"] = m_runtimeId;
    response["instanceId"] = m_instanceId;
    response["installationId"] = m_installationId;
    response["publicKey"] = base64UrlEncode(m_identityStore->publicKeyDer());
    response["signature"] = base64UrlEncode(signature);
    return sendRawControlMessage(response);
}

bool WebSocketClient::validateServerPolicy(const QJsonObject& policy,
                                           QString* errorMessage) const {
    struct Rule { const char* name; qint64 minimum; qint64 maximum; };
    static constexpr Rule rules[] = {
        {"policyVersion", 1, 1000000},
        {"heartbeatIntervalMs", 250, 5000},
        {"leaseTimeoutMs", 1000, 30000},
        {"scenePrepareTimeoutMs", 1000, 120000},
        {"sceneActivationLeadMs", 500, 10000},
        {"sceneMaxClockSkewMs", 0, 250},
        {"sceneStartedAckTimeoutMs", 1000, 15000},
        {"sceneStopTimeoutMs", 1000, 120000},
        {"sceneMaxStartSkewMs", 50, 5000},
        {"uploadIdleTimeoutMs", 5000, 600000},
        {"uploadTargetAckTimeoutMs", 1000, 120000},
        {"removalAckTimeoutMs", 1000, 120000},
    };
    QHash<QString, qint64> values;
    for (const Rule& rule : rules) {
        const qint64 value = boundedInteger(policy.value(QString::fromLatin1(rule.name)),
                                            rule.minimum, rule.maximum);
        if (value < 0) {
            if (errorMessage) {
                *errorMessage = QStringLiteral("Invalid server policy field: %1")
                                    .arg(QString::fromLatin1(rule.name));
            }
            return false;
        }
        values.insert(QString::fromLatin1(rule.name), value);
    }
    if (values.value("leaseTimeoutMs") < values.value("heartbeatIntervalMs") * 4
        || values.value("sceneMaxClockSkewMs") >= values.value("sceneActivationLeadMs")
        || values.value("sceneMaxClockSkewMs") * 2
            > values.value("sceneMaxStartSkewMs")
        || values.value("sceneMaxStartSkewMs")
            >= values.value("sceneStartedAckTimeoutMs")) {
        if (errorMessage) *errorMessage = QStringLiteral("Server policy invariants are invalid");
        return false;
    }
    return true;
}

bool WebSocketClient::handleWelcome(const QJsonObject& message) {
    if (m_authenticated
        || boundedInteger(message.value(QStringLiteral("protocolVersion")),
                          ProtocolVersion, ProtocolVersion) != ProtocolVersion
        || !isCanonicalUuid(
            message.value(QStringLiteral("messageId")).toString())
        || message.value("serverBootId").toString() != m_pendingServerBootId
        || !isCanonicalUuid(
            message.value(QStringLiteral("connectionId")).toString())
        || message.value("installationId").toString() != m_installationId
        || message.value("endpointId").toString() != m_endpointId
        || message.value("instanceId").toString() != m_instanceId
        || message.value("runtimeId").toString() != m_runtimeId) {
        emit fatalError(QStringLiteral("Authenticated welcome does not match this connection"));
        abortConnectionAttempt();
        return false;
    }

    quint64 newGeneration = 0;
    const bool validGeneration = readPositiveSafeJsonInteger(
        message.value(QStringLiteral("connectionGeneration")), &newGeneration);
    QString policyError;
    const QJsonObject policy = message.value("policy").toObject();
    const qint64 serverMonotonicMs = boundedInteger(
        message.value(QStringLiteral("serverMonotonicMs")), 0,
        9007199254740991LL);
    if (!validGeneration || serverMonotonicMs < 0
        || !validateServerPolicy(policy, &policyError)) {
        emit fatalError(policyError.isEmpty()
                            ? QStringLiteral("Invalid welcome protocol integer")
                            : policyError);
        abortConnectionAttempt();
        return false;
    }

    const QString newBootId = message.value("serverBootId").toString();
    const QString previousBootId = m_serverBootId;
    const quint64 previousGeneration = m_connectionGeneration;
    const bool sameBoot = previousBootId.isEmpty() || previousBootId == newBootId;
    const bool withinLease = m_hasEstablishedLease && sameBoot && hasUnexpiredLease();

    if (!previousBootId.isEmpty() && previousBootId != newBootId) {
        expireLease();
        emit serverRestarted(previousBootId, newBootId);
    } else if (m_hasEstablishedLease && !hasUnexpiredLease()) {
        expireLease();
    }
    if (!previousBootId.isEmpty() && previousBootId == newBootId
        && previousGeneration > 0 && newGeneration <= previousGeneration) {
        emit fatalError(QStringLiteral("Server returned a stale connection generation"));
        abortConnectionAttempt();
        return false;
    }

    m_serverBootId = newBootId;
    m_pendingServerBootId.clear();
    m_connectionGeneration = newGeneration;
    m_socketClientId = message.value("connectionId").toString();
    m_serverPolicy = policy;
    m_heartbeatIntervalMs = policy.value("heartbeatIntervalMs").toInt();
    m_leaseTimeoutMs = policy.value("leaseTimeoutMs").toInt();
    if (m_sceneRuns) {
        m_sceneRuns->setPrepareTimeoutMs(policy.value("scenePrepareTimeoutMs").toInt());
    }
    m_authenticated = true;
    m_hasEstablishedLease = true;
    m_leaseExpired = false;
    m_degraded = false;
    m_disconnectSignalEmitted = false;
    noteServerContact();
    m_heartbeatTimer->start(m_heartbeatIntervalMs);
    m_leaseHealthTimer->start();
    setConnectionStatus("Connected");
    emit serverPolicyReceived(m_serverPolicy);
    emit transportHealthChanged(false);
    emit connected();
    if (withinLease && previousGeneration > 0) {
        emit reauthenticatedWithinLease(previousGeneration, newGeneration);
        QTimer::singleShot(0, this, &WebSocketClient::resumeAllRemoteSessions);
    }
    sendHeartbeat();
    return true;
}

void WebSocketClient::noteServerContact() {
    if (!m_authenticated || m_leaseExpired) return;
    m_lastServerContactContinuousMs = suspendInclusiveNowMs();
    if (m_degraded) {
        m_degraded = false;
        emit transportHealthChanged(false);
        setConnectionStatus("Connected");
    }
}

qint64 WebSocketClient::suspendInclusiveNowMs() const
{
    return m_suspendInclusiveClock ? m_suspendInclusiveClock() : -1;
}

qint64 WebSocketClient::leaseElapsedMs() const
{
    if (m_lastServerContactContinuousMs < 0) {
        return std::numeric_limits<qint64>::max();
    }
    const qint64 now = suspendInclusiveNowMs();
    if (now < m_lastServerContactContinuousMs) {
        // A broken/injected clock or wall-clock rollback on the fallback path
        // can never grant extra control time.
        return std::numeric_limits<qint64>::max();
    }
    return now - m_lastServerContactContinuousMs;
}

void WebSocketClient::expireLease() {
    if (!m_hasEstablishedLease || m_leaseExpired) return;
    m_leaseExpired = true;
    m_heartbeatTimer->stop();
    m_clockSyncBurstTimer->stop();
    m_clockSyncBurstRemaining = 0;
    resetSceneClockEstimate();
    if (!m_degraded) {
        m_degraded = true;
        emit transportHealthChanged(true);
    }
    // Observers must still be able to enumerate every incoming/outgoing
    // binding while handling the terminal edge (cache quarantine, upload
    // invalidation, render teardown). Clear only after synchronous delivery.
    emit leaseExpired(m_serverBootId, m_connectionGeneration);
    if (m_sceneRuns) m_sceneRuns->clearSessions();
}

void WebSocketClient::handleMessage(const QJsonObject& message) {
    QString type = message["type"].toString();
    if (type == "auth_challenge") {
        handleAuthChallenge(message);
        return;
    }
    if (type == "welcome") {
        handleWelcome(message);
        return;
    }
    if (!m_authenticated) {
        if (type == "error") {
            emit connectionError(message.value("message").toString(
                QStringLiteral("Authentication rejected")));
        } else {
            emit fatalError(QStringLiteral("Server sent application data before authentication"));
        }
        abortConnectionAttempt();
        return;
    }
    if (boundedInteger(message.value(QStringLiteral("protocolVersion")),
                       ProtocolVersion, ProtocolVersion) != ProtocolVersion
        || message.value("serverBootId").toString() != m_serverBootId) {
        emit fatalError(QStringLiteral("Rejected message with mismatched protocol or server boot id"));
        abortConnectionAttempt();
        return;
    }
    if (!isCanonicalUuid(message.value(QStringLiteral("messageId")).toString())) {
        qWarning() << "Rejected protocol v5 message without a canonical messageId";
        return;
    }
    if (isRemovedWireType(type) || containsRemovedWireField(message)) {
        qWarning() << "Rejected removed protocol message type or field" << type;
        return;
    }
    // The first socket event after system wake must observe the absolute lease
    // boundary before it can refresh lastContact. At exactly timeout the old
    // RemoteSession is terminal and this packet is never dispatched.
    if (!hasUnexpiredLease()) {
        checkLeaseHealth();
        return;
    }
    noteServerContact();
    // Suppress noisy logs for high-frequency message types
    if (type != "upload_progress" && type != "upload_chunk"
        && type != "remote_session_cursor") {
        qDebug() << "Received message type:" << type;
    }
    
    if (type == "heartbeat_ack") {
        quint64 sequence = 0;
        quint64 responseConnectionGeneration = 0;
        qint64 echoedAt = -1;
        qint64 serverAt = -1;
        if (!readPositiveSafeJsonInteger(message.value("sequence"), &sequence)
            || !readPositiveSafeJsonInteger(
                message.value("connectionGeneration"),
                &responseConnectionGeneration)
            || responseConnectionGeneration != m_connectionGeneration
            || (echoedAt = boundedInteger(
                    message.value("clientMonotonicMs"), 0,
                    9007199254740991LL)) < 0
            || (serverAt = boundedInteger(
                    message.value("serverMonotonicMs"), 0,
                    9007199254740991LL)) < 0) {
            return;
        }
        if (!m_heartbeatSentAt.contains(sequence)) return;
        const qint64 sentAt = m_heartbeatSentAt.take(sequence);
        const qint64 receivedAt = m_processClock.elapsed();
        if (sentAt >= 0 && echoedAt == sentAt && serverAt >= 0 && receivedAt >= sentAt) {
            const qint64 rtt = receivedAt - sentAt;
            qint64 offset = serverAt - (sentAt + rtt / 2);
            // Add one millisecond for the independent integer timestamp
            // quantization at the two clocks; the value remains a conservative
            // bound instead of merely a rounded RTT statistic.
            qint64 uncertainty = (rtt + 1) / 2 + 1;

            // Protocol-v5 servers include both their receive and transmit
            // timestamps. Removing server-side processing time from the RTT is
            // the standard four-timestamp/NTP estimate and prevents a busy
            // relay from looking like network/clock uncertainty. Keep the
            // legacy single timestamp fallback for rolling upgrades.
            const qint64 serverReceivedAt = boundedInteger(
                message.value(QStringLiteral("serverReceiveMonotonicMs")), 0,
                9007199254740991LL);
            const qint64 serverTransmittedAt = boundedInteger(
                message.value(QStringLiteral("serverTransmitMonotonicMs")), 0,
                9007199254740991LL);
            if (serverReceivedAt >= 0
                && serverTransmittedAt >= serverReceivedAt
                && serverTransmittedAt == serverAt) {
                const qint64 serverProcessingMs =
                    serverTransmittedAt - serverReceivedAt;
                // Millisecond quantization can make the measured server span
                // exceed the client RTT by one tick. Larger contradictions are
                // malformed and deliberately fall back to the conservative
                // legacy estimate.
                if (serverProcessingMs <= rtt + 1) {
                    const qint64 networkRoundTripMs =
                        std::max<qint64>(0, rtt - serverProcessingMs);
                    offset = ((serverReceivedAt - sentAt)
                              + (serverTransmittedAt - receivedAt)) / 2;
                    uncertainty = (networkRoundTripMs + 1) / 2 + 1;
                }
            }

            // NTP-style clock filtering: queueing and scheduling spikes make
            // a single RTT sample worse, never more authoritative. Retain a
            // short rolling window and use its lowest-delay sample instead of
            // replacing a precise estimate with the latest outlier. The
            // window is bounded by both time and count, and is reset for every
            // transport/authentication generation.
            const qint64 sampleWindowMs = std::max<qint64>(1, m_leaseTimeoutMs);
            m_clockSamples.append({receivedAt, offset, uncertainty});
            while (!m_clockSamples.isEmpty()
                   && (m_clockSamples.size() > 8
                       || receivedAt - m_clockSamples.constFirst().receivedAtMs
                              >= sampleWindowMs)) {
                m_clockSamples.removeFirst();
            }
            const auto best = std::min_element(
                m_clockSamples.cbegin(), m_clockSamples.cend(),
                [](const ClockSample& left, const ClockSample& right) {
                    if (left.uncertaintyMs != right.uncertaintyMs) {
                        return left.uncertaintyMs < right.uncertaintyMs;
                    }
                    return left.receivedAtMs > right.receivedAtMs;
                });
            if (best != m_clockSamples.cend()) {
                m_serverMonotonicOffsetMs = best->offsetMs;
                m_clockUncertaintyMs = best->uncertaintyMs;
                m_selectedClockSampleReceivedAtMs = best->receivedAtMs;
            }
            emit heartbeatSampleReceived(sequence, rtt, offset, uncertainty);
        }
    }
    else if (type == "error") {
        if (message.value("scope").toString() == QLatin1String("scene")) {
            emit sceneErrorReceived(message);
            return;
        }
        if (message.value("scope").toString() == QLatin1String("upload_remove")) {
            const QString remoteSessionId =
                message.value(QStringLiteral("remoteSessionId")).toString();
            quint64 generation = 0;
            quint64 connectionGeneration = 0;
            const double offset =
                message.value(QStringLiteral("offset")).toDouble(-1.0);
            const double size =
                message.value(QStringLiteral("size")).toDouble(-1.0);
            const SceneRunCoordinator::SessionBinding binding = m_sceneRuns
                ? m_sceneRuns->sessionById(remoteSessionId)
                : SceneRunCoordinator::SessionBinding();
            const bool validGenerations = readPositiveSafeJsonInteger(
                    message.value(QStringLiteral("generation")), &generation)
                && readPositiveSafeJsonInteger(
                    message.value(QStringLiteral("connectionGeneration")),
                    &connectionGeneration);
            const bool correlated = validGenerations
                && !binding.remoteSessionId.isEmpty()
                && binding.ownerEndpointId == m_endpointId
                && binding.generation == generation
                && connectionGeneration == m_connectionGeneration
                && message.value(QStringLiteral("ownerEndpointId")).toString()
                    == binding.ownerEndpointId
                && message.value(QStringLiteral("targetEndpointId")).toString()
                    == binding.targetEndpointId
                && isCanonicalUuid(
                    message.value(QStringLiteral("removalId")).toString())
                && isUploadOpaqueId(
                    message.value(QStringLiteral("uploadId")).toString())
                && isUploadOpaqueId(
                    message.value(QStringLiteral("assetId")).toString())
                && isSafeJsonInteger(size, 1.0,
                                     16.0 * 1024 * 1024 * 1024)
                && isSafeJsonInteger(offset, 0.0, size)
                && offset == size
                && isUploadSha256(
                    message.value(QStringLiteral("sha256")).toString());
            if (!correlated) {
                qWarning() << "Rejected uncorrelated asset removal error";
                return;
            }
            emit uploadMessageReceived(message);
            return;
        }
        const QString code = message.value(QStringLiteral("code")).toString();
        if (message.value(QStringLiteral("scope")).toString()
                == QLatin1String("remote_session")
            || isRemoteSessionBusinessError(code)) {
            qWarning() << "RemoteSession command rejected:" << code;
            emit remoteSessionError(message);
            return;
        }
        const QString err = message.value("message").toString();
        qWarning() << "❌ Server error:" << err;

        // For other errors, emit signal so UI can show the error to user
        emit connectionError(err);
    }
    else if (type == "endpoint_snapshot_applied") {
        QJsonObject clientInfoObj = message["snapshot"].toObject();
        clientInfoObj.insert(QStringLiteral("id"), m_endpointId);
        clientInfoObj.insert(QStringLiteral("runtimeId"), m_runtimeId);
        ClientInfo clientInfo = ClientInfo::fromJson(clientInfoObj);
        if (clientInfo.installationId() != m_installationId
            || clientInfo.endpointId() != m_endpointId
            || clientInfo.instanceId() != m_instanceId
            || clientInfo.instanceOrdinal() != m_instanceOrdinal
            || clientInfo.runtimeId() != m_runtimeId) {
            emit fatalError(QStringLiteral("Registration identity does not match authenticated endpoint"));
            abortConnectionAttempt();
            return;
        }
        qDebug() << "Endpoint snapshot applied for endpoint" << m_endpointId
                 << "runtime" << m_runtimeId;
        emit registrationConfirmed(clientInfo);
        // Keep the optional high-throughput channel ready before the first
        // upload. Failure is harmless; beginUploadSession() pins the control
        // channel immediately when this fast path is unavailable.
        QTimer::singleShot(0, this, [this]() { ensureUploadChannel(); });
    }
    else if (type == "upload_channel_token") {
        const QString token = message.value("token").toString();
        quint64 tokenGeneration = 0;
        m_uploadChannelTokenRequested = false;
        if (isUploadOpaqueId(token) && token.size() >= 32
            && readPositiveSafeJsonInteger(
                message.value(QStringLiteral("connectionGeneration")),
                &tokenGeneration)
            && tokenGeneration == m_connectionGeneration
            && boundedInteger(message.value(QStringLiteral("expiresAt")), 0,
                              9007199254740991LL) >= 0) {
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
    else if (type == "upload_start" || type == "upload_resume"
             || type == "upload_resume_ready" || type == "upload_ready"
             || type == "upload_chunk" || type == "upload_progress"
             || type == "upload_complete" || type == "upload_finished"
             || type == "upload_rejected" || type == "upload_abort"
             || type == "upload_aborted" || type == "upload_abort_ack"
             || type == "upload_remove" || type == "upload_removed") {
        const QString uploadId = message.value(QStringLiteral("uploadId")).toString();
        const QString remoteSessionId =
            message.value(QStringLiteral("remoteSessionId")).toString();
        quint64 generation = 0;
        const bool validGeneration = readPositiveSafeJsonInteger(
            message.value(QStringLiteral("generation")), &generation);
        const bool unboundStartRejection = type == QLatin1String("upload_rejected")
            && remoteSessionId.isEmpty()
            && !message.contains(QStringLiteral("generation"));
        const SceneRunCoordinator::SessionBinding binding = m_sceneRuns
            ? m_sceneRuns->sessionById(remoteSessionId)
            : SceneRunCoordinator::SessionBinding();
        quint64 sourceConnectionGeneration = 0;
        const bool removalTransportCorrelated = type != QLatin1String("upload_remove")
            || (readPositiveSafeJsonInteger(
                    message.value(QStringLiteral("connectionGeneration")),
                    &sourceConnectionGeneration)
                && sourceConnectionGeneration == binding.ownerConnectionGeneration);
        const bool correlated = validGeneration
            && !binding.remoteSessionId.isEmpty()
            && binding.generation == generation
            && removalTransportCorrelated
            && message.value(QStringLiteral("ownerEndpointId")).toString()
                == binding.ownerEndpointId
            && message.value(QStringLiteral("targetEndpointId")).toString()
                == binding.targetEndpointId;
        if (!isUploadOpaqueId(uploadId) || (!correlated && !unboundStartRejection)) {
            qWarning() << "Rejected stale or malformed protocol v5 upload envelope";
            return;
        }
        emit uploadMessageReceived(message);
    }
    else if (type == "remote_session_offer") {
        if (!m_sceneRuns
            || !m_sceneRuns->upsertSession(message, m_connectionGeneration)) {
            qWarning() << "Rejected stale or malformed RemoteSession offer";
            return;
        }
        if (m_endpointDraining) {
            closeRemoteSession(
                message.value(QStringLiteral("remoteSessionId")).toString(),
                nullptr, QStringLiteral("client_disabled"));
            return;
        }
        emit remoteSessionOfferReceived(message);
        if (!acceptRemoteSessionOffer(message)) {
            qWarning() << "Could not accept RemoteSession offer with a fresh snapshot";
        }
    }
    else if (type == "media_residency") {
        const auto binding = m_sceneRuns->sessionById(
            message.value(QStringLiteral("remoteSessionId")).toString());
        if (!binding.active || binding.ownerEndpointId != m_endpointId
            || binding.generation != message.value(QStringLiteral("generation")).toInteger()
            || binding.targetEndpointId != message.value(QStringLiteral("targetEndpointId")).toString()
            || binding.ownerEndpointId != message.value(QStringLiteral("ownerEndpointId")).toString()) return;
        emit mediaResidencyReceived(message);
    }
    else if (type == "remote_session_cursor") {
        const QString sessionId = message.value(QStringLiteral("remoteSessionId")).toString();
        const auto binding = m_sceneRuns->sessionById(sessionId);
        quint64 generation = 0, sequence = 0, ownerConnection = 0, targetConnection = 0;
        quint64 recipientConnection = 0;
        const auto visibleValue = message.value(QStringLiteral("visible"));
        const qint64 screenId = boundedInteger(message.value(QStringLiteral("screenId")), -1, 1000000);
        const qint64 x = boundedInteger(message.value(QStringLiteral("x")), 0, 99999);
        const qint64 y = boundedInteger(message.value(QStringLiteral("y")), 0, 99999);
        if (!binding.active || binding.ownerEndpointId != m_endpointId
            || binding.ownerConnectionGeneration != m_connectionGeneration
            || !readPositiveSafeJsonInteger(message.value(QStringLiteral("connectionGeneration")), &recipientConnection)
            || recipientConnection != m_connectionGeneration
            || !readPositiveSafeJsonInteger(message.value(QStringLiteral("generation")), &generation)
            || generation != binding.generation
            || !readPositiveSafeJsonInteger(message.value(QStringLiteral("sequence")), &sequence)
            || !readPositiveSafeJsonInteger(message.value(QStringLiteral("ownerConnectionGeneration")), &ownerConnection)
            || ownerConnection != binding.ownerConnectionGeneration
            || !readPositiveSafeJsonInteger(message.value(QStringLiteral("targetConnectionGeneration")), &targetConnection)
            || targetConnection != binding.targetConnectionGeneration
            || message.value(QStringLiteral("ownerEndpointId")).toString() != binding.ownerEndpointId
            || message.value(QStringLiteral("targetEndpointId")).toString() != binding.targetEndpointId
            || !visibleValue.isBool() || x < 0 || y < 0
            || (visibleValue.toBool() && screenId < 0)
            || (!visibleValue.toBool()
                && (!message.value(QStringLiteral("screenId")).isDouble()
                    || message.value(QStringLiteral("screenId")).toDouble() != -1
                    || x != 0 || y != 0))) return;
        CursorSequence& previous = m_receivedCursorSequenceBySession[sessionId];
        if (previous.generation == generation && sequence <= previous.sequence) return;
        previous = {generation, sequence};
        emit remoteCursorReceived(sessionId, static_cast<int>(screenId),
                                  QPointF(x, y), visibleValue.toBool());
    }
    else if (type == "remote_session_snapshot") {
        RemoteSessionCoordinator* sessions = remoteSessionCoordinator();
        if (!sessions || !sessions->acceptSnapshot(message, m_connectionGeneration)) {
            qWarning() << "Rejected stale or malformed RemoteSession snapshot";
            return;
        }
        emit remoteSessionSnapshotReceived(message);
        emit messageReceived(message);
    }
    else if (type == "remote_session_opening" || type == "remote_session_opened"
             || type == "remote_session_resumed"
             || type == "remote_session_lease_state"
             || type == "remote_session_terminating") {
        if (!m_sceneRuns
            || !m_sceneRuns->upsertSession(message, m_connectionGeneration)) {
            qWarning() << "Rejected stale or malformed RemoteSession envelope" << type;
            return;
        }
        if (type == "remote_session_opening") {
            emit messageReceived(message);
            return;
        }
        if (type == "remote_session_opened") {
            const RemoteSessionCoordinator::Binding binding =
                m_sceneRuns->sessionById(
                    message.value(QStringLiteral("remoteSessionId")).toString());
            if (binding.ownerEndpointId == m_endpointId
                && (!remoteSessionCoordinator()
                    || !remoteSessionCoordinator()->acceptSnapshot(
                        message, m_connectionGeneration))) {
                closeRemoteSession(binding.remoteSessionId, nullptr,
                                   QStringLiteral("invalid_initial_snapshot"));
                QJsonObject failure{
                    {QStringLiteral("scope"), QStringLiteral("remote_session")},
                    {QStringLiteral("code"), QStringLiteral("invalid_initial_snapshot")},
                    {QStringLiteral("message"),
                     QStringLiteral("The remote client returned an invalid initial snapshot")},
                    {QStringLiteral("requestId"),
                     message.value(QStringLiteral("requestId"))},
                    {QStringLiteral("remoteSessionId"), binding.remoteSessionId},
                    {QStringLiteral("generation"),
                     static_cast<double>(binding.generation)},
                    {QStringLiteral("ownerEndpointId"), binding.ownerEndpointId},
                    {QStringLiteral("targetEndpointId"), binding.targetEndpointId}
                };
                emit remoteSessionError(failure);
                return;
            }
            m_targetSnapshotSequenceBySession.insert(binding.remoteSessionId, 1);
            emit remoteSessionOpened(message);
        }
        else if (type == "remote_session_resumed") {
            const RemoteSessionCoordinator::Binding binding =
                m_sceneRuns->sessionById(
                    message.value(QStringLiteral("remoteSessionId")).toString());
            if (binding.targetEndpointId == m_endpointId
                && !m_registeredTargetSnapshot.isEmpty()) {
                sendRemoteSessionSnapshot(binding.remoteSessionId,
                                          binding.generation,
                                          m_registeredTargetSnapshot);
            }
            emit remoteSessionResumed(message);
        }
        else if (type == "remote_session_lease_state") emit remoteSessionLeaseStateChanged(message);
        else emit remoteSessionTerminating(message);
        emit messageReceived(message);
    }
    else if (type == "remote_session_closed") {
        if (!m_sceneRuns
            || !m_sceneRuns->removeSession(message, m_connectionGeneration)) {
            qWarning() << "Rejected stale, unknown, or malformed RemoteSession close";
            return;
        }
        emit remoteSessionClosed(message);
        m_targetSnapshotSequenceBySession.remove(
            message.value(QStringLiteral("remoteSessionId")).toString());
        m_receivedCursorSequenceBySession.remove(
            message.value(QStringLiteral("remoteSessionId")).toString());
        emit messageReceived(message);
    }
    else if (type == "endpoint_disable_started") {
        if (!m_endpointDraining) {
            qWarning() << "Rejected unsolicited endpoint disable acknowledgement";
            return;
        }
        emit endpointDisableAcknowledged();
    }
    else if (type == "scene_prepare" || type == "prepare_progress"
             || type == "prepared" || type == "armed" || type == "commit"
             || type == "started" || type == "state_snapshot"
             || type == "stop" || type == "stopped") {
        QString validationError;
        if (!m_sceneRuns || !m_sceneRuns->acceptInboundEnvelope(message, &validationError)) {
            qWarning() << "Rejected protocol v5 scene message:" << validationError;
            return;
        }
        if (type == "scene_prepare") emit scenePrepareReceived(message);
        else if (type == "prepare_progress") emit scenePrepareProgressReceived(message);
        else if (type == "prepared") emit scenePreparedReceived(message);
        else if (type == "armed") emit sceneArmedReceived(message);
        else if (type == "commit") emit sceneCommitReceived(message);
        else if (type == "started") emit sceneStartedReceived(message);
        else if (type == "state_snapshot") emit sceneStateSnapshotReceived(message);
        else if (type == "stop") emit sceneStopReceived(message);
        else if (type == "stopped") emit sceneStoppedReceived(message);
    }
    else {
        // Forward unknown messages
        emit messageReceived(message);
    }
}

void WebSocketClient::sendMessage(const QJsonObject& message) {
    sendControlMessage(message);
}

QJsonObject WebSocketClient::addProtocolEnvelope(const QJsonObject& message) const {
    QJsonObject enveloped = message;
    enveloped["protocolVersion"] = ProtocolVersion;
    enveloped["serverBootId"] = m_serverBootId;
    enveloped["connectionGeneration"] = static_cast<double>(m_connectionGeneration);
    if (enveloped.value("messageId").toString().isEmpty()) {
        enveloped["messageId"] = QUuid::createUuid().toString(QUuid::WithoutBraces);
    }
    return enveloped;
}

bool WebSocketClient::sendRawControlMessage(const QJsonObject& message) {
    if (!isTransportConnected() || !m_webSocket) return false;
    const QJsonDocument document(message);
    return m_webSocket->sendTextMessage(document.toJson(QJsonDocument::Compact)) >= 0;
}

bool WebSocketClient::sendControlMessage(const QJsonObject& message) {
    if (!isConnected() || !m_webSocket) {
        qWarning() << "Cannot send message: not connected";
        return false;
    }
    if (!hasUnexpiredLease()) {
        checkLeaseHealth();
        qWarning() << "Cannot send message: connection lease expired";
        return false;
    }
    if (containsRemovedWireField(message)) {
        qWarning() << "Refusing protocol v5 message containing a removed wire field";
        return false;
    }

    return sendRawControlMessage(addProtocolEnvelope(message));
}

bool WebSocketClient::sendMessageUpload(const QJsonObject& message) {
    if (!m_uploadSessionActive) {
        qWarning() << "Cannot send upload payload before the session transport is locked";
        return false;
    }
    if (!hasUnexpiredLease()) {
        checkLeaseHealth();
        qWarning() << "Cannot send upload payload: connection lease expired";
        return false;
    }
    if (containsRemovedWireField(message)) {
        qWarning() << "Refusing upload message containing a removed wire field";
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

    QJsonDocument doc(addProtocolEnvelope(message));
    return channel->sendTextMessage(doc.toJson(QJsonDocument::Compact)) >= 0;
}

void WebSocketClient::setConnectionStatus(const QString& status) {
    if (m_connectionStatus != status) {
        m_connectionStatus = status;
        qDebug() << "Connection status changed to:" << status;
        emit connectionStatusChanged(m_connectionStatus);
    }
}
