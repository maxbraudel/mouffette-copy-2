#include <QtTest>
#include <QDateTime>
#include <QJsonArray>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QTimer>

#include "backend/runtime/ApplicationRuntime.h"
#include "backend/runtime/RuntimeProfile.h"
#include "backend/domain/canvas/CanvasDocument.h"
#include "backend/domain/models/ClientInfo.h"
#include "backend/domain/project/ProjectManager.h"
#include "backend/network/WebSocketClient.h"
#include "frontend/qml/ClientListModel.h"
#include "frontend/rendering/navigation/ScreenNavigationManager.h"
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
};

QTEST_MAIN(ClientConnectionFlowTest)
#include "tst_ClientConnectionFlow.moc"
