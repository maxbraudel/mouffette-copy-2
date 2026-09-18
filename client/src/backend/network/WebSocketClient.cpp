#include "backend/network/WebSocketClient.h"
#include "backend/config/AppConfig.h"
#include "backend/network/RetryPolicy.h"
#include "backend/runtime/SuspendInclusiveClock.h"
#include "backend/runtime/RuntimeProfile.h"
#include "backend/network/SceneRunCoordinator.h"
#include "backend/security/DeviceIdentityStore.h"
#include "MediaFormatContract.h"
#include <QJsonArray>
#include <QDebug>
#include <QUrlQuery>
#include <QUuid>
#include <QRegularExpression>
#include <QDateTime>
#include <QDir>
#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>

namespace {
constexpr int kClockSyncBurstProbeCount = 5;
constexpr int kClockSyncBurstIntervalMs = 100;
constexpr int kClockSyncBurstCooldownMs = 1000;

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
    : WebSocketClient(QString(), true, parent, {}, 1) {}

WebSocketClient::WebSocketClient(const QString& identityFallbackDirectory,
                                 bool preferNativeIdentityVault,
                                 QObject *parent,
                                 SuspendInclusiveClock suspendInclusiveClock,
                                 int instanceOrdinal)
    : QObject(parent)
    , m_controlRetries(this, [this] { return suspendInclusiveNowMs(); })
    , m_uploadRetries(this, [this] { return suspendInclusiveNowMs(); })
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
    , m_instanceId(DeviceIdentityStore::instanceIdForOrdinal(instanceOrdinal))
    , m_instanceOrdinal(instanceOrdinal)
{
    if (!m_suspendInclusiveClock) {
        m_suspendInclusiveClock = MouffetteClock::nowMs;
    }
    m_processClock.start();
    m_deviceSnapshotRetryTimer.setSingleShot(true);
    m_deviceSnapshotRetryTimer.setInterval(100);
    connect(&m_deviceSnapshotRetryTimer, &QTimer::timeout,
            this, &WebSocketClient::publishDeviceSnapshots);
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
        completeSessionRequests(sessionId, true, true);
        m_receivedCursorSequenceBySession.remove(sessionId);
        m_publishedDeviceSnapshots.remove(sessionId);
        m_sessionDeadlines.remove(sessionId);
        m_resumeRequestIds.remove(sessionId);
    });

    const auto profile = RuntimeProfile::context();
    const bool preparedInstallation = !profile.installationRootPath.isEmpty()
        && (identityFallbackDirectory.isEmpty()
            || QDir::cleanPath(identityFallbackDirectory)
                == QDir::cleanPath(RuntimeProfile::resolvedInstallationRoot(profile)));
    const auto loadIdentity = [&] {
        // Bootstrap alone may create a shared installation key, under its
        // interprocess lock. Losing it afterwards must not silently rotate
        // all numbered endpoints during construction of a network client.
        return preparedInstallation
            ? m_identityStore->initializeExisting(&m_identityInitializationError)
            : m_identityStore->initialize(&m_identityInitializationError);
    };
    if (m_instanceId.isEmpty()) {
        m_identityInitializationError = QStringLiteral("Instance ordinal must be between 1 and INT_MAX");
        qCritical().noquote() << m_identityInitializationError;
    } else if (!loadIdentity()) {
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
        failUploadChannelAttempt(QStringLiteral("Upload channel response rejected"));
        return;
    }
    if (!doc.isObject()) {
        qWarning() << "Rejected non-object message on upload channel";
        failUploadChannelAttempt(QStringLiteral("Upload channel response rejected"));
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
            failUploadChannelAttempt(QStringLiteral("Upload channel response rejected"));
            return;
        }
        m_uploadRetries.cancel(QStringLiteral("attempt"));
        m_uploadRetries.schedule(QStringLiteral("stable"), AppConfig::instance().reconnectStableResetMs(), [this] {
            if (isUploadChannelConnected()) m_uploadRetryAttempt = 0;
        });
        m_uploadClientId = readyEndpointId;
        qDebug() << "Upload channel authenticated for client:" << m_uploadClientId;
        return;
    }
    if (type == "welcome") {
        // The removed upload-channel welcome is not proof of identity. Keep the
        // dedicated socket unauthenticated and let the upload fall back to the
        // already authenticated control connection.
        qWarning() << "Upload channel server did not provide authenticated readiness";
        failUploadChannelAttempt(QStringLiteral("Upload channel response rejected"));
        return;
    }
    if (type == "error" && !m_uploadChannelAuthenticated) {
        qWarning() << "Upload channel authentication failed:"
                   << obj.value("message").toString();
        failUploadChannelAttempt(QStringLiteral("Upload channel response rejected"));
        return;
    }
    if (!m_uploadChannelAuthenticated) {
        qWarning() << "Rejected application message on an unauthenticated upload channel";
        failUploadChannelAttempt(QStringLiteral("Upload channel response rejected"));
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
    clearControlRequests();
    m_endpointDraining = true;
    m_pendingServerBootId.clear();
    m_endpointDisableRequestId.clear();
    if (m_hasEstablishedLease) {
        emit sessionsInvalidated(QStringLiteral("user_disabled"), m_serverBootId, m_connectionGeneration);
        if (m_sceneRuns) m_sceneRuns->clearSessions();
    }
    m_sessionDeadlines.clear();
    m_resumeRequestIds.clear();
    m_reconcileRequestId.clear();
    m_hasEstablishedLease = false;
    m_leaseExpired = true;
    m_leaseHealthTimer->stop();
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
    m_pendingServerBootId.clear();
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

void WebSocketClient::scheduleUploadChannelRetry(const QString& reason)
{
    if (!isConnected() || m_endpointDraining || m_uploadRetries.contains(QStringLiteral("retry"))) return;
    const auto& config = AppConfig::instance();
    const int delay = RetryPolicy{config.uploadChannelRetryBaseMs(), config.uploadChannelRetryMaxMs(),
                                 config.reconnectJitterPercent()}.delay(m_uploadRetryAttempt);
    m_uploadRetryAttempt = RetryPolicy::increment(m_uploadRetryAttempt);
    const quint64 transport = m_connectionGeneration;
    qInfo() << "upload_channel_retry" << "reason" << reason << "delayMs" << delay << "transport" << transport;
    m_uploadRetries.schedule(QStringLiteral("retry"), delay, [this, transport] {
        if (transport == m_connectionGeneration && isConnected() && !m_endpointDraining) ensureUploadChannel();
    });
}

void WebSocketClient::failUploadChannelAttempt(const QString& reason)
{
    const bool selected = m_uploadSessionActive && m_useUploadSocketForSession;
    closeUploadChannel();
    if (selected) reportSelectedUploadTransportLost(reason);
    scheduleUploadChannelRetry(reason);
}

void WebSocketClient::onUploadDisconnected()
{
    failUploadChannelAttempt(QStringLiteral("Dedicated upload connection was lost"));
}

void WebSocketClient::onUploadError(QAbstractSocket::SocketError error)
{
    failUploadChannelAttempt(QStringLiteral("Upload socket error: %1").arg(error));
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
        && leaseElapsedMs() < sessionProofBudgetMs();
}

qint64 WebSocketClient::leaseRemainingMs() const {
    if (m_leaseTimeoutMs <= 0 || !m_hasEstablishedLease || m_leaseExpired
        || m_lastServerContactContinuousMs < 0) {
        return 0;
    }
    return std::max<qint64>(0, sessionProofBudgetMs()
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
    if (m_endpointDraining || m_uploadRetries.contains(QStringLiteral("retry"))) return false;
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
            m_uploadTokenRequestId = QUuid::createUuid().toString(QUuid::WithoutBraces);
            const quint64 transport = m_connectionGeneration;
            m_uploadRetries.schedule(QStringLiteral("attempt"), AppConfig::instance().uploadChannelAttemptTimeoutMs(), [this, transport] {
                if (transport == m_connectionGeneration && !isUploadChannelConnected())
                    failUploadChannelAttempt(QStringLiteral("Upload channel establishment timed out"));
            });
            QJsonObject request;
            request["requestId"] = m_uploadTokenRequestId;
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
    m_uploadRetries.cancelAll();
    m_uploadTokenRequestId.clear();
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

    const QJsonObject content{
        {QStringLiteral("screens"), screensArray},
        {QStringLiteral("systemUI"), QJsonArray()},
        {QStringLiteral("volumePercent"), message.value(QStringLiteral("volumePercent"))}
    };
    // Preserve the shared value on identical captures so cursor gating remains
    // a cheap comparison and never copies/detaches a topology per mouse tick.
    if (content != m_registeredDeviceContent) m_registeredDeviceContent = content;
    m_registeredTargetSnapshot = m_registeredDeviceContent;
    m_registeredTargetSnapshot.insert(QStringLiteral("revision"), static_cast<double>(++m_targetSnapshotRevision));
    m_registeredTargetSnapshot.insert(QStringLiteral("capturedAtEpochMs"),
                                     static_cast<double>(QDateTime::currentMSecsSinceEpoch()));
    
    m_registeredEndpointSnapshot = message;
    publishDeviceSnapshots();
}

void WebSocketClient::invalidateLocalDeviceSnapshot()
{
    completeControlRequest(m_registrationRequestId);
    m_registrationRequestId.clear();
    m_deviceSnapshotRetryTimer.stop();
    m_registeredTargetSnapshot = {};
    m_registeredDeviceContent = {};
    m_registeredEndpointSnapshot = {};
    m_publishedEndpointSnapshot = {};
    m_publishedDeviceSnapshots.clear();
}

bool WebSocketClient::sendTrackedControl(const QJsonObject& message)
{
    const QString id = message.value(QStringLiteral("requestId")).toString();
    if (id.isEmpty() || (!m_pendingControl.contains(id) && m_pendingControl.size() >= 4096)) {
        qWarning() << "Control retry capacity exhausted or request identity missing";
        return false;
    }
    if (!sendControlMessage(message)) return false;
    m_pendingControl.insert(id, {message, m_connectionGeneration, m_serverBootId});
    if (!m_controlRetries.contains(id)) scheduleControlRetry(id);
    return true;
}

void WebSocketClient::scheduleControlRetry(const QString& id)
{
    const auto& config = AppConfig::instance();
    const int delay = RetryPolicy{config.controlRequestRetryMs(), config.controlRequestRetryMs(),
                                 config.reconnectJitterPercent(), RetryPolicy::Growth::Fixed}.delay(0);
    m_controlRetries.schedule(id, delay, [this, id] {
        auto it = m_pendingControl.find(id);
        if (it == m_pendingControl.end()) return;
        const PendingControl pending = it.value();
        const QString type = pending.message.value(QStringLiteral("type")).toString();
        const QString session = pending.message.value(QStringLiteral("remoteSessionId")).toString();
        if (!isConnected() || pending.transport != m_connectionGeneration || pending.boot != m_serverBootId
            || (m_endpointDraining && type != QLatin1String("endpoint_disable")
                && type != QLatin1String("remote_session_close"))) {
            completeControlRequest(id);
            return;
        }
        // Revalidate the session clock before a delayed RESUME, including on wake.
        if (type == QLatin1String("remote_session_resume") && sessionRecoveryRemainingMs(session) <= 0) {
            completeControlRequest(id);
            checkSessionRecoveryDeadlines();
            return;
        }
        qInfo() << "control_retry" << "type" << type << "requestId" << id
                << "session" << session << "transport" << m_connectionGeneration << "replay" << true;
        sendControlMessage(pending.message);
        if (m_pendingControl.contains(id)) scheduleControlRetry(id);
    });
}

void WebSocketClient::completeControlRequest(const QString& id)
{
    m_pendingControl.remove(id);
    m_controlRetries.cancel(id);
}

void WebSocketClient::clearControlRequests()
{
    m_pendingControl.clear();
    m_controlRetries.cancelAll();
    m_registrationRequestId.clear();
}

bool WebSocketClient::sessionRecoveryInProgress(const QString& sessionId) const
{
    for (const auto& pending : m_pendingControl) {
        if (pending.message.value(QStringLiteral("remoteSessionId")).toString() == sessionId
            && pending.message.value(QStringLiteral("type")).toString() == QLatin1String("remote_session_resume")) return true;
    }
    return false;
}

void WebSocketClient::completeSessionRequests(const QString& sessionId, bool terminal, bool final)
{
    const auto binding = remoteSessionCoordinator()->byId(sessionId);
    const quint64 localTransport = binding.ownerEndpointId == m_endpointId
        ? binding.ownerConnectionGeneration : binding.targetConnectionGeneration;
    for (const QString& id : m_pendingControl.keys()) {
        const auto message = m_pendingControl.value(id).message;
        const QString type = message.value(QStringLiteral("type")).toString();
        if (message.value(QStringLiteral("remoteSessionId")).toString() == sessionId
            && (type == QLatin1String("remote_session_close") ? final
                : (terminal || (type == QLatin1String("remote_session_resume") && localTransport == m_connectionGeneration))))
            completeControlRequest(id);
        if (type == QLatin1String("remote_session_open")
            && binding.ownerEndpointId == m_endpointId
            && message.value(QStringLiteral("targetEndpointId")).toString() == binding.targetEndpointId
            && (terminal || binding.phase == QLatin1String("Active"))) completeControlRequest(id);
    }
}

void WebSocketClient::publishDeviceSnapshots()
{
    m_deviceSnapshotRetryTimer.stop();
    if (!isConnected() || m_endpointDraining || m_registeredEndpointSnapshot.isEmpty()) return;
    // Retain just the latest capture. No historical snapshots are queued when
    // transport backpressure is present; the next retry sends current state.
    if (m_webSocket->bytesToWrite() > 64 * 1024) {
        m_deviceSnapshotRetryTimer.start();
        return;
    }
    if (m_publishedEndpointGeneration != m_connectionGeneration
        || m_publishedEndpointSnapshot != m_registeredEndpointSnapshot) {
        completeControlRequest(m_registrationRequestId);
        m_registrationRequestId = QUuid::createUuid().toString(QUuid::WithoutBraces);
        QJsonObject request = m_registeredEndpointSnapshot;
        request.insert(QStringLiteral("requestId"), m_registrationRequestId);
        if (!sendTrackedControl(request)) return;
        m_publishedEndpointGeneration = m_connectionGeneration;
        m_publishedEndpointSnapshot = m_registeredEndpointSnapshot;
    }
    const QJsonObject& content = m_registeredDeviceContent;
    const qint64 now = m_processClock.elapsed();
    for (const auto& binding : remoteSessionCoordinator()->all()) {
        if (!binding.active || binding.targetEndpointId != m_endpointId
            || binding.targetConnectionGeneration != m_connectionGeneration) continue;
        auto& last = m_publishedDeviceSnapshots[binding.remoteSessionId];
        if (last.generation == binding.generation && last.content == content
            && last.sentAtMs >= 0 && now - last.sentAtMs < 5000) continue;
        if (m_webSocket->bytesToWrite() > 64 * 1024) {
            m_deviceSnapshotRetryTimer.start();
            break;
        }
        if (sendRemoteSessionSnapshot(binding.remoteSessionId, binding.generation,
                                      m_registeredTargetSnapshot)) {
            last = {binding.generation, content, now};
        }
    }
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
    m_canceledUploadOrder.removeAll(uploadId);

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
    if (!m_canceledUploads.contains(uploadId)) {
        m_canceledUploads.insert(uploadId);
        m_canceledUploadOrder.append(uploadId);
        while (m_canceledUploadOrder.size() > 4096)
            m_canceledUploads.remove(m_canceledUploadOrder.takeFirst());
    }

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
    if (type == QLatin1String("upload_removed")) {
        // Quarantine may finish after resume or logical close. Its authority
        // comes from the exact authenticated removal instruction we accepted,
        // independently of the current command-capable session generation.
        const auto obligation = m_assetRemovalObligations.constFind(
            message.value(QStringLiteral("removalId")).toString());
        if (obligation == m_assetRemovalObligations.cend()
            || obligation->value(QStringLiteral("serverBootId")).toString() != m_serverBootId
            || !obligation->value(QStringLiteral("acceptedGenerations")).toArray().contains(
                QJsonValue(static_cast<double>(generation)))) return false;
        static const QStringList identityFields = {
            QStringLiteral("remoteSessionId"),
            QStringLiteral("removalId"), QStringLiteral("uploadId"),
            QStringLiteral("assetId"), QStringLiteral("offset"), QStringLiteral("size"),
            QStringLiteral("sha256"), QStringLiteral("fileId"), QStringLiteral("extension")
        };
        for (const auto& field : identityFields)
            if (message.value(field) != obligation->value(field)) return false;
    } else {
        if (binding.remoteSessionId.isEmpty() || binding.generation != generation
            || binding.targetEndpointId != m_endpointId) return false;
        if ((type == QLatin1String("upload_ready") || type == QLatin1String("upload_progress")
             || type == QLatin1String("upload_finished")) && !canIssueSessionCommands(remoteSessionId)) return false;
    }
    message.remove(QStringLiteral("ownerEndpointId"));
    message.remove(QStringLiteral("targetEndpointId"));
    const bool sent = sendControlMessage(message);
    if (sent && type == QLatin1String("upload_removed"))
        m_assetRemovalObligations.remove(message.value(QStringLiteral("removalId")).toString());
    return sent;
}

bool WebSocketClient::openRemoteSession(const QString& targetEndpointId,
                                        QString* requestId)
{
    if (!isConnected() || m_endpointDraining || targetEndpointId.isEmpty() || targetEndpointId == m_endpointId) return false;
    const QString correlationId = QUuid::createUuid().toString(QUuid::WithoutBraces);
    if (!replayRemoteSessionOpen(targetEndpointId, correlationId)) return false;
    if (requestId) *requestId = correlationId;
    return true;
}

bool WebSocketClient::replayRemoteSessionOpen(
    const QString& targetEndpointId,
    const QString& requestId)
{
    if (!isConnected() || m_endpointDraining || targetEndpointId.isEmpty()
        || targetEndpointId == m_endpointId
        || !isCanonicalUuid(requestId)) {
        return false;
    }
    QJsonObject message{
        {QStringLiteral("type"), QStringLiteral("remote_session_open")},
        {QStringLiteral("targetEndpointId"), targetEndpointId},
        {QStringLiteral("requestId"), requestId}
    };
    return sendTrackedControl(message);
}

bool WebSocketClient::acceptRemoteSessionOffer(const QJsonObject& offer)
{
    if (!isConnected() || m_endpointDraining || !m_sceneRuns || m_registeredTargetSnapshot.isEmpty()) {
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
    if (m_registeredTargetSnapshot.isEmpty()) return false;
    {
        const QJsonObject& content = m_registeredDeviceContent;
        const auto last = m_publishedDeviceSnapshots.value(remoteSessionId);
        if (last.generation != generation || last.content != content
            || m_publishedEndpointGeneration != m_connectionGeneration
            || m_publishedEndpointSnapshot != m_registeredEndpointSnapshot) return false;
    }
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
    if (!isConnected() || m_endpointDraining || !m_sceneRuns) return false;
    const SceneRunCoordinator::SessionBinding binding =
        m_sceneRuns->sessionById(remoteSessionId);
    if (binding.remoteSessionId.isEmpty() || binding.resumeToken.isEmpty()
        || binding.generation == 0 || m_sessionDeadlines.value(remoteSessionId).expired
        || (m_sessionDeadlines.contains(remoteSessionId)
            && sessionRecoveryRemainingMs(remoteSessionId) <= 0)) return false;
    if (!m_resumeRequestIds.contains(remoteSessionId))
        m_resumeRequestIds.insert(remoteSessionId, QUuid::createUuid().toString(QUuid::WithoutBraces));
    QJsonObject message{
        {QStringLiteral("type"), QStringLiteral("remote_session_resume")},
        {QStringLiteral("requestId"), m_resumeRequestIds.value(remoteSessionId)},
        {QStringLiteral("remoteSessionId"), binding.remoteSessionId},
        {QStringLiteral("generation"), static_cast<double>(binding.generation)},
        {QStringLiteral("resumeToken"), binding.resumeToken}
    };
    return sendTrackedControl(message);
}

void WebSocketClient::resumeAllRemoteSessions()
{
    if (!m_sceneRuns) return;
    // A client can have several outgoing and incoming sessions. Do not derive
    // this list from discovery: an offline project may still own
    // a resumable session during the bounded session recovery window.
    const QList<SceneRunCoordinator::SessionBinding> bindings = m_sceneRuns->sessions();
    for (const SceneRunCoordinator::SessionBinding& binding : bindings) {
        if (!binding.resumeToken.isEmpty()) resumeRemoteSession(binding.remoteSessionId);
    }
}

qint64 WebSocketClient::sessionRecoveryRemainingMs(const QString& remoteSessionId) const
{
    const auto it = m_sessionDeadlines.constFind(remoteSessionId);
    if (it == m_sessionDeadlines.cend()) {
        const auto binding = remoteSessionCoordinator()->byId(remoteSessionId);
        // Versionless coordinator fixtures may use the transport clock. Every
        // real v6 session needs its own installed proof before authorizing work.
        return !binding.remoteSessionId.isEmpty() && binding.stateRevision == 0
            ? leaseRemainingMs() : 0;
    }
    if (it->expired) return 0;
    const qint64 now = suspendInclusiveNowMs();
    return now >= 0 ? std::max<qint64>(0, it->localDeadlineMs - now) : 0;
}

bool WebSocketClient::isSessionRecovering(const QString& remoteSessionId) const
{
    const auto it = m_sessionDeadlines.constFind(remoteSessionId);
    return it != m_sessionDeadlines.cend() && !it->expired
        && it->interruptionDeadlineMs >= 0 && sessionRecoveryRemainingMs(remoteSessionId) > 0;
}

bool WebSocketClient::canIssueSessionCommands(const QString& remoteSessionId) const
{
    if (!isConnected() || m_endpointDraining || m_degraded || !hasUnexpiredLease()
        || sessionRecoveryRemainingMs(remoteSessionId) <= 0 || !m_sceneRuns) return false;
    const auto binding = m_sceneRuns->sessionById(remoteSessionId);
    if (!binding.active || !binding.commandReady) return false;
    const quint64 localGeneration = binding.ownerEndpointId == m_endpointId
        ? binding.ownerConnectionGeneration : binding.targetConnectionGeneration;
    return localGeneration == m_connectionGeneration;
}

bool WebSocketClient::reconcileRemoteSessions()
{
    if (!isConnected() || m_endpointDraining) return false;
    if (!m_reconcileRequestId.isEmpty() && m_pendingControl.contains(m_reconcileRequestId)) return true;
    const qint64 now = suspendInclusiveNowMs();
    if (m_reconcileSentAtMs >= 0 && now - m_reconcileSentAtMs < AppConfig::instance().controlRequestRetryMs()) return false;
    if (m_reconcileRequestId.isEmpty())
        m_reconcileRequestId = QUuid::createUuid().toString(QUuid::WithoutBraces);
    QJsonArray sessions;
    for (const auto& binding : remoteSessionCoordinator()->all()) {
        sessions.append(QJsonObject{
            {QStringLiteral("remoteSessionId"), binding.remoteSessionId},
            {QStringLiteral("generation"), static_cast<double>(binding.generation)},
            {QStringLiteral("stateRevision"), static_cast<double>(binding.stateRevision)}
        });
    }
    if (!sendTrackedControl(QJsonObject{
        {QStringLiteral("type"), QStringLiteral("remote_session_reconcile")},
        {QStringLiteral("requestId"), m_reconcileRequestId},
        {QStringLiteral("sessions"), sessions}
    })) return false;
    m_reconcileSentAtMs = now;
    return true;
}

bool WebSocketClient::acknowledgeSessionState(const QJsonObject& envelope)
{
    quint64 generation = 0, revision = 0;
    if (!readPositiveSafeJsonInteger(envelope.value(QStringLiteral("generation")), &generation)
        || !readPositiveSafeJsonInteger(envelope.value(QStringLiteral("stateRevision")), &revision)) return false;
    return sendControlMessage(QJsonObject{
        {QStringLiteral("type"), QStringLiteral("remote_session_state_ack")},
        {QStringLiteral("remoteSessionId"), envelope.value(QStringLiteral("remoteSessionId"))},
        {QStringLiteral("generation"), static_cast<double>(generation)},
        {QStringLiteral("stateRevision"), static_cast<double>(revision)}
    });
}

qint64 WebSocketClient::sessionProofBudgetMs() const
{
    // Policy 4 measures recovery from interruption detection. A normal proof
    // also covers the two missed heartbeat intervals needed to detect silence.
    return std::max(1, m_sessionRecoveryTimeoutMs > 0
        ? m_sessionRecoveryTimeoutMs : m_leaseTimeoutMs)
        + (m_serverPolicy.value(QStringLiteral("policyVersion")).toInt() >= 4
            ? m_transportSuspectAfterMs : 0);
}

void WebSocketClient::beginSessionRecovery(qint64 detectedAtMs)
{
    if (m_endpointDraining || !m_hasEstablishedLease || m_leaseExpired) return;
    for (auto it = m_sessionDeadlines.begin(); it != m_sessionDeadlines.end(); ++it) {
        if (it->expired) continue;
        const auto binding = remoteSessionCoordinator()->byId(it.key());
        if (binding.phase != QLatin1String("Active") && binding.phase != QLatin1String("Grace")) continue;
        const qint64 deadline = std::min(it->localDeadlineMs,
            detectedAtMs + m_sessionRecoveryTimeoutMs);
        it->interruptionDeadlineMs = it->interruptionDeadlineMs < 0
            ? deadline : std::min(it->interruptionDeadlineMs, deadline);
        it->localDeadlineMs = std::min(it->localDeadlineMs, it->interruptionDeadlineMs);
    }
}

void WebSocketClient::updateSessionDeadline(const QJsonObject& envelope)
{
    const QString id = envelope.value(QStringLiteral("remoteSessionId")).toString();
    const QString phase = envelope.value(QStringLiteral("phase")).toString();
    if (phase == QLatin1String("Terminating") || phase == QLatin1String("CleanupPending")
        || phase == QLatin1String("Closed")) {
        m_sessionDeadlines.remove(id);
        return;
    }
    const qint64 deadline = boundedInteger(
        envelope.value(QStringLiteral("validUntilServerMonotonicMs")), 0, 9007199254740991LL);
    if (id.isEmpty() || deadline < 0 || m_localClockAnchorMs < 0 || m_serverClockAnchorMs < 0) return;
    auto& tracked = m_sessionDeadlines[id];
    if (tracked.expired) return;
    const qint64 now = suspendInclusiveNowMs();
    if (tracked.localDeadlineMs >= 0 && now >= tracked.localDeadlineMs) {
        checkSessionRecoveryDeadlines();
        return;
    }
    const qint64 estimatedServerNow = m_serverClockAnchorMs + now - m_localClockAnchorMs;
    const qint64 remaining = std::clamp<qint64>(deadline - estimatedServerNow, 0,
        sessionProofBudgetMs());
    const qint64 localDeadline = now + remaining;
    const bool recovering = envelope.value(QStringLiteral("degraded")).toBool()
        || phase == QLatin1String("Grace")
        || envelope.value(QStringLiteral("state")).toString() == QLatin1String("Degraded");
    if (recovering) {
        tracked.interruptionDeadlineMs = tracked.interruptionDeadlineMs < 0
            ? localDeadline : std::min(tracked.interruptionDeadlineMs, localDeadline);
    } else if (phase == QLatin1String("Active")
               && envelope.value(QStringLiteral("commandReady")).toBool()) {
        // Only the authoritative two-party applied-state barrier completes
        // recovery. A welcome or an unacknowledged RESUME cannot reset it.
        tracked.interruptionDeadlineMs = -1;
    }
    // Preserve the mapping of the largest observed proof independently of the
    // interruption cap. Finishing recovery may restore that SAME normal proof;
    // it must not keep the shorter interruption deadline by accident.
    if (tracked.proofDeadlineMs < 0 || deadline > tracked.serverDeadlineMs) {
        tracked.proofDeadlineMs = localDeadline;
        tracked.serverDeadlineMs = deadline;
    } else if (deadline == tracked.serverDeadlineMs) {
        tracked.proofDeadlineMs = std::min(tracked.proofDeadlineMs, localDeadline);
    }
    tracked.localDeadlineMs = std::min(localDeadline, tracked.proofDeadlineMs);
    if (tracked.interruptionDeadlineMs >= 0)
        tracked.localDeadlineMs = std::min(tracked.localDeadlineMs, tracked.interruptionDeadlineMs);
}

void WebSocketClient::checkSessionRecoveryDeadlines()
{
    if (!m_sceneRuns) return;
    const qint64 now = suspendInclusiveNowMs();
    for (const QString& id : m_sessionDeadlines.keys()) {
        auto it = m_sessionDeadlines.find(id);
        if (it == m_sessionDeadlines.end()
            || (!it->expired && now >= 0 && now < it->localDeadlineMs)) continue;
        const auto binding = m_sceneRuns->sessionById(id);
        if (binding.remoteSessionId.isEmpty()) continue;
        if (!it->expired) {
            it->expired = true;
            remoteSessionCoordinator()->suspend(id);
            emit remoteSessionRecoveryExpired(id, binding.generation);
        }
        retryExpiredSessionClose(id, binding.generation);
    }
}

void WebSocketClient::retryExpiredSessionClose(const QString& id, quint64 generation)
{
    if (!isConnected() || !hasUnexpiredLease() || generation == 0) return;
    QString requestId;
    closeRemoteSessionByIdentity(id, generation, &requestId, QStringLiteral("session_recovery_expired"));
}

void WebSocketClient::refreshSessionProofs(const QJsonObject& heartbeat)
{
    const QJsonValue states = heartbeat.value(QStringLiteral("sessionStates"));
    if (!states.isArray() || states.toArray().size() > 4096) return;
    bool reconcile = false;
    for (const QJsonValue& value : states.toArray()) {
        if (!value.isObject()) continue;
        const QJsonObject state = value.toObject();
        const QString id = state.value(QStringLiteral("remoteSessionId")).toString();
        const auto binding = remoteSessionCoordinator()->byId(id);
        quint64 generation = 0, revision = 0, ownerTransport = 0, targetTransport = 0;
        if (!readPositiveSafeJsonInteger(state.value(QStringLiteral("generation")), &generation)
            || !readPositiveSafeJsonInteger(state.value(QStringLiteral("stateRevision")), &revision)) continue;
        if (binding.remoteSessionId.isEmpty() || generation != binding.generation
            || !readPositiveSafeJsonInteger(state.value(QStringLiteral("ownerConnectionGeneration")), &ownerTransport)
            || !readPositiveSafeJsonInteger(state.value(QStringLiteral("targetConnectionGeneration")), &targetTransport)
            || ownerTransport != binding.ownerConnectionGeneration || targetTransport != binding.targetConnectionGeneration
            || state.value(QStringLiteral("ownerEndpointId")).toString() != binding.ownerEndpointId
            || state.value(QStringLiteral("targetEndpointId")).toString() != binding.targetEndpointId
            || revision != binding.stateRevision || state.value(QStringLiteral("phase")).toString() != binding.phase
            || state.value(QStringLiteral("commandReady")).toBool() != binding.commandReady
            || (state.value(QStringLiteral("degraded")).toBool()
                || state.value(QStringLiteral("state")).toString() == QLatin1String("Degraded")) != binding.degraded) {
            reconcile = true;
            continue;
        }
        const quint64 localGeneration = binding.ownerEndpointId == m_endpointId
            ? binding.ownerConnectionGeneration : binding.targetConnectionGeneration;
        if (localGeneration != m_connectionGeneration) continue;
        updateSessionDeadline(state);
        // A lost applied-state ACK must not leave both healthy transports
        // permanently waiting. The heartbeat repeats authoritative state, so
        // retry its idempotent ACK until the two-party barrier is confirmed.
        if (!binding.commandReady && binding.phase == QLatin1String("Active")
            && !m_sessionDeadlines.value(id).expired) acknowledgeSessionState(state);
    }
    if (reconcile || !m_reconcileRequestId.isEmpty()) reconcileRemoteSessions();
}

bool WebSocketClient::beginEndpointDisable(const QString& requestId)
{
    if (!isConnected() || m_endpointDraining) return false;
    const QString correlation = requestId.isEmpty()
        ? QUuid::createUuid().toString(QUuid::WithoutBraces) : requestId;
    if (!isUploadOpaqueId(correlation)) return false;
    if (!sendTrackedControl(QJsonObject{
            {QStringLiteral("type"), QStringLiteral("endpoint_disable")},
            {QStringLiteral("requestId"), correlation}
        })) {
        return false;
    }
    for (const QString& id : m_pendingControl.keys()) {
        const QString type = m_pendingControl.value(id).message.value(QStringLiteral("type")).toString();
        if (type != QLatin1String("endpoint_disable") && type != QLatin1String("remote_session_close")) completeControlRequest(id);
    }
    closeUploadChannel();
    m_endpointDraining = true;
    m_endpointDisableRequestId = correlation;
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
    for (const QString& id : m_pendingControl.keys()) {
        const auto pending = m_pendingControl.value(id).message;
        if (pending.value(QStringLiteral("type")).toString() != QLatin1String("remote_session_close")
            || pending.value(QStringLiteral("remoteSessionId")).toString() != remoteSessionId) continue;
        if (pending.value(QStringLiteral("generation")).toInteger() == static_cast<qint64>(generation)) {
            if (requestId) *requestId = id;
            return true;
        }
        completeControlRequest(id);
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
    if (!sendTrackedControl(message)) return false;
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
    const auto outgoing = remoteSessionCoordinator()->outgoingForPeer(targetEndpointId);
    if (!canIssueSessionCommands(outgoing.remoteSessionId)) {
        if (errorMessage) *errorMessage = QStringLiteral("Remote session is recovering");
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
    if (m_endpointDraining) {
        abortConnectionAttempt();
        return;
    }
    qDebug() << "Control transport connected; waiting for signed authentication challenge";
    m_authenticated = false;
    resetSceneClockEstimate();
    setConnectionStatus("Authenticating...");
    emit transportConnected();
}

void WebSocketClient::onDisconnected() {
    const qint64 now = suspendInclusiveNowMs();
    const qint64 detectedAt = m_lastServerContactContinuousMs >= 0
        ? std::min(now, m_lastServerContactContinuousMs + m_transportSuspectAfterMs) : now;
    beginSessionRecovery(detectedAt);
    checkSessionRecoveryDeadlines();
    if (!m_endpointDraining && !m_degraded) {
        m_degraded = true;
        emit transportHealthChanged(true);
    }
    clearControlRequests();
    m_deviceSnapshotRetryTimer.stop();
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
    if (m_endpointDraining && !m_authenticated) return;
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
            // A TLS listener or its certificate configuration can recover
            // while this client remains enabled. Reject this handshake, but
            // keep retrying with normal certificate verification on each attempt.
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
    const qint64 now = suspendInclusiveNowMs();
    const qint64 lag = m_previousLeaseCheckMs >= 0
        ? now - m_previousLeaseCheckMs - m_leaseHealthTimer->interval() : 0;
    m_previousLeaseCheckMs = now;
    if (lag > 250) qWarning() << "Network event loop delayed" << "lagMs" << lag;
    checkSessionRecoveryDeadlines();
    // A replacement transport authenticates independently of its predecessor.
    // Retained session deadlines still run, but the old transport watchdog
    // must never abort this new socket before welcome establishes its lease.
    if (!m_authenticated || m_leaseTimeoutMs <= 0 || !m_hasEstablishedLease || m_leaseExpired) return;
    const qint64 elapsed = leaseElapsedMs();
    if (elapsed >= m_leaseTimeoutMs) {
        beginSessionRecovery(m_lastServerContactContinuousMs + m_transportSuspectAfterMs);
        if (!m_degraded) {
            m_degraded = true;
            emit transportHealthChanged(true);
        }
        if (isTransportConnected()) {
            setConnectionStatus("Disconnected");
            abortConnectionAttempt();
        }
        return;
    }
    const bool degraded = elapsed >= (m_transportSuspectAfterMs > 0
        ? m_transportSuspectAfterMs : std::max(1, m_leaseTimeoutMs / 2));
    if (m_degraded != degraded) {
        m_degraded = degraded;
        emit transportHealthChanged(degraded);
        if (isConnected()) setConnectionStatus(degraded ? "Degraded" : "Connected");
    }
}

bool WebSocketClient::handleAuthChallenge(const QJsonObject& message) {
    if (m_endpointDraining || m_authenticated || !m_pendingServerBootId.isEmpty()
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

    const QByteArray payload = QStringLiteral("mouffette-v%1\n%2\n%3\n%4\n%5\n%6")
        .arg(ProtocolVersion)
        .arg(serverBootId, nonce, m_runtimeId, m_instanceId)
        .arg(m_instanceOrdinal)
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
    response["instanceOrdinal"] = m_instanceOrdinal;
    response["installationId"] = m_installationId;
    response["publicKey"] = base64UrlEncode(m_identityStore->publicKeyDer());
    response["signature"] = base64UrlEncode(signature);
    m_authenticationSentAtMs = suspendInclusiveNowMs();
    return sendRawControlMessage(response);
}

bool WebSocketClient::validateServerPolicy(const QJsonObject& policy,
                                           QString* errorMessage) const {
    struct Rule { const char* name; qint64 minimum; qint64 maximum; };
    static constexpr Rule rules[] = {
        {"policyVersion", 1, 1000000},
        {"heartbeatIntervalMs", 250, 5000},
        {"leaseTimeoutMs", 500, 30000},
        {"transportSuspectAfterMs", 250, 30000},
        {"sessionRecoveryTimeoutMs", 1000, 300000},
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
    const bool interruptionPolicy = values.value("policyVersion") >= 4;
    const bool invalidTiming = interruptionPolicy
        ? (values.value("leaseTimeoutMs") != values.value("heartbeatIntervalMs") * 2
           || values.value("transportSuspectAfterMs") != values.value("leaseTimeoutMs"))
        : (values.value("sessionRecoveryTimeoutMs") < values.value("leaseTimeoutMs")
           || values.value("transportSuspectAfterMs") >= values.value("leaseTimeoutMs")
           || values.value("leaseTimeoutMs") < values.value("heartbeatIntervalMs") * 4);
    if (invalidTiming
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
    if (m_endpointDraining || !isTransportConnected() || m_pendingServerBootId.isEmpty()) return false;
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
        || boundedInteger(message.value("instanceOrdinal"), 1,
                          std::numeric_limits<int>::max()) != m_instanceOrdinal
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
        m_clientListRevision = 0;
        m_assetRemovalObligations.clear();
        expireLease();
        emit serverRestarted(previousBootId, newBootId);
    } else {
        checkSessionRecoveryDeadlines();
    }
    if (!previousBootId.isEmpty() && previousBootId == newBootId
        && previousGeneration > 0 && newGeneration <= previousGeneration) {
        emit fatalError(QStringLiteral("Server returned a stale connection generation"));
        abortConnectionAttempt();
        return false;
    }

    // A new authenticated transport must advertise once, including when a
    // restarted server reuses connection generation 1.
    clearControlRequests();
    m_publishedEndpointSnapshot = {};
    m_publishedEndpointGeneration = 0;
    m_publishedDeviceSnapshots.clear();
    m_deviceSnapshotRetryTimer.stop();
    m_serverBootId = newBootId;
    m_pendingServerBootId.clear();
    m_connectionGeneration = newGeneration;
    m_resumeRequestIds.clear();
    m_reconcileRequestId.clear();
    m_reconcileSentAtMs = -1;
    // Bound the unknown one-way delay by the whole authentication round trip.
    // Session expiry must never be extended by network transit or a clock fit.
    m_serverClockAnchorMs = serverMonotonicMs
        + (m_authenticationSentAtMs >= 0
           ? std::max<qint64>(0, suspendInclusiveNowMs() - m_authenticationSentAtMs) : 0);
    m_localClockAnchorMs = suspendInclusiveNowMs();
    m_socketClientId = message.value("connectionId").toString();
    m_serverPolicy = policy;
    m_heartbeatIntervalMs = policy.value("heartbeatIntervalMs").toInt();
    m_leaseTimeoutMs = policy.value("leaseTimeoutMs").toInt();
    m_sessionRecoveryTimeoutMs = policy.value("sessionRecoveryTimeoutMs").toInt();
    m_transportSuspectAfterMs = policy.value("transportSuspectAfterMs").toInt();
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
    if (sameBoot && previousGeneration > 0) {
        if (withinLease) emit reauthenticatedWithinLease(previousGeneration, newGeneration);
        // The transport lease is deliberately shorter than session recovery.
        // Each binding retains its own immutable proof deadline across welcome.
        QTimer::singleShot(0, this, &WebSocketClient::resumeAllRemoteSessions);
    }
    QTimer::singleShot(0, this, &WebSocketClient::reconcileRemoteSessions);
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
    m_sessionDeadlines.clear();
    m_resumeRequestIds.clear();
    m_reconcileRequestId.clear();
}

void WebSocketClient::handleMessage(const QJsonObject& message) {
    if (m_endpointDraining && !m_authenticated) return;
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
        qWarning() << "Rejected message without a canonical messageId";
        return;
    }
    if (isRemovedWireType(type) || containsRemovedWireField(message)) {
        qWarning() << "Rejected removed protocol message type or field" << type;
        return;
    }
    checkSessionRecoveryDeadlines();
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
        && type != "heartbeat_ack" && type != "remote_session_cursor") {
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
            m_serverClockAnchorMs = serverAt + rtt;
            m_localClockAnchorMs = suspendInclusiveNowMs();
            refreshSessionProofs(message);
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
            QJsonObject remoteError = message;
            remoteError.remove(QStringLiteral("identityValid"));
            completeControlRequest(message.value(QStringLiteral("requestId")).toString());
            emit remoteSessionError(remoteError);
            return;
        }
        const QString err = message.value("message").toString();
        qWarning() << "❌ Server error:" << err;

        // For other errors, emit signal so UI can show the error to user
        emit connectionError(err);
    }
    else if (type == "endpoint_snapshot_applied") {
        const QString requestId = message.value(QStringLiteral("requestId")).toString();
        if (!requestId.isEmpty() && requestId != m_registrationRequestId) return;
        const QJsonObject clientInfoObj = message["snapshot"].toObject();
        ClientInfo clientInfo = ClientInfo::fromJson(clientInfoObj);
        if (clientInfo.installationId() != m_installationId
            || clientInfo.endpointId() != m_endpointId
            || clientInfo.instanceId() != m_instanceId
            || boundedInteger(clientInfoObj.value("instanceOrdinal"), 1,
                              std::numeric_limits<int>::max()) != m_instanceOrdinal
            || clientInfo.instanceOrdinal() != m_instanceOrdinal
            || clientInfo.runtimeId() != m_runtimeId) {
            emit fatalError(QStringLiteral("Registration identity does not match authenticated endpoint"));
            abortConnectionAttempt();
            return;
        }
        qDebug() << "Endpoint snapshot applied for endpoint" << m_endpointId
                 << "runtime" << m_runtimeId;
        completeControlRequest(m_registrationRequestId);
        emit registrationConfirmed(clientInfo);
        // Keep the optional high-throughput channel ready before the first
        // upload. Failure is harmless; beginUploadSession() pins the control
        // channel immediately when this fast path is unavailable.
        QTimer::singleShot(0, this, [this]() { ensureUploadChannel(); });
    }
    else if (type == "upload_channel_token") {
        const QString requestId = message.value(QStringLiteral("requestId")).toString();
        if (!m_uploadChannelTokenRequested || m_endpointDraining
            || (!requestId.isEmpty() && requestId != m_uploadTokenRequestId)) return;
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
            failUploadChannelAttempt(QStringLiteral("Server returned an invalid upload channel token"));
        }
    }
    else if (type == "client_list") {
        quint64 revision = 0;
        if (!readPositiveSafeJsonInteger(message.value(QStringLiteral("revision")), &revision)
            || revision <= m_clientListRevision) return;
        if (!message.value("clients").isArray()
            || boundedInteger(message.value("observedAtServerMonotonicMs"),
                              0, 9007199254740991LL) < 0) return;
        const QJsonArray clientsArray = message["clients"].toArray();
        QList<ClientInfo> clients;
        QSet<QString> endpointIds;
        
        for (const auto& clientValue : clientsArray) {
            if (!clientValue.isObject()) return;
            const QJsonObject entry = clientValue.toObject();
            const QString installationId = entry.value("installationId").toString();
            const QString endpointId = entry.value("endpointId").toString();
            const qint64 ordinal = boundedInteger(entry.value("instanceOrdinal"),
                                                  1, std::numeric_limits<int>::max());
            const QString instanceId = ordinal > 0
                ? DeviceIdentityStore::instanceIdForOrdinal(static_cast<int>(ordinal)) : QString();
            const QString status = entry.value("status").toString();
            static const QSet<QString> statuses{
                QStringLiteral("Available"), QStringLiteral("Degraded"),
                QStringLiteral("Reconnecting"), QStringLiteral("Disconnected")};
            static const QSet<QString> reasons{
                QStringLiteral("enabled"), QStringLiteral("transport_suspect"),
                QStringLiteral("transport_lost"), QStringLiteral("disabled"),
                QStringLiteral("offline")};
            if (base64UrlDecode(installationId).size() != 32
                || base64UrlEncode(base64UrlDecode(installationId)) != installationId
                || instanceId.isEmpty() || entry.value("instanceId").toString() != instanceId
                || endpointId != DeviceIdentityStore::endpointIdForInstallation(installationId, instanceId)
                || endpointIds.contains(endpointId)
                || !isCanonicalUuid(entry.value("runtimeId").toString())
                || !statuses.contains(status)
                || !entry.value("canAcceptSession").isBool()
                || (entry.value("canAcceptSession").toBool() && status != QLatin1String("Available"))
                || !reasons.contains(entry.value("reason").toString())
                || boundedInteger(entry.value("lastSeenAt"), 0, 9007199254740991LL) < 0) {
                qWarning() << "Rejected malformed client presence" << "endpointId" << endpointId
                           << "instanceOrdinal" << entry.value("instanceOrdinal");
                return;
            }
            endpointIds.insert(endpointId);
            ClientInfo client = ClientInfo::fromJson(entry);
            clients.append(client);
        }
        // A malformed row must neither erase known peers nor consume its revision.
        m_clientListRevision = revision;
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
            qWarning() << "Rejected stale or malformed upload envelope";
            return;
        }
        if (type == QLatin1String("upload_remove") && binding.targetEndpointId == m_endpointId) {
            const QString removalId = message.value(QStringLiteral("removalId")).toString();
            if (!isCanonicalUuid(removalId)) return;
            const auto rejectInstruction = [this, &message](const QString& reason) {
                QJsonObject response = message;
                response.remove(QStringLiteral("messageId"));
                response.remove(QStringLiteral("ownerEndpointId"));
                response.remove(QStringLiteral("targetEndpointId"));
                response.insert(QStringLiteral("type"), QStringLiteral("upload_removed"));
                response.insert(QStringLiteral("success"), false);
                response.insert(QStringLiteral("cacheQuarantined"), false);
                response.insert(QStringLiteral("result"), QStringLiteral("cleanup_error"));
                response.insert(QStringLiteral("errorCode"), reason);
                response.insert(QStringLiteral("reason"), reason);
                response.insert(QStringLiteral("removedFileCount"), 0);
                response.insert(QStringLiteral("quarantinedBytes"), 0);
                sendControlMessage(response);
            };
            auto obligation = m_assetRemovalObligations.find(removalId);
            if (obligation == m_assetRemovalObligations.end()) {
                // Pending asynchronous work must never be evicted to make room.
                if (m_assetRemovalObligations.size() >= 4096) {
                    rejectInstruction(QStringLiteral("removal_capacity_exceeded"));
                    return;
                }
                QJsonObject accepted = message;
                accepted.insert(QStringLiteral("acceptedGenerations"), QJsonArray{static_cast<double>(generation)});
                m_assetRemovalObligations.insert(removalId, accepted);
            } else {
                static const QStringList immutableFields = {
                    QStringLiteral("remoteSessionId"), QStringLiteral("removalId"),
                    QStringLiteral("uploadId"), QStringLiteral("assetId"),
                    QStringLiteral("offset"), QStringLiteral("size"), QStringLiteral("sha256"),
                    QStringLiteral("fileId"), QStringLiteral("extension"),
                    QStringLiteral("ownerEndpointId"), QStringLiteral("targetEndpointId"),
                    QStringLiteral("serverBootId")
                };
                for (const auto& field : immutableFields) {
                    if (message.value(field) != obligation->value(field)) {
                        rejectInstruction(QStringLiteral("removal_identity_conflict"));
                        return;
                    }
                }
                QJsonArray generations = obligation->value(QStringLiteral("acceptedGenerations")).toArray();
                const QJsonValue dispatchGeneration(static_cast<double>(generation));
                if (!generations.contains(dispatchGeneration)) {
                    if (generations.size() >= 64) {
                        rejectInstruction(QStringLiteral("removal_generation_capacity_exceeded"));
                        return;
                    }
                    generations.append(dispatchGeneration);
                    obligation->insert(QStringLiteral("acceptedGenerations"), generations);
                }
            }
        }
        emit uploadMessageReceived(message);
    }
    else if (type == "remote_session_offer") {
        if (!m_sceneRuns
            || !m_sceneRuns->upsertSession(message, m_connectionGeneration)) {
            ++m_reconciliationFailureSerial;
            qWarning() << "Rejected stale or malformed RemoteSession offer";
            return;
        }
        if (m_endpointDraining) {
            closeRemoteSession(
                message.value(QStringLiteral("remoteSessionId")).toString(),
                nullptr, QStringLiteral("client_disabled"));
            return;
        }
        updateSessionDeadline(message);
        acknowledgeSessionState(message);
        emit localDeviceSnapshotRequested();
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
        const QString sessionId = message.value(QStringLiteral("remoteSessionId")).toString();
        const auto previousBinding = remoteSessionCoordinator()->byId(sessionId);
        if (remoteSessionCoordinator() && remoteSessionCoordinator()->isClosedDuplicate(message)) {
            completeControlRequest(message.value(QStringLiteral("requestId")).toString());
            acknowledgeSessionState(message);
            return;
        }
        if (type != QLatin1String("remote_session_terminating")
            && m_sessionDeadlines.value(sessionId).expired) {
            ++m_reconciliationFailureSerial;
            quint64 generation = 0, localGeneration = 0, revision = 0;
            const QString localGenerationField = previousBinding.ownerEndpointId == m_endpointId
                ? QStringLiteral("ownerConnectionGeneration") : QStringLiteral("targetConnectionGeneration");
            if (message.value(QStringLiteral("ownerEndpointId")).toString() == previousBinding.ownerEndpointId
                && message.value(QStringLiteral("targetEndpointId")).toString() == previousBinding.targetEndpointId
                && readPositiveSafeJsonInteger(message.value(QStringLiteral("generation")), &generation)
                && generation >= previousBinding.generation
                && readPositiveSafeJsonInteger(message.value(QStringLiteral("stateRevision")), &revision)
                && revision >= previousBinding.stateRevision
                && readPositiveSafeJsonInteger(message.value(localGenerationField), &localGeneration)
                && localGeneration == m_connectionGeneration) retryExpiredSessionClose(sessionId, generation);
            reconcileRemoteSessions();
            return;
        }
        QString stateValidationError;
        if (!m_sceneRuns
            || !m_sceneRuns->upsertSession(message, m_connectionGeneration, &stateValidationError)) {
            ++m_reconciliationFailureSerial;
            if (stateValidationError == QLatin1String("invalid_initial_snapshot")) {
                const quint64 generation = static_cast<quint64>(message.value(QStringLiteral("generation")).toDouble());
                closeRemoteSessionByIdentity(sessionId, generation, nullptr, stateValidationError);
                QJsonObject failure{
                    {QStringLiteral("scope"), QStringLiteral("remote_session")},
                    {QStringLiteral("code"), stateValidationError},
                    {QStringLiteral("identityValid"), true},
                    {QStringLiteral("message"), QStringLiteral("The remote client returned an invalid initial snapshot")},
                    {QStringLiteral("requestId"), message.value(QStringLiteral("requestId"))},
                    {QStringLiteral("remoteSessionId"), sessionId},
                    {QStringLiteral("generation"), static_cast<double>(generation)},
                    {QStringLiteral("ownerEndpointId"), message.value(QStringLiteral("ownerEndpointId"))},
                    {QStringLiteral("targetEndpointId"), message.value(QStringLiteral("targetEndpointId"))}
                };
                emit remoteSessionError(failure);
                return;
            }
            qWarning() << "Rejected RemoteSession state" << type << sessionId
                       << "cause" << "identity_or_transition_mismatch"
                       << "expectedGeneration" << previousBinding.generation
                       << "receivedGeneration" << message.value(QStringLiteral("generation"))
                       << "expectedRevision" << previousBinding.stateRevision
                       << "receivedRevision" << message.value(QStringLiteral("stateRevision"));
            reconcileRemoteSessions();
            return;
        }
        updateSessionDeadline(message);
        completeSessionRequests(sessionId, type == QLatin1String("remote_session_terminating"));
        if (type == "remote_session_opening") {
            acknowledgeSessionState(message);
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
                        message, m_connectionGeneration, true))) {
                ++m_reconciliationFailureSerial;
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
            acknowledgeSessionState(message);
            if (previousBinding.phase == QLatin1String("Active")
                && previousBinding.generation == binding.generation
                && previousBinding.stateRevision == binding.stateRevision) return;
            if (!m_targetSnapshotSequenceBySession.contains(binding.remoteSessionId))
                m_targetSnapshotSequenceBySession.insert(binding.remoteSessionId, 1);
            emit localDeviceSnapshotRequested();
            publishDeviceSnapshots();
            emit remoteSessionOpened(message);
        }
        else if (type == "remote_session_resumed") {
            const RemoteSessionCoordinator::Binding binding =
                m_sceneRuns->sessionById(
                    message.value(QStringLiteral("remoteSessionId")).toString());
            if (binding.ownerEndpointId == m_endpointId && binding.active
                && message.contains(QStringLiteral("snapshot"))
                && !remoteSessionCoordinator()->acceptSnapshot(message, m_connectionGeneration, true)) {
                ++m_reconciliationFailureSerial;
                reconcileRemoteSessions();
                return;
            }
            acknowledgeSessionState(message);
            if (binding.targetEndpointId == m_endpointId) {
                emit localDeviceSnapshotRequested();
                publishDeviceSnapshots();
            }
            emit remoteSessionResumed(message);
        }
        else if (type == "remote_session_lease_state") {
            acknowledgeSessionState(message);
            const auto binding = remoteSessionCoordinator()->byId(sessionId);
            if (binding.commandReady && !previousBinding.commandReady
                && binding.targetEndpointId == m_endpointId) {
                emit localDeviceSnapshotRequested();
                publishDeviceSnapshots();
            }
            emit remoteSessionLeaseStateChanged(message);
        } else {
            acknowledgeSessionState(message);
            emit remoteSessionTerminating(message);
        }
        emit messageReceived(message);
    }
    else if (type == "remote_session_closed") {
        if (!m_sceneRuns) return;
        const QString sessionId = message.value(QStringLiteral("remoteSessionId")).toString();
        if (remoteSessionCoordinator()->isClosedDuplicate(message)) {
            completeControlRequest(message.value(QStringLiteral("requestId")).toString());
            completeSessionRequests(sessionId, true, true);
            acknowledgeSessionState(message);
            return;
        }
        const QString cleanup = message.value(QStringLiteral("cleanupState")).toString();
        if (cleanup == QLatin1String("pending") || cleanup == QLatin1String("error")) {
            QJsonObject terminal = message;
            terminal.insert(QStringLiteral("type"), QStringLiteral("remote_session_terminating"));
            terminal.insert(QStringLiteral("phase"), QStringLiteral("CleanupPending"));
            if (!m_sceneRuns->upsertSession(terminal, m_connectionGeneration)) {
                ++m_reconciliationFailureSerial;
                reconcileRemoteSessions();
                return;
            }
            completeSessionRequests(sessionId, true);
            m_sessionDeadlines.remove(sessionId);
            emit remoteSessionTerminating(terminal);
            emit remoteSessionLogicallyClosed(message);
            acknowledgeSessionState(message);
            return;
        }
        if (!m_sceneRuns->removeSession(message, m_connectionGeneration)) {
            ++m_reconciliationFailureSerial;
            qWarning() << "Rejected RemoteSession close" << sessionId << cleanup;
            reconcileRemoteSessions();
            return;
        }
        acknowledgeSessionState(message);
        m_sessionDeadlines.remove(sessionId);
        m_resumeRequestIds.remove(sessionId);
        emit remoteSessionClosed(message);
        m_targetSnapshotSequenceBySession.remove(sessionId);
        m_receivedCursorSequenceBySession.remove(sessionId);
        emit messageReceived(message);
    }
    else if (type == "remote_session_reconciled") {
        if (m_reconcileRequestId.isEmpty()
            || message.value(QStringLiteral("requestId")).toString() != m_reconcileRequestId
            || !message.value(QStringLiteral("complete")).toBool()
            || !message.value(QStringLiteral("sessions")).isArray()
            || message.value(QStringLiteral("sessions")).toArray().size() > 4096
            || message.value(QStringLiteral("absentSessionIds")).toArray().size() > 4096) return;
        completeControlRequest(m_reconcileRequestId);
        m_reconcileRequestId.clear();
        const quint64 failureSerial = m_reconciliationFailureSerial;
        const QJsonArray states = message.value(QStringLiteral("sessions")).toArray();
        for (const QJsonValue& value : states) {
            if (!value.isObject()) { ++m_reconciliationFailureSerial; continue; }
            QJsonObject state = value.toObject();
            const QString nestedType = state.value(QStringLiteral("type")).toString();
            if (nestedType == QLatin1String("remote_session_reconciled")) { ++m_reconciliationFailureSerial; continue; }
            if (nestedType != QLatin1String("remote_session_opened")
                && nestedType != QLatin1String("remote_session_opening")
                && nestedType != QLatin1String("remote_session_offer")
                && nestedType != QLatin1String("remote_session_resumed")
                && nestedType != QLatin1String("remote_session_lease_state")
                && nestedType != QLatin1String("remote_session_terminating")
                && nestedType != QLatin1String("remote_session_closed")) { ++m_reconciliationFailureSerial; continue; }
            state.insert(QStringLiteral("reconciled"), true);
            state.insert(QStringLiteral("protocolVersion"), ProtocolVersion);
            state.insert(QStringLiteral("serverBootId"), m_serverBootId);
            if (!isCanonicalUuid(state.value(QStringLiteral("messageId")).toString()))
                state.insert(QStringLiteral("messageId"), QUuid::createUuid().toString(QUuid::WithoutBraces));
            handleMessage(state);
        }
        for (const QJsonValue& value : message.value(QStringLiteral("absentSessionIds")).toArray()) {
            const QString id = value.toString();
            const auto binding = remoteSessionCoordinator()->byId(id);
            if (binding.remoteSessionId.isEmpty()) continue;
            emit remoteSessionAbsent(id, binding.generation);
            discardRemoteSessionAfterAuthoritativeRejection(id);
            m_sessionDeadlines.remove(id);
            m_resumeRequestIds.remove(id);
        }
        if (failureSerial == m_reconciliationFailureSerial && m_reconcileRequestId.isEmpty()) {
            emit reconciliationCompleted();
        } else {
            if (m_reconcileRequestId.isEmpty())
                m_reconcileRequestId = QUuid::createUuid().toString(QUuid::WithoutBraces);
            m_controlRetries.schedule(QStringLiteral("reconcile-next"), AppConfig::instance().controlRequestRetryMs(),
                [this] { reconcileRemoteSessions(); });
        }
    }
    else if (type == "endpoint_disable_started") {
        if (!m_endpointDraining) {
            qWarning() << "Rejected unsolicited endpoint disable acknowledgement";
            return;
        }
        quint64 acknowledgedGeneration = 0;
        if (message.value(QStringLiteral("requestId")).toString() != m_endpointDisableRequestId
            || !readPositiveSafeJsonInteger(message.value(QStringLiteral("connectionGeneration")), &acknowledgedGeneration)
            || acknowledgedGeneration != m_connectionGeneration) return;
        completeControlRequest(m_endpointDisableRequestId);
        emit endpointDisableAcknowledged(m_endpointDisableRequestId, acknowledgedGeneration);
    }
    else if (type == "scene_prepare" || type == "prepare_progress"
             || type == "prepared" || type == "armed" || type == "commit"
             || type == "started" || type == "state_snapshot"
             || type == "stop" || type == "stopped") {
        if (type != QLatin1String("stop") && type != QLatin1String("stopped")
            && !canIssueSessionCommands(message.value(QStringLiteral("remoteSessionId")).toString())) return;
        QString validationError;
        if (!m_sceneRuns || !m_sceneRuns->acceptInboundEnvelope(message, &validationError)) {
            qWarning() << "Rejected scene message:" << validationError;
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
    const QString sessionId = message.value(QStringLiteral("remoteSessionId")).toString();
    const QString commandType = message.value(QStringLiteral("type")).toString();
    static const QSet<QString> sessionCommands = {
        QStringLiteral("upload_start"), QStringLiteral("upload_resume"), QStringLiteral("upload_chunk"),
        QStringLiteral("upload_complete"), QStringLiteral("scene_prepare"), QStringLiteral("armed"),
        QStringLiteral("prepared"), QStringLiteral("prepare_progress"), QStringLiteral("started"),
        QStringLiteral("state_snapshot"), QStringLiteral("remote_session_cursor"),
        QStringLiteral("remote_session_snapshot"), QStringLiteral("media_residency")
    };
    if (!sessionId.isEmpty() && sessionCommands.contains(commandType)
        && !canIssueSessionCommands(sessionId)) return false;
    if (containsRemovedWireField(message)) {
        qWarning() << "Refusing message containing a removed wire field";
        return false;
    }

    return sendRawControlMessage(addProtocolEnvelope(message));
}

bool WebSocketClient::sendMessageUpload(const QJsonObject& message) {
    const QString sessionId = message.value(QStringLiteral("remoteSessionId")).toString();
    if (!sessionId.isEmpty() && !canIssueSessionCommands(sessionId)) return false;
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
