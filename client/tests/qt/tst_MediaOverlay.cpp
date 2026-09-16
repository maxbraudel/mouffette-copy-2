#include <QColor>
#include <QDir>
#include <QFileInfo>
#include <QImage>
#include <QMouseEvent>
#include <QMediaPlayer>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QQuickItem>
#include <QQuickWindow>
#include <QScopeGuard>
#include <QTemporaryDir>
#include <QtTest>
#include <QtGui/private/qpointingdevice_p.h>
#include <QtQuick/private/qquickhoverhandler_p.h>
#include <QtQuick/private/qquickwindow_p.h>

#include "frontend/rendering/canvas/MediaListModel.h"
#include "frontend/rendering/canvas/CanvasQmlTypes.h"
#include "frontend/rendering/canvas/QuickCanvasController.h"
#include "frontend/rendering/canvas/QuickCanvasHost.h"
#include "frontend/qml/ClientWorkspaceViewModel.h"
#include "frontend/qml/ApplicationController.h"
#include "frontend/qml/MediaSettingsViewModel.h"
#include "backend/domain/canvas/CanvasDocument.h"
#include "backend/domain/media/CanvasMedia.h"
#include "backend/media/MediaResidencyManager.h"
#include "backend/files/FileManager.h"
#include "backend/network/UploadManager.h"
#ifdef Q_OS_MACOS
#include "backend/platform/macos/MacWindowManager.h"
#endif

class MediaOverlayTest final : public QObject
{
    Q_OBJECT

private slots:
    void init()
    {
        MediaResidencyManager::instance().setMemorySnapshotForTesting(
            {8ULL << 30, 6ULL << 30, 512ULL << 20, false, 0});
    }
    void cleanup()
    {
        MediaResidencyManager::instance().clearMemorySnapshotForTesting();
    }
    void initTestCase();
    void segmentedStatusFillReachesBothEdges_data();
    void segmentedStatusFillReachesBothEdges();
    void availableStatusUsesNeutralGreyPalette();
    void segmentedStatusKeepsSingleTopBorder();
    void mediaPanelVisibilityAnchorInteractionAndScroll();
    void mediaCountTracksRealCanvasInsertions();
    void typedCapabilitiesGuardDirectCppInvocations();
    void overlayButtonHoverIsImmediate();
    void activationDuringBootstrapKeepsMainWindowHidden();
    void mainWindowPointerActivity_data();
    void mainWindowPointerActivity();
    void mediaActionPalette_data();
    void mediaActionPalette();
    void mediaRowsAndProgress();
    void uploadActionLocksBeforeDispatchAndRecovers();
    void toolbarToolsAndGlobalMemoryUsage();
    void mediaSettingsPanelRestoresTabsAndBindings();
    void videoVolumeAndMuteStayIndependentAndSyncWithSettings();
    void toastUsesBottomLeftDoubleBackground();
    void themeTracksApplicationPalette();
};

namespace {
QQuickItem* createStatusCard(QQmlEngine& engine, QQuickWindow& window,
                             bool auxiliaryVisible = false)
{
    QQmlComponent component(
        &engine,
        QUrl(QStringLiteral(
            "qrc:/qt/qml/Mouffette/App/resources/qml/app/components/SegmentedStatusCard.qml")));
    if (component.isError()) {
        qWarning().noquote() << component.errorString();
        return nullptr;
    }
    QObject* object = component.create();
    auto* item = qobject_cast<QQuickItem*>(object);
    if (!item) {
        delete object;
        return nullptr;
    }
    item->setParentItem(window.contentItem());
    item->setPosition(QPointF(20, 20));
    item->setProperty("primaryText", QStringLiteral("You"));
    item->setProperty("statusText", QStringLiteral("CONNECTED"));
    item->setProperty("statusKind", 0);
    item->setProperty("auxiliaryText", QStringLiteral("75%"));
    item->setProperty("auxiliaryVisible", auxiliaryVisible);
    item->setSize(QSizeF(item->implicitWidth(), item->implicitHeight()));
    return item;
}

QColor imagePixel(const QImage& image, const QSize& logicalSize,
                  const QPointF& logicalPoint)
{
    const qreal scaleX = image.width() / qreal(logicalSize.width());
    const qreal scaleY = image.height() / qreal(logicalSize.height());
    return image.pixelColor(
        qBound(0, qRound(logicalPoint.x() * scaleX), image.width() - 1),
        qBound(0, qRound(logicalPoint.y() * scaleY), image.height() - 1));
}

bool nearColor(const QColor& actual, const QColor& expected, int tolerance = 8)
{
    return qAbs(actual.red() - expected.red()) <= tolerance
        && qAbs(actual.green() - expected.green()) <= tolerance
        && qAbs(actual.blue() - expected.blue()) <= tolerance;
}

QColor overWhite(const QColor& color)
{
    const qreal alpha = color.alphaF();
    return QColor::fromRgbF(color.redF() * alpha + (1.0 - alpha),
                            color.greenF() * alpha + (1.0 - alpha),
                            color.blueF() * alpha + (1.0 - alpha));
}

QQuickItem* findVisualItem(QQuickItem* root, const QString& objectName)
{
    if (!root) return nullptr;
    QList<QQuickItem*> pending{root};
    while (!pending.isEmpty()) {
        QQuickItem* item = pending.takeLast();
        if (item->objectName() == objectName) return item;
        pending.append(item->childItems());
    }
    return nullptr;
}

QQuickItem* createMediaPanelHarness(QQmlEngine& engine, QQuickWindow& window,
                                    MediaListModel* model, QString* error)
{
    static const QByteArray qml = R"QML(
import QtQuick
import "../canvas"

Item {
    id: host
    required property var externalModel
    property int testMediaCount: 0
    property int selectionCalls: 0
    property alias session: fakeSession
    property int canvasPresses: 0
    MouseArea {
        anchors.fill: parent
        onPressed: host.canvasPresses += 1
    }

    QtObject {
        id: fakeSession
        property bool hasProject: true
        property bool actionPending: false
        property int remoteSceneActionTone: 0
        property int testSceneActionTone: 0
        property var mediaModel: host.externalModel
        property int mediaCount: host.testMediaCount
        property string remoteSceneActionText: "Launch Remote Scene"
        property bool remoteSceneActionEnabled: true
        property string remoteSceneUnavailableReason: ""
        property string testSceneActionText: "Launch Test Scene"
        property bool testSceneActionEnabled: true
        property string testSceneUnavailableReason: ""
        property string uploadActionText: "Upload"
        property int uploadActionTone: 0
        property bool uploadActionEnabled: true
        property string uploadUnavailableReason: ""
        function selectMedia(mediaId, additive) { host.selectionCalls += 1 }
        function toggleRemoteScene() {}
        function toggleTestScene() {}
        function triggerUploadAction() {}
    }

    MediaListPanel {
        objectName: "mediaListPanel"
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        anchors.margins: 16
        session: fakeSession
    }
}
)QML";

    QQmlComponent component(&engine);
    component.setData(qml, QUrl(QStringLiteral(
        "qrc:/qt/qml/Mouffette/App/resources/qml/app/pages/MediaPanelHarness.qml")));
    QObject* object = component.createWithInitialProperties({
        {QStringLiteral("externalModel"), QVariant::fromValue<QObject*>(model)}});
    if (!object) {
        if (error) *error = component.errorString();
        return nullptr;
    }
    auto* root = qobject_cast<QQuickItem*>(object);
    if (!root) {
        if (error) *error = QStringLiteral("media panel harness root is not an item");
        delete object;
        return nullptr;
    }
    root->setSize(window.size());
    root->setParentItem(window.contentItem());
    return root;
}

QQuickItem* createToastHarness(QQmlEngine& engine, QQuickWindow& window,
                               QString* error)
{
    static const QByteArray qml = R"QML(
import QtQuick
import "../components"

Item {
    ListModel {
        id: toastRows
        ListElement {
            severityKind: 0
            message: "Connected"
            dismissing: false
        }
    }
    QtObject {
        id: fakeController
        property var toastModel: toastRows
    }
    ToastStack {
        objectName: "toastStack"
        controller: fakeController
    }
}
)QML";
    QQmlComponent component(&engine);
    component.setData(qml, QUrl(QStringLiteral(
        "qrc:/qt/qml/Mouffette/App/resources/qml/app/pages/ToastHarness.qml")));
    QObject* object = component.create();
    if (!object) {
        if (error) *error = component.errorString();
        return nullptr;
    }
    auto* root = qobject_cast<QQuickItem*>(object);
    if (!root) {
        if (error) *error = QStringLiteral("toast harness root is not an item");
        delete object;
        return nullptr;
    }
    root->setSize(window.size());
    root->setParentItem(window.contentItem());
    return root;
}

QQuickItem* createRealMediaPanelHarness(QQmlEngine& engine,
                                        QQuickWindow& window,
                                        QObject* session, QString* error)
{
    static const QByteArray qml = R"QML(
import QtQuick
import "../canvas"

Item {
    required property var externalSession
    MediaListPanel {
        objectName: "realMediaListPanel"
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        anchors.margins: 16
        session: parent.externalSession
    }
}
)QML";
    QQmlComponent component(&engine);
    component.setData(qml, QUrl(QStringLiteral(
        "qrc:/qt/qml/Mouffette/App/resources/qml/app/pages/RealMediaPanelHarness.qml")));
    QObject* object = component.createWithInitialProperties({
        {QStringLiteral("externalSession"), QVariant::fromValue<QObject*>(session)}});
    if (!object) {
        if (error) *error = component.errorString();
        return nullptr;
    }
    auto* root = qobject_cast<QQuickItem*>(object);
    if (!root) {
        if (error) *error = QStringLiteral("real media panel harness root is not an item");
        delete object;
        return nullptr;
    }
    root->setSize(window.size());
    root->setParentItem(window.contentItem());
    return root;
}

QQuickItem* createOverlayButton(QQmlEngine& engine, QQuickWindow& window,
                                QString* error)
{
    QQmlComponent component(
        &engine,
        QUrl(QStringLiteral(
            "qrc:/qt/qml/Mouffette/App/resources/qml/OverlayButton.qml")));
    if (component.isError()) {
        if (error) *error = component.errorString();
        return nullptr;
    }
    QObject* object = component.create();
    auto* item = qobject_cast<QQuickItem*>(object);
    if (!item) {
        if (error) *error = component.errorString();
        delete object;
        return nullptr;
    }
    item->setPosition(QPointF(20, 20));
    item->setSize(QSizeF(36, 36));
    item->setParentItem(window.contentItem());
    return item;
}

QQuickItem* createCanvasToolbarHarness(QQmlEngine& engine,
                                       QQuickWindow& window, QString* error)
{
    static const QByteArray qml = R"QML(
import QtQuick
import "../canvas"

Item {
    property alias fakeSessionHasProject: fakeSession.hasProject

    QtObject {
        id: fakeSession
        property bool settingsVisible: false
        property bool hasProject: true
        property bool mediaEditingEnabled: true
        property bool canvasNavigation: true
        property bool textCreation: true
        property string canvasNavigationUnavailableReason: ""
        property string mediaEditingUnavailableReason: ""
        property string textCreationUnavailableReason: ""
        property string activeTool: "selection"
        function setActiveTool(tool) { activeTool = tool }
    }
    CanvasToolbar {
        objectName: "canvasToolbar"
        session: fakeSession
    }
}
)QML";
    QQmlComponent component(&engine);
    component.setData(qml, QUrl(QStringLiteral(
        "qrc:/qt/qml/Mouffette/App/resources/qml/app/pages/CanvasToolbarHarness.qml")));
    QObject* object = component.create();
    if (!object) {
        if (error) *error = component.errorString();
        return nullptr;
    }
    auto* root = qobject_cast<QQuickItem*>(object);
    if (!root) {
        if (error) *error = QStringLiteral("canvas toolbar harness root is not an item");
        delete object;
        return nullptr;
    }
    root->setSize(window.size());
    root->setParentItem(window.contentItem());
    return root;
}

QQuickItem* createMediaSettingsHarness(QQmlEngine& engine,
                                       QQuickWindow& window,
                                       QObject* session, QString* error)
{
    static const QByteArray qml = R"QML(
import QtQuick
import "../canvas"

Item {
    id: host
    required property var externalSession
    property int canvasPrimaryPressCount: 0

    Item {
        anchors.fill: parent

        PointHandler {
            target: null
            acceptedDevices: PointerDevice.Mouse | PointerDevice.TouchPad
            acceptedButtons: Qt.LeftButton
            onActiveChanged: {
                if (active)
                    host.canvasPrimaryPressCount += 1
            }
        }
    }

    SceneElementPanel {
        objectName: "realMediaSettingsPanel"
        x: 10
        y: 52
        maximumHeight: Math.max(0, host.height - y - 10)
        session: host.externalSession
    }
}
)QML";
    QQmlComponent component(&engine);
    component.setData(qml, QUrl(QStringLiteral(
        "qrc:/qt/qml/Mouffette/App/resources/qml/app/pages/MediaSettingsHarness.qml")));
    QObject* object = component.createWithInitialProperties({
        {QStringLiteral("externalSession"), QVariant::fromValue(session)}});
    if (!object) {
        if (error) *error = component.errorString();
        return nullptr;
    }
    auto* root = qobject_cast<QQuickItem*>(object);
    if (!root) {
        if (error) *error = QStringLiteral("media settings harness root is not an item");
        delete object;
        return nullptr;
    }
    root->setSize(window.size());
    root->setParentItem(window.contentItem());
    return root;
}
}

void MediaOverlayTest::initTestCase()
{
    registerCanvasQmlTypes();
}

void MediaOverlayTest::segmentedStatusFillReachesBothEdges_data()
{
    QTest::addColumn<bool>("auxiliaryVisible");
    QTest::newRow("without-volume") << false;
    QTest::newRow("with-volume") << true;
}

void MediaOverlayTest::segmentedStatusFillReachesBothEdges()
{
    QFETCH(bool, auxiliaryVisible);
    QQmlEngine engine;
    QQuickWindow window;
    window.setColor(Qt::white);
    window.resize(420, 100);
    std::unique_ptr<QQuickItem> card(
        createStatusCard(engine, window, auxiliaryVisible));
    QVERIFY2(card, "SegmentedStatusCard must instantiate from packaged QML");
    auto* status = card->findChild<QQuickItem*>(QStringLiteral("statusSegment"));
    QVERIFY(status);

    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));
    QTRY_VERIFY(window.isExposed());
    QCoreApplication::processEvents();
    const QImage image = window.grabWindow();
    QVERIFY(!image.isNull());

    const QPointF origin = status->mapToItem(window.contentItem(), QPointF(0, 0));
    const QColor expected = overWhite(status->property("color").value<QColor>());
    const qreal x = origin.x() + status->width() / 2.0;
    QVERIFY2(nearColor(imagePixel(image, window.size(),
                                 QPointF(x, origin.y() + 2.0)), expected),
             "The colored status fill must reach the top edge behind the border");
    QVERIFY2(nearColor(imagePixel(image, window.size(),
                                 QPointF(x, origin.y() + status->height() - 3.0)), expected),
             "The colored status fill must reach the bottom edge behind the border");
}

void MediaOverlayTest::availableStatusUsesNeutralGreyPalette()
{
    QQmlEngine engine;
    QQuickWindow window;
    std::unique_ptr<QQuickItem> card(createStatusCard(engine, window));
    QVERIFY(card);

    card->setProperty("statusText", QStringLiteral("Available"));
    QCOMPARE(card->property("availableStatus").toBool(), true);

    const QColor foreground = card->property("statusForeground").value<QColor>();
    const QColor background = card->property("statusBackground").value<QColor>();
    QVERIFY(foreground.alphaF() > 0.5);
    QVERIFY(foreground.alphaF() < 0.6);
    QVERIFY(qAbs(foreground.red() - foreground.green()) <= 8);
    QVERIFY(qAbs(foreground.green() - foreground.blue()) <= 8);
    QVERIFY(qAbs(background.red() - background.green()) <= 8);
    QVERIFY(qAbs(background.green() - background.blue()) <= 8);
}

void MediaOverlayTest::segmentedStatusKeepsSingleTopBorder()
{
    QQmlEngine engine;
    QQuickWindow window;
    window.setColor(Qt::white);
    window.resize(420, 100);
    std::unique_ptr<QQuickItem> card(createStatusCard(engine, window));
    QVERIFY(card);
    auto* border = card->findChild<QQuickItem*>(QStringLiteral("cardBorder"));
    QVERIFY(border);

    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));
    QCoreApplication::processEvents();
    const QImage image = window.grabWindow();
    QVERIFY(!image.isNull());
    const QPointF origin = border->mapToItem(window.contentItem(), QPointF(0, 0));

    const QColor top = imagePixel(
        image, window.size(),
        QPointF(origin.x() + border->width() / 2.0, origin.y()));
    const QColor justOutside = imagePixel(
        image, window.size(),
        QPointF(origin.x() + border->width() / 2.0, origin.y() - 2.0));
    QVERIFY(top != justOutside);
}

void MediaOverlayTest::mediaPanelVisibilityAnchorInteractionAndScroll()
{
    QQmlEngine engine;
    QQuickWindow window;
    window.resize(640, 480);
    MediaListModel model;
    QString error;
    std::unique_ptr<QQuickItem> harness(
        createMediaPanelHarness(engine, window, &model, &error));
    QVERIFY2(harness, qPrintable(error));
    auto* panel = harness->findChild<QQuickItem*>(QStringLiteral("mediaListPanel"));
    auto* list = harness->findChild<QQuickItem*>(QStringLiteral("mediaList"));
    QVERIFY(panel);
    QVERIFY(list);

    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));
#ifdef Q_OS_MACOS
    MacWindowManager::activateApplicationWindow(&window);
#else
    window.requestActivate();
#endif
    QVERIFY(QTest::qWaitForWindowActive(&window));
    harness->setSize(window.size());
    QCoreApplication::processEvents();
    QVERIFY(!panel->isVisible());
    QVERIFY(!list->isVisible());

    QVariantList rows;
    rows.append(QVariantMap{{QStringLiteral("rowKey"), QStringLiteral("media-0")},
                            {QStringLiteral("mediaId"), QStringLiteral("media-0")},
                            {QStringLiteral("displayName"), QStringLiteral("Image 0")},
                            {QStringLiteral("mediaType"), QStringLiteral("image")},
                            {QStringLiteral("uploadState"), QStringLiteral("not_uploaded")},
                            {QStringLiteral("uploadProgress"), 0}});
    model.updateFromList(rows);
    harness->setProperty("testMediaCount", 1);
    QCoreApplication::processEvents();

    QVERIFY(panel->isVisible());
    QCOMPARE(qRound(panel->x() + panel->width()), window.width() - 16);
    QCOMPARE(qRound(panel->y() + panel->height()), window.height() - 16);

    QTRY_COMPARE(list->property("count").toInt(), 1);
    QTRY_VERIFY(findVisualItem(harness.get(), QStringLiteral("mediaRow_0")));
    auto* firstRow = findVisualItem(harness.get(), QStringLiteral("mediaRow_0"));
    QVERIFY(firstRow);
    QTRY_VERIFY(firstRow->width() > 0 && firstRow->height() > 0);
    QTRY_VERIFY(list->height() >= firstRow->height());
    const QPoint clickPoint = firstRow->mapToScene(
        QPointF(firstRow->width() / 2.0, firstRow->height() / 2.0)).toPoint();
    QTest::mouseClick(&window, Qt::LeftButton, Qt::NoModifier, clickPoint);
    QCoreApplication::processEvents();
    QCOMPARE(harness->property("selectionCalls").toInt(), 1);

    auto cachedRow = rows[0].toMap();
    cachedRow.insert(QStringLiteral("uploadState"), QStringLiteral("uploaded"));
    for (const bool cached : {false, true, false}) {
        cachedRow.insert(QStringLiteral("remoteCached"), cached);
        rows[0] = cachedRow;
        model.updateFromList(rows);
        auto* status = findVisualItem(panel, QStringLiteral("mediaStatus_0"));
        auto* progress = findVisualItem(panel, QStringLiteral("mediaProgress_0"));
        auto* fill = findVisualItem(panel, QStringLiteral("mediaProgressFill_0"));
        QVERIFY(status && progress && fill);
        QTRY_COMPARE(status->isVisible(), cached);
        QCOMPARE(progress->isVisible(), !cached);
        if (!cached) QCOMPARE(fill->width(), progress->width());
        else QCOMPARE(fill->opacity(), 1.0);
        QTRY_COMPARE(status->property("text").toString(), cached
            ? QStringLiteral("Uploaded and Cached") : QStringLiteral("Uploaded"));
        QCOMPARE(status->property("color").value<QColor>(),
                 QColor(cached ? "#2ecc71" : "#f39c12"));
        QCOMPARE(findVisualItem(panel, QStringLiteral("mediaRow_0")), firstRow);
    }

    for (int index = 1; index < 30; ++index) {
        rows.append(QVariantMap{
            {QStringLiteral("rowKey"), QStringLiteral("media-%1").arg(index)},
            {QStringLiteral("mediaId"), QStringLiteral("media-%1").arg(index)},
            {QStringLiteral("displayName"), QStringLiteral("Image %1").arg(index)},
            {QStringLiteral("mediaType"), QStringLiteral("image")},
            {QStringLiteral("uploadState"), QStringLiteral("uploaded")},
            {QStringLiteral("uploadProgress"), 100}});
    }
    model.updateFromList(rows);
    harness->setProperty("testMediaCount", rows.size());
    QCoreApplication::processEvents();

    QTRY_COMPARE(list->property("count").toInt(), rows.size());
    QTRY_VERIFY(list->property("contentHeight").toReal() > list->height());
    list->setProperty("contentY", 100.0);
    QCoreApplication::processEvents();
    QVERIFY(list->property("contentY").toReal() > 0.0);
    QCOMPARE(qRound(panel->x() + panel->width()), window.width() - 16);
    QCOMPARE(qRound(panel->y() + panel->height()), window.height() - 16);
}

void MediaOverlayTest::mediaCountTracksRealCanvasInsertions()
{
    QQmlEngine engine;
    QQuickWindow window;
    window.resize(640, 480);
    QString error;
    std::unique_ptr<QuickCanvasHost> host(QuickCanvasHost::create(&error));
    QVERIFY2(host, qPrintable(error));
    host->setProjectEditingEnabled(true);
    ClientWorkspaceViewModel session(
        QStringLiteral("media-count-session"), host.get(), [] {}, nullptr,
        [] { return false; }, [] { return true; }, [] { return true; });
    QSignalSpy countChanged(&session, &ClientWorkspaceViewModel::mediaCountChanged);
    std::unique_ptr<QQuickItem> harness(
        createRealMediaPanelHarness(engine, window, &session, &error));
    QVERIFY2(harness, qPrintable(error));
    auto* panel = findVisualItem(
        harness.get(), QStringLiteral("realMediaListPanel"));
    QVERIFY(panel);

    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));
    harness->setSize(window.size());
    QCoreApplication::processEvents();
    QCOMPARE(session.mediaCount(), 0);
    QVERIFY(!panel->isVisible());
    QVERIFY(host->document()->addText(QPointF(100, 100)));
    QCOMPARE(session.mediaCount(), 1);
    QVERIFY(countChanged.count() >= 1);
    QTRY_VERIFY(panel->isVisible());
    QCOMPARE(qRound(panel->x() + panel->width()), window.width() - 16);
    QCOMPARE(qRound(panel->y() + panel->height()), window.height() - 16);
}

void MediaOverlayTest::typedCapabilitiesGuardDirectCppInvocations()
{
    QString error;
    std::unique_ptr<QuickCanvasHost> host(QuickCanvasHost::create(&error));
    QVERIFY2(host, qPrintable(error));
    bool projectExists = false;
    int uploadInvocations = 0;
    ClientWorkspaceViewModel workspace(
        QStringLiteral("capability-workspace"), host.get(),
        [&uploadInvocations] { ++uploadInvocations; }, nullptr,
        [] { return false; }, [] { return true; },
        [&projectExists] { return projectExists; });

    QVERIFY(workspace.canvasNavigation());
    QVERIFY(!workspace.mediaEditingEnabled());
    QVERIFY(!workspace.textCreation());
    QVERIFY(!workspace.fileDrop());
    QVERIFY(!workspace.localTest());
    QVERIFY(!workspace.mediaSync());
    QVERIFY(!workspace.remoteScene());
    QCOMPARE(workspace.mediaEditingUnavailableReason(),
             QStringLiteral("Create a project first"));
    QCOMPARE(workspace.textCreationUnavailableReason(),
             QStringLiteral("Create a project first"));
    QCOMPARE(workspace.fileDropUnavailableReason(),
             QStringLiteral("Create a project first"));
    QCOMPARE(workspace.mediaSyncUnavailableReason(),
             QStringLiteral("Create a project first"));

    workspace.setActiveTool(QStringLiteral("text"));
    QCOMPARE(host->currentTool(), ICanvasHost::Tool::Selection);
    host->controller()->handleTextCreateRequested(10, 10);
    QVERIFY(host->document()->media().isEmpty());

    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString imagePath = temporary.filePath(QStringLiteral("drop.png"));
    QImage image(8, 8, QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::red);
    QVERIFY(image.save(imagePath));
    const QVariantList urls{QUrl::fromLocalFile(imagePath)};
    QVERIFY(!workspace.beginFileDrag(urls, 20, 20));
    QVERIFY(!host->controller()->beginLocalFileDrag(urls, 20, 20));
    workspace.toggleRemoteScene();
    workspace.toggleTestScene();
    workspace.triggerUploadAction();
    QCOMPARE(uploadInvocations, 0);
    QVERIFY(!host->remoteSceneLaunched());
    QVERIFY(!host->testSceneLaunched());

    projectExists = true;
    host->setProjectEditingEnabled(true);
    workspace.refreshCapabilities();
    QVERIFY(workspace.mediaEditingEnabled());
    QVERIFY(workspace.textCreation());
    QVERIFY(workspace.fileDrop());
    QVERIFY(!workspace.mediaSync());
    QCOMPARE(workspace.mediaSyncUnavailableReason(),
             QStringLiteral("Launch a remote session first"));

    host->controller()->handleTextCreateRequested(10, 10);
    QCOMPARE(host->document()->media().size(), 1);
    CanvasMedia* media = host->document()->media().first();
    QVERIFY(media);
    host->controller()->selectMedia(media->mediaId());
    auto* settings = qobject_cast<MediaSettingsViewModel*>(
        workspace.mediaSettings());
    QVERIFY(settings);
    QVERIFY(settings->available());
    settings->setOpacityText(QStringLiteral("75"));
    QCOMPARE(media->settings().opacityText, QStringLiteral("75"));
    QVERIFY(workspace.localTest());

    host->setOverlayActionsEnabled(true);
    workspace.refreshCapabilities();
    QVERIFY(workspace.mediaSync());

    projectExists = false;
    host->setProjectEditingEnabled(false);
    workspace.refreshCapabilities();
    QVERIFY(!settings->available());
    settings->setOpacityText(QStringLiteral("25"));
    QCOMPARE(media->settings().opacityText, QStringLiteral("75"));
    host->controller()->handleOverlayDelete(media->mediaId());
    QCOMPARE(host->document()->media().size(), 1);
}

void MediaOverlayTest::mediaActionPalette_data()
{
    QTest::addColumn<int>("tone");
    QTest::addColumn<bool>("enabled");
    QTest::addColumn<bool>("busy");
    QTest::addColumn<QColor>("foreground");
    QTest::addColumn<QColor>("idle");
    QTest::addColumn<QColor>("hover");
    QTest::addColumn<QColor>("pressed");
    QTest::newRow("idle") << 0 << true << false << QColor(255, 255, 255, 230)
        << QColor(Qt::transparent) << QColor(255, 255, 255, 13) << QColor(255, 255, 255, 26);
    QTest::newRow("uploading") << 1 << true << true << QColor("#4a90e2")
        << QColor(74, 144, 226, 38) << QColor(74, 144, 226, 56) << QColor(74, 144, 226, 77);
    QTest::newRow("awaiting-ack") << 1 << false << true << QColor("#4a90e2")
        << QColor(74, 144, 226, 38) << QColor(74, 144, 226, 38) << QColor(74, 144, 226, 38);
    QTest::newRow("unload") << 2 << true << false << QColor("#2ecc71")
        << QColor(76, 175, 80, 38) << QColor(76, 175, 80, 56) << QColor(76, 175, 80, 77);
    for (int scene : {3, 4}) {
        QTest::newRow(scene == 3 ? "remote-active" : "test-active")
            << scene << true << false << QColor("#ff96ff")
            << QColor(255, 0, 255, 38) << QColor(255, 0, 255, 56) << QColor(255, 0, 255, 77);
    }
    for (int disabledTone : {0, 1, 2, 3, 4}) {
        QTest::newRow(qPrintable(QStringLiteral("disabled-%1").arg(disabledTone)))
            << disabledTone << false << false << QColor(255, 255, 255, 102)
            << QColor(255, 255, 255, 10) << QColor(255, 255, 255, 10) << QColor(255, 255, 255, 10);
    }
}

void MediaOverlayTest::mediaActionPalette()
{
    QFETCH(int, tone);
    QFETCH(bool, enabled);
    QFETCH(bool, busy);
    QFETCH(QColor, foreground);
    QFETCH(QColor, idle);
    QFETCH(QColor, hover);
    QFETCH(QColor, pressed);
    QQmlEngine engine;
    QQuickWindow window;
    window.resize(360, 120);
    window.setColor(Qt::white);
    QQmlComponent component(&engine, QUrl(QStringLiteral(
        "qrc:/qt/qml/Mouffette/App/resources/qml/app/canvas/OverlayActionButton.qml")));
    std::unique_ptr<QQuickItem> button(qobject_cast<QQuickItem*>(component.create()));
    QVERIFY2(button, qPrintable(component.errorString()));
    button->setParentItem(window.contentItem());
    button->setPosition({20, 20});
    button->setSize({300, 40});
    button->setProperty("text", QStringLiteral("Media action"));
    button->setProperty("tone", tone);
    button->setProperty("busy", busy);
    button->setEnabled(enabled);
    QSignalSpy clicked(button.get(), SIGNAL(clicked()));
    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));
#ifdef Q_OS_MACOS
    MacWindowManager::activateApplicationWindow(&window);
#else
    window.requestActivate();
#endif
    QVERIFY(QTest::qWaitForWindowActive(&window));
    QTest::mouseMove(&window, {5, 5});
    QCOMPARE(button->property("foregroundColor").value<QColor>(), foreground);
    auto verifyFill = [&](const QColor& fill) {
        const QImage frame = window.grabWindow();
        return !frame.isNull() && nearColor(imagePixel(frame, window.size(), {25, 25}), overWhite(fill));
    };
    QTRY_VERIFY(verifyFill(idle));
    QTest::mouseMove(&window, {100, 40});
    if (enabled) QTRY_VERIFY(button->property("hovered").toBool());
    QTRY_VERIFY(verifyFill(hover));
    QTest::mousePress(&window, Qt::LeftButton, Qt::NoModifier, {100, 40});
    QTRY_VERIFY(verifyFill(pressed));
    QTest::mouseRelease(&window, Qt::LeftButton, Qt::NoModifier, {100, 40});
    QCOMPARE(clicked.count(), enabled ? 1 : 0);
}

void MediaOverlayTest::mediaRowsAndProgress()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString path = temporary.filePath(QStringLiteral("Photo de vacances.png"));
    QImage source(1920, 1080, QImage::Format_ARGB32_Premultiplied);
    source.fill(Qt::darkCyan);
    QVERIFY(source.save(path));
    QString error;
    std::unique_ptr<QuickCanvasHost> host(QuickCanvasHost::create(&error));
    QVERIFY2(host, qPrintable(error));
    host->setProjectEditingEnabled(true);
    CanvasMedia* photo = host->document()->addPreparedFile(path, source.size(), false, {0, 0});
    CanvasMedia* text = host->document()->addText({0, 0}, QStringLiteral("Titre de la scène"));
    QVERIFY(photo);
    QVERIFY(text);
    photo->setZ(10);
    ClientWorkspaceViewModel session(QStringLiteral("rows"), host.get(), [] {}, nullptr,
                                  [] { return false; }, [] { return true; }, [] { return true; });
    QQmlEngine engine;
    QQuickWindow window;
    window.resize(900, 600);
    window.setColor(QColor("#1e1e1e"));
    std::unique_ptr<QQuickItem> harness(createRealMediaPanelHarness(engine, window, &session, &error));
    QVERIFY2(harness, qPrintable(error));
    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));
    auto* panel = findVisualItem(harness.get(), QStringLiteral("realMediaListPanel"));
    QVERIFY(panel);
    QTRY_VERIFY(findVisualItem(panel, QStringLiteral("mediaRow_1")));
    auto* row = findVisualItem(panel, QStringLiteral("mediaRow_0"));
    auto* textRow = findVisualItem(panel, QStringLiteral("mediaRow_1"));
    QCOMPARE(row->property("mediaId").toString(), photo->mediaId());
    QCOMPARE(textRow->property("mediaId").toString(), text->mediaId());
    QVERIFY(row->height() > textRow->height());
    auto* name = findVisualItem(panel, QStringLiteral("mediaName_0"));
    auto* details = findVisualItem(panel, QStringLiteral("mediaDetails_0"));
    auto* status = findVisualItem(panel, QStringLiteral("mediaStatus_0"));
    auto* progress = findVisualItem(panel, QStringLiteral("mediaProgress_0"));
    auto* fill = findVisualItem(panel, QStringLiteral("mediaProgressFill_0"));
    QVERIFY(name && details && status && progress && fill);
    QCOMPARE(name->property("text").toString(), QFileInfo(path).fileName());
    QVERIFY(details->property("text").toString().startsWith(QStringLiteral("1920 x 1080 px  ·  ")));
    QVERIFY(!details->property("text").toString().endsWith(QStringLiteral("n/a")));
    QCOMPARE(status->property("text").toString(), QStringLiteral("Not uploaded"));
    QVERIFY(!findVisualItem(panel, QStringLiteral("mediaStatus_1"))->isVisible());
    QVERIFY(!textRow->property("detailsText").toString().contains(QStringLiteral(" · ")));
    const qreal originalHeight = row->height();
    photo->setUploadUploading(37);
    QTRY_VERIFY(progress->isVisible());
    QVERIFY(!status->isVisible());
    QCOMPARE(progress->height(), 10.0);
    QCOMPARE(progress->width(), row->width() - 40);
    QCOMPARE(fill->width(), progress->width() * 0.37);
    QCOMPARE(fill->property("color").value<QColor>(), QColor("#2d8cff"));
    QCOMPARE(row->height(), originalHeight);
    // The border is painted over the fill; rounded corners reveal the canvas.
    const QImage frame = window.grabWindow();
    QVERIFY(!frame.isNull());
    const QPointF origin = panel->mapToScene({0, 0});
    QVERIFY(nearColor(imagePixel(frame, window.size(), origin), window.color()));
    const QString artifactDir = qEnvironmentVariable("MOUFFETTE_OVERLAY_ARTIFACT_DIR");
    if (!artifactDir.isEmpty()) {
        QVERIFY(QDir().mkpath(artifactDir));
        QVERIFY(frame.save(QDir(artifactDir).filePath(QStringLiteral("media-overlay-uploading.png"))));
    }
    photo->setUploadUploaded();
    QTRY_COMPARE(fill->width(), progress->width());
    QVERIFY(progress->isVisible());
    QVERIFY(!status->isVisible());
    QTRY_VERIFY_WITH_TIMEOUT(fill->opacity() < 0.6, 1000);
    if (!artifactDir.isEmpty())
        QVERIFY(window.grabWindow().save(QDir(artifactDir).filePath(QStringLiteral("media-overlay-caching.png"))));
    QTRY_VERIFY_WITH_TIMEOUT(fill->opacity() > 0.95, 1200);
    QCOMPARE(row->height(), originalHeight);
    host->document()->select(photo->mediaId());
    QTRY_VERIFY(row->property("selected").toBool());
    const QPoint clickPoint = textRow->mapToScene({textRow->width() / 2, textRow->height() / 2}).toPoint();
    QTest::mouseClick(&window, Qt::LeftButton, Qt::NoModifier, clickPoint);
    QTRY_COMPARE(host->document()->selectedMedia(), text);
    QVERIFY(!row->property("selected").toBool());
    text->setZ(20);
    QTRY_COMPARE(findVisualItem(panel, QStringLiteral("mediaRow_0"))->property("mediaId").toString(), text->mediaId());
    text->setText(QString(80, QLatin1Char('W')));
    QTRY_COMPARE(panel->width(), 420.0);
    harness->setWidth(600);
    QTRY_COMPARE(panel->width(), 300.0);
    photo->setUploadNotUploaded();
    host->document()->clear();
    QTRY_VERIFY(!panel->isVisible());
}

void MediaOverlayTest::uploadActionLocksBeforeDispatchAndRecovers()
{
    QTemporaryDir temporary;
    FileManager files;
    UploadManager uploads(&files, nullptr, temporary.filePath(QStringLiteral("Uploads")));
    QString error;
    std::unique_ptr<QuickCanvasHost> host(QuickCanvasHost::create(&error));
    QVERIFY2(host, qPrintable(error));
    host->setProjectEditingEnabled(true);
    host->setOverlayActionsEnabled(true);
    QVERIFY(host->document()->addText({0, 0}));
    bool remoteFiles = false;
    bool acceptAction = false;
    bool hasProject = true;
    bool sawLock = false;
    int calls = 0;
    ClientWorkspaceViewModel* workspace = nullptr;
    ClientWorkspaceViewModel session(QStringLiteral("persistent-workspace"), host.get(), [&] {
        ++calls;
        sawLock = workspace->actionPending() && !workspace->uploadActionEnabled()
            && !workspace->testSceneActionEnabled();
        workspace->triggerUploadAction(); // Reentrant input cannot send twice.
        if (acceptAction) remoteFiles = !remoteFiles;
    }, &uploads, [&] { return remoteFiles; }, [&] { return !remoteFiles; }, [&] { return hasProject; });
    workspace = &session;
    QCOMPARE(session.uploadActionTone(), 0);
    for (int expectedCalls = 1; expectedCalls <= 4; ++expectedCalls) {
        acceptAction = expectedCalls % 2 == 0;
        const QString before = session.uploadActionText();
        session.triggerUploadAction();
        QVERIFY(session.actionPending());
        QVERIFY(!session.uploadActionEnabled());
        QCOMPARE(calls, expectedCalls - 1);
        session.triggerUploadAction();
        QTRY_VERIFY(!session.actionPending());
        QCOMPARE(calls, expectedCalls);
        QVERIFY(sawLock);
        QVERIFY(session.uploadActionEnabled());
        if (!acceptAction) QCOMPARE(session.uploadActionText(), before);
        QCOMPARE(session.uploadActionText(), remoteFiles ? QStringLiteral("Unload") : QStringLiteral("Upload"));
        QCOMPARE(session.uploadActionTone(), remoteFiles ? 2 : 0);
    }
    session.triggerUploadAction();
    hasProject = false; // A queued request must recheck capabilities.
    QTRY_VERIFY(!session.actionPending());
    QCOMPARE(calls, 4);
    QVERIFY(!session.uploadActionEnabled());
}

void MediaOverlayTest::overlayButtonHoverIsImmediate()
{
    QQmlEngine engine;
    QQuickWindow window;
    window.resize(100, 80);
    QString error;
    std::unique_ptr<QQuickItem> button(createOverlayButton(engine, window, &error));
    QVERIFY2(button, qPrintable(error));

    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));
    const QColor normal = button->property("currentBackgroundColor").value<QColor>();
    QTest::mouseMove(&window, QPoint(38, 38));
    QTRY_VERIFY(button->property("hovered").toBool());
    const QColor hovered = button->property("currentBackgroundColor").value<QColor>();
    QVERIFY(hovered != normal);

    QTest::mousePress(&window, Qt::LeftButton, Qt::NoModifier, QPoint(38, 38));
    QCOMPARE(button->property("currentBackgroundColor").value<QColor>(),
             QColor(52, 87, 128, 242));
    QTest::mouseRelease(&window, Qt::LeftButton, Qt::NoModifier, QPoint(38, 38));
    QCOMPARE(button->property("currentBackgroundColor").value<QColor>(), hovered);
}

void MediaOverlayTest::activationDuringBootstrapKeepsMainWindowHidden()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    RuntimeProfileContext profile;
    profile.ordinal = 2;
    profile.instanceId = QStringLiteral("bootstrap-activation-test");
    profile.profileId = profile.instanceId;
    profile.rootPath = directory.path();
    profile.persistent = false;
    QQmlEngine engine;
    ApplicationController controller(profile, {});
    QQmlComponent component(&engine, QUrl(QStringLiteral(
        "qrc:/qt/qml/Mouffette/App/resources/qml/app/Main.qml")));
    std::unique_ptr<QObject> root(component.createWithInitialProperties({
        {QStringLiteral("controller"), QVariant::fromValue(&controller)}
    }));
    QVERIFY2(root, qPrintable(component.errorString()));
    auto* bootstrap = qobject_cast<QWindow*>(root->property("bootstrap").value<QObject*>());
    auto* window = qobject_cast<QWindow*>(root->property("window").value<QObject*>());
    QVERIFY(bootstrap && window);
    QVERIFY(!controller.ready());
    QVERIFY(!window->isVisible());
    bootstrap->hide();
    // A second launch/activation must raise the loading window, not bypass
    // the storage and multimedia readiness gate on the main application.
    controller.raiseRequested();
    QTRY_VERIFY(bootstrap->isVisible());
    QVERIFY(!window->isVisible());
    bootstrap->hide();
}

void MediaOverlayTest::mainWindowPointerActivity_data()
{
    QTest::addColumn<bool>("startsAsTouchpad");
    QTest::addColumn<bool>("reclassifyAfterScroll");
    QTest::newRow("mouse") << false << false;
    QTest::newRow("trackpad-already-scrolled") << true << false;
    QTest::newRow("macos-trackpad-or-magic-mouse-first-scroll") << false << true;
}

void MediaOverlayTest::mainWindowPointerActivity()
{
    QFETCH(bool, startsAsTouchpad);
    QFETCH(bool, reclassifyAfterScroll);

    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    RuntimeProfileContext profile;
    profile.ordinal = 2;
    profile.instanceId = QStringLiteral("window-activity-test");
    profile.profileId = profile.instanceId;
    profile.rootPath = directory.path();
    profile.persistent = false;

    // Load the shipped shell with its real presentation controller. Bootstrap
    // is deliberately not started: this input test needs no network or stores.
    QQmlEngine engine;
    ApplicationController controller(profile, {});
    QQmlComponent component(&engine, QUrl(QStringLiteral(
        "qrc:/qt/qml/Mouffette/App/resources/qml/app/Main.qml")));
    std::unique_ptr<QObject> root(component.createWithInitialProperties({
        {QStringLiteral("controller"), QVariant::fromValue(&controller)}
    }));
    QVERIFY2(root, qPrintable(component.errorString()));
    auto* bootstrap = qobject_cast<QWindow*>(
        root->property("bootstrap").value<QObject*>());
    QVERIFY(bootstrap);
    bootstrap->hide();
    auto* window = qobject_cast<QQuickWindow*>(
        root->property("window").value<QObject*>());
    QVERIFY(window);
    auto* hover = window->findChild<QQuickHoverHandler*>(
        QStringLiteral("windowActivityHover"));
    QVERIFY(hover);

    window->showNormal();
    QVERIFY(QTest::qWaitForWindowExposed(window));
    // Qt Quick synthesizes hover events using the primary pointing device,
    // even when a mouse event carries a different test device.
    const auto* device = QPointingDevice::primaryPointingDevice();
    auto* deviceState = QPointingDevicePrivate::get(
        const_cast<QPointingDevice*>(device));
    const auto originalType = deviceState->deviceType;
    const auto restoreDevice = qScopeGuard([&]() {
        deviceState->deviceType = originalType;
    });
    deviceState->deviceType = startsAsTouchpad
        ? QInputDevice::DeviceType::TouchPad : QInputDevice::DeviceType::Mouse;
    const auto move = [&](const QPointF& position,
                          Qt::MouseButtons buttons = Qt::NoButton) {
        QMouseEvent event(QEvent::MouseMove, position, position,
                          window->mapToGlobal(position), Qt::NoButton,
                          buttons, Qt::NoModifier, device);
        QCoreApplication::sendEvent(window, &event);
        // Qt Quick coalesces moves until the next frame.
        QQuickWindowPrivate::get(window)->deliveryAgentPrivate()
            ->flushFrameSynchronousEvents(window);
    };

    move(QPointF(100, 100));
    QVERIFY(hover->isHovered());
    QSignalSpy hoverChanges(hover, &QQuickHoverHandler::hoveredChanged);

    if (reclassifyAfterScroll) {
        // Match QCocoa's precise-scroll path: the SAME device changes type
        // after the first trackpad/Magic Mouse wheel event.
        deviceState->deviceType = QInputDevice::DeviceType::TouchPad;
    }
    move(QPointF(150, 120));
    QVERIFY(hover->isHovered());
    move(QPointF(200, 150), Qt::LeftButton);
    QVERIFY(hover->isHovered());
    QCOMPARE(hoverChanges.count(), 0);

    // Stationary presence is activity; no recurring move or click is needed.
    QTest::qWait(100);
    QVERIFY(hover->isHovered());
    QCOMPARE(hoverChanges.count(), 0);

    QEvent leave(QEvent::Leave);
    QCoreApplication::sendEvent(window, &leave);
    QVERIFY(!hover->isHovered());
    QCOMPARE(hoverChanges.count(), 1);
    move(QPointF(120, 140));
    QVERIFY(hover->isHovered());
    QCOMPARE(hoverChanges.count(), 2);
}

void MediaOverlayTest::toolbarToolsAndGlobalMemoryUsage()
{
    QTemporaryDir directory;
    const QString mediaPath = directory.filePath(QStringLiteral("memory-example.png"));
    QImage image(640, 360, QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::cyan);
    QVERIFY(image.save(mediaPath));
    CanvasDocument document;
    auto* media = document.addPreparedFile(mediaPath, image.size(), false, {});
    QTRY_VERIFY_WITH_TIMEOUT(media->residencyReady(), 5000);
    QQmlEngine engine;
    QQuickWindow window;
    window.resize(900, 650);
    QString error;
    std::unique_ptr<QQuickItem> harness(
        createCanvasToolbarHarness(engine, window, &error));
    QVERIFY2(harness, qPrintable(error));
    auto* settings = findVisualItem(harness.get(), QStringLiteral("canvasSettingsButton"));
    auto* selection = findVisualItem(harness.get(), QStringLiteral("canvasSelectionToolButton"));
    auto* text = findVisualItem(harness.get(), QStringLiteral("canvasTextToolButton"));
    QVERIFY(settings);
    QVERIFY(selection);
    QVERIFY(text);

    QCOMPARE(settings->width(), 36.0);
    QCOMPARE(settings->height(), 36.0);
    QVERIFY(settings->property("isToggle").toBool());
    QVERIFY(selection->property("toggled").toBool());
    QVERIFY(!text->property("toggled").toBool());
    QCOMPARE(selection->property("segmentRole").toString(), QStringLiteral("leading"));
    QCOMPARE(text->property("segmentRole").toString(), QStringLiteral("trailing"));
    QCOMPARE(selection->x() + selection->width(), text->x());

    harness->setProperty("fakeSessionHasProject", false);
    QTRY_VERIFY(!text->isVisible());
    QTRY_VERIFY(!selection->isVisible());
    QVERIFY(selection->property("toggled").toBool());

    harness->setProperty("fakeSessionHasProject", true);
    QTRY_VERIFY(text->isVisible());
    QTRY_VERIFY(selection->isVisible());

    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));
#ifdef Q_OS_MACOS
    MacWindowManager::activateApplicationWindow(&window);
#else
    window.requestActivate();
#endif
    QVERIFY(QTest::qWaitForWindowActive(&window));
    const QPoint textCenter = text->mapToScene(
        QPointF(text->width() / 2.0, text->height() / 2.0)).toPoint();
    QTest::mouseClick(&window, Qt::LeftButton, Qt::NoModifier, textCenter);
    QTRY_VERIFY(text->property("toggled").toBool());
    QVERIFY(!selection->property("toggled").toBool());
    window.hide();

    // RAM belongs to the application shell and must work without a client or
    // project open. Keep the popup coverage on the shipped Main.qml.
    RuntimeProfileContext profile;
    profile.ordinal = 2;
    profile.instanceId = QStringLiteral("global-memory-test");
    profile.profileId = profile.instanceId;
    profile.rootPath = directory.path();
    profile.persistent = false;
    ApplicationController controller(profile, {});
    QQmlComponent component(&engine, QUrl(QStringLiteral(
        "qrc:/qt/qml/Mouffette/App/resources/qml/app/Main.qml")));
    std::unique_ptr<QObject> root(component.createWithInitialProperties({
        {QStringLiteral("controller"), QVariant::fromValue(&controller)}
    }));
    QVERIFY2(root, qPrintable(component.errorString()));
    auto* bootstrap = qobject_cast<QWindow*>(root->property("bootstrap").value<QObject*>());
    QVERIFY(bootstrap);
    bootstrap->hide();
    auto* appWindow = qobject_cast<QQuickWindow*>(root->property("window").value<QObject*>());
    QVERIFY(appWindow);
    auto* memory = findVisualItem(appWindow->contentItem(), QStringLiteral("memoryUsageButton"));
    QVERIFY(memory && memory->isEnabled());
    QCOMPARE(memory->property("text").toString(), QStringLiteral("Usage RAM"));
    appWindow->resize(900, 650);
    appWindow->showNormal();
    QVERIFY(QTest::qWaitForWindowExposed(appWindow));
#ifdef Q_OS_MACOS
    MacWindowManager::activateApplicationWindow(appWindow);
#else
    appWindow->requestActivate();
#endif
    QVERIFY(QTest::qWaitForWindowActive(appWindow));
    const qreal toolbarY = memory->mapToScene({0, 0}).y();
    const QPoint memoryCenter = memory->mapToScene({memory->width() / 2, memory->height() / 2}).toPoint();
    QTest::mouseClick(appWindow, Qt::LeftButton, Qt::NoModifier, memoryCenter);
    QTRY_VERIFY(memory->property("checked").toBool());
    auto* bar = findVisualItem(appWindow->contentItem(), QStringLiteral("memoryDistributionBar"));
    QVERIFY(bar && bar->isVisible() && bar->width() > 400);
    auto* assets = findVisualItem(appWindow->contentItem(), QStringLiteral("memoryAssetList"));
    QVERIFY(assets && assets->property("count").toInt() >= 1);
    const QString artifactDir = qEnvironmentVariable("MOUFFETTE_OVERLAY_ARTIFACT_DIR");
    if (!artifactDir.isEmpty()) {
        QVERIFY(QDir().mkpath(artifactDir));
        QSignalSpy frames(appWindow, &QQuickWindow::frameSwapped);
        appWindow->update();
        QTRY_VERIFY(frames.size() > 0);
        const QString scale = qEnvironmentVariable("QT_SCALE_FACTOR");
        const QString name = scale.isEmpty() ? "memory-usage-popup.png"
            : "memory-usage-popup-scale-" + scale + ".png";
        QVERIFY(appWindow->grabWindow().save(QDir(artifactDir).filePath(name)));
    }
    QTest::keyClick(appWindow, Qt::Key_Escape);
    QTRY_VERIFY(!memory->property("checked").toBool());

    appWindow->resize(480, 650);
    QTRY_VERIFY(memory->mapToScene({memory->width(), 0}).x() <= appWindow->width());
    QVERIFY(memory->isVisible() && memory->width() >= memory->implicitWidth());
    QTest::mouseClick(appWindow, Qt::LeftButton, Qt::NoModifier,
                     memory->mapToScene({memory->width() / 2, memory->height() / 2}).toPoint());
    QTRY_VERIFY(memory->property("checked").toBool());
    QTest::keyClick(appWindow, Qt::Key_Escape);
    QTRY_VERIFY(!memory->property("checked").toBool());
    appWindow->resize(900, 650);
    QTRY_COMPARE(memory->mapToScene({0, 0}).y(), toolbarY);
}

void MediaOverlayTest::mediaSettingsPanelRestoresTabsAndBindings()
{
    QQmlEngine engine;
    QQuickWindow window;
    window.resize(640, 480);
    QString error;
    std::unique_ptr<QuickCanvasHost> host(QuickCanvasHost::create(&error));
    QVERIFY2(host, qPrintable(error));
    host->setProjectEditingEnabled(true);
    ClientWorkspaceViewModel session(
        QStringLiteral("media-settings-session"), host.get(), [] {}, nullptr,
        [] { return false; }, [] { return true; }, [] { return true; });
    std::unique_ptr<QQuickItem> harness(
        createMediaSettingsHarness(engine, window, &session, &error));
    QVERIFY2(harness, qPrintable(error));
    connect(&window, &QWindow::widthChanged, harness.get(), [&window, item = harness.get()] { item->setSize(window.size()); });
    connect(&window, &QWindow::heightChanged, harness.get(), [&window, item = harness.get()] { item->setSize(window.size()); });

    auto* panel = findVisualItem(
        harness.get(), QStringLiteral("realMediaSettingsPanel"));
    auto* sceneTab = findVisualItem(
        harness.get(), QStringLiteral("sceneSettingsTab"));
    auto* elementTab = findVisualItem(
        harness.get(), QStringLiteral("elementSettingsTab"));
    auto* scenePage = findVisualItem(
        harness.get(), QStringLiteral("sceneSettingsPage"));
    auto* elementPage = findVisualItem(
        harness.get(), QStringLiteral("elementSettingsPage"));
    QVERIFY(panel);
    QVERIFY(sceneTab);
    QVERIFY(elementTab);
    QVERIFY(scenePage);
    QVERIFY(elementPage);

    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));
#ifdef Q_OS_MACOS
    MacWindowManager::activateApplicationWindow(&window);
#else
    window.requestActivate();
#endif
    QVERIFY(QTest::qWaitForWindowActive(&window));
    harness->setSize(window.size());
    session.setSettingsVisible(true);
    QCoreApplication::processEvents();
    QVERIFY(!panel->isVisible());

    CanvasMedia* media = host->document()->addText(
        QPointF(100, 100), QStringLiteral("Settings test"));
    QVERIFY(media);
    QTRY_VERIFY(panel->isVisible());
    panel->setProperty("presentationReady", false);
    QVERIFY(!panel->isVisible() && !panel->isEnabled());
    panel->setProperty("presentationReady", true);
    QTRY_VERIFY(panel->isVisible() && panel->isEnabled());
    QCOMPARE(panel->width(), 221.0);
    QCOMPARE(panel->x(), 10.0);
    QCOMPARE(panel->y(), 52.0);
    QVERIFY(panel->height() > 41.0);
    QVERIFY(panel->height() <= 418.0);
    QCOMPARE(panel->property("activeTab").toInt(), 0);
    QVERIFY(scenePage->isVisible());
    QVERIFY(!elementPage->isVisible());
    auto* contentFlick = findVisualItem(
        harness.get(), QStringLiteral("settingsContentFlick"));
    auto* scrollBar = findVisualItem(
        harness.get(), QStringLiteral("settingsOverlayScrollBar"));
    QVERIFY(contentFlick);
    QVERIFY(scrollBar);
    QTRY_VERIFY(!contentFlick->property("overflowing").toBool());
    QVERIFY(qAbs(contentFlick->property("contentHeight").toReal()
                 - contentFlick->height()) <= 0.5);
    QVERIFY(!scrollBar->isVisible());

    const QStringList restoredControls{
        QStringLiteral("displayAutomaticallyCheck"),
        QStringLiteral("displayDelayCheck"),
        QStringLiteral("hideDelayCheck"),
        QStringLiteral("hideWhenVideoEndsCheck"),
        QStringLiteral("unmuteAutomaticallyCheck"),
        QStringLiteral("unmuteDelayCheck"),
        QStringLiteral("muteDelayCheck"),
        QStringLiteral("muteWhenVideoEndsCheck"),
        QStringLiteral("playAutomaticallyCheck"),
        QStringLiteral("playDelayCheck"),
        QStringLiteral("pauseDelayCheck"),
        QStringLiteral("repeatCheck"),
        QStringLiteral("imageFadeInCheck"),
        QStringLiteral("imageFadeOutCheck"),
        QStringLiteral("opacityCheck"),
        QStringLiteral("volumeCheck"),
        QStringLiteral("audioFadeInCheck"),
        QStringLiteral("audioFadeOutCheck"),
        QStringLiteral("textColorCheck"),
        QStringLiteral("highlightCheck"),
        QStringLiteral("textBorderWidthCheck"),
        QStringLiteral("textBorderColorCheck"),
        QStringLiteral("fontWeightCheck"),
        QStringLiteral("underlineCheck"),
        QStringLiteral("italicCheck"),
        QStringLiteral("uppercaseCheck")};
    for (const QString& objectName : restoredControls) {
        QVERIFY2(findVisualItem(harness.get(), objectName),
                 qPrintable(QStringLiteral("missing restored setting: %1")
                                .arg(objectName)));
    }

    auto* displayAutomaticallyCheck = findVisualItem(
        harness.get(), QStringLiteral("displayAutomaticallyCheck"));
    auto* displayDelayCheck = findVisualItem(
        harness.get(), QStringLiteral("displayDelayCheck"));
    auto* displayDelayField = findVisualItem(
        harness.get(), QStringLiteral("displayDelayField"));
    QVERIFY(displayAutomaticallyCheck);
    QVERIFY(displayDelayCheck);
    QVERIFY(displayDelayField);
    QVERIFY(displayDelayCheck->isEnabled());
    QTest::mouseClick(
        &window, Qt::LeftButton, Qt::NoModifier,
        displayDelayCheck->mapToScene(
            QPointF(displayDelayCheck->width() / 2.0,
                    displayDelayCheck->height() / 2.0)).toPoint());
    QTRY_VERIFY(media->settings().displayDelayEnabled);
    QTest::mouseClick(
        &window, Qt::LeftButton, Qt::NoModifier,
        displayAutomaticallyCheck->mapToScene(
            QPointF(displayAutomaticallyCheck->width() / 2.0,
                    displayAutomaticallyCheck->height() / 2.0)).toPoint());
    QTRY_VERIFY(!media->settings().displayAutomatically);
    QTRY_VERIFY(!media->settings().displayDelayEnabled);
    QTRY_VERIFY(!displayDelayCheck->isEnabled());

    const int canvasPressesBeforeDisabledInput =
        harness->property("canvasPrimaryPressCount").toInt();
    QTest::mouseClick(
        &window, Qt::LeftButton, Qt::NoModifier,
        displayDelayField->mapToScene(
            QPointF(displayDelayField->width() / 2.0,
                    displayDelayField->height() / 2.0)).toPoint());
    QCOMPARE(harness->property("canvasPrimaryPressCount").toInt(),
             canvasPressesBeforeDisabledInput);
    QVERIFY(!displayDelayField->hasActiveFocus());
    QCOMPARE(host->document()->selectedMedia(), media);

    const QPoint elementTabCenter = elementTab->mapToScene(
        QPointF(elementTab->width() / 2.0,
                elementTab->height() / 2.0)).toPoint();
    QTest::mouseClick(&window, Qt::LeftButton, Qt::NoModifier,
                      elementTabCenter);
    QTRY_COMPARE(panel->property("activeTab").toInt(), 1);
    QVERIFY(!scenePage->isVisible());
    QVERIFY(elementPage->isVisible());

    window.resize(640, 800);
    harness->setSize(window.size());
    QTest::qWait(50); // Cocoa constrains the native window on scaled displays.
    const bool contentFits = panel->property("desiredHeight").toReal()
        <= panel->property("maximumHeight").toReal();
    QTRY_COMPARE(contentFlick->property("overflowing").toBool(), !contentFits);
    if (contentFits) {
        QVERIFY(qAbs(contentFlick->property("contentHeight").toReal()
                     - contentFlick->height()) <= 0.5);
        QVERIFY(!scrollBar->isVisible());
    } else {
        QVERIFY(contentFlick->property("contentHeight").toReal() > contentFlick->height());
        QVERIFY(scrollBar->isVisible());
    }

    auto* textSection = findVisualItem(
        harness.get(), QStringLiteral("textSettingsSection"));
    auto* opacityCheck = findVisualItem(
        harness.get(), QStringLiteral("opacityCheck"));
    auto* opacityField = findVisualItem(
        harness.get(), QStringLiteral("opacityField"));
    auto* textBorderWidthCheck = findVisualItem(
        harness.get(), QStringLiteral("textBorderWidthCheck"));
    auto* textBorderWidthField = findVisualItem(
        harness.get(), QStringLiteral("textBorderWidthField"));
    auto* elementAudioSection = findVisualItem(
        harness.get(), QStringLiteral("elementAudioSection"));
    QVERIFY(textSection);
    QVERIFY(textSection->isVisible());
    QVERIFY(elementAudioSection);
    QVERIFY(!elementAudioSection->isVisible());
    QVERIFY(opacityCheck);
    QVERIFY(opacityField);
    QVERIFY(textBorderWidthCheck);
    QVERIFY(textBorderWidthField);
    if (contentFits) QTRY_COMPARE(scrollBar->opacity(), 0.0);
    QVERIFY(!opacityField->property("cursorVisible").isValid());
    QCOMPARE(opacityCheck->property("checkedColor").value<QColor>(),
             QColor(QStringLiteral("#4a90e2")));

    const QPoint opacityCenter = opacityCheck->mapToScene(
        QPointF(opacityCheck->width() / 2.0,
                opacityCheck->height() / 2.0)).toPoint();
    QTest::mouseClick(&window, Qt::LeftButton, Qt::NoModifier,
                      opacityCenter);
    QTRY_VERIFY(media->settings().opacityOverrideEnabled);

    const QPoint opacityFieldCenter = opacityField->mapToScene(
        QPointF(opacityField->width() / 2.0,
                opacityField->height() / 2.0)).toPoint();
    const int canvasPressesBeforeInput =
        harness->property("canvasPrimaryPressCount").toInt();
    QCOMPARE(canvasPressesBeforeInput, 0);
    QTest::mouseClick(&window, Qt::LeftButton, Qt::NoModifier,
                      opacityFieldCenter);
    QCOMPARE(harness->property("canvasPrimaryPressCount").toInt(),
             canvasPressesBeforeInput);
    QCOMPARE(host->document()->selectedMedia(), media);
    QTest::keyClick(&window, Qt::Key_7);
    QTest::keyClick(&window, Qt::Key_5);
    QCOMPARE(opacityField->property("draftText").toString(),
             QStringLiteral("75"));
    QCOMPARE(media->settings().opacityText, QStringLiteral("100"));
    QTest::keyClick(&window, Qt::Key_Return);
    QTRY_COMPARE(media->settings().opacityText, QStringLiteral("75"));

    QTest::mouseClick(&window, Qt::LeftButton, Qt::NoModifier,
                      opacityFieldCenter);
    QTest::keyClick(&window, Qt::Key_2);
    QTest::keyClick(&window, Qt::Key_5);
    QTest::keyClick(&window, Qt::Key_0);
    QTest::keyClick(&window, Qt::Key_Return);
    QTRY_COMPARE(media->settings().opacityText, QStringLiteral("100"));

    QVERIFY(QMetaObject::invokeMethod(textBorderWidthCheck, "click"));
    QTRY_VERIFY(media->outlineWidthOverrideEnabled());
    QVERIFY(QMetaObject::invokeMethod(textBorderWidthField, "activate"));
    QTRY_VERIFY(textBorderWidthField->hasActiveFocus());
    QTest::keyClick(&window, Qt::Key_1);
    QTest::keyClick(&window, Qt::Key_0);
    QTest::keyClick(&window, Qt::Key_0);
    QTest::keyClick(&window, Qt::Key_Return);
    QTRY_COMPARE(media->outlineWidthPercent(), 100.0);
    QVERIFY(media->toModelMap().value(
                QStringLiteral("textOutlineWidthPx")).toDouble() > 0.0);

    QVERIFY(QMetaObject::invokeMethod(textBorderWidthCheck, "click"));
    QTRY_VERIFY(!media->outlineWidthOverrideEnabled());
    QCOMPARE(media->outlineWidthPercent(), 100.0);
    QTRY_COMPARE(textBorderWidthField->property("draftText").toString(),
                 QStringLiteral("100"));
    QCOMPARE(media->toModelMap().value(
                 QStringLiteral("textOutlineWidthPx")).toDouble(), 0.0);
    QCOMPARE(host->document()->selectedMedia(), media);

    session.setSettingsVisible(false);
    QTRY_VERIFY(!panel->isVisible());
}

void MediaOverlayTest::videoVolumeAndMuteStayIndependentAndSyncWithSettings()
{
    const QString overridePath = qEnvironmentVariable("MOUFFETTE_TEST_VIDEO_FILE");
    const QString videoPath = overridePath.isEmpty()
        ? QFINDTESTDATA("../fixtures/resident-timeline.mp4") : overridePath;
    QVERIFY(!videoPath.isEmpty());
    QString error;
    std::unique_ptr<QuickCanvasHost> host(QuickCanvasHost::create(&error));
    QVERIFY2(host, qPrintable(error));
    host->setProjectEditingEnabled(true);
    ClientWorkspaceViewModel session(
        QStringLiteral("audio-settings"), host.get(), [] {}, nullptr,
        [] { return false; }, [] { return true; }, [] { return true; });
    session.setLoading(false);
    session.setSettingsVisible(true);

    QQmlEngine engine;
    QQuickWindow window;
    window.resize(1100, 800);
    QQmlComponent component(&engine, QUrl(QStringLiteral(
        "qrc:/qt/qml/Mouffette/App/resources/qml/app/pages/CanvasPage.qml")));
    std::unique_ptr<QObject> pageObject(component.createWithInitialProperties({
        {QStringLiteral("controller"), QVariantMap{
             {QStringLiteral("activeWorkspace"), QVariant::fromValue(&session)},
             {QStringLiteral("remoteBusy"), false}}}
    }));
    auto* page = qobject_cast<QQuickItem*>(pageObject.get());
    QVERIFY2(page, qPrintable(component.errorString()));
    page->setParentItem(window.contentItem());
    page->setSize(window.size());
    connect(&window, &QWindow::widthChanged, page, [&window, page] { page->setSize(window.size()); });
    connect(&window, &QWindow::heightChanged, page, [&window, page] { page->setSize(window.size()); });
    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));
    // Cocoa can constrain the requested size on scaled displays. Keep both
    // settings and transport controls inside the actual native window.
    page->setSize(window.size());
#ifdef Q_OS_MACOS
    MacWindowManager::activateApplicationWindow(&window);
#else
    window.requestActivate();
#endif
    QVERIFY(QTest::qWaitForWindowActive(&window));
    host->controller()->updateCamera(1.0, 0.0, 0.0);
    CanvasMedia* video = host->document()->addPreparedFile(
        videoPath, QSize(160, 90), true, QPointF(window.width() / 2, 30));
    QVERIFY(video);
    QTRY_VERIFY2_WITH_TIMEOUT(video->residencyReady(),
        qPrintable(video->residencyState() + ": " + video->residencyError()), 30000);
    auto* panel = findVisualItem(page, QStringLiteral("canvasSceneElementPanel"));
    QVERIFY(panel);
    panel->setProperty("activeTab", 1);
    QQuickItem* slider = nullptr;
    QTRY_VERIFY((slider = findVisualItem(page, QStringLiteral("videoVolumeSlider"))));
    auto* mute = findVisualItem(page, QStringLiteral("videoMuteButton"));
    auto* check = findVisualItem(page, QStringLiteral("volumeCheck"));
    auto* field = findVisualItem(page, QStringLiteral("volumeField"));
    QVERIFY(mute && check && field);
    auto* settingsFlick = findVisualItem(page, QStringLiteral("settingsContentFlick"));
    QVERIFY(settingsFlick);
    const auto revealSettingsControl = [&](QQuickItem* item) {
        bool inSettings = false;
        for (QQuickItem* parent = item->parentItem(); parent; parent = parent->parentItem())
            if (parent == settingsFlick) { inSettings = true; break; }
        if (!inSettings) return;
        const qreal center = item->mapToItem(settingsFlick, {0, item->height() / 2}).y();
        if (center >= item->height() / 2 && center <= settingsFlick->height() - item->height() / 2) return;
        const qreal maximum = qMax<qreal>(0, settingsFlick->property("contentHeight").toReal() - settingsFlick->height());
        settingsFlick->setProperty("contentY", qBound<qreal>(0,
            settingsFlick->property("contentY").toReal() + center - settingsFlick->height() / 2, maximum));
        QCoreApplication::processEvents();
    };
    const auto click = [&](QQuickItem* item) {
        revealSettingsControl(item);
        QTest::mouseClick(&window, Qt::LeftButton, Qt::NoModifier,
                         item->mapToScene({item->width() / 2, item->height() / 2}).toPoint());
    };
    QTRY_VERIFY(check->property("checked").toBool());

    // Editing the numeric field changes the actual audio and overlay slider.
    QTRY_VERIFY(field->isVisible());
    QTRY_VERIFY(panel->isEnabled());
    QTest::qWait(50); // Polish the newly selected settings tab before hit testing.
    revealSettingsControl(field);
    const QString artifactDir = qEnvironmentVariable("MOUFFETTE_OVERLAY_ARTIFACT_DIR");
    const auto capture = [&](const QString& name) {
        if (artifactDir.isEmpty()) return;
        QDir().mkpath(artifactDir);
        const QString scale = qEnvironmentVariable("QT_SCALE_FACTOR");
        window.grabWindow().save(QDir(artifactDir).filePath(name
            + (scale.isEmpty() ? QString() : "-scale-" + scale) + ".png"));
    };
    capture("video-volume-before-edit");
    click(field);
    QTRY_VERIFY(field->hasActiveFocus());
    QTest::keyClick(&window, Qt::Key_7);
    QTest::keyClick(&window, Qt::Key_0);
    QCOMPARE(field->property("draftText").toString(), QStringLiteral("70"));
    QTest::keyClick(&window, Qt::Key_Return);
    QTRY_VERIFY(qAbs(video->volume() - 0.7) < 0.001);
    QTRY_VERIFY(qAbs(slider->property("progress").toReal() - 0.7) < 0.001);
    click(mute);
    capture("video-volume-after-mute");
    QTRY_VERIFY(video->muted());
    QTRY_VERIFY(!check->property("checked").toBool());
    QCOMPARE(field->property("draftText").toString(), QStringLiteral("70"));
    QVERIFY(qAbs(slider->property("progress").toReal() - 0.7) < 0.001);

    // Both ways of changing volume remain available while muted.
    QTest::mouseClick(&window, Qt::LeftButton, Qt::NoModifier,
                     slider->mapToScene({slider->width() * 0.4, slider->height() / 2}).toPoint());
    QTRY_VERIFY(qAbs(video->volume() - 0.4) < 0.03);
    QVERIFY(video->muted());
    QTRY_COMPARE(field->property("draftText").toString(),
                 QString::number(qRound(video->volume() * 100)));
    click(field);
    QTest::keyClick(&window, Qt::Key_6);
    QTest::keyClick(&window, Qt::Key_5);
    QTest::keyClick(&window, Qt::Key_Return);
    QTRY_VERIFY(qAbs(slider->property("progress").toReal() - 0.65) < 0.001);
    QVERIFY(video->muted());
    QVERIFY(!check->property("checked").toBool());

    click(check);
    QTRY_VERIFY(!video->muted());
    QTRY_VERIFY(!mute->property("toggled").toBool());
    QVERIFY(qAbs(video->volume() - 0.65) < 0.001);
    click(check);
    QTRY_VERIFY(video->muted());
    QTRY_VERIFY(mute->property("toggled").toBool());
    QCOMPARE(field->property("draftText").toString(), QStringLiteral("65"));
    click(mute);
    QTRY_VERIFY(!video->muted());
    QTRY_VERIFY(check->property("checked").toBool());

    // Zero volume is independent too: it must not toggle mute or the checkbox.
    host->controller()->handleOverlayVolumeChange(video->mediaId(), 0.0);
    QCOMPARE(slider->property("progress").toReal(), 0.0);
    QTRY_COMPARE(field->property("draftText").toString(), QStringLiteral("0"));
    QVERIFY(!video->muted());
    QVERIFY(check->property("checked").toBool());
    QTRY_COMPARE(slider->property("progress").toReal(), 0.0);
    QTRY_COMPARE(slider->property("_visualValue").toReal(), 0.0);

    // Keep the grab beyond both ends, then release outside the control.
    const auto dragOutside = [&](QQuickItem* control, bool right) {
        const QPoint start = control->mapToScene({control->width() * .5, control->height() * .5}).toPoint();
        const QPoint edge = control->mapToScene({right ? control->width() : 0, control->height() * .5}).toPoint();
        const QPoint outside = edge + QPoint(right ? 60 : -60, 0);
        QTest::mousePress(&window, Qt::LeftButton, Qt::NoModifier, start);
        QTest::mouseMove(&window, edge, 20);
        QTest::mouseMove(&window, outside, 20);
        QTest::mouseRelease(&window, Qt::LeftButton, Qt::NoModifier, outside);
    };
    for (bool right : {true, false}) {
        dragOutside(slider, right);
        QTRY_COMPARE(video->volume(), right ? 1.0 : 0.0);
        QTRY_COMPARE(slider->property("_visualValue").toReal(), right ? 1.0 : 0.0);
        QVERIFY(!slider->property("_dragging").toBool());
        QTest::qWait(120); // Authoritative playback publication must retain the endpoint.
        QCOMPARE(slider->property("progress").toReal(), right ? 1.0 : 0.0);
    }

    auto* progress = findVisualItem(page, QStringLiteral("videoProgressSlider"));
    QVERIFY(progress);
    video->stopToBeginning();
    QTRY_VERIFY(video->player()->duration() > 0);
    QSignalSpy seeks(host->controller(), &QuickCanvasController::mediaSeekRequested);
    for (bool right : {true, false}) {
        dragOutside(progress, right);
        QVERIFY(!progress->property("_dragging").toBool());
        QCOMPARE(seeks.last().at(1).toReal(), right ? 1.0 : 0.0);
        QTRY_VERIFY(qAbs(progress->property("_visualValue").toReal() - (right ? 1.0 : 0.0)) < .01);
        QTRY_VERIFY(qAbs(video->positionMs() - (right ? video->player()->duration() : 0)) < 100);
    }

    auto* startButton = findVisualItem(page, QStringLiteral("videoStartButton"));
    auto* endButton = findVisualItem(page, QStringLiteral("videoEndButton"));
    QVERIFY(startButton && endButton);
    video->setPositionMs(1200);
    QTRY_VERIFY(startButton->isEnabled() && endButton->isEnabled());
    click(startButton);
    QTRY_COMPARE(video->startMarkerMs(), 1200);
    QCOMPARE(startButton->property("text").toString(), QStringLiteral("Remove start"));
    QVERIFY(endButton->isEnabled());
    click(endButton);
    QCOMPARE(video->endMarkerMs(), -1);
    video->setPositionMs(2400);
    QTRY_VERIFY(endButton->isEnabled());
    click(endButton);
    QTRY_COMPARE(video->endMarkerMs(), 2400);
    auto* startMarker = findVisualItem(page, QStringLiteral("videoStartMarker"));
    auto* endMarker = findVisualItem(page, QStringLiteral("videoEndMarker"));
    QVERIFY(startMarker && endMarker);
    QTRY_VERIFY(startMarker->isVisible() && endMarker->isVisible());
    QTRY_VERIFY(qAbs(startMarker->x() - progress->width() * 1200 / video->player()->duration()) < 1);
    QTRY_VERIFY(qAbs(endMarker->x() - progress->width() * 2400 / video->player()->duration()) < 1);
    click(startButton);
    QTRY_VERIFY(!startMarker->isVisible());
    video->setPositionMs(2500);
    QTRY_VERIFY(startButton->isEnabled());
    click(startButton);
    QCOMPARE(video->startMarkerMs(), -1);
    click(endButton);
    QTRY_VERIFY(!endMarker->isVisible());
    QTRY_VERIFY(startButton->isEnabled());

}

void MediaOverlayTest::toastUsesBottomLeftDoubleBackground()
{
    QQmlEngine engine;
    QQuickWindow window;
    window.setColor(Qt::magenta);
    window.resize(640, 480);
    QString error;
    std::unique_ptr<QQuickItem> harness(
        createToastHarness(engine, window, &error));
    QVERIFY2(harness, qPrintable(error));

    auto* base = findVisualItem(harness.get(), QStringLiteral("toastBase_0"));
    auto* tint = findVisualItem(harness.get(), QStringLiteral("toastTint_0"));
    auto* textItem = findVisualItem(harness.get(), QStringLiteral("toastText_0"));
    QVERIFY(base);
    QVERIFY(tint);
    QVERIFY(textItem);

    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));
    QTRY_VERIFY(base->opacity() > 0.99);
    const QPointF origin = base->mapToItem(window.contentItem(), QPointF());
    QCOMPARE(qRound(origin.x()), 40);
    QCOMPARE(qRound(origin.y() + base->height()), 440);

    const QColor baseColor = base->property("color").value<QColor>();
    const QColor tintColor = tint->property("color").value<QColor>();
    const QColor textColor = textItem->property("color").value<QColor>();
    QCOMPARE(baseColor, QGuiApplication::palette().color(QPalette::Active,
                                                         QPalette::Base));
    QCOMPARE(baseColor.alpha(), 255);
    QCOMPARE(tintColor.alpha(), 38);
    QCOMPARE(textColor, QColor(QStringLiteral("#4c9b50")));
}

void MediaOverlayTest::themeTracksApplicationPalette()
{
    const QPalette original = QGuiApplication::palette();
    const auto restorePalette = qScopeGuard([original]() {
        QGuiApplication::setPalette(original);
    });

    QPalette light = original;
    light.setColor(QPalette::Active, QPalette::Base, QColor("#f4f5f6"));
    light.setColor(QPalette::Active, QPalette::Text, QColor("#111213"));
    QGuiApplication::setPalette(light);

    QQmlEngine engine;
    QQmlComponent component(&engine);
    component.setData(R"QML(
import QtQuick
import Mouffette.App
Rectangle { color: Theme.windowBackground }
)QML", QUrl(QStringLiteral("qrc:/ThemePaletteHarness.qml")));
    std::unique_ptr<QObject> surface(component.create());
    QVERIFY2(surface, qPrintable(component.errorString()));
    QTRY_COMPARE(surface->property("color").value<QColor>(), QColor("#f4f5f6"));

    QPalette dark = original;
    dark.setColor(QPalette::Active, QPalette::Base, QColor("#202124"));
    dark.setColor(QPalette::Active, QPalette::Text, QColor("#f1f3f4"));
    QGuiApplication::setPalette(dark);
    QTRY_COMPARE(surface->property("color").value<QColor>(), QColor("#202124"));
}

QTEST_MAIN(MediaOverlayTest)
#include "tst_MediaOverlay.moc"
