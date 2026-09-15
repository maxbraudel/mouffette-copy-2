#include <QtTest>
#include <QDateTime>
#include <QDir>
#include <QEvent>
#include <QFile>
#include <QHostAddress>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPointer>
#include <QTemporaryDir>
#include <QTimer>
#include <QUuid>
#include <QWebSocket>
#include <QWebSocketServer>

#include "backend/runtime/ApplicationRuntime.h"
#include "backend/runtime/ApplicationActivityMonitor.h"
#include "backend/runtime/RuntimeProfile.h"
#include "backend/runtime/RuntimeStorageBootstrap.h"
#include "backend/domain/canvas/CanvasDocument.h"
#include "backend/domain/models/ClientInfo.h"
#include "backend/domain/project/ProjectManager.h"
#include "backend/managers/network/ConnectionManager.h"
#include "backend/network/RemoteSessionCoordinator.h"
#include "backend/network/WebSocketClient.h"
#include "backend/security/DeviceIdentityStore.h"
#include "frontend/qml/ApplicationController.h"
#include "frontend/qml/ClientListModel.h"
#include "frontend/qml/ClientWorkspaceViewModel.h"
#include "frontend/rendering/navigation/ScreenNavigationManager.h"
#include "frontend/rendering/remote/RemoteSceneController.h"
#include "shared/rendering/ICanvasHost.h"

namespace {
ClientInfo onlineClient(const QString& endpointId, const QString& machineName)
{
    ClientInfo client(endpointId, machineName, QStringLiteral("Windows"));
    client.setEndpointId(endpointId);
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
        return sendOn(peer, bootId, std::move(message));
    }

    bool sendOn(QWebSocket* socket, const QString& socketBootId,
                QJsonObject message)
    {
        if (!socket) return false;
        message.insert(QStringLiteral("protocolVersion"), 4);
        message.insert(QStringLiteral("serverBootId"), socketBootId);
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
    QString ownerEndpointId;
    QString bootId;
    quint64 connectionGeneration = 1;
    int heartbeatIntervalMs = 1'000;
    int leaseTimeoutMs = 10'000;
    bool acknowledgeHeartbeats = true;
    int acceptedConnections = 0;
    QList<QJsonObject> openCommands;
    QList<QJsonObject> closeCommands;
    QList<QJsonObject> resumeCommands;
    QList<QJsonObject> teardownAcknowledgements;

private:
    void acceptConnection()
    {
        QWebSocket* socket = server.nextPendingConnection();
        if (!socket) return;
        peer = socket;
        peer->setParent(&server);
        ++acceptedConnections;
        const QString socketBootId = bootId;
        const quint64 socketGeneration = connectionGeneration;

        connect(peer, &QWebSocket::textMessageReceived,
                this, [this, socket, socketBootId,
                       socketGeneration](const QString& encoded) {
            const QJsonObject message =
                QJsonDocument::fromJson(encoded.toUtf8()).object();
            const QString type = message.value(QStringLiteral("type")).toString();
            if (type == QLatin1String("auth_response")) {
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
            } else if (type == QLatin1String("remote_session_open")) {
                openCommands.append(message);
            } else if (type == QLatin1String("remote_session_close")) {
                closeCommands.append(message);
            } else if (type == QLatin1String("remote_session_resume")) {
                resumeCommands.append(message);
            } else if (type
                       == QLatin1String("remote_session_teardown_ack")) {
                teardownAcknowledgements.append(message);
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
            {QStringLiteral("policyVersion"), 1},
            {QStringLiteral("heartbeatIntervalMs"), heartbeatIntervalMs},
            {QStringLiteral("leaseTimeoutMs"), leaseTimeoutMs},
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

    void projectDeadlinesRefreshAsLiveCountdowns()
    {
        ClientListModel model;
        ClientInfo client = onlineClient(
            QStringLiteral("endpoint-countdown"),
            QStringLiteral("Countdown client"));
        const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
        client.setHasProject(true);
        client.setRemoteSessionCloseAtMs(nowMs + 4'000);
        client.setProjectDeleteAtMs(nowMs + 8'000);

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

        QTRY_VERIFY_WITH_TIMEOUT(changes.count() >= 1, 1'500);
        const QString refreshed = model.data(
            model.index(0), ClientListModel::SecondaryTextRole).toString();
        QVERIFY2(refreshed != initial,
                 qPrintable(QStringLiteral("Countdown stayed frozen at '%1'")
                                .arg(initial)));

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
        const ClientInfo client = onlineClient(
            QStringLiteral("endpoint-real-canvas"), QStringLiteral("Windows B"));
        runtime.buildDisplayClientList({client});

        QSignalSpy activeWorkspaceChanged(
            &runtime, &ApplicationRuntime::activeWorkspaceChanged);
        runtime.activateClient(client.endpointId());

        QCOMPARE(runtime.activeWorkspaceEndpointId(), client.endpointId());
        QVERIFY(!runtime.getActiveCanvas());
        QVERIFY(!runtime.activeProjectExists());
        QVERIFY(!runtime.activeRemoteSessionExists());
        QVERIFY(runtime.findWorkspace(client.endpointId()));
        QVERIFY(!runtime.findWorkspace(client.endpointId())->canvas);
        QVERIFY(activeWorkspaceChanged.count() >= 1);

        QCOMPARE(runtime.getProjectManager()->projectCount(), 0);
        QVERIFY(runtime.getNavigationManager()->isLoading());

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
        ClientInfo client = onlineClient(
            QStringLiteral("endpoint-first-snapshot"),
            QStringLiteral("Windows B"));
        client.setScreens({});
        client.setVolumePercent(-1);
        runtime.buildDisplayClientList({client});
        runtime.activateClient(client.endpointId());

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
        const QJsonObject envelope{
            {QStringLiteral("type"), QStringLiteral("remote_session_opened")},
            {QStringLiteral("requestId"), QStringLiteral("open-request-1")},
            {QStringLiteral("remoteSessionId"), QStringLiteral("session-1")},
            {QStringLiteral("generation"), 1},
            {QStringLiteral("phase"), QStringLiteral("Active")},
            {QStringLiteral("ownerEndpointId"),
             runtime.getWebSocketClient()->endpointId()},
            {QStringLiteral("targetEndpointId"), client.endpointId()},
            {QStringLiteral("snapshot"), QJsonObject{
                {QStringLiteral("screens"), QJsonArray{remoteScreen.toJson()}},
                {QStringLiteral("systemUI"), QJsonArray{}},
                {QStringLiteral("volumePercent"), 47},
                {QStringLiteral("revision"), 1},
                {QStringLiteral("capturedAtEpochMs"),
                 QDateTime::currentMSecsSinceEpoch()}
            }}
        };

        QVERIFY(QMetaObject::invokeMethod(
            runtime.getWebSocketClient(), "remoteSessionOpened",
            Qt::DirectConnection, Q_ARG(QJsonObject, envelope)));

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

        const QString targetEndpointId(43, QLatin1Char('V'));
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
        QCOMPARE(runtime.remoteStatusText(), QStringLiteral("CONNECTING..."));

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
        const QString targetEndpointId(43, QLatin1Char('B'));
        const QString firstSessionId = QStringLiteral("inactivity-session-1");
        const QString secondSessionId = QStringLiteral("inactivity-session-2");
        const QString teardownId = QStringLiteral("inactivity-teardown-1");
        QCOMPARE(ownerEndpointId.size(), 43);

        QPointer<QWebSocket> peer;
        QList<QJsonObject> openCommands;
        QList<QJsonObject> closeCommands;
        auto sendServerMessage = [&](QJsonObject message) {
            QVERIFY2(peer, "The fake server has no authenticated peer");
            message.insert(QStringLiteral("protocolVersion"), 4);
            message.insert(QStringLiteral("serverBootId"), bootId);
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
                    const QJsonObject policy{
                        {QStringLiteral("policyVersion"), 1},
                        {QStringLiteral("heartbeatIntervalMs"), 1'000},
                        {QStringLiteral("leaseTimeoutMs"), 10'000},
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
        QCOMPARE(runtime.remoteStatusText(), QStringLiteral("AVAILABLE"));
        QVERIFY(runtime.getWorkspaceManager()->remoteSessionState(targetEndpointId)
                != WorkspaceManager::RemoteSessionState::Active);
        const QList<ClientInfo> afterLateActive = runtime.displayClients();
        QCOMPARE(afterLateActive.size(), 1);
        QCOMPARE(afterLateActive.constFirst().endpointId(), targetEndpointId);
        QCOMPARE(afterLateActive.constFirst().availabilityBadgeText(),
                 QStringLiteral("Available"));

        runtime.getProjectManager()->processDeadlines(deleteAt - 1);
        QVERIFY(runtime.getProjectManager()->hasProjectForTarget(targetEndpointId));
        runtime.getProjectManager()->processDeadlines(deleteAt);
        QTRY_VERIFY_WITH_TIMEOUT(
            !runtime.getProjectManager()->hasProjectForTarget(targetEndpointId),
            1'000);
        QVERIFY(!runtime.getNavigationManager()->isOnScreenView());
        QVERIFY(!runtime.findWorkspace(targetEndpointId));
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
        WorkspaceManager* workspaces = runtime.getWorkspaceManager();
        ProjectManager* projects = runtime.getProjectManager();
        workspaces->stopAutomaticTimersForTesting();
        projects->stopAutomaticTimersForTesting();
        workspaces->setRemoteSessionHiddenTimeoutMs(60'000);

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
            QString(43, QLatin1Char('A')), QString(43, QLatin1Char('B'))};
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
        QCOMPARE(projectDeletedSpy.count(), 0);

        // A single departure starts both deadlines for every project,
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
        }

        // Returning before expiry cancels both deadlines everywhere; even
        // advancing past their former expiry cannot close or delete anything.
        runtime.setPointerInsideControlWindow(true);
        QVERIFY(activity->isActive());
        for (const QString& target : targets) {
            QCOMPARE(workspaces->remoteSessionCloseAtMs(target), qint64(-1));
            QCOMPARE(projects->projectDeleteAtMs(target), qint64(-1));
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

        const QString targetEndpointId(43, QLatin1Char('I'));
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
        QCOMPARE(runtime.remoteStatusText(), QStringLiteral("AVAILABLE"));

        // Pointer activity is the automatic reopen intent. Since CLOSE1 is
        // not committed yet it must not emit OPEN2 prematurely.
        ++nowMs;
        runtime.setPointerInsideControlWindow(true);
        QCOMPARE(server.openCommands.size(), 1);
        QCOMPARE(runtime.remoteStatusText(), QStringLiteral("CONNECTING..."));
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

        const QString targetEndpointId(43, QLatin1Char('D'));
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

        const QString targetEndpointId(43, QLatin1Char('K'));
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
                                  QStringLiteral("CONNECTING..."), 1'000);
        QCOMPARE(server.openCommands.size(), 3);
        QVERIFY(runtime.getNavigationManager()->isOnScreenView());
        QVERIFY(runtime.getNavigationManager()->isLoading());

        QVERIFY(server.sendClientList(client));
        QTRY_COMPARE_WITH_TIMEOUT(server.openCommands.size(), 4, 1'000);
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

        const QString targetEndpointId(43, QLatin1Char('R'));
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
        QCOMPARE(runtime.remoteStatusText(), QStringLiteral("RECONNECTING..."));
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
        QCOMPARE(runtime.remoteStatusText(), QStringLiteral("RECONNECTING..."));
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

        const QString targetEndpointId(43, QLatin1Char('D'));
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

        const QString targetEndpointId(43, QLatin1Char('E'));
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
        QCOMPARE(runtime.remoteStatusText(), QStringLiteral("CONNECTING..."));

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
        QCOMPARE(runtime.remoteStatusText(), QStringLiteral("CONNECTING..."));

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

        const QString targetEndpointId(43, QLatin1Char('F'));
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
        // Use the shortest valid lease so the test exercises the production
        // lease-expiry path which clears the coordinator before reconnect.
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
                                  &WebSocketClient::leaseExpired);
        connections->connectToServer(server.url());
        QTRY_COMPARE_WITH_TIMEOUT(connectedSpy.count(), 1, 2'000);

        const QString targetEndpointId(43, QLatin1Char('G'));
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
        QCOMPARE(runtime.remoteStatusText(), QStringLiteral("CONNECTING..."));

        // Stop authenticated contact. The lease-expiry signal is followed by
        // SceneRunCoordinator::clearSessions(), then ConnectionManager creates
        // generation 2 against the same server boot.
        server.connectionGeneration = 2;
        server.acknowledgeHeartbeats = false;
        QTRY_COMPARE_WITH_TIMEOUT(leaseExpiredSpy.count(), 1, 3'000);
        QTRY_VERIFY_WITH_TIMEOUT(
            runtime.getWebSocketClient()->remoteSessionCoordinator()
                ->outgoingForPeer(targetEndpointId).remoteSessionId.isEmpty(),
            1'000);
        server.acknowledgeHeartbeats = true;
        QTRY_COMPARE_WITH_TIMEOUT(disconnectedSpy.count(), 1, 2'000);
        QTRY_COMPARE_WITH_TIMEOUT(connectedSpy.count(), 2, 3'000);
        QTRY_COMPARE_WITH_TIMEOUT(server.closeCommands.size(), 2, 1'000);

        const QJsonObject closeOnGeneration2 = server.closeCommands.constLast();
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
        QCOMPARE(server.closeCommands.size(), 2);
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

    void cleanupPendingOpenRetainsIntentUntilAuthenticatedDiscoveryRetry()
    {
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

        const QString targetEndpointId(43, QLatin1Char('I'));
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
             QStringLiteral("session_cleanup_pending")},
            {QStringLiteral("message"),
             QStringLiteral("The previous session is still being cleaned up")},
            {QStringLiteral("requestId"),
             firstOpen.value(QStringLiteral("requestId"))}
        }));
        QTRY_COMPARE_WITH_TIMEOUT(runtime.remoteStatusText(),
                                  QStringLiteral("CONNECTING..."), 1'000);
        QVERIFY(runtime.getNavigationManager()->isOnScreenView());
        QVERIFY(runtime.getNavigationManager()->isLoading());
        QVERIFY(!runtime.activeProjectExists());
        QCOMPARE(server.openCommands.size(), 1);
        QTRY_COMPARE_WITH_TIMEOUT(
            runtime.displayClients().constFirst().availabilityBadgeText(),
            QStringLiteral("Connecting"), 1'000);

        // Cleanup completion causes an authenticated client-list broadcast in
        // production. That boundary consumes the retained intent once.
        QVERIFY(server.sendClientList(client));
        QTRY_COMPARE_WITH_TIMEOUT(server.openCommands.size(), 2, 1'000);
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

        const QString targetB(43, QLatin1Char('H'));
        const QString targetC(43, QLatin1Char('I'));
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
        QCOMPARE(runtime.remoteStatusText(), QStringLiteral("CONNECTING..."));

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

        const QString peerEndpointId(43, QLatin1Char('J'));
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
        QCOMPARE(runtime.remoteStatusText(), QStringLiteral("CONNECTING..."));

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
        QCOMPARE(runtime.remoteStatusText(), QStringLiteral("CONNECTING..."));

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

        const QString ownerX(43, QLatin1Char('K'));
        const QString ownerY(43, QLatin1Char('L'));
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

        // Seed a valid temporary profile so ApplicationController's normal
        // bootstrap does not pause on the first-run recovery acknowledgement.
        RuntimeStorageBootstrap initialBootstrap(context);
        const RuntimeStorageBootstrap::Result bootstrapResult =
            initialBootstrap.run();
        QVERIFY(bootstrapResult.succeeded());

        QWebSocketServer server(QStringLiteral("controller-workspace-eviction-test"),
                                QWebSocketServer::NonSecureMode);
        QVERIFY(server.listen(QHostAddress::LocalHost, 0));

        const QString bootId = QUuid::createUuid().toString(QUuid::WithoutBraces);
        const QString targetEndpointId(43, QLatin1Char('C'));
        const QString firstSessionId = QStringLiteral("controller-session-1");
        const QString secondSessionId = QStringLiteral("controller-session-2");
        QPointer<QWebSocket> peer;
        QString ownerEndpointId;
        QList<QJsonObject> endpointSnapshots;
        QList<QJsonObject> openCommands;
        QList<QJsonObject> closeCommands;

        auto sendServerMessage = [&](QJsonObject message) {
            QVERIFY2(peer, "The fake server has no authenticated peer");
            message.insert(QStringLiteral("protocolVersion"), 4);
            message.insert(QStringLiteral("serverBootId"), bootId);
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
                    ownerEndpointId =
                        DeviceIdentityStore::endpointIdForInstallation(
                            message.value(QStringLiteral("installationId"))
                                .toString(),
                            message.value(QStringLiteral("instanceId")).toString());
                    const QJsonObject policy{
                        {QStringLiteral("policyVersion"), 1},
                        {QStringLiteral("heartbeatIntervalMs"), 1'000},
                        {QStringLiteral("leaseTimeoutMs"), 10'000},
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
        controller.start();
        QTRY_VERIFY_WITH_TIMEOUT(controller.ready(), 2'000);
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

QTEST_MAIN(ClientConnectionFlowTest)
#include "tst_ClientConnectionFlow.moc"
