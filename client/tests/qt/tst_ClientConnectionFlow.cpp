#include <QtTest>
#include <QDateTime>
#include <QDir>
#include <QEvent>
#include <QFile>
#include <QHostAddress>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QImage>
#include <QPointer>
#include <QProcess>
#include <QPromise>
#include <QApplication>
#include <QQmlApplicationEngine>
#include <QQuickStyle>
#include <QScopeGuard>
#include <QThreadPool>
#include <QTemporaryDir>
#include <QTimer>
#include <QUuid>
#include <QWebSocket>
#include <QWebSocketServer>

#include "backend/runtime/ApplicationRuntime.h"
#include "backend/media/MediaBackendBootstrap.h"
#include "backend/media/MediaResidencyManager.h"
#include "backend/runtime/ApplicationActivityMonitor.h"
#include "backend/runtime/RuntimeProfile.h"
#include "backend/runtime/RuntimeStorageBootstrap.h"
#include "backend/runtime/storage/StorageRegistry.h"
#include "backend/domain/project/ProjectStore.h"
#include "backend/domain/canvas/CanvasDocument.h"
#include "backend/domain/media/CanvasMedia.h"
#include "backend/domain/models/ClientInfo.h"
#include "backend/domain/project/ProjectManager.h"
#include "backend/managers/network/ConnectionManager.h"
#include "backend/managers/network/ClientListBuilder.h"
#include "backend/network/RemoteSessionCoordinator.h"
#include "backend/network/WebSocketClient.h"
#include "backend/network/UploadManager.h"
#include "backend/handlers/UploadEventHandler.h"
#include "backend/files/FileManager.h"
#include "backend/security/DeviceIdentityStore.h"
#include "frontend/qml/ApplicationController.h"
#include "frontend/qml/QmlRuntime.h"
#include "frontend/rendering/canvas/CanvasQmlTypes.h"
#include "frontend/qml/ClientListModel.h"
#include "frontend/qml/ClientWorkspaceViewModel.h"
#include "frontend/rendering/navigation/ScreenNavigationManager.h"
#include "frontend/rendering/remote/RemoteSceneController.h"
#include "shared/rendering/ICanvasHost.h"


// The fake peer speaks the same versioned state/proof protocol as production.
// Keep authoritative revisions and renew only Active sessions in heartbeat ACKs.
static void completeV7TestEnvelope(QJsonObject& message)
{
    static QHash<QString, quint64> revisions;
    static QHash<QString, QJsonObject> sessions;
    static quint64 proof = 60'000'000;
    const QString boot = message.value(QStringLiteral("serverBootId")).toString();
    const QString type = message.value(QStringLiteral("type")).toString();
    if (type == QLatin1String("client_list")) {
        if (!message.contains(QStringLiteral("revision")))
            message.insert(QStringLiteral("revision"), static_cast<qint64>(++revisions[boot]));
        message.insert(QStringLiteral("observedAtServerMonotonicMs"), 0);
        QJsonArray clients;
        for (const QJsonValue& value : message.value("clients").toArray()) {
            QJsonObject entry = value.toObject();
            const QString status = entry.value("status").toString();
            entry.insert("reason", status == QLatin1String("Available") ? "enabled"
                : status == QLatin1String("Degraded") ? "transport_suspect"
                : status == QLatin1String("Reconnecting") ? "transport_lost" : "offline");
            entry.insert("lastSeenAt", 1);
            clients.append(entry);
        }
        message.insert("clients", clients);
    }
    const QString id = message.value(QStringLiteral("remoteSessionId")).toString();
    const QString key = boot + QLatin1Char(':') + id;
    const QString phase = message.value(QStringLiteral("phase")).toString();
    if (!id.isEmpty() && !phase.isEmpty()) {
        if (!message.contains(QStringLiteral("commandReady")))
            message.insert(QStringLiteral("commandReady"), phase == QLatin1String("Active"));
        if (!message.contains(QStringLiteral("stateRevision")))
            message.insert(QStringLiteral("stateRevision"), static_cast<qint64>(++revisions[key]));
        if (!message.contains(QStringLiteral("validUntilServerMonotonicMs")))
            message.insert(QStringLiteral("validUntilServerMonotonicMs"), static_cast<qint64>(++proof));
        if (phase == QLatin1String("Closed") || phase == QLatin1String("Terminating")
            || phase == QLatin1String("CleanupPending")) sessions.remove(key);
        else sessions.insert(key, message);
    }
    if (type == QLatin1String("heartbeat_ack")) {
        QJsonArray states;
        for (auto it = sessions.begin(); it != sessions.end(); ++it) {
            if (!it.key().startsWith(boot + QLatin1Char(':'))) continue;
            if (it->value(QStringLiteral("phase")) == QLatin1String("Active"))
                it->insert(QStringLiteral("validUntilServerMonotonicMs"), static_cast<qint64>(++proof));
            states.append(it.value());
        }
        message.insert(QStringLiteral("sessionStates"), states);
    }
}

namespace {
QString fixtureInstallationId()
{
    return QString::fromLatin1(QByteArray(32, 'i').toBase64(
        QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals));
}

QHash<QString, int> fixtureOrdinals;
QString fixtureEndpoint(QChar label)
{
    const int ordinal = label.unicode();
    const QString endpointId = DeviceIdentityStore::endpointIdForInstallation(
        fixtureInstallationId(), DeviceIdentityStore::instanceIdForOrdinal(ordinal));
    fixtureOrdinals.insert(endpointId, ordinal);
    return endpointId;
}

ClientInfo onlineClient(const QString& endpointId, const QString& machineName)
{
    ClientInfo client(endpointId, machineName, QStringLiteral("Windows"));
    client.setEndpointId(endpointId);
    client.setInstallationId(fixtureInstallationId());
    client.setInstanceOrdinal(fixtureOrdinals.value(endpointId, 1));
    client.setInstanceId(DeviceIdentityStore::instanceIdForOrdinal(client.instanceOrdinal()));
    client.setRuntimeId(QStringLiteral("123e4567-e89b-42d3-a456-426614174000"));
    client.setOnline(true);
    client.setStatus(QStringLiteral("Available"));
    client.setAvailabilityStatus(QStringLiteral("Available"));
    client.setScreens({ScreenInfo(0, 1920, 1080, 0, 0, true)});
    return client;
}

class RemoteSessionTestServer final : public QObject
{
public:
    explicit RemoteSessionTestServer(QString ownerEndpointId,
                                     QObject* parent = nullptr)
        : QObject(parent)
        , server(QStringLiteral("client-connection-regression-test"),
                 QWebSocketServer::NonSecureMode, this)
        , ownerEndpointId(std::move(ownerEndpointId))
        , bootId(QUuid::createUuid().toString(QUuid::WithoutBraces))
    {
        connect(&server, &QWebSocketServer::newConnection,
                this, [this]() { acceptConnection(); });
    }

    bool listen()
    {
        return server.listen(QHostAddress::LocalHost, 0);
    }

    QString url() const
    {
        return QStringLiteral("ws://127.0.0.1:%1").arg(server.serverPort());
    }

    void beginNewBoot()
    {
        bootId = QUuid::createUuid().toString(QUuid::WithoutBraces);
        connectionGeneration = 1;
    }

    bool send(QJsonObject message)
    {
        const QString type = message.value("type").toString();
        static const QSet<QString> dataTypes {"upload_start", "upload_chunk", "upload_complete", "upload_resume",
            "upload_ready", "upload_resume_ready", "upload_progress", "upload_finished"};
        return sendOn(dataTypes.contains(type) ? dataPeer : peer, bootId, std::move(message));
    }

    bool sendOn(QWebSocket* socket, const QString& socketBootId,
                QJsonObject message)
    {
        if (!socket) return false;
        message.insert(QStringLiteral("protocolVersion"), 12);
        message.insert(QStringLiteral("serverBootId"), socketBootId);
        completeV7TestEnvelope(message);
        if (message.value(QStringLiteral("messageId")).toString().isEmpty()) {
            message.insert(
                QStringLiteral("messageId"),
                QUuid::createUuid().toString(QUuid::WithoutBraces));
        }
        return socket->sendTextMessage(QString::fromUtf8(
                   QJsonDocument(message).toJson(QJsonDocument::Compact))) >= 0;
    }

    bool sendClientList(const ClientInfo& client)
    {
        return send(QJsonObject{
            {QStringLiteral("type"), QStringLiteral("client_list")},
            {QStringLiteral("clients"), QJsonArray{client.toJson()}}
        });
    }

    bool sendOpened(const QString& remoteSessionId,
                    const QString& requestId,
                    const QString& targetEndpointId,
                    const ScreenInfo& screen,
                    int volumePercent)
    {
        const QJsonObject snapshot{
            {QStringLiteral("screens"), QJsonArray{screen.toJson()}},
            {QStringLiteral("systemUI"), QJsonArray{}},
            {QStringLiteral("volumePercent"), volumePercent},
            {QStringLiteral("revision"), 1},
            {QStringLiteral("capturedAtEpochMs"),
             static_cast<double>(QDateTime::currentMSecsSinceEpoch())}
        };
        return send(QJsonObject{
            {QStringLiteral("type"), QStringLiteral("remote_session_opened")},
            {QStringLiteral("requestId"), requestId},
            {QStringLiteral("remoteSessionId"), remoteSessionId},
            {QStringLiteral("generation"), 1},
            {QStringLiteral("ownerConnectionGeneration"),
             static_cast<double>(connectionGeneration)},
            {QStringLiteral("targetConnectionGeneration"), 1},
            {QStringLiteral("phase"), QStringLiteral("Active")},
            {QStringLiteral("ownerEndpointId"), ownerEndpointId},
            {QStringLiteral("targetEndpointId"), targetEndpointId},
            {QStringLiteral("resumeToken"),
             QStringLiteral("resume-") + remoteSessionId},
            {QStringLiteral("snapshotSequence"), 1},
            {QStringLiteral("snapshot"), snapshot}
        });
    }

    bool sendResumed(const QString& remoteSessionId,
                     const QString& targetEndpointId,
                     quint64 sessionGeneration,
                     quint64 targetConnectionGeneration,
                     const QString& phase)
    {
        return send(QJsonObject{
            {QStringLiteral("type"), QStringLiteral("remote_session_resumed")},
            {QStringLiteral("remoteSessionId"), remoteSessionId},
            {QStringLiteral("generation"),
             static_cast<double>(sessionGeneration)},
            {QStringLiteral("ownerConnectionGeneration"),
             static_cast<double>(connectionGeneration)},
            {QStringLiteral("targetConnectionGeneration"),
             static_cast<double>(targetConnectionGeneration)},
            {QStringLiteral("phase"), phase},
            {QStringLiteral("ownerEndpointId"), ownerEndpointId},
            {QStringLiteral("targetEndpointId"), targetEndpointId},
            {QStringLiteral("requestStateSnapshot"), false}
        });
    }

    bool sendIncomingOpened(const QString& remoteSessionId,
                            const QString& requestId,
                            const QString& remoteOwnerEndpointId)
    {
        return send(QJsonObject{
            {QStringLiteral("type"), QStringLiteral("remote_session_opened")},
            {QStringLiteral("requestId"), requestId},
            {QStringLiteral("remoteSessionId"), remoteSessionId},
            {QStringLiteral("generation"), 1},
            {QStringLiteral("ownerConnectionGeneration"), 1},
            {QStringLiteral("targetConnectionGeneration"),
             static_cast<double>(connectionGeneration)},
            {QStringLiteral("phase"), QStringLiteral("Active")},
            {QStringLiteral("ownerEndpointId"), remoteOwnerEndpointId},
            {QStringLiteral("targetEndpointId"), ownerEndpointId},
            {QStringLiteral("resumeToken"),
             QStringLiteral("resume-") + remoteSessionId}
        });
    }

    bool sendTerminating(const QString& remoteSessionId,
                         const QString& targetEndpointId)
    {
        return send(QJsonObject{
            {QStringLiteral("type"), QStringLiteral("remote_session_terminating")},
            {QStringLiteral("remoteSessionId"), remoteSessionId},
            {QStringLiteral("generation"), 1},
            {QStringLiteral("ownerConnectionGeneration"),
             static_cast<double>(connectionGeneration)},
            {QStringLiteral("targetConnectionGeneration"), 1},
            {QStringLiteral("phase"), QStringLiteral("CleanupPending")},
            {QStringLiteral("teardownId"),
             QStringLiteral("teardown-") + remoteSessionId},
            {QStringLiteral("ownerEndpointId"), ownerEndpointId},
            {QStringLiteral("targetEndpointId"), targetEndpointId}
        });
    }

    bool sendClosed(const QString& remoteSessionId,
                    const QString& targetEndpointId)
    {
        return send(QJsonObject{
            {QStringLiteral("type"), QStringLiteral("remote_session_closed")},
            {QStringLiteral("remoteSessionId"), remoteSessionId},
            {QStringLiteral("generation"), 1},
            {QStringLiteral("ownerConnectionGeneration"),
             static_cast<double>(connectionGeneration)},
            {QStringLiteral("targetConnectionGeneration"), 1},
            {QStringLiteral("phase"), QStringLiteral("Closed")},
            {QStringLiteral("cleanupState"), QStringLiteral("confirmed")},
            {QStringLiteral("teardownId"),
             QStringLiteral("teardown-") + remoteSessionId},
            {QStringLiteral("ownerEndpointId"), ownerEndpointId},
            {QStringLiteral("targetEndpointId"), targetEndpointId}
        });
    }

    void closePeer()
    {
        if (peer) peer->close();
    }

    QWebSocketServer server;
    QPointer<QWebSocket> peer;
    QPointer<QWebSocket> dataPeer;
    QString ownerEndpointId;
    QString bootId;
    quint64 connectionGeneration = 1;
    int heartbeatIntervalMs = 1'000;
    int leaseTimeoutMs = 10'000;
    int policyVersion = 5;
    int sessionRecoveryTimeoutMs = -1;
    bool acknowledgeHeartbeats = true;
    int acceptedConnections = 0;
    QList<QJsonObject> openCommands;
    QList<QJsonObject> closeCommands;
    QList<QJsonObject> resumeCommands;
    QList<QJsonObject> teardownAcknowledgements;
    QList<QJsonObject> cursorSamples;
    QList<QJsonObject> disableCommands;
    QList<QJsonObject> uploadStarts;
    QList<QJsonObject> uploadAborts;
    bool replyRegistration = true;
    bool replyReconciliation = true;
    QJsonObject registrationReply;
    QJsonObject reconciliationReply;

private:
    void acceptConnection()
    {
        QWebSocket* socket = server.nextPendingConnection();
        if (!socket) return;
        if (socket->requestUrl().hasQuery()) {
            dataPeer = socket;
            socket->setParent(&server);
            connect(socket, &QWebSocket::textMessageReceived, this, [this](const QString& encoded) {
                const QJsonObject message = QJsonDocument::fromJson(encoded.toUtf8()).object();
                if (message.value("type") == "upload_start") uploadStarts.append(message);
                if (message.value("type") == "upload_abort") uploadAborts.append(message);
            });
            sendOn(socket, bootId, {{"type", "upload_channel_ready"}, {"endpointId", ownerEndpointId},
                {"connectionGeneration", double(connectionGeneration)}});
            return;
        }
        peer = socket;
        peer->setParent(&server);
        ++acceptedConnections;
        const QString socketBootId = bootId;
        const quint64 socketGeneration = connectionGeneration;

        connect(peer, &QWebSocket::textMessageReceived,
                this, [this, socket, socketBootId,
                       socketGeneration, authentication = QJsonObject{}](const QString& encoded) mutable {
            const QJsonObject message =
                QJsonDocument::fromJson(encoded.toUtf8()).object();
            const QString type = message.value(QStringLiteral("type")).toString();
            if (type == QLatin1String("auth_response")) {
                authentication = message;
                sendWelcome(socket, socketBootId, socketGeneration, message);
            } else if (type == QLatin1String("heartbeat")) {
                if (!acknowledgeHeartbeats) return;
                sendOn(socket, socketBootId, QJsonObject{
                    {QStringLiteral("type"), QStringLiteral("heartbeat_ack")},
                    {QStringLiteral("connectionGeneration"),
                     static_cast<double>(socketGeneration)},
                    {QStringLiteral("sequence"),
                     message.value(QStringLiteral("sequence"))},
                    {QStringLiteral("clientMonotonicMs"),
                     message.value(QStringLiteral("clientMonotonicMs"))},
                    {QStringLiteral("serverMonotonicMs"),
                     message.value(QStringLiteral("clientMonotonicMs"))},
                    {QStringLiteral("serverEpochMs"),
                     static_cast<double>(QDateTime::currentMSecsSinceEpoch())}
                });
            } else if (type == QLatin1String("endpoint_snapshot")) {
                QJsonObject snapshot = message;
                snapshot.insert(QStringLiteral("installationId"), authentication.value("installationId"));
                snapshot.insert(QStringLiteral("instanceId"), authentication.value("instanceId"));
                snapshot.insert(QStringLiteral("runtimeId"), authentication.value("runtimeId"));
                snapshot.insert(QStringLiteral("endpointId"), ownerEndpointId);
                registrationReply = QJsonObject{
                    {"type", "endpoint_snapshot_applied"}, {"snapshot", snapshot},
                    {"connectionGeneration", static_cast<qint64>(socketGeneration)}
                };
                if (replyRegistration) sendOn(socket, socketBootId, registrationReply);
            } else if (type == QLatin1String("remote_session_reconcile")) {
                reconciliationReply = QJsonObject{
                    {"type", "remote_session_reconciled"}, {"requestId", message.value("requestId")},
                    {"sessions", QJsonArray{}}, {"complete", true}, {"absentSessionIds", QJsonArray{}}
                };
                if (replyReconciliation) sendOn(socket, socketBootId, reconciliationReply);
            } else if (type == QLatin1String("request_upload_channel")) {
                sendOn(socket, socketBootId, {{"type", "upload_channel_token"}, {"token", QString(48, QLatin1Char('d'))},
                    {"connectionGeneration", double(socketGeneration)}, {"expiresAt", double(QDateTime::currentMSecsSinceEpoch() + 10000)},
                    {"requestId", message.value("requestId")}});
            } else if (type == QLatin1String("upload_start")) {
                uploadStarts.append(message);
            } else if (type == QLatin1String("upload_abort")) {
                uploadAborts.append(message);
            } else if (type == QLatin1String("remote_session_open")) {
                openCommands.append(message);
            } else if (type == QLatin1String("remote_session_close")) {
                closeCommands.append(message);
            } else if (type == QLatin1String("remote_session_resume")) {
                resumeCommands.append(message);
            } else if (type
                       == QLatin1String("remote_session_teardown_ack")) {
                teardownAcknowledgements.append(message);
            } else if (type == QLatin1String("endpoint_disable")) {
                disableCommands.append(message);
            } else if (type == QLatin1String("remote_session_cursor")) {
                cursorSamples.append(message);
            }
        });

        sendOn(socket, socketBootId, QJsonObject{
            {QStringLiteral("type"), QStringLiteral("auth_challenge")},
            {QStringLiteral("nonce"), QString::fromLatin1(
                 QByteArray(32, 'r').toBase64(
                     QByteArray::Base64UrlEncoding
                     | QByteArray::OmitTrailingEquals))},
            {QStringLiteral("issuedAt"), 1}
        });
    }

    void sendWelcome(QWebSocket* socket, const QString& socketBootId,
                     quint64 socketGeneration,
                     const QJsonObject& authentication)
    {
        const QJsonObject policy{
            {QStringLiteral("policyVersion"), policyVersion},
            {QStringLiteral("transportTimeoutMs"), 5000},
            {QStringLiteral("heartbeatIntervalMs"), heartbeatIntervalMs},
            {QStringLiteral("transportSuspectAfterMs"), heartbeatIntervalMs * 2},
            {QStringLiteral("sessionRecoveryTimeoutMs"), sessionRecoveryTimeoutMs > 0 ? sessionRecoveryTimeoutMs : leaseTimeoutMs},
            {QStringLiteral("leaseTimeoutMs"), heartbeatIntervalMs * 2},
            {QStringLiteral("scenePrepareTimeoutMs"), 15'000},
            {QStringLiteral("sceneActivationLeadMs"), 4'000},
            {QStringLiteral("sceneMaxClockSkewMs"), 50},
            {QStringLiteral("sceneStartedAckTimeoutMs"), 5'000},
            {QStringLiteral("sceneMaxStartSkewMs"), 750},
            {QStringLiteral("sceneStopTimeoutMs"), 5'000},
            {QStringLiteral("uploadIdleTimeoutMs"), 45'000},
            {QStringLiteral("uploadTargetAckTimeoutMs"), 30'000},
            {QStringLiteral("removalAckTimeoutMs"), 30'000}
        };
        sendOn(socket, socketBootId, QJsonObject{
            {QStringLiteral("type"), QStringLiteral("welcome")},
            {QStringLiteral("connectionId"),
             QUuid::createUuid().toString(QUuid::WithoutBraces)},
            {QStringLiteral("installationId"),
             authentication.value(QStringLiteral("installationId"))},
            {QStringLiteral("endpointId"), ownerEndpointId},
            {QStringLiteral("instanceId"),
             authentication.value(QStringLiteral("instanceId"))},
            {QStringLiteral("instanceOrdinal"),
             authentication.value(QStringLiteral("instanceOrdinal"))},
            {QStringLiteral("runtimeId"),
             authentication.value(QStringLiteral("runtimeId"))},
            {QStringLiteral("connectionGeneration"),
             static_cast<double>(socketGeneration)},
            {QStringLiteral("policy"), policy},
            {QStringLiteral("serverMonotonicMs"), 1}
        });
    }
};
}

class ClientConnectionFlowTest final : public QObject
{
    Q_OBJECT

private slots:
    void serverRecoveryBadgeSurvivesRetriesAndExpires_data()
    {
        QTest::addColumn<bool>("abortSocket");
        QTest::newRow("closed-websocket") << false;
        QTest::newRow("abrupt-network-loss") << true;
    }

    void serverRecoveryBadgeSurvivesRetriesAndExpires()
    {
        QFETCH(bool, abortSocket);
        QTemporaryDir root;
        WebSocketClient client(root.path(), false);
        ConnectionManager connections(&client);
        RemoteSessionTestServer server(client.endpointId());
        server.policyVersion = 5;
        server.heartbeatIntervalMs = 750;
        server.leaseTimeoutMs = 1'500;
        server.sessionRecoveryTimeoutMs = 3'000;
        QVERIFY(server.listen());
        const QString url = server.url();
        connect(&client, &WebSocketClient::connected, &connections, [&] {
            client.registrationConfirmed(ClientInfo());
            client.reconciliationCompleted();
        });
        connections.connectToServer(url);
        QTRY_VERIFY_WITH_TIMEOUT(connections.isReady(), 2'000);
        QSignalSpy statuses(&connections, &ConnectionManager::statusChanged);
        server.server.close();
        if (abortSocket) server.peer->abort();
        else server.closePeer();
        QTRY_VERIFY_WITH_TIMEOUT(!client.isConnected(), 1'000);
        QCOMPARE(connections.getConnectionStatus(), QStringLiteral("Degraded"));
        QVERIFY(!connections.isReady());
        QTest::qWait(300);
        QCOMPARE(connections.getConnectionStatus(), QStringLiteral("Degraded"));
        for (const auto& status : statuses)
            QCOMPARE(status.first().toString(), QStringLiteral("Degraded"));
        // Repeated failed connection attempts cannot renew the grace period.
        QTRY_VERIFY_WITH_TIMEOUT(connections.getConnectionStatus() != QLatin1String("Degraded"), 3'500);
        QVERIFY(!statuses.isEmpty());
        QTRY_VERIFY_WITH_TIMEOUT(statuses.last().first().toString() != QLatin1String("Degraded"), 500);
        connections.disconnect();
        QCOMPARE(connections.getConnectionStatus(), QStringLiteral("Disconnected"));
    }

    void lateSocketLossDoesNotRestartExpiredRecovery()
    {
        QTemporaryDir root;
        qint64 now = 1'000;
        WebSocketClient client(root.path(), false, nullptr, [&] { return now; });
        ConnectionManager connections(&client, nullptr, [&] { return now; });
        RemoteSessionTestServer server(client.endpointId());
        server.policyVersion = 5;
        server.heartbeatIntervalMs = 750;
        server.leaseTimeoutMs = 1'500;
        server.sessionRecoveryTimeoutMs = 3'000;
        QVERIFY(server.listen());
        connect(&client, &WebSocketClient::connected, &connections, [&] {
            client.registrationConfirmed(ClientInfo());
            client.reconciliationCompleted();
        });
        connections.connectToServer(server.url());
        QTRY_VERIFY_WITH_TIMEOUT(connections.isReady(), 2'000);
        QSignalSpy statuses(&connections, &ConnectionManager::statusChanged);
        // Simulate sleep past both silence detection and the recovery window.
        now += 4'500;
        server.server.close();
        server.peer->abort();
        QTRY_VERIFY_WITH_TIMEOUT(!client.isConnected(), 1'000);
        QVERIFY(!connections.isReady());
        QVERIFY(connections.getConnectionStatus() != QLatin1String("Degraded"));
        QVERIFY(!statuses.contains({QStringLiteral("Degraded")}));
        connections.disconnect();
    }

    void serverRecoveryBadgeClearsOnRecoveryAndExplicitDisable()
    {
        QTemporaryDir root;
        WebSocketClient client(root.path(), false);
        ConnectionManager connections(&client);
        RemoteSessionTestServer server(client.endpointId());
        server.policyVersion = 5;
        server.heartbeatIntervalMs = 750;
        server.leaseTimeoutMs = 1'500;
        server.sessionRecoveryTimeoutMs = 3'000;
        QVERIFY(server.listen());
        connect(&client, &WebSocketClient::connected, &connections, [&] {
            client.registrationConfirmed(ClientInfo());
            client.reconciliationCompleted();
        });
        connections.connectToServer(server.url());
        QTRY_VERIFY_WITH_TIMEOUT(connections.isReady(), 2'000);
        QSignalSpy statuses(&connections, &ConnectionManager::statusChanged);
        ++server.connectionGeneration;
        server.closePeer();
        QTRY_VERIFY_WITH_TIMEOUT(server.acceptedConnections >= 2 && connections.isReady(), 2'000);
        QCOMPARE(connections.getConnectionStatus(), QStringLiteral("Connected"));
        QVERIFY(statuses.contains({QStringLiteral("Degraded")}));
        for (const auto& status : statuses)
            QVERIFY(status.first().toString() == QLatin1String("Degraded")
                || status.first().toString() == QLatin1String("Connected"));
        server.server.close();
        server.closePeer();
        QTRY_VERIFY_WITH_TIMEOUT(!client.isConnected(), 1'000);
        QCOMPARE(connections.getConnectionStatus(), QStringLiteral("Degraded"));
        connections.setConnectionEnabled(false);
        QCOMPARE(connections.getConnectionStatus(), QStringLiteral("Disconnecting"));
        connections.completeDisconnect(connections.transitionId());
        QCOMPARE(connections.getConnectionStatus(), QStringLiteral("Disconnected"));
    }

    void repeatedRemoteErrorsHaveReadableDeduplicatedToasts()
    {
        QTemporaryDir root;
        RuntimeProfileContext context;
        context.rootPath = root.path();
        context.persistent = false;
        RuntimeProfile::configure(context);
        ApplicationRuntime runtime(context);
        QSignalSpy toasts(runtime.getNotificationCenter(), &NotificationCenter::toastRequested);
        const QStringList codes{QStringLiteral("cleanup_not_committed"),
            QStringLiteral("target_offline"), QStringLiteral("resume_required"),
            QStringLiteral("unknown_remote_session"), QStringLiteral("new_internal_error")};
        for (const QString& code : codes) {
            for (int i = 0; i < 100; ++i) {
                runtime.getWebSocketClient()->remoteSessionError(QJsonObject{
                    {"code", code}, {"message", code}, {"requestId", ""},
                    {"remoteSessionId", "repeated-error-session"},
                    {"messageId", QUuid::createUuid().toString(QUuid::WithoutBraces)}});
            }
        }
        QCOMPARE(toasts.size(), codes.size());
        runtime.connectUploadSignals();
        for (int i = 0; i < 100; ++i)
            runtime.getUploadManager()->uploadRejected(QStringLiteral("repeated-upload"),
                QStringLiteral("remote_session_unavailable"));
        QCOMPARE(toasts.size(), codes.size() + 1);
        for (const auto& toast : toasts) {
            QVERIFY(toast.first().toString().contains(QLatin1Char(' ')));
            QVERIFY(!toast.first().toString().contains(QLatin1Char('_')));
        }
        runtime.getUploadManager()->uploadRejected(QStringLiteral("readable-upload"),
            QStringLiteral("Not enough disk space"));
        QCOMPARE(toasts.last().first().toString(), QStringLiteral("Upload failed: Not enough disk space"));
        runtime.handleApplicationAboutToQuit();
    }

    void deletingLastWorkspaceSourceCancelsOnlyItsUpload_data()
    {
        QTest::addColumn<bool>("preparing");
        QTest::newRow("background-verification") << true;
        QTest::newRow("awaiting-remote-ack") << false;
    }

    void deletingLastWorkspaceSourceCancelsOnlyItsUpload()
    {
        QFETCH(bool, preparing);
        QTemporaryDir root;
        QVERIFY(root.isValid());
        const auto previous = RuntimeProfile::context();
        const auto restoreProfile = qScopeGuard([previous] { RuntimeProfile::configure(previous); });
        RuntimeProfileContext context;
        context.rootPath = root.path();
        context.persistent = false;
        RuntimeProfile::configure(context);
        ApplicationRuntime runtime(context);
        runtime.setQmlWindowVisible(true);
        runtime.setPointerInsideControlWindow(true);
        RemoteSessionTestServer server(runtime.getWebSocketClient()->endpointId());
        QVERIFY(server.listen());
        runtime.findChild<ConnectionManager*>()->connectToServer(server.url());
        QTRY_COMPARE_WITH_TIMEOUT(runtime.localStatusText(), QStringLiteral("CONNECTED"), 2000);
        const QString targetA = fixtureEndpoint(QLatin1Char('A'));
        const QString targetB = fixtureEndpoint(QLatin1Char('B'));
        const QString sessionA = QStringLiteral("source-removal-session-a");
        const QString sessionB = QStringLiteral("source-removal-session-b");
        QVERIFY(server.send(QJsonObject{{"type", "client_list"}, {"clients", QJsonArray{
            onlineClient(targetA, QStringLiteral("Source target A")).toJson(),
            onlineClient(targetB, QStringLiteral("Source target B")).toJson()}}}));
        QTRY_COMPARE(runtime.displayClients().size(), 2);
        for (const auto& target : {targetA, targetB}) {
            runtime.activateClient(target);
            QTRY_VERIFY(!server.openCommands.isEmpty()
                && server.openCommands.last().value("targetEndpointId").toString() == target);
            QVERIFY(server.sendOpened(target == targetA ? sessionA : sessionB,
                server.openCommands.last().value("requestId").toString(), target,
                ScreenInfo(0, 1920, 1080, 0, 0, true), 50));
            QTRY_VERIFY(runtime.getProjectManager()->hasProjectForTarget(target));
        }
        auto* workspaceA = runtime.findWorkspace(targetA);
        auto* workspaceB = runtime.findWorkspace(targetB);
        QVERIFY(workspaceA && workspaceB && workspaceA->canvas && workspaceB->canvas);
        const QString sourcePath = root.filePath(QStringLiteral("shared-upload.png"));
        QImage image(32, 32, QImage::Format_ARGB32_Premultiplied);
        image.fill(Qt::darkYellow);
        QVERIFY(image.save(sourcePath));
        auto* mediaA = workspaceA->canvas->document()->addPreparedFile(sourcePath, {32, 32}, false, {});
        auto* mediaB = workspaceB->canvas->document()->addPreparedFile(sourcePath, {32, 32}, false, {});
        QVERIFY(mediaA && mediaB);
        QTRY_VERIFY_WITH_TIMEOUT(mediaA->residencyReady() && mediaB->residencyReady(), 10000);
        const QString fileId = mediaA->fileId();
        QCOMPARE(mediaB->fileId(), fileId);
        QCOMPARE(runtime.getFileManager()->getMediaIdsForFile(fileId).size(), 2);
        runtime.getFileManager()->markFileUploadedToClient(fileId, targetB);
        workspaceB->knownRemoteFileIds.insert(fileId);
        UploadEventHandler handler(&runtime);
        auto* uploads = runtime.getUploadManager();
        QSignalSpy rejected(uploads, &UploadManager::uploadRejected);
        QSignalSpy cancelled(uploads, &UploadManager::uploadCancelled);
        handler.uploadWorkspace(targetA);
        QVERIFY(workspaceA->knownRemoteFileIds.isEmpty()); // No target ACK has arrived.
        QVERIFY(workspaceA->upload.fileIds.contains(fileId));
        if (!preparing) {
            QTRY_COMPARE_WITH_TIMEOUT(server.uploadStarts.size(), 1, 5000);
            QCOMPARE(server.uploadStarts.first().value("remoteSessionId").toString(), sessionA);
            QCOMPARE(uploads->activeOutgoingTransferCount(), 1);
        }
        QVERIFY(workspaceA->canvas->document()->removeMedia(mediaA->mediaId()));
        // The global last-reference notifier cannot run: B still owns this source.
        QCOMPARE(runtime.getFileManager()->getMediaIdsForFile(fileId), QList<QString>{mediaB->mediaId()});
        if (preparing) {
            // Commit reconciliation before dispatching the completed background
            // verification callback, exercising the pre-upload removal path.
            runtime.reconcileRemoteFilesForWorkspace(*workspaceA, {});
            QCOMPARE(cancelled.count(), 1);
            QCOMPARE(rejected.count(), 0);
            QCoreApplication::processEvents();
            QCOMPARE(server.uploadStarts.size(), 0);
            QCOMPARE(server.uploadAborts.size(), 0);
            QVERIFY(runtime.getWebSocketClient()->remoteSessionCoordinator()->outgoingForPeer(targetA).active);
        } else {
            // No explicit reconciliation: the committed document change timer
            // must cancel the transfer on its own.
            QTRY_COMPARE_WITH_TIMEOUT(rejected.count(), 1, 2000);
            QTRY_COMPARE_WITH_TIMEOUT(uploads->activeOutgoingTransferCount(), 0, 2000);
            QTRY_COMPARE_WITH_TIMEOUT(server.uploadAborts.size(), 1, 2000);
            QCOMPARE(server.uploadAborts.first().value("remoteSessionId").toString(), sessionA);
            QCOMPARE(server.uploadAborts.first().value("uploadId"), server.uploadStarts.first().value("uploadId"));
        }
        QVERIFY(runtime.getWebSocketClient()->remoteSessionCoordinator()->outgoingForPeer(targetB).active);
        QCOMPARE(workspaceB->canvas->document()->media().size(), 1);
        QVERIFY(mediaB->residencyReady());
        QVERIFY(runtime.getFileManager()->isFileUploadedToClient(fileId, targetB));
        QVERIFY(workspaceB->knownRemoteFileIds.contains(fileId));
        QVERIFY(QFile::exists(sourcePath));
        runtime.handleApplicationAboutToQuit();
    }

    void failedIncomingCleanupDoesNotLoopOnServerEchoes()
    {
        QTemporaryDir root;
        RuntimeProfileContext context;
        context.rootPath = root.path();
        context.persistent = false;
        RuntimeProfile::configure(context);
        ApplicationRuntime runtime(context);
        auto* client = runtime.getWebSocketClient();
        RemoteSessionTestServer server(client->endpointId());
        QVERIFY(server.listen());
        runtime.findChild<ConnectionManager*>()->connectToServer(server.url());
        QTRY_VERIFY_WITH_TIMEOUT(client->isConnected(), 2'000);
        const QString owner = fixtureEndpoint(QLatin1Char('K'));
        const QString sessionId = QStringLiteral("failed-incoming-cleanup");
        const QString teardownId = QUuid::createUuid().toString(QUuid::WithoutBraces);
        QVERIFY(server.sendIncomingOpened(sessionId, QStringLiteral("failed-cleanup-open"), owner));
        QTRY_COMPARE(client->remoteSessionCoordinator()->byId(sessionId).phase, QStringLiteral("Active"));
        auto* cache = runtime.getUploadManager()->remoteCacheStore();
        QVERIFY(cache->ensureSession({owner, sessionId, 1}));
        // A real filesystem failure, independent of renderer or platform locks.
        const QString quarantine = QDir(cache->rootPath()).filePath(QStringLiteral(".quarantine"));
        QVERIFY(QDir().rmdir(quarantine));
        QFile blocker(quarantine);
        QVERIFY(blocker.open(QIODevice::WriteOnly));
        blocker.close();
        QJsonObject terminal{{"type", "remote_session_terminating"},
            {"remoteSessionId", sessionId}, {"generation", 1},
            {"ownerConnectionGeneration", 1}, {"targetConnectionGeneration", 1},
            {"phase", "CleanupPending"}, {"ownerEndpointId", owner},
            {"targetEndpointId", client->endpointId()}, {"teardownId", teardownId}};
        QVERIFY(client->remoteSessionCoordinator()->upsert(terminal, client->connectionGeneration()));
        QSignalSpy toasts(runtime.getNotificationCenter(), &NotificationCenter::toastRequested);
        client->remoteSessionTerminating(terminal);
        QTRY_VERIFY_WITH_TIMEOUT(!server.teardownAcknowledgements.isEmpty(), 2'000);
        QCOMPARE(server.teardownAcknowledgements.last().value("result").toString(), QStringLiteral("cleanup_error"));
        const int ackCount = server.teardownAcknowledgements.size();
        const int toastCount = toasts.size();
        QVERIFY(toastCount > 0);
        for (int i = 0; i < 100; ++i) {
            client->remoteSessionTerminating(terminal);
            client->remoteSessionError(QJsonObject{{"code", "cleanup_not_committed"},
                {"message", "cleanup_not_committed"}, {"remoteSessionId", sessionId},
                {"messageId", QUuid::createUuid().toString(QUuid::WithoutBraces)}});
        }
        QTest::qWait(100);
        QCOMPARE(server.teardownAcknowledgements.size(), ackCount);
        QCOMPARE(toasts.size(), toastCount);
        QVERIFY(QFile::remove(quarantine));
        // The server's later, scheduled retry can still finish the same tuple.
        QTRY_VERIFY_WITH_TIMEOUT(([&] {
            client->remoteSessionTerminating(terminal);
            return server.teardownAcknowledgements.last().value("result") == QLatin1String("committed");
        })(), 5'000);
        QCOMPARE(toasts.size(), toastCount);
        runtime.handleApplicationAboutToQuit();
    }

    void clientListRetainsOnlyProjectsWhenDiscoveryIsUnavailable()
    {
        QTemporaryDir root;
        RuntimeProfileContext context;
        context.rootPath = root.path();
        context.persistent = false;
        RuntimeProfile::configure(context);
        ApplicationRuntime runtime(context);
        runtime.getProjectManager()->stopAutomaticTimersForTesting();
        RemoteSessionTestServer server(runtime.getWebSocketClient()->endpointId());
        QVERIFY(server.listen());
        auto* connections = runtime.findChild<ConnectionManager*>();
        connections->connectToServer(server.url());
        QTRY_COMPARE(connections->state(), ConnectionManager::State::Connected);
        ClientInfo first = onlineClient(fixtureEndpoint(QLatin1Char('A')), "Same host");
        ClientInfo second = onlineClient(fixtureEndpoint(QLatin1Char('B')), "Same host");
        const auto publish = [&] {
            return server.send(QJsonObject{{"type", "client_list"},
                {"clients", QJsonArray{first.toJson(), second.toJson()}}});
        };
        QSignalSpy presence(runtime.getWebSocketClient(), &WebSocketClient::clientListReceived);
        QVERIFY(publish());
        QTRY_COMPARE(runtime.displayClients().size(), 2);
        QVERIFY(!runtime.getProjectManager()->createProjectFromSnapshot(
            ProjectTargetReference::fromClientInfo(first), first.getScreens(), 57,
            1, 1).isEmpty());
        QCOMPARE(runtime.getProjectManager()->projectCount(), 1);

        auto* workspaces = runtime.getWorkspaceManager();
        QVERIFY(workspaces->getOrCreateWorkspace(first.endpointId(), first));
        workspaces->updateWorkspaceProjectId(first.endpointId(),
            runtime.getProjectManager()->projectForTarget(first.endpointId())->projectId);
        QVERIFY(workspaces->getOrCreateWorkspace(second.endpointId(), second));
        const auto fallback = ClientListBuilder::buildDisplayClientList(
            &runtime, {first, second}, false);
        QCOMPARE(fallback.size(), 1);
        QCOMPARE(fallback.first().endpointId(), first.endpointId());
        QVERIFY(fallback.first().hasProject());
        // Remove the provisional workspace to also check obsolete clicks below.
        workspaces->deleteWorkspace(second.endpointId());

        // A local health transition reprojects the existing presence without
        // waiting for a new server revision or rewriting its admission fields.
        const int previousPresenceCount = presence.count();
        runtime.getWebSocketClient()->transportHealthChanged(true);
        QCOMPARE(connections->state(), ConnectionManager::State::Degraded);
        QCOMPARE(runtime.displayClients().size(), 1);
        QCOMPARE(runtime.displayClients().first().endpointId(), first.endpointId());
        QCOMPARE(runtime.displayClients().first().availabilityBadgeText(), QStringLiteral("Unreachable"));
        runtime.getWebSocketClient()->transportHealthChanged(false);
        QCOMPARE(connections->state(), ConnectionManager::State::Connected);
        QCOMPARE(runtime.displayClients().size(), 2);
        QCOMPARE(presence.count(), previousPresenceCount);

        for (const QString& status : {QStringLiteral("Degraded"),
                                      QStringLiteral("Reconnecting"),
                                      QStringLiteral("Disconnected")}) {
            second.setOnline(status != QLatin1String("Disconnected"));
            second.setCanAcceptSession(false);
            second.setStatus(status);
            second.setAvailabilityStatus(status);
            QVERIFY(publish());
            QTRY_COMPARE(runtime.displayClients().size(), 1);
            QCOMPARE(qvariant_cast<QList<ClientInfo>>(presence.last().first()).size(), 2);
            runtime.activateClient(second.endpointId()); // queued obsolete click
            QVERIFY(!runtime.findWorkspace(second.endpointId()));
            QVERIFY(server.openCommands.isEmpty());
            second.setOnline(true);
            second.setCanAcceptSession(true);
            second.setStatus(QStringLiteral("Available"));
            second.setAvailabilityStatus(QStringLiteral("Available"));
            QVERIFY(publish());
            QTRY_COMPARE(runtime.displayClients().size(), 2);
            QCOMPARE(runtime.getProjectManager()->projectCount(), 1);
        }

        first.setOnline(false);
        first.setCanAcceptSession(false);
        first.setStatus(QStringLiteral("Disconnected"));
        first.setAvailabilityStatus(QStringLiteral("Disconnected"));
        second = first;
        second.setEndpointId(fixtureEndpoint(QLatin1Char('B')));
        second.setInstanceOrdinal(QLatin1Char('B').unicode());
        second.setInstanceId(DeviceIdentityStore::instanceIdForOrdinal(second.instanceOrdinal()));
        QVERIFY(publish());
        QTRY_COMPARE(runtime.displayClients().size(), 1);
        QVERIFY(runtime.displayClients().first().hasProject());
        QCOMPARE(runtime.displayClients().first().getVolumePercent(), 57);
        QVERIFY(runtime.getProjectManager()->deleteProject(first.endpointId()));
        QVERIFY(runtime.displayClients().isEmpty()); // no new presence required
        runtime.handleApplicationAboutToQuit();
    }

    void disableEnableQueuesLatestIntentAndIgnoresObsoleteAcknowledgement()
    {
        QTemporaryDir root;
        QVERIFY(root.isValid());
        RuntimeProfileContext context;
        context.ordinal = 2;
        context.instanceId = QStringLiteral("disable-intent");
        context.profileId = context.instanceId;
        context.rootPath = root.path();
        context.persistent = false;
        RuntimeProfile::configure(context);
        ApplicationRuntime runtime(context);
        QVERIFY(!runtime.isUserDisconnected()); // Every process starts enabled.
        auto* connections = runtime.findChild<ConnectionManager*>();
        QVERIFY(connections);
        RemoteSessionTestServer server(runtime.getWebSocketClient()->endpointId());
        server.replyRegistration = false;
        server.replyReconciliation = false;
        QVERIFY(server.listen());
        QSignalSpy ready(runtime.getWebSocketClient(), &WebSocketClient::connected);
        QSignalSpy leaseExpired(runtime.getWebSocketClient(), &WebSocketClient::leaseExpired);
        connections->connectToServer(server.url());
        QTRY_COMPARE_WITH_TIMEOUT(ready.count(), 1, 2000);
        QTRY_VERIFY(!server.registrationReply.isEmpty() && !server.reconciliationReply.isEmpty());
        QCOMPARE(runtime.localStatusText(), QStringLiteral("CONNECTING"));
        QVERIFY(server.send(server.registrationReply));
        QTest::qWait(20);
        QCOMPARE(runtime.localStatusText(), QStringLiteral("CONNECTING"));
        QVERIFY(server.send(server.reconciliationReply));
        QTRY_COMPARE(runtime.localStatusText(), QStringLiteral("CONNECTED"));
        server.replyRegistration = true;
        server.replyReconciliation = true;
        runtime.setConnectionEnabled(false);
        QTRY_COMPARE_WITH_TIMEOUT(server.disableCommands.size(), 1, 1000);
        QCOMPARE(runtime.localStatusText(), QStringLiteral("DISCONNECTING"));
        runtime.setConnectionEnabled(true);
        QVERIFY(!runtime.isUserDisconnected());
        QCOMPARE(server.acceptedConnections, 1);
        QVERIFY(server.send(QJsonObject{
            {"type", "endpoint_disable_started"}, {"requestId", "obsolete"},
            {"connectionGeneration", 1}
        }));
        QTest::qWait(50);
        QCOMPARE(runtime.localStatusText(), QStringLiteral("DISCONNECTING"));
        // Transport loss during drain must obey queued Enable exactly once.
        server.connectionGeneration = 2;
        server.closePeer();
        QTRY_COMPARE_WITH_TIMEOUT(ready.count(), 2, 2000);
        QCOMPARE(server.acceptedConnections, 2);
        QTRY_COMPARE(runtime.localStatusText(), QStringLiteral("CONNECTED"));
        runtime.setConnectionEnabled(false);
        QTRY_COMPARE_WITH_TIMEOUT(server.disableCommands.size(), 2, 1000);
        runtime.setConnectionEnabled(true);
        runtime.setConnectionEnabled(false);
        QVERIFY(server.send(QJsonObject{
            {"type", "endpoint_disable_started"},
            {"requestId", server.disableCommands.last().value("requestId")},
            {"connectionGeneration", 2}
        }));
        QTRY_COMPARE_WITH_TIMEOUT(runtime.localStatusText(), QStringLiteral("DISCONNECTED"), 1000);
        QTest::qWait(100);
        QCOMPARE(runtime.localStatusText(), QStringLiteral("DISCONNECTED"));
        QVERIFY(runtime.isUserDisconnected());
        QCOMPARE(server.acceptedConnections, 2);
        QCOMPARE(leaseExpired.count(), 0);
    }

    void lostInitialOpenReplaysSameRequestUntilSelectionIsCancelled()
    {
        QTemporaryDir root;
        QVERIFY(root.isValid());
        RuntimeProfileContext context;
        context.ordinal = 2;
        context.instanceId = QStringLiteral("lost-open-intent");
        context.profileId = context.instanceId;
        context.rootPath = root.path();
        context.persistent = false;
        RuntimeProfile::configure(context);
        ApplicationRuntime runtime(context);
        runtime.setQmlWindowVisible(true);
        runtime.setPointerInsideControlWindow(true);
        RemoteSessionTestServer server(runtime.getWebSocketClient()->endpointId());
        QVERIFY(server.listen());
        runtime.findChild<ConnectionManager*>()->connectToServer(server.url());
        QTRY_COMPARE_WITH_TIMEOUT(runtime.localStatusText(), QStringLiteral("CONNECTED"), 2000);
        const QString target = fixtureEndpoint(QLatin1Char('O'));
        QVERIFY(server.sendClientList(onlineClient(target, QStringLiteral("Lost OPEN target"))));
        QTRY_COMPARE(runtime.displayClients().size(), 1);
        runtime.activateClient(target);
        QTRY_COMPARE(server.openCommands.size(), 1);
        QTRY_VERIFY_WITH_TIMEOUT(server.openCommands.size() >= 2, 3000);
        QCOMPARE(server.openCommands.first().value("requestId"),
                 server.openCommands.last().value("requestId"));
        QVERIFY(!runtime.activeProjectExists());
        runtime.navigateToClients();
        const int count = server.openCommands.size();
        QTest::qWait(1200);
        QCOMPARE(server.openCommands.size(), count);
    }

    void expiredSelectedProjectRequiresExplicitReopen_data()
    {
        QTest::addColumn<bool>("closeBeforePurge");
        QTest::newRow("session-already-closed") << true;
        QTest::newRow("close-still-pending") << false;
    }

    void expiredSelectedProjectRequiresExplicitReopen()
    {
        QFETCH(bool, closeBeforePurge);
        QTemporaryDir root;
        QVERIFY(root.isValid());
        RuntimeProfileContext context;
        context.ordinal = 2;
        context.instanceId = QStringLiteral("expired-selection");
        context.profileId = context.instanceId;
        context.rootPath = root.path();
        context.persistent = false;
        RuntimeProfile::configure(context);
        ApplicationRuntime runtime(context);
        auto* workspaces = runtime.getWorkspaceManager();
        auto* projects = runtime.getProjectManager();
        workspaces->stopAutomaticTimersForTesting();
        projects->stopAutomaticTimersForTesting();
        qint64 now = 1'000'000;
        auto* activity = runtime.findChild<ApplicationActivityMonitor*>();
        QVERIFY(activity);
        activity->setNowProviderForTesting([&] { return now; });
        workspaces->setNowProviderForTesting([&] { return now; });
        projects->setNowProviderForTesting([&] { return now; });
        runtime.setQmlWindowVisible(true);
        runtime.setPointerInsideControlWindow(true);
        RemoteSessionTestServer server(runtime.getWebSocketClient()->endpointId());
        QVERIFY(server.listen());
        QSignalSpy ready(runtime.getWebSocketClient(), &WebSocketClient::connected);
        runtime.findChild<ConnectionManager*>()->connectToServer(server.url());
        QTRY_COMPARE_WITH_TIMEOUT(ready.count(), 1, 2000);
        const QString target = fixtureEndpoint(QLatin1Char('E'));
        const ClientInfo client = onlineClient(target, QStringLiteral("Retained selection"));
        QVERIFY(server.sendClientList(client));
        QTRY_COMPARE(runtime.displayClients().size(), 1);
        runtime.activateClient(target);
        QTRY_COMPARE(server.openCommands.size(), 1);
        QVERIFY(server.sendOpened(QStringLiteral("expired-project-session-1"),
            server.openCommands.last().value("requestId").toString(), target,
            ScreenInfo(0, 1920, 1080, 0, 0, true), 50));
        QTRY_VERIFY(runtime.activeProjectExists());
        const QString oldProject = projects->projectForTarget(target)->projectId;
        runtime.setPointerInsideControlWindow(false);
        now = workspaces->remoteSessionCloseAtMs(target);
        workspaces->processDeadlines(now);
        QTRY_COMPARE(server.closeCommands.size(), 1);
        if (closeBeforePurge) {
            QVERIFY(server.sendClosed(QStringLiteral("expired-project-session-1"), target));
            QTRY_COMPARE(runtime.remoteStatusText(), QStringLiteral("AVAILABLE"));
        }
        QTRY_VERIFY(!runtime.isRemoteClientConnected());
        QSignalSpy pages(&runtime, &ApplicationRuntime::applicationPageChanged);
        connect(workspaces, &WorkspaceManager::workspaceDeleted, &runtime,
                [&](const QString& endpoint) {
            if (endpoint != target) return;
            // The page must already be gone when its graph is detached.
            QVERIFY(!runtime.getNavigationManager()->isOnScreenView());
            QCOMPARE(pages.count(), 1);
            QCOMPARE(pages.last().first().toInt(), 0);
        });
        now = projects->projectDeleteAtMs(target);
        projects->processDeadlines(now);
        QVERIFY(!runtime.activeProjectExists());
        QVERIFY(runtime.activeWorkspaceEndpointId().isEmpty());
        QVERIFY(!runtime.getNavigationManager()->isOnScreenView());
        QVERIFY(!runtime.getActiveCanvas());
        QVERIFY(!runtime.findWorkspace(target));
        QCOMPARE(server.openCommands.size(), 1);
        ++now;
        runtime.setPointerInsideControlWindow(true);
        QVERIFY(server.sendClientList(client));
        if (!closeBeforePurge) {
            QVERIFY(server.sendClosed(QStringLiteral("expired-project-session-1"), target));
        }
        QTRY_COMPARE(runtime.displayClients().first().availabilityBadgeText(),
                     QStringLiteral("Available"));
        QCOMPARE(server.openCommands.size(), 1);

        // Reconnection and rediscovery cannot restore a purged selection.
        QSignalSpy disconnected(runtime.getWebSocketClient(), &WebSocketClient::disconnected);
        server.connectionGeneration = 2;
        server.closePeer();
        QTRY_COMPARE(disconnected.count(), 1);
        QTRY_COMPARE(ready.count(), 2);
        QVERIFY(server.sendClientList(client));
        QTRY_COMPARE(runtime.displayClients().size(), 1);
        QCoreApplication::processEvents();
        QCOMPARE(server.openCommands.size(), 1);
        QCOMPARE(projects->projectCount(), 0);
        QVERIFY(!runtime.getNavigationManager()->isOnScreenView());
        QCOMPARE(pages.count(), 1);

        runtime.activateClient(target);
        QTRY_COMPARE(server.openCommands.size(), 2);
        QVERIFY(!runtime.activeProjectExists()); // Fresh snapshot is required.
        QVERIFY(server.sendOpened(QStringLiteral("expired-project-session-2"),
            server.openCommands.last().value("requestId").toString(), target,
            ScreenInfo(1, 2560, 1440, 0, 0, true), 60));
        QTRY_VERIFY(runtime.activeProjectExists());
        QVERIFY(projects->projectForTarget(target)->projectId != oldProject);
        QVERIFY(runtime.getActiveCanvas());
        QVERIFY(runtime.getActiveCanvas()->document()->media().isEmpty());
        QVERIFY(!runtime.getActiveCanvas()->testSceneLaunched());
        runtime.handleApplicationAboutToQuit();
        runtime.getNavigationManager()->setActiveCanvas(nullptr);
        runtime.setActiveCanvas(nullptr);
        auto* workspace = runtime.findWorkspace(target);
        QVERIFY(workspace);
        delete workspace->canvas;
        workspace->canvas = nullptr;
    }

    void purgingBackgroundProjectPreservesCurrentPage_data()
    {
        QTest::addColumn<int>("destination");
        QTest::newRow("clients") << 0;
        QTest::newRow("another-client") << 1;
        QTest::newRow("history") << 2;
    }

    void purgingBackgroundProjectPreservesCurrentPage()
    {
        QFETCH(int, destination);
        QTemporaryDir root;
        RuntimeProfileContext context;
        context.rootPath = root.path();
        context.persistent = false;
        RuntimeProfile::configure(context);
        ApplicationRuntime runtime(context);
        auto* projects = runtime.getProjectManager();
        projects->stopAutomaticTimersForTesting();
        runtime.getWorkspaceManager()->stopAutomaticTimersForTesting();
        qint64 now = 1'000'000;
        projects->setNowProviderForTesting([&] { return now; });
        const auto first = onlineClient(fixtureEndpoint(QLatin1Char('J')), "Purged project");
        const auto second = onlineClient(fixtureEndpoint(QLatin1Char('K')), "Other project");
        for (const auto& client : {first, second}) {
            QVERIFY(!projects->createProjectFromSnapshot(
                ProjectTargetReference::fromClientInfo(client), client.getScreens(),
                50, 1, now).isEmpty());
        }
        runtime.activateClient(first.endpointId());
        QVERIFY(runtime.getActiveCanvas());
        if (destination == 0) runtime.navigateToClients();
        else if (destination == 2) runtime.navigateToHistory();
        else runtime.activateClient(second.endpointId());
        auto* const otherCanvas = destination == 1 ? runtime.getActiveCanvas() : nullptr;
        QSignalSpy pages(&runtime, &ApplicationRuntime::applicationPageChanged);
        QVERIFY(projects->setHidden(first.endpointId(), now));
        now = projects->projectDeleteAtMs(first.endpointId());
        projects->processDeadlines(now);
        QVERIFY(!projects->hasProjectForTarget(first.endpointId()));
        QVERIFY(projects->hasProjectForTarget(second.endpointId()));
        QVERIFY(!runtime.findWorkspace(first.endpointId()));
        QCOMPARE(pages.count(), 0);
        if (destination == 1) {
            QCOMPARE(runtime.activeWorkspaceEndpointId(), second.endpointId());
            QCOMPARE(runtime.getActiveCanvas(), otherCanvas);
            QVERIFY(runtime.getNavigationManager()->isOnScreenView());
        } else {
            QVERIFY(!runtime.getNavigationManager()->isOnScreenView());
            QVERIFY(!runtime.getActiveCanvas());
        }
        runtime.handleApplicationAboutToQuit();
        runtime.getNavigationManager()->setActiveCanvas(nullptr);
        runtime.setActiveCanvas(nullptr);
        if (auto* workspace = runtime.findWorkspace(second.endpointId())) {
            delete workspace->canvas;
            workspace->canvas = nullptr;
        }
    }

    void purgingProjectCancelsAnUnacknowledgedOpen_data()
    {
        QTest::addColumn<bool>("background");
        QTest::newRow("displayed") << false;
        QTest::newRow("background") << true;
    }

    void purgingProjectCancelsAnUnacknowledgedOpen()
    {
        QFETCH(bool, background);
        QTemporaryDir root;
        RuntimeProfileContext context;
        context.rootPath = root.path();
        context.persistent = false;
        RuntimeProfile::configure(context);
        ApplicationRuntime runtime(context);
        auto* projects = runtime.getProjectManager();
        projects->stopAutomaticTimersForTesting();
        runtime.getWorkspaceManager()->stopAutomaticTimersForTesting();
        qint64 now = 1'000'000;
        projects->setNowProviderForTesting([&] { return now; });
        RemoteSessionTestServer server(runtime.getWebSocketClient()->endpointId());
        QVERIFY(server.listen());
        runtime.findChild<ConnectionManager*>()->connectToServer(server.url());
        QTRY_COMPARE(runtime.localStatusText(), QStringLiteral("CONNECTED"));
        const auto client = onlineClient(fixtureEndpoint(QLatin1Char('L')), "Pending open");
        QVERIFY(server.sendClientList(client));
        QTRY_COMPARE(runtime.displayClients().size(), 1);
        QVERIFY(!projects->createProjectFromSnapshot(
            ProjectTargetReference::fromClientInfo(client), client.getScreens(),
            50, 1, now).isEmpty());
        runtime.activateClient(client.endpointId());
        QTRY_COMPARE(server.openCommands.size(), 1);
        if (background) runtime.navigateToHistory();
        QSignalSpy pages(&runtime, &ApplicationRuntime::applicationPageChanged);
        QVERIFY(projects->setHidden(client.endpointId(), now));
        now = projects->projectDeleteAtMs(client.endpointId());
        projects->processDeadlines(now);
        QCOMPARE(projects->projectCount(), 0);
        QVERIFY(!runtime.getNavigationManager()->isOnScreenView());
        QCOMPARE(pages.count(), background ? 0 : 1);

        // The old OPEN can still reach the server after local cancellation.
        QVERIFY(server.sendOpened("purged-pending-open", server.openCommands.first().value("requestId").toString(),
                                  client.endpointId(), ScreenInfo(0, 1920, 1080, 0, 0, true), 50));
        QTRY_COMPARE(server.closeCommands.size(), 1);
        QCOMPARE(projects->projectCount(), 0);
        QVERIFY(!runtime.findWorkspace(client.endpointId()));
        QVERIFY(server.sendClosed("purged-pending-open", client.endpointId()));
        QTRY_COMPARE(runtime.displayClients().first().availabilityBadgeText(), QStringLiteral("Available"));
        runtime.setQmlWindowVisible(true);
        runtime.setPointerInsideControlWindow(true);
        QVERIFY(server.sendClientList(client));
        QCoreApplication::processEvents();
        QCOMPARE(server.openCommands.size(), 1);
        QVERIFY(!runtime.getNavigationManager()->isOnScreenView());
        QCOMPARE(projects->projectCount(), 0);
        runtime.handleApplicationAboutToQuit();
    }

    void remoteBadgesUsePresenceIndependentlyOfSessionLifetime()
    {
        QTemporaryDir root;
        RuntimeProfileContext context;
        context.rootPath = root.path();
        context.persistent = false;
        RuntimeProfile::configure(context);
        ApplicationRuntime runtime(context);
        runtime.getProjectManager()->stopAutomaticTimersForTesting();
        runtime.getWorkspaceManager()->stopAutomaticTimersForTesting();
        RemoteSessionTestServer server(runtime.getWebSocketClient()->endpointId());
        QVERIFY(server.listen());
        auto* connection = runtime.findChild<ConnectionManager*>();
        connection->connectToServer(server.url());
        QTRY_COMPARE(runtime.localStatusText(), QStringLiteral("CONNECTED"));
        ClientInfo client = onlineClient(fixtureEndpoint(QLatin1Char('M')), "Status target");
        QVERIFY(server.sendClientList(client));
        QTRY_COMPARE(runtime.displayClients().size(), 1);
        const auto verifyBadge = [&](const QString& expected) {
            QCOMPARE(runtime.remoteStatusText(), expected.toUpper());
            QCOMPARE(runtime.displayClients().size(), 1);
            QCOMPARE(runtime.displayClients().first().availabilityBadgeText(), expected);
        };
        runtime.activateClient(client.endpointId());
        QTRY_COMPARE(server.openCommands.size(), 1);
        verifyBadge("Connecting");
        QVERIFY(server.sendOpened("status-session", server.openCommands.first().value("requestId").toString(),
                                  client.endpointId(), ScreenInfo(0, 1920, 1080, 0, 0, true), 50));
        QTRY_VERIFY(runtime.activeProjectExists());
        verifyBadge("Connected");
        QVERIFY(server.sendTerminating("status-session", client.endpointId()));
        QTRY_COMPARE(runtime.remoteStatusText(), QStringLiteral("DISCONNECTING"));
        verifyBadge("Disconnecting");
        QVERIFY(!runtime.isRemoteOverlayActionsEnabled());
        QVERIFY(server.sendClosed("status-session", client.endpointId()));
        QTRY_COMPARE(runtime.remoteStatusText(), QStringLiteral("AVAILABLE"));
        verifyBadge("Available");
        QVERIFY(runtime.displayClients().first().isOnline());
        QVERIFY(runtime.displayClients().first().canAcceptSession());
        QCOMPARE(server.openCommands.size(), 1);

        // Reproject without a new server presence message. Local health cannot
        // rewrite peer presence, and without a session there is no recovery badge.
        emit runtime.getWebSocketClient()->transportHealthChanged(true);
        verifyBadge("Unreachable");
        QVERIFY(runtime.displayClients().first().isOnline());
        QVERIFY(runtime.displayClients().first().canAcceptSession());
        emit runtime.getWebSocketClient()->transportHealthChanged(false);
        verifyBadge("Available");

        client.setCanAcceptSession(false);
        client.setStatus("Degraded");
        client.setAvailabilityStatus("Degraded");
        QVERIFY(server.sendClientList(client));
        QTRY_COMPARE(runtime.remoteStatusText(), QStringLiteral("DEGRADED"));
        verifyBadge("Degraded");
        QVERIFY(!runtime.displayClients().first().canAcceptSession());
        client.setOnline(false);
        client.setStatus("Disconnected");
        client.setAvailabilityStatus("Disconnected");
        QVERIFY(server.sendClientList(client));
        QTRY_COMPARE(runtime.remoteStatusText(), QStringLiteral("DISCONNECTED"));
        verifyBadge("Disconnected");

        // Even a previously confirmed offline peer becomes unverified when
        // we disable our own connection; only the local badge says Disconnected.
        runtime.setConnectionEnabled(false);
        verifyBadge("Unreachable");
        QTRY_COMPARE(server.disableCommands.size(), 1);
        QVERIFY(server.send(QJsonObject{
            {"type", "endpoint_disable_started"},
            {"requestId", server.disableCommands.last().value("requestId")},
            {"connectionGeneration", static_cast<qint64>(server.connectionGeneration)}
        }));
        QTRY_COMPARE(runtime.localStatusText(), QStringLiteral("DISCONNECTED"));
        verifyBadge("Unreachable");
        QVERIFY(!runtime.isRemoteClientConnected());
        QVERIFY(!runtime.isRemoteOverlayActionsEnabled());
        QCOMPARE(server.openCommands.size(), 1);
        auto* workspace = runtime.findWorkspace(client.endpointId());
        runtime.handleApplicationAboutToQuit();
        runtime.getNavigationManager()->setActiveCanvas(nullptr);
        runtime.setActiveCanvas(nullptr);
        QVERIFY(workspace);
        delete workspace->canvas;
        workspace->canvas = nullptr;
    }

    void localTestSurvivesRemoteSessionCleanup_data()
    {
        QTest::addColumn<QString>("cause");
        QTest::newRow("peer-session-closed") << QStringLiteral("peer-closed");
        QTest::newRow("connection-lease-expired") << QStringLiteral("lease-expired");
        QTest::newRow("remote-session-inactivity") << QStringLiteral("inactivity");
    }

    void localTestSurvivesRemoteSessionCleanup()
    {
        QFETCH(QString, cause);
        QTemporaryDir root;
        QVERIFY(root.isValid());
        RuntimeProfileContext context;
        context.ordinal = 2;
        context.instanceId = QStringLiteral("local-scene-network-loss");
        context.profileId = context.instanceId;
        context.rootPath = root.path();
        context.persistent = false;
        RuntimeProfile::configure(context);
        ApplicationRuntime runtime(context);
        runtime.getWorkspaceManager()->stopAutomaticTimersForTesting();
        runtime.getProjectManager()->stopAutomaticTimersForTesting();
        auto* websocket = runtime.getWebSocketClient();
        RemoteSessionTestServer server(websocket->endpointId());
        server.heartbeatIntervalMs = 250;
        server.leaseTimeoutMs = 1000;
        QVERIFY(server.listen());
        auto* connections = runtime.findChild<ConnectionManager*>();
        QVERIFY(connections);
        connections->connectToServer(server.url());
        QTRY_VERIFY_WITH_TIMEOUT(websocket->isConnected(), 2000);
        const QString target = fixtureEndpoint(QLatin1Char('L'));
        const QString sessionId = QStringLiteral("local-test-remote-session");
        QVERIFY(server.sendClientList(onlineClient(target, QStringLiteral("Remote peer"))));
        QTRY_COMPARE_WITH_TIMEOUT(runtime.displayClients().size(), 1, 1000);
        runtime.activateClient(target);
        QTRY_COMPARE_WITH_TIMEOUT(server.openCommands.size(), 1, 1000);
        QVERIFY(server.sendOpened(sessionId,
            server.openCommands.last().value(QStringLiteral("requestId")).toString(),
            target, ScreenInfo(0, 1920, 1080, 0, 0, true), 50));
        QTRY_VERIFY_WITH_TIMEOUT(runtime.isRemoteClientConnected(), 1000);
        ICanvasHost* canvas = runtime.getActiveCanvas();
        QVERIFY(canvas && runtime.activeProjectExists());
        CanvasMedia* media = canvas->document()->addText({}, QStringLiteral("Local demo"));
        QVERIFY(media);
        media->setContentVisible(false);
        canvas->triggerTestSceneAction();
        QVERIFY(canvas->testSceneLaunched());
        QVERIFY(canvas->document()->editsLocked());
        QVERIFY(!media->contentVisible());

        if (cause == QLatin1String("lease-expired")) {
            QSignalSpy expired(websocket, &WebSocketClient::remoteSessionRecoveryExpired);
            server.acknowledgeHeartbeats = false;
            QTRY_COMPARE_WITH_TIMEOUT(expired.count(), 1, 3000);
        } else if (cause == QLatin1String("inactivity")) {
            runtime.getWorkspaceManager()->setRemoteSessionHiddenTimeoutMs(10);
            runtime.getWorkspaceManager()->markAllWorkspacesHidden();
            runtime.getWorkspaceManager()->processDeadlines(
                runtime.getWorkspaceManager()->remoteSessionCloseAtMs(target));
            QTRY_COMPARE_WITH_TIMEOUT(server.closeCommands.size(), 1, 1000);
        } else {
            QVERIFY(server.send(QJsonObject{{QStringLiteral("type"), QStringLiteral("client_list")},
                                           {QStringLiteral("clients"), QJsonArray{}}}));
            QVERIFY(server.sendTerminating(sessionId, target));
            QTRY_COMPARE_WITH_TIMEOUT(websocket->remoteSessionCoordinator()->byId(sessionId).phase,
                                     QStringLiteral("CleanupPending"), 1000);
            QVERIFY(canvas->testSceneLaunched());
            QVERIFY(canvas->document()->editsLocked());
            QVERIFY(server.sendClosed(sessionId, target));
            QTRY_VERIFY_WITH_TIMEOUT(websocket->remoteSessionCoordinator()
                ->outgoingForPeer(target).remoteSessionId.isEmpty(), 1000);
        }
        QCOMPARE(runtime.getActiveCanvas(), canvas);
        QVERIFY(canvas->testSceneLaunched());
        QVERIFY(canvas->testSceneActionEnabled());
        QVERIFY(canvas->document()->editsLocked());
        QVERIFY(!media->contentVisible());
        QVERIFY(!canvas->remoteSceneActionEnabled());
        canvas->triggerTestSceneAction();
        QVERIFY(!canvas->testSceneLaunched());
        QVERIFY(!canvas->document()->editsLocked());
        QVERIFY(!media->contentVisible());

        auto* workspace = runtime.findWorkspace(target);
        runtime.handleApplicationAboutToQuit();
        runtime.getNavigationManager()->setActiveCanvas(nullptr);
        runtime.setActiveCanvas(nullptr);
        QVERIFY(workspace && workspace->canvas);
        delete workspace->canvas;
        workspace->canvas = nullptr;
    }

    void remoteCursorStreamsWithoutSceneAndRecoversAfterStaleOrResumedSession()
    {
        QTemporaryDir root;
        QVERIFY(root.isValid());
        RuntimeProfileContext context;
        context.ordinal = 2;
        context.instanceId = QStringLiteral("remote-cursor-regression");
        context.profileId = context.instanceId;
        context.rootPath = root.path();
        context.persistent = false;
        RuntimeProfile::configure(context);
        ApplicationRuntime runtime(context);
        runtime.getWorkspaceManager()->stopAutomaticTimersForTesting();
        runtime.getProjectManager()->stopAutomaticTimersForTesting();
        auto* websocket = runtime.getWebSocketClient();
        RemoteSessionTestServer server(websocket->endpointId());
        QVERIFY(server.listen());
        auto* connections = runtime.findChild<ConnectionManager*>();
        QVERIFY(connections);
        connections->connectToServer(server.url());
        QTRY_VERIFY_WITH_TIMEOUT(websocket->isConnected(), 2000);
        const QString target = fixtureEndpoint(QLatin1Char('C'));
        const QString sessionId = QStringLiteral("cursor-outgoing");
        QVERIFY(server.sendClientList(onlineClient(target, QStringLiteral("Cursor target"))));
        QTRY_COMPARE_WITH_TIMEOUT(runtime.displayClients().size(), 1, 1000);
        runtime.activateClient(target);
        QTRY_COMPARE_WITH_TIMEOUT(server.openCommands.size(), 1, 1000);
        QVERIFY(server.sendOpened(sessionId,
            server.openCommands.last().value(QStringLiteral("requestId")).toString(),
            target, ScreenInfo(7, 1920, 1080, -1920, 0, true), 50));
        QTRY_VERIFY_WITH_TIMEOUT(runtime.getActiveCanvas(), 1000);
        CanvasDocument* document = runtime.getActiveCanvas()->document();
        QVERIFY(document);
        QVERIFY(!document->remoteCursorVisible());
        QJsonObject sample{
            {QStringLiteral("type"), QStringLiteral("remote_session_cursor")},
            {QStringLiteral("remoteSessionId"), sessionId},
            {QStringLiteral("generation"), 1},
            {QStringLiteral("ownerConnectionGeneration"), 1},
            {QStringLiteral("targetConnectionGeneration"), 1},
            {QStringLiteral("connectionGeneration"), 1},
            {QStringLiteral("ownerEndpointId"), websocket->endpointId()},
            {QStringLiteral("targetEndpointId"), target},
            {QStringLiteral("sequence"), 1},
            {QStringLiteral("visible"), true},
            {QStringLiteral("screenId"), 7},
            {QStringLiteral("x"), 120},
            {QStringLiteral("y"), 240}
        };
        QVERIFY(server.send(sample));
        QTRY_VERIFY_WITH_TIMEOUT(document->remoteCursorVisible(), 1000);
        QPointF expected;
        QVERIFY(document->mapRemoteCursor(7, {120, 240}, &expected));
        QCOMPARE(document->remoteCursorPosition(), expected);
        // A live websocket heartbeat cannot keep a stale pointer visible.
        QTRY_VERIFY_WITH_TIMEOUT(!document->remoteCursorVisible(), 4000);
        QVERIFY(websocket->isConnected());
        QVERIFY(server.cursorSamples.isEmpty()); // no incoming session to sample for
        sample.insert(QStringLiteral("sequence"), 2);
        QVERIFY(server.send(sample));
        QTRY_VERIFY_WITH_TIMEOUT(document->remoteCursorVisible(), 1000);
        QVERIFY(server.sendResumed(sessionId, target, 1, 1, QStringLiteral("Grace")));
        QTRY_VERIFY_WITH_TIMEOUT(!document->remoteCursorVisible(), 1000);
        QVERIFY(server.sendResumed(sessionId, target, 2, 2, QStringLiteral("Active")));
        QTRY_VERIFY_WITH_TIMEOUT(websocket->remoteSessionCoordinator()->byId(sessionId).active, 1000);
        sample.insert(QStringLiteral("generation"), 2);
        sample.insert(QStringLiteral("targetConnectionGeneration"), 2);
        sample.insert(QStringLiteral("sequence"), 1); // fresh generation restarts ordering
        QVERIFY(server.send(sample));
        QTRY_VERIFY_WITH_TIMEOUT(document->remoteCursorVisible(), 1000);

        // The reverse session starts local sampling even without media or an
        // active SceneRun. Stationary cursors still refresh at least every 1s.
        const QString incomingId = QStringLiteral("cursor-incoming");
        QVERIFY(server.sendIncomingOpened(incomingId, QStringLiteral("cursor-offer"), target));
        QTRY_VERIFY_WITH_TIMEOUT(server.cursorSamples.size() >= 2, 1600);
        QCOMPARE(server.cursorSamples.first().value(QStringLiteral("remoteSessionId")).toString(), incomingId);
        QCOMPARE(server.cursorSamples.first().value(QStringLiteral("sequence")).toInt(), 1);
        QVERIFY(server.cursorSamples.last().value(QStringLiteral("sequence")).toInteger()
                > server.cursorSamples.first().value(QStringLiteral("sequence")).toInteger());
        QVERIFY(server.send(QJsonObject{
            {QStringLiteral("type"), QStringLiteral("remote_session_lease_state")},
            {QStringLiteral("remoteSessionId"), incomingId},
            {QStringLiteral("generation"), 1},
            {QStringLiteral("ownerConnectionGeneration"), 1},
            {QStringLiteral("targetConnectionGeneration"), 1},
            {QStringLiteral("ownerEndpointId"), target},
            {QStringLiteral("targetEndpointId"), websocket->endpointId()},
            {QStringLiteral("phase"), QStringLiteral("Grace")}
        }));
        QTRY_VERIFY_WITH_TIMEOUT(!websocket->remoteSessionCoordinator()->byId(incomingId).active, 1000);
        const qsizetype stoppedCount = server.cursorSamples.size();
        QTest::qWait(100);
        QCOMPARE(server.cursorSamples.size(), stoppedCount);
        sample.insert(QStringLiteral("sequence"), 2);
        QVERIFY(server.send(sample));
        QTRY_VERIFY_WITH_TIMEOUT(document->remoteCursorVisible(), 1000);
        connections->disconnect();
        QTRY_VERIFY_WITH_TIMEOUT(!document->remoteCursorVisible(), 1000);
        auto* workspace = runtime.findWorkspace(target);
        runtime.handleApplicationAboutToQuit();
        runtime.getNavigationManager()->setActiveCanvas(nullptr);
        runtime.setActiveCanvas(nullptr);
        QVERIFY(workspace && workspace->canvas);
        delete workspace->canvas;
        workspace->canvas = nullptr;
    }

    void selectedClientSnapshotSurvivesSynchronousModelRebuild()
    {
        ClientListModel model;
        model.setClients({onlineClient(QStringLiteral("endpoint-a"),
                                       QStringLiteral("Windows B"))});

        const ClientInfo clicked = model.client(QStringLiteral("endpoint-a"));
        model.setClients({onlineClient(QStringLiteral("endpoint-c"),
                                       QStringLiteral("Replacement"))});

        QCOMPARE(clicked.endpointId(), QStringLiteral("endpoint-a"));
        QCOMPARE(clicked.getMachineName(), QStringLiteral("Windows B"));
        QCOMPARE(model.rowCount(), 1);
        QCOMPARE(model.data(model.index(0), ClientListModel::EndpointIdRole).toString(),
                 QStringLiteral("endpoint-c"));
    }

    void initialConnectionKeepsLoadingStateUntilCanvasIsReady()
    {
        ScreenNavigationManager navigation;
        QSignalSpy changes(&navigation, &ScreenNavigationManager::presentationChanged);
        const ClientInfo client = onlineClient(
            QStringLiteral("endpoint-b"), QStringLiteral("Windows B"));

        navigation.showScreenView(client, false);
        QVERIFY(navigation.isOnScreenView());
        QVERIFY(navigation.isLoading());
        QVERIFY(!navigation.canvasVisible());
        QCOMPARE(navigation.currentClientId(), QStringLiteral("endpoint-b"));

        navigation.refreshActiveClientPreservingCanvas(client);
        QVERIFY(navigation.isLoading());
        QVERIFY(!navigation.canvasVisible());

        navigation.revealCanvas();
        QVERIFY(!navigation.isLoading());
        QVERIFY(navigation.canvasVisible());
        QVERIFY(changes.count() >= 2);

        navigation.showClientList();
        QVERIFY(!navigation.isOnScreenView());
        QVERIFY(!navigation.canvasVisible());
    }

    void projectDeadlinesRefreshAsLiveCountdowns_data()
    {
        QTest::addColumn<bool>("mediaOnly");
        QTest::newRow("all-deadlines") << false;
        QTest::newRow("media-only") << true;
    }

    void projectDeadlinesRefreshAsLiveCountdowns()
    {
        QFETCH(bool, mediaOnly);
        ClientListModel model;
        ClientInfo client = onlineClient(
            QStringLiteral("endpoint-countdown"),
            QStringLiteral("Countdown client"));
        const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
        client.setHasProject(true);
        client.setProjectMediaReleaseAtMs(nowMs + 3'000);
        if (!mediaOnly) {
            client.setRemoteSessionCloseAtMs(nowMs + 4'000);
            client.setProjectDeleteAtMs(nowMs + 8'000);
        }

        QSignalSpy changes(&model, &QAbstractItemModel::dataChanged);
        model.setClients({client});
        QCOMPARE(model.data(model.index(0), ClientListModel::HasProjectRole)
                     .toBool(), true);

        QTimer* refreshTimer = model.findChild<QTimer*>(
            QStringLiteral("clientCountdownRefreshTimer"));
        QVERIFY(refreshTimer);
        QVERIFY(refreshTimer->isActive());
        const QString initial = model.data(
            model.index(0), ClientListModel::SecondaryTextRole).toString();
        QVERIFY(initial.startsWith(QStringLiteral("Free RAM in ")));

        QTRY_VERIFY_WITH_TIMEOUT(changes.count() >= 1, 1'500);
        const QString refreshed = model.data(
            model.index(0), ClientListModel::SecondaryTextRole).toString();
        QVERIFY2(refreshed != initial,
                 qPrintable(QStringLiteral("Countdown stayed frozen at '%1'")
                                .arg(initial)));

        client.setProjectMediaReleaseAtMs(-1);
        client.setRemoteSessionCloseAtMs(0);
        client.setProjectDeleteAtMs(0);
        client.setHasProject(false);
        model.setClients({client});
        QCOMPARE(model.data(model.index(0), ClientListModel::HasProjectRole)
                     .toBool(), false);
        QVERIFY(!refreshTimer->isActive());
    }

    void clickingClientWaitsForAuthenticatedSnapshotBeforeCreatingProject()
    {
        QTemporaryDir root;
        QVERIFY(root.isValid());

        RuntimeProfileContext context;
        context.ordinal = 2;
        context.instanceId = QStringLiteral("client-connection-flow");
        context.profileId = QStringLiteral("client-connection-flow");
        context.rootPath = root.path();
        context.persistent = false;
        RuntimeProfile::configure(context);

        ApplicationRuntime runtime(context);
        RemoteSessionTestServer server(runtime.getWebSocketClient()->endpointId());
        QVERIFY(server.listen());
        auto* connections = runtime.findChild<ConnectionManager*>();
        connections->connectToServer(server.url());
        QTRY_COMPARE(connections->state(), ConnectionManager::State::Connected);
        const ClientInfo client = onlineClient(
            QStringLiteral("endpoint-real-canvas"), QStringLiteral("Windows B"));
        runtime.buildDisplayClientList({client});

        QSignalSpy activeWorkspaceChanged(
            &runtime, &ApplicationRuntime::activeWorkspaceChanged);
        runtime.activateClient(client.endpointId());

        QCOMPARE(runtime.activeWorkspaceEndpointId(), client.endpointId());
        QVERIFY(!runtime.getActiveCanvas());
        QVERIFY(!runtime.activeProjectExists());
        QCOMPARE(runtime.getWorkspaceManager()->remoteSessionState(client.endpointId()),
                 WorkspaceManager::RemoteSessionState::Opening);
        QVERIFY(runtime.findWorkspace(client.endpointId()));
        QVERIFY(!runtime.findWorkspace(client.endpointId())->canvas);
        QVERIFY(activeWorkspaceChanged.count() >= 1);

        QCOMPARE(runtime.getProjectManager()->projectCount(), 0);
        QVERIFY(runtime.getNavigationManager()->isLoading());

        ClientInfo unavailable = client;
        unavailable.setOnline(false);
        unavailable.setCanAcceptSession(false);
        unavailable.setStatus(QStringLiteral("Disconnected"));
        runtime.buildDisplayClientList({unavailable});
        QVERIFY(runtime.displayClients().isEmpty());
        QCOMPARE(runtime.getProjectManager()->projectCount(), 0);
        QCOMPARE(runtime.activeWorkspaceEndpointId(), client.endpointId());
        QVERIFY(runtime.findWorkspace(client.endpointId()));

        runtime.handleApplicationAboutToQuit();
    }

    void initialSnapshotPublishesReadyWorkspaceAndCompletePresentation()
    {
        QTemporaryDir root;
        QVERIFY(root.isValid());

        RuntimeProfileContext context;
        context.ordinal = 2;
        context.instanceId = QStringLiteral("client-first-snapshot");
        context.profileId = QStringLiteral("client-first-snapshot");
        context.rootPath = root.path();
        context.persistent = false;
        RuntimeProfile::configure(context);

        ApplicationRuntime runtime(context);
        RemoteSessionTestServer server(runtime.getWebSocketClient()->endpointId());
        QVERIFY(server.listen());
        auto* connections = runtime.findChild<ConnectionManager*>();
        connections->connectToServer(server.url());
        QTRY_COMPARE(connections->state(), ConnectionManager::State::Connected);
        ClientInfo client = onlineClient(
            fixtureEndpoint(QLatin1Char('N')),
            QStringLiteral("Windows B"));
        client.setScreens({});
        client.setVolumePercent(-1);
        QVERIFY(server.sendClientList(client));
        QTRY_COMPARE(runtime.displayClients().size(), 1);
        runtime.activateClient(client.endpointId());
        QTRY_COMPARE(server.openCommands.size(), 1);

        QVERIFY(!runtime.getActiveCanvas());
        QCOMPARE(runtime.remoteVolumePercent(), -1);

        QSignalSpy activeWorkspaceChanged(
            &runtime, &ApplicationRuntime::activeWorkspaceChanged);
        QList<int> presentedVolumes;
        connect(&runtime, &ApplicationRuntime::presentationStateChanged,
                &runtime, [&runtime, &presentedVolumes] {
            presentedVolumes.append(runtime.remoteVolumePercent());
        });

        const ScreenInfo remoteScreen(7, 2560, 1440, -2560, 0, true);
        QVERIFY(server.sendOpened(
            QStringLiteral("first-snapshot-session"),
            server.openCommands.first().value(QStringLiteral("requestId")).toString(),
            client.endpointId(), remoteScreen, 47));
        QTRY_COMPARE(activeWorkspaceChanged.count(), 1);
        QVERIFY(runtime.isRemoteClientConnected());

        ApplicationRuntime::ClientWorkspace* workspace =
            runtime.findWorkspace(client.endpointId());
        QVERIFY(workspace);
        QVERIFY(workspace->canvas);
        QCOMPARE(runtime.getActiveCanvas(), workspace->canvas);
        QCOMPARE(workspace->lastClientInfo.getScreens().size(), 1);
        QCOMPARE(workspace->lastClientInfo.getScreens().first().id, 7);
        QCOMPARE(runtime.remoteVolumePercent(), 47);
        QCOMPARE(activeWorkspaceChanged.count(), 1);
        QVERIFY(!presentedVolumes.isEmpty());
        QCOMPARE(presentedVolumes.constLast(), 47);

        runtime.handleApplicationAboutToQuit();
        runtime.getNavigationManager()->setActiveCanvas(nullptr);
        runtime.setActiveCanvas(nullptr);
        delete workspace->canvas;
        workspace->canvas = nullptr;
    }

    void invalidInitialSnapshotKeepsExactCloseFenceUntilTerminalCommit()
    {
        QTemporaryDir root;
        QVERIFY(root.isValid());

        RuntimeProfileContext context;
        context.ordinal = 2;
        context.instanceId = QStringLiteral("invalid-snapshot-close-fence");
        context.profileId = QStringLiteral("invalid-snapshot-close-fence");
        context.rootPath = root.path();
        context.persistent = false;
        RuntimeProfile::configure(context);

        ApplicationRuntime runtime(context);
        runtime.getWorkspaceManager()->stopAutomaticTimersForTesting();
        runtime.getProjectManager()->stopAutomaticTimersForTesting();

        RemoteSessionTestServer server(
            runtime.getWebSocketClient()->endpointId());
        QVERIFY(server.listen());
        ConnectionManager* connections =
            runtime.findChild<ConnectionManager*>();
        QVERIFY(connections);
        QSignalSpy connectedSpy(runtime.getWebSocketClient(),
                                &WebSocketClient::connected);
        QSignalSpy errorSpy(runtime.getWebSocketClient(),
                            &WebSocketClient::remoteSessionError);
        connections->connectToServer(server.url());
        QTRY_COMPARE_WITH_TIMEOUT(connectedSpy.count(), 1, 2'000);

        const QString targetEndpointId = fixtureEndpoint(QLatin1Char('V'));
        const QString malformedSessionId =
            QStringLiteral("malformed-snapshot-session");
        ClientInfo client = onlineClient(
            targetEndpointId, QStringLiteral("Malformed snapshot target"));
        client.setScreens({});
        client.setVolumePercent(-1);
        QVERIFY(server.sendClientList(client));
        QTRY_COMPARE_WITH_TIMEOUT(runtime.displayClients().size(), 1, 1'000);

        runtime.activateClient(targetEndpointId);
        QTRY_COMPARE_WITH_TIMEOUT(server.openCommands.size(), 1, 1'000);
        const QString firstRequestId = server.openCommands.constFirst()
            .value(QStringLiteral("requestId")).toString();
        QVERIFY(!firstRequestId.isEmpty());

        // The Active binding is structurally valid, but its initial snapshot
        // omits capturedAtEpochMs. WebSocketClient must fail-close it before
        // ApplicationRuntime ever receives remoteSessionOpened.
        const ScreenInfo malformedScreen(51, 1920, 1080, 0, 0, true);
        QVERIFY(server.send(QJsonObject{
            {QStringLiteral("type"), QStringLiteral("remote_session_opened")},
            {QStringLiteral("requestId"), firstRequestId},
            {QStringLiteral("remoteSessionId"), malformedSessionId},
            {QStringLiteral("generation"), 1},
            {QStringLiteral("ownerConnectionGeneration"), 1},
            {QStringLiteral("targetConnectionGeneration"), 1},
            {QStringLiteral("phase"), QStringLiteral("Active")},
            {QStringLiteral("ownerEndpointId"),
             runtime.getWebSocketClient()->endpointId()},
            {QStringLiteral("targetEndpointId"), targetEndpointId},
            {QStringLiteral("resumeToken"),
             QStringLiteral("resume-malformed-snapshot")},
            {QStringLiteral("snapshotSequence"), 1},
            {QStringLiteral("snapshot"), QJsonObject{
                {QStringLiteral("screens"),
                 QJsonArray{malformedScreen.toJson()}},
                {QStringLiteral("systemUI"), QJsonArray{}},
                {QStringLiteral("volumePercent"), 42},
                {QStringLiteral("revision"), 1}
            }}
        }));
        QTRY_COMPARE_WITH_TIMEOUT(errorSpy.count(), 1, 1'000);
        QTRY_VERIFY_WITH_TIMEOUT(!server.closeCommands.isEmpty(), 1'000);
        QCOMPARE(errorSpy.constFirst().constFirst().toJsonObject()
                     .value(QStringLiteral("remoteSessionId")).toString(),
                 malformedSessionId);
        QVERIFY(!runtime.activeProjectExists());
        QVERIFY(!runtime.getActiveCanvas());
        QVERIFY(!runtime.isRemoteClientConnected());
        QTRY_VERIFY_WITH_TIMEOUT(
            !runtime.getNavigationManager()->isOnScreenView(), 1'000);

        runtime.activateClient(targetEndpointId);
        QVERIFY(runtime.getNavigationManager()->isOnScreenView());
        QTest::qWait(50);

        // The click is remembered as replacement intent, but the exact Active
        // identity stays fenced until its Closed tombstone. It must not be
        // reused and no empty canvas may be manufactured from discovery data.
        QCOMPARE(server.openCommands.size(), 1);
        QVERIFY(!runtime.activeProjectExists());
        QVERIFY(!runtime.getActiveCanvas());
        QVERIFY(!runtime.isRemoteClientConnected());
        QVERIFY(runtime.findWorkspace(targetEndpointId));
        QVERIFY(!runtime.findWorkspace(targetEndpointId)->canvas);
        QCOMPARE(runtime.remoteStatusText(), QStringLiteral("DISCONNECTING"));

        QVERIFY(server.sendClosed(malformedSessionId, targetEndpointId));
        QTRY_COMPARE_WITH_TIMEOUT(server.openCommands.size(), 2, 1'000);
        const QString replacementRequestId = server.openCommands.constLast()
            .value(QStringLiteral("requestId")).toString();
        QVERIFY(!replacementRequestId.isEmpty());
        QVERIFY(replacementRequestId != firstRequestId);

        const ScreenInfo replacementScreen(52, 2560, 1440, 0, 0, true);
        QVERIFY(server.sendOpened(
            QStringLiteral("valid-replacement-session"),
            replacementRequestId, targetEndpointId, replacementScreen, 73));
        QTRY_VERIFY_WITH_TIMEOUT(runtime.activeProjectExists(), 1'000);
        QTRY_VERIFY_WITH_TIMEOUT(runtime.getActiveCanvas(), 1'000);
        QTRY_VERIFY_WITH_TIMEOUT(runtime.isRemoteClientConnected(), 1'000);
        QCOMPARE(runtime.remoteStatusText(), QStringLiteral("CONNECTED"));
        QCOMPARE(runtime.remoteVolumePercent(), 73);
        QCOMPARE(runtime.findWorkspace(targetEndpointId)
                     ->lastClientInfo.getScreens().constFirst().id, 52);

        ApplicationRuntime::ClientWorkspace* workspace =
            runtime.findWorkspace(targetEndpointId);
        runtime.handleApplicationAboutToQuit();
        runtime.getNavigationManager()->setActiveCanvas(nullptr);
        runtime.setActiveCanvas(nullptr);
        QVERIFY(workspace && workspace->canvas);
        delete workspace->canvas;
        workspace->canvas = nullptr;
    }

    void inactivityCloseCannotBeResurrectedAndFreshOpenReinitializesWorkspace()
    {
        QTemporaryDir root;
        QVERIFY(root.isValid());

        RuntimeProfileContext context;
        context.ordinal = 2;
        context.instanceId = QStringLiteral("client-inactivity-reopen");
        context.profileId = QStringLiteral("client-inactivity-reopen");
        context.rootPath = root.path();
        context.persistent = false;
        RuntimeProfile::configure(context);

        ApplicationRuntime runtime(context);
        runtime.getWorkspaceManager()->stopAutomaticTimersForTesting();
        runtime.getProjectManager()->stopAutomaticTimersForTesting();
        runtime.getWorkspaceManager()->setRemoteSessionHiddenTimeoutMs(10);

        QWebSocketServer server(QStringLiteral("runtime-inactivity-reopen-test"),
                                QWebSocketServer::NonSecureMode);
        QVERIFY(server.listen(QHostAddress::LocalHost, 0));

        const QString bootId = QUuid::createUuid().toString(QUuid::WithoutBraces);
        const QString ownerEndpointId = runtime.getWebSocketClient()->endpointId();
        const QString targetEndpointId = fixtureEndpoint(QLatin1Char('B'));
        const QString firstSessionId = QStringLiteral("inactivity-session-1");
        const QString secondSessionId = QStringLiteral("inactivity-session-2");
        const QString teardownId = QStringLiteral("inactivity-teardown-1");
        QCOMPARE(ownerEndpointId.size(), 43);

        QPointer<QWebSocket> peer;
        QList<QJsonObject> openCommands;
        QList<QJsonObject> closeCommands;
        QJsonObject authentication;
        auto sendServerMessage = [&](QJsonObject message) {
            QVERIFY2(peer, "The fake server has no authenticated peer");
            message.insert(QStringLiteral("protocolVersion"), 12);
            message.insert(QStringLiteral("serverBootId"), bootId);
            completeV7TestEnvelope(message);
            if (message.value(QStringLiteral("messageId")).toString().isEmpty()) {
                message.insert(
                    QStringLiteral("messageId"),
                    QUuid::createUuid().toString(QUuid::WithoutBraces));
            }
            peer->sendTextMessage(QString::fromUtf8(
                QJsonDocument(message).toJson(QJsonDocument::Compact)));
        };

        connect(&server, &QWebSocketServer::newConnection, this, [&]() {
            peer = server.nextPendingConnection();
            QVERIFY(peer);
            peer->setParent(&server);
            sendServerMessage(QJsonObject{
                {QStringLiteral("type"), QStringLiteral("auth_challenge")},
                {QStringLiteral("nonce"), QString::fromLatin1(
                    QByteArray(32, 'n').toBase64(
                        QByteArray::Base64UrlEncoding
                        | QByteArray::OmitTrailingEquals))},
                {QStringLiteral("issuedAt"), 1}
            });

            connect(peer, &QWebSocket::textMessageReceived, &server,
                    [&](const QString& encoded) {
                const QJsonObject message =
                    QJsonDocument::fromJson(encoded.toUtf8()).object();
                const QString type =
                    message.value(QStringLiteral("type")).toString();
                if (type == QLatin1String("auth_response")) {
                    authentication = message;
                    const QJsonObject policy{
                        {QStringLiteral("policyVersion"), 5},
                        {QStringLiteral("transportTimeoutMs"), 5000},
                        {QStringLiteral("heartbeatIntervalMs"), 1'000},
                        {QStringLiteral("transportSuspectAfterMs"), 2000},
                        {QStringLiteral("sessionRecoveryTimeoutMs"), 10'000},
                        {QStringLiteral("leaseTimeoutMs"), 2000},
                        {QStringLiteral("scenePrepareTimeoutMs"), 15'000},
                        {QStringLiteral("sceneActivationLeadMs"), 4'000},
                        {QStringLiteral("sceneMaxClockSkewMs"), 50},
                        {QStringLiteral("sceneStartedAckTimeoutMs"), 5'000},
                        {QStringLiteral("sceneMaxStartSkewMs"), 750},
                        {QStringLiteral("sceneStopTimeoutMs"), 5'000},
                        {QStringLiteral("uploadIdleTimeoutMs"), 45'000},
                        {QStringLiteral("uploadTargetAckTimeoutMs"), 30'000},
                        {QStringLiteral("removalAckTimeoutMs"), 30'000}
                    };
                    sendServerMessage(QJsonObject{
                        {QStringLiteral("type"), QStringLiteral("welcome")},
                        {QStringLiteral("connectionId"),
                         QUuid::createUuid().toString(QUuid::WithoutBraces)},
                        {QStringLiteral("installationId"),
                         message.value(QStringLiteral("installationId"))},
                        {QStringLiteral("endpointId"), ownerEndpointId},
                        {QStringLiteral("instanceId"),
                         message.value(QStringLiteral("instanceId"))},
                        {QStringLiteral("instanceOrdinal"),
                         message.value(QStringLiteral("instanceOrdinal"))},
                        {QStringLiteral("runtimeId"),
                         message.value(QStringLiteral("runtimeId"))},
                        {QStringLiteral("connectionGeneration"), 1},
                        {QStringLiteral("policy"), policy},
                        {QStringLiteral("serverMonotonicMs"), 1}
                    });
                } else if (type == QLatin1String("heartbeat")) {
                    sendServerMessage(QJsonObject{
                        {QStringLiteral("type"), QStringLiteral("heartbeat_ack")},
                        {QStringLiteral("connectionGeneration"), 1},
                        {QStringLiteral("sequence"),
                         message.value(QStringLiteral("sequence"))},
                        {QStringLiteral("clientMonotonicMs"),
                         message.value(QStringLiteral("clientMonotonicMs"))},
                        {QStringLiteral("serverMonotonicMs"),
                         message.value(QStringLiteral("clientMonotonicMs"))},
                        {QStringLiteral("serverEpochMs"),
                         QDateTime::currentMSecsSinceEpoch()}
                    });
                } else if (type == QLatin1String("endpoint_snapshot")) {
                    QJsonObject snapshot = message;
                    for (const QString& key : {QStringLiteral("installationId"),
                                              QStringLiteral("instanceId"),
                                              QStringLiteral("instanceOrdinal"),
                                              QStringLiteral("runtimeId")})
                        snapshot.insert(key, authentication.value(key));
                    snapshot.insert(QStringLiteral("endpointId"), ownerEndpointId);
                    sendServerMessage(QJsonObject{{"type", "endpoint_snapshot_applied"},
                        {"snapshot", snapshot}, {"connectionGeneration", 1}});
                } else if (type == QLatin1String("remote_session_reconcile")) {
                    sendServerMessage(QJsonObject{{"type", "remote_session_reconciled"},
                        {"requestId", message.value("requestId")}, {"sessions", QJsonArray{}},
                        {"complete", true}, {"absentSessionIds", QJsonArray{}}});
                } else if (type == QLatin1String("remote_session_open")) {
                    openCommands.append(message);
                } else if (type == QLatin1String("remote_session_close")) {
                    closeCommands.append(message);
                }
            });
        });

        QSignalSpy connectedSpy(runtime.getWebSocketClient(),
                                &WebSocketClient::connected);
        runtime.getWebSocketClient()->connectToServer(
            QStringLiteral("ws://127.0.0.1:%1").arg(server.serverPort()));
        QTRY_COMPARE_WITH_TIMEOUT(connectedSpy.count(), 1, 2'000);
        QTRY_COMPARE(runtime.findChild<ConnectionManager*>()->state(),
                     ConnectionManager::State::Connected);

        ClientInfo client = onlineClient(targetEndpointId,
                                         QStringLiteral("Windows B"));
        client.setScreens({});
        client.setVolumePercent(-1);
        runtime.buildDisplayClientList({client});
        runtime.activateClient(targetEndpointId);
        QTRY_COMPARE_WITH_TIMEOUT(openCommands.size(), 1, 1'000);

        auto snapshot = [](const ScreenInfo& screen, int volume,
                           quint64 revision) {
            return QJsonObject{
                {QStringLiteral("screens"), QJsonArray{screen.toJson()}},
                {QStringLiteral("systemUI"), QJsonArray{}},
                {QStringLiteral("volumePercent"), volume},
                {QStringLiteral("revision"), static_cast<double>(revision)},
                {QStringLiteral("capturedAtEpochMs"),
                 static_cast<double>(QDateTime::currentMSecsSinceEpoch())}
            };
        };
        auto openedEnvelope = [&](const QString& sessionId,
                                  const QString& requestId,
                                  const ScreenInfo& screen,
                                  int volume) {
            return QJsonObject{
                {QStringLiteral("type"), QStringLiteral("remote_session_opened")},
                {QStringLiteral("requestId"), requestId},
                {QStringLiteral("remoteSessionId"), sessionId},
                {QStringLiteral("generation"), 1},
                {QStringLiteral("ownerConnectionGeneration"), 1},
                {QStringLiteral("targetConnectionGeneration"), 1},
                {QStringLiteral("phase"), QStringLiteral("Active")},
                {QStringLiteral("ownerEndpointId"), ownerEndpointId},
                {QStringLiteral("targetEndpointId"), targetEndpointId},
                {QStringLiteral("resumeToken"),
                 QStringLiteral("resume-token-") + sessionId},
                {QStringLiteral("snapshotSequence"), 1},
                {QStringLiteral("snapshot"), snapshot(screen, volume, 1)}
            };
        };

        const ScreenInfo firstScreen(7, 2560, 1440, -2560, 0, true);
        QSignalSpy openedSpy(runtime.getWebSocketClient(),
                             &WebSocketClient::remoteSessionOpened);
        sendServerMessage(openedEnvelope(
            firstSessionId,
            openCommands.constFirst().value(QStringLiteral("requestId")).toString(),
            firstScreen, 47));
        QTRY_COMPARE_WITH_TIMEOUT(openedSpy.count(), 1, 1'000);
        QTRY_VERIFY_WITH_TIMEOUT(runtime.activeProjectExists(), 1'000);
        QVERIFY(runtime.getActiveCanvas());
        QCOMPARE(runtime.remoteVolumePercent(), 47);
        QCOMPARE(runtime.remoteStatusText(), QStringLiteral("CONNECTED"));
        const QString firstProjectId = runtime.findWorkspace(targetEndpointId)->projectId;
        QVERIFY(!firstProjectId.isEmpty());

        const qint64 hiddenAt = QDateTime::currentMSecsSinceEpoch();
        runtime.getWorkspaceManager()->markAllWorkspacesHidden(hiddenAt);
        runtime.getProjectManager()->markAllHidden(hiddenAt);
        const qint64 closeAt =
            runtime.getWorkspaceManager()->remoteSessionCloseAtMs(targetEndpointId);
        const qint64 deleteAt =
            runtime.getProjectManager()->projectDeleteAtMs(targetEndpointId);
        QVERIFY(closeAt > hiddenAt);
        QVERIFY(deleteAt > closeAt);

        runtime.getWorkspaceManager()->processDeadlines(closeAt - 1);
        QCOMPARE(closeCommands.size(), 0);
        runtime.getWorkspaceManager()->processDeadlines(closeAt);
        QTRY_COMPARE_WITH_TIMEOUT(closeCommands.size(), 1, 1'000);
        QCOMPARE(closeCommands.constFirst()
                     .value(QStringLiteral("remoteSessionId")).toString(),
                 firstSessionId);
        QVERIFY(!runtime.isRemoteClientConnected());
        QVERIFY(runtime.getWorkspaceManager()->remoteSessionState(targetEndpointId)
                != WorkspaceManager::RemoteSessionState::Active);

        // This can already be queued when the local inactivity deadline sends
        // CLOSE. Local terminal intent must dominate the stale Active replay.
        QSignalSpy leaseStateSpy(runtime.getWebSocketClient(),
                                 &WebSocketClient::remoteSessionLeaseStateChanged);
        sendServerMessage(QJsonObject{
            {QStringLiteral("type"), QStringLiteral("remote_session_lease_state")},
            {QStringLiteral("remoteSessionId"), firstSessionId},
            {QStringLiteral("generation"), 1},
            {QStringLiteral("ownerConnectionGeneration"), 1},
            {QStringLiteral("targetConnectionGeneration"), 1},
            {QStringLiteral("phase"), QStringLiteral("Active")},
            {QStringLiteral("state"), QStringLiteral("Active")},
            {QStringLiteral("ownerEndpointId"), ownerEndpointId},
            {QStringLiteral("targetEndpointId"), targetEndpointId}
        });
        QTRY_COMPARE_WITH_TIMEOUT(leaseStateSpy.count(), 1, 1'000);
        QVERIFY(!runtime.isRemoteClientConnected());
        QCOMPARE(runtime.remoteStatusText(), QStringLiteral("DISCONNECTING"));
        QVERIFY(runtime.getWorkspaceManager()->remoteSessionState(targetEndpointId)
                != WorkspaceManager::RemoteSessionState::Active);
        const QList<ClientInfo> afterLateActive = runtime.displayClients();
        QCOMPARE(afterLateActive.size(), 1);
        QCOMPARE(afterLateActive.constFirst().endpointId(), targetEndpointId);
        QCOMPARE(afterLateActive.constFirst().availabilityBadgeText(),
                 QStringLiteral("Disconnecting"));

        runtime.getProjectManager()->processDeadlines(deleteAt - 1);
        QVERIFY(runtime.getProjectManager()->hasProjectForTarget(targetEndpointId));
        runtime.getProjectManager()->processDeadlines(deleteAt);
        QTRY_VERIFY_WITH_TIMEOUT(
            !runtime.getProjectManager()->hasProjectForTarget(targetEndpointId),
            1'000);
        QVERIFY(!runtime.getNavigationManager()->isOnScreenView());
        QVERIFY(runtime.activeWorkspaceEndpointId().isEmpty());
        QVERIFY(!runtime.findWorkspace(targetEndpointId));
        QVERIFY(!runtime.getActiveCanvas());
        QCOMPARE(closeCommands.size(), 1);
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);

        // A click made while session 1 is still awaiting its server tombstone
        // is remembered, but it cannot reuse that terminal binding.
        runtime.activateClient(targetEndpointId);
        QVERIFY(runtime.getNavigationManager()->isOnScreenView());
        QCOMPARE(runtime.activeWorkspaceEndpointId(), targetEndpointId);
        QVERIFY(!runtime.activeProjectExists());
        QCOMPARE(openCommands.size(), 1);

        QSignalSpy closedSpy(runtime.getWebSocketClient(),
                             &WebSocketClient::remoteSessionClosed);
        sendServerMessage(QJsonObject{
            {QStringLiteral("type"), QStringLiteral("remote_session_closed")},
            {QStringLiteral("remoteSessionId"), firstSessionId},
            {QStringLiteral("generation"), 1},
            {QStringLiteral("ownerConnectionGeneration"), 1},
            {QStringLiteral("targetConnectionGeneration"), 1},
            {QStringLiteral("phase"), QStringLiteral("Closed")},
            {QStringLiteral("cleanupState"), QStringLiteral("confirmed")},
            {QStringLiteral("teardownId"), teardownId},
            {QStringLiteral("ownerEndpointId"), ownerEndpointId},
            {QStringLiteral("targetEndpointId"), targetEndpointId}
        });
        QTRY_COMPARE_WITH_TIMEOUT(closedSpy.count(), 1, 1'000);
        QTRY_COMPARE_WITH_TIMEOUT(openCommands.size(), 2, 1'000);
        QTest::qWait(25);
        QCOMPARE(openCommands.size(), 2);
        QCOMPARE(openCommands.constLast()
                     .value(QStringLiteral("targetEndpointId")).toString(),
                 targetEndpointId);

        const ScreenInfo secondScreen(11, 3840, 2160, 0, 0, true);
        sendServerMessage(openedEnvelope(
            secondSessionId,
            openCommands.constLast().value(QStringLiteral("requestId")).toString(),
            secondScreen, 73));
        QTRY_COMPARE_WITH_TIMEOUT(openedSpy.count(), 2, 1'000);
        QTRY_VERIFY_WITH_TIMEOUT(runtime.activeProjectExists(), 1'000);

        ApplicationRuntime::ClientWorkspace* reopened =
            runtime.findWorkspace(targetEndpointId);
        QVERIFY(reopened);
        QVERIFY(reopened->canvas);
        QVERIFY(runtime.getActiveCanvas() == reopened->canvas);
        QVERIFY(reopened->projectId != firstProjectId);
        QCOMPARE(reopened->lastClientInfo.getScreens().size(), 1);
        QCOMPARE(reopened->lastClientInfo.getScreens().constFirst().id, 11);
        QCOMPARE(runtime.remoteVolumePercent(), 73);
        QCOMPARE(runtime.remoteStatusText(), QStringLiteral("CONNECTED"));
        QVERIFY(runtime.activeRemoteSessionExists());
        QVERIFY(runtime.isRemoteOverlayActionsEnabled());
        QVERIFY(runtime.canDeleteActiveProject());
        QVERIFY(!runtime.getNavigationManager()->isLoading());
        QVERIFY(runtime.getNavigationManager()->canvasVisible());

        // Old-session traffic remains fenced even after a new binding for the
        // same peer is active.
        sendServerMessage(QJsonObject{
            {QStringLiteral("type"), QStringLiteral("remote_session_lease_state")},
            {QStringLiteral("remoteSessionId"), firstSessionId},
            {QStringLiteral("generation"), 1},
            {QStringLiteral("ownerConnectionGeneration"), 1},
            {QStringLiteral("targetConnectionGeneration"), 1},
            {QStringLiteral("phase"), QStringLiteral("Active")},
            {QStringLiteral("state"), QStringLiteral("Active")},
            {QStringLiteral("ownerEndpointId"), ownerEndpointId},
            {QStringLiteral("targetEndpointId"), targetEndpointId}
        });
        QTest::qWait(25);
        QCOMPARE(runtime.getWebSocketClient()->remoteSessionCoordinator()
                     ->outgoingForPeer(targetEndpointId).remoteSessionId,
                 secondSessionId);
        QCOMPARE(runtime.remoteVolumePercent(), 73);
        QCOMPARE(openCommands.size(), 2);

        runtime.handleApplicationAboutToQuit();
        runtime.getNavigationManager()->setActiveCanvas(nullptr);
        runtime.setActiveCanvas(nullptr);
        delete reopened->canvas;
        reopened->canvas = nullptr;
    }

    void pointerPresenceKeepsAllProjectsAndSessionsAliveUntilItLeaves()
    {
        QTemporaryDir root;
        QVERIFY(root.isValid());

        RuntimeProfileContext context;
        context.ordinal = 2;
        context.instanceId = QStringLiteral("activity-multiple-projects");
        context.profileId = QStringLiteral("activity-multiple-projects");
        context.rootPath = root.path();
        context.persistent = false;
        RuntimeProfile::configure(context);

        ApplicationRuntime runtime(context);
        auto& residency = MediaResidencyManager::instance();
        constexpr quint64 GiB = 1024ULL * 1024 * 1024;
        residency.setMemorySnapshotForTesting({16 * GiB, 12 * GiB, 128 * 1024 * 1024});
        const auto restoreMemory = qScopeGuard([&] { residency.clearMemorySnapshotForTesting(); });
        WorkspaceManager* workspaces = runtime.getWorkspaceManager();
        ProjectManager* projects = runtime.getProjectManager();
        workspaces->stopAutomaticTimersForTesting();
        projects->stopAutomaticTimersForTesting();
        workspaces->setRemoteSessionHiddenTimeoutMs(
            projects->timingPolicy().projectMediaHiddenTimeoutMs + 60'000);

        qint64 nowMs = 1'000'000;
        ApplicationActivityMonitor* activity =
            runtime.findChild<ApplicationActivityMonitor*>();
        QVERIFY(activity);
        activity->setNowProviderForTesting([&nowMs]() { return nowMs; });
        workspaces->setNowProviderForTesting([&nowMs]() { return nowMs; });
        projects->setNowProviderForTesting([&nowMs]() { return nowMs; });
        runtime.setQmlWindowVisible(true);
        runtime.setPointerInsideControlWindow(true);
        QVERIFY(activity->isActive());

        RemoteSessionTestServer server(runtime.getWebSocketClient()->endpointId());
        QVERIFY(server.listen());
        ConnectionManager* connections = runtime.findChild<ConnectionManager*>();
        QVERIFY(connections);
        QSignalSpy connectedSpy(runtime.getWebSocketClient(),
                                &WebSocketClient::connected);
        connections->connectToServer(server.url());
        QTRY_COMPARE_WITH_TIMEOUT(connectedSpy.count(), 1, 2'000);

        const QStringList targets{
            fixtureEndpoint(QLatin1Char('A')), fixtureEndpoint(QLatin1Char('B'))};
        QJsonArray clients;
        for (qsizetype i = 0; i < targets.size(); ++i) {
            clients.append(onlineClient(
                targets.at(i), QStringLiteral("Activity target %1").arg(i)).toJson());
        }
        QVERIFY(server.send(QJsonObject{
            {QStringLiteral("type"), QStringLiteral("client_list")},
            {QStringLiteral("clients"), clients}
        }));
        QTRY_COMPARE_WITH_TIMEOUT(runtime.displayClients().size(), 2, 1'000);

        for (qsizetype i = 0; i < targets.size(); ++i) {
            runtime.activateClient(targets.at(i));
            QTRY_COMPARE_WITH_TIMEOUT(server.openCommands.size(), i + 1, 1'000);
            QVERIFY(server.sendOpened(
                QStringLiteral("activity-session-%1").arg(i),
                server.openCommands.at(i).value(QStringLiteral("requestId")).toString(),
                targets.at(i), ScreenInfo(81 + i, 1920, 1080, 0, 0, true), 46));
            QTRY_VERIFY_WITH_TIMEOUT(projects->hasProjectForTarget(targets.at(i)), 1'000);
            QTRY_COMPARE_WITH_TIMEOUT(workspaces->remoteSessionState(targets.at(i)),
                                     WorkspaceManager::RemoteSessionState::Active, 1'000);
        }

        QImage image(16, 16, QImage::Format_ARGB32);
        image.fill(Qt::green);
        const QString source = root.filePath(QStringLiteral("inactivity.png"));
        QVERIFY(image.save(source));
        QList<CanvasMedia*> media;
        for (const QString& target : targets) {
            CanvasDocument* document = runtime.findWorkspace(target)->canvas->document();
            media.append(document->addPreparedFile(source, image.size(), false, {}));
            QVERIFY(media.last());
            QTRY_VERIFY_WITH_TIMEOUT(media.last()->residencyReady(), 5'000);
        }
        QSignalSpy mediaReleaseSpy(projects, &ProjectManager::projectMediaReleaseDue);
        QSignalSpy closeDueSpy(workspaces, &WorkspaceManager::remoteSessionCloseDue);
        QSignalSpy projectDeletedSpy(projects, &ProjectManager::projectDeleted);
        const qint64 retentionMs = projects->timingPolicy().projectHiddenRetentionMs;
        // No new pointer event is required while it remains inside. Also,
        // losing keyboard focus must not change this pointer-based policy.
        runtime.handleApplicationStateChanged(Qt::ApplicationInactive);
        QVERIFY(activity->isActive());
        nowMs += qMax(retentionMs, workspaces->remoteSessionHiddenTimeoutMs()) + 1;
        workspaces->processDeadlines(nowMs);
        projects->processDeadlines(nowMs);
        for (const QString& target : targets) {
            QCOMPARE(workspaces->remoteSessionCloseAtMs(target), qint64(-1));
            QCOMPARE(projects->projectDeleteAtMs(target), qint64(-1));
            QCOMPARE(workspaces->remoteSessionState(target),
                     WorkspaceManager::RemoteSessionState::Active);
            QVERIFY(projects->hasProjectForTarget(target));
        }
        QCOMPARE(closeDueSpy.count(), 0);
        QCOMPARE(mediaReleaseSpy.count(), 0);
        QCOMPARE(projectDeletedSpy.count(), 0);

        // A single departure starts all deadlines for every project,
        // including the project that is no longer the selected page.
        const qint64 leftAtMs = nowMs;
        runtime.setPointerInsideControlWindow(false);
        QVERIFY(!activity->isActive());
        ++nowMs;
        runtime.setPointerInsideControlWindow(false);
        for (const QString& target : targets) {
            QCOMPARE(workspaces->remoteSessionCloseAtMs(target),
                     leftAtMs + workspaces->remoteSessionHiddenTimeoutMs());
            QCOMPARE(projects->projectDeleteAtMs(target), leftAtMs + retentionMs);
            QCOMPARE(projects->projectMediaReleaseAtMs(target),
                     leftAtMs + projects->timingPolicy().projectMediaHiddenTimeoutMs);
        }

        // Returning before expiry cancels all deadlines everywhere; even
        // advancing past their former expiry cannot close or delete anything.
        runtime.setPointerInsideControlWindow(true);
        QVERIFY(activity->isActive());
        for (const QString& target : targets) {
            QCOMPARE(workspaces->remoteSessionCloseAtMs(target), qint64(-1));
            QCOMPARE(projects->projectDeleteAtMs(target), qint64(-1));
            QCOMPARE(projects->projectMediaReleaseAtMs(target), qint64(-1));
        }
        nowMs += qMax(retentionMs, workspaces->remoteSessionHiddenTimeoutMs()) + 1;
        workspaces->processDeadlines(nowMs);
        projects->processDeadlines(nowMs);
        QCOMPARE(closeDueSpy.count(), 0);
        QCOMPARE(projectDeletedSpy.count(), 0);
        QCOMPARE(projects->projectCount(), 2);
        for (const QString& target : targets) {
            QCOMPARE(workspaces->remoteSessionState(target),
                     WorkspaceManager::RemoteSessionState::Active);
        }
        QCOMPARE(server.closeCommands.size(), 0);
        QCOMPARE(mediaReleaseSpy.count(), 0);

        // Each project sheds only its own leases. A scene in the second
        // project keeps the shared asset pinned until its draft is restored.
        ICanvasHost* playingCanvas = runtime.findWorkspace(targets.last())->canvas;
        playingCanvas->triggerTestSceneAction();
        QVERIFY(playingCanvas->testSceneLaunched());
        QVERIFY(playingCanvas->document()->editsLocked());
        runtime.setPointerInsideControlWindow(false);
        const qint64 mediaDeadline = projects->projectMediaReleaseAtMs(targets.first());
        QVERIFY(mediaDeadline > nowMs);
        nowMs = mediaDeadline - 1;
        projects->processDeadlines(nowMs);
        QCOMPARE(mediaReleaseSpy.count(), 0);
        QVERIFY(media.first()->residencyReady());
        ++nowMs;
        projects->processDeadlines(nowMs);
        QCOMPARE(mediaReleaseSpy.count(), 2);
        QVERIFY(!media.first()->residencyReady());
        QVERIFY(media.last()->residencyReady());
        QVERIFY(playingCanvas->testSceneLaunched());
        QVERIFY(!playingCanvas->document()->mediaResidencySuspended());
        QCOMPARE(projects->projectCount(), 2);
        QCOMPARE(server.closeCommands.size(), 0);
        for (const QString& target : targets) {
            QCOMPARE(workspaces->remoteSessionState(target),
                     WorkspaceManager::RemoteSessionState::Active);
            QCOMPARE(projects->projectForTarget(target)->mediaReferences.size(), 1);
        }

        playingCanvas->triggerTestSceneAction();
        QVERIFY(!playingCanvas->testSceneLaunched());
        QVERIFY(playingCanvas->document()->mediaResidencySuspended());
        QVERIFY(!media.last()->residencyReady());
        QVERIFY(!residency.asset(media.first()->residencyOwnerId()));
        QVERIFY(!residency.asset(media.last()->residencyOwnerId()));
        QVERIFY(QFileInfo::exists(source));
        projects->processDeadlines(nowMs);
        QCOMPARE(mediaReleaseSpy.count(), 2);

        // Pointer return rehydrates both existing graphs and cancels all
        // inactivity deadlines without creating another project or session.
        runtime.setPointerInsideControlWindow(true);
        for (qsizetype i = 0; i < targets.size(); ++i) {
            const auto* workspace = runtime.findWorkspace(targets.at(i));
            QVERIFY(!workspace->canvas->document()->mediaResidencySuspended());
            QCOMPARE(workspace->canvas->document()->media().size(), 1);
            QTRY_VERIFY_WITH_TIMEOUT(media.at(i)->residencyReady(), 5'000);
            QCOMPARE(projects->projectMediaReleaseAtMs(targets.at(i)), qint64(-1));
        }
        QCOMPARE(server.closeCommands.size(), 0);

        runtime.handleApplicationAboutToQuit();
        runtime.getNavigationManager()->setActiveCanvas(nullptr);
        runtime.setActiveCanvas(nullptr);
        for (const QString& target : targets) {
            ApplicationRuntime::ClientWorkspace* workspace = runtime.findWorkspace(target);
            QVERIFY(workspace && workspace->canvas);
            delete workspace->canvas;
            workspace->canvas = nullptr;
        }
    }

    void returningActivityReopensTheForegroundSessionAfterInactivityTimeout()
    {
        QTemporaryDir root;
        QVERIFY(root.isValid());

        RuntimeProfileContext context;
        context.ordinal = 2;
        context.instanceId = QStringLiteral("activity-resume-reopens-session");
        context.profileId = QStringLiteral("activity-resume-reopens-session");
        context.rootPath = root.path();
        context.persistent = false;
        RuntimeProfile::configure(context);

        ApplicationRuntime runtime(context);
        runtime.getWorkspaceManager()->stopAutomaticTimersForTesting();
        runtime.getProjectManager()->stopAutomaticTimersForTesting();
        runtime.getWorkspaceManager()->setRemoteSessionHiddenTimeoutMs(10);

        qint64 nowMs = 1'000'000;
        ApplicationActivityMonitor* activity =
            runtime.findChild<ApplicationActivityMonitor*>();
        QVERIFY(activity);
        activity->setNowProviderForTesting([&nowMs]() { return nowMs; });
        runtime.getWorkspaceManager()->setNowProviderForTesting(
            [&nowMs]() { return nowMs; });
        runtime.getProjectManager()->setNowProviderForTesting(
            [&nowMs]() { return nowMs; });
        runtime.setQmlWindowVisible(true);
        runtime.setPointerInsideControlWindow(true);

        RemoteSessionTestServer server(
            runtime.getWebSocketClient()->endpointId());
        QVERIFY(server.listen());
        ConnectionManager* connections =
            runtime.findChild<ConnectionManager*>();
        QVERIFY(connections);
        QSignalSpy connectedSpy(runtime.getWebSocketClient(),
                                &WebSocketClient::connected);
        connections->connectToServer(server.url());
        QTRY_COMPARE_WITH_TIMEOUT(connectedSpy.count(), 1, 2'000);

        const QString targetEndpointId = fixtureEndpoint(QLatin1Char('I'));
        const QString firstSessionId = QStringLiteral("inactivity-auto-session-1");
        ClientInfo client = onlineClient(
            targetEndpointId, QStringLiteral("Inactivity resume target"));
        client.setScreens({});
        client.setVolumePercent(-1);
        QVERIFY(server.sendClientList(client));
        QTRY_COMPARE_WITH_TIMEOUT(runtime.displayClients().size(), 1, 1'000);

        runtime.activateClient(targetEndpointId);
        QTRY_COMPARE_WITH_TIMEOUT(server.openCommands.size(), 1, 1'000);
        QVERIFY(server.sendOpened(
            firstSessionId,
            server.openCommands.constFirst()
                .value(QStringLiteral("requestId")).toString(),
            targetEndpointId, ScreenInfo(81, 1920, 1080, 0, 0, true), 46));
        QTRY_VERIFY_WITH_TIMEOUT(runtime.activeProjectExists(), 1'000);
        QVERIFY(runtime.getActiveCanvas());
        QVERIFY(runtime.isRemoteClientConnected());

        // Leaving the app starts both deadlines. Let only the shorter session
        // deadline expire; the Project and foreground page remain alive.
        nowMs += 1'000;
        runtime.setPointerInsideControlWindow(false);
        const qint64 closeAt =
            runtime.getWorkspaceManager()->remoteSessionCloseAtMs(
                targetEndpointId);
        QVERIFY(closeAt > nowMs);
        nowMs = closeAt;
        runtime.getWorkspaceManager()->processDeadlines(nowMs);
        QTRY_COMPARE_WITH_TIMEOUT(server.closeCommands.size(), 1, 1'000);
        QCOMPARE(server.closeCommands.constFirst()
                     .value(QStringLiteral("remoteSessionId")).toString(),
                 firstSessionId);
        QVERIFY(runtime.activeProjectExists());
        QVERIFY(runtime.getActiveCanvas());
        QVERIFY(!runtime.isRemoteClientConnected());
        QCOMPARE(runtime.remoteStatusText(), QStringLiteral("DISCONNECTING"));

        // Pointer activity is the automatic reopen intent. Since CLOSE1 is
        // not committed yet it must not emit OPEN2 prematurely.
        ++nowMs;
        runtime.setPointerInsideControlWindow(true);
        QCOMPARE(server.openCommands.size(), 1);
        QCOMPARE(runtime.remoteStatusText(), QStringLiteral("DISCONNECTING"));
        QVERIFY(!runtime.isRemoteClientConnected());
        QVERIFY(!runtime.isRemoteOverlayActionsEnabled());

        QVERIFY(server.sendClosed(firstSessionId, targetEndpointId));
        QTRY_COMPARE_WITH_TIMEOUT(server.openCommands.size(), 2, 1'000);
        const QString secondRequestId = server.openCommands.constLast()
            .value(QStringLiteral("requestId")).toString();
        QVERIFY(!secondRequestId.isEmpty());
        QCOMPARE(server.openCommands.constLast()
                     .value(QStringLiteral("targetEndpointId")).toString(),
                 targetEndpointId);

        const QString secondSessionId =
            QStringLiteral("inactivity-auto-session-2");
        QVERIFY(server.sendOpened(
            secondSessionId, secondRequestId,
            targetEndpointId, ScreenInfo(82, 2560, 1440, 0, 0, true), 74));
        QTRY_VERIFY_WITH_TIMEOUT(runtime.isRemoteClientConnected(), 1'000);
        QCOMPARE(runtime.remoteStatusText(), QStringLiteral("CONNECTED"));
        QCOMPARE(runtime.remoteVolumePercent(), 74);
        QVERIFY(runtime.activeProjectExists());
        QVERIFY(runtime.getActiveCanvas());
        QCOMPARE(runtime.findWorkspace(targetEndpointId)
                     ->lastClientInfo.getScreens().constFirst().id,
                 82);
        QCOMPARE(server.openCommands.size(), 2);

        // Exercise the opposite network ordering as well: CLOSE2 commits
        // before activity returns, so the activity edge must OPEN3 directly.
        nowMs += 1'000;
        runtime.setPointerInsideControlWindow(false);
        const qint64 secondCloseAt =
            runtime.getWorkspaceManager()->remoteSessionCloseAtMs(
                targetEndpointId);
        QVERIFY(secondCloseAt > nowMs);
        nowMs = secondCloseAt;
        runtime.getWorkspaceManager()->processDeadlines(nowMs);
        QTRY_COMPARE_WITH_TIMEOUT(server.closeCommands.size(), 2, 1'000);
        QCOMPARE(server.closeCommands.constLast()
                     .value(QStringLiteral("remoteSessionId")).toString(),
                 secondSessionId);
        QVERIFY(server.sendClosed(secondSessionId, targetEndpointId));
        QTRY_COMPARE_WITH_TIMEOUT(runtime.remoteStatusText(),
                                  QStringLiteral("AVAILABLE"), 1'000);
        QCOMPARE(server.openCommands.size(), 2);

        ++nowMs;
        runtime.setPointerInsideControlWindow(true);
        QTRY_COMPARE_WITH_TIMEOUT(server.openCommands.size(), 3, 1'000);
        const QString thirdRequestId = server.openCommands.constLast()
            .value(QStringLiteral("requestId")).toString();
        QVERIFY(!thirdRequestId.isEmpty());
        QVERIFY(server.sendOpened(
            QStringLiteral("inactivity-auto-session-3"), thirdRequestId,
            targetEndpointId, ScreenInfo(83, 3440, 1440, 0, 0, true), 88));
        QTRY_VERIFY_WITH_TIMEOUT(runtime.isRemoteClientConnected(), 1'000);
        QCOMPARE(runtime.remoteStatusText(), QStringLiteral("CONNECTED"));
        QCOMPARE(runtime.remoteVolumePercent(), 88);
        QCOMPARE(runtime.findWorkspace(targetEndpointId)
                     ->lastClientInfo.getScreens().constFirst().id,
                 83);
        QCOMPARE(server.openCommands.size(), 3);

        ApplicationRuntime::ClientWorkspace* workspace =
            runtime.findWorkspace(targetEndpointId);
        runtime.handleApplicationAboutToQuit();
        runtime.getNavigationManager()->setActiveCanvas(nullptr);
        runtime.setActiveCanvas(nullptr);
        QVERIFY(workspace && workspace->canvas);
        delete workspace->canvas;
        workspace->canvas = nullptr;
    }

    void cancelledInitialOpenDoesNotPoisonTheNextSession()
    {
        QTemporaryDir root;
        QVERIFY(root.isValid());

        RuntimeProfileContext context;
        context.ordinal = 2;
        context.instanceId = QStringLiteral("cancelled-open-correlation");
        context.profileId = QStringLiteral("cancelled-open-correlation");
        context.rootPath = root.path();
        context.persistent = false;
        RuntimeProfile::configure(context);

        ApplicationRuntime runtime(context);
        runtime.getWorkspaceManager()->stopAutomaticTimersForTesting();
        runtime.getProjectManager()->stopAutomaticTimersForTesting();

        RemoteSessionTestServer server(
            runtime.getWebSocketClient()->endpointId());
        QVERIFY(server.listen());
        ConnectionManager* connections =
            runtime.findChild<ConnectionManager*>();
        QVERIFY(connections);
        QSignalSpy connectedSpy(runtime.getWebSocketClient(),
                                &WebSocketClient::connected);
        connections->connectToServer(server.url());
        QTRY_COMPARE_WITH_TIMEOUT(connectedSpy.count(), 1, 2'000);

        const QString targetEndpointId = fixtureEndpoint(QLatin1Char('D'));
        ClientInfo client = onlineClient(
            targetEndpointId, QStringLiteral("Cancelled-open target"));
        client.setScreens({});
        client.setVolumePercent(-1);
        QVERIFY(server.sendClientList(client));
        QTRY_COMPARE_WITH_TIMEOUT(runtime.displayClients().size(), 1, 1'000);

        runtime.activateClient(targetEndpointId);
        QTRY_COMPARE_WITH_TIMEOUT(server.openCommands.size(), 1, 1'000);
        QVERIFY(!runtime.activeProjectExists());
        QVERIFY(runtime.getNavigationManager()->isLoading());

        // The user leaves before the target accepts Open1. The server can
        // expire that Opening transaction directly, without ever emitting
        // Opened1. Its terminal result must retire only Open1's cancellation.
        runtime.navigateToClients();
        QVERIFY(!runtime.getNavigationManager()->isOnScreenView());
        QSignalSpy closedSpy(runtime.getWebSocketClient(),
                            &WebSocketClient::remoteSessionClosed);
        QVERIFY(server.sendClosed(QStringLiteral("cancelled-session-1"),
                                  targetEndpointId));
        QTRY_COMPARE_WITH_TIMEOUT(closedSpy.count(), 1, 1'000);

        runtime.activateClient(targetEndpointId);
        QVERIFY(runtime.getNavigationManager()->isOnScreenView());
        QTRY_COMPARE_WITH_TIMEOUT(server.openCommands.size(), 2, 1'000);

        const ScreenInfo screen(31, 2560, 1440, 0, 0, true);
        QVERIFY(server.sendOpened(
            QStringLiteral("cancelled-session-2"),
            server.openCommands.constLast()
                .value(QStringLiteral("requestId")).toString(),
            targetEndpointId, screen, 61));
        QTRY_VERIFY_WITH_TIMEOUT(runtime.activeProjectExists(), 1'000);
        QTRY_VERIFY_WITH_TIMEOUT(runtime.getActiveCanvas(), 1'000);
        QCOMPARE(runtime.remoteStatusText(), QStringLiteral("CONNECTED"));
        QCOMPARE(runtime.remoteVolumePercent(), 61);
        QCOMPARE(runtime.findWorkspace(targetEndpointId)
                     ->lastClientInfo.getScreens().size(), 1);
        QCOMPARE(runtime.findWorkspace(targetEndpointId)
                     ->lastClientInfo.getScreens().constFirst().id, 31);
        QCOMPARE(server.openCommands.size(), 2);
        QCOMPARE(server.closeCommands.size(), 0);

        ApplicationRuntime::ClientWorkspace* workspace =
            runtime.findWorkspace(targetEndpointId);
        runtime.handleApplicationAboutToQuit();
        runtime.getNavigationManager()->setActiveCanvas(nullptr);
        runtime.setActiveCanvas(nullptr);
        QVERIFY(workspace && workspace->canvas);
        delete workspace->canvas;
        workspace->canvas = nullptr;
    }

    void cancelledUnacknowledgedOpenIsReconciledAfterTransportReplacement()
    {
        QTemporaryDir root;
        QVERIFY(root.isValid());

        RuntimeProfileContext context;
        context.ordinal = 2;
        context.instanceId = QStringLiteral("cancelled-open-transport-replay");
        context.profileId = QStringLiteral("cancelled-open-transport-replay");
        context.rootPath = root.path();
        context.persistent = false;
        RuntimeProfile::configure(context);

        ApplicationRuntime runtime(context);
        runtime.setQmlWindowVisible(true);
        runtime.setPointerInsideControlWindow(true);
        runtime.getWorkspaceManager()->stopAutomaticTimersForTesting();
        runtime.getProjectManager()->stopAutomaticTimersForTesting();

        RemoteSessionTestServer server(
            runtime.getWebSocketClient()->endpointId());
        QVERIFY(server.listen());
        ConnectionManager* connections =
            runtime.findChild<ConnectionManager*>();
        QVERIFY(connections);
        QSignalSpy connectedSpy(runtime.getWebSocketClient(),
                                &WebSocketClient::connected);
        QSignalSpy disconnectedSpy(runtime.getWebSocketClient(),
                                   &WebSocketClient::disconnected);
        connections->connectToServer(server.url());
        QTRY_COMPARE_WITH_TIMEOUT(connectedSpy.count(), 1, 2'000);

        const QString targetEndpointId = fixtureEndpoint(QLatin1Char('K'));
        ClientInfo client = onlineClient(
            targetEndpointId, QStringLiteral("Unacknowledged-open target"));
        client.setScreens({});
        client.setVolumePercent(-1);
        QVERIFY(server.sendClientList(client));
        QTRY_COMPARE_WITH_TIMEOUT(runtime.displayClients().size(), 1, 1'000);

        runtime.activateClient(targetEndpointId);
        QTRY_COMPARE_WITH_TIMEOUT(server.openCommands.size(), 1, 1'000);
        const QString cancelledRequestId = server.openCommands.constFirst()
            .value(QStringLiteral("requestId")).toString();
        QVERIFY(!cancelledRequestId.isEmpty());

        // Leave before even remote_session_opening arrives, so there is no
        // session identity which could be closed locally.
        runtime.navigateToClients();
        QVERIFY(!runtime.getNavigationManager()->isOnScreenView());

        // The replacement authenticated transport replays that exact request
        // ID. A new ID could either leak the former Opening or poison a later
        // explicit click; the idempotent replay gives an authoritative answer.
        server.connectionGeneration = 2;
        server.closePeer();
        QTRY_COMPARE_WITH_TIMEOUT(disconnectedSpy.count(), 1, 2'000);
        QTRY_COMPARE_WITH_TIMEOUT(connectedSpy.count(), 2, 3'000);
        QTRY_COMPARE_WITH_TIMEOUT(server.openCommands.size(), 2, 1'000);
        QCOMPARE(server.openCommands.constLast()
                     .value(QStringLiteral("requestId")).toString(),
                 cancelledRequestId);
        QCOMPARE(server.openCommands.constLast()
                     .value(QStringLiteral("targetEndpointId")).toString(),
                 targetEndpointId);

        QVERIFY(server.send(QJsonObject{
            {QStringLiteral("type"), QStringLiteral("error")},
            {QStringLiteral("scope"), QStringLiteral("remote_session")},
            {QStringLiteral("code"),
             QStringLiteral("session_requires_resume")},
            {QStringLiteral("message"),
             QStringLiteral("The lost Ready left the old session in Grace")},
            {QStringLiteral("requestId"), cancelledRequestId},
            {QStringLiteral("remoteSessionId"),
             QStringLiteral("lost-ready-session")},
            {QStringLiteral("targetEndpointId"), targetEndpointId}
        }));
        QTest::qWait(25);

        QVERIFY(server.sendClientList(client));
        QTRY_COMPARE_WITH_TIMEOUT(runtime.displayClients().size(), 1, 1'000);
        runtime.activateClient(targetEndpointId);
        QTRY_COMPARE_WITH_TIMEOUT(server.openCommands.size(), 3, 1'000);
        const QJsonObject freshOpen = server.openCommands.constLast();
        QVERIFY(freshOpen.value(QStringLiteral("requestId")).toString()
                != cancelledRequestId);

        // OPEN cannot replace the old Grace session without its resume proof.
        // Preserve this new explicit intent until the server's terminal
        // cleanup broadcast supplies a safe retry boundary.
        QVERIFY(server.send(QJsonObject{
            {QStringLiteral("type"), QStringLiteral("error")},
            {QStringLiteral("scope"), QStringLiteral("remote_session")},
            {QStringLiteral("code"),
             QStringLiteral("session_requires_resume")},
            {QStringLiteral("message"),
             QStringLiteral("The old session still requires resume proof")},
            {QStringLiteral("requestId"),
             freshOpen.value(QStringLiteral("requestId"))},
            {QStringLiteral("remoteSessionId"),
             QStringLiteral("lost-ready-session")},
            {QStringLiteral("targetEndpointId"), targetEndpointId}
        }));
        QTRY_COMPARE_WITH_TIMEOUT(runtime.remoteStatusText(),
                                  QStringLiteral("CONNECTING"), 1'000);
        QCOMPARE(server.openCommands.size(), 3);
        QVERIFY(runtime.getNavigationManager()->isOnScreenView());
        QVERIFY(runtime.getNavigationManager()->isLoading());

        QVERIFY(server.sendClientList(client));
        QTRY_COMPARE_WITH_TIMEOUT(server.openCommands.size(), 4, 3'000);
        const QJsonObject convergedOpen = server.openCommands.constLast();
        QVERIFY(server.sendOpened(
            QStringLiteral("cancelled-transport-session-2"),
            convergedOpen.value(QStringLiteral("requestId")).toString(),
            targetEndpointId, ScreenInfo(42, 1920, 1200, 0, 0, true), 57));
        QTRY_VERIFY_WITH_TIMEOUT(runtime.activeProjectExists(), 1'000);
        QTRY_VERIFY_WITH_TIMEOUT(runtime.getActiveCanvas(), 1'000);
        QCOMPARE(runtime.remoteStatusText(), QStringLiteral("CONNECTED"));
        QCOMPARE(runtime.remoteVolumePercent(), 57);
        QCOMPARE(server.openCommands.size(), 4);

        ApplicationRuntime::ClientWorkspace* workspace =
            runtime.findWorkspace(targetEndpointId);
        runtime.handleApplicationAboutToQuit();
        runtime.getNavigationManager()->setActiveCanvas(nullptr);
        runtime.setActiveCanvas(nullptr);
        QVERIFY(workspace && workspace->canvas);
        delete workspace->canvas;
        workspace->canvas = nullptr;
    }

    void resumedGraceAndStaleActiveTransportNeverGrantCommands()
    {
        QTemporaryDir root;
        QVERIFY(root.isValid());

        RuntimeProfileContext context;
        context.ordinal = 2;
        context.instanceId = QStringLiteral("resumed-grace-presentation");
        context.profileId = QStringLiteral("resumed-grace-presentation");
        context.rootPath = root.path();
        context.persistent = false;
        RuntimeProfile::configure(context);

        ApplicationRuntime runtime(context);
        runtime.getWorkspaceManager()->stopAutomaticTimersForTesting();
        runtime.getProjectManager()->stopAutomaticTimersForTesting();

        RemoteSessionTestServer server(
            runtime.getWebSocketClient()->endpointId());
        QVERIFY(server.listen());
        ConnectionManager* connections =
            runtime.findChild<ConnectionManager*>();
        QVERIFY(connections);
        QSignalSpy connectedSpy(runtime.getWebSocketClient(),
                                &WebSocketClient::connected);
        QSignalSpy disconnectedSpy(runtime.getWebSocketClient(),
                                   &WebSocketClient::disconnected);
        connections->connectToServer(server.url());
        QTRY_COMPARE_WITH_TIMEOUT(connectedSpy.count(), 1, 2'000);

        const QString targetEndpointId = fixtureEndpoint(QLatin1Char('R'));
        const QString remoteSessionId = QStringLiteral("resumed-grace-session");
        ClientInfo client = onlineClient(
            targetEndpointId, QStringLiteral("Grace target"));
        client.setScreens({});
        client.setVolumePercent(-1);
        QVERIFY(server.sendClientList(client));
        QTRY_COMPARE_WITH_TIMEOUT(runtime.displayClients().size(), 1, 1'000);

        runtime.activateClient(targetEndpointId);
        QTRY_COMPARE_WITH_TIMEOUT(server.openCommands.size(), 1, 1'000);
        QVERIFY(server.sendOpened(
            remoteSessionId,
            server.openCommands.constFirst()
                .value(QStringLiteral("requestId")).toString(),
            targetEndpointId, ScreenInfo(62, 2560, 1440, 0, 0, true), 68));
        QTRY_VERIFY_WITH_TIMEOUT(runtime.activeProjectExists(), 1'000);
        QVERIFY(runtime.getActiveCanvas());
        QVERIFY(runtime.isRemoteClientConnected());
        QVERIFY(runtime.isRemoteOverlayActionsEnabled());
        QCOMPARE(runtime.remoteStatusText(), QStringLiteral("CONNECTED"));
        QCOMPARE(runtime.remoteVolumePercent(), 68);

        // Keep the lease-bound session in memory while replacing the owner's
        // authenticated transport. Before RESUME is accepted, its old Active
        // tuple is recovery state only and cannot authorize commands.
        server.connectionGeneration = 2;
        server.closePeer();
        QTRY_COMPARE_WITH_TIMEOUT(disconnectedSpy.count(), 1, 2'000);
        QTRY_COMPARE_WITH_TIMEOUT(connectedSpy.count(), 2, 3'000);
        QTRY_COMPARE_WITH_TIMEOUT(server.resumeCommands.size(), 1, 1'000);
        QTRY_COMPARE_WITH_TIMEOUT(connections->state(), ConnectionManager::State::Connected, 1000);
        QCOMPARE(server.resumeCommands.constFirst()
                     .value(QStringLiteral("remoteSessionId")).toString(),
                 remoteSessionId);
        QVERIFY(server.sendClientList(client));
        QTRY_COMPARE_WITH_TIMEOUT(runtime.displayClients().size(), 1, 1'000);

        runtime.navigateToClients();
        runtime.activateClient(targetEndpointId);
        QVERIFY(runtime.getNavigationManager()->isOnScreenView());
        QVERIFY(runtime.activeProjectExists());
        QVERIFY(runtime.getActiveCanvas());
        QVERIFY(!runtime.isRemoteClientConnected());
        QVERIFY(!runtime.isRemoteOverlayActionsEnabled());
        QCOMPARE(runtime.remoteStatusText(), QStringLiteral("DEGRADED"));
        QCOMPARE(runtime.remoteVolumePercent(), -1);
        QCOMPARE(server.openCommands.size(), 1);
        QCOMPARE(runtime.getWorkspaceManager()->remoteSessionState(
                     targetEndpointId),
                 WorkspaceManager::RemoteSessionState::Grace);

        // The first party may resume while its peer is still in Grace. This
        // protocol-success response remains non-command-capable until a later
        // authoritative Active envelope arrives.
        QSignalSpy resumedSpy(runtime.getWebSocketClient(),
                              &WebSocketClient::remoteSessionResumed);
        QVERIFY(server.sendResumed(
            remoteSessionId, targetEndpointId, 2, 1,
            QStringLiteral("Grace")));
        QTRY_COMPARE_WITH_TIMEOUT(resumedSpy.count(), 1, 1'000);
        QCOMPARE(runtime.getWebSocketClient()->remoteSessionCoordinator()
                     ->outgoingForPeer(targetEndpointId).phase,
                 QStringLiteral("Grace"));
        QCOMPARE(runtime.getWorkspaceManager()->remoteSessionState(
                     targetEndpointId),
                 WorkspaceManager::RemoteSessionState::Grace);
        QVERIFY(!runtime.isRemoteClientConnected());
        QVERIFY(!runtime.isRemoteOverlayActionsEnabled());
        QCOMPARE(runtime.remoteStatusText(), QStringLiteral("DEGRADED"));
        QCOMPARE(runtime.remoteVolumePercent(), -1);
        QCOMPARE(server.openCommands.size(), 1);

        ApplicationRuntime::ClientWorkspace* workspace =
            runtime.findWorkspace(targetEndpointId);
        runtime.handleApplicationAboutToQuit();
        runtime.getNavigationManager()->setActiveCanvas(nullptr);
        runtime.setActiveCanvas(nullptr);
        QVERIFY(workspace && workspace->canvas);
        delete workspace->canvas;
        workspace->canvas = nullptr;
    }

    void failedProjectDeleteLeavesActiveSessionAndCanvasIntact()
    {
        QTemporaryDir root;
        QVERIFY(root.isValid());

        RuntimeProfileContext context;
        context.ordinal = 2;
        context.instanceId = QStringLiteral("failed-delete-is-atomic");
        context.profileId = QStringLiteral("failed-delete-is-atomic");
        context.rootPath = root.path();
        context.persistent = false;
        RuntimeProfile::configure(context);

        ApplicationRuntime runtime(context);
        runtime.getWorkspaceManager()->stopAutomaticTimersForTesting();
        runtime.getProjectManager()->stopAutomaticTimersForTesting();

        RemoteSessionTestServer server(
            runtime.getWebSocketClient()->endpointId());
        QVERIFY(server.listen());
        ConnectionManager* connections =
            runtime.findChild<ConnectionManager*>();
        QVERIFY(connections);
        QSignalSpy connectedSpy(runtime.getWebSocketClient(),
                                &WebSocketClient::connected);
        connections->connectToServer(server.url());
        QTRY_COMPARE_WITH_TIMEOUT(connectedSpy.count(), 1, 2'000);

        const QString targetEndpointId = fixtureEndpoint(QLatin1Char('D'));
        const QString remoteSessionId = QStringLiteral("failed-delete-session");
        ClientInfo client = onlineClient(
            targetEndpointId, QStringLiteral("Atomic delete target"));
        client.setScreens({});
        client.setVolumePercent(-1);
        QVERIFY(server.sendClientList(client));
        QTRY_COMPARE_WITH_TIMEOUT(runtime.displayClients().size(), 1, 1'000);
        runtime.activateClient(targetEndpointId);
        QTRY_COMPARE_WITH_TIMEOUT(server.openCommands.size(), 1, 1'000);
        QVERIFY(server.sendOpened(
            remoteSessionId,
            server.openCommands.constFirst()
                .value(QStringLiteral("requestId")).toString(),
            targetEndpointId, ScreenInfo(71, 1920, 1080, 0, 0, true), 39));
        QTRY_VERIFY_WITH_TIMEOUT(runtime.activeProjectExists(), 1'000);
        ICanvasHost* const originalCanvas = runtime.getActiveCanvas();
        QVERIFY(originalCanvas);
        QVERIFY(runtime.isRemoteClientConnected());

        const QString storeDirectory = root.filePath(QStringLiteral("projects"));
        const QString backupDirectory =
            root.filePath(QStringLiteral("projects-backup"));
        QVERIFY(QDir(storeDirectory).exists());
        QVERIFY(QDir().rename(storeDirectory, backupDirectory));
        QFile blocker(storeDirectory);
        QVERIFY(blocker.open(QIODevice::WriteOnly));
        QCOMPARE(blocker.write("blocked"), qint64(7));
        blocker.close();

        QSignalSpy persistenceErrors(
            runtime.getProjectManager(), &ProjectManager::persistenceError);
        runtime.deleteActiveProjectConfirmed();
        QTRY_COMPARE_WITH_TIMEOUT(persistenceErrors.count(), 1, 1'000);

        // Failure to atomically persist deletion is a complete rollback: the
        // project graph and its command-capable RemoteSession remain intact.
        QVERIFY(runtime.getProjectManager()->hasProjectForTarget(
            targetEndpointId));
        QCOMPARE(runtime.getActiveCanvas(), originalCanvas);
        QCOMPARE(runtime.findWorkspace(targetEndpointId)->canvas,
                 originalCanvas);
        QCOMPARE(runtime.getWorkspaceManager()->remoteSessionState(
                     targetEndpointId),
                 WorkspaceManager::RemoteSessionState::Active);
        QVERIFY(runtime.isRemoteClientConnected());
        QVERIFY(runtime.isRemoteOverlayActionsEnabled());
        QCOMPARE(runtime.remoteStatusText(), QStringLiteral("CONNECTED"));
        QCOMPARE(server.closeCommands.size(), 0);

        QVERIFY(QFile::remove(storeDirectory));
        QVERIFY(QDir().rename(backupDirectory, storeDirectory));

        ApplicationRuntime::ClientWorkspace* workspace =
            runtime.findWorkspace(targetEndpointId);
        runtime.handleApplicationAboutToQuit();
        runtime.getNavigationManager()->setActiveCanvas(nullptr);
        runtime.setActiveCanvas(nullptr);
        QVERIFY(workspace && workspace->canvas);
        delete workspace->canvas;
        workspace->canvas = nullptr;
    }

    void explicitReplacementIntentSurvivesServerRestart()
    {
        QTemporaryDir root;
        QVERIFY(root.isValid());

        RuntimeProfileContext context;
        context.ordinal = 2;
        context.instanceId = QStringLiteral("restart-replacement-intent");
        context.profileId = QStringLiteral("restart-replacement-intent");
        context.rootPath = root.path();
        context.persistent = false;
        RuntimeProfile::configure(context);

        ApplicationRuntime runtime(context);
        runtime.getWorkspaceManager()->stopAutomaticTimersForTesting();
        runtime.getProjectManager()->stopAutomaticTimersForTesting();
        runtime.getWorkspaceManager()->setRemoteSessionHiddenTimeoutMs(10);

        RemoteSessionTestServer server(
            runtime.getWebSocketClient()->endpointId());
        QVERIFY(server.listen());
        ConnectionManager* connections =
            runtime.findChild<ConnectionManager*>();
        QVERIFY(connections);
        QSignalSpy connectedSpy(runtime.getWebSocketClient(),
                                &WebSocketClient::connected);
        connections->connectToServer(server.url());
        QTRY_COMPARE_WITH_TIMEOUT(connectedSpy.count(), 1, 2'000);

        const QString targetEndpointId = fixtureEndpoint(QLatin1Char('E'));
        ClientInfo client = onlineClient(
            targetEndpointId, QStringLiteral("Restart target"));
        client.setScreens({});
        client.setVolumePercent(-1);
        QVERIFY(server.sendClientList(client));
        QTRY_COMPARE_WITH_TIMEOUT(runtime.displayClients().size(), 1, 1'000);

        runtime.activateClient(targetEndpointId);
        QTRY_COMPARE_WITH_TIMEOUT(server.openCommands.size(), 1, 1'000);
        QVERIFY(server.sendOpened(
            QStringLiteral("restart-session-1"),
            server.openCommands.constFirst()
                .value(QStringLiteral("requestId")).toString(),
            targetEndpointId, ScreenInfo(32, 1920, 1080, 0, 0, true), 44));
        QTRY_VERIFY_WITH_TIMEOUT(runtime.activeProjectExists(), 1'000);

        const qint64 hiddenAt = QDateTime::currentMSecsSinceEpoch();
        runtime.getWorkspaceManager()->markAllWorkspacesHidden(hiddenAt);
        const qint64 closeAt =
            runtime.getWorkspaceManager()->remoteSessionCloseAtMs(
                targetEndpointId);
        QVERIFY(closeAt >= hiddenAt);
        runtime.getWorkspaceManager()->processDeadlines(closeAt);
        QTRY_COMPARE_WITH_TIMEOUT(server.closeCommands.size(), 1, 1'000);

        // A selection while Session1 is Closing is an explicit request for a
        // replacement, but OPEN2 must wait for the old boot's terminal result.
        runtime.activateClient(targetEndpointId);
        QCOMPARE(server.openCommands.size(), 1);
        QCOMPARE(runtime.remoteStatusText(), QStringLiteral("DISCONNECTING"));

        QSignalSpy disconnectedSpy(runtime.getWebSocketClient(),
                                   &WebSocketClient::disconnected);
        QSignalSpy restartedSpy(runtime.getWebSocketClient(),
                                &WebSocketClient::serverRestarted);
        // Rotate first: ConnectionManager's within-lease retry may run with a
        // zero-delay timer as soon as the old socket reports disconnected.
        server.beginNewBoot();
        server.closePeer();
        QTRY_COMPARE_WITH_TIMEOUT(disconnectedSpy.count(), 1, 2'000);
        QTRY_COMPARE_WITH_TIMEOUT(connectedSpy.count(), 2, 2'000);
        QTRY_COMPARE_WITH_TIMEOUT(restartedSpy.count(), 1, 1'000);

        // The restart proves Session1 can no longer own the pair. Discovery of
        // B must consume the still-current user intent exactly once.
        QVERIFY(server.sendClientList(client));
        QTRY_COMPARE_WITH_TIMEOUT(server.openCommands.size(), 2, 1'000);
        QTest::qWait(50);
        QCOMPARE(server.openCommands.size(), 2);
        QCOMPARE(server.openCommands.constLast()
                     .value(QStringLiteral("targetEndpointId")).toString(),
                 targetEndpointId);
        QCOMPARE(runtime.remoteStatusText(), QStringLiteral("CONNECTING"));

        ApplicationRuntime::ClientWorkspace* workspace =
            runtime.findWorkspace(targetEndpointId);
        runtime.handleApplicationAboutToQuit();
        runtime.getNavigationManager()->setActiveCanvas(nullptr);
        runtime.setActiveCanvas(nullptr);
        QVERIFY(workspace && workspace->canvas);
        delete workspace->canvas;
        workspace->canvas = nullptr;
    }

    void activeCanvasAutomaticallyReopensRecoveredPeer_data()
    {
        QTest::addColumn<bool>("discoveryBeforeClosed");
        QTest::addColumn<bool>("hasTerminatingEnvelope");
        QTest::newRow("discovery-before-closed") << true << false;
        QTest::newRow("closed-before-discovery") << false << false;
        QTest::newRow("discovery-before-fenced-closed") << true << true;
        QTest::newRow("fenced-closed-before-discovery") << false << true;
    }

    void activeCanvasAutomaticallyReopensRecoveredPeer()
    {
        QFETCH(bool, discoveryBeforeClosed);
        QFETCH(bool, hasTerminatingEnvelope);
        QTemporaryDir root;
        QVERIFY(root.isValid());
        RuntimeProfileContext context;
        context.ordinal = 2;
        context.instanceId = QStringLiteral("active-peer-recovery");
        context.profileId = context.instanceId;
        context.rootPath = root.path();
        context.persistent = false;
        RuntimeProfile::configure(context);

        ApplicationRuntime runtime(context);
        runtime.getWorkspaceManager()->stopAutomaticTimersForTesting();
        runtime.getProjectManager()->stopAutomaticTimersForTesting();
        runtime.setQmlWindowVisible(true);
        runtime.setPointerInsideControlWindow(true);
        RemoteSessionTestServer server(runtime.getWebSocketClient()->endpointId());
        QVERIFY(server.listen());
        ConnectionManager* connections = runtime.findChild<ConnectionManager*>();
        QVERIFY(connections);
        QSignalSpy connectedSpy(runtime.getWebSocketClient(),
                                &WebSocketClient::connected);
        QSignalSpy listSpy(runtime.getWebSocketClient(),
                           &WebSocketClient::clientListReceived);
        QSignalSpy closedSpy(runtime.getWebSocketClient(),
                             &WebSocketClient::remoteSessionClosed);
        connections->connectToServer(server.url());
        QTRY_COMPARE_WITH_TIMEOUT(connectedSpy.count(), 1, 2'000);

        const QString targetEndpointId = fixtureEndpoint(QLatin1Char('R'));
        const QString firstSessionId = QStringLiteral("recovered-peer-session-1");
        const QString secondSessionId = QStringLiteral("recovered-peer-session-2");
        ClientInfo client = onlineClient(targetEndpointId,
                                        QStringLiteral("Recovered foreground peer"));
        client.setScreens({});
        client.setVolumePercent(-1);
        QVERIFY(server.sendClientList(client));
        QTRY_COMPARE_WITH_TIMEOUT(listSpy.count(), 1, 1'000);
        runtime.activateClient(targetEndpointId);
        QTRY_COMPARE_WITH_TIMEOUT(server.openCommands.size(), 1, 1'000);
        QVERIFY(server.sendOpened(
            firstSessionId,
            server.openCommands.constFirst().value(QStringLiteral("requestId")).toString(),
            targetEndpointId, ScreenInfo(91, 1920, 1080, 0, 0, true), 42));
        QTRY_VERIFY_WITH_TIMEOUT(runtime.isRemoteClientConnected(), 1'000);
        ICanvasHost* const originalCanvas = runtime.getActiveCanvas();
        QVERIFY(originalCanvas);

        // Discovery can disappear before the session's terminal result. The
        // retained project and page survive both possible recovery orderings.
        QVERIFY(server.send(QJsonObject{
            {QStringLiteral("type"), QStringLiteral("client_list")},
            {QStringLiteral("clients"), QJsonArray{}}
        }));
        QTRY_COMPARE_WITH_TIMEOUT(listSpy.count(), 2, 1'000);
        if (hasTerminatingEnvelope) {
            QVERIFY(server.sendTerminating(firstSessionId, targetEndpointId));
            QTRY_COMPARE_WITH_TIMEOUT(
                runtime.getWebSocketClient()->remoteSessionCoordinator()
                    ->outgoingForPeer(targetEndpointId).phase,
                QStringLiteral("CleanupPending"), 1'000);
        }
        if (discoveryBeforeClosed) {
            QVERIFY(server.sendClientList(client));
            QTRY_COMPARE_WITH_TIMEOUT(listSpy.count(), 3, 1'000);
            QTest::qWait(25);
            QCOMPARE(server.openCommands.size(), 1);
        }

        QVERIFY(server.sendClosed(firstSessionId, targetEndpointId));
        QTRY_COMPARE_WITH_TIMEOUT(closedSpy.count(), 1, 1'000);
        if (!discoveryBeforeClosed) {
            QTest::qWait(25);
            QCOMPARE(server.openCommands.size(), 1);
            QVERIFY(server.sendClientList(client));
            QTRY_COMPARE_WITH_TIMEOUT(listSpy.count(), 3, 1'000);
        }
        QTRY_COMPARE_WITH_TIMEOUT(server.openCommands.size(), 2, 1'000);
        const QString replacementRequestId = server.openCommands.constLast()
            .value(QStringLiteral("requestId")).toString();
        QVERIFY(!replacementRequestId.isEmpty());
        QVERIFY(replacementRequestId != server.openCommands.constFirst()
                    .value(QStringLiteral("requestId")).toString());
        QCOMPARE(server.openCommands.constLast()
                     .value(QStringLiteral("targetEndpointId")).toString(),
                 targetEndpointId);
        QCOMPARE(runtime.getActiveCanvas(), originalCanvas);
        QVERIFY(runtime.activeProjectExists());
        QVERIFY(!runtime.isRemoteClientConnected());
        QCOMPARE(runtime.remoteStatusText(), QStringLiteral("CONNECTING"));

        // Repeated discovery and a delayed old terminal message must neither
        // duplicate nor erase the in-flight replacement before Ready arrives.
        QVERIFY(server.sendClientList(client));
        QVERIFY(server.sendClosed(firstSessionId, targetEndpointId));
        QVERIFY(server.sendClientList(client));
        QTRY_COMPARE_WITH_TIMEOUT(listSpy.count(), 5, 1'000);
        QTest::qWait(25);
        QCOMPARE(server.openCommands.size(), 2);
        QVERIFY(server.sendOpened(
            secondSessionId, replacementRequestId, targetEndpointId,
            ScreenInfo(92, 2560, 1440, 0, 0, true), 77));
        QTRY_VERIFY_WITH_TIMEOUT(runtime.isRemoteClientConnected(), 1'000);
        QCOMPARE(runtime.getActiveCanvas(), originalCanvas);
        QCOMPARE(runtime.remoteVolumePercent(), 77);
        QVERIFY(runtime.isRemoteOverlayActionsEnabled());

        QVERIFY(server.sendClosed(firstSessionId, targetEndpointId));
        QVERIFY(server.sendClientList(client));
        QTRY_COMPARE_WITH_TIMEOUT(listSpy.count(), 6, 1'000);
        QTest::qWait(25);
        QCOMPARE(server.openCommands.size(), 2);
        QCOMPARE(runtime.remoteStatusText(), QStringLiteral("CONNECTED"));
        QCOMPARE(runtime.getWebSocketClient()->remoteSessionCoordinator()
                     ->outgoingForPeer(targetEndpointId).remoteSessionId,
                 secondSessionId);
        QCOMPARE(runtime.remoteVolumePercent(), 77);

        ApplicationRuntime::ClientWorkspace* workspace =
            runtime.findWorkspace(targetEndpointId);
        runtime.handleApplicationAboutToQuit();
        runtime.getNavigationManager()->setActiveCanvas(nullptr);
        runtime.setActiveCanvas(nullptr);
        QVERIFY(workspace && workspace->canvas);
        delete workspace->canvas;
        workspace->canvas = nullptr;
    }

    void foregroundRecoveryRequiresCurrentActivityAndCanvas_data()
    {
        QTest::addColumn<int>("blocker");
        QTest::addColumn<bool>("leaveAfterDiscovery");
        QTest::newRow("pointer-outside") << 0 << false;
        QTest::newRow("window-hidden") << 1 << false;
        QTest::newRow("clients-page") << 2 << false;
        QTest::newRow("pointer-leaves-during-close") << 0 << true;
        QTest::newRow("window-hides-during-close") << 1 << true;
        QTest::newRow("clients-page-during-close") << 2 << true;
    }

    void foregroundRecoveryRequiresCurrentActivityAndCanvas()
    {
        QFETCH(int, blocker);
        QFETCH(bool, leaveAfterDiscovery);
        QTemporaryDir root;
        QVERIFY(root.isValid());
        RuntimeProfileContext context;
        context.ordinal = 2;
        context.instanceId = QStringLiteral("gated-peer-recovery");
        context.profileId = context.instanceId;
        context.rootPath = root.path();
        context.persistent = false;
        RuntimeProfile::configure(context);

        ApplicationRuntime runtime(context);
        runtime.getWorkspaceManager()->stopAutomaticTimersForTesting();
        runtime.getProjectManager()->stopAutomaticTimersForTesting();
        runtime.setQmlWindowVisible(true);
        runtime.setPointerInsideControlWindow(true);
        RemoteSessionTestServer server(runtime.getWebSocketClient()->endpointId());
        QVERIFY(server.listen());
        ConnectionManager* connections = runtime.findChild<ConnectionManager*>();
        QVERIFY(connections);
        QSignalSpy connectedSpy(runtime.getWebSocketClient(),
                                &WebSocketClient::connected);
        QSignalSpy listSpy(runtime.getWebSocketClient(),
                           &WebSocketClient::clientListReceived);
        QSignalSpy closedSpy(runtime.getWebSocketClient(),
                             &WebSocketClient::remoteSessionClosed);
        connections->connectToServer(server.url());
        QTRY_COMPARE_WITH_TIMEOUT(connectedSpy.count(), 1, 2'000);

        const QString targetEndpointId = fixtureEndpoint(QLatin1Char('S'));
        const QString firstSessionId = QStringLiteral("gated-peer-session-1");
        ClientInfo client = onlineClient(targetEndpointId,
                                        QStringLiteral("Gated foreground peer"));
        client.setScreens({});
        client.setVolumePercent(-1);
        QVERIFY(server.sendClientList(client));
        QTRY_COMPARE_WITH_TIMEOUT(listSpy.count(), 1, 1'000);
        QTRY_COMPARE_WITH_TIMEOUT(connections->state(), ConnectionManager::State::Connected, 1000);
        QTRY_COMPARE_WITH_TIMEOUT(runtime.displayClients().size(), 1, 1000);
        runtime.activateClient(targetEndpointId);
        QTRY_COMPARE_WITH_TIMEOUT(server.openCommands.size(), 1, 1'000);
        QVERIFY(server.sendOpened(
            firstSessionId,
            server.openCommands.constFirst().value(QStringLiteral("requestId")).toString(),
            targetEndpointId, ScreenInfo(93, 1920, 1080, 0, 0, true), 42));
        QTRY_VERIFY_WITH_TIMEOUT(runtime.isRemoteClientConnected(), 1'000);
        ICanvasHost* const originalCanvas = runtime.getActiveCanvas();
        QVERIFY(originalCanvas);
        QVERIFY(server.sendTerminating(firstSessionId, targetEndpointId));
        QTRY_COMPARE_WITH_TIMEOUT(
            runtime.getWebSocketClient()->remoteSessionCoordinator()
                ->outgoingForPeer(targetEndpointId).phase,
            QStringLiteral("CleanupPending"), 1'000);

        const auto leave = [&]() {
            if (blocker == 0) runtime.setPointerInsideControlWindow(false);
            else if (blocker == 1) runtime.setQmlWindowVisible(false);
            else runtime.navigateToClients();
        };
        if (!leaveAfterDiscovery) leave();
        QVERIFY(server.sendClientList(client));
        QTRY_COMPARE_WITH_TIMEOUT(listSpy.count(), 2, 1'000);
        if (leaveAfterDiscovery) leave();
        QVERIFY(server.sendClosed(firstSessionId, targetEndpointId));
        QTRY_COMPARE_WITH_TIMEOUT(closedSpy.count(), 1, 1'000);
        QVERIFY(server.sendClientList(client));
        QTRY_COMPARE_WITH_TIMEOUT(listSpy.count(), 3, 1'000);
        QTest::qWait(25);
        QCOMPARE(server.openCommands.size(), 1);
        QVERIFY(runtime.getProjectManager()->hasProjectForTarget(targetEndpointId));
        QCOMPARE(runtime.findWorkspace(targetEndpointId)->canvas, originalCanvas);

        // No inactivity deadline expired: returning activity must reconcile a
        // peer-driven loss as well, while an unrelated page stays quiescent.
        if (blocker == 0) runtime.setPointerInsideControlWindow(true);
        else if (blocker == 1) {
            runtime.setQmlWindowVisible(true);
            runtime.setPointerInsideControlWindow(true);
        } else {
            runtime.setPointerInsideControlWindow(false);
            runtime.setPointerInsideControlWindow(true);
        }
        if (blocker == 2) {
            QTest::qWait(25);
            QCOMPARE(server.openCommands.size(), 1);
            QVERIFY(!runtime.getNavigationManager()->isOnScreenView());
        } else {
            QTRY_COMPARE_WITH_TIMEOUT(server.openCommands.size(), 2, 1'000);
            QVERIFY(server.sendOpened(
                QStringLiteral("gated-peer-session-2"),
                server.openCommands.constLast().value(QStringLiteral("requestId")).toString(),
                targetEndpointId, ScreenInfo(94, 2560, 1440, 0, 0, true), 75));
            QTRY_VERIFY_WITH_TIMEOUT(runtime.isRemoteClientConnected(), 1'000);
            QCOMPARE(runtime.getActiveCanvas(), originalCanvas);
            QCOMPARE(runtime.remoteVolumePercent(), 75);
        }

        ApplicationRuntime::ClientWorkspace* workspace =
            runtime.findWorkspace(targetEndpointId);
        runtime.handleApplicationAboutToQuit();
        runtime.getNavigationManager()->setActiveCanvas(nullptr);
        runtime.setActiveCanvas(nullptr);
        QVERIFY(workspace && workspace->canvas);
        delete workspace->canvas;
        workspace->canvas = nullptr;
    }

    void automaticReplacementOpenReplaysItsIdentityAfterTransportRecovery()
    {
        QTemporaryDir root;
        QVERIFY(root.isValid());
        RuntimeProfileContext context;
        context.ordinal = 2;
        context.instanceId = QStringLiteral("automatic-open-transport-replay");
        context.profileId = context.instanceId;
        context.rootPath = root.path();
        context.persistent = false;
        RuntimeProfile::configure(context);

        ApplicationRuntime runtime(context);
        runtime.getWorkspaceManager()->stopAutomaticTimersForTesting();
        runtime.getProjectManager()->stopAutomaticTimersForTesting();
        runtime.setQmlWindowVisible(true);
        runtime.setPointerInsideControlWindow(true);
        RemoteSessionTestServer server(runtime.getWebSocketClient()->endpointId());
        QVERIFY(server.listen());
        ConnectionManager* connections = runtime.findChild<ConnectionManager*>();
        QVERIFY(connections);
        QSignalSpy connectedSpy(runtime.getWebSocketClient(),
                                &WebSocketClient::connected);
        QSignalSpy disconnectedSpy(runtime.getWebSocketClient(),
                                   &WebSocketClient::disconnected);
        QSignalSpy listSpy(runtime.getWebSocketClient(),
                           &WebSocketClient::clientListReceived);
        connections->connectToServer(server.url());
        QTRY_COMPARE_WITH_TIMEOUT(connectedSpy.count(), 1, 2'000);

        const QString targetEndpointId = fixtureEndpoint(QLatin1Char('T'));
        const QString firstSessionId = QStringLiteral("automatic-replay-session-1");
        ClientInfo client = onlineClient(targetEndpointId,
                                        QStringLiteral("Automatic replay peer"));
        client.setScreens({});
        client.setVolumePercent(-1);
        QVERIFY(server.sendClientList(client));
        QTRY_COMPARE_WITH_TIMEOUT(listSpy.count(), 1, 1'000);
        QTRY_COMPARE_WITH_TIMEOUT(connections->state(), ConnectionManager::State::Connected, 1000);
        QTRY_COMPARE_WITH_TIMEOUT(runtime.displayClients().size(), 1, 1000);
        runtime.activateClient(targetEndpointId);
        QTRY_COMPARE_WITH_TIMEOUT(server.openCommands.size(), 1, 1'000);
        QVERIFY(server.sendOpened(
            firstSessionId,
            server.openCommands.constFirst().value(QStringLiteral("requestId")).toString(),
            targetEndpointId, ScreenInfo(95, 1920, 1080, 0, 0, true), 42));
        QTRY_VERIFY_WITH_TIMEOUT(runtime.isRemoteClientConnected(), 1'000);
        ICanvasHost* const originalCanvas = runtime.getActiveCanvas();
        QVERIFY(originalCanvas);

        QVERIFY(server.sendClosed(firstSessionId, targetEndpointId));
        QTRY_COMPARE_WITH_TIMEOUT(server.openCommands.size(), 2, 1'000);
        const QString replacementRequestId = server.openCommands.constLast()
            .value(QStringLiteral("requestId")).toString();
        QVERIFY(!replacementRequestId.isEmpty());

        // The server received OPEN but neither Opening nor Ready reached A.
        // Recovering the transport must resolve that same idempotent request,
        // even though no resumable session identity has been learned yet.
        server.connectionGeneration = 2;
        server.closePeer();
        QTRY_COMPARE_WITH_TIMEOUT(disconnectedSpy.count(), 1, 2'000);
        QTRY_COMPARE_WITH_TIMEOUT(connectedSpy.count(), 2, 3'000);
        QTRY_COMPARE_WITH_TIMEOUT(server.openCommands.size(), 3, 1'000);
        QCOMPARE(server.openCommands.constLast()
                     .value(QStringLiteral("requestId")).toString(),
                 replacementRequestId);
        QCOMPARE(server.openCommands.constLast()
                     .value(QStringLiteral("targetEndpointId")).toString(),
                 targetEndpointId);
        QCOMPARE(server.openCommands.constLast()
                     .value(QStringLiteral("connectionGeneration")).toInt(), 2);
        QVERIFY(server.sendClientList(client));
        QVERIFY(server.sendClientList(client));
        QTRY_COMPARE_WITH_TIMEOUT(listSpy.count(), 3, 1'000);
        QTest::qWait(25);
        QCOMPARE(server.openCommands.size(), 3);

        QVERIFY(server.sendOpened(
            QStringLiteral("automatic-replay-session-2"), replacementRequestId,
            targetEndpointId, ScreenInfo(96, 2560, 1440, 0, 0, true), 76));
        QTRY_VERIFY_WITH_TIMEOUT(runtime.isRemoteClientConnected(), 1'000);
        QCOMPARE(runtime.getActiveCanvas(), originalCanvas);
        QCOMPARE(runtime.remoteVolumePercent(), 76);
        QCOMPARE(runtime.remoteStatusText(), QStringLiteral("CONNECTED"));

        ApplicationRuntime::ClientWorkspace* workspace =
            runtime.findWorkspace(targetEndpointId);
        runtime.handleApplicationAboutToQuit();
        runtime.getNavigationManager()->setActiveCanvas(nullptr);
        runtime.setActiveCanvas(nullptr);
        QVERIFY(workspace && workspace->canvas);
        delete workspace->canvas;
        workspace->canvas = nullptr;
    }

    void automaticConvergenceRetryStopsWhenPointerLeaves_data()
    {
        QTest::addColumn<QString>("errorCode");
        QTest::newRow("cleanup-pending") << QStringLiteral("session_cleanup_pending");
        QTest::newRow("requires-resume") << QStringLiteral("session_requires_resume");
    }

    void automaticConvergenceRetryStopsWhenPointerLeaves()
    {
        QFETCH(QString, errorCode);
        QTemporaryDir root;
        QVERIFY(root.isValid());
        RuntimeProfileContext context;
        context.ordinal = 2;
        context.instanceId = QStringLiteral("automatic-convergence-activity");
        context.profileId = context.instanceId;
        context.rootPath = root.path();
        context.persistent = false;
        RuntimeProfile::configure(context);

        ApplicationRuntime runtime(context);
        runtime.getWorkspaceManager()->stopAutomaticTimersForTesting();
        runtime.getProjectManager()->stopAutomaticTimersForTesting();
        runtime.setQmlWindowVisible(true);
        runtime.setPointerInsideControlWindow(true);
        RemoteSessionTestServer server(runtime.getWebSocketClient()->endpointId());
        QVERIFY(server.listen());
        ConnectionManager* connections = runtime.findChild<ConnectionManager*>();
        QVERIFY(connections);
        QSignalSpy connectedSpy(runtime.getWebSocketClient(),
                                &WebSocketClient::connected);
        QSignalSpy listSpy(runtime.getWebSocketClient(),
                           &WebSocketClient::clientListReceived);
        QSignalSpy errorSpy(runtime.getWebSocketClient(),
                            &WebSocketClient::remoteSessionError);
        connections->connectToServer(server.url());
        QTRY_COMPARE_WITH_TIMEOUT(connectedSpy.count(), 1, 2'000);

        const QString targetEndpointId = fixtureEndpoint(QLatin1Char('U'));
        const QString firstSessionId = QStringLiteral("automatic-convergence-session-1");
        ClientInfo client = onlineClient(targetEndpointId,
                                        QStringLiteral("Automatic convergence peer"));
        client.setScreens({});
        client.setVolumePercent(-1);
        QVERIFY(server.sendClientList(client));
        QTRY_COMPARE_WITH_TIMEOUT(listSpy.count(), 1, 1'000);
        runtime.activateClient(targetEndpointId);
        QTRY_COMPARE_WITH_TIMEOUT(server.openCommands.size(), 1, 1'000);
        QVERIFY(server.sendOpened(
            firstSessionId,
            server.openCommands.constFirst().value(QStringLiteral("requestId")).toString(),
            targetEndpointId, ScreenInfo(97, 1920, 1080, 0, 0, true), 42));
        QTRY_VERIFY_WITH_TIMEOUT(runtime.isRemoteClientConnected(), 1'000);
        ICanvasHost* const originalCanvas = runtime.getActiveCanvas();
        QVERIFY(originalCanvas);

        QVERIFY(server.sendClosed(firstSessionId, targetEndpointId));
        QTRY_COMPARE_WITH_TIMEOUT(server.openCommands.size(), 2, 3'000);
        const QString automaticRequestId = server.openCommands.constLast()
            .value(QStringLiteral("requestId")).toString();
        QVERIFY(server.send(QJsonObject{
            {QStringLiteral("type"), QStringLiteral("error")},
            {QStringLiteral("scope"), QStringLiteral("remote_session")},
            {QStringLiteral("code"), errorCode},
            {QStringLiteral("message"), QStringLiteral("The peer is still converging")},
            {QStringLiteral("requestId"), automaticRequestId}
        }));
        QTRY_COMPARE_WITH_TIMEOUT(errorSpy.count(), 1, 1'000);
        QTest::qWait(25);
        QCOMPARE(server.openCommands.size(), 2);

        // An automatic request cannot become a sticky explicit selection just
        // because it encountered a transient server convergence response.
        runtime.setPointerInsideControlWindow(false);
        QVERIFY(server.sendClientList(client));
        QVERIFY(server.sendClientList(client));
        QTRY_COMPARE_WITH_TIMEOUT(listSpy.count(), 3, 1'000);
        QTest::qWait(25);
        QCOMPARE(server.openCommands.size(), 2);
        QVERIFY(runtime.getNavigationManager()->isOnScreenView());
        QCOMPARE(runtime.getActiveCanvas(), originalCanvas);

        runtime.setPointerInsideControlWindow(true);
        QTRY_COMPARE_WITH_TIMEOUT(server.openCommands.size(), 3, 3'000);
        const QString retryRequestId = server.openCommands.constLast()
            .value(QStringLiteral("requestId")).toString();
        QVERIFY(retryRequestId != automaticRequestId);
        QVERIFY(server.sendOpened(
            QStringLiteral("automatic-convergence-session-2"), retryRequestId,
            targetEndpointId, ScreenInfo(98, 2560, 1440, 0, 0, true), 78));
        QTRY_VERIFY_WITH_TIMEOUT(runtime.isRemoteClientConnected(), 1'000);
        QCOMPARE(runtime.getActiveCanvas(), originalCanvas);
        QCOMPARE(runtime.remoteVolumePercent(), 78);

        ApplicationRuntime::ClientWorkspace* workspace =
            runtime.findWorkspace(targetEndpointId);
        runtime.handleApplicationAboutToQuit();
        runtime.getNavigationManager()->setActiveCanvas(nullptr);
        runtime.setActiveCanvas(nullptr);
        QVERIFY(workspace && workspace->canvas);
        delete workspace->canvas;
        workspace->canvas = nullptr;
    }

    void invalidAutomaticReplacementRequiresExplicitRetry_data()
    {
        QTest::addColumn<bool>("invalidSnapshot");
        QTest::newRow("malformed-snapshot") << true;
        QTest::newRow("permanent-open-rejection") << false;
    }

    void invalidAutomaticReplacementRequiresExplicitRetry()
    {
        QFETCH(bool, invalidSnapshot);
        QTemporaryDir root;
        QVERIFY(root.isValid());
        RuntimeProfileContext context;
        context.ordinal = 2;
        context.instanceId = QStringLiteral("invalid-automatic-replacement");
        context.profileId = context.instanceId;
        context.rootPath = root.path();
        context.persistent = false;
        RuntimeProfile::configure(context);

        ApplicationRuntime runtime(context);
        runtime.getWorkspaceManager()->stopAutomaticTimersForTesting();
        runtime.getProjectManager()->stopAutomaticTimersForTesting();
        runtime.setQmlWindowVisible(true);
        runtime.setPointerInsideControlWindow(true);
        RemoteSessionTestServer server(runtime.getWebSocketClient()->endpointId());
        QVERIFY(server.listen());
        ConnectionManager* connections = runtime.findChild<ConnectionManager*>();
        QVERIFY(connections);
        QSignalSpy connectedSpy(runtime.getWebSocketClient(),
                                &WebSocketClient::connected);
        QSignalSpy listSpy(runtime.getWebSocketClient(),
                           &WebSocketClient::clientListReceived);
        QSignalSpy errorSpy(runtime.getWebSocketClient(),
                            &WebSocketClient::remoteSessionError);
        QSignalSpy closedSpy(runtime.getWebSocketClient(),
                             &WebSocketClient::remoteSessionClosed);
        connections->connectToServer(server.url());
        QTRY_COMPARE_WITH_TIMEOUT(connectedSpy.count(), 1, 2'000);

        const QString targetEndpointId = fixtureEndpoint(QLatin1Char('V'));
        const QString firstSessionId = QStringLiteral("invalid-auto-session-1");
        const QString rejectedSessionId = QStringLiteral("invalid-auto-session-2");
        ClientInfo client = onlineClient(targetEndpointId,
                                        QStringLiteral("Invalid automatic replacement peer"));
        client.setScreens({});
        client.setVolumePercent(-1);
        QVERIFY(server.sendClientList(client));
        QTRY_COMPARE_WITH_TIMEOUT(listSpy.count(), 1, 1'000);
        runtime.activateClient(targetEndpointId);
        QTRY_COMPARE_WITH_TIMEOUT(server.openCommands.size(), 1, 1'000);
        QVERIFY(server.sendOpened(
            firstSessionId,
            server.openCommands.constFirst().value(QStringLiteral("requestId")).toString(),
            targetEndpointId, ScreenInfo(99, 1920, 1080, 0, 0, true), 42));
        QTRY_VERIFY_WITH_TIMEOUT(runtime.isRemoteClientConnected(), 1'000);
        ICanvasHost* const originalCanvas = runtime.getActiveCanvas();
        QVERIFY(originalCanvas);
        QVERIFY(server.sendClosed(firstSessionId, targetEndpointId));
        QTRY_COMPARE_WITH_TIMEOUT(server.openCommands.size(), 2, 1'000);
        const QString rejectedRequestId = server.openCommands.constLast()
            .value(QStringLiteral("requestId")).toString();

        if (invalidSnapshot) {
            // A valid Active envelope carrying an invalid snapshot must close
            // its exact identity without repeatedly reopening on every list.
            QVERIFY(server.send(QJsonObject{
                {QStringLiteral("type"), QStringLiteral("remote_session_opened")},
                {QStringLiteral("requestId"), rejectedRequestId},
                {QStringLiteral("remoteSessionId"), rejectedSessionId},
                {QStringLiteral("generation"), 1},
                {QStringLiteral("ownerConnectionGeneration"), 1},
                {QStringLiteral("targetConnectionGeneration"), 1},
                {QStringLiteral("phase"), QStringLiteral("Active")},
                {QStringLiteral("ownerEndpointId"),
                 runtime.getWebSocketClient()->endpointId()},
                {QStringLiteral("targetEndpointId"), targetEndpointId},
                {QStringLiteral("resumeToken"), QStringLiteral("invalid-auto-resume")},
                {QStringLiteral("snapshotSequence"), 1},
                {QStringLiteral("snapshot"), QJsonObject{
                    {QStringLiteral("screens"), QJsonArray{
                         ScreenInfo(100, 2560, 1440, 0, 0, true).toJson()}},
                    {QStringLiteral("systemUI"), QJsonArray{}},
                    {QStringLiteral("volumePercent"), 67},
                    {QStringLiteral("revision"), 1}
                    // capturedAtEpochMs is intentionally missing.
                }}
            }));
        } else {
            QVERIFY(server.send(QJsonObject{
                {QStringLiteral("type"), QStringLiteral("error")},
                {QStringLiteral("scope"), QStringLiteral("remote_session")},
                {QStringLiteral("code"), QStringLiteral("request_id_conflict")},
                {QStringLiteral("message"), QStringLiteral("OPEN request identity conflict")},
                {QStringLiteral("requestId"), rejectedRequestId}
            }));
        }
        QTRY_COMPARE_WITH_TIMEOUT(errorSpy.count(), 1, 1'000);
        QVERIFY(!runtime.isRemoteClientConnected());
        QVERIFY(!runtime.isRemoteOverlayActionsEnabled());
        QVERIFY(runtime.activeProjectExists());
        QCOMPARE(runtime.getActiveCanvas(), originalCanvas);
        if (invalidSnapshot) {
            QTRY_VERIFY_WITH_TIMEOUT(!server.closeCommands.isEmpty(), 1'000);
            QCOMPARE(server.closeCommands.constLast()
                         .value(QStringLiteral("remoteSessionId")).toString(),
                     rejectedSessionId);
            QVERIFY(server.sendClosed(rejectedSessionId, targetEndpointId));
            QTRY_COMPARE_WITH_TIMEOUT(closedSpy.count(), 2, 1'000);
        }

        QVERIFY(server.sendClientList(client));
        QVERIFY(server.sendClientList(client));
        QTRY_COMPARE_WITH_TIMEOUT(listSpy.count(), 3, 1'000);
        runtime.setPointerInsideControlWindow(false);
        runtime.setPointerInsideControlWindow(true);
        QTest::qWait(25);
        QCOMPARE(server.openCommands.size(), 2);
        QVERIFY(!runtime.isRemoteClientConnected());
        QCOMPARE(runtime.getActiveCanvas(), originalCanvas);

        // An explicit selection is the recovery boundary after validation or
        // permanent rejection; it admits a fresh request and valid snapshot.
        runtime.activateClient(targetEndpointId);
        QTRY_COMPARE_WITH_TIMEOUT(server.openCommands.size(), 3, 1'000);
        const QString explicitRequestId = server.openCommands.constLast()
            .value(QStringLiteral("requestId")).toString();
        QVERIFY(explicitRequestId != rejectedRequestId);
        QVERIFY(server.sendOpened(
            QStringLiteral("invalid-auto-session-3"), explicitRequestId,
            targetEndpointId, ScreenInfo(101, 2560, 1440, 0, 0, true), 79));
        QTRY_VERIFY_WITH_TIMEOUT(runtime.isRemoteClientConnected(), 1'000);
        QCOMPARE(runtime.getActiveCanvas(), originalCanvas);
        QCOMPARE(runtime.remoteVolumePercent(), 79);
        QVERIFY(runtime.isRemoteOverlayActionsEnabled());

        ApplicationRuntime::ClientWorkspace* workspace =
            runtime.findWorkspace(targetEndpointId);
        runtime.handleApplicationAboutToQuit();
        runtime.getNavigationManager()->setActiveCanvas(nullptr);
        runtime.setActiveCanvas(nullptr);
        QVERIFY(workspace && workspace->canvas);
        delete workspace->canvas;
        workspace->canvas = nullptr;
    }

    void recoveredProjectWithoutSessionBecomesAvailableWithoutImplicitOpen()
    {
        QTemporaryDir root;
        QVERIFY(root.isValid());

        RuntimeProfileContext context;
        context.ordinal = 2;
        context.instanceId = QStringLiteral("available-after-recovery");
        context.profileId = QStringLiteral("available-after-recovery");
        context.rootPath = root.path();
        context.persistent = false;
        RuntimeProfile::configure(context);

        ApplicationRuntime runtime(context);
        runtime.getWorkspaceManager()->stopAutomaticTimersForTesting();
        runtime.getProjectManager()->stopAutomaticTimersForTesting();

        RemoteSessionTestServer server(
            runtime.getWebSocketClient()->endpointId());
        QVERIFY(server.listen());
        ConnectionManager* connections =
            runtime.findChild<ConnectionManager*>();
        QVERIFY(connections);
        QSignalSpy connectedSpy(runtime.getWebSocketClient(),
                                &WebSocketClient::connected);
        connections->connectToServer(server.url());
        QTRY_COMPARE_WITH_TIMEOUT(connectedSpy.count(), 1, 2'000);

        const QString targetEndpointId = fixtureEndpoint(QLatin1Char('F'));
        ClientInfo client = onlineClient(
            targetEndpointId, QStringLiteral("Recovered target"));
        client.setScreens({});
        client.setVolumePercent(-1);
        QVERIFY(server.sendClientList(client));
        QTRY_COMPARE_WITH_TIMEOUT(runtime.displayClients().size(), 1, 1'000);

        runtime.activateClient(targetEndpointId);
        QTRY_COMPARE_WITH_TIMEOUT(server.openCommands.size(), 1, 1'000);
        QVERIFY(server.sendOpened(
            QStringLiteral("available-session-1"),
            server.openCommands.constFirst()
                .value(QStringLiteral("requestId")).toString(),
            targetEndpointId, ScreenInfo(33, 3440, 1440, 0, 0, true), 52));
        QTRY_VERIFY_WITH_TIMEOUT(runtime.activeProjectExists(), 1'000);

        // Local health must update the selected header and command capability
        // without waiting for another server presence message.
        QTRY_VERIFY_WITH_TIMEOUT(runtime.isRemoteClientConnected(), 1000);
        emit runtime.getWebSocketClient()->transportHealthChanged(true);
        QCOMPARE(runtime.remoteStatusText(), QStringLiteral("DEGRADED"));
        QVERIFY(!runtime.isRemoteClientConnected());
        QVERIFY(!runtime.isRemoteOverlayActionsEnabled());
        emit runtime.getWebSocketClient()->transportHealthChanged(false);
        QCOMPARE(runtime.remoteStatusText(), QStringLiteral("CONNECTED"));
        QVERIFY(runtime.isRemoteClientConnected());
        QCOMPARE(server.openCommands.size(), 1);

        // End the wire session while retaining the local Project/canvas.
        QSignalSpy closedSpy(runtime.getWebSocketClient(),
                            &WebSocketClient::remoteSessionClosed);
        QVERIFY(server.sendClosed(QStringLiteral("available-session-1"),
                                  targetEndpointId));
        QTRY_COMPARE_WITH_TIMEOUT(closedSpy.count(), 1, 1'000);
        QCOMPARE(runtime.remoteStatusText(), QStringLiteral("AVAILABLE"));
        QCOMPARE(server.openCommands.size(), 1);

        QSignalSpy disconnectedSpy(runtime.getWebSocketClient(),
                                   &WebSocketClient::disconnected);
        server.connectionGeneration = 2;
        server.closePeer();
        QTRY_COMPARE_WITH_TIMEOUT(disconnectedSpy.count(), 1, 2'000);
        QTRY_COMPARE_WITH_TIMEOUT(runtime.remoteStatusText(),
                                  QStringLiteral("UNREACHABLE"), 1'000);

        // Same boot and a newer transport generation, but no resumable session.
        // Rediscovery updates the active header; it must not manufacture OPEN2.
        QTRY_COMPARE_WITH_TIMEOUT(connectedSpy.count(), 2, 2'000);
        QVERIFY(server.sendClientList(client));
        QTRY_COMPARE_WITH_TIMEOUT(runtime.remoteStatusText(),
                                  QStringLiteral("AVAILABLE"), 1'000);
        QTest::qWait(50);
        QCOMPARE(server.openCommands.size(), 1);
        QVERIFY(!runtime.isRemoteClientConnected());
        QVERIFY(runtime.activeProjectExists());
        QVERIFY(runtime.getActiveCanvas());

        ApplicationRuntime::ClientWorkspace* workspace =
            runtime.findWorkspace(targetEndpointId);
        runtime.handleApplicationAboutToQuit();
        runtime.getNavigationManager()->setActiveCanvas(nullptr);
        runtime.setActiveCanvas(nullptr);
        QVERIFY(workspace && workspace->canvas);
        delete workspace->canvas;
        workspace->canvas = nullptr;
    }

    void pendingCloseRetriesAfterSameBootLeaseExpiryAndAuthoritativeAbsenceReopens()
    {
        QTemporaryDir root;
        QVERIFY(root.isValid());

        RuntimeProfileContext context;
        context.ordinal = 2;
        context.instanceId = QStringLiteral("same-boot-close-retry");
        context.profileId = QStringLiteral("same-boot-close-retry");
        context.rootPath = root.path();
        context.persistent = false;
        RuntimeProfile::configure(context);

        ApplicationRuntime runtime(context);
        runtime.getWorkspaceManager()->stopAutomaticTimersForTesting();
        runtime.getProjectManager()->stopAutomaticTimersForTesting();
        runtime.getWorkspaceManager()->setRemoteSessionHiddenTimeoutMs(10);

        RemoteSessionTestServer server(
            runtime.getWebSocketClient()->endpointId());
        // Use a short recovery window to exercise command invalidation while
        // the immutable session identity survives reconnect for cleanup.
        server.heartbeatIntervalMs = 250;
        server.leaseTimeoutMs = 1'000;
        QVERIFY(server.listen());
        ConnectionManager* connections =
            runtime.findChild<ConnectionManager*>();
        QVERIFY(connections);
        QSignalSpy connectedSpy(runtime.getWebSocketClient(),
                                &WebSocketClient::connected);
        QSignalSpy disconnectedSpy(runtime.getWebSocketClient(),
                                   &WebSocketClient::disconnected);
        QSignalSpy leaseExpiredSpy(runtime.getWebSocketClient(),
                                  &WebSocketClient::remoteSessionRecoveryExpired);
        connections->connectToServer(server.url());
        QTRY_COMPARE_WITH_TIMEOUT(connectedSpy.count(), 1, 2'000);
        disconnectedSpy.clear();

        const QString targetEndpointId = fixtureEndpoint(QLatin1Char('G'));
        const QString remoteSessionId = QStringLiteral("same-boot-session-1");
        ClientInfo client = onlineClient(
            targetEndpointId, QStringLiteral("Same-boot target"));
        client.setScreens({});
        client.setVolumePercent(-1);
        QVERIFY(server.sendClientList(client));
        QTRY_COMPARE_WITH_TIMEOUT(runtime.displayClients().size(), 1, 1'000);

        runtime.activateClient(targetEndpointId);
        QTRY_COMPARE_WITH_TIMEOUT(server.openCommands.size(), 1, 1'000);
        QVERIFY(server.sendOpened(
            remoteSessionId,
            server.openCommands.constFirst()
                .value(QStringLiteral("requestId")).toString(),
            targetEndpointId, ScreenInfo(34, 1920, 1080, 0, 0, true), 63));
        QTRY_VERIFY_WITH_TIMEOUT(runtime.activeProjectExists(), 1'000);

        const qint64 hiddenAt = QDateTime::currentMSecsSinceEpoch();
        runtime.getWorkspaceManager()->markAllWorkspacesHidden(hiddenAt);
        const qint64 closeAt =
            runtime.getWorkspaceManager()->remoteSessionCloseAtMs(
                targetEndpointId);
        QVERIFY(closeAt >= hiddenAt);
        runtime.getWorkspaceManager()->processDeadlines(closeAt);
        QTRY_COMPARE_WITH_TIMEOUT(server.closeCommands.size(), 1, 1'000);

        const QJsonObject closeOnGeneration1 =
            server.closeCommands.constFirst();
        QCOMPARE(closeOnGeneration1
                     .value(QStringLiteral("remoteSessionId")).toString(),
                 remoteSessionId);
        QCOMPARE(closeOnGeneration1.value(QStringLiteral("generation")).toInt(),
                 1);
        QCOMPARE(closeOnGeneration1
                     .value(QStringLiteral("connectionGeneration")).toInt(),
                 1);

        // Selecting B while its old identity is fenced records a durable
        // replacement intent, but must not emit OPEN until CLOSE converges.
        runtime.activateClient(targetEndpointId);
        QCOMPARE(server.openCommands.size(), 1);
        QCOMPARE(runtime.remoteStatusText(), QStringLiteral("DISCONNECTING"));

        // Stop contact. The exact session expires and loses command capability;
        // its immutable identity survives for authoritative reconciliation.
        server.connectionGeneration = 2;
        server.acknowledgeHeartbeats = false;
        QTRY_COMPARE_WITH_TIMEOUT(leaseExpiredSpy.count(), 1, 3'000);
        QTRY_VERIFY_WITH_TIMEOUT(
             !runtime.getWebSocketClient()->remoteSessionCoordinator()
                ->outgoingForPeer(targetEndpointId).active,
            1'000);
        server.acknowledgeHeartbeats = true;
        QTRY_COMPARE_WITH_TIMEOUT(disconnectedSpy.count(), 1, 2'000);
        QTRY_COMPARE_WITH_TIMEOUT(connectedSpy.count(), 2, 3'000);
        const auto reauthenticatedClose = [&server]() {
            for (const QJsonObject& close : std::as_const(server.closeCommands)) {
                if (close.value("connectionGeneration").toInt() == 2
                    && close.value("reason").toString() == QLatin1String("transport_reauthenticated"))
                    return close;
            }
            return QJsonObject();
        };
        QTRY_VERIFY_WITH_TIMEOUT(!reauthenticatedClose().isEmpty(), 1'000);
        const QJsonObject closeOnGeneration2 = reauthenticatedClose();
        for (const QJsonObject& close : std::as_const(server.closeCommands)) {
            QCOMPARE(close.value("remoteSessionId").toString(), remoteSessionId);
            QCOMPARE(close.value("generation").toInt(), 1);
        }
        QCOMPARE(closeOnGeneration2
                     .value(QStringLiteral("remoteSessionId")).toString(),
                 remoteSessionId);
        QCOMPARE(closeOnGeneration2.value(QStringLiteral("generation")).toInt(),
                 1);
        QCOMPARE(closeOnGeneration2
                     .value(QStringLiteral("connectionGeneration")).toInt(),
                 2);
        QVERIFY(closeOnGeneration2.value(QStringLiteral("requestId")).toString()
                    != closeOnGeneration1
                           .value(QStringLiteral("requestId")).toString());

        // Discovery alone cannot bypass the fence. An authoritative, exactly
        // correlated absence for CLOSE2 releases it and consumes the explicit
        // replacement intent once.
        QVERIFY(server.sendClientList(client));
        QTest::qWait(50);
        QCOMPARE(server.openCommands.size(), 1);
        const int closeCountBeforeAbsence = server.closeCommands.size();
        QVERIFY(server.send(QJsonObject{
            {QStringLiteral("type"), QStringLiteral("error")},
            {QStringLiteral("scope"), QStringLiteral("remote_session")},
            {QStringLiteral("code"), QStringLiteral("unknown_remote_session")},
            {QStringLiteral("message"),
             QStringLiteral("The former session no longer exists")},
            {QStringLiteral("requestId"),
             closeOnGeneration2.value(QStringLiteral("requestId"))},
            {QStringLiteral("remoteSessionId"), remoteSessionId}
        }));
        QTRY_COMPARE_WITH_TIMEOUT(server.openCommands.size(), 2, 1'000);
        QTest::qWait(50);
        QCOMPARE(server.openCommands.size(), 2);
        QCOMPARE(server.closeCommands.size(), closeCountBeforeAbsence);
        QCOMPARE(server.openCommands.constLast()
                     .value(QStringLiteral("targetEndpointId")).toString(),
                 targetEndpointId);
        QCOMPARE(server.openCommands.constLast()
                     .value(QStringLiteral("connectionGeneration")).toInt(),
                 2);

        ApplicationRuntime::ClientWorkspace* workspace =
            runtime.findWorkspace(targetEndpointId);
        runtime.handleApplicationAboutToQuit();
        runtime.getNavigationManager()->setActiveCanvas(nullptr);
        runtime.setActiveCanvas(nullptr);
        QVERIFY(workspace && workspace->canvas);
        delete workspace->canvas;
        workspace->canvas = nullptr;
    }

    void cleanupPendingOpenRetainsIntentUntilAuthenticatedDiscoveryRetry_data()
    {
        QTest::addColumn<QString>("transientCode");
        QTest::newRow("cleanup-pending") << QStringLiteral("session_cleanup_pending");
        QTest::newRow("target-recovering") << QStringLiteral("target_reconnecting");
        QTest::newRow("target-unavailable") << QStringLiteral("target_unavailable");
    }

    void cleanupPendingOpenRetainsIntentUntilAuthenticatedDiscoveryRetry()
    {
        QFETCH(QString, transientCode);
        QTemporaryDir root;
        QVERIFY(root.isValid());

        RuntimeProfileContext context;
        context.ordinal = 2;
        context.instanceId = QStringLiteral("cleanup-pending-open-retry");
        context.profileId = QStringLiteral("cleanup-pending-open-retry");
        context.rootPath = root.path();
        context.persistent = false;
        RuntimeProfile::configure(context);

        ApplicationRuntime runtime(context);
        runtime.setQmlWindowVisible(true);
        runtime.setPointerInsideControlWindow(true);
        runtime.getWorkspaceManager()->stopAutomaticTimersForTesting();
        runtime.getProjectManager()->stopAutomaticTimersForTesting();

        RemoteSessionTestServer server(
            runtime.getWebSocketClient()->endpointId());
        QVERIFY(server.listen());
        ConnectionManager* connections =
            runtime.findChild<ConnectionManager*>();
        QVERIFY(connections);
        QSignalSpy connectedSpy(runtime.getWebSocketClient(),
                                &WebSocketClient::connected);
        connections->connectToServer(server.url());
        QTRY_COMPARE_WITH_TIMEOUT(connectedSpy.count(), 1, 2'000);

        const QString targetEndpointId = fixtureEndpoint(QLatin1Char('I'));
        ClientInfo client = onlineClient(
            targetEndpointId, QStringLiteral("Converging target"));
        client.setScreens({});
        client.setVolumePercent(-1);
        QVERIFY(server.sendClientList(client));
        QTRY_COMPARE_WITH_TIMEOUT(runtime.displayClients().size(), 1, 1'000);

        runtime.activateClient(targetEndpointId);
        QTRY_COMPARE_WITH_TIMEOUT(server.openCommands.size(), 1, 1'000);
        const QJsonObject firstOpen = server.openCommands.constFirst();

        // The previous server-side pair is still CleanupPending. This is a
        // transient convergence response, not a rejection of the user's
        // selection: stay on B and retain exactly one future OPEN intent.
        QVERIFY(server.send(QJsonObject{
            {QStringLiteral("type"), QStringLiteral("error")},
            {QStringLiteral("scope"), QStringLiteral("remote_session")},
            {QStringLiteral("code"),
             transientCode},
            {QStringLiteral("message"),
             QStringLiteral("The previous session is still being cleaned up")},
            {QStringLiteral("requestId"),
             firstOpen.value(QStringLiteral("requestId"))}
        }));
        QTRY_COMPARE_WITH_TIMEOUT(runtime.remoteStatusText(),
                                  QStringLiteral("AVAILABLE"), 1'000);
        QVERIFY(runtime.getNavigationManager()->isOnScreenView());
        QVERIFY(runtime.getNavigationManager()->isLoading());
        QVERIFY(!runtime.activeProjectExists());
        QCOMPARE(server.openCommands.size(), 1);
        QTRY_COMPARE_WITH_TIMEOUT(
            runtime.displayClients().constFirst().availabilityBadgeText(),
            QStringLiteral("Available"), 1'000);

        // No fresh presence event is necessary: the owned retry deadline
        // consumes the retained intent once, without a perpetual polling timer.
        QTRY_COMPARE_WITH_TIMEOUT(server.openCommands.size(), 2, 3'000);
        QTest::qWait(50);
        QCOMPARE(server.openCommands.size(), 2);
        const QJsonObject secondOpen = server.openCommands.constLast();
        QVERIFY(secondOpen.value(QStringLiteral("requestId")).toString()
                != firstOpen.value(QStringLiteral("requestId")).toString());
        QCOMPARE(secondOpen.value(QStringLiteral("targetEndpointId")).toString(),
                 targetEndpointId);

        QVERIFY(server.sendOpened(
            QStringLiteral("cleanup-pending-session-2"),
            secondOpen.value(QStringLiteral("requestId")).toString(),
            targetEndpointId, ScreenInfo(41, 2560, 1440, 0, 0, true), 68));
        QTRY_VERIFY_WITH_TIMEOUT(runtime.activeProjectExists(), 1'000);
        QTRY_VERIFY_WITH_TIMEOUT(runtime.getActiveCanvas(), 1'000);
        QCOMPARE(runtime.remoteStatusText(), QStringLiteral("CONNECTED"));
        QCOMPARE(runtime.remoteVolumePercent(), 68);
        QVERIFY(runtime.isRemoteOverlayActionsEnabled());
        QVERIFY(runtime.canDeleteActiveProject());
        QVERIFY(!runtime.getNavigationManager()->isLoading());
        QVERIFY(runtime.getNavigationManager()->canvasVisible());
        QCOMPARE(server.openCommands.size(), 2);

        ApplicationRuntime::ClientWorkspace* workspace =
            runtime.findWorkspace(targetEndpointId);
        runtime.handleApplicationAboutToQuit();
        runtime.getNavigationManager()->setActiveCanvas(nullptr);
        runtime.setActiveCanvas(nullptr);
        QVERIFY(workspace && workspace->canvas);
        delete workspace->canvas;
        workspace->canvas = nullptr;
    }

    void correlatedCloseErrorDoesNotConsumeAnotherTargetsPendingOpen()
    {
        QTemporaryDir root;
        QVERIFY(root.isValid());

        RuntimeProfileContext context;
        context.ordinal = 2;
        context.instanceId = QStringLiteral("cross-target-close-correlation");
        context.profileId = QStringLiteral("cross-target-close-correlation");
        context.rootPath = root.path();
        context.persistent = false;
        RuntimeProfile::configure(context);

        ApplicationRuntime runtime(context);
        runtime.getWorkspaceManager()->stopAutomaticTimersForTesting();
        runtime.getProjectManager()->stopAutomaticTimersForTesting();
        runtime.getWorkspaceManager()->setRemoteSessionHiddenTimeoutMs(10);

        RemoteSessionTestServer server(
            runtime.getWebSocketClient()->endpointId());
        QVERIFY(server.listen());
        ConnectionManager* connections =
            runtime.findChild<ConnectionManager*>();
        QVERIFY(connections);
        QSignalSpy connectedSpy(runtime.getWebSocketClient(),
                                &WebSocketClient::connected);
        connections->connectToServer(server.url());
        QTRY_COMPARE_WITH_TIMEOUT(connectedSpy.count(), 1, 2'000);

        const QString targetB = fixtureEndpoint(QLatin1Char('H'));
        const QString targetC = fixtureEndpoint(QLatin1Char('I'));
        const QString sessionB = QStringLiteral("correlated-session-b");
        ClientInfo clientB = onlineClient(targetB, QStringLiteral("Target B"));
        ClientInfo clientC = onlineClient(targetC, QStringLiteral("Target C"));
        clientB.setScreens({});
        clientB.setVolumePercent(-1);
        clientC.setScreens({});
        clientC.setVolumePercent(-1);
        QVERIFY(server.send(QJsonObject{
            {QStringLiteral("type"), QStringLiteral("client_list")},
            {QStringLiteral("clients"),
             QJsonArray{clientB.toJson(), clientC.toJson()}}
        }));
        QTRY_COMPARE_WITH_TIMEOUT(runtime.displayClients().size(), 2, 1'000);

        runtime.activateClient(targetB);
        QTRY_COMPARE_WITH_TIMEOUT(server.openCommands.size(), 1, 1'000);
        QVERIFY(server.sendOpened(
            sessionB,
            server.openCommands.constFirst()
                .value(QStringLiteral("requestId")).toString(),
            targetB, ScreenInfo(35, 1920, 1080, 0, 0, true), 47));
        QTRY_VERIFY_WITH_TIMEOUT(runtime.activeProjectExists(), 1'000);

        const qint64 hiddenAt = QDateTime::currentMSecsSinceEpoch();
        runtime.getWorkspaceManager()->markAllWorkspacesHidden(hiddenAt);
        const qint64 closeAt =
            runtime.getWorkspaceManager()->remoteSessionCloseAtMs(targetB);
        QVERIFY(closeAt >= hiddenAt);
        runtime.getWorkspaceManager()->processDeadlines(closeAt);
        QTRY_COMPARE_WITH_TIMEOUT(server.closeCommands.size(), 1, 1'000);
        const QJsonObject closeB = server.closeCommands.constFirst();

        // OPEN C is the only OPEN transaction in flight when the server
        // rejects CLOSE B. Target/request correlation must keep them isolated.
        runtime.activateClient(targetC);
        QTRY_COMPARE_WITH_TIMEOUT(server.openCommands.size(), 2, 1'000);
        const QJsonObject openC = server.openCommands.constLast();
        QCOMPARE(openC.value(QStringLiteral("targetEndpointId")).toString(),
                 targetC);
        QVERIFY(runtime.getNavigationManager()->isOnScreenView());
        QVERIFY(runtime.getNavigationManager()->isLoading());
        QCOMPARE(runtime.getNavigationManager()->currentClientId(), targetC);

        QSignalSpy remoteErrorSpy(runtime.getWebSocketClient(),
                                  &WebSocketClient::remoteSessionError);
        QVERIFY(server.send(QJsonObject{
            {QStringLiteral("type"), QStringLiteral("error")},
            {QStringLiteral("scope"), QStringLiteral("remote_session")},
            {QStringLiteral("code"), QStringLiteral("unknown_remote_session")},
            {QStringLiteral("message"),
             QStringLiteral("Target B session no longer exists")},
            {QStringLiteral("requestId"),
             closeB.value(QStringLiteral("requestId"))},
            {QStringLiteral("remoteSessionId"), sessionB}
        }));
        QTRY_COMPARE_WITH_TIMEOUT(remoteErrorSpy.count(), 1, 1'000);
        QTest::qWait(50);
        QCOMPARE(server.openCommands.size(), 2);
        QCOMPARE(server.closeCommands.size(), 1);
        QVERIFY(runtime.getNavigationManager()->isOnScreenView());
        QVERIFY(runtime.getNavigationManager()->isLoading());
        QCOMPARE(runtime.getNavigationManager()->currentClientId(), targetC);
        QCOMPARE(runtime.remoteStatusText(), QStringLiteral("CONNECTING"));

        QVERIFY(server.sendOpened(
            QStringLiteral("correlated-session-c"),
            openC.value(QStringLiteral("requestId")).toString(),
            targetC, ScreenInfo(36, 2560, 1440, 0, 0, true), 72));
        QTRY_VERIFY_WITH_TIMEOUT(runtime.activeProjectExists(), 1'000);
        QTRY_VERIFY_WITH_TIMEOUT(runtime.getActiveCanvas(), 1'000);
        QCOMPARE(runtime.getNavigationManager()->currentClientId(), targetC);
        QCOMPARE(runtime.remoteStatusText(), QStringLiteral("CONNECTED"));
        QCOMPARE(runtime.remoteVolumePercent(), 72);
        QCOMPARE(server.openCommands.size(), 2);

        ApplicationRuntime::ClientWorkspace* workspaceB =
            runtime.findWorkspace(targetB);
        ApplicationRuntime::ClientWorkspace* workspaceC =
            runtime.findWorkspace(targetC);
        runtime.handleApplicationAboutToQuit();
        runtime.getNavigationManager()->setActiveCanvas(nullptr);
        runtime.setActiveCanvas(nullptr);
        QVERIFY(workspaceB && workspaceB->canvas);
        QVERIFY(workspaceC && workspaceC->canvas);
        delete workspaceB->canvas;
        workspaceB->canvas = nullptr;
        delete workspaceC->canvas;
        workspaceC->canvas = nullptr;
    }

    void incomingReadyDoesNotConsumeOutgoingOpenForSamePeer()
    {
        QTemporaryDir root;
        QVERIFY(root.isValid());

        RuntimeProfileContext context;
        context.ordinal = 2;
        context.instanceId = QStringLiteral("bidirectional-session-correlation");
        context.profileId = QStringLiteral("bidirectional-session-correlation");
        context.rootPath = root.path();
        context.persistent = false;
        RuntimeProfile::configure(context);

        ApplicationRuntime runtime(context);
        runtime.getWorkspaceManager()->stopAutomaticTimersForTesting();
        runtime.getProjectManager()->stopAutomaticTimersForTesting();

        RemoteSessionTestServer server(
            runtime.getWebSocketClient()->endpointId());
        QVERIFY(server.listen());
        ConnectionManager* connections =
            runtime.findChild<ConnectionManager*>();
        QVERIFY(connections);
        QSignalSpy connectedSpy(runtime.getWebSocketClient(),
                                &WebSocketClient::connected);
        connections->connectToServer(server.url());
        QTRY_COMPARE_WITH_TIMEOUT(connectedSpy.count(), 1, 2'000);

        const QString peerEndpointId = fixtureEndpoint(QLatin1Char('J'));
        ClientInfo client = onlineClient(
            peerEndpointId, QStringLiteral("Bidirectional peer"));
        client.setScreens({});
        client.setVolumePercent(-1);
        QVERIFY(server.sendClientList(client));
        QTRY_COMPARE_WITH_TIMEOUT(runtime.displayClients().size(), 1, 1'000);

        runtime.activateClient(peerEndpointId);
        QTRY_COMPARE_WITH_TIMEOUT(server.openCommands.size(), 1, 1'000);
        const QJsonObject outgoingOpen = server.openCommands.constFirst();
        const QString outgoingRequestId =
            outgoingOpen.value(QStringLiteral("requestId")).toString();
        QVERIFY(!outgoingRequestId.isEmpty());
        QVERIFY(runtime.getNavigationManager()->isLoading());
        QCOMPARE(runtime.remoteStatusText(), QStringLiteral("CONNECTING"));

        // The peer independently owns the reverse-direction session. Its
        // Ready envelope must only populate incomingForPeer(), never consume
        // our target-keyed pending OPEN or alter the loading/header state.
        const QString incomingSessionId =
            QStringLiteral("bidirectional-incoming-session");
        QVERIFY(server.sendIncomingOpened(
            // A reverse-direction owner can observe protocol correlations and
            // deliberately reuse ours. Direction/role must namespace it.
            incomingSessionId, outgoingRequestId,
            peerEndpointId));
        QTRY_COMPARE_WITH_TIMEOUT(
            runtime.getWebSocketClient()->remoteSessionCoordinator()
                ->incomingForPeer(peerEndpointId).remoteSessionId,
            incomingSessionId, 1'000);
        QVERIFY(runtime.getWebSocketClient()->remoteSessionCoordinator()
                    ->outgoingForPeer(peerEndpointId).remoteSessionId.isEmpty());

        QVERIFY(server.sendClientList(client));
        QTest::qWait(50);
        QCOMPARE(server.openCommands.size(), 1);
        QVERIFY(runtime.getNavigationManager()->isOnScreenView());
        QVERIFY(runtime.getNavigationManager()->isLoading());
        QCOMPARE(runtime.getNavigationManager()->currentClientId(),
                 peerEndpointId);
        QCOMPARE(runtime.remoteStatusText(), QStringLiteral("CONNECTING"));

        const QString outgoingSessionId =
            QStringLiteral("bidirectional-outgoing-session");
        QVERIFY(server.sendOpened(
            outgoingSessionId, outgoingRequestId, peerEndpointId,
            ScreenInfo(37, 3840, 2160, 0, 0, true), 68));
        QTRY_VERIFY_WITH_TIMEOUT(runtime.activeProjectExists(), 1'000);
        QTRY_VERIFY_WITH_TIMEOUT(runtime.getActiveCanvas(), 1'000);
        QCOMPARE(runtime.remoteStatusText(), QStringLiteral("CONNECTED"));
        QCOMPARE(runtime.remoteVolumePercent(), 68);
        QCOMPARE(runtime.getWebSocketClient()->remoteSessionCoordinator()
                     ->incomingForPeer(peerEndpointId).remoteSessionId,
                 incomingSessionId);
        QCOMPARE(runtime.getWebSocketClient()->remoteSessionCoordinator()
                     ->outgoingForPeer(peerEndpointId).remoteSessionId,
                 outgoingSessionId);
        QCOMPARE(server.openCommands.size(), 1);

        ApplicationRuntime::ClientWorkspace* workspace =
            runtime.findWorkspace(peerEndpointId);
        runtime.handleApplicationAboutToQuit();
        runtime.getNavigationManager()->setActiveCanvas(nullptr);
        runtime.setActiveCanvas(nullptr);
        QVERIFY(workspace && workspace->canvas);
        delete workspace->canvas;
        workspace->canvas = nullptr;
    }

    void concurrentIncomingTeardownsWaitForRendererAdmissionAndBothCommit()
    {
        QTemporaryDir root;
        QVERIFY(root.isValid());

        RuntimeProfileContext context;
        context.ordinal = 2;
        context.instanceId = QStringLiteral("incoming-teardown-queue");
        context.profileId = QStringLiteral("incoming-teardown-queue");
        context.rootPath = root.path();
        context.persistent = false;
        RuntimeProfile::configure(context);

        ApplicationRuntime runtime(context);
        runtime.getWorkspaceManager()->stopAutomaticTimersForTesting();
        runtime.getProjectManager()->stopAutomaticTimersForTesting();

        RemoteSessionTestServer server(
            runtime.getWebSocketClient()->endpointId());
        QVERIFY(server.listen());
        ConnectionManager* connections =
            runtime.findChild<ConnectionManager*>();
        QVERIFY(connections);
        QSignalSpy connectedSpy(runtime.getWebSocketClient(),
                                &WebSocketClient::connected);
        connections->connectToServer(server.url());
        QTRY_COMPARE_WITH_TIMEOUT(connectedSpy.count(), 1, 2'000);

        const QString ownerX = fixtureEndpoint(QLatin1Char('K'));
        const QString ownerY = fixtureEndpoint(QLatin1Char('L'));
        const QString sessionX = QStringLiteral("queued-incoming-session-x");
        const QString sessionY = QStringLiteral("queued-incoming-session-y");
        const QString teardownX = QUuid::createUuid()
            .toString(QUuid::WithoutBraces).toLower();
        const QString teardownY = QUuid::createUuid()
            .toString(QUuid::WithoutBraces).toLower();
        QVERIFY(server.sendIncomingOpened(
            sessionX, QStringLiteral("queued-open-x"), ownerX));
        QVERIFY(server.sendIncomingOpened(
            sessionY, QStringLiteral("queued-open-y"), ownerY));
        QTRY_COMPARE_WITH_TIMEOUT(
            runtime.getWebSocketClient()->remoteSessionCoordinator()
                ->byId(sessionX).phase,
            QStringLiteral("Active"), 1'000);
        QTRY_COMPARE_WITH_TIMEOUT(
            runtime.getWebSocketClient()->remoteSessionCoordinator()
                ->byId(sessionY).phase,
            QStringLiteral("Active"), 1'000);

        RemoteSceneController* renderer =
            runtime.findChild<RemoteSceneController*>();
        QVERIFY(renderer);
        // Creates an asynchronous target-wide destruction barrier. Both
        // terminal envelopes below arrive before its queued settlement, so
        // the first renderer admission is deterministically refused.
        renderer->setEnabled(false);

        const auto terminatingEnvelope = [&](const QString& sessionId,
                                              const QString& ownerEndpointId,
                                              const QString& teardownId) {
            return QJsonObject{
                {QStringLiteral("type"),
                 QStringLiteral("remote_session_terminating")},
                {QStringLiteral("remoteSessionId"), sessionId},
                {QStringLiteral("generation"), 1},
                {QStringLiteral("ownerConnectionGeneration"), 1},
                {QStringLiteral("targetConnectionGeneration"), 1},
                {QStringLiteral("phase"),
                 QStringLiteral("CleanupPending")},
                {QStringLiteral("ownerEndpointId"), ownerEndpointId},
                {QStringLiteral("targetEndpointId"),
                 runtime.getWebSocketClient()->endpointId()},
                {QStringLiteral("teardownId"), teardownId}
            };
        };
        const auto dispatchTerminal = [&](const QJsonObject& envelope) {
            QVERIFY(runtime.getWebSocketClient()->remoteSessionCoordinator()
                        ->upsert(
                            envelope,
                            runtime.getWebSocketClient()
                                ->connectionGeneration()));
            QVERIFY(QMetaObject::invokeMethod(
                runtime.getWebSocketClient(), "remoteSessionTerminating",
                Qt::DirectConnection, Q_ARG(QJsonObject, envelope)));
        };
        dispatchTerminal(terminatingEnvelope(sessionX, ownerX, teardownX));
        dispatchTerminal(terminatingEnvelope(sessionY, ownerY, teardownY));

        const auto hasCommittedAck = [&](const QString& sessionId) {
            for (const QJsonObject& acknowledgement :
                 std::as_const(server.teardownAcknowledgements)) {
                if (acknowledgement
                        .value(QStringLiteral("remoteSessionId")).toString()
                        == sessionId
                    && acknowledgement.value(QStringLiteral("result"))
                           .toString() == QLatin1String("committed")) {
                    return true;
                }
            }
            return false;
        };
        QTRY_VERIFY_WITH_TIMEOUT(hasCommittedAck(sessionX), 3'000);
        QTRY_VERIFY_WITH_TIMEOUT(hasCommittedAck(sessionY), 3'000);

        // A failed admission is queue backpressure. It must never be exposed
        // or retained as a final cleanup_error acknowledgement.
        for (const QJsonObject& acknowledgement :
             std::as_const(server.teardownAcknowledgements)) {
            QCOMPARE(acknowledgement.value(QStringLiteral("result")).toString(),
                     QStringLiteral("committed"));
            QVERIFY(acknowledgement
                        .value(QStringLiteral("sceneStopped")).toBool());
            QVERIFY(acknowledgement
                        .value(QStringLiteral("uploadsAborted")).toBool());
            QVERIFY(acknowledgement
                        .value(QStringLiteral("cacheQuarantined")).toBool());
        }

        runtime.handleApplicationAboutToQuit();
    }

    void settingsClearStorageAndClose_data()
    {
        QTest::addColumn<QString>("channel");
        QTest::newRow("development") << QStringLiteral("development");
        QTest::newRow("production") << QStringLiteral("production");
    }

    void startupFailureActions_data()
    {
        QTest::addColumn<QString>("action");
        QTest::newRow("retry") << QStringLiteral("retry");
        QTest::newRow("close") << QStringLiteral("close");
        QTest::newRow("clear") << QStringLiteral("clear");
        QTest::newRow("media") << QStringLiteral("media");
        QTest::newRow("identity") << QStringLiteral("identity");
    }

    void startupFailureActions()
    {
        QFETCH(QString, action);
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString root = RuntimeProfile::persistentRoot(
            directory.path(), QStringLiteral("development"));
        const QString otherRoot = RuntimeProfile::persistentRoot(
            directory.path(), QStringLiteral("production"));
        QVERIFY(QDir().mkpath(root));
        QVERIFY(QDir().mkpath(otherRoot));
        const QString blockerPath = QDir(root).filePath(QStringLiteral("settings"));
        const QString sentinelPath = QDir(otherRoot).filePath(QStringLiteral("keep"));
        {
            if (action != QLatin1String("media") && action != QLatin1String("identity")) {
                QFile blocker(blockerPath);
                QVERIFY(blocker.open(QIODevice::WriteOnly));
                QCOMPARE(blocker.write("blocked"), qint64(7));
            }
            QFile sentinel(sentinelPath);
            QVERIFY(sentinel.open(QIODevice::WriteOnly));
            QCOMPARE(sentinel.write("other channel"), qint64(13));
        }
        QProcess worker;
        worker.start(QCoreApplication::applicationFilePath(),
                     {QStringLiteral("--startup-failure-worker"), root, action});
        QVERIFY(worker.waitForStarted(5000));
        QVERIFY2(worker.waitForFinished(20000), qPrintable(worker.errorString()));
        QCOMPARE(worker.exitStatus(), QProcess::NormalExit);
        const QByteArray diagnostics = worker.readAllStandardError();
        QVERIFY2(worker.exitCode() == 0, diagnostics.constData());
        QVERIFY2(!diagnostics.contains("TypeError:")
                     && !diagnostics.contains("ReferenceError:"),
                 diagnostics.constData());
        QCOMPARE(QFileInfo::exists(root), action != QLatin1String("clear"));
        QCOMPARE(QFileInfo(blockerPath).isFile(), action == QLatin1String("close"));
        if (action == QLatin1String("retry") || action == QLatin1String("media")
            || action == QLatin1String("identity"))
            QVERIFY(QFileInfo(QDir(blockerPath).filePath(QStringLiteral("settings.ini"))).isFile());
        QFile sentinel(sentinelPath);
        QVERIFY(sentinel.open(QIODevice::ReadOnly));
        QCOMPARE(sentinel.readAll(), QByteArray("other channel"));
    }

    void settingsClearStorageAndClose()
    {
        QFETCH(QString, channel);
        QTemporaryDir directory;
        const QString root = RuntimeProfile::persistentRoot(directory.path(), channel);
        const QString otherChannel = channel == QLatin1String("development")
            ? QStringLiteral("production") : QStringLiteral("development");
        const QString otherRoot = RuntimeProfile::persistentRoot(directory.path(), otherChannel);
        QVERIFY(QDir().mkpath(otherRoot));
        const QString sentinelPath = QDir(otherRoot).filePath(QStringLiteral("keep"));
        {
            QFile sentinel(sentinelPath);
            QVERIFY(sentinel.open(QIODevice::WriteOnly));
            QCOMPARE(sentinel.write("other channel"), qint64(13));
        }
        QProcess worker;
        worker.start(QCoreApplication::applicationFilePath(),
                     {QStringLiteral("--clear-storage-worker"), root, channel});
        QVERIFY(worker.waitForStarted(5000));
        QVERIFY2(worker.waitForFinished(15000), qPrintable(worker.errorString()));
        QCOMPARE(worker.exitStatus(), QProcess::NormalExit);
        const QByteArray diagnostics = worker.readAllStandardError();
        QVERIFY2(worker.exitCode() == 0, diagnostics.constData());
        QVERIFY2(!diagnostics.contains("TypeError:")
                     && !diagnostics.contains("ReferenceError:"),
                 diagnostics.constData());
        // Check after process destruction, including all QObject destructors:
        // no settings/project/cache writer may resurrect the directory.
        QVERIFY(!QFileInfo::exists(root));
        QFile sentinel(sentinelPath);
        QVERIFY(sentinel.open(QIODevice::ReadOnly));
        QCOMPARE(sentinel.readAll(), QByteArray("other channel"));
    }

    void storageBootstrapDoesNotRequestRecoveryAcknowledgement_data()
    {
        QTest::addColumn<bool>("corrupt");
        QTest::newRow("first-launch") << false;
        QTest::newRow("automatic-project-reset") << true;
    }

    void storageBootstrapDoesNotRequestRecoveryAcknowledgement()
    {
        QFETCH(bool, corrupt);
        QTemporaryDir root;
        QVERIFY(root.isValid());
        RuntimeProfileContext context;
        context.rootPath = root.path();
        context.profileId = QStringLiteral("silent-bootstrap-test");
        context.persistent = false;
        if (corrupt) {
            QVERIFY(RuntimeStorageBootstrap(context).run().succeeded());
            QFile projects(RuntimeProfile::projectsFilePath());
            QVERIFY(projects.open(QIODevice::WriteOnly | QIODevice::Truncate));
            QCOMPARE(projects.write("invalid"), qint64(7));
        }
        ApplicationController controller(context,
            {QStringLiteral("tst_ClientConnectionFlow"), QStringLiteral("--server-url=ws://127.0.0.1:1")});
        bool requestedDecision = false;
        connect(&controller, &ApplicationController::bootstrapChanged, this, [&] {
            requestedDecision |= controller.bootstrapDecisionRequired();
        });
        controller.start();
        QTRY_VERIFY_WITH_TIMEOUT(controller.ready(), 8000);
        QVERIFY(!requestedDecision);
        QVERIFY(!controller.bootstrapDecisionRequired());
        controller.handleApplicationAboutToQuit();
    }

    void workspaceDeletionEvictsViewModelAndReopenCreatesFreshPresentation()
    {
        QTemporaryDir root;
        QVERIFY(root.isValid());

        RuntimeProfileContext context;
        context.ordinal = 2;
        context.instanceId = QStringLiteral("controller-workspace-eviction");
        context.profileId = QStringLiteral("controller-workspace-eviction");
        context.rootPath = root.path();
        context.persistent = false;

        QWebSocketServer server(QStringLiteral("controller-workspace-eviction-test"),
                                QWebSocketServer::NonSecureMode);
        QVERIFY(server.listen(QHostAddress::LocalHost, 0));

        const QString bootId = QUuid::createUuid().toString(QUuid::WithoutBraces);
        const QString targetEndpointId = fixtureEndpoint(QLatin1Char('C'));
        const QString firstSessionId = QStringLiteral("controller-session-1");
        const QString secondSessionId = QStringLiteral("controller-session-2");
        QPointer<QWebSocket> peer;
        QString ownerEndpointId;
        QList<QJsonObject> endpointSnapshots;
        QList<QJsonObject> openCommands;
        QList<QJsonObject> closeCommands;
        QJsonObject authentication;

        auto sendServerMessage = [&](QJsonObject message) {
            QVERIFY2(peer, "The fake server has no authenticated peer");
            message.insert(QStringLiteral("protocolVersion"), 12);
            message.insert(QStringLiteral("serverBootId"), bootId);
            completeV7TestEnvelope(message);
            if (message.value(QStringLiteral("messageId")).toString().isEmpty()) {
                message.insert(
                    QStringLiteral("messageId"),
                    QUuid::createUuid().toString(QUuid::WithoutBraces));
            }
            peer->sendTextMessage(QString::fromUtf8(
                QJsonDocument(message).toJson(QJsonDocument::Compact)));
        };

        connect(&server, &QWebSocketServer::newConnection, this, [&]() {
            peer = server.nextPendingConnection();
            QVERIFY(peer);
            peer->setParent(&server);
            sendServerMessage(QJsonObject{
                {QStringLiteral("type"), QStringLiteral("auth_challenge")},
                {QStringLiteral("nonce"), QString::fromLatin1(
                    QByteArray(32, 'v').toBase64(
                        QByteArray::Base64UrlEncoding
                        | QByteArray::OmitTrailingEquals))},
                {QStringLiteral("issuedAt"), 1}
            });

            connect(peer, &QWebSocket::textMessageReceived, &server,
                    [&](const QString& encoded) {
                const QJsonObject message =
                    QJsonDocument::fromJson(encoded.toUtf8()).object();
                const QString type =
                    message.value(QStringLiteral("type")).toString();
                if (type == QLatin1String("auth_response")) {
                    authentication = message;
                    ownerEndpointId =
                        DeviceIdentityStore::endpointIdForInstallation(
                            message.value(QStringLiteral("installationId"))
                                .toString(),
                            message.value(QStringLiteral("instanceId")).toString());
                    const QJsonObject policy{
                        {QStringLiteral("policyVersion"), 5},
                        {QStringLiteral("transportTimeoutMs"), 5000},
                        {QStringLiteral("heartbeatIntervalMs"), 1'000},
                        {QStringLiteral("transportSuspectAfterMs"), 2000},
                        {QStringLiteral("sessionRecoveryTimeoutMs"), 10'000},
                        {QStringLiteral("leaseTimeoutMs"), 2000},
                        {QStringLiteral("scenePrepareTimeoutMs"), 15'000},
                        {QStringLiteral("sceneActivationLeadMs"), 4'000},
                        {QStringLiteral("sceneMaxClockSkewMs"), 50},
                        {QStringLiteral("sceneStartedAckTimeoutMs"), 5'000},
                        {QStringLiteral("sceneMaxStartSkewMs"), 750},
                        {QStringLiteral("sceneStopTimeoutMs"), 5'000},
                        {QStringLiteral("uploadIdleTimeoutMs"), 45'000},
                        {QStringLiteral("uploadTargetAckTimeoutMs"), 30'000},
                        {QStringLiteral("removalAckTimeoutMs"), 30'000}
                    };
                    sendServerMessage(QJsonObject{
                        {QStringLiteral("type"), QStringLiteral("welcome")},
                        {QStringLiteral("connectionId"),
                         QUuid::createUuid().toString(QUuid::WithoutBraces)},
                        {QStringLiteral("installationId"),
                         message.value(QStringLiteral("installationId"))},
                        {QStringLiteral("endpointId"), ownerEndpointId},
                        {QStringLiteral("instanceId"),
                         message.value(QStringLiteral("instanceId"))},
                        {QStringLiteral("instanceOrdinal"),
                         message.value(QStringLiteral("instanceOrdinal"))},
                        {QStringLiteral("runtimeId"),
                         message.value(QStringLiteral("runtimeId"))},
                        {QStringLiteral("connectionGeneration"), 1},
                        {QStringLiteral("policy"), policy},
                        {QStringLiteral("serverMonotonicMs"), 1}
                    });
                } else if (type == QLatin1String("heartbeat")) {
                    sendServerMessage(QJsonObject{
                        {QStringLiteral("type"), QStringLiteral("heartbeat_ack")},
                        {QStringLiteral("connectionGeneration"), 1},
                        {QStringLiteral("sequence"),
                         message.value(QStringLiteral("sequence"))},
                        {QStringLiteral("clientMonotonicMs"),
                         message.value(QStringLiteral("clientMonotonicMs"))},
                        {QStringLiteral("serverMonotonicMs"),
                         message.value(QStringLiteral("clientMonotonicMs"))},
                        {QStringLiteral("serverEpochMs"),
                         QDateTime::currentMSecsSinceEpoch()}
                    });
                } else if (type == QLatin1String("endpoint_snapshot")) {
                    endpointSnapshots.append(message);
                    QJsonObject snapshot = message;
                    for (const QString& key : {QStringLiteral("installationId"),
                                              QStringLiteral("instanceId"),
                                              QStringLiteral("instanceOrdinal"),
                                              QStringLiteral("runtimeId")})
                        snapshot.insert(key, authentication.value(key));
                    snapshot.insert(QStringLiteral("endpointId"), ownerEndpointId);
                    sendServerMessage(QJsonObject{{"type", "endpoint_snapshot_applied"},
                        {"snapshot", snapshot}, {"connectionGeneration", 1}});
                } else if (type == QLatin1String("remote_session_reconcile")) {
                    sendServerMessage(QJsonObject{{"type", "remote_session_reconciled"},
                        {"requestId", message.value("requestId")}, {"sessions", QJsonArray{}},
                        {"complete", true}, {"absentSessionIds", QJsonArray{}}});
                } else if (type == QLatin1String("remote_session_open")) {
                    openCommands.append(message);
                } else if (type == QLatin1String("remote_session_close")) {
                    closeCommands.append(message);
                }
            });
        });

        const QString serverUrl =
            QStringLiteral("ws://127.0.0.1:%1").arg(server.serverPort());
        ApplicationController controller(
            context,
            {QStringLiteral("tst_ClientConnectionFlow"),
             QStringLiteral("--server-url=%1").arg(serverUrl)});
        bool observedMediaBootstrap = false;
        bool backendReadyAtPublication = false;
        connect(&controller, &ApplicationController::bootstrapChanged, this, [&] {
            if (controller.bootstrapDetail() == QStringLiteral("Preparing audio and video…")
                && !controller.ready()) {
                observedMediaBootstrap = true;
                QVERIFY(!controller.connectionEnabled());
            }
        });
        connect(&controller, &ApplicationController::readyChanged, this, [&] {
            const auto prepared = MediaBackendBootstrap::initialize();
            backendReadyAtPublication = prepared.isFinished() && prepared.result().ready;
        });
        controller.start();
        QVERIFY(!controller.ready());
        QTRY_VERIFY_WITH_TIMEOUT(controller.ready(), 8'000);
        QVERIFY(observedMediaBootstrap);
        QVERIFY(backendReadyAtPublication);
        QVERIFY(controller.settingsAppAlwaysOnTop());
        QVERIFY(controller.saveSettings(controller.settingsServerUrl(),
            controller.settingsAutoUpload(), false).isEmpty());
        QVERIFY(!controller.settingsAppAlwaysOnTop());
        QCOMPARE(RuntimeProfile::readSettings().value("appAlwaysOnTop").toBool(), false);
        QVERIFY(controller.saveSettings(controller.settingsServerUrl(),
            controller.settingsAutoUpload(), true).isEmpty());
        QVERIFY(controller.settingsAppAlwaysOnTop());
        QCOMPARE(RuntimeProfile::readSettings().value("appAlwaysOnTop").toBool(), true);
        QTRY_VERIFY_WITH_TIMEOUT(!ownerEndpointId.isEmpty(), 2'000);
        QTRY_VERIFY_WITH_TIMEOUT(!endpointSnapshots.isEmpty(), 2'000);

        ClientInfo client = onlineClient(targetEndpointId,
                                         QStringLiteral("Controller target B"));
        client.setScreens({});
        client.setVolumePercent(-1);
        sendServerMessage(QJsonObject{
            {QStringLiteral("type"), QStringLiteral("client_list")},
            {QStringLiteral("clients"), QJsonArray{client.toJson()}}
        });
        auto* clients = qobject_cast<ClientListModel*>(controller.clientsModel());
        QVERIFY(clients);
        QTRY_COMPARE_WITH_TIMEOUT(clients->rowCount(), 1, 1'000);

        auto snapshot = [](const ScreenInfo& screen, int volume) {
            return QJsonObject{
                {QStringLiteral("screens"), QJsonArray{screen.toJson()}},
                {QStringLiteral("systemUI"), QJsonArray{}},
                {QStringLiteral("volumePercent"), volume},
                {QStringLiteral("revision"), 1},
                {QStringLiteral("capturedAtEpochMs"),
                 static_cast<double>(QDateTime::currentMSecsSinceEpoch())}
            };
        };
        auto openedEnvelope = [&](const QString& sessionId,
                                  const QString& requestId,
                                  const ScreenInfo& screen,
                                  int volume) {
            return QJsonObject{
                {QStringLiteral("type"), QStringLiteral("remote_session_opened")},
                {QStringLiteral("requestId"), requestId},
                {QStringLiteral("remoteSessionId"), sessionId},
                {QStringLiteral("generation"), 1},
                {QStringLiteral("ownerConnectionGeneration"), 1},
                {QStringLiteral("targetConnectionGeneration"), 1},
                {QStringLiteral("phase"), QStringLiteral("Active")},
                {QStringLiteral("ownerEndpointId"), ownerEndpointId},
                {QStringLiteral("targetEndpointId"), targetEndpointId},
                {QStringLiteral("resumeToken"),
                 QStringLiteral("controller-resume-") + sessionId},
                {QStringLiteral("snapshotSequence"), 1},
                {QStringLiteral("snapshot"), snapshot(screen, volume)}
            };
        };

        controller.openClient(targetEndpointId);
        QTRY_COMPARE_WITH_TIMEOUT(openCommands.size(), 1, 1'000);
        const ScreenInfo firstScreen(21, 1920, 1080, 0, 0, true);
        sendServerMessage(openedEnvelope(
            firstSessionId,
            openCommands.constFirst().value(QStringLiteral("requestId")).toString(),
            firstScreen, 35));
        QTRY_VERIFY_WITH_TIMEOUT(controller.activeWorkspace() != nullptr, 1'000);

        auto* firstRaw = qobject_cast<ClientWorkspaceViewModel*>(
            controller.activeWorkspace());
        QVERIFY(firstRaw);
        QPointer<ClientWorkspaceViewModel> firstViewModel(firstRaw);
        QPointer<QObject> firstCanvasController(firstRaw->canvasController());
        QSignalSpy firstDestroyed(firstRaw, &QObject::destroyed);
        QVERIFY(!firstRaw->loading());
        QVERIFY(firstRaw->hasProject());
        QVERIFY(firstRaw->hasScreens());
        QVERIFY(firstRaw->canvasNavigation());
        QVERIFY(firstCanvasController);

        controller.requestDeleteProject();
        QVERIFY(controller.dialogDestructive());
        controller.acceptDialog();

        // workspaceDeleted invalidates the VM synchronously. deleteLater then
        // removes both the cached VM and its detached canvas graph.
        QCOMPARE(controller.activeWorkspace(), nullptr);
        QVERIFY(firstViewModel);
        QVERIFY(firstViewModel->loading());
        QVERIFY(!firstViewModel->canvasNavigation());
        QVERIFY(!firstViewModel->hasScreens());
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        QCoreApplication::processEvents();
        QCOMPARE(firstDestroyed.count(), 1);
        QVERIFY(firstViewModel.isNull());
        QVERIFY(firstCanvasController.isNull());
        QTRY_COMPARE_WITH_TIMEOUT(closeCommands.size(), 1, 1'000);
        QCOMPARE(controller.applicationPage(),
                 ApplicationController::ApplicationPage::Clients);

        // The old wire session still owns the server-side pair. Re-selecting
        // remembers user intent but must not resurrect the evicted VM.
        controller.openClient(targetEndpointId);
        QCOMPARE(controller.applicationPage(),
                 ApplicationController::ApplicationPage::Canvas);
        QCOMPARE(controller.activeWorkspace(), nullptr);
        QCOMPARE(openCommands.size(), 1);

        sendServerMessage(QJsonObject{
            {QStringLiteral("type"), QStringLiteral("remote_session_closed")},
            {QStringLiteral("remoteSessionId"), firstSessionId},
            {QStringLiteral("generation"), 1},
            {QStringLiteral("ownerConnectionGeneration"), 1},
            {QStringLiteral("targetConnectionGeneration"), 1},
            {QStringLiteral("phase"), QStringLiteral("Closed")},
            {QStringLiteral("cleanupState"), QStringLiteral("confirmed")},
            {QStringLiteral("teardownId"),
             QStringLiteral("controller-teardown-1")},
            {QStringLiteral("ownerEndpointId"), ownerEndpointId},
            {QStringLiteral("targetEndpointId"), targetEndpointId}
        });
        QTRY_COMPARE_WITH_TIMEOUT(openCommands.size(), 2, 1'000);

        const ScreenInfo secondScreen(22, 3840, 2160, 0, 0, true);
        sendServerMessage(openedEnvelope(
            secondSessionId,
            openCommands.constLast().value(QStringLiteral("requestId")).toString(),
            secondScreen, 73));
        QTRY_VERIFY_WITH_TIMEOUT(controller.activeWorkspace() != nullptr, 1'000);

        auto* secondViewModel = qobject_cast<ClientWorkspaceViewModel*>(
            controller.activeWorkspace());
        QVERIFY(secondViewModel);
        QCOMPARE(secondViewModel->workspaceId(), targetEndpointId);
        QVERIFY(!secondViewModel->loading());
        QVERIFY(secondViewModel->hasProject());
        QVERIFY(secondViewModel->hasScreens());
        QVERIFY(secondViewModel->canvasNavigation());
        QVERIFY(secondViewModel->mediaEditingEnabled());
        QVERIFY(secondViewModel->mediaSync());
        QVERIFY(secondViewModel->canvasController());
        QVERIFY(firstViewModel.isNull());
        QVERIFY(firstCanvasController.isNull());
        QVERIFY(controller.remoteVolumeVisible());
        QCOMPARE(controller.remoteVolumeText(), QStringLiteral("73%"));
        QVERIFY(controller.hasProject());
        QVERIFY(controller.canDeleteProject());
        QCOMPARE(openCommands.size(), 2);

        // Leave no live canvas behind when this public-controller fixture is
        // destroyed; production shutdown still receives its normal callback.
        controller.requestDeleteProject();
        controller.acceptDialog();
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        controller.handleApplicationAboutToQuit();
    }
};

int main(int argc, char** argv)
{
    QApplication app(argc, argv);
    const QStringList args = app.arguments();
    if (args.size() == 4 && args.at(1) == QLatin1String("--startup-failure-worker")) {
        app.setQuitOnLastWindowClosed(false);
        RuntimeProfileContext profile;
        profile.rootPath = args.at(2);
        profile.channel = QStringLiteral("development");
        profile.persistent = false;
        profile.installationRootPath = RuntimeProfile::persistentInstallationRoot(
            QDir(profile.rootPath).absoluteFilePath(QStringLiteral("../../..")), profile.channel);
        profile.legacyPrimaryRootPath = profile.rootPath;
        const QString action = args.at(3);
        if (action == QLatin1String("identity")) {
            if (!RuntimeStorageBootstrap(profile).run().succeeded()) return 33;
            DeviceIdentityStore identity(profile.installationRootPath, false, profile.identityNamespace());
            QFile key(identity.fallbackFilePath());
            if (!key.open(QIODevice::WriteOnly | QIODevice::Truncate)
                || key.write("corrupt") != 7) return 34;
            key.close();
        }
        const QString blockerPath = QDir(profile.rootPath).filePath(QStringLiteral("settings"));
        ApplicationController::MediaBootstrapFunction mediaBootstrap;
        if (action == QLatin1String("media")) {
            mediaBootstrap = [] {
                QPromise<MediaBackendBootstrap::Result> promise;
                promise.start();
                promise.addResult({false, QStringLiteral("Simulated media failure")});
                promise.finish();
                return promise.future();
            };
        }
        QQuickStyle::setStyle(QStringLiteral("Basic"));
        registerCanvasQmlTypes();
        int requests = 0;
        int code = 0;
        bool handled = false;
        bool ready = false;
        {
            QQmlApplicationEngine engine;
            QmlRuntime::setEngine(&engine);
            ApplicationController controller(profile,
                {QStringLiteral("startup-failure-test"), QStringLiteral("--server-url=ws://127.0.0.1:1")},
                nullptr, mediaBootstrap);
            engine.setInitialProperties({
                {QStringLiteral("controller"), QVariant::fromValue(&controller)}
            });
            engine.loadFromModule(QStringLiteral("Mouffette.App"), QStringLiteral("Main"));
            if (engine.rootObjects().isEmpty()) return 20;
            QObject* shell = engine.rootObjects().constFirst();
            QObject::connect(&controller, &ApplicationController::clearStorageOnExitRequested,
                             &app, [&] { ++requests; });
            QObject::connect(&app, &QCoreApplication::aboutToQuit,
                             &controller, &ApplicationController::handleApplicationAboutToQuit);
            QObject::connect(&controller, &ApplicationController::readyChanged,
                             &app, [&] {
                if (controller.ready()) {
                    ready = true;
                    app.quit();
                }
            });
            QObject::connect(&controller, &ApplicationController::bootstrapChanged,
                             &app, [&] {
                if (handled || !controller.bootstrapDecisionRequired()) return;
                handled = true;
                QTimer::singleShot(0, &controller, [&] {
                    QObject* clear = shell->findChild<QObject*>(
                        QStringLiteral("bootstrapClearStorageAndCloseButton"));
                    QObject* close = shell->findChild<QObject*>(
                        QStringLiteral("bootstrapCloseButton"));
                    QObject* retry = shell->findChild<QObject*>(
                        QStringLiteral("bootstrapRetryButton"));
                    const bool canClear = action != QLatin1String("media")
                        && action != QLatin1String("identity");
                    if (controller.bootstrapCanClearStorage() != canClear || !clear || !close || !retry
                        || clear->property("text").toString() != QLatin1String("Clear storage and close")
                        || !clear->property("destructive").toBool()
                        || clear->property("visible").toBool() != canClear
                        || clear->property("enabled").toBool() != canClear
                        || close->property("text").toString() != QLatin1String("Close")
                        || !close->property("enabled").toBool()
                        || retry->property("text").toString() != QLatin1String("Retry")
                        || !retry->property("enabled").toBool()) {
                        app.exit(21);
                        return;
                    }
                    if (action == QLatin1String("retry")) {
                        if (!QFile::remove(blockerPath)
                            || !QMetaObject::invokeMethod(retry, "clicked")
                            || controller.bootstrapDecisionRequired()
                            || controller.bootstrapCanClearStorage()) {
                            app.exit(22);
                        }
                    } else if (action == QLatin1String("close")
                               || action == QLatin1String("media")
                               || action == QLatin1String("identity")) {
                        if (action == QLatin1String("media") || action == QLatin1String("identity")) {
                            controller.clearStorageAndClose();
                            if (controller.clearingStorage() || requests != 0) {
                                app.exit(32);
                                return;
                            }
                        }
                        if (!QMetaObject::invokeMethod(close, "clicked")) app.exit(23);
                    } else if (action == QLatin1String("clear")) {
                        if (!QMetaObject::invokeMethod(clear, "clicked")) {
                            app.exit(24);
                            return;
                        }
                        controller.clearStorageAndClose(); // A second request is ignored.
                        if (!controller.clearingStorage()) app.exit(25);
                    } else {
                        app.exit(26);
                    }
                });
            });
            QTimer::singleShot(15000, &app, [&] { app.exit(27); });
            controller.start();
            code = app.exec();
            qDeleteAll(engine.rootObjects());
        }
        QThreadPool::globalInstance()->waitForDone();
        if (code != 0 || !handled) return code != 0 ? code : 28;
        if (action == QLatin1String("clear")) {
            if (requests != 1) return 29;
            const auto cleared = RuntimeStorage::clearProfileStorage(profile);
            if (!cleared.succeeded()) { qWarning().noquote() << cleared.reason; return 30; }
        } else if (requests != 0 || (action == QLatin1String("retry") && !ready)) {
            return 31;
        }
        return 0;
    }
    if (args.size() == 4 && args.at(1) == QLatin1String("--clear-storage-worker")) {
        app.setQuitOnLastWindowClosed(false);
        RuntimeProfileContext profile;
        profile.rootPath = args.at(2);
        profile.channel = args.at(3);
        profile.persistent = false;
        profile.installationRootPath = RuntimeProfile::persistentInstallationRoot(
            QDir(profile.rootPath).absoluteFilePath(QStringLiteral("../../..")), profile.channel);
        profile.legacyPrimaryRootPath = profile.rootPath;
        if (!RuntimeStorageBootstrap(profile).run().succeeded()) return 10;
        ProjectRecord project;
        project.projectId = QStringLiteral("shutdown-write");
        project.targetEndpointId = project.target.endpointId = QStringLiteral("target");
        project.createdAtMs = project.updatedAtMs = project.snapshotCapturedAtMs = 1000;
        project.snapshotRevision = 1;
        project.state = ProjectLifecycleState::Visible;
        if (!ProjectStore().save({project})) return 11;
        QQuickStyle::setStyle(QStringLiteral("Basic"));
        registerCanvasQmlTypes();
        int requests = 0;
        int code = 0;
        {
            QQmlApplicationEngine engine;
            QmlRuntime::setEngine(&engine);
            ApplicationController controller(profile,
                {QStringLiteral("storage-clear-test"), QStringLiteral("--server-url=ws://127.0.0.1:1")});
            engine.setInitialProperties({
                {QStringLiteral("controller"), QVariant::fromValue(&controller)}
            });
            // Load the complete shipped shell, including BootstrapWindow:
            // a standalone SettingsDialog misses controller lifetime errors.
            engine.loadFromModule(QStringLiteral("Mouffette.App"), QStringLiteral("Main"));
            if (engine.rootObjects().isEmpty()) return 12;
            QObject* shell = engine.rootObjects().constFirst();
            QObject::connect(&controller, &ApplicationController::clearStorageOnExitRequested,
                             &app, [&] { ++requests; });
            QObject::connect(&app, &QCoreApplication::aboutToQuit,
                             &controller, &ApplicationController::handleApplicationAboutToQuit);
            QObject::connect(&controller, &ApplicationController::readyChanged, &app, [&] {
                if (!controller.ready()) return;
                QTimer::singleShot(0, &controller, [&] {
                    QObject* button = shell->findChild<QObject*>(QStringLiteral("clearStorageAndCloseButton"));
                    if (!button || button->property("text").toString() != QLatin1String("Clear storage and close")
                        || !button->property("destructive").toBool() || !button->property("enabled").toBool()) {
                        app.exit(13);
                        return;
                    }
                    // Exercise the shipped QML action without depending on
                    // native window activation. Pending invalid edits must not
                    // route the clear action through Save validation.
                    if (QObject* field = shell->findChild<QObject*>(QStringLiteral("settingsServerUrl")))
                        field->setProperty("text", QStringLiteral("invalid unsaved URL"));
                    if (!QMetaObject::invokeMethod(button, "clicked")) { app.exit(14); return; }
                    controller.clearStorageAndClose(); // Duplicate delivery is ignored.
                });
            });
            QTimer::singleShot(10000, &app, [&] { app.exit(15); });
            controller.start();
            code = app.exec();
            // Match main: shell, controller (including its windows), then engine.
            qDeleteAll(engine.rootObjects());
        }
        QThreadPool::globalInstance()->waitForDone();
        if (code != 0 || requests != 1) return code != 0 ? code : 16;
        const auto cleared = RuntimeStorage::clearProfileStorage(profile);
        if (!cleared.succeeded()) { qWarning().noquote() << cleared.reason; return 17; }
        return 0;
    }
    ClientConnectionFlowTest test;
    return QTest::qExec(&test, argc, argv);
}
#include "tst_ClientConnectionFlow.moc"
