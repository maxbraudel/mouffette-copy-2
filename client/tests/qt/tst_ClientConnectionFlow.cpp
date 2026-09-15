#include <QtTest>
#include <QTemporaryDir>

#include "backend/runtime/ApplicationRuntime.h"
#include "backend/runtime/RuntimeProfile.h"
#include "backend/domain/canvas/CanvasDocument.h"
#include "backend/domain/models/ClientInfo.h"
#include "backend/domain/project/ProjectManager.h"
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
};

QTEST_MAIN(ClientConnectionFlowTest)
#include "tst_ClientConnectionFlow.moc"
