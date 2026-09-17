#include <QColor>
#include <QDir>
#include <QFileInfo>
#include <QImage>
#include <QMouseEvent>
#include <QMediaPlayer>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QQuickItem>
#include <QPointer>
#include <QQuickWindow>
#include <QScopeGuard>
#include <QTemporaryDir>
#include <QtTest>
#include <cmath>
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
    void stateLabelsReserveWidthBeforeTransitions();
    void mediaPanelWidthSurvivesActionAndUploadTransitions();
    void emptyScreenHintStaysBehindMediaAndCenteredInViewport();
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
    void scenePlaybackUnloadsEditorOverlays_data();
    void scenePlaybackUnloadsEditorOverlays();
    void mediaSettingsPanelRestoresTabsAndBindings_data();
    void mediaSettingsPanelRestoresTabsAndBindings();
    void videoVolumeAndMuteStayIndependentAndSyncWithSettings();
    void toastUsesBottomLeftDoubleBackground();
    void themeTracksApplicationPalette();
};

namespace {
QObject* applicationTheme(QQmlEngine& engine)
{
    return engine.singletonInstance<QObject*>("Mouffette.App", "Theme");
}

QColor themeColor(QQmlEngine& engine, const char* name)
{
    QObject* theme = applicationTheme(engine);
    return theme ? theme->property(name).value<QColor>() : QColor();
}

QColor compositeOver(const QColor& foreground, const QColor& background)
{
    const qreal alpha = foreground.alphaF();
    return QColor::fromRgbF(
        foreground.redF() * alpha + background.redF() * (1.0 - alpha),
        foreground.greenF() * alpha + background.greenF() * (1.0 - alpha),
        foreground.blueF() * alpha + background.blueF() * (1.0 - alpha));
}

qreal luminance(const QColor& color)
{
    const auto linear = [](qreal channel) {
        return channel <= 0.04045 ? channel / 12.92
                                : std::pow((channel + 0.055) / 1.055, 2.4);
    };
    return 0.2126 * linear(color.redF()) + 0.7152 * linear(color.greenF())
        + 0.0722 * linear(color.blueF());
}

qreal contrastRatio(const QColor& foreground, const QColor& background)
{
    const qreal text = luminance(compositeOver(foreground, background));
    const qreal surface = luminance(background);
    return (qMax(text, surface) + 0.05) / (qMin(text, surface) + 0.05);
}

QPalette testPalette(bool dark)
{
    QPalette palette = QGuiApplication::palette();
    const QColor background(dark ? "#202124" : "#f4f5f6");
    QColor foreground(dark ? "#f1f3f4" : "#111213");
    // Native macOS text colors may be translucent. UI surfaces and derived
    // muted text still need an opaque semantic color before tint composition.
    foreground.setAlphaF(0.9);
    for (auto group : {QPalette::Active, QPalette::Inactive}) {
        palette.setColor(group, QPalette::Base, background);
        palette.setColor(group, QPalette::Window, background);
        palette.setColor(group, QPalette::Text, foreground);
        palette.setColor(group, QPalette::WindowText, foreground);
        // Platform Mid is a bevel role, not a readable text color. Setting
        // it to Base reproduces the invisible empty-state text regression.
        palette.setColor(group, QPalette::Mid, background);
    }
    return palette;
}

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
    property int severityKind: 0
    onSeverityKindChanged: toastRows.setProperty(0, "severityKind", severityKind)
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
    QCOMPARE(foreground, themeColor(engine, "availableText"));
    QCOMPARE(foreground.alpha(), 255);
    QVERIFY(contrastRatio(foreground, compositeOver(background,
        themeColor(engine, "windowBackground"))) >= 4.5);
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

void MediaOverlayTest::stateLabelsReserveWidthBeforeTransitions()
{
    QQmlEngine engine;
    QQuickWindow window;
    std::unique_ptr<QQuickItem> card(createStatusCard(engine, window, true));
    QVERIFY(card);
    const qreal width = card->implicitWidth();
    for (const auto& status : {"AVAILABLE", "CONNECTED", "DISCONNECTED", "CONNECTING",
                               "RECONNECTING", "DISCONNECTING", "UNREACHABLE", "DEGRADED"}) {
        card->setProperty("statusText", status);
        for (const auto& volume : {"0%", "9%", "50%", "100%"}) {
            card->setProperty("auxiliaryText", volume);
            QCOMPARE(card->implicitWidth(), width);
        }
    }

    QQmlComponent component(&engine);
    component.setData(R"QML(
import QtQuick
import Mouffette.App
Item {
    AppButton {
        objectName: "button"
        text: "Enable"
        textVariants: ["Enable", "Disable"]
    }
    StateTextMetrics {
        objectName: "metrics"
        text: "i"
        textVariants: ["i", "WWW"]
        font.pixelSize: 12
    }
}
)QML", QUrl());
    std::unique_ptr<QObject> harness(component.create());
    QVERIFY2(harness, qPrintable(component.errorString()));
    auto* button = harness->findChild<QQuickItem*>("button");
    auto* metrics = harness->findChild<QObject*>("metrics");
    QVERIFY(button && metrics);
    const qreal buttonWidth = button->implicitWidth();
    button->setProperty("text", "Disable");
    QCOMPARE(button->implicitWidth(), buttonWidth);
    const qreal measured = metrics->property("maximumWidth").toReal();
    metrics->setProperty("text", "WWW");
    QCOMPARE(metrics->property("maximumWidth").toReal(), measured);
    QFont font = metrics->property("font").value<QFont>();
    font.setPixelSize(24);
    metrics->setProperty("font", font);
    QVERIFY(metrics->property("maximumWidth").toReal() > measured * 1.9);
}

void MediaOverlayTest::mediaPanelWidthSurvivesActionAndUploadTransitions()
{
    QQmlEngine engine;
    QQuickWindow window;
    window.resize(1200, 800);
    MediaListModel model;
    QString error;
    std::unique_ptr<QQuickItem> harness(createMediaPanelHarness(engine, window, &model, &error));
    QVERIFY2(harness, qPrintable(error));
    harness->setSize(window.size());
    auto* panel = harness->findChild<QQuickItem*>("mediaListPanel");
    auto* session = harness->property("session").value<QObject*>();
    QVERIFY(panel && session);
    QVariantMap row{{"rowKey", "image"}, {"mediaId", "image"}, {"displayName", "Image"}, {"mediaType", "image"},
                    {"width", 160}, {"height", 90}, {"sourceSizeBytes", 10},
                    {"uploadState", "not_uploaded"}, {"uploadProgress", 0}};
    model.updateFromList({row});
    harness->setProperty("testMediaCount", 10);
    QCoreApplication::processEvents();
    const qreal initialWidth = panel->implicitWidth();
    const qreal initialLeft = panel->x();
    auto* remote = findVisualItem(panel, "remoteSceneAction");
    auto* upload = findVisualItem(panel, "uploadAction");
    QVERIFY(remote && upload);
    const qreal remoteWidth = remote->implicitWidth();
    const qreal uploadWidth = upload->implicitWidth();
    for (const auto& label : {"Launch Remote Scene", "Launching Remote Scene...",
                              "Stop Remote Scene", "Stopping Remote Scene..."}) {
        session->setProperty("remoteSceneActionText", label);
        QCOMPARE(remote->implicitWidth(), remoteWidth);
        QCOMPARE(panel->implicitWidth(), initialWidth);
        QCOMPARE(panel->x(), initialLeft);
    }
    for (const auto& state : {"not_uploaded", "uploading", "uploaded"}) {
        row["uploadState"] = state;
        row["remoteCached"] = true;
        model.updateFromList({row});
        QCoreApplication::processEvents();
        QCOMPARE(panel->implicitWidth(), initialWidth);
    }
    for (const auto& label : {"Preparing…", "Uploading…", "Uploading (0/10) 0%",
                              "Uploading (10/10) 100%", "Finalizing…", "Cancelling…",
                              "Removing…", "Unload", "Upload"}) {
        const bool progress = QString::fromUtf8(label).startsWith("Uploading (");
        session->setProperty("uploadActionText", QString::fromUtf8(label));
        session->setProperty("uploadActionTone", progress ? 1 : 0);
        QCOMPARE(upload->implicitWidth(), uploadWidth);
        QCOMPARE(panel->implicitWidth(), initialWidth);
        QCOMPARE(panel->x(), initialLeft);
    }
}

void MediaOverlayTest::emptyScreenHintStaysBehindMediaAndCenteredInViewport()
{
    QString error;
    std::unique_ptr<QuickCanvasHost> host(QuickCanvasHost::create(&error));
    QVERIFY2(host, qPrintable(error));
    host->setProjectEditingEnabled(true);
    ClientWorkspaceViewModel session("empty-screen", host.get(), [] {}, nullptr,
                                    [] { return false; }, [] { return true; }, [] { return true; });
    session.setLoading(false);
    QQmlEngine engine;
    QQuickWindow window;
    window.resize(1000, 700);
    QQmlComponent component(&engine, QUrl(QStringLiteral(
        "qrc:/qt/qml/Mouffette/App/resources/qml/app/pages/CanvasPage.qml")));
    std::unique_ptr<QObject> pageObject(component.createWithInitialProperties({
        {"controller", QVariantMap{{"activeWorkspace", QVariant::fromValue(&session)}, {"remoteBusy", false}}}
    }));
    auto* page = qobject_cast<QQuickItem*>(pageObject.get());
    QVERIFY2(page, qPrintable(component.errorString()));
    page->setParentItem(window.contentItem());
    page->setSize(window.size());
    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));
    page->setSize(window.size());
    host->controller()->updateCamera(1.0, 0.0, 0.0);
    auto* hint = findVisualItem(page, "emptyScreenHint");
    QVERIFY(hint && hint->isVisible());
    const QPointF center = hint->mapToScene({hint->width() / 2, hint->height() / 2});
    // Qt's centered anchors align text to logical pixels (up to half a pixel
    // per axis when the text's implicit height is odd).
    QVERIFY(QLineF(center, QPointF(page->width() / 2, page->height() / 2)).length() <= 1.0);

    QTemporaryDir directory;
    const QString path = directory.filePath("foreground.png");
    QImage source(640, 120, QImage::Format_ARGB32_Premultiplied);
    source.fill(Qt::cyan);
    QVERIFY(source.save(path));
    auto* media = host->document()->addPreparedFile(path, source.size(), false, center - QPointF(320, 60));
    QVERIFY(media);
    media->setZ(-100000);
    host->document()->clearSelection();
    QTRY_VERIFY(media->residencyReady());
    QTRY_VERIFY(imagePixel(window.grabWindow(), window.size(), center) == QColor(Qt::cyan));
    const QImage frame = window.grabWindow();
    QVERIFY(!frame.isNull());
    for (int y = -15; y <= 15; ++y)
        for (int x = -220; x <= 220; ++x)
            QCOMPARE(imagePixel(frame, window.size(), center + QPointF(x, y)), QColor(Qt::cyan));

    host->controller()->panBy(70, -30);
    host->controller()->zoomAt(120, 160, 1.4);
    QCOMPARE(hint->mapToScene({hint->width() / 2, hint->height() / 2}), center);
    session.setLoading(true);
    QVERIFY(!hint->isVisible());
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
    window.requestActivate();
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
                 themeColor(engine, cached ? "mediaUploaded" : "mediaNotUploaded"));
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
    QTest::addColumn<bool>("dark");
    QTest::addColumn<int>("tone");
    QTest::addColumn<bool>("enabled");
    QTest::addColumn<bool>("busy");
    QTest::addColumn<QByteArray>("foregroundToken");
    QTest::addColumn<QByteArray>("idleToken");
    QTest::addColumn<QByteArray>("hoverToken");
    QTest::addColumn<QByteArray>("pressedToken");
    for (const bool dark : {false, true}) {
        const auto row = [dark](const char* name, int tone, bool enabled, bool busy,
                              const char* foreground, const char* idle,
                              const char* hover, const char* pressed) {
            QTest::newRow(qPrintable(QStringLiteral("%1-%2")
                .arg(dark ? "dark" : "light", name)))
                << dark << tone << enabled << busy << QByteArray(foreground)
                << QByteArray(idle) << QByteArray(hover) << QByteArray(pressed);
        };
        row("idle", 0, true, false, "overlayText", "transparent",
            "overlayHover", "overlayPressed");
        row("uploading", 1, true, true, "brandBlue", "primaryBackground",
            "primaryHover", "primaryPressed");
        row("awaiting-ack", 1, false, true, "brandBlue", "primaryBackground",
            "primaryBackground", "primaryBackground");
        row("unload", 2, true, false, "mediaUploaded", "connectedBackground",
            "overlayUploadedHover", "overlayUploadedPressed");
        for (int scene : {3, 4})
            row(scene == 3 ? "remote-active" : "test-active", scene, true, false,
                "overlaySceneText", "overlaySceneBackground", "overlaySceneHover",
                "overlayScenePressed");
        for (int tone : {0, 1, 2, 3, 4})
            row(qPrintable(QStringLiteral("disabled-%1").arg(tone)), tone, false, false,
                "overlayDisabledText", "overlayDisabledBackground",
                "overlayDisabledBackground", "overlayDisabledBackground");
    }
}

void MediaOverlayTest::mediaActionPalette()
{
    QFETCH(int, tone);
    QFETCH(bool, enabled);
    QFETCH(bool, busy);
    QFETCH(bool, dark);
    QFETCH(QByteArray, foregroundToken);
    QFETCH(QByteArray, idleToken);
    QFETCH(QByteArray, hoverToken);
    QFETCH(QByteArray, pressedToken);
    const QPalette original = QGuiApplication::palette();
    const auto restorePalette = qScopeGuard([original] { QGuiApplication::setPalette(original); });
    QGuiApplication::setPalette(testPalette(dark));
    QQmlEngine engine;
    const auto resolve = [&](const QByteArray& token) {
        return token == "transparent" ? QColor(Qt::transparent)
                                      : themeColor(engine, token.constData());
    };
    const QColor foreground = resolve(foregroundToken);
    const QColor idle = resolve(idleToken);
    const QColor hover = resolve(hoverToken);
    const QColor pressed = resolve(pressedToken);
    QVERIFY(foreground.isValid() && idle.isValid() && hover.isValid() && pressed.isValid());
    QQuickWindow window;
    window.resize(360, 120);
    window.setColor(themeColor(engine, "overlayBackground"));
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
    window.requestActivate();
    QVERIFY(QTest::qWaitForWindowActive(&window));
    QTest::mouseMove(&window, {5, 5});
    QCOMPARE(button->property("foregroundColor").value<QColor>(), foreground);
    auto verifyFill = [&](const QColor& fill) {
        const QImage frame = window.grabWindow();
        return !frame.isNull() && nearColor(imagePixel(frame, window.size(), {25, 25}),
                                            compositeOver(fill, window.color()));
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
    // Cocoa can constrain the first window size on a scaled display. Keep
    // the animated rows and click targets inside the actual visible surface.
    harness->setSize(window.size());
    window.requestActivate();
    QVERIFY(QTest::qWaitForWindowActive(&window));
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
    QCOMPARE(fill->property("color").value<QColor>(), themeColor(engine, "mediaProgress"));
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
    // Verify both pulse extrema, allowing two full 1400 ms cycles for native
    // compositor scheduling at either scale factor. A frozen pulse still fails.
    QTRY_VERIFY_WITH_TIMEOUT(fill->opacity() < 0.6, 3000);
    if (!artifactDir.isEmpty())
        QVERIFY(window.grabWindow().save(QDir(artifactDir).filePath(QStringLiteral("media-overlay-caching.png"))));
    QTRY_VERIFY_WITH_TIMEOUT(fill->opacity() > 0.95, 3000);
    QCOMPARE(row->height(), originalHeight);
    host->document()->select(photo->mediaId());
    QTRY_VERIFY(row->property("selected").toBool());
    const QPoint clickPoint = textRow->mapToScene({textRow->width() / 2, textRow->height() / 2}).toPoint();
    QVERIFY(QRect(QPoint(), window.size()).contains(clickPoint));
    QTest::mouseClick(&window, Qt::LeftButton, Qt::NoModifier, clickPoint);
    QTRY_COMPARE(host->document()->selectedMedia(), text);
    QVERIFY(!row->property("selected").toBool());
    text->setZ(20);
    QTRY_COMPARE(findVisualItem(panel, QStringLiteral("mediaRow_0"))->property("mediaId").toString(), text->mediaId());
    harness->setWidth(900);
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
             themeColor(engine, "overlayPressed"));
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
    QVERIFY(window->flags().testFlag(Qt::WindowTitleHint));
    QVERIFY(window->flags().testFlag(Qt::WindowStaysOnTopHint));
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
    window.requestActivate();
    QVERIFY(QTest::qWaitForWindowActive(&window));
    // The shared edge has one logical pixel of border, with a button fill on
    // either side. Sampling both neighbors catches adjacent duplicate lines.
    const QPointF joint = text->mapToScene({0, text->height() / 2});
    const QImage toolbarFrame = window.grabWindow();
    QVERIFY(nearColor(imagePixel(toolbarFrame, window.size(), joint + QPointF(-0.9, 0)),
                      selection->property("currentBackgroundColor").value<QColor>()));
    QVERIFY(nearColor(imagePixel(toolbarFrame, window.size(), joint + QPointF(0.1, 0)),
                      themeColor(engine, "overlayBorder")));
    QVERIFY(nearColor(imagePixel(toolbarFrame, window.size(), joint + QPointF(1.1, 0)),
                      text->property("currentBackgroundColor").value<QColor>()));
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
    auto* topBar = findVisualItem(appWindow->contentItem(), QStringLiteral("topBar"));
    auto* localStatus = findVisualItem(appWindow->contentItem(), QStringLiteral("localConnectionStatus"));
    QVERIFY(topBar && localStatus);
    appWindow->resize(900, 650);
    appWindow->showNormal();
    QVERIFY(QTest::qWaitForWindowExposed(appWindow));
    appWindow->requestActivate();
    QVERIFY(QTest::qWaitForWindowActive(appWindow));
    // First exposure can fit the window to a smaller screen at high DPR.
    // Establish the same wide size used below and wait for layout before
    // recording its position and clicking the button.
    appWindow->resize(900, 650);
    QCOMPARE(appWindow->width(), 900);
    QTRY_COMPARE(localStatus->y(), 0.0);
    QTRY_COMPARE(memory->mapToScene({0, 0}).y(), topBar->mapToScene({0, 0}).y());
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
    QTRY_VERIFY(localStatus->y() >= memory->height());
    QTRY_COMPARE(localStatus->width(), topBar->width());
    QTRY_COMPARE(memory->mapToScene({0, 0}).y(), toolbarY);
    QTRY_VERIFY(memory->mapToScene({memory->width(), 0}).x() <= appWindow->width());
    QVERIFY(memory->isVisible() && memory->width() >= memory->implicitWidth());
    for (const auto* name : {"connectionButton", "historyButton", "settingsButton"}) {
        auto* button = findVisualItem(topBar, QString::fromLatin1(name));
        QVERIFY(button && button->isVisible());
        QCOMPARE(button->mapToScene({0, 0}).y(), toolbarY);
        QVERIFY(button->mapToScene({0, 0}).x() >= topBar->x());
        QVERIFY(button->mapToScene({button->width(), 0}).x() <= appWindow->width());
    }
    if (!artifactDir.isEmpty()) {
        QSignalSpy frames(appWindow, &QQuickWindow::frameSwapped);
        appWindow->update();
        QTRY_VERIFY(frames.size() > 0);
        const QString scale = qEnvironmentVariable("QT_SCALE_FACTOR");
        const QString name = scale.isEmpty() ? "topbar-clients-narrow.png"
            : "topbar-clients-narrow-scale-" + scale + ".png";
        QVERIFY(appWindow->grabWindow().save(QDir(artifactDir).filePath(name)));
    }
    QTest::mouseClick(appWindow, Qt::LeftButton, Qt::NoModifier,
                     memory->mapToScene({memory->width() / 2, memory->height() / 2}).toPoint());
    QTRY_VERIFY(memory->property("checked").toBool());
    QTest::keyClick(appWindow, Qt::Key_Escape);
    QTRY_VERIFY(!memory->property("checked").toBool());
    appWindow->resize(900, 650);
    QTRY_COMPARE(localStatus->y(), 0.0);
    QTRY_COMPARE(memory->mapToScene({0, 0}).y(), toolbarY);
}

void MediaOverlayTest::scenePlaybackUnloadsEditorOverlays_data()
{
    QTest::addColumn<bool>("launchTestScene");
    QTest::addColumn<bool>("videoSelected");
    QTest::newRow("test-scene-text") << true << false;
    QTest::newRow("test-scene-video") << true << true;
    // Remote preparation/running/stopping hold this same document lock.
    QTest::newRow("remote-scene-lock-text") << false << false;
    QTest::newRow("remote-scene-lock-video") << false << true;
}

void MediaOverlayTest::scenePlaybackUnloadsEditorOverlays()
{
    QFETCH(bool, launchTestScene);
    QFETCH(bool, videoSelected);
    QString error;
    std::unique_ptr<QuickCanvasHost> host(QuickCanvasHost::create(&error));
    QVERIFY2(host, qPrintable(error));
    host->setProjectEditingEnabled(true);
    ClientWorkspaceViewModel session(
        QStringLiteral("scene-overlays"), host.get(), [] {}, nullptr,
        [] { return false; }, [] { return true; }, [] { return true; });
    session.setLoading(false);
    session.setSettingsVisible(true);

    QQmlEngine engine;
    QQuickWindow window;
    window.resize(900, 650);
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
    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));
    host->controller()->updateCamera(1.0, 0.0, 0.0);

    CanvasMedia* media = nullptr;
    if (videoSelected) {
        const QString overridePath = qEnvironmentVariable("MOUFFETTE_TEST_VIDEO_FILE");
        const QString videoPath = overridePath.isEmpty()
            ? QFINDTESTDATA("../fixtures/resident-timeline.mp4") : overridePath;
        QVERIFY(!videoPath.isEmpty());
        media = host->document()->addPreparedFile(
            videoPath, QSize(160, 90), true, QPointF(360, 260));
        QVERIFY(media);
        QTRY_VERIFY2_WITH_TIMEOUT(media->residencyReady(),
            qPrintable(media->residencyState() + ": " + media->residencyError()), 30000);
    } else {
        media = host->document()->addText(QPointF(360, 260), QStringLiteral("Scene"));
        QVERIFY(media);
    }
    host->controller()->selectMedia(media->mediaId());
    const QStringList editorNames{
        QStringLiteral("canvasSettingsButton"),
        QStringLiteral("canvasSelectionToolButton"),
        QStringLiteral("canvasTextToolButton"),
        QStringLiteral("canvasSceneElementPanel"),
        QStringLiteral("canvasSelectionChrome"),
        QStringLiteral("canvasSnapGuides"),
        QStringLiteral("canvasMediaOverlays"),
        QStringLiteral("mediaTopOverlay"),
        QStringLiteral("mediaVideoOverlay"),
        QStringLiteral("mediaTextOverlay"),
        QStringLiteral("videoProgressSlider"),
        QStringLiteral("videoVolumeSlider")};
    QList<QPointer<QQuickItem>> previousControls;
    for (const auto& name : editorNames) {
        QQuickItem* item = nullptr;
        QTRY_VERIFY2((item = findVisualItem(page, name)), qPrintable(name));
        previousControls.append(item);
    }
    // The remote pointer conveys presence, so it survives the editing lock
    // even though interactive editor controls are destroyed during playback.
    host->setScreens({ScreenInfo(0, 1920, 1080, 0, 0, true)});
    host->updateRemoteCursor(0, {200, 150});
    QPointer<QQuickItem> remoteCursor = findVisualItem(
        page, QStringLiteral("canvasRemoteCursor"));
    QVERIFY(remoteCursor && remoteCursor->isVisible());
    auto* panel = findVisualItem(page, QStringLiteral("canvasSceneElementPanel"));
    panel->setProperty("activeTab", 1);
    QPointer<QQuickItem> mediaList = findVisualItem(page, QStringLiteral("mediaListPanel"));
    QVERIFY(mediaList);
    QVERIFY(mediaList->isVisible());
    if (videoSelected) {
        QTRY_VERIFY_WITH_TIMEOUT(findVisualItem(page, QStringLiteral("videoProgressSlider"))->isVisible(), 8000);
    }

    if (launchTestScene) {
        QTRY_VERIFY(host->testSceneActionEnabled());
        host->triggerTestSceneAction();
        QVERIFY(host->testSceneLaunched());
    } else {
        host->document()->setEditsLocked(true);
    }
    QVERIFY(!host->controller()->editingEnabled());
    // Checking object destruction (rather than visible/enabled) catches the
    // old disabled-but-rendered controls and stale accessibility subtrees.
    for (const auto& item : previousControls)
        QTRY_VERIFY(item.isNull());
    for (const auto& name : editorNames)
        QVERIFY2(!findVisualItem(page, name), qPrintable(name));
    QVERIFY(remoteCursor && remoteCursor->isVisible());
    QCOMPARE(findVisualItem(page, QStringLiteral("canvasRemoteCursor")), remoteCursor.data());
    QVERIFY(mediaList);
    QVERIFY(mediaList->isVisible());
    CanvasMedia* selectionBeforeInput = host->document()->selectedMedia();

    // Panning/keyboard input still traverses CanvasRoot while chrome is absent.
    QTest::keyRelease(&window, Qt::Key_Shift);
    QTest::mouseClick(&window, Qt::LeftButton, Qt::NoModifier, QPoint(30, 300));
    QCOMPARE(host->document()->selectedMedia(), selectionBeforeInput);

    if (launchTestScene)
        host->triggerTestSceneAction();
    else
        host->document()->setEditsLocked(false);
    QTRY_VERIFY(host->controller()->editingEnabled());
    host->controller()->selectMedia(media->mediaId());
    for (const auto& name : editorNames)
        QTRY_VERIFY2(findVisualItem(page, name), qPrintable(name));
    QVERIFY(remoteCursor && remoteCursor->isVisible());
    QCOMPARE(findVisualItem(page, QStringLiteral("canvasSceneElementPanel"))
                 ->property("activeTab").toInt(), 1);
    QCOMPARE(findVisualItem(page, QStringLiteral("mediaListPanel")), mediaList.data());
}

void MediaOverlayTest::mediaSettingsPanelRestoresTabsAndBindings_data()
{
    QTest::addColumn<bool>("dark");
    QTest::newRow("light") << false;
    QTest::newRow("dark") << true;
}

void MediaOverlayTest::mediaSettingsPanelRestoresTabsAndBindings()
{
    QFETCH(bool, dark);
    const QPalette original = QGuiApplication::palette();
    const auto restorePalette = qScopeGuard([original] { QGuiApplication::setPalette(original); });
    QGuiApplication::setPalette(testPalette(dark));
    QQmlEngine engine;
    QQuickWindow window;
    window.setColor(Qt::magenta);
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
    window.requestActivate();
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

    // Hovered/pressed tab backgrounds must not paint into either rounded
    // outer corner. A contrasting canvas reveals any rectangular overflow.
    for (auto* tab : {sceneTab, elementTab}) {
        const QPoint center = tab->mapToScene({tab->width() / 2, tab->height() / 2}).toPoint();
        QTest::mouseMove(&window, center);
        for (const bool pressed : {false, true}) {
            if (pressed) QTest::mousePress(&window, Qt::LeftButton, Qt::NoModifier, center);
            const QImage frame = window.grabWindow();
            QVERIFY(nearColor(imagePixel(frame, window.size(), panel->mapToScene({1.1, 1.1})), window.color()));
            QVERIFY(nearColor(imagePixel(frame, window.size(), panel->mapToScene({panel->width() - 2.1, 1.1})), window.color()));
            if (pressed) QTest::mouseRelease(&window, Qt::LeftButton, Qt::NoModifier, center);
        }
    }
    panel->setProperty("activeTab", 0);
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
             themeColor(engine, "controlSelectionBackground"));

    const QPoint opacityCenter = opacityCheck->mapToScene(
        QPointF(opacityCheck->width() / 2.0,
                opacityCheck->height() / 2.0)).toPoint();
    QTest::mouseClick(&window, Qt::LeftButton, Qt::NoModifier,
                      opacityCenter);
    QTRY_VERIFY(media->settings().opacityOverrideEnabled);
    auto* indicator = opacityCheck->property("indicator").value<QQuickItem*>();
    QVERIFY(indicator && !indicator->childItems().isEmpty());
    QVERIFY(contrastRatio(indicator->childItems().first()->property("color").value<QColor>(),
                          indicator->property("color").value<QColor>()) >= 4.5);

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
    QVERIFY(opacityField->hasActiveFocus());
    auto* valueBackground = findVisualItem(opacityField, QStringLiteral("settingsValueBackground"));
    auto* valueText = findVisualItem(opacityField, QStringLiteral("settingsValueText"));
    QVERIFY(valueBackground && valueText);
    QVERIFY(contrastRatio(valueText->property("color").value<QColor>(),
                          valueBackground->property("color").value<QColor>()) >= 4.5);
    QCOMPARE(valueBackground->property("color").value<QColor>(), indicator->property("color").value<QColor>());
    const QString artifactDir = qEnvironmentVariable("MOUFFETTE_OVERLAY_ARTIFACT_DIR");
    if (!artifactDir.isEmpty()) {
        QVERIFY(QDir().mkpath(artifactDir));
        QVERIFY(window.grabWindow().save(QDir(artifactDir).filePath(
            dark ? QStringLiteral("settings-focused-dark.png") : QStringLiteral("settings-focused-light.png"))));
    }
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
    window.requestActivate();
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
    // Residency can precede asynchronous player/output preparation. Clicking
    // the still-hidden transport hits the canvas and clears the selection.
    QTRY_VERIFY_WITH_TIMEOUT(mute->isVisible() && mute->isEnabled(), 8000);
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
    // Native window managers can clamp the requested size at larger scale
    // factors. Keep the standalone harness anchored to its actual viewport.
    harness->setSize(window.size());
    QTRY_VERIFY(base->opacity() > 0.99);
    const QPointF origin = base->mapToItem(window.contentItem(), QPointF());
    QCOMPARE(qRound(origin.x()), 40);
    QCOMPARE(qRound(origin.y() + base->height()), window.height() - 40);

    const QPalette original = QGuiApplication::palette();
    const auto restorePalette = qScopeGuard([original] { QGuiApplication::setPalette(original); });
    const char* foregroundTokens[] = {"connectedText", "errorText", "warningText", "brandBlue"};
    const char* tintTokens[] = {"connectedBackground", "errorBackground", "warningBackground", "brandBlueLight"};
    for (const bool dark : {false, true, false}) {
        QGuiApplication::setPalette(testPalette(dark));
        QTRY_COMPARE(base->property("color").value<QColor>(),
                     QGuiApplication::palette().color(QPalette::Active, QPalette::Base));
        for (int severity = 0; severity < 4; ++severity) {
            harness->setProperty("severityKind", severity);
            QTRY_COMPARE(textItem->property("color").value<QColor>(),
                         themeColor(engine, foregroundTokens[severity]));
            const QColor baseColor = base->property("color").value<QColor>();
            const QColor tintColor = tint->property("color").value<QColor>();
            const QColor textColor = textItem->property("color").value<QColor>();
            QCOMPARE(baseColor.alpha(), 255);
            QCOMPARE(tintColor, themeColor(engine, tintTokens[severity]));
            QCOMPARE(tintColor.alpha(), 38);
            const QColor composite = compositeOver(tintColor, baseColor);
            QVERIFY2(contrastRatio(textColor, composite) >= 4.5,
                     "Every toast severity must remain readable in both themes");
            const QPointF sample = base->mapToScene({base->width() / 2, base->height() - 4});
            QColor rendered;
            const auto matchesComposite = [&] {
                rendered = imagePixel(window.grabWindow(), window.size(), sample);
                return nearColor(rendered, composite);
            };
            QTRY_VERIFY2(matchesComposite(), qPrintable(QStringLiteral(
                "Toast severity %1, dark=%2: rendered %3, expected %4 at %5,%6")
                .arg(severity).arg(dark).arg(rendered.name()).arg(composite.name())
                .arg(sample.x()).arg(sample.y())));
        }
    }
}

void MediaOverlayTest::themeTracksApplicationPalette()
{
    const QPalette original = QGuiApplication::palette();
    const auto restorePalette = qScopeGuard([original] { QGuiApplication::setPalette(original); });
    QGuiApplication::setPalette(testPalette(false));
    QQmlEngine engine;
    QQuickWindow window;
    window.resize(960, 640);
    QQmlComponent component(&engine);
    component.setData(R"QML(
import QtQuick
import Mouffette.App
Rectangle {
    color: Theme.windowBackground
    Text {
        x: 20; y: 16
        text: "Mouffette"
        color: Theme.text
        font.pixelSize: 22
        font.bold: true
    }
    ClientListPanel {
        objectName: "emptyClients"
        x: 20; y: 56; width: 360; height: 155
        emptyText: "No clients connected. Make sure other devices are running Mouffette and connected to the same server."
    }
    ClientListPanel {
        objectName: "emptyScenes"
        x: 20; y: 227; width: 360; height: 155
        sceneMode: true
        emptyText: "No current ongoing scenes."
    }
    SegmentedStatusCard {
        objectName: "themeStatus"
        x: 20; y: 400
        primaryText: "You"
        statusText: "CONNECTED"
        statusKind: 0
    }
    CanvasRoot {
        objectName: "themeCanvas"
        x: 400; y: 56; width: 540; height: 484
        screensModel: [{ x: 50, y: 125, width: 440, height: 247,
                         primary: true, screenId: 1, displayIndex: 1,
                         pixelWidth: 1920, pixelHeight: 1080 }]
    }
    OverlayButton {
        objectName: "themeOverlay"
        x: 420; y: 76
        iconSource: "qrc:/icons/icons/arrow-up.svg"
    }
    OverlayButton {
        x: 462; y: 76
        iconSource: "qrc:/icons/icons/arrow-down.svg"
        isToggle: true
        toggled: true
    }
    OverlayButton {
        x: 504; y: 76
        iconSource: "qrc:/icons/icons/delete.svg"
        enabled: false
    }
    MediaNamePill {
        x: 570; y: 76; width: 180; height: 36
        displayName: "Selected media"
    }
    ListModel {
        id: toasts
        ListElement { severityKind: 0; message: "Connected"; dismissing: false }
        ListElement { severityKind: 1; message: "Connection lost"; dismissing: false }
        ListElement { severityKind: 2; message: "Reconnecting"; dismissing: false }
        ListElement { severityKind: 3; message: "Project loaded"; dismissing: false }
    }
    QtObject {
        id: testController
        property var toastModel: toasts
    }
    ToastStack { controller: testController }
}
)QML", QUrl(QStringLiteral("qrc:/ThemePaletteHarness.qml")));
    std::unique_ptr<QQuickItem> surface(qobject_cast<QQuickItem*>(component.create()));
    QVERIFY2(surface, qPrintable(component.errorString()));
    surface->setSize(window.size());
    surface->setParentItem(window.contentItem());
    auto* canvas = findVisualItem(surface.get(), QStringLiteral("themeCanvas"));
    auto* overlay = findVisualItem(surface.get(), QStringLiteral("themeOverlay"));
    auto* status = findVisualItem(surface.get(), QStringLiteral("themeStatus"));
    QVERIFY(canvas && overlay && status);
    const QRectF canvasGeometry(canvas->position(), canvas->size());
    const QRectF overlayGeometry(overlay->position(), overlay->size());
    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));
    QTest::mouseMove(&window, {5, 5});

    QColor previousCanvas;
    QColor previousOverlay;
    QColor previousMuted;
    for (const bool dark : {false, true, false}) {
        QGuiApplication::setPalette(testPalette(dark));
        const QColor background = QGuiApplication::palette().color(QPalette::Active, QPalette::Base);
        QTRY_COMPARE(surface->property("color").value<QColor>(), background);
        QTRY_COMPARE(applicationTheme(engine)->property("dark").toBool(), dark);
        QTRY_COMPARE(canvas->property("color").value<QColor>(), themeColor(engine, "canvasBackground"));
        QTRY_COMPARE(overlay->property("currentBackgroundColor").value<QColor>(), themeColor(engine, "overlayBackground"));
        const QColor canvasColor = canvas->property("color").value<QColor>();
        const QColor overlayColor = overlay->property("currentBackgroundColor").value<QColor>();
        const QColor muted = themeColor(engine, "mutedText");
        QCOMPARE(themeColor(engine, "text").alpha(), 255);
        QCOMPARE(muted.alpha(), 255);
        QVERIFY(muted != QGuiApplication::palette().color(QPalette::Active, QPalette::Mid));
        QVERIFY(contrastRatio(muted, background) >= 4.5);
        QCOMPARE(canvasColor.alpha(), 255);
        QCOMPARE(overlayColor.alpha(), 255);
        QVERIFY(dark ? luminance(canvasColor) < 0.15 : luminance(canvasColor) > 0.65);
        QVERIFY(contrastRatio(themeColor(engine, "overlayText"), overlayColor) >= 4.5);
        QVERIFY(contrastRatio(themeColor(engine, "overlaySecondaryText"), overlayColor) >= 4.5);
        QCOMPARE(QRectF(canvas->position(), canvas->size()), canvasGeometry);
        QCOMPARE(QRectF(overlay->position(), overlay->size()), overlayGeometry);
        if (previousCanvas.isValid()) {
            QVERIFY(canvasColor != previousCanvas);
            QVERIFY(overlayColor != previousOverlay);
            QVERIFY(muted != previousMuted);
        }
        previousCanvas = canvasColor;
        previousOverlay = overlayColor;
        previousMuted = muted;
        for (const char* name : {"emptyClients", "emptyScenes"}) {
            auto* panel = findVisualItem(surface.get(), QString::fromLatin1(name));
            QVERIFY(panel);
            QList<QQuickItem*> pending = panel->childItems();
            QQuickItem* emptyLabel = nullptr;
            while (!pending.isEmpty()) {
                auto* item = pending.takeLast();
                if (item->property("text").toString() == panel->property("emptyText").toString()) {
                    emptyLabel = item;
                    break;
                }
                pending.append(item->childItems());
            }
            QVERIFY(emptyLabel && emptyLabel->isVisible());
            QCOMPARE(emptyLabel->property("color").value<QColor>().rgba(), muted.rgba());
            QVERIFY(contrastRatio(muted, panel->property("color").value<QColor>()) >= 4.5);
        }
        for (int kind = 0; kind < 4; ++kind) {
            status->setProperty("statusKind", kind % 3);
            status->setProperty("statusText", kind == 3 ? "AVAILABLE" : "CONNECTED");
            QVERIFY(contrastRatio(status->property("statusForeground").value<QColor>(),
                compositeOver(status->property("statusBackground").value<QColor>(), background)) >= 4.5);
        }
        status->setProperty("statusKind", 0);
        status->setProperty("statusText", "CONNECTED");
        const QString artifactDir = qEnvironmentVariable("MOUFFETTE_OVERLAY_ARTIFACT_DIR");
        if (!artifactDir.isEmpty()) {
            QVERIFY(QDir().mkpath(artifactDir));
            auto* toast = findVisualItem(surface.get(), QStringLiteral("toastBase_0"));
            QVERIFY(toast);
            QTRY_VERIFY(toast->opacity() > 0.99);
            QVERIFY(window.grabWindow().save(QDir(artifactDir).filePath(
                dark ? QStringLiteral("theme-dark.png") : QStringLiteral("theme-light.png"))));
        }
    }
}

QTEST_MAIN(MediaOverlayTest)
#include "tst_MediaOverlay.moc"
