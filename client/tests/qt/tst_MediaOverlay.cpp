#include <QColor>
#include <QDir>
#include <QFileInfo>
#include <QFontMetricsF>
#include <QImage>
#include <QMouseEvent>
#include <QMediaPlayer>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QQmlProperty>
#include <QQuickItem>
#include <QPointer>
#include <QPromise>
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
#include "frontend/qml/QmlRuntime.h"
#include "frontend/qml/TimelineController.h"
#include "frontend/ui/notifications/ToastNotificationSystem.h"
#include "backend/domain/canvas/CanvasDocument.h"
#include "backend/domain/media/CanvasMedia.h"
#include "backend/media/MediaResidencyManager.h"
#include "backend/files/FileManager.h"
#include "backend/network/UploadManager.h"
#include "backend/runtime/RuntimeProfile.h"

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
    void screenAvailabilitySharesStatusCardWithVolume();
    void audioAndScreenStatusesRemainIndependent();
    void mediaPanelWidthSurvivesActionAndUploadTransitions();
    void emptyScreenHintStaysBehindMediaAndCenteredInViewport();
    void mediaPanelVisibilityAnchorInteractionAndScroll();
    void mediaCountTracksRealCanvasInsertions();
    void typedCapabilitiesGuardDirectCppInvocations();
    void overlayButtonHoverIsImmediate();
    void activationDuringBootstrapKeepsMainWindowHidden();
    void mainWindowPointerActivity_data();
    void mainWindowPointerActivity();
    void screenContentButtonTogglesAndPersists();
    void systemAudioButtonPersistsIndependentlyOfScreen();
    void mediaActionPalette_data();
    void mediaActionPalette();
    void mediaRowsAndProgress();
    void mediaRamProgressRequiresActiveOperation();
    void sourceRowsDeduplicateAndKeepEndpointState();
    void sourceAssociationsCommitAtomically();
    void timelineFragmentsPreserveSourceReferences();
    void uploadActionLocksBeforeDispatchAndRecovers();
    void uploadedMediaKeepsUnloadWhileRemoteCommandsAreUnavailable();
    void uploadCancelHoverTracksBothPhases();
    void unavailableActionsStayClickableAndExplainWhy();
    void toolbarToolsAndGlobalMemoryUsage();
    void memoryBreakdownIsClearAndScrollable();
    void scenePlaybackUnloadsEditorOverlays_data();
    void scenePlaybackUnloadsEditorOverlays();
    void mediaSettingsElementBindings_data();
    void mediaSettingsElementBindings();
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

QQuickItem* findVisualItem(QQuickItem* root, const QString& objectName,
                           bool visibleOnly = false)
{
    if (!root) return nullptr;
    QList<QQuickItem*> pending{root};
    while (!pending.isEmpty()) {
        QQuickItem* item = pending.takeLast();
        if (item->objectName() == objectName && (!visibleOnly || item->isVisible())) return item;
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
        property bool hasCanvasMedia: host.testMediaCount > 0
        property bool actionPending: false
        property int remoteSceneActionTone: 0
        property int testSceneActionTone: 0
        property var mediaModel: host.externalModel
        property int mediaCount: host.testMediaCount
        property string remoteSceneActionText: "Launch Remote Scene"
        property url remoteSceneActionIcon: "qrc:/icons/icons/remote-play.svg"
        property bool remoteSceneActionEnabled: true
        property string remoteSceneUnavailableReason: ""
        property string testSceneActionText: "Launch Test Scene"
        property bool testSceneActionEnabled: true
        property string testSceneUnavailableReason: ""
        property string uploadActionText: "Upload"
        property url uploadActionIcon: "qrc:/icons/icons/upload.svg"
        property int uploadActionTone: 0
        property bool uploadActionEnabled: true
        property bool uploadCancelAvailable: false
        property int uploadActionCalls: 0
        property string uploadUnavailableReason: ""
        function selectMedia(mediaId, additive) { host.selectionCalls += 1 }
        function toggleRemoteScene() {}
        function toggleTestScene() {}
        function triggerUploadAction() { uploadActionCalls += 1 }
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
            peers: []
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
                                       QQuickWindow& window, QString* error,
                                       QObject* controller = nullptr)
{
    static const QByteArray qml = R"QML(
import QtQuick
import "../canvas"

Item {
    property alias fakeSessionHasProject: fakeSession.hasProject
    property var externalController: null

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
        controller: parent.externalController
    }
}
)QML";
    QQmlComponent component(&engine);
    component.setData(qml, QUrl(QStringLiteral(
        "qrc:/qt/qml/Mouffette/App/resources/qml/app/pages/CanvasToolbarHarness.qml")));
    QObject* object = component.createWithInitialProperties({
        {QStringLiteral("externalController"), QVariant::fromValue(controller)}});
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
    property int canvasWheelCount: 0

    Item {
        anchors.fill: parent

        MouseArea {
            anchors.fill: parent
            acceptedButtons: Qt.NoButton
            scrollGestureEnabled: true
            onWheel: wheel => {
                host.canvasWheelCount += 1
                wheel.accepted = true
            }
        }

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

void MediaOverlayTest::memoryBreakdownIsClearAndScrollable()
{
    QTemporaryDir directory;
    QImage image(512, 512, QImage::Format_RGBA8888); image.fill(Qt::cyan);
    const auto path = directory.filePath("Picture.png"); QVERIFY(image.save(path));
    auto& manager = MediaResidencyManager::instance();
    manager.acquire("memory-popup-image", path);
    manager.acquire("memory-popup-video", QFINDTESTDATA("../fixtures/resident-timeline.mp4"));
    const auto cleanup = qScopeGuard([&] {
        manager.release("memory-popup-image"); manager.release("memory-popup-video");
    });
    QTRY_VERIFY_WITH_TIMEOUT(manager.ready("memory-popup-image") && manager.ready("memory-popup-video"), 10000);
    QQmlEngine engine;
    QQuickWindow window;
    window.resize(760, 700);
    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));
    QQmlComponent component(&engine, QUrl(QStringLiteral(
        "qrc:/qt/qml/Mouffette/App/resources/qml/app/dialogs/MemoryUsagePopup.qml")));
    std::unique_ptr<QObject> popup(component.createWithInitialProperties({
        {QStringLiteral("parent"), QVariant::fromValue(window.contentItem())}
    }));
    QVERIFY2(popup, qPrintable(component.errorString()));
    QVERIFY(QMetaObject::invokeMethod(popup.get(), "open"));
    QTRY_VERIFY(popup->property("opened").toBool());
    auto* list = findVisualItem(window.contentItem(), "memoryAssetList"); QVERIFY(list);
    QQuickItem* bar = nullptr;
    QTRY_VERIFY(bar = findVisualItem(window.contentItem(), "memoryDistributionBar"));
    QTRY_VERIFY(bar->isVisible() && bar->width() > 400 && bar->height() == 26);
    auto* process = findVisualItem(window.contentItem(), "memoryProcessSegment"); QVERIFY(process);
    auto* other = findVisualItem(window.contentItem(), "memoryOtherSegment"); QVERIFY(other);
    auto* available = findVisualItem(window.contentItem(), "memoryAvailableSegment"); QVERIFY(available);
    QTRY_VERIFY(qAbs(process->width() / bar->width() - 0.0625) < 0.001);
    QTRY_VERIFY(qAbs(other->width() / bar->width() - 0.1875) < 0.001);
    QTRY_VERIFY(qAbs(available->width() / bar->width() - 0.75) < 0.001);
    QVERIFY(!findVisualItem(window.contentItem(), "memoryCategory_videoBytes"));
    auto* playback = findVisualItem(window.contentItem(), "memoryPlaybackEstimate"); QVERIFY(playback);
    QCOMPARE(playback->property("text").toString(), QStringLiteral("0 B"));
    const auto asset = manager.asset("memory-popup-video");
    QVERIFY(asset->reservePlayback());
    const auto releasePlayer = qScopeGuard([&] { asset->releasePlayback(false); });
    QTRY_VERIFY(playback->property("text").toString() != QStringLiteral("0 B"));
    const auto totals = popup->property("usage").toMap();
    QCOMPARE(totals.value("mediaBytes").toULongLong(),
        totals.value("videoBytes").toULongLong() + totals.value("imageBytes").toULongLong()
        + totals.value("posterBytes").toULongLong() + totals.value("thumbnailBytes").toULongLong()
        + totals.value("audioPreviewBytes").toULongLong());
    const auto capture = [&](const QString& name) {
        const auto output = qEnvironmentVariable("MOUFFETTE_OVERLAY_ARTIFACT_DIR");
        if (output.isEmpty()) return;
        QVERIFY(QDir().mkpath(output));
        QTest::qWait(100);
        QSignalSpy frames(&window, &QQuickWindow::frameSwapped);
        window.update(); QTRY_VERIFY(!frames.isEmpty());
        QVERIFY(window.grabWindow().save(QDir(output).filePath(name)));
    };
    capture("memory-breakdown-wide.png");
    QVERIFY(QMetaObject::invokeMethod(list, "positionViewAtEnd"));
    QTRY_VERIFY(findVisualItem(window.contentItem(), "memoryAssetBreakdown"));
    auto* details = findVisualItem(window.contentItem(), "memoryAssetBreakdown");
    QVERIFY(details->property("text").toString().contains("Thumbnails"));
    QVERIFY(details->property("text").toString().contains("Stored total"));
    capture("memory-breakdown-assets.png");
    window.resize(420, 360);
    QTRY_VERIFY(list->width() < 400);
    QTRY_VERIFY(list->property("contentHeight").toReal() > list->height());
    auto* first = findVisualItem(window.contentItem(), "memoryLegend_process");
    auto* third = findVisualItem(window.contentItem(), "memoryLegend_available");
    QVERIFY(first && third);
    QTRY_VERIFY(third->y() > first->y()); // The legend wraps on narrow windows.
    QTRY_VERIFY(bar->width() <= list->width() && bar->width() > 0);
    QVERIFY(QMetaObject::invokeMethod(popup.get(), "close"));
    QTRY_VERIFY(!popup->property("opened").toBool());
    QVERIFY(QMetaObject::invokeMethod(popup.get(), "open"));
    QTRY_VERIFY(popup->property("opened").toBool());
    auto* totalLabel = findVisualItem(window.contentItem(), "memoryStoredTotal");
    QVERIFY(totalLabel);
    QTest::qWait(150); // Let the resized header and model refresh finish laying out.
    QTRY_VERIFY(qAbs(totalLabel->mapToItem(list, {0, 0}).y()) < 1);
    capture("memory-breakdown-narrow.png");
    QVERIFY(QMetaObject::invokeMethod(popup.get(), "close"));
    QTRY_VERIFY(!popup->property("opened").toBool());
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

void MediaOverlayTest::screenAvailabilitySharesStatusCardWithVolume()
{
    QQmlEngine engine;
    QQuickWindow window;
    window.resize(480, 110);
    std::unique_ptr<QQuickItem> card(createStatusCard(engine, window, true));
    QVERIFY(card);
    card->setProperty("primaryText", QStringLiteral("Remote client"));
    card->setProperty("screenStatusVisible", true);
    card->setWidth(440);
    auto* screen = card->findChild<QQuickItem*>(QStringLiteral("screenAvailabilitySegment"));
    auto* icon = card->findChild<QQuickItem*>(QStringLiteral("screenAvailabilityIcon"));
    auto* label = card->findChild<QQuickItem*>(QStringLiteral("screenAvailabilityLabel"));
    auto* volume = card->findChild<QQuickItem*>(QStringLiteral("volumeSegment"));
    auto* connectionLabel = card->findChild<QQuickItem*>(QStringLiteral("connectionStatusLabel"));
    auto* connectionSegment = card->findChild<QQuickItem*>(QStringLiteral("statusSegment"));
    QVERIFY(screen && icon && label && volume && connectionLabel && connectionSegment);
    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));
    const qreal stableWidth = card->implicitWidth();
    struct ScreenState {
        bool enabled;
        bool loading;
        bool available;
        int statusKind;
        QString label;
        QString artifact;
    };
    const ScreenState states[] = {
        {true, true, false, 1, QStringLiteral("Screen loading"), QStringLiteral("screen-loading.png")},
        {true, false, true, 0, QStringLiteral("Screen available"), QStringLiteral("screen-available.png")},
        {true, false, false, 2, QStringLiteral("Screen not available"), QStringLiteral("screen-not-available.png")},
        // The viewer's choice takes precedence over a stale remote state.
        {false, true, true, 2, QStringLiteral("Screen disabled"), QStringLiteral("screen-disabled.png")}
    };
    for (const auto& state : states) {
        card->setProperty("screenContentEnabled", state.enabled);
        card->setProperty("screenLoading", state.loading);
        card->setProperty("screenAvailable", state.available);
        card->setProperty("statusKind", state.statusKind);
        card->setProperty("statusText", state.statusKind == 0 ? "CONNECTED"
            : state.statusKind == 1 ? "CONNECTING" : "DISCONNECTED");
        QCOMPARE(label->property("text").toString(), state.label);
        QCOMPARE(label->property("color"), connectionLabel->property("color"));
        QCOMPARE(label->property("font").value<QFont>(), connectionLabel->property("font").value<QFont>());
        QCOMPARE(screen->property("color"), connectionSegment->property("color"));
        QCOMPARE(icon->property("source").toUrl().fileName(), state.enabled && (state.available || state.loading)
            ? QStringLiteral("screen.svg") : QStringLiteral("screen-off.svg"));
        QTRY_COMPARE(icon->property("status").toInt(), 1); // Image.Ready.
        QCOMPARE(card->implicitWidth(), stableWidth);
        QVERIFY(screen->isVisible());
        QVERIFY(volume->isVisible());
        for (const auto* volumeText : {"—", "0%", "75%", "100%"}) {
            card->setProperty("auxiliaryText", QString::fromUtf8(volumeText));
            QVERIFY(volume->isVisible());
            QCOMPARE(card->implicitWidth(), stableWidth);
        }
        card->setProperty("auxiliaryText", QStringLiteral("75%"));
        QTRY_VERIFY(screen->x() + screen->width() <= volume->x());
        QVERIFY(icon->x() + icon->width() <= label->x());
        QVERIFY(label->property("implicitWidth").toReal() <= label->width());
        QVERIFY(volume->x() + volume->width() <= card->width());
        const QString artifactDir = qEnvironmentVariable("MOUFFETTE_OVERLAY_ARTIFACT_DIR");
        if (!artifactDir.isEmpty()) {
            QVERIFY(QDir().mkpath(artifactDir));
            QSignalSpy frames(&window, &QQuickWindow::frameSwapped);
            window.update();
            QTRY_VERIFY(!frames.isEmpty());
            QVERIFY(window.grabWindow().save(QDir(artifactDir).filePath(state.artifact)));
        }
    }
}

void MediaOverlayTest::audioAndScreenStatusesRemainIndependent()
{
    QQmlEngine engine;
    QQuickWindow window;
    window.resize(900, 110);
    std::unique_ptr<QQuickItem> card(createStatusCard(engine, window, true));
    QVERIFY(card);
    card->setProperty("primaryText", QStringLiteral("Remote client"));
    card->setProperty("screenStatusVisible", true);
    card->setProperty("audioStatusVisible", true);
    card->setProperty("screenState", QStringLiteral("available"));
    card->setWidth(860);
    auto* screen = card->findChild<QQuickItem*>(QStringLiteral("screenAvailabilitySegment"));
    auto* screenLabel = card->findChild<QQuickItem*>(QStringLiteral("screenAvailabilityLabel"));
    auto* audio = card->findChild<QQuickItem*>(QStringLiteral("audioAvailabilitySegment"));
    auto* audioLabel = card->findChild<QQuickItem*>(QStringLiteral("audioAvailabilityLabel"));
    auto* audioIcon = card->findChild<QQuickItem*>(QStringLiteral("audioAvailabilityIcon"));
    auto* volume = card->findChild<QQuickItem*>(QStringLiteral("volumeSegment"));
    QVERIFY(screen && screenLabel && audio && audioLabel && audioIcon && volume);
    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));
    // Cocoa may fit first exposure to the screen at a high scale factor.
    window.resize(900, 110);
    const qreal stableWidth = card->implicitWidth();
    const QList<QPair<QString, QString>> states{
        {QStringLiteral("loading"), QStringLiteral("Audio loading")},
        {QStringLiteral("available"), QStringLiteral("Audio available")},
        {QStringLiteral("sharing_disabled"), QStringLiteral("Audio not shared")},
        {QStringLiteral("unavailable"), QStringLiteral("Audio not available")},
        {QStringLiteral("error"), QStringLiteral("Audio error")}
    };
    for (const auto& [state, label] : states) {
        card->setProperty("audioState", state);
        QCOMPARE(audioLabel->property("text").toString(), label);
        QCOMPARE(screenLabel->property("text").toString(), QStringLiteral("Screen available"));
        QCOMPARE(audioIcon->property("source").toUrl().fileName(),
            state == QLatin1String("loading") || state == QLatin1String("available")
                ? QStringLiteral("volume-on.svg") : QStringLiteral("volume-off.svg"));
        QCOMPARE(card->implicitWidth(), stableWidth);
        QTRY_VERIFY(screen->x() + screen->width() <= audio->x());
        QTRY_VERIFY(audio->x() + audio->width() <= volume->x());
        QVERIFY(audioLabel->isVisible());
        QVERIFY(audioLabel->implicitWidth() <= audioLabel->width());
        QVERIFY(volume->x() + volume->width() <= card->width());
        const QString artifactDir = qEnvironmentVariable("MOUFFETTE_OVERLAY_ARTIFACT_DIR");
        if (!artifactDir.isEmpty()) {
            QVERIFY(QDir().mkpath(artifactDir));
            QSignalSpy frames(&window, &QQuickWindow::frameSwapped);
            window.update();
            QTRY_VERIFY(!frames.isEmpty());
            QVERIFY(window.grabWindow().save(QDir(artifactDir).filePath(
                QStringLiteral("audio-status-%1.png").arg(state))));
        }
    }
    card->setProperty("audioState", QStringLiteral("available"));
    card->setProperty("screenState", QStringLiteral("error"));
    QCOMPARE(screenLabel->property("text").toString(), QStringLiteral("Screen error"));
    QCOMPARE(audioLabel->property("text").toString(), QStringLiteral("Audio available"));
    card->setProperty("screenState", QStringLiteral("sharing_disabled"));
    QCOMPARE(screenLabel->property("text").toString(), QStringLiteral("Screen not shared"));
    QCOMPARE(audioLabel->property("text").toString(), QStringLiteral("Audio available"));
    card->setProperty("screenContentEnabled", false);
    QCOMPARE(screenLabel->property("text").toString(), QStringLiteral("Screen disabled"));
    QCOMPARE(audioLabel->property("text").toString(), QStringLiteral("Audio available"));
    card->setProperty("systemAudioEnabled", false);
    QCOMPARE(audioLabel->property("text").toString(), QStringLiteral("Audio disabled"));
    card->setProperty("audioState", QStringLiteral("error"));
    QCOMPARE(audioLabel->property("text").toString(), QStringLiteral("Audio disabled"));

    // A long endpoint name should elide before either status loses its label.
    card->setProperty("primaryText", QString(200, QLatin1Char('W')));
    QVERIFY(card->implicitWidth() > card->width());
    QVERIFY(!card->property("compactMediaStatus").toBool());
    QVERIFY(screenLabel->isVisible() && audioLabel->isVisible());
    card->setProperty("primaryText", QStringLiteral("Remote client"));
    card->setWidth(440);
    QTRY_VERIFY(card->property("compactMediaStatus").toBool());
    QVERIFY(screen->isVisible() && audio->isVisible() && volume->isVisible());
    QVERIFY(!screenLabel->isVisible() && !audioLabel->isVisible());
    QTRY_VERIFY(screen->x() >= 0);
    QTRY_VERIFY(volume->x() + volume->width() <= card->width());
    const QString artifactDir = qEnvironmentVariable("MOUFFETTE_OVERLAY_ARTIFACT_DIR");
    if (!artifactDir.isEmpty()) {
        QSignalSpy frames(&window, &QQuickWindow::frameSwapped);
        window.update();
        QTRY_VERIFY(!frames.isEmpty());
        QVERIFY(window.grabWindow().save(QDir(artifactDir).filePath(QStringLiteral("audio-screen-status-narrow.png"))));
    }
    card->setWidth(860);
    QTRY_VERIFY(screenLabel->isVisible() && audioLabel->isVisible());
    QCOMPARE(card->implicitWidth(), stableWidth);
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
    const qreal resizedWidth = metrics->property("maximumWidth").toReal();
    QVERIFY(resizedWidth > measured);
    // Platform fallback fonts may not scale linearly. Match the actual font's
    // advance exactly while still requiring the binding to follow its resize.
    QCOMPARE(resizedWidth, std::ceil(QFontMetricsF(font).horizontalAdvance(QStringLiteral("WWW"))));
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
    for (const auto& label : {"Launch Remote Scene", "Launching Remote Scene",
                              "Stop Remote Scene", "Stopping Remote Scene"}) {
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
                              "Uploading (10/10) 100%", "Loading in ram (0/10)",
                              "Loading in ram (10/10)", "Cancel", "Cancelling…",
                              "Removing…", "Unload", "Upload"}) {
        const bool progress = QString::fromUtf8(label).startsWith("Uploading (")
            || QString::fromUtf8(label).startsWith("Loading in ram (");
        session->setProperty("uploadActionText", QString::fromUtf8(label));
        session->setProperty("uploadActionTone", progress ? 1 : 0);
        QCOMPARE(upload->implicitWidth(), uploadWidth);
        QCOMPARE(panel->implicitWidth(), initialWidth);
        QCOMPARE(panel->x(), initialLeft);
    }
}

void MediaOverlayTest::uploadCancelHoverTracksBothPhases()
{
    QQmlEngine engine;
    QQuickWindow window;
    window.resize(600, 350);
    MediaListModel model;
    QString error;
    std::unique_ptr<QQuickItem> harness(createMediaPanelHarness(engine, window, &model, &error));
    QVERIFY2(harness, qPrintable(error));
    harness->setSize(window.size());
    harness->setProperty("testMediaCount", 10);
    auto* session = harness->property("session").value<QObject*>();
    auto* button = findVisualItem(harness.get(), "uploadAction");
    QVERIFY(session && button);
    session->setProperty("uploadActionTone", 1);
    session->setProperty("uploadCancelAvailable", true);
    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));
    int clicks = 0;
    for (const auto& label : {"Uploading (2/10) 20%", "Loading in ram (3/10)"}) {
        session->setProperty("uploadActionText", label);
        QTest::mouseMove(&window, {1, 1});
        QTRY_COMPARE(button->property("text").toString(), QString::fromUtf8(label));
        QCOMPARE(button->property("foregroundColor").value<QColor>(), themeColor(engine, "brandBlue"));
        const qreal originalWidth = button->implicitWidth();
        const QPoint center = button->mapToScene(QPointF(button->width() / 2, button->height() / 2)).toPoint();
        QTest::mouseMove(&window, center);
        QTRY_VERIFY(button->property("showingCancel").toBool());
        QCOMPARE(button->property("text").toString(), QStringLiteral("Cancel"));
        QCOMPARE(button->property("foregroundColor").value<QColor>(), themeColor(engine, "errorText"));
        QCOMPARE(button->property("backgroundColor").value<QColor>(), themeColor(engine, "destructiveHover"));
        QCOMPARE(button->implicitWidth(), originalWidth);
        QTest::mouseClick(&window, Qt::LeftButton, Qt::NoModifier, center);
        QCOMPARE(session->property("uploadActionCalls").toInt(), ++clicks);
        QTest::mouseMove(&window, {1, 1});
        QTRY_COMPARE(button->property("text").toString(), QString::fromUtf8(label));
    }
    session->setProperty("uploadCancelAvailable", false);
    session->setProperty("uploadActionText", "Unload");
    session->setProperty("uploadActionTone", 2);
    QTest::mouseMove(&window, button->mapToScene(QPointF(button->width() / 2, button->height() / 2)).toPoint());
    QVERIFY(!button->property("showingCancel").toBool());
    QCOMPARE(button->property("text").toString(), QStringLiteral("Unload"));
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
        {"controller", QVariantMap{{"activeWorkspace", QVariant::fromValue(&session)}, {"remoteStatusText", QStringLiteral("CONNECTED")}}}
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
    auto* canvasLoader = findVisualItem(page, QStringLiteral("activeCanvasLoader"));
    QVERIFY(canvasLoader);
    const QPointF canvasCenter = canvasLoader->mapToScene({canvasLoader->width() / 2, canvasLoader->height() / 2});
    QVERIFY(QLineF(center, canvasCenter).length() <= 1.0);

    QTemporaryDir directory;
    const QString path = directory.filePath("foreground.png");
    QImage source(640, 120, QImage::Format_ARGB32_Premultiplied);
    source.fill(Qt::cyan);
    QVERIFY(source.save(path));
    auto* media = host->document()->addPreparedFile(path, source.size(), false, center - QPointF(320, 60));
    QVERIFY(media);
    host->document()->clearSelection();
    // This check isolates media-versus-hint stacking. The sources panel can
    // overlap the sample area on small screens with a taller timeline.
    auto* sourcesPanel = findVisualItem(page, QStringLiteral("mediaListPanel"));
    QVERIFY(sourcesPanel);
    sourcesPanel->setVisible(false);
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
    QCOMPARE(list->height(), 0.0);

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
    QCOMPARE(harness->property("selectionCalls").toInt(), 0);

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
        QTRY_VERIFY(status->isVisible());
        QVERIFY(!progress->isVisible());
        QCOMPARE(fill->opacity(), 1.0);
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
    auto* text = host->document()->addText(QPointF(100, 100));
    QVERIFY(text);
    QCOMPARE(session.mediaCount(), 0);
    QCOMPARE(countChanged.count(), 0);
    QTRY_VERIFY(panel->isVisible());
    auto* upload = findVisualItem(panel, "uploadAction");
    auto* remote = findVisualItem(panel, "remoteSceneAction");
    QVERIFY(upload && remote);
    QVERIFY(!upload->isVisible());
    QVERIFY(remote->isVisible());
    QCOMPARE(panel->height(), remote->height() + 1);
    QCOMPARE(remote->property("bottomRadius"), panel->property("radius"));
    QCOMPARE(qRound(panel->x() + panel->width()), window.width() - 16);
    QCOMPARE(qRound(panel->y() + panel->height()), window.height() - 16);
    QVERIFY(host->document()->removeMedia(text->mediaId()));
    QTRY_VERIFY(!panel->isVisible());
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
    // Hash resolution replaces the provisional path row. Wait before retaining
    // its delegates for the animation assertions below.
    QTRY_VERIFY_WITH_TIMEOUT(photo->residencyReady() && !photo->fileId().isEmpty(), 10000);
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
    QTRY_VERIFY(findVisualItem(panel, QStringLiteral("mediaRow_0")));
    QVERIFY(!findVisualItem(panel, QStringLiteral("mediaRow_1")));
    auto* row = findVisualItem(panel, QStringLiteral("mediaRow_0"));
    QCOMPARE(session.mediaCount(), 1);
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
    QVERIFY(!progress->isVisible());
    QVERIFY(status->isVisible());
    QCOMPARE(status->property("text").toString(), QStringLiteral("Uploaded"));
    QCOMPARE(row->height(), originalHeight);
    host->document()->select(text->mediaId());
    const QPoint clickPoint = row->mapToScene({row->width() / 2, row->height() / 2}).toPoint();
    QVERIFY(QRect(QPoint(), window.size()).contains(clickPoint));
    QTest::mouseClick(&window, Qt::LeftButton, Qt::NoModifier, clickPoint);
    QCOMPARE(host->document()->selectedMedia(), text);
    QVERIFY(!row->hasActiveFocus());
    host->document()->select(photo->mediaId());
    QVERIFY(!row->property("selected").isValid());
    QCOMPARE(findVisualItem(panel, QStringLiteral("mediaRow_0")), row);
    harness->setWidth(600);
    QVERIFY(panel->width() <= 300.0);
    photo->setUploadNotUploaded();
    host->document()->clear();
    QTRY_VERIFY(!panel->isVisible());
    QTRY_COMPARE(panel->height(), panel->property("actionAreaHeight").toReal());
}

void MediaOverlayTest::mediaRamProgressRequiresActiveOperation()
{
    QQmlEngine engine;
    QQuickWindow window;
    window.resize(640, 480);
    MediaListModel model;
    QString error;
    std::unique_ptr<QQuickItem> harness(createMediaPanelHarness(engine, window, &model, &error));
    QVERIFY2(harness, qPrintable(error));
    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));
    harness->setSize(window.size());
    harness->setProperty("testMediaCount", 1);
    QVariantMap source{{"rowKey", "source"}, {"displayName", "Source"},
                       {"mediaType", "image"}, {"uploadState", "uploaded"},
                       {"uploadProgress", 100}, {"remoteCached", false}};
    model.updateFromList({source});
    auto* row = findVisualItem(harness.get(), "mediaRow_0");
    auto* status = findVisualItem(harness.get(), "mediaStatus_0");
    auto* progress = findVisualItem(harness.get(), "mediaProgress_0");
    auto* fill = findVisualItem(harness.get(), "mediaProgressFill_0");
    QVERIFY(row && status && progress && fill);
    auto* pulse = fill->findChild<QObject*>("mediaCachePulse_0");
    QVERIFY(pulse);

    // An uploaded ledger entry without current cache evidence must stay idle.
    QVERIFY(status->isVisible());
    QVERIFY(!progress->isVisible());
    QVERIFY(!row->property("awaitingRemoteCache").toBool());
    QVERIFY(!pulse->property("running").toBool());
    source.insert("remoteLoadingInRam", true);
    model.updateFromList({source});
    QTRY_VERIFY(progress->isVisible());
    QVERIFY(!status->isVisible());
    QVERIFY(row->property("awaitingRemoteCache").toBool());
    QCOMPARE(fill->width(), progress->width());
    QVERIFY(pulse->property("running").toBool());

    // Closing or failing the load stops the pulse even if an uploaded marker
    // and 100% transfer progress were still published before inventory clears.
    source.insert("remoteLoadingInRam", false);
    model.updateFromList({source});
    QTRY_VERIFY(status->isVisible());
    QVERIFY(!progress->isVisible());
    QVERIFY(!pulse->property("running").toBool());
    QCOMPARE(fill->opacity(), 1.0);
    source.insert("uploadState", "not_uploaded");
    source.insert("uploadProgress", 0);
    model.updateFromList({source});
    QCOMPARE(status->property("text").toString(), QStringLiteral("Not uploaded"));
    QVERIFY(!progress->isVisible());

    // A subsequent upload creates a fresh operation and can complete normally.
    source.insert("uploadState", "uploading");
    source.insert("uploadProgress", 100);
    source.insert("remoteLoadingInRam", true);
    model.updateFromList({source});
    QVERIFY(progress->isVisible());
    QVERIFY(row->property("awaitingRemoteCache").toBool());
    source.insert("uploadState", "uploaded");
    source.insert("remoteCached", true);
    source.insert("remoteLoadingInRam", false);
    model.updateFromList({source});
    QVERIFY(status->isVisible());
    QVERIFY(!progress->isVisible());
    QCOMPARE(status->property("text").toString(), QStringLiteral("Uploaded and Cached"));
    QCOMPARE(findVisualItem(harness.get(), "mediaRow_0"), row);
}

void MediaOverlayTest::sourceRowsDeduplicateAndKeepEndpointState()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString firstPath = temporary.filePath(QStringLiteral("zebra.png"));
    const QString aliasPath = temporary.filePath(QStringLiteral("alpha.png"));
    QVERIFY(QDir().mkpath(temporary.filePath(QStringLiteral("other"))));
    const QString distinctPath = temporary.filePath(QStringLiteral("other/zebra.png"));
    QImage original(128, 64, QImage::Format_ARGB32_Premultiplied);
    original.fill(Qt::darkGreen);
    QVERIFY(original.save(firstPath));
    QVERIFY(QFile::copy(firstPath, aliasPath));
    original.fill(Qt::darkRed);
    QVERIFY(original.save(distinctPath));
    FileManager files;
    UploadManager uploads(&files, nullptr, temporary.filePath(QStringLiteral("remote-cache")));
    QString error;
    std::unique_ptr<QuickCanvasHost> host(QuickCanvasHost::create(&error));
    QVERIFY2(host, qPrintable(error));
    host->setFileManager(&files);
    host->setProjectEditingEnabled(true);
    ClientWorkspaceViewModel sessionA(QStringLiteral("endpoint-a"), host.get(), [] {}, &uploads,
        [] { return false; }, [] { return true; }, [] { return true; });
    ClientWorkspaceViewModel sessionB(QStringLiteral("endpoint-b"), host.get(), [] {}, &uploads,
        [] { return false; }, [] { return true; }, [] { return true; });
    auto* first = host->document()->addPreparedFile(firstPath, {128, 64}, false, {});
    auto* duplicate = host->document()->addPreparedFile(firstPath, {128, 64}, false, {});
    QVERIFY(first && duplicate);
    QCOMPARE(sessionA.mediaCount(), 1); // Same path is one source even before hashing.
    auto* alias = host->document()->addPreparedFile(aliasPath, {128, 64}, false, {});
    auto* distinct = host->document()->addPreparedFile(distinctPath, {128, 64}, false, {});
    QVERIFY(alias && distinct);
    QVERIFY(host->document()->addText({}, QStringLiteral("Not a source")));
    QTRY_VERIFY_WITH_TIMEOUT(first->residencyReady() && duplicate->residencyReady()
        && alias->residencyReady() && distinct->residencyReady(), 10000);
    QCOMPARE(first->fileId(), alias->fileId());
    QVERIFY(first->fileId() != distinct->fileId());
    QTRY_COMPARE(sessionA.mediaCount(), 2);
    auto* model = qobject_cast<MediaListModel*>(sessionA.mediaModel());
    QVERIFY(model);
    const auto sourceRow = [model](const QString& id) {
        for (int index = 0; index < model->rowCount(); ++index) {
            const auto row = model->data(model->index(index), MediaListModel::ModelDataRole).toMap();
            if (row.value(QStringLiteral("sourceId")).toString() == id) return row;
        }
        return QVariantMap();
    };
    QCOMPARE(model->data(model->index(0), MediaListModel::DisplayNameRole).toString(), QStringLiteral("alpha.png"));
    const QString fileId = first->fileId();
    first->setBaseSize({1024, 768});
    QCOMPARE(sourceRow(fileId).value(QStringLiteral("width")).toInt(), 128);
    QCOMPARE(sourceRow(fileId).value(QStringLiteral("height")).toInt(), 64);
    files.markFileUploadedToClient(fileId, QStringLiteral("endpoint-a"));
    emit uploads.uiStateChanged();
    QCOMPARE(sourceRow(fileId).value(QStringLiteral("uploadState")).toString(), QStringLiteral("uploaded"));
    QVERIFY(!sourceRow(fileId).value(QStringLiteral("remoteCached")).toBool());
    QVERIFY(!sourceRow(fileId).value(QStringLiteral("remoteLoadingInRam")).toBool());
    auto* modelB = qobject_cast<MediaListModel*>(sessionB.mediaModel());
    QVERIFY(modelB);
    QCOMPARE(modelB->data(modelB->index(0), MediaListModel::UploadStateRole).toString(), QStringLiteral("not_uploaded"));
    first->setUploadNotUploaded();
    QVERIFY(host->document()->removeMedia(first->mediaId()));
    QCOMPARE(sessionA.mediaCount(), 2);
    QCOMPARE(sourceRow(fileId).value(QStringLiteral("uploadState")).toString(), QStringLiteral("uploaded"));
    QVERIFY(host->document()->removeMedia(duplicate->mediaId()));
    QCOMPARE(sessionA.mediaCount(), 2);
    QVERIFY(host->document()->removeMedia(alias->mediaId()));
    QCOMPARE(sessionA.mediaCount(), 1);
    QCOMPARE(sourceRow(fileId), QVariantMap());
    QVERIFY(QFile::exists(firstPath));
}

void MediaOverlayTest::sourceAssociationsCommitAtomically()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString path = temporary.filePath(QStringLiteral("source.png"));
    QImage image(16, 16, QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::cyan);
    QVERIFY(image.save(path));
    FileManager files;
    const QString fileId = files.getOrCreateFileId(path);
    QVERIFY(!fileId.isEmpty());
    files.associateMediaWithFile(QStringLiteral("original"), fileId);
    files.markFileUploadedToClient(fileId, QStringLiteral("endpoint-a"));
    files.markFileUploadedToClient(fileId, QStringLiteral("endpoint-b"));
    int removals = 0;
    QSet<QString> removedTargets;
    FileManager::setFileRemovalNotifier([&](const QString& removedId, const QList<QString>& targets,
                                           const QList<QString>&) {
        QCOMPARE(removedId, fileId);
        ++removals;
        removedTargets = QSet<QString>(targets.cbegin(), targets.cend());
    });
    const auto clearNotifier = qScopeGuard([] { FileManager::setFileRemovalNotifier({}); });
    files.beginMediaAssociationTransaction();
    files.beginMediaAssociationTransaction();
    files.removeMediaAssociation(QStringLiteral("original"));
    QVERIFY(files.hasFileId(fileId));
    files.endMediaAssociationTransaction();
    QCOMPARE(removals, 0);
    files.associateMediaWithFile(QStringLiteral("fragment-left"), fileId);
    files.associateMediaWithFile(QStringLiteral("fragment-right"), fileId);
    files.endMediaAssociationTransaction();
    QCOMPARE(removals, 0);
    QVERIFY(files.isFileUploadedToClient(fileId, QStringLiteral("endpoint-a")));
    files.removeMediaAssociation(QStringLiteral("fragment-left"));
    QCOMPARE(removals, 0);
    files.removeMediaAssociation(QStringLiteral("fragment-right"));
    QCOMPARE(removals, 1);
    QCOMPARE(removedTargets, (QSet<QString>{QStringLiteral("endpoint-a"), QStringLiteral("endpoint-b")}));
    QVERIFY(QFile::exists(path));
}

void MediaOverlayTest::timelineFragmentsPreserveSourceReferences()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString path = temporary.filePath(QStringLiteral("fragment-source.png"));
    QImage image(16, 16, QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::darkBlue);
    QVERIFY(image.save(path));
    FileManager files;
    CanvasDocument document;
    document.setFileManager(&files);
    auto* original = document.addPreparedFile(path, {16, 16}, false, {});
    QVERIFY(original);
    QTRY_VERIFY_WITH_TIMEOUT(original->residencyReady(), 10000);
    const QString sourceId = original->fileId();
    QVERIFY(!sourceId.isEmpty());
    auto track = original->timelineTrack();
    track.clip.startSlot = 0;
    track.clip.durationSlots = 100;
    original->setTimelineTrack(track);
    files.markFileUploadedToClient(sourceId, QStringLiteral("fragment-endpoint"));
    int removals = 0;
    FileManager::setFileRemovalNotifier([&](const QString&, const QList<QString>&,
                                           const QList<QString>&) { ++removals; });
    const auto clearNotifier = qScopeGuard([] { FileManager::setFileRemovalNotifier({}); });
    const auto residentAsset = MediaResidencyManager::instance().asset(original->residencyOwnerId());
    QVERIFY(residentAsset);
    const QString originalId = original->mediaId();
    auto snapshot = document.timelineMediaSnapshot(originalId);
    QString error;
    const QString replacementId = document.pasteTimelineClip(snapshot, {{originalId, path}}, 0, 0, &error);
    QVERIFY2(!replacementId.isEmpty(), qPrintable(error));
    QCOMPARE(document.media().size(), 1);
    QCOMPARE(files.getMediaIdsForFile(sourceId), QList<QString>{replacementId});
    QCOMPARE(removals, 0);
    original = document.mediaById(replacementId);
    QVERIFY(original);
    QTRY_VERIFY_WITH_TIMEOUT(original->residencyReady(), 10000);
    QCOMPARE(MediaResidencyManager::instance().asset(original->residencyOwnerId()).get(), residentAsset.get());
    snapshot = document.timelineMediaSnapshot(replacementId);
    track = original->timelineTrack();
    auto pastedTrack = track;
    pastedTrack.clip.durationSlots = 20;
    snapshot.insert(QStringLiteral("timeline"), pastedTrack.toJson());
    const QString pastedId = document.pasteTimelineClip(snapshot, {{original->mediaId(), path}}, 40, 0, &error);
    QVERIFY2(!pastedId.isEmpty(), qPrintable(error));
    QCOMPARE(document.media().size(), 3);
    // Assert immediately, before any asynchronous identityReady from the clones.
    QCOMPARE(files.getMediaIdsForFile(sourceId).size(), 3);
    QCOMPARE(removals, 0);
    auto* pasted = document.mediaById(pastedId);
    QVERIFY(pasted);
    QVERIFY2(document.splitTimelineClip(pasted->timelineTrack().clip.id, 50, &error), qPrintable(error));
    QCOMPARE(files.getMediaIdsForFile(sourceId).size(), 4);
    QCOMPARE(removals, 0);
    QVERIFY(files.isFileUploadedToClient(sourceId, QStringLiteral("fragment-endpoint")));
    const QList<CanvasMedia*> media = document.media();
    for (int index = 0; index < media.size(); ++index) {
        QVERIFY(document.removeMedia(media.at(index)->mediaId()));
        QCOMPARE(removals, index == media.size() - 1 ? 1 : 0);
    }
    QVERIFY(QFile::exists(path));
}

void MediaOverlayTest::unavailableActionsStayClickableAndExplainWhy()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const auto previousProfile = RuntimeProfile::context();
    auto profile = previousProfile;
    profile.rootPath = temporary.path();
    RuntimeProfile::configure(profile);
    const auto restoreProfile = qScopeGuard([&] { RuntimeProfile::configure(previousProfile); });
    ToastNotificationSystem notifications;
    auto* previousNotifications = ToastNotificationSystem::instance();
    ToastNotificationSystem::setInstance(&notifications);
    const auto restoreNotifications = qScopeGuard([&] {
        ToastNotificationSystem::setInstance(previousNotifications);
    });
    QSignalSpy toasts(notifications.notificationCenter(), &NotificationCenter::toastRequested);
    FileManager files;
    UploadManager uploads(&files, nullptr, temporary.filePath("Uploads"));
    QString error;
    std::unique_ptr<QuickCanvasHost> host(QuickCanvasHost::create(&error));
    QVERIFY2(host, qPrintable(error));
    host->setProjectEditingEnabled(true);
    bool projectExists = true;
    int uploadCalls = 0;
    ClientWorkspaceViewModel session("toast-workspace", host.get(), [&] { ++uploadCalls; },
        &uploads, [] { return false; }, [] { return true; }, [&] { return projectExists; });
    QQmlEngine engine;
    QQuickWindow window;
    window.resize(640, 480);
    std::unique_ptr<QQuickItem> harness(createRealMediaPanelHarness(engine, window, &session, &error));
    QVERIFY2(harness, qPrintable(error));
    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));
    harness->setSize(window.size());
    auto* upload = findVisualItem(harness.get(), "uploadAction");
    auto* remote = findVisualItem(harness.get(), "remoteSceneAction");
    QVERIFY(upload && remote);
    const auto clickUnavailable = [&](QQuickItem* button, const QString& expected) {
        QVERIFY(button->isVisible());
        QVERIFY(button->isEnabled());
        QVERIFY(!button->property("dimmed").toBool());
        const auto count = toasts.count();
        QTest::mouseClick(&window, Qt::LeftButton, Qt::NoModifier,
            button->mapToScene({button->width() / 2, button->height() / 2}).toPoint());
        QCOMPARE(toasts.count(), count + 1);
        QCOMPARE(toasts.last().at(0).value<NotificationEntry>().message, expected);
        QCOMPARE(uploadCalls, 0);
        QVERIFY(!session.actionPending());
        QVERIFY(!host->remoteSceneLaunching());
        QVERIFY(!host->remoteSceneLaunched());
    };

    // Empty projects hide the entire panel, including both action buttons.
    QVERIFY(!upload->isVisible());
    QVERIFY(!remote->isVisible());
    QVERIFY(host->document()->addText({100, 100}, "Scene content"));
    QVERIFY(!upload->isVisible());
    const auto imagePath = temporary.filePath("uploadable.png");
    QImage image(32, 32, QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::darkCyan);
    QVERIFY(image.save(imagePath));
    auto* photo = host->document()->addPreparedFile(imagePath, image.size(), false, {0, 0});
    QVERIFY(photo);
    QTRY_VERIFY(photo->residencyReady());
    clickUnavailable(upload, "Launch a remote session first");
    clickUnavailable(remote, "No target screens available");
    host->setScreens({ScreenInfo(0, 800, 600, 0, 0, true)});
    clickUnavailable(remote, "Launch a remote session first");
    host->setOverlayActionsEnabled(true);
    clickUnavailable(remote, "The server is disconnected. Wait for the connection to be restored");
    host->timelinePlay();
    QVERIFY(host->testSceneLaunched());
    clickUnavailable(remote, "Pause the local preview before launching a remote scene");
    host->timelinePause();

    // The clickable UI does not bypass duplicate or queued-action guards.
    session.triggerUploadAction();
    QVERIFY(session.actionPending());
    QVERIFY(upload->isEnabled() && remote->isEnabled());
    session.triggerUploadAction();
    QCOMPARE(toasts.last().at(0).value<NotificationEntry>().message, "An action is already being processed. Please wait");
    session.toggleRemoteScene();
    QCOMPARE(toasts.last().at(0).value<NotificationEntry>().message, "An action is already being processed. Please wait");
    projectExists = false;
    QTRY_VERIFY(!session.actionPending());
    QCOMPARE(uploadCalls, 0);
    QCOMPARE(toasts.last().at(0).value<NotificationEntry>().message, "Create a project first");
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

void MediaOverlayTest::uploadedMediaKeepsUnloadWhileRemoteCommandsAreUnavailable()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    FileManager files;
    UploadManager uploads(&files, nullptr, temporary.filePath(QStringLiteral("Uploads")));
    QString error;
    std::unique_ptr<QuickCanvasHost> host(QuickCanvasHost::create(&error));
    QVERIFY2(host, qPrintable(error));
    host->setFileManager(&files);
    host->setProjectEditingEnabled(true);
    host->setOverlayActionsEnabled(true);
    const QString endpoint = QStringLiteral("retained-upload-workspace");
    const QString firstPath = temporary.filePath(QStringLiteral("first.png"));
    QImage image(32, 32, QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::darkCyan);
    QVERIFY(image.save(firstPath));
    auto* first = host->document()->addPreparedFile(firstPath, image.size(), false, {});
    QVERIFY(first);
    QTRY_VERIFY_WITH_TIMEOUT(first->residencyReady() && !first->fileId().isEmpty(), 10000);
    files.markFileUploadedToClient(first->fileId(), endpoint);
    bool projectExists = true;
    int dispatches = 0;
    const auto hasFiles = [&](bool uploaded) {
        for (const auto* media : host->document()->media()) {
            if (!media->isText()
                && files.isFileUploadedToClient(media->fileId(), endpoint) == uploaded)
                return true;
        }
        return false;
    };
    ClientWorkspaceViewModel session(endpoint, host.get(), [&] { ++dispatches; }, &uploads,
        [&] { return hasFiles(true); }, [&] { return hasFiles(false); }, [&] { return projectExists; });
    QQmlEngine engine;
    QQuickWindow window;
    window.resize(640, 480);
    std::unique_ptr<QQuickItem> harness(createRealMediaPanelHarness(engine, window, &session, &error));
    QVERIFY2(harness, qPrintable(error));
    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));
    harness->setSize(window.size());
    auto* upload = findVisualItem(harness.get(), QStringLiteral("uploadAction"));
    QVERIFY(upload);
    auto* sources = qobject_cast<MediaListModel*>(session.mediaModel());
    QVERIFY(sources);
    QCOMPARE(sources->rowCount(), 1);
    QTRY_VERIFY(findVisualItem(harness.get(), QStringLiteral("mediaStatus_0")));
    const auto verifyRetainedUpload = [&] {
        QCOMPARE(session.uploadState(), ClientWorkspaceViewModel::UploadState::Uploaded);
        QCOMPARE(session.uploadActionText(), QStringLiteral("Unload"));
        QCOMPARE(session.uploadActionIcon(), QUrl(QStringLiteral("qrc:/icons/icons/delete.svg")));
        QCOMPARE(session.uploadActionTone(), 2);
        QCOMPARE(upload->property("text").toString(), QStringLiteral("Unload"));
        QCOMPARE(upload->property("iconSource").toUrl(), session.uploadActionIcon());
        QCOMPARE(upload->property("tone").toInt(), 2);
        QVERIFY(upload->isVisible());
        QVERIFY(upload->isEnabled()); // Unavailable actions remain clickable to explain why.
        QCOMPARE(sources->data(sources->index(0), MediaListModel::UploadStateRole).toString(),
                 QStringLiteral("uploaded"));
        auto* status = findVisualItem(harness.get(), QStringLiteral("mediaStatus_0"));
        QVERIFY(status);
        QCOMPARE(status->property("text").toString(), QStringLiteral("Uploaded"));
        QVERIFY(files.isFileUploadedToClient(first->fileId(), endpoint));
    };
    verifyRetainedUpload();
    QVERIFY(session.uploadActionEnabled());
    const auto clickUpload = [&] {
        QTest::mouseClick(&window, Qt::LeftButton, Qt::NoModifier,
            upload->mapToScene({upload->width() / 2, upload->height() / 2}).toPoint());
    };
    for (int attempt = 0; attempt < 3; ++attempt) {
        host->setOverlayActionsEnabled(false);
        verifyRetainedUpload();
        QVERIFY(!session.uploadActionEnabled());
        QCOMPARE(session.uploadUnavailableReason(), QStringLiteral("Wait for the remote session to become ready"));
        QCOMPARE(upload->property("unavailableReason").toString(), session.uploadUnavailableReason());
        clickUpload();
        session.triggerUploadAction();
        QVERIFY(!session.actionPending());
        QCOMPARE(dispatches, 0);
        host->setOverlayActionsEnabled(true);
        verifyRetainedUpload();
        QVERIFY(session.uploadActionEnabled());
        QVERIFY(session.uploadUnavailableReason().isEmpty());
    }

    // Losing command readiness after accepting a click must prevent the queued
    // unload, while its semantic state and visible affordance stay unchanged.
    session.triggerUploadAction();
    QVERIFY(session.actionPending());
    QCOMPARE(dispatches, 0);
    host->setOverlayActionsEnabled(false);
    verifyRetainedUpload();
    QTRY_VERIFY(!session.actionPending());
    QCOMPARE(dispatches, 0);
    verifyRetainedUpload();
    host->setOverlayActionsEnabled(true);
    clickUpload();
    QTRY_COMPARE(dispatches, 1);
    QTRY_VERIFY(!session.actionPending());
    verifyRetainedUpload();

    // Real inventory loss changes the action even while remote commands are
    // unavailable; reconnecting alone never creates an uploaded inventory.
    host->setOverlayActionsEnabled(false);
    files.unmarkFileUploadedToClient(first->fileId(), endpoint);
    emit uploads.uiStateChanged();
    QCOMPARE(session.uploadState(), ClientWorkspaceViewModel::UploadState::Ready);
    QCOMPARE(upload->property("text").toString(), QStringLiteral("Upload"));
    QCOMPARE(upload->property("iconSource").toUrl(), QUrl(QStringLiteral("qrc:/icons/icons/upload.svg")));
    QCOMPARE(upload->property("tone").toInt(), 0);
    QCOMPARE(sources->data(sources->index(0), MediaListModel::UploadStateRole).toString(),
             QStringLiteral("not_uploaded"));
    QCOMPARE(findVisualItem(harness.get(), QStringLiteral("mediaStatus_0"))->property("text").toString(),
             QStringLiteral("Not uploaded"));
    host->setOverlayActionsEnabled(true);
    QCOMPARE(session.uploadState(), ClientWorkspaceViewModel::UploadState::Ready);
    QVERIFY(session.uploadActionEnabled());
    files.markFileUploadedToClient(first->fileId(), endpoint);
    emit uploads.uiStateChanged();
    verifyRetainedUpload();

    // Adding an unsent source legitimately changes Unload to Upload while the
    // already uploaded row retains its endpoint-specific source status.
    const QString secondPath = temporary.filePath(QStringLiteral("second.png"));
    image.fill(Qt::darkRed);
    QVERIFY(image.save(secondPath));
    auto* second = host->document()->addPreparedFile(secondPath, image.size(), false, {});
    QVERIFY(second);
    QTRY_VERIFY_WITH_TIMEOUT(second->residencyReady() && !second->fileId().isEmpty(), 10000);
    QTRY_COMPARE(sources->rowCount(), 2);
    QCOMPARE(session.uploadState(), ClientWorkspaceViewModel::UploadState::Ready);
    QCOMPARE(upload->property("text").toString(), QStringLiteral("Upload"));
    QCOMPARE(upload->property("tone").toInt(), 0);
    QCOMPARE(sources->data(sources->index(0), MediaListModel::UploadStateRole).toString(),
             QStringLiteral("uploaded"));
    QCOMPARE(sources->data(sources->index(1), MediaListModel::UploadStateRole).toString(),
             QStringLiteral("not_uploaded"));
    files.markFileUploadedToClient(second->fileId(), endpoint);
    emit uploads.uiStateChanged();
    verifyRetainedUpload();

    projectExists = false;
    session.refreshCapabilities();
    QCOMPARE(session.uploadState(), ClientWorkspaceViewModel::UploadState::Unavailable);
    QVERIFY(!session.uploadActionEnabled());
    QCOMPARE(session.uploadUnavailableReason(), QStringLiteral("Create a project first"));
    session.triggerUploadAction();
    QVERIFY(!session.actionPending());
    QCOMPARE(dispatches, 1);
    projectExists = true;
    session.refreshCapabilities();
    verifyRetainedUpload();
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
#ifdef Q_OS_MACOS
    // AppKit supplies decorations for a standard Qt window; the presenter
    // deliberately does not request the Windows-only title-hint flags.
    QCOMPARE(window->flags() & Qt::WindowType_Mask, Qt::WindowFlags(Qt::Window));
#else
    QVERIFY(window->flags().testFlag(Qt::WindowTitleHint));
#endif
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

void MediaOverlayTest::screenContentButtonTogglesAndPersists()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const auto previousProfile = RuntimeProfile::context();
    const auto restoreProfile = qScopeGuard([&] { RuntimeProfile::configure(previousProfile); });
    RuntimeProfileContext profile;
    profile.rootPath = directory.filePath(QStringLiteral("runtime"));
    profile.installationRootPath = directory.filePath(QStringLiteral("installation"));
    RuntimeProfile::configure(profile);
    QQmlEngine* previousEngine = QmlRuntime::engine();

    // Recreate the real controller and shell to verify that the viewer's
    // preference survives a new session without any project being open.
    for (int session = 0; session < 2; ++session) {
        QQmlEngine engine;
        QmlRuntime::setEngine(&engine);
        const auto restoreEngine = qScopeGuard([&] { QmlRuntime::setEngine(previousEngine); });
        ApplicationController controller(profile,
            {QStringLiteral("screen-content-test"), QStringLiteral("--server-url=ws://127.0.0.1:1")},
            nullptr, [] {
                QPromise<MediaBackendBootstrap::Result> promise;
                promise.start();
                promise.addResult({true, {}});
                promise.finish();
                return promise.future();
            });
        controller.start();
        QTRY_VERIFY_WITH_TIMEOUT(controller.ready(), 8000);
        QVERIFY(!controller.hasProject());
        QCOMPARE(controller.screenContentVisible(), session == 0);
        QQmlComponent component(&engine, QUrl(QStringLiteral(
            "qrc:/qt/qml/Mouffette/App/resources/qml/app/Main.qml")));
        std::unique_ptr<QObject> root(component.createWithInitialProperties({
            {QStringLiteral("controller"), QVariant::fromValue(&controller)}
        }));
        QVERIFY2(root, qPrintable(component.errorString()));
        auto* bootstrap = qobject_cast<QWindow*>(root->property("bootstrap").value<QObject*>());
        QVERIFY(bootstrap);
        bootstrap->hide();
        auto* window = qobject_cast<QQuickWindow*>(root->property("window").value<QObject*>());
        QVERIFY(window);
        window->showNormal();
        QVERIFY(QTest::qWaitForWindowExposed(window));
        window->resize(1200, 650);
        auto* button = findVisualItem(window->contentItem(), QStringLiteral("screenContentButton"));
        auto* connection = findVisualItem(window->contentItem(), QStringLiteral("connectionButton"));
        QVERIFY(button && connection);
        QVERIFY(button->isVisible() && button->isEnabled());
        QTRY_VERIFY(!button->property("iconOnly").toBool());
        QCOMPARE(button->parentItem(), connection->parentItem());
        QTRY_COMPARE(button->x(), connection->x() + connection->width()
                     + button->parentItem()->property("spacing").toReal());
        QCOMPARE(button->property("checked").toBool(), session == 0);
        QCOMPARE(button->property("text").toString(), session == 0
                     ? QStringLiteral("Hide screen content") : QStringLiteral("Show screen content"));
        QCOMPARE(button->property("iconSource").toUrl().fileName(), session == 0
                     ? QStringLiteral("visibility-off.svg") : QStringLiteral("visibility-on.svg"));

        const QString artifactDir = qEnvironmentVariable("MOUFFETTE_OVERLAY_ARTIFACT_DIR");
        const auto saveFrame = [&](const QString& name) {
            if (artifactDir.isEmpty()) return;
            QVERIFY(QDir().mkpath(artifactDir));
            QSignalSpy frames(window, &QQuickWindow::frameSwapped);
            window->update();
            QTRY_VERIFY(frames.size() > 0);
            const QString scale = qEnvironmentVariable("QT_SCALE_FACTOR");
            const QString suffix = scale.isEmpty() ? QString() : "-scale-" + scale;
            QVERIFY(window->grabWindow().save(QDir(artifactDir).filePath(name + suffix + ".png")));
        };
        if (session == 1) {
            saveFrame(QStringLiteral("screen-content-hidden-restored"));
            continue;
        }
        saveFrame(QStringLiteral("screen-content-visible"));
        const qreal fullTextWidth = button->property("textWidth").toReal();
        QSignalSpy changes(&controller, &ApplicationController::screenContentVisibleChanged);
        const auto clickButton = [&] {
            QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier,
                button->mapToScene({button->width() / 2, button->height() / 2}).toPoint());
        };
        clickButton();
        QTRY_VERIFY(!controller.screenContentVisible());
        QCOMPARE(changes.size(), 1);
        QCOMPARE(button->property("text").toString(), QStringLiteral("Show screen content"));
        QCOMPARE(button->property("iconSource").toUrl().fileName(), QStringLiteral("visibility-on.svg"));
        QVERIFY(!button->property("checked").toBool());
        QCOMPARE(button->property("textWidth").toReal(), fullTextWidth);
        QCOMPARE(RuntimeProfile::readSettings().value(QStringLiteral("screenContentVisible")).toString(),
                 QStringLiteral("false"));
        saveFrame(QStringLiteral("screen-content-hidden"));

        controller.showHistory();
        QTRY_COMPARE(controller.applicationPage(), ApplicationController::ApplicationPage::History);
        QVERIFY(button->isVisible());
        QVERIFY(!controller.screenContentVisible());
        controller.goBack();
        QTRY_COMPARE(controller.applicationPage(), ApplicationController::ApplicationPage::Clients);
        clickButton();
        QTRY_VERIFY(controller.screenContentVisible());
        QCOMPARE(changes.size(), 2);
        QCOMPARE(button->property("text").toString(), QStringLiteral("Hide screen content"));
        QCOMPARE(button->property("iconSource").toUrl().fileName(), QStringLiteral("visibility-off.svg"));
        QVERIFY(button->property("checked").toBool());
        QCOMPARE(button->property("textWidth").toReal(), fullTextWidth);
        QCOMPARE(RuntimeProfile::readSettings().value(QStringLiteral("screenContentVisible")).toString(),
                 QStringLiteral("true"));

        // The compact button has the same action and persists the hidden state.
        window->resize(480, 650);
        QTRY_VERIFY(button->property("iconOnly").toBool());
        QVERIFY(button->isVisible());
        QTRY_VERIFY(button->mapToScene({0, 0}).x() >= 0);
        QTRY_VERIFY(button->mapToScene({button->width(), 0}).x() <= window->width());
        clickButton();
        QTRY_VERIFY(!controller.screenContentVisible());
        QCOMPARE(changes.size(), 3);
        QCOMPARE(RuntimeProfile::readSettings().value(QStringLiteral("screenContentVisible")).toString(),
                 QStringLiteral("false"));
        saveFrame(QStringLiteral("screen-content-hidden-narrow"));
    }
}

void MediaOverlayTest::systemAudioButtonPersistsIndependentlyOfScreen()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const auto previousProfile = RuntimeProfile::context();
    const auto restoreProfile = qScopeGuard([&] { RuntimeProfile::configure(previousProfile); });
    RuntimeProfileContext profile;
    profile.rootPath = directory.filePath(QStringLiteral("runtime"));
    profile.installationRootPath = directory.filePath(QStringLiteral("installation"));
    RuntimeProfile::configure(profile);
    QQmlEngine* previousEngine = QmlRuntime::engine();

    for (int launch = 0; launch < 2; ++launch) {
        QQmlEngine engine;
        QmlRuntime::setEngine(&engine);
        const auto restoreEngine = qScopeGuard([&] { QmlRuntime::setEngine(previousEngine); });
        ApplicationController controller(profile,
            {QStringLiteral("remote-audio-ui-test"), QStringLiteral("--server-url=ws://127.0.0.1:1")},
            nullptr, [] {
                QPromise<MediaBackendBootstrap::Result> promise;
                promise.start();
                promise.addResult({true, {}});
                promise.finish();
                return promise.future();
            });
        controller.start();
        QTRY_VERIFY_WITH_TIMEOUT(controller.ready(), 8000);
        QCOMPARE(controller.systemAudioEnabled(), launch == 0);
        QVERIFY(controller.screenContentVisible());
        QVERIFY(!controller.settingsScreenSharingEnabled());
        QCOMPARE(controller.settingsAudioSharingEnabled(), launch == 1);
        QQmlComponent component(&engine, QUrl(QStringLiteral(
            "qrc:/qt/qml/Mouffette/App/resources/qml/app/Main.qml")));
        std::unique_ptr<QObject> root(component.createWithInitialProperties({
            {QStringLiteral("controller"), QVariant::fromValue(&controller)}
        }));
        QVERIFY2(root, qPrintable(component.errorString()));
        auto* bootstrap = qobject_cast<QWindow*>(root->property("bootstrap").value<QObject*>());
        QVERIFY(bootstrap);
        bootstrap->hide();
        auto* window = qobject_cast<QQuickWindow*>(root->property("window").value<QObject*>());
        QVERIFY(window);
        window->showNormal();
        QVERIFY(QTest::qWaitForWindowExposed(window));
        window->resize(1400, 650);
        auto* audio = findVisualItem(window->contentItem(), QStringLiteral("systemAudioButton"));
        auto* screen = findVisualItem(window->contentItem(), QStringLiteral("screenContentButton"));
        QVERIFY(audio && screen);
        QVERIFY(audio->isVisible() && audio->isEnabled());
        QCOMPARE(audio->parentItem(), screen->parentItem());
        QTRY_COMPARE(audio->x(), screen->x() + screen->width()
            + audio->parentItem()->property("spacing").toReal());
        QCOMPARE(audio->property("checked").toBool(), launch == 0);
        QCOMPARE(audio->property("text").toString(), launch == 0
            ? QStringLiteral("Stop system audio") : QStringLiteral("Play system audio"));
        QCOMPARE(audio->property("iconSource").toUrl().fileName(), launch == 0
            ? QStringLiteral("volume-off.svg") : QStringLiteral("volume-on.svg"));

        // Listening is an application action; no mute control remains in the canvas.
        QQuickWindow toolbarWindow;
        QString error;
        std::unique_ptr<QQuickItem> toolbar(createCanvasToolbarHarness(engine, toolbarWindow, &error, &controller));
        QVERIFY2(toolbar, qPrintable(error));
        QVERIFY(!findVisualItem(toolbar.get(), QStringLiteral("canvasRemoteAudioButton")));
        if (launch == 1) continue;

        // The settings dialog commits the two source permissions separately.
        auto* settingsButton = findVisualItem(window->contentItem(), QStringLiteral("settingsButton"));
        auto* settingsDialog = root->findChild<QObject*>(QStringLiteral("settingsDialog"));
        QVERIFY(settingsButton && settingsDialog);
        for (bool shareScreen : {true, false}) {
            QVERIFY(QMetaObject::invokeMethod(settingsButton, "clicked"));
            QTRY_VERIFY(settingsDialog->property("opened").toBool());
            QTRY_VERIFY(findVisualItem(window->contentItem(), QStringLiteral("settingsAudioSharingEnabled")));
            auto* screenSharing = findVisualItem(window->contentItem(), QStringLiteral("settingsScreenSharingEnabled"));
            auto* audioSharing = findVisualItem(window->contentItem(), QStringLiteral("settingsAudioSharingEnabled"));
            auto* save = findVisualItem(window->contentItem(), QStringLiteral("settingsSave"));
            QVERIFY(screenSharing && audioSharing && save);
            QCOMPARE(screenSharing->property("text").toString(), QStringLiteral("Share my screen"));
            QCOMPARE(audioSharing->property("text").toString(), QStringLiteral("Share my system audio"));
            QCOMPARE(screenSharing->property("checked").toBool(), controller.settingsScreenSharingEnabled());
            QCOMPARE(audioSharing->property("checked").toBool(), controller.settingsAudioSharingEnabled());
            screenSharing->setProperty("checked", shareScreen);
            audioSharing->setProperty("checked", !shareScreen);
            QVERIFY(QMetaObject::invokeMethod(save, "clicked"));
            QTRY_COMPARE(controller.settingsScreenSharingEnabled(), shareScreen);
            QTRY_COMPARE(controller.settingsAudioSharingEnabled(), !shareScreen);
            QTRY_VERIFY(!settingsDialog->property("opened").toBool());
            QTRY_VERIFY(!save->isVisible());
        }

        const auto click = [&](QQuickItem* button) {
            // Native first exposure can resize the window at high DPI. Wait
            // for Row's polish before measuring the button's click position.
            QSignalSpy frames(window, &QQuickWindow::frameSwapped);
            window->update();
            QTRY_VERIFY(!frames.isEmpty());
            QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier,
                button->mapToScene({button->width() / 2, button->height() / 2}).toPoint());
        };
        const qreal stableWidth = audio->property("textWidth").toReal();
        QSignalSpy audioChanges(&controller, &ApplicationController::systemAudioEnabledChanged);
        QSignalSpy screenChanges(&controller, &ApplicationController::screenContentVisibleChanged);
        click(audio);
        QTRY_VERIFY(!controller.systemAudioEnabled());
        QVERIFY(controller.screenContentVisible());
        QCOMPARE(audioChanges.size(), 1);
        QCOMPARE(screenChanges.size(), 0);
        QCOMPARE(audio->property("text").toString(), QStringLiteral("Play system audio"));
        QCOMPARE(audio->property("textWidth").toReal(), stableWidth);
        QCOMPARE(RuntimeProfile::readSettings().value(QStringLiteral("systemAudioEnabled")).toString(),
            QStringLiteral("false"));

        click(screen);
        QTRY_VERIFY(!controller.screenContentVisible());
        QVERIFY(!controller.systemAudioEnabled());
        QCOMPARE(audioChanges.size(), 1);
        click(audio);
        QTRY_VERIFY(controller.systemAudioEnabled());
        QVERIFY(!controller.screenContentVisible());
        QCOMPARE(screenChanges.size(), 1);
        click(screen);
        QTRY_VERIFY(controller.screenContentVisible());
        QVERIFY(controller.systemAudioEnabled());

        controller.showHistory();
        QTRY_COMPARE(controller.applicationPage(), ApplicationController::ApplicationPage::History);
        QVERIFY(audio->isVisible() && audio->isEnabled());
        window->resize(480, 650);
        QTRY_VERIFY(audio->property("iconOnly").toBool());
        QTRY_VERIFY(audio->mapToScene({0, 0}).x() >= 0);
        QTRY_VERIFY(audio->mapToScene({audio->width(), 0}).x() <= window->width());
        click(audio);
        QTRY_VERIFY(!controller.systemAudioEnabled());
        QVERIFY(controller.screenContentVisible());
        QCOMPARE(audioChanges.size(), 3);
        controller.goBack();
        QTRY_COMPARE(controller.applicationPage(), ApplicationController::ApplicationPage::Clients);
        QVERIFY(!controller.systemAudioEnabled());
        QCOMPARE(RuntimeProfile::readSettings().value(QStringLiteral("systemAudioEnabled")).toString(),
            QStringLiteral("false"));
    }
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
    appWindow->resize(1200, 650);
    appWindow->showNormal();
    QVERIFY(QTest::qWaitForWindowExposed(appWindow));
    appWindow->requestActivate();
    QVERIFY(QTest::qWaitForWindowActive(appWindow));
    // First exposure can fit the window to a smaller screen at high DPR.
    // Establish the same wide size used below and wait for layout before
    // recording its position and clicking the button.
    appWindow->resize(1200, 650);
    QCOMPARE(appWindow->width(), 1200);
    QTRY_COMPARE(localStatus->y(), 0.0);
    QTRY_COMPARE(memory->mapToScene({0, 0}).y(), topBar->mapToScene({0, 0}).y());
    const qreal toolbarY = memory->mapToScene({0, 0}).y();
    const QPoint memoryCenter = memory->mapToScene({memory->width() / 2, memory->height() / 2}).toPoint();
    QTest::mouseClick(appWindow, Qt::LeftButton, Qt::NoModifier, memoryCenter);
    QTRY_VERIFY(memory->property("checked").toBool());
    // ListView creates and lays out its summary header when the popup opens.
    QTRY_VERIFY(findVisualItem(appWindow->contentItem(), QStringLiteral("memoryDistributionBar")));
    auto* bar = findVisualItem(appWindow->contentItem(), QStringLiteral("memoryDistributionBar"));
    QTRY_VERIFY(bar->isVisible() && bar->width() > 400);
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
    for (const auto* name : {"connectionButton", "screenContentButton", "systemAudioButton", "historyButton", "settingsButton"}) {
        auto* button = findVisualItem(topBar, QString::fromLatin1(name));
        QVERIFY(button && button->isVisible());
        QCOMPARE(button->mapToScene({0, 0}).y(), toolbarY);
        // Row positions its children during the next polish after resize.
        QTRY_VERIFY(button->mapToScene({0, 0}).x() >= topBar->mapToScene({0, 0}).x());
        QTRY_VERIFY(button->mapToScene({button->width(), 0}).x() <= appWindow->width());
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
    appWindow->resize(1200, 650);
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
             {QStringLiteral("remoteStatusText"), QStringLiteral("CONNECTED")}}}
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
    QPointer<QQuickItem> mediaList = findVisualItem(page, QStringLiteral("mediaListPanel"));
    QVERIFY(mediaList);
    QVERIFY(mediaList->isVisible());
    QPointer<QQuickItem> timelineToggle = findVisualItem(page, QStringLiteral("canvasTimelineButton"));
    QVERIFY(timelineToggle && timelineToggle->isVisible() && timelineToggle->isEnabled());
    if (videoSelected) {
        QTRY_VERIFY_WITH_TIMEOUT(findVisualItem(page, QStringLiteral("videoVolumeSlider"))->isVisible(), 8000);
    }

    if (launchTestScene) {
        QTRY_VERIFY(host->testSceneActionEnabled());
        qobject_cast<TimelineController*>(session.timeline())->togglePlayback();
        QVERIFY(qobject_cast<TimelineController*>(session.timeline())->playing());
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
    QVERIFY(timelineToggle && timelineToggle->isVisible() && timelineToggle->isEnabled());
    QCOMPARE(findVisualItem(page, QStringLiteral("canvasTimelineButton")), timelineToggle.data());
    auto* canvasViewport = findVisualItem(page, QStringLiteral("activeCanvasLoader"));
    QVERIFY(canvasViewport);
    QTRY_COMPARE(timelineToggle->mapToItem(canvasViewport, {0, 0}).x(), 10.0); // No gap left by unloaded tools.
    auto* timelinePanel = findVisualItem(page, QStringLiteral("sceneTimeline"));
    auto* timelineBody = findVisualItem(page, QStringLiteral("timelineEditorBody"));
    QVERIFY(timelinePanel && timelineBody);
    const auto expandedHeight = timelinePanel->height();
    const auto toggleTimeline = [&] {
        QTest::mouseClick(&window, Qt::LeftButton, Qt::NoModifier,
            timelineToggle->mapToScene({timelineToggle->width()/2, timelineToggle->height()/2}).toPoint());
    };
    toggleTimeline();
    QTRY_VERIFY(!timelineBody->isVisible());
    QVERIFY(!timelineToggle->property("toggled").toBool());
    QCOMPARE(timelinePanel->height(), timelinePanel->property("transportHeight").toReal());
    toggleTimeline();
    QTRY_VERIFY(timelineBody->isVisible());
    QVERIFY(timelineToggle->property("toggled").toBool());
    QCOMPARE(timelinePanel->height(), expandedHeight);
    QVERIFY(!host->controller()->editingEnabled());
    CanvasMedia* selectionBeforeInput = host->document()->selectedMedia();

    // Panning/keyboard input still traverses CanvasRoot while chrome is absent.
    QTest::keyRelease(&window, Qt::Key_Shift);
    QTest::mouseClick(&window, Qt::LeftButton, Qt::NoModifier, QPoint(30, 300));
    QCOMPARE(host->document()->selectedMedia(), selectionBeforeInput);

    if (launchTestScene)
        qobject_cast<TimelineController*>(session.timeline())->togglePlayback();
    else
        host->document()->setEditsLocked(false);
    QTRY_VERIFY(host->controller()->editingEnabled());
    host->controller()->selectMedia(media->mediaId());
    for (const auto& name : editorNames)
        QTRY_VERIFY2(findVisualItem(page, name), qPrintable(name));
    QVERIFY(remoteCursor && remoteCursor->isVisible());
    QVERIFY(findVisualItem(page, QStringLiteral("sceneTimeline"))->isVisible());
    QCOMPARE(findVisualItem(page, QStringLiteral("canvasTimelineButton")), timelineToggle.data());
    QVERIFY(!findVisualItem(page, QStringLiteral("sceneSettingsTab")));
    QCOMPARE(findVisualItem(page, QStringLiteral("mediaListPanel")), mediaList.data());
}

void MediaOverlayTest::mediaSettingsElementBindings_data()
{
    QTest::addColumn<bool>("dark");
    QTest::newRow("light") << false;
    QTest::newRow("dark") << true;
}

void MediaOverlayTest::mediaSettingsElementBindings()
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
    auto* elementPage = findVisualItem(harness.get(), QStringLiteral("elementSettingsPage"));
    QVERIFY(panel);
    QVERIFY(elementPage);
    QVERIFY(!findVisualItem(harness.get(), QStringLiteral("sceneSettingsTab")));
    QVERIFY(!findVisualItem(harness.get(), QStringLiteral("sceneSettingsPage")));
    QVERIFY(!findVisualItem(harness.get(), QStringLiteral("elementSettingsTitle")));

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
    QVERIFY(elementPage->isVisible());
    const QImage frame = window.grabWindow();
    QVERIFY(nearColor(imagePixel(frame, window.size(), panel->mapToScene({1.1, 1.1})), window.color()));
    QVERIFY(nearColor(imagePixel(frame, window.size(), panel->mapToScene({panel->width() - 2.1, 1.1})), window.color()));
    auto* contentFlick = findVisualItem(harness.get(), QStringLiteral("settingsContentFlick"));
    auto* scrollBar = findVisualItem(harness.get(), QStringLiteral("settingsOverlayScrollBar"));
    QVERIFY(contentFlick);
    QVERIFY(scrollBar);
    QCOMPARE(contentFlick->y(), 1.0);
    QCOMPARE(contentFlick->height(), panel->height() - 2);
    for (const auto* name : {"opacityCheck", "volumeCheck", "textColorCheck", "highlightCheck",
             "textBorderWidthCheck", "textBorderColorCheck", "fontWeightCheck", "underlineCheck",
             "italicCheck", "uppercaseCheck"})
        QVERIFY2(findVisualItem(harness.get(), QString::fromLatin1(name)), name);
    for (const auto* name : {"displayAutomaticallyCheck", "displayDelayCheck", "hideDelayCheck",
             "hideWhenVideoEndsCheck", "unmuteAutomaticallyCheck", "playAutomaticallyCheck",
             "repeatCheck", "imageFadeInCheck", "imageFadeOutCheck", "audioFadeInCheck",
             "audioFadeOutCheck", "videoStartButton", "videoEndButton"})
        QVERIFY2(!findVisualItem(harness.get(), QString::fromLatin1(name)), name);
    window.resize(640, 230);
    harness->setSize(window.size());
    QTRY_VERIFY(contentFlick->property("overflowing").toBool());
    QVERIFY(scrollBar->isVisible());
    ulong wheelTimestamp = 0;
    const auto wheel = [&](QPointF localPoint, QPoint pixels, QPoint angles,
                           Qt::KeyboardModifiers modifiers = Qt::NoModifier,
                           Qt::ScrollPhase phase = Qt::NoScrollPhase) {
        auto* deviceState = QPointingDevicePrivate::get(const_cast<QPointingDevice*>(QPointingDevice::primaryPointingDevice()));
        const auto originalType = deviceState->deviceType;
        const auto restoreDevice = qScopeGuard([&] { deviceState->deviceType = originalType; });
        deviceState->deviceType = !pixels.isNull() || phase != Qt::NoScrollPhase
            ? QInputDevice::DeviceType::TouchPad : QInputDevice::DeviceType::Mouse;
        const auto point = panel->mapToScene(localPoint);
        QWheelEvent event(point, window.mapToGlobal(point.toPoint()), pixels, angles,
            Qt::NoButton, modifiers, phase, false);
        event.setTimestamp(wheelTimestamp += 16);
        QCoreApplication::sendEvent(&window, &event);
    };
    const auto isolatedWheel = [&] {
        return harness->property("canvasWheelCount").toInt() == 0;
    };
    const QPointF inside(panel->width()/2, panel->height()/2);
    wheel(inside, {}, {0, -120});
    QTRY_VERIFY(contentFlick->property("contentY").toReal() > 0);
    wheel(inside, {0, -30}, {}, Qt::NoModifier, Qt::ScrollBegin);
    wheel(inside, {0, -30}, {}, Qt::NoModifier, Qt::ScrollUpdate);
    wheel(inside, {}, {}, Qt::NoModifier, Qt::ScrollEnd);
    QVERIFY(isolatedWheel());
    QVERIFY(QMetaObject::invokeMethod(contentFlick, "cancelFlick"));
    const qreal scrollMaximum = contentFlick->property("contentHeight").toReal() - contentFlick->height();
    for (const int direction : {-1, 1}) {
        contentFlick->setProperty("contentY", direction < 0 ? scrollMaximum : 0.0);
        for (auto modifiers : {Qt::NoModifier, Qt::ControlModifier, Qt::AltModifier, Qt::ShiftModifier}) {
            wheel(inside, {}, {0, direction * 120}, modifiers);
            wheel(inside, {0, direction * 30}, {}, modifiers, Qt::ScrollBegin);
            wheel(inside, {0, direction * 30}, {}, modifiers, Qt::ScrollUpdate);
            wheel(inside, {0, direction * 30}, {}, modifiers, Qt::ScrollMomentum);
            wheel(inside, {}, {}, modifiers, Qt::ScrollEnd);
            wheel(inside, {direction * 30, 0}, {}, modifiers);
            QVERIFY(isolatedWheel());
        }
    }
    // The border and blank padding also belong to the overlay.
    wheel({0.5, panel->height()/2}, {0, -30}, {});
    QVERIFY(isolatedWheel());
    QVERIFY(QMetaObject::invokeMethod(contentFlick, "cancelFlick"));
    contentFlick->setProperty("contentY", 0.0);
    const auto overflowArtifacts = qEnvironmentVariable("MOUFFETTE_OVERLAY_ARTIFACT_DIR");
    if (!overflowArtifacts.isEmpty()) {
        QVERIFY(QDir().mkpath(overflowArtifacts));
        QTest::mouseMove(&window, scrollBar->mapToScene({scrollBar->width()/2, scrollBar->height()/2}).toPoint());
        QTest::qWait(250);
        QVERIFY(window.grabWindow().save(QDir(overflowArtifacts).filePath(
            dark ? "settings-scrollbar-dark.png" : "settings-scrollbar-light.png")));
    }

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
        wheel({panel->width()/2, panel->height()/2}, {}, {0, -120});
        wheel({panel->width()/2, panel->height()/2}, {0, -30}, {}, Qt::ControlModifier);
        QVERIFY(isolatedWheel());
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
    if (contentFits) QVERIFY(!scrollBar->isVisible());
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
             {QStringLiteral("remoteStatusText"), QStringLiteral("CONNECTED")}}}
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

    // Video transport belongs exclusively to the persistent scene timeline.
    for (const auto* name : {"videoProgressSlider", "videoStartButton", "videoEndButton",
                            "videoStartMarker", "videoEndMarker", "testSceneAction"})
        QVERIFY2(!findVisualItem(page, QString::fromLatin1(name)), name);
    auto* timelinePanel = findVisualItem(page, QStringLiteral("sceneTimeline"));
    auto* clipViewport = findVisualItem(page, QStringLiteral("timelineClipViewport"));
    QVERIFY(timelinePanel && timelinePanel->isVisible());
    QVERIFY(clipViewport && clipViewport->isVisible());
    // Repeater delegates belong to the visual tree, and its buffered rows can
    // be hidden; require a rendered track rather than QObject ownership.
    QVERIFY(findVisualItem(timelinePanel, QStringLiteral("timelineClipTrack"), true));
    QVERIFY(findVisualItem(page, QStringLiteral("timelinePlayPause")));
    const qreal pageBorderWidth = QQmlProperty::read(page, QStringLiteral("border.width")).toReal();
    QCOMPARE(timelinePanel->x(), pageBorderWidth);
    QCOMPARE(timelinePanel->width(), page->width() - 2 * pageBorderWidth);
    QCOMPARE(timelinePanel->y() + timelinePanel->height(), page->height() - pageBorderWidth);
    auto* canvasLoader = findVisualItem(page, QStringLiteral("activeCanvasLoader"));
    QVERIFY(canvasLoader);
    QCOMPARE(canvasLoader->x(), pageBorderWidth);
    QCOMPARE(canvasLoader->y(), pageBorderWidth);
    QCOMPARE(canvasLoader->width(), timelinePanel->width());
    QCOMPARE(canvasLoader->y() + canvasLoader->height() + 1, timelinePanel->y());
    QTRY_VERIFY(!qobject_cast<TimelineController*>(session.timeline())->clips().isEmpty());
    qobject_cast<TimelineController*>(session.timeline())->seek(1200);
    QTRY_COMPARE(qobject_cast<TimelineController*>(session.timeline())->positionMs(), 1200);
    QVERIFY(!video->isPlaying());
    qobject_cast<TimelineController*>(session.timeline())->placeStop();
    QCOMPARE(qobject_cast<TimelineController*>(session.timeline())->stopTimeMs(), 1200);
    QVERIFY(findVisualItem(page, QStringLiteral("timelineStopMarker"))->isVisible());
    qobject_cast<TimelineController*>(session.timeline())->removeStop();
    QVERIFY(!findVisualItem(page, QStringLiteral("timelineStopMarker"))->isVisible());

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
        ListElement { severityKind: 0; message: "Connected"; peers: []; dismissing: false }
        ListElement { severityKind: 1; message: "Connection lost"; peers: []; dismissing: false }
        ListElement { severityKind: 2; message: "Reconnecting"; peers: []; dismissing: false }
        ListElement { severityKind: 3; message: "Project loaded"; peers: []; dismissing: false }
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
