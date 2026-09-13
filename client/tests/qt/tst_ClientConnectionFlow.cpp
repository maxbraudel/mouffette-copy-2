#include <QtTest>

#include <QGraphicsOpacityEffect>
#include <QListWidget>
#include <QPropertyAnimation>
#include <QPushButton>
#include <QStackedWidget>

#include "backend/domain/models/ClientInfo.h"
#include "frontend/rendering/navigation/ScreenNavigationManager.h"
#include "frontend/ui/pages/ClientListPage.h"
#include "frontend/ui/widgets/ClientListDelegate.h"
#include "frontend/ui/widgets/SpinnerWidget.h"

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

QListWidget* clientListWidget(ClientListPage& page)
{
    for (QListWidget* list : page.findChildren<QListWidget*>()) {
        for (int row = 0; row < list->count(); ++row) {
            QListWidgetItem* item = list->item(row);
            if (item && item->data(ClientListRoles::IsClientRow).toBool()
                && !item->data(ClientListRoles::ClientId).toString().isEmpty()) {
                return list;
            }
        }
    }
    return nullptr;
}
}

class ClientConnectionFlowTest final : public QObject {
    Q_OBJECT

private slots:
    void clickedClientSurvivesSynchronousListRebuild()
    {
        ClientListPage page(nullptr);
        const ClientInfo selected = onlineClient(
            QStringLiteral("endpoint-a"), QStringLiteral("Windows B"));
        page.updateClientList({selected});

        QString observedEndpoint;
        QString observedName;
        connect(&page, &ClientListPage::clientClicked, &page,
                [&](const ClientInfo& clicked, int) {
            // Project creation refreshes this list synchronously in the real
            // flow. The signal argument must remain an owned click snapshot.
            page.updateClientList({onlineClient(
                QStringLiteral("endpoint-c"), QStringLiteral("Replacement"))});
            observedEndpoint = clicked.endpointId();
            observedName = clicked.getMachineName();
        });

        QListWidget* list = clientListWidget(page);
        QVERIFY(list);
        QListWidgetItem* item = list->item(0);
        QVERIFY(item);
        emit list->itemClicked(item);

        QCOMPARE(observedEndpoint, QStringLiteral("endpoint-a"));
        QCOMPARE(observedName, QStringLiteral("Windows B"));
    }

    void initialConnectionKeepsBlockingLoaderUntilReady()
    {
        QStackedWidget applicationStack;
        auto* listPage = new QWidget;
        auto* screenPage = new QWidget;
        applicationStack.addWidget(listPage);
        applicationStack.addWidget(screenPage);

        QStackedWidget canvasStack;
        canvasStack.addWidget(new QWidget);
        canvasStack.addWidget(new QWidget);
        SpinnerWidget loadingSpinner;
        SpinnerWidget inlineSpinner;
        QGraphicsOpacityEffect spinnerOpacity;
        QGraphicsOpacityEffect canvasOpacity;
        QGraphicsOpacityEffect volumeOpacity;
        QPropertyAnimation spinnerFade(&spinnerOpacity, "opacity");
        QPropertyAnimation canvasFade(&canvasOpacity, "opacity");
        QPropertyAnimation volumeFade(&volumeOpacity, "opacity");
        QPushButton backButton;
        bool contentEverLoaded = false;

        ScreenNavigationManager navigation;
        ScreenNavigationManager::Widgets widgets;
        widgets.stack = &applicationStack;
        widgets.clientListPage = listPage;
        widgets.screenViewPage = screenPage;
        widgets.backButton = &backButton;
        widgets.canvasStack = &canvasStack;
        widgets.loadingSpinner = &loadingSpinner;
        widgets.spinnerOpacity = &spinnerOpacity;
        widgets.spinnerFade = &spinnerFade;
        widgets.canvasOpacity = &canvasOpacity;
        widgets.canvasFade = &canvasFade;
        widgets.volumeOpacity = &volumeOpacity;
        widgets.volumeFade = &volumeFade;
        widgets.inlineSpinner = &inlineSpinner;
        widgets.canvasContentEverLoaded = &contentEverLoaded;
        navigation.setWidgets(widgets);
        navigation.setDurations(1000, 0, 0);

        const ClientInfo client = onlineClient(
            QStringLiteral("endpoint-b"), QStringLiteral("Windows B"));
        navigation.showScreenView(client, false);
        QCOMPARE(applicationStack.currentWidget(), screenPage);
        QCOMPARE(canvasStack.currentIndex(), 0);
        QVERIFY(loadingSpinner.isSpinning());
        QCOMPARE(spinnerOpacity.opacity(), qreal(1.0));

        // A discovery refresh cannot masquerade as session readiness.
        navigation.refreshActiveClientPreservingCanvas(client);
        QCOMPARE(canvasStack.currentIndex(), 0);
        QVERIFY(loadingSpinner.isSpinning());

        navigation.revealCanvas();
        QCOMPARE(canvasStack.currentIndex(), 1);
        QVERIFY(!loadingSpinner.isSpinning());
        QTRY_COMPARE(canvasOpacity.opacity(), qreal(1.0));
    }
};

QTEST_MAIN(ClientConnectionFlowTest)
#include "tst_ClientConnectionFlow.moc"
