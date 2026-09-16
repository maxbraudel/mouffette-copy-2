#include <QtTest>

#include "backend/domain/models/ClientInfo.h"
#include "backend/managers/network/ConnectionManager.h"
#include "backend/network/WebSocketClient.h"
#include "backend/network/RemoteSessionCoordinator.h"
#include "backend/security/DeviceIdentityStore.h"

#include <QJsonDocument>
#include <QPointer>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTimer>
#include <QUuid>
#include <QWebSocket>
#include <QWebSocketServer>

#include <openssl/evp.h>
#include <openssl/x509.h>

#include <limits>

class ConnectionManagerTest : public QObject {
    Q_OBJECT

private slots:
    void fastRetrySchedule();
    void backgroundRetrySchedule();
    void clientInfoUsesProtocolIdentity();
    void signedHandshakeAndHeartbeat();
    void handshakeRejectsMalformedEnvelope_data();
    void handshakeRejectsMalformedEnvelope();
    void uploadChannelReadyRequiresExactEnvelope_data();
    void uploadChannelReadyRequiresExactEnvelope();
    void protocolUploadWireSchemaAndActiveGate();
    void suspendInclusiveLeaseBoundaryRejectsLateContact();
};

void ConnectionManagerTest::fastRetrySchedule() {
    QCOMPARE(ConnectionManager::retryDelayForAttempt(0, true), 0);
    QCOMPARE(ConnectionManager::retryDelayForAttempt(1, true), 250);
    QCOMPARE(ConnectionManager::retryDelayForAttempt(2, true), 500);
    QCOMPARE(ConnectionManager::retryDelayForAttempt(3, true), 750);
    QCOMPARE(ConnectionManager::retryDelayForAttempt(20, true), 750);
}

void ConnectionManagerTest::backgroundRetrySchedule() {
    const auto verifyJittered = [](int attempt, int base) {
        const int delay = ConnectionManager::retryDelayForAttempt(attempt, false);
        QVERIFY(delay >= qMax(1, base - base / 5));
        QVERIFY(delay <= base + base / 5);
    };
    verifyJittered(0, 1000);
    verifyJittered(1, 2000);
    verifyJittered(4, 16000);
    verifyJittered(5, 30000);
    verifyJittered(50, 30000);
}

void ConnectionManagerTest::clientInfoUsesProtocolIdentity() {
    const QString installationId(43, QLatin1Char('i'));
    const QString endpointId(43, QLatin1Char('a'));
    const QString instanceId = QStringLiteral("primary");
    const QString runtimeId = QStringLiteral("123e4567-e89b-42d3-a456-426614174000");
    QJsonObject wire{
        {"installationId", installationId},
        {"endpointId", endpointId},
        {"instanceId", instanceId},
        {"instanceOrdinal", 1},
        {"runtimeId", runtimeId},
        {"machineName", "target"},
        {"platform", "Linux"},
        {"status", "Available"},
    };
    ClientInfo client = ClientInfo::fromJson(wire);
    QCOMPARE(client.getId(), endpointId);
    QCOMPARE(client.installationId(), installationId);
    QCOMPARE(client.endpointId(), endpointId);
    QCOMPARE(client.instanceId(), instanceId);
    QCOMPARE(client.instanceOrdinal(), 1);
    QCOMPARE(client.runtimeId(), runtimeId);
    QCOMPARE(client.availabilityStatus(), QStringLiteral("Available"));

    client.setProjectId(QStringLiteral("project-1"));
    client.setHasProject(true);
    client.setRemoteSessionCloseAtMs(60'000);
    client.setProjectDeleteAtMs(300'000);
    const QJsonObject serialized = client.toJson();
    QCOMPARE(serialized.value("endpointId").toString(), endpointId);
    QCOMPARE(serialized.value("runtimeId").toString(), runtimeId);
    QVERIFY(!serialized.contains("id"));
    QVERIFY(!serialized.contains("clientId"));
    QVERIFY(!serialized.contains("persistentClientId"));
    QVERIFY(!serialized.contains("projectId"));
}

void ConnectionManagerTest::signedHandshakeAndHeartbeat() {
    QTemporaryDir identityDirectory;
    QVERIFY(identityDirectory.isValid());
    QWebSocketServer server(QStringLiteral("protocol-v4-test"),
                            QWebSocketServer::NonSecureMode);
    QVERIFY(server.listen(QHostAddress::LocalHost, 0));

    const QString bootId = QUuid::createUuid().toString(QUuid::WithoutBraces);
    const QString nonce = QString::fromLatin1(QByteArray(32, 'n').toBase64(
        QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals));
    bool signatureVerified = false;
    bool heartbeatReceived = false;
    bool acknowledgeHeartbeats = true;
    bool delayNextHeartbeat = false;
    int processNextHeartbeatOnServerMs = 0;
    int heartbeatCount = 0;
    int authenticationCount = 0;
    qint64 serverMonotonicOffsetMs = 4'000'000'000'000LL;
    qint64 serverEpochMs = 1;
    QPointer<QWebSocket> latestConnection;

    connect(&server, &QWebSocketServer::newConnection, this, [&]() {
        QWebSocket* const connection = server.nextPendingConnection();
        QVERIFY(connection != nullptr);
        latestConnection = connection;
        QJsonObject challenge{
            {"type", "auth_challenge"},
            {"protocolVersion", 5},
            {"serverBootId", bootId},
            {"messageId", QUuid::createUuid().toString(QUuid::WithoutBraces)},
            {"nonce", nonce},
            {"issuedAt", 1},
        };
        connection->sendTextMessage(QString::fromUtf8(
            QJsonDocument(challenge).toJson(QJsonDocument::Compact)));

        connect(connection, &QWebSocket::textMessageReceived, this,
                [&, connection](const QString& encoded) {
            const QJsonObject message = QJsonDocument::fromJson(encoded.toUtf8()).object();
            const QString type = message.value("type").toString();
            if (type == QStringLiteral("auth_response")) {
                const int connectionGeneration = ++authenticationCount;
                QCOMPARE(message.value("protocolVersion").toInt(), 5);
                QCOMPARE(message.value("serverBootId").toString(), bootId);
                const QByteArray publicDer = QByteArray::fromBase64(
                    message.value("publicKey").toString().toLatin1(),
                    QByteArray::Base64UrlEncoding | QByteArray::AbortOnBase64DecodingErrors);
                const QByteArray signature = QByteArray::fromBase64(
                    message.value("signature").toString().toLatin1(),
                    QByteArray::Base64UrlEncoding | QByteArray::AbortOnBase64DecodingErrors);
                const QString installationId =
                    DeviceIdentityStore::installationIdForPublicKey(publicDer);
                QCOMPARE(message.value("installationId").toString(), installationId);
                QCOMPARE(message.value("instanceId").toString(), QStringLiteral("primary"));
                const QString endpointId = DeviceIdentityStore::endpointIdForInstallation(
                    installationId, message.value("instanceId").toString());
                const QByteArray signedPayload = QStringLiteral("mouffette-v5\n%1\n%2\n%3\n%4")
                    .arg(bootId, nonce, message.value("runtimeId").toString(),
                         message.value("instanceId").toString()).toUtf8();
                const unsigned char* cursor = reinterpret_cast<const unsigned char*>(
                    publicDer.constData());
                EVP_PKEY* publicKey = d2i_PUBKEY(nullptr, &cursor, publicDer.size());
                QVERIFY(publicKey != nullptr);
                EVP_MD_CTX* context = EVP_MD_CTX_new();
                QVERIFY(context != nullptr);
                QVERIFY(EVP_DigestVerifyInit(context, nullptr, nullptr, nullptr, publicKey) == 1);
                signatureVerified = EVP_DigestVerify(
                    context,
                    reinterpret_cast<const unsigned char*>(signature.constData()), signature.size(),
                    reinterpret_cast<const unsigned char*>(signedPayload.constData()), signedPayload.size()) == 1;
                EVP_MD_CTX_free(context);
                EVP_PKEY_free(publicKey);

                const QJsonObject policy{
                    {"policyVersion", 1},
                    {"heartbeatIntervalMs", 750},
                    {"leaseTimeoutMs", 3000},
                    {"scenePrepareTimeoutMs", 15000},
                    {"sceneActivationLeadMs", 500},
                    {"sceneMaxClockSkewMs", 50},
                    {"sceneStartedAckTimeoutMs", 5000},
                    {"sceneMaxStartSkewMs", 750},
                    {"sceneStopTimeoutMs", 5000},
                    {"uploadIdleTimeoutMs", 45000},
                    {"uploadTargetAckTimeoutMs", 30000},
                    {"removalAckTimeoutMs", 30000},
                };
                QJsonObject welcome{
                    {"type", "welcome"},
                    {"protocolVersion", 5},
                    {"serverBootId", bootId},
                    {"messageId", QUuid::createUuid().toString(QUuid::WithoutBraces)},
                    {"connectionId", QUuid::createUuid().toString(QUuid::WithoutBraces)},
                    {"installationId", installationId},
                    {"endpointId", endpointId},
                    {"instanceId", message.value("instanceId")},
                    {"runtimeId", message.value("runtimeId")},
                    {"connectionGeneration", connectionGeneration},
                    {"policy", policy},
                    {"serverMonotonicMs", 10},
                };
                connection->sendTextMessage(QString::fromUtf8(
                    QJsonDocument(welcome).toJson(QJsonDocument::Compact)));
            } else if (type == QStringLiteral("heartbeat")) {
                QCOMPARE(message.value("protocolVersion").toInt(), 5);
                QCOMPARE(message.value("serverBootId").toString(), bootId);
                heartbeatReceived = true;
                ++heartbeatCount;
                if (!acknowledgeHeartbeats) return;
                QJsonObject ack{
                    {"type", "heartbeat_ack"},
                    {"protocolVersion", 5},
                    {"serverBootId", bootId},
                    {"messageId", QUuid::createUuid().toString(QUuid::WithoutBraces)},
                    {"connectionGeneration", message.value("connectionGeneration")},
                    {"sequence", message.value("sequence")},
                    {"clientMonotonicMs", message.value("clientMonotonicMs")},
                    {"serverMonotonicMs", message.value("clientMonotonicMs").toDouble()
                                                  + serverMonotonicOffsetMs},
                    {"serverEpochMs", static_cast<double>(serverEpochMs)},
                };
                if (processNextHeartbeatOnServerMs > 0) {
                    const int processingMs = processNextHeartbeatOnServerMs;
                    processNextHeartbeatOnServerMs = 0;
                    const qint64 clientSentAt =
                        message.value("clientMonotonicMs").toInteger();
                    const qint64 serverReceivedAt =
                        clientSentAt + serverMonotonicOffsetMs;
                    const qint64 serverTransmittedAt =
                        serverReceivedAt + qMax(0, processingMs - 100);
                    ack.insert("serverMonotonicMs",
                               static_cast<double>(serverTransmittedAt));
                    ack.insert("serverReceiveMonotonicMs",
                               static_cast<double>(serverReceivedAt));
                    ack.insert("serverTransmitMonotonicMs",
                               static_cast<double>(serverTransmittedAt));
                    const QString processedAck = QString::fromUtf8(
                        QJsonDocument(ack).toJson(QJsonDocument::Compact));
                    QPointer<QWebSocket> guardedPeer(connection);
                    QTimer::singleShot(processingMs, this,
                                       [guardedPeer, processedAck]() {
                        if (guardedPeer) guardedPeer->sendTextMessage(processedAck);
                    });
                    return;
                }
                const QString encodedAck = QString::fromUtf8(
                    QJsonDocument(ack).toJson(QJsonDocument::Compact));
                if (delayNextHeartbeat) {
                    delayNextHeartbeat = false;
                    QPointer<QWebSocket> guardedPeer(connection);
                    QTimer::singleShot(160, this, [guardedPeer, encodedAck]() {
                        if (guardedPeer) guardedPeer->sendTextMessage(encodedAck);
                    });
                } else {
                    connection->sendTextMessage(encodedAck);
                }
            }
        });
    });

    WebSocketClient client(identityDirectory.path(), false);
    QSignalSpy connectedSpy(&client, &WebSocketClient::connected);
    QSignalSpy heartbeatSpy(&client, &WebSocketClient::heartbeatSampleReceived);
    client.connectToServer(QStringLiteral("ws://127.0.0.1:%1").arg(server.serverPort()));
    QTRY_COMPARE_WITH_TIMEOUT(connectedSpy.count(), 1, 2000);
    QTRY_VERIFY_WITH_TIMEOUT(signatureVerified, 2000);
    QTRY_VERIFY_WITH_TIMEOUT(heartbeatReceived, 2000);
    QTRY_VERIFY_WITH_TIMEOUT(heartbeatSpy.count() >= 1, 2000);
    const qint64 preciseUncertainty = client.sceneClockUncertaintyMs();
    QVERIFY(preciseUncertainty >= 0);
    const QList<QVariant> preciseSample = heartbeatSpy.last();
    const qint64 preciseOffset = preciseSample.at(2).toLongLong();
    QVERIFY(qAbs(preciseOffset - serverMonotonicOffsetMs) <= preciseUncertainty);

    // A delayed heartbeat is a queueing outlier, not evidence that the
    // previously established clock mapping became less precise. Scene launch
    // must keep using the best recent NTP-style sample.
    delayNextHeartbeat = true;
    QTRY_VERIFY_WITH_TIMEOUT([&heartbeatSpy]() {
        for (const QList<QVariant>& sample : heartbeatSpy) {
            if (sample.at(1).toLongLong() >= 120) return true;
        }
        return false;
    }(), 2000);
    QVERIFY(client.sceneClockUncertaintyMs() <= preciseUncertainty);

    // Civil time, local timezone and monotonic time are separate domains.
    // Even a nonsensical wall-clock jump in the heartbeat metadata must not
    // perturb the server-monotonic mapping used to schedule a scene.
    serverEpochMs = 8'000'000'000'000'000LL;
    const int samplesBeforeEpochJump = heartbeatSpy.count();
    processNextHeartbeatOnServerMs = 500;
    QTRY_VERIFY_WITH_TIMEOUT([&]() {
        for (int index = samplesBeforeEpochJump;
            index < heartbeatSpy.count(); ++index) {
            const QList<QVariant> sample = heartbeatSpy.at(index);
            // A busy relay is not network uncertainty: the four-timestamp
            // sample subtracts its measured processing interval from RTT.
            const qint64 rttMs = sample.at(1).toLongLong();
            const qint64 uncertaintyMs = sample.at(3).toLongLong();
            if (rttMs >= 400
                && uncertaintyMs * 2 <= rttMs - 300) return true;
        }
        return false;
    }(), 2500);
    const QList<QVariant> postEpochJumpSample = heartbeatSpy.last();
    const qint64 postEpochJumpOffset = postEpochJumpSample.at(2).toLongLong();
    const qint64 postEpochJumpUncertainty = postEpochJumpSample.at(3).toLongLong();
    QVERIFY(qAbs(postEpochJumpOffset - serverMonotonicOffsetMs)
            <= postEpochJumpUncertainty);

    // A replacement transport must never inherit an old clock mapping. Its
    // first (delayed) probe models a cold start over a 120+ ms RTT path; the
    // following low-delay sample must then recover scene-launch quality.
    const int samplesBeforeReconnect = heartbeatSpy.count();
    QVector<qint64> reconnectSelectedUncertainties;
    const QMetaObject::Connection reconnectSampleConnection = connect(
        &client, &WebSocketClient::heartbeatSampleReceived, this,
        [&](quint64, qint64, qint64, qint64) {
            if (heartbeatSpy.count() > samplesBeforeReconnect) {
                reconnectSelectedUncertainties.append(client.sceneClockUncertaintyMs());
            }
        });
    delayNextHeartbeat = true;
    serverMonotonicOffsetMs = 5'000'000'000'000LL;
    client.connectToServer(QStringLiteral("ws://127.0.0.1:%1").arg(server.serverPort()));
    QVERIFY(client.sceneClockUncertaintyMs() > 50);
    QTRY_COMPARE_WITH_TIMEOUT(connectedSpy.count(), 2, 2000);
    QCOMPARE(authenticationCount, 2);
    QVERIFY(client.sceneClockUncertaintyMs() > 50);
    QTRY_VERIFY_WITH_TIMEOUT(!reconnectSelectedUncertainties.isEmpty(), 2000);
    QVERIFY(reconnectSelectedUncertainties.first() > 50);

    const int probesBeforeRequestedBurst = heartbeatCount;
    QVERIFY(client.requestSceneClockSynchronization());
    // This must be the explicit probe, not the 750 ms passive heartbeat.
    QTRY_VERIFY_WITH_TIMEOUT(heartbeatCount > probesBeforeRequestedBurst, 200);
    QTRY_VERIFY_WITH_TIMEOUT(client.sceneClockUncertaintyMs() <= 50, 2000);
    QVERIFY(reconnectSelectedUncertainties.last() <= 50);
    const QList<QVariant> recoveredSample = heartbeatSpy.last();
    const qint64 recoveredOffset = recoveredSample.at(2).toLongLong();
    const qint64 recoveredUncertainty = recoveredSample.at(3).toLongLong();
    QVERIFY(qAbs(recoveredOffset - serverMonotonicOffsetMs) <= recoveredUncertainty);
    QCOMPARE(client.connectionGeneration(), quint64(2));
    disconnect(reconnectSampleConnection);
    QCOMPARE(client.serverBootId(), bootId);
    QVERIFY(client.hasUnexpiredLease());

    // Non-clock traffic may keep a transport lease healthy even if heartbeat
    // acknowledgements are selectively lost. The last precise mapping must
    // still expire on its own lease-sized freshness boundary.
    acknowledgeHeartbeats = false;
    QTimer keepAlive;
    keepAlive.setInterval(200);
    connect(&keepAlive, &QTimer::timeout, this, [&]() {
        if (!latestConnection) return;
        const QJsonObject message{
            {"type", "client_list"},
            {"protocolVersion", 5},
            {"serverBootId", bootId},
            {"messageId", QUuid::createUuid().toString(QUuid::WithoutBraces)},
            {"connectionGeneration", static_cast<double>(client.connectionGeneration())},
            {"clients", QJsonArray{}},
        };
        latestConnection->sendTextMessage(QString::fromUtf8(
            QJsonDocument(message).toJson(QJsonDocument::Compact)));
    });
    keepAlive.start();
    QTRY_VERIFY_WITH_TIMEOUT(client.sceneClockUncertaintyMs() > 50, 4000);
    QCOMPARE(client.estimatedServerMonotonicMs(), qint64(-1));
    QVERIFY(client.hasUnexpiredLease());

    // When sampling resumes, the expired low-delay entry must not win the
    // minimum-delay filter again over the new (deliberately slower) sample.
    const int samplesBeforeFreshMapping = heartbeatSpy.count();
    delayNextHeartbeat = true;
    acknowledgeHeartbeats = true;
    QTRY_VERIFY_WITH_TIMEOUT([&]() {
        for (int index = samplesBeforeFreshMapping;
             index < heartbeatSpy.count(); ++index) {
            if (heartbeatSpy.at(index).at(1).toLongLong() >= 120) return true;
        }
        return false;
    }(), 2000);
    QVERIFY(client.sceneClockUncertaintyMs() > 50);
    QVERIFY(client.sceneClockUncertaintyMs()
            < std::numeric_limits<qint64>::max());
    QVERIFY(client.estimatedServerMonotonicMs() >= 0);
    keepAlive.stop();
    client.disconnect();
}

void ConnectionManagerTest::suspendInclusiveLeaseBoundaryRejectsLateContact()
{
    QTemporaryDir identityDirectory;
    QVERIFY(identityDirectory.isValid());
    QWebSocketServer server(QStringLiteral("continuous-lease-clock-test"),
                            QWebSocketServer::NonSecureMode);
    QVERIFY(server.listen(QHostAddress::LocalHost, 0));
    const QString bootId = QUuid::createUuid().toString(QUuid::WithoutBraces);
    const QString nonce = QString::fromLatin1(QByteArray(32, 'c').toBase64(
        QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals));
    QPointer<QWebSocket> peer;

    connect(&server, &QWebSocketServer::newConnection, this, [&]() {
        peer = server.nextPendingConnection();
        QVERIFY(peer);
        peer->sendTextMessage(QString::fromUtf8(QJsonDocument(QJsonObject{
            {"type", "auth_challenge"}, {"protocolVersion", 5},
            {"serverBootId", bootId},
            {"messageId", QUuid::createUuid().toString(QUuid::WithoutBraces)},
            {"nonce", nonce}, {"issuedAt", 1},
        }).toJson(QJsonDocument::Compact)));
        connect(peer, &QWebSocket::textMessageReceived, this,
                [&, peer](const QString& encoded) {
            const QJsonObject request =
                QJsonDocument::fromJson(encoded.toUtf8()).object();
            if (request.value("type").toString()
                != QLatin1String("auth_response")) {
                return;
            }
            const QJsonObject policy{
                {"policyVersion", 1}, {"heartbeatIntervalMs", 750},
                {"leaseTimeoutMs", 3000}, {"scenePrepareTimeoutMs", 15000},
                {"sceneActivationLeadMs", 4000}, {"sceneMaxClockSkewMs", 50},
                {"sceneStartedAckTimeoutMs", 5000}, {"sceneMaxStartSkewMs", 750},
                {"sceneStopTimeoutMs", 5000},
                {"uploadIdleTimeoutMs", 45000},
                {"uploadTargetAckTimeoutMs", 30000},
                {"removalAckTimeoutMs", 30000},
            };
            peer->sendTextMessage(QString::fromUtf8(QJsonDocument(QJsonObject{
                {"type", "welcome"}, {"protocolVersion", 5},
                {"serverBootId", bootId},
                {"messageId", QUuid::createUuid().toString(QUuid::WithoutBraces)},
                {"connectionId", QUuid::createUuid().toString(QUuid::WithoutBraces)},
                {"installationId", request.value("installationId")},
                {"endpointId", DeviceIdentityStore::endpointIdForInstallation(
                     request.value("installationId").toString(),
                     request.value("instanceId").toString())},
                {"instanceId", request.value("instanceId")},
                {"runtimeId", request.value("runtimeId")},
                {"connectionGeneration", 1}, {"policy", policy},
                {"serverMonotonicMs", 1},
            }).toJson(QJsonDocument::Compact)));
        });
    });

    qint64 continuousNowMs = 10'000;
    WebSocketClient client(identityDirectory.path(), false, nullptr,
                           [&continuousNowMs]() { return continuousNowMs; });
    QSignalSpy connectedSpy(&client, &WebSocketClient::connected);
    QSignalSpy leaseExpiredSpy(&client, &WebSocketClient::leaseExpired);
    QSignalSpy listSpy(&client, &WebSocketClient::clientListReceived);
    client.connectToServer(QStringLiteral("ws://127.0.0.1:%1")
                               .arg(server.serverPort()));
    QTRY_COMPARE_WITH_TIMEOUT(connectedSpy.count(), 1, 2000);
    QVERIFY(client.hasUnexpiredLease());
    QCOMPARE(client.leaseRemainingMs(), qint64(3000));

    continuousNowMs += 2999;
    QVERIFY(client.hasUnexpiredLease());
    QCOMPARE(client.leaseRemainingMs(), qint64(1));

    continuousNowMs += 1;
    QVERIFY(!client.hasUnexpiredLease());
    // A valid packet at exactly 3000 ms cannot refresh lastContact. It first
    // drives the irreversible terminal edge and is never dispatched.
    peer->sendTextMessage(QString::fromUtf8(QJsonDocument(QJsonObject{
        {"type", "client_list"}, {"protocolVersion", 5},
        {"serverBootId", bootId},
        {"messageId", QUuid::createUuid().toString(QUuid::WithoutBraces)},
        {"connectionGeneration", 1}, {"clients", QJsonArray{}},
    }).toJson(QJsonDocument::Compact)));
    QTRY_COMPARE_WITH_TIMEOUT(leaseExpiredSpy.count(), 1, 1000);
    QCOMPARE(listSpy.count(), 0);
    QVERIFY(!client.hasUnexpiredLease());
    client.disconnect();
}

void ConnectionManagerTest::handshakeRejectsMalformedEnvelope_data() {
    QTest::addColumn<QString>("scenario");
    QTest::newRow("challenge-missing-message-id")
        << QStringLiteral("challenge_missing_message_id");
    QTest::newRow("challenge-fractional-protocol")
        << QStringLiteral("challenge_fractional_protocol");
    QTest::newRow("challenge-fractional-issued-at")
        << QStringLiteral("challenge_fractional_issued_at");
    QTest::newRow("welcome-missing-message-id")
        << QStringLiteral("welcome_missing_message_id");
    QTest::newRow("welcome-fractional-generation")
        << QStringLiteral("welcome_fractional_generation");
    QTest::newRow("welcome-fractional-monotonic")
        << QStringLiteral("welcome_fractional_monotonic");
    QTest::newRow("welcome-fractional-clock-policy")
        << QStringLiteral("welcome_fractional_clock_policy");
}

void ConnectionManagerTest::handshakeRejectsMalformedEnvelope() {
    QFETCH(QString, scenario);
    QTemporaryDir identityDirectory;
    QVERIFY(identityDirectory.isValid());
    QWebSocketServer server(QStringLiteral("malformed-handshake-test"),
                            QWebSocketServer::NonSecureMode);
    QVERIFY(server.listen(QHostAddress::LocalHost, 0));
    const QString bootId = QUuid::createUuid().toString(QUuid::WithoutBraces);
    QPointer<QWebSocket> peer;

    connect(&server, &QWebSocketServer::newConnection, this, [&]() {
        peer = server.nextPendingConnection();
        QVERIFY(peer);
        QJsonObject challenge{
            {QStringLiteral("type"), QStringLiteral("auth_challenge")},
            {QStringLiteral("protocolVersion"), 5},
            {QStringLiteral("serverBootId"), bootId},
            {QStringLiteral("messageId"),
             QUuid::createUuid().toString(QUuid::WithoutBraces)},
            {QStringLiteral("nonce"), QString::fromLatin1(
                 QByteArray(32, 'm').toBase64(
                     QByteArray::Base64UrlEncoding
                     | QByteArray::OmitTrailingEquals))},
            {QStringLiteral("issuedAt"), 1}
        };
        if (scenario == QLatin1String("challenge_missing_message_id")) {
            challenge.remove(QStringLiteral("messageId"));
        } else if (scenario == QLatin1String("challenge_fractional_protocol")) {
            challenge.insert(QStringLiteral("protocolVersion"), 2.5);
        } else if (scenario == QLatin1String("challenge_fractional_issued_at")) {
            challenge.insert(QStringLiteral("issuedAt"), 1.5);
        }
        peer->sendTextMessage(QString::fromUtf8(
            QJsonDocument(challenge).toJson(QJsonDocument::Compact)));

        connect(peer, &QWebSocket::textMessageReceived, this,
                [&, peer](const QString& encoded) {
            const QJsonObject request =
                QJsonDocument::fromJson(encoded.toUtf8()).object();
            if (request.value(QStringLiteral("type")).toString()
                != QLatin1String("auth_response")) {
                return;
            }
            QJsonObject policy{
                {QStringLiteral("policyVersion"), 1},
                {QStringLiteral("heartbeatIntervalMs"), 750},
                {QStringLiteral("leaseTimeoutMs"), 3000},
                {QStringLiteral("scenePrepareTimeoutMs"), 15000},
                {QStringLiteral("sceneActivationLeadMs"), 4000},
                {QStringLiteral("sceneMaxClockSkewMs"), 50},
                {QStringLiteral("sceneStartedAckTimeoutMs"), 5000},
                {QStringLiteral("sceneMaxStartSkewMs"), 750},
                {QStringLiteral("sceneStopTimeoutMs"), 5000},
                {QStringLiteral("uploadIdleTimeoutMs"), 45000},
                {QStringLiteral("uploadTargetAckTimeoutMs"), 30000},
                {QStringLiteral("removalAckTimeoutMs"), 30000}
            };
            if (scenario == QLatin1String("welcome_fractional_clock_policy")) {
                policy.insert(QStringLiteral("sceneMaxClockSkewMs"), 50.5);
            }
            QJsonObject welcome{
                {QStringLiteral("type"), QStringLiteral("welcome")},
                {QStringLiteral("protocolVersion"), 5},
                {QStringLiteral("serverBootId"), bootId},
                {QStringLiteral("messageId"),
                 QUuid::createUuid().toString(QUuid::WithoutBraces)},
                {QStringLiteral("connectionId"),
                 QUuid::createUuid().toString(QUuid::WithoutBraces)},
                {QStringLiteral("installationId"),
                 request.value(QStringLiteral("installationId"))},
                {QStringLiteral("endpointId"),
                 DeviceIdentityStore::endpointIdForInstallation(
                     request.value(QStringLiteral("installationId")).toString(),
                     request.value(QStringLiteral("instanceId")).toString())},
                {QStringLiteral("instanceId"),
                 request.value(QStringLiteral("instanceId"))},
                {QStringLiteral("runtimeId"),
                 request.value(QStringLiteral("runtimeId"))},
                {QStringLiteral("connectionGeneration"), 1},
                {QStringLiteral("policy"), policy},
                {QStringLiteral("serverMonotonicMs"), 1}
            };
            if (scenario == QLatin1String("welcome_missing_message_id")) {
                welcome.remove(QStringLiteral("messageId"));
            } else if (scenario == QLatin1String("welcome_fractional_generation")) {
                welcome.insert(QStringLiteral("connectionGeneration"), 1.5);
            } else if (scenario == QLatin1String("welcome_fractional_monotonic")) {
                welcome.insert(QStringLiteral("serverMonotonicMs"), 1.5);
            }
            peer->sendTextMessage(QString::fromUtf8(
                QJsonDocument(welcome).toJson(QJsonDocument::Compact)));
        });
    });

    WebSocketClient client(identityDirectory.path(), false);
    QSignalSpy fatalSpy(&client, &WebSocketClient::fatalError);
    QSignalSpy connectedSpy(&client, &WebSocketClient::connected);
    client.connectToServer(
        QStringLiteral("ws://127.0.0.1:%1").arg(server.serverPort()));
    QTRY_COMPARE_WITH_TIMEOUT(fatalSpy.count(), 1, 2000);
    QCOMPARE(connectedSpy.count(), 0);
    QVERIFY(!client.isConnected());
}

void ConnectionManagerTest::uploadChannelReadyRequiresExactEnvelope_data() {
    QTest::addColumn<QString>("scenario");
    QTest::newRow("missing-message-id")
        << QStringLiteral("missing_message_id");
    QTest::newRow("fractional-generation")
        << QStringLiteral("fractional_generation");
    QTest::newRow("wrong-server-boot")
        << QStringLiteral("wrong_server_boot");
}

void ConnectionManagerTest::uploadChannelReadyRequiresExactEnvelope() {
    QFETCH(QString, scenario);
    QTemporaryDir identityDirectory;
    QVERIFY(identityDirectory.isValid());
    QWebSocketServer server(QStringLiteral("upload-ready-envelope-test"),
                            QWebSocketServer::NonSecureMode);
    QVERIFY(server.listen(QHostAddress::LocalHost, 0));

    const QString bootId = QUuid::createUuid().toString(QUuid::WithoutBraces);
    const QString token(43, QLatin1Char('t'));
    QPointer<QWebSocket> controlPeer;
    QPointer<QWebSocket> uploadPeer;
    bool uploadPeerSeen = false;

    connect(&server, &QWebSocketServer::newConnection, this, [&]() {
        QWebSocket* candidate = server.nextPendingConnection();
        QVERIFY(candidate);
        if (candidate->requestUrl().query().contains(
                QStringLiteral("channel=upload"))) {
            uploadPeer = candidate;
            uploadPeerSeen = true;
            QJsonObject ready{
                {QStringLiteral("type"), QStringLiteral("upload_channel_ready")},
                {QStringLiteral("protocolVersion"), 5},
                {QStringLiteral("serverBootId"), bootId},
                {QStringLiteral("messageId"),
                 QUuid::createUuid().toString(QUuid::WithoutBraces)},
                {QStringLiteral("endpointId"),
                 controlPeer ? controlPeer->property("endpointId").toString()
                             : QString()},
                {QStringLiteral("connectionGeneration"), 1}
            };
            if (scenario == QLatin1String("missing_message_id")) {
                ready.remove(QStringLiteral("messageId"));
            } else if (scenario == QLatin1String("fractional_generation")) {
                ready.insert(QStringLiteral("connectionGeneration"), 1.5);
            } else if (scenario == QLatin1String("wrong_server_boot")) {
                ready.insert(QStringLiteral("serverBootId"),
                             QUuid::createUuid().toString(QUuid::WithoutBraces));
            }
            candidate->sendTextMessage(QString::fromUtf8(
                QJsonDocument(ready).toJson(QJsonDocument::Compact)));
            return;
        }

        controlPeer = candidate;
        const QJsonObject challenge{
            {QStringLiteral("type"), QStringLiteral("auth_challenge")},
            {QStringLiteral("protocolVersion"), 5},
            {QStringLiteral("serverBootId"), bootId},
            {QStringLiteral("messageId"),
             QUuid::createUuid().toString(QUuid::WithoutBraces)},
            {QStringLiteral("nonce"), QString::fromLatin1(
                 QByteArray(32, 'c').toBase64(
                     QByteArray::Base64UrlEncoding
                     | QByteArray::OmitTrailingEquals))},
            {QStringLiteral("issuedAt"), 1}
        };
        candidate->sendTextMessage(QString::fromUtf8(
            QJsonDocument(challenge).toJson(QJsonDocument::Compact)));
        connect(candidate, &QWebSocket::textMessageReceived, this,
                [&, candidate](const QString& encoded) {
            const QJsonObject request =
                QJsonDocument::fromJson(encoded.toUtf8()).object();
            const QString type = request.value(QStringLiteral("type")).toString();
            if (type == QLatin1String("auth_response")) {
                const QString endpointId = DeviceIdentityStore::endpointIdForInstallation(
                    request.value(QStringLiteral("installationId")).toString(),
                    request.value(QStringLiteral("instanceId")).toString());
                candidate->setProperty("endpointId", endpointId);
                const QJsonObject policy{
                    {QStringLiteral("policyVersion"), 1},
                    {QStringLiteral("heartbeatIntervalMs"), 750},
                    {QStringLiteral("leaseTimeoutMs"), 3000},
                    {QStringLiteral("scenePrepareTimeoutMs"), 15000},
                    {QStringLiteral("sceneActivationLeadMs"), 4000},
                    {QStringLiteral("sceneMaxClockSkewMs"), 50},
                    {QStringLiteral("sceneStartedAckTimeoutMs"), 5000},
                    {QStringLiteral("sceneMaxStartSkewMs"), 750},
                    {QStringLiteral("sceneStopTimeoutMs"), 5000},
                    {QStringLiteral("uploadIdleTimeoutMs"), 45000},
                    {QStringLiteral("uploadTargetAckTimeoutMs"), 30000},
                    {QStringLiteral("removalAckTimeoutMs"), 30000}
                };
                candidate->sendTextMessage(QString::fromUtf8(QJsonDocument(
                    QJsonObject{
                        {QStringLiteral("type"), QStringLiteral("welcome")},
                        {QStringLiteral("protocolVersion"), 5},
                        {QStringLiteral("serverBootId"), bootId},
                        {QStringLiteral("messageId"),
                         QUuid::createUuid().toString(QUuid::WithoutBraces)},
                        {QStringLiteral("connectionId"),
                         QUuid::createUuid().toString(QUuid::WithoutBraces)},
                        {QStringLiteral("installationId"),
                         request.value(QStringLiteral("installationId"))},
                        {QStringLiteral("endpointId"), endpointId},
                        {QStringLiteral("instanceId"),
                         request.value(QStringLiteral("instanceId"))},
                        {QStringLiteral("runtimeId"),
                         request.value(QStringLiteral("runtimeId"))},
                        {QStringLiteral("connectionGeneration"), 1},
                        {QStringLiteral("policy"), policy},
                        {QStringLiteral("serverMonotonicMs"), 1}
                    }).toJson(QJsonDocument::Compact)));
            } else if (type == QLatin1String("request_upload_channel")) {
                candidate->sendTextMessage(QString::fromUtf8(QJsonDocument(
                    QJsonObject{
                        {QStringLiteral("type"),
                         QStringLiteral("upload_channel_token")},
                        {QStringLiteral("protocolVersion"), 5},
                        {QStringLiteral("serverBootId"), bootId},
                        {QStringLiteral("messageId"),
                         QUuid::createUuid().toString(QUuid::WithoutBraces)},
                        {QStringLiteral("connectionGeneration"), 1},
                        {QStringLiteral("token"), token},
                        {QStringLiteral("expiresAt"), 1000}
                    }).toJson(QJsonDocument::Compact)));
            }
        });
    });

    WebSocketClient client(identityDirectory.path(), false);
    QSignalSpy connectedSpy(&client, &WebSocketClient::connected);
    client.connectToServer(
        QStringLiteral("ws://127.0.0.1:%1").arg(server.serverPort()));
    QTRY_COMPARE_WITH_TIMEOUT(connectedSpy.count(), 1, 2000);
    QVERIFY(client.ensureUploadChannel());
    QTRY_VERIFY_WITH_TIMEOUT(uploadPeerSeen, 2000);
    QTest::qWait(100);
    QVERIFY(!client.isUploadChannelConnected());
    QVERIFY(client.beginUploadSession(true));
    QVERIFY(!client.isUploadSessionUsingDedicatedChannel());
    client.endUploadSession();
    client.disconnect();
}

void ConnectionManagerTest::protocolUploadWireSchemaAndActiveGate() {
    QTemporaryDir identityDirectory;
    QVERIFY(identityDirectory.isValid());
    QWebSocketServer server(QStringLiteral("upload-v2-test"),
                            QWebSocketServer::NonSecureMode);
    QVERIFY(server.listen(QHostAddress::LocalHost, 0));

    const QString bootId = QUuid::createUuid().toString(QUuid::WithoutBraces);
    const QString targetEndpointId(43, QLatin1Char('B'));
    const QString remoteSessionId = QStringLiteral("remote_session_upload_test");
    const QString uploadId = QStringLiteral("upload_test_1");
    const QString digest(64, QLatin1Char('a'));
    QVector<QJsonObject> uploadCommands;
    QVector<QJsonObject> endpointSnapshots;
    QVector<QJsonObject> teardownAcknowledgements;
    QVector<QJsonObject> cursorCommands;
    QWebSocket* peer = nullptr;

    WebSocketClient client(identityDirectory.path(), false);
    const auto sendServerMessage = [&](QJsonObject message) {
        QVERIFY(peer != nullptr);
        message.insert(QStringLiteral("protocolVersion"), 5);
        message.insert(QStringLiteral("serverBootId"), bootId);
        message.insert(QStringLiteral("messageId"),
                       QUuid::createUuid().toString(QUuid::WithoutBraces));
        peer->sendTextMessage(QString::fromUtf8(
            QJsonDocument(message).toJson(QJsonDocument::Compact)));
    };

    connect(&server, &QWebSocketServer::newConnection, this, [&]() {
        peer = server.nextPendingConnection();
        QVERIFY(peer != nullptr);
        peer->sendTextMessage(QString::fromUtf8(QJsonDocument(QJsonObject{
            {QStringLiteral("type"), QStringLiteral("auth_challenge")},
            {QStringLiteral("protocolVersion"), 5},
            {QStringLiteral("serverBootId"), bootId},
            {QStringLiteral("messageId"),
             QUuid::createUuid().toString(QUuid::WithoutBraces)},
            {QStringLiteral("nonce"), QString::fromLatin1(QByteArray(32, 'u').toBase64(
                 QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals))},
            {QStringLiteral("issuedAt"), 1}
        }).toJson(QJsonDocument::Compact)));

        connect(peer, &QWebSocket::textMessageReceived, this,
                [&](const QString& encoded) {
            const QJsonObject message =
                QJsonDocument::fromJson(encoded.toUtf8()).object();
            const QString type = message.value(QStringLiteral("type")).toString();
            if (type == QLatin1String("auth_response")) {
                const QJsonObject policy{
                    {QStringLiteral("policyVersion"), 1},
                    {QStringLiteral("heartbeatIntervalMs"), 750},
                    {QStringLiteral("leaseTimeoutMs"), 3000},
                    {QStringLiteral("scenePrepareTimeoutMs"), 15000},
                    {QStringLiteral("sceneActivationLeadMs"), 4000},
                    {QStringLiteral("sceneMaxClockSkewMs"), 50},
                    {QStringLiteral("sceneStartedAckTimeoutMs"), 5000},
                    {QStringLiteral("sceneMaxStartSkewMs"), 750},
                    {QStringLiteral("sceneStopTimeoutMs"), 5000},
                    {QStringLiteral("uploadIdleTimeoutMs"), 45000},
                    {QStringLiteral("uploadTargetAckTimeoutMs"), 30000},
                    {QStringLiteral("removalAckTimeoutMs"), 30000}
                };
                peer->sendTextMessage(QString::fromUtf8(QJsonDocument(QJsonObject{
                    {QStringLiteral("type"), QStringLiteral("welcome")},
                    {QStringLiteral("protocolVersion"), 5},
                    {QStringLiteral("serverBootId"), bootId},
                    {QStringLiteral("messageId"),
                     QUuid::createUuid().toString(QUuid::WithoutBraces)},
                    {QStringLiteral("connectionId"),
                     QUuid::createUuid().toString(QUuid::WithoutBraces)},
                    {QStringLiteral("installationId"),
                     message.value(QStringLiteral("installationId"))},
                    {QStringLiteral("endpointId"),
                     DeviceIdentityStore::endpointIdForInstallation(
                         message.value(QStringLiteral("installationId")).toString(),
                         message.value(QStringLiteral("instanceId")).toString())},
                    {QStringLiteral("instanceId"),
                     message.value(QStringLiteral("instanceId"))},
                    {QStringLiteral("runtimeId"), message.value(QStringLiteral("runtimeId"))},
                    {QStringLiteral("connectionGeneration"), 1},
                    {QStringLiteral("policy"), policy},
                    {QStringLiteral("serverMonotonicMs"), 1}
                }).toJson(QJsonDocument::Compact)));
            } else if (type == QLatin1String("heartbeat")) {
                sendServerMessage(QJsonObject{
                    {QStringLiteral("type"), QStringLiteral("heartbeat_ack")},
                    {QStringLiteral("connectionGeneration"), 1},
                    {QStringLiteral("sequence"), message.value(QStringLiteral("sequence"))},
                    {QStringLiteral("clientMonotonicMs"),
                     message.value(QStringLiteral("clientMonotonicMs"))},
                    {QStringLiteral("serverMonotonicMs"),
                     message.value(QStringLiteral("clientMonotonicMs"))},
                    {QStringLiteral("serverEpochMs"), 1}
                });
            } else if (type == QLatin1String("endpoint_snapshot")) {
                endpointSnapshots.append(message);
            } else if (type.startsWith(QLatin1String("upload_"))) {
                uploadCommands.append(message);
            } else if (type == QLatin1String("remote_session_teardown_ack")) {
                teardownAcknowledgements.append(message);
            } else if (type == QLatin1String("remote_session_cursor")) {
                cursorCommands.append(message);
            }
        });
    });

    QSignalSpy connectedSpy(&client, &WebSocketClient::connected);
    QSignalSpy sessionSpy(&client, &WebSocketClient::remoteSessionOpened);
    QSignalSpy uploadEnvelopeSpy(&client, &WebSocketClient::uploadMessageReceived);
    QSignalSpy cursorSpy(&client, &WebSocketClient::remoteCursorReceived);
    client.connectToServer(QStringLiteral("ws://127.0.0.1:%1").arg(server.serverPort()));
    QTRY_COMPARE_WITH_TIMEOUT(connectedSpy.count(), 1, 2000);
    client.registerClient(QStringLiteral("test-device"), QStringLiteral("test-platform"),
                          {}, -1);
    QTRY_COMPARE_WITH_TIMEOUT(endpointSnapshots.size(), 1, 1000);
    QCOMPARE(endpointSnapshots.first().value(QStringLiteral("screens")).toArray().size(), 0);
    QVERIFY(endpointSnapshots.first().contains(QStringLiteral("volumePercent")));
    QVERIFY(endpointSnapshots.first().value(QStringLiteral("volumePercent")).isNull());

    sendServerMessage(QJsonObject{
        {QStringLiteral("type"), QStringLiteral("remote_session_opened")},
        {QStringLiteral("remoteSessionId"), remoteSessionId},
        {QStringLiteral("generation"), 1},
        {QStringLiteral("ownerConnectionGeneration"), 1},
        {QStringLiteral("targetConnectionGeneration"), 1},
        {QStringLiteral("phase"), QStringLiteral("Active")},
        {QStringLiteral("ownerEndpointId"), client.endpointId()},
        {QStringLiteral("targetEndpointId"), targetEndpointId},
        {QStringLiteral("resumeToken"), QStringLiteral("memory_only_resume_token")},
        {QStringLiteral("snapshotSequence"), 1},
        {QStringLiteral("snapshot"), QJsonObject{
            {QStringLiteral("screens"), QJsonArray{}},
            {QStringLiteral("systemUI"), QJsonArray{}},
            {QStringLiteral("volumePercent"), QJsonValue::Null},
            {QStringLiteral("revision"), 1},
            {QStringLiteral("capturedAtEpochMs"), 1}
        }}
    });
    QTRY_COMPARE_WITH_TIMEOUT(sessionSpy.count(), 1, 1000);
    QVERIFY(client.remoteSessionCoordinator()->forPeer(targetEndpointId).active);
    // The cursor rides the same authenticated session without an obsolete
    // watch_screens subscription. Reject stale, malformed and reverse traffic.
    QJsonObject cursor{
        {QStringLiteral("type"), QStringLiteral("remote_session_cursor")},
        {QStringLiteral("remoteSessionId"), remoteSessionId},
        {QStringLiteral("generation"), 1},
        {QStringLiteral("ownerConnectionGeneration"), 1},
        {QStringLiteral("targetConnectionGeneration"), 1},
        {QStringLiteral("connectionGeneration"), 1},
        {QStringLiteral("ownerEndpointId"), client.endpointId()},
        {QStringLiteral("targetEndpointId"), targetEndpointId},
        {QStringLiteral("sequence"), 1},
        {QStringLiteral("visible"), true},
        {QStringLiteral("screenId"), 2},
        {QStringLiteral("x"), 120},
        {QStringLiteral("y"), 240}
    };
    for (const auto& mutation : QList<QPair<QString, QJsonValue>>{
             {QStringLiteral("generation"), 1.5},
             {QStringLiteral("sequence"), 1.5},
             {QStringLiteral("sequence"), 0},
             {QStringLiteral("connectionGeneration"), 2},
             {QStringLiteral("targetConnectionGeneration"), 2},
             {QStringLiteral("ownerEndpointId"), targetEndpointId},
             {QStringLiteral("screenId"), -1},
             {QStringLiteral("x"), -1},
             {QStringLiteral("x"), 120.5},
             {QStringLiteral("visible"), QStringLiteral("true")}}) {
        QJsonObject malformed = cursor;
        malformed.insert(mutation.first, mutation.second);
        sendServerMessage(malformed);
    }
    sendServerMessage(cursor);
    QTRY_COMPARE_WITH_TIMEOUT(cursorSpy.count(), 1, 1000);
    QCOMPARE(cursorSpy.last().at(0).toString(), remoteSessionId);
    QCOMPARE(cursorSpy.last().at(1).toInt(), 2);
    QCOMPARE(cursorSpy.last().at(2).toPointF(), QPointF(120, 240));
    QVERIFY(cursorSpy.last().at(3).toBool());
    sendServerMessage(cursor); // duplicate
    cursor.insert(QStringLiteral("sequence"), 2);
    cursor.insert(QStringLiteral("visible"), false);
    cursor.insert(QStringLiteral("screenId"), -1);
    cursor.insert(QStringLiteral("x"), 0);
    cursor.insert(QStringLiteral("y"), 0);
    sendServerMessage(cursor);
    QTRY_COMPARE_WITH_TIMEOUT(cursorSpy.count(), 2, 1000);
    QVERIFY(!cursorSpy.last().at(3).toBool());
    QVERIFY(!client.sendRemoteCursor(remoteSessionId, 1, 1, true, 0, {10, 20}));
    QVERIFY(client.beginUploadSession(false));
    QVERIFY(client.beginUploadSession(false));
    client.endUploadSession();
    QVERIFY(client.isUploadSessionTransportAvailable());

    const QJsonArray manifest{QJsonObject{
        {QStringLiteral("assetId"), digest},
        {QStringLiteral("fileId"), digest},
        {QStringLiteral("sha256"), digest},
        {QStringLiteral("name"), QStringLiteral("asset.png")},
        {QStringLiteral("extension"), QStringLiteral("png")},
        {QStringLiteral("size"), 3},
        {QStringLiteral("mediaIds"), QJsonArray{QStringLiteral("media_1")}}
    }};
    QVERIFY(client.sendUploadStart(remoteSessionId, 1, uploadId, manifest));
    QVERIFY(client.sendUploadChunk(remoteSessionId, 1, uploadId, digest,
                                   0, digest, QByteArray("abc")));
    const QJsonArray completed{QJsonObject{
        {QStringLiteral("assetId"), digest},
        {QStringLiteral("offset"), 3},
        {QStringLiteral("size"), 3},
        {QStringLiteral("sha256"), digest}
    }};
    QVERIFY(client.sendUploadComplete(remoteSessionId, 1, uploadId, completed));
    QTRY_COMPARE_WITH_TIMEOUT(uploadCommands.size(), 3, 1000);

    static const QStringList forbiddenFields{
        QStringLiteral("targetClientId"), QStringLiteral("targetPersistentClientId"),
        QStringLiteral("senderClientId"), QStringLiteral("senderPersistentClientId"),
        QStringLiteral("persistentClientId"), QStringLiteral("canvasSessionId"),
        QStringLiteral("sessionId"), QStringLiteral("deviceId")
    };
    for (const QJsonObject& command : std::as_const(uploadCommands)) {
        QCOMPARE(command.value(QStringLiteral("protocolVersion")).toInt(), 5);
        QCOMPARE(command.value(QStringLiteral("remoteSessionId")).toString(),
                 remoteSessionId);
        QCOMPARE(command.value(QStringLiteral("generation")).toInt(), 1);
        QCOMPARE(command.value(QStringLiteral("uploadId")).toString(), uploadId);
        for (const QString& forbidden : forbiddenFields) {
            QVERIFY2(!command.contains(forbidden), qPrintable(forbidden));
        }
    }
    QCOMPARE(uploadCommands.at(0).value(QStringLiteral("files")).toArray(), manifest);
    const QJsonObject chunk = uploadCommands.at(1);
    QCOMPARE(chunk.value(QStringLiteral("assetId")).toString(), digest);
    QCOMPARE(chunk.value(QStringLiteral("offset")).toInt(), 0);
    QCOMPARE(chunk.value(QStringLiteral("size")).toInt(), 3);
    QCOMPARE(chunk.value(QStringLiteral("sha256")).toString(), digest);
    const QByteArray encoded = chunk.value(QStringLiteral("data")).toString().toLatin1();
    QCOMPARE(QByteArray::fromBase64(encoded), QByteArray("abc"));
    QCOMPARE(QByteArray::fromBase64(encoded).toBase64(), encoded);
    QCOMPARE(uploadCommands.at(2).value(QStringLiteral("assets")).toArray(), completed);

    QJsonObject uploadReady{
        {QStringLiteral("type"), QStringLiteral("upload_ready")},
        {QStringLiteral("remoteSessionId"), remoteSessionId},
        {QStringLiteral("generation"), 1},
        {QStringLiteral("ownerConnectionGeneration"), 1},
        {QStringLiteral("targetConnectionGeneration"), 1},
        {QStringLiteral("uploadId"), uploadId},
        {QStringLiteral("ownerEndpointId"), client.endpointId()},
        {QStringLiteral("targetEndpointId"), targetEndpointId},
        {QStringLiteral("assets"), completed}
    };
    QJsonObject fractionalUploadReady = uploadReady;
    fractionalUploadReady.insert(QStringLiteral("generation"), 1.5);
    sendServerMessage(fractionalUploadReady);
    QTest::qWait(50);
    QCOMPARE(uploadEnvelopeSpy.count(), 0);
    sendServerMessage(uploadReady);
    QTRY_COMPARE_WITH_TIMEOUT(uploadEnvelopeSpy.count(), 1, 1000);

    sendServerMessage(QJsonObject{
        {QStringLiteral("type"), QStringLiteral("remote_session_lease_state")},
        {QStringLiteral("remoteSessionId"), remoteSessionId},
        {QStringLiteral("generation"), 1},
        {QStringLiteral("ownerConnectionGeneration"), 1},
        {QStringLiteral("targetConnectionGeneration"), 1},
        {QStringLiteral("phase"), QStringLiteral("Grace")},
        {QStringLiteral("state"), QStringLiteral("Grace")},
        {QStringLiteral("ownerEndpointId"), client.endpointId()},
        {QStringLiteral("targetEndpointId"), targetEndpointId}
    });
    QTRY_VERIFY_WITH_TIMEOUT(
        !client.remoteSessionCoordinator()->forPeer(targetEndpointId).active, 1000);
    QVERIFY(!client.sendUploadStart(remoteSessionId, 1,
                                    QStringLiteral("upload_blocked_in_grace"), manifest));
    cursor.insert(QStringLiteral("sequence"), 3);
    sendServerMessage(cursor);

    // The reverse direction is a distinct binding even though the peer is the
    // same device. B may control A while A's outgoing session to B is in Grace.
    const QString incomingSessionId = QStringLiteral("remote_session_reverse_test");
    sendServerMessage(QJsonObject{
        {QStringLiteral("type"), QStringLiteral("remote_session_opened")},
        {QStringLiteral("remoteSessionId"), incomingSessionId},
        {QStringLiteral("generation"), 1},
        {QStringLiteral("ownerConnectionGeneration"), 1},
        {QStringLiteral("targetConnectionGeneration"), 1},
        {QStringLiteral("phase"), QStringLiteral("Active")},
        {QStringLiteral("ownerEndpointId"), targetEndpointId},
        {QStringLiteral("targetEndpointId"), client.endpointId()},
        {QStringLiteral("resumeToken"), QStringLiteral("reverse_memory_token")}
    });
    QTRY_COMPARE_WITH_TIMEOUT(sessionSpy.count(), 2, 1000);
    QVERIFY(!client.remoteSessionCoordinator()
                 ->outgoingForPeer(targetEndpointId).active);
    QVERIFY(client.remoteSessionCoordinator()
                ->incomingForPeer(targetEndpointId).active);
    QCOMPARE(cursorSpy.count(), 2); // inactive outgoing session was ignored
    QVERIFY(client.sendRemoteCursor(incomingSessionId, 1, 1, true, 2, {12.9, 24.1}));
    QVERIFY(client.sendRemoteCursor(incomingSessionId, 1, 2, false, 2, {12.9, 24.1}));
    QVERIFY(!client.sendRemoteCursor(incomingSessionId, 2, 3, true, 2, {12, 24}));
    QVERIFY(!client.sendRemoteCursor(incomingSessionId, 1, 0, true, 2, {12, 24}));
    QVERIFY(!client.sendRemoteCursor(incomingSessionId, 1, 3, true, 2, {-1, 24}));
    QTRY_COMPARE_WITH_TIMEOUT(cursorCommands.size(), 2, 1000);
    QCOMPARE(cursorCommands.first().value(QStringLiteral("screenId")).toInt(), 2);
    QCOMPARE(cursorCommands.first().value(QStringLiteral("x")).toInt(), 12);
    QCOMPARE(cursorCommands.first().value(QStringLiteral("y")).toInt(), 24);
    QCOMPARE(cursorCommands.first().value(QStringLiteral("remoteSessionId")).toString(), incomingSessionId);
    QCOMPARE(cursorCommands.first().value(QStringLiteral("protocolVersion")).toInt(), 5);
    QCOMPARE(cursorCommands.first().value(QStringLiteral("connectionGeneration")).toInt(), 1);
    QCOMPARE(cursorCommands.last().value(QStringLiteral("screenId")).toInt(), -1);
    QCOMPARE(cursorCommands.last().value(QStringLiteral("x")).toInt(), 0);
    QCOMPARE(cursorCommands.last().value(QStringLiteral("y")).toInt(), 0);
    QVERIFY(!client.acknowledgeRemoteSessionTeardown(
        incomingSessionId, QStringLiteral("teardown_reverse"), true, true, true, 0));

    sendServerMessage(QJsonObject{
        {QStringLiteral("type"), QStringLiteral("remote_session_terminating")},
        {QStringLiteral("remoteSessionId"), incomingSessionId},
        {QStringLiteral("generation"), 1},
        {QStringLiteral("ownerConnectionGeneration"), 1},
        {QStringLiteral("targetConnectionGeneration"), 1},
        {QStringLiteral("phase"), QStringLiteral("CleanupPending")},
        {QStringLiteral("ownerEndpointId"), targetEndpointId},
        {QStringLiteral("targetEndpointId"), client.endpointId()},
        {QStringLiteral("teardownId"), QStringLiteral("teardown_reverse")}
    });
    QTRY_COMPARE_WITH_TIMEOUT(
        client.remoteSessionCoordinator()->incomingForPeer(targetEndpointId).phase,
        QStringLiteral("CleanupPending"), 1000);
    QVERIFY(!client.sendRemoteCursor(incomingSessionId, 1, 3, true, 2, {12, 24}));
    QVERIFY(!client.acknowledgeRemoteSessionTeardown(
        incomingSessionId, QStringLiteral("teardown_wrong"), true, true, true, 0));
    QVERIFY(client.acknowledgeRemoteSessionTeardown(
        incomingSessionId, QStringLiteral("teardown_reverse"), true, true, true, 0));
    QTRY_COMPARE_WITH_TIMEOUT(teardownAcknowledgements.size(), 1, 1000);
    QCOMPARE(teardownAcknowledgements.first()
                 .value(QStringLiteral("teardownId")).toString(),
             QStringLiteral("teardown_reverse"));
    QCOMPARE(teardownAcknowledgements.first()
                 .value(QStringLiteral("generation")).toInt(), 1);
    client.endUploadSession();
    client.disconnect();
}

QTEST_GUILESS_MAIN(ConnectionManagerTest)
#include "tst_ConnectionManager.moc"
