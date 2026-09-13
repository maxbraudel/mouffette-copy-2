#include <QColor>
#include <QImage>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QQuickItem>
#include <QQuickWindow>
#include <QScopeGuard>
#include <QtTest>

#include "frontend/rendering/canvas/MediaListModel.h"
#include "frontend/rendering/canvas/QuickCanvasHost.h"
#include "frontend/qml/CanvasSessionViewModel.h"
#include "backend/domain/canvas/CanvasDocument.h"

class MediaOverlayTest final : public QObject
{
    Q_OBJECT

private slots:
    void segmentedStatusFillReachesBothEdges_data();
    void segmentedStatusFillReachesBothEdges();
    void segmentedStatusKeepsSingleTopBorder();
    void mediaPanelVisibilityAnchorInteractionAndScroll();
    void mediaCountTracksRealCanvasInsertions();
    void overlayButtonHoverIsImmediate();
    void canvasToolbarUsesOverlaySwitchAndSegmentedTools();
    void toastMatchesLegacyBottomLeftDoubleBackground();
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

    QtObject {
        id: fakeSession
        property var mediaModel: host.externalModel
        property int mediaCount: host.testMediaCount
        property string remoteSceneActionText: "Launch Remote Scene"
        property bool remoteSceneActionEnabled: true
        property string testSceneActionText: "Launch Test Scene"
        property bool testSceneActionEnabled: true
        property string uploadActionText: "Upload"
        property int uploadActionTone: 0
        property bool uploadActionEnabled: true
        function selectMedia(mediaId, additive) { host.selectionCalls += 1 }
        function toggleRemoteScene() {}
        function toggleTestScene() {}
        function triggerUploadAction() {}
    }

    MediaListPanel {
        objectName: "mediaListPanel"
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        anchors.margins: 10
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
        anchors.margins: 10
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
    QtObject {
        id: fakeSession
        property bool settingsVisible: false
        property bool actionsEnabled: true
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
    harness->setSize(window.size());
    QCoreApplication::processEvents();
    QVERIFY(!panel->isVisible());

    QVariantList rows;
    rows.append(QVariantMap{{QStringLiteral("mediaId"), QStringLiteral("media-0")},
                            {QStringLiteral("displayName"), QStringLiteral("Image 0")},
                            {QStringLiteral("mediaType"), QStringLiteral("image")},
                            {QStringLiteral("uploadState"), QStringLiteral("not_uploaded")},
                            {QStringLiteral("uploadProgress"), 0}});
    model.updateFromList(rows);
    harness->setProperty("testMediaCount", 1);
    QCoreApplication::processEvents();

    QVERIFY(panel->isVisible());
    QCOMPARE(qRound(panel->x() + panel->width()), window.width() - 10);
    QCOMPARE(qRound(panel->y() + panel->height()), window.height() - 10);

    QTRY_COMPARE(list->property("count").toInt(), 1);
    QTRY_VERIFY(findVisualItem(harness.get(), QStringLiteral("mediaRow_0")));
    auto* firstRow = findVisualItem(harness.get(), QStringLiteral("mediaRow_0"));
    QVERIFY(firstRow);
    const QPoint clickPoint = firstRow->mapToScene(
        QPointF(firstRow->width() / 2.0, firstRow->height() / 2.0)).toPoint();
    QTest::mouseClick(&window, Qt::LeftButton, Qt::NoModifier, clickPoint);
    QCoreApplication::processEvents();
    QCOMPARE(harness->property("selectionCalls").toInt(), 1);

    for (int index = 1; index < 30; ++index) {
        rows.append(QVariantMap{
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
    QCOMPARE(qRound(panel->x() + panel->width()), window.width() - 10);
    QCOMPARE(qRound(panel->y() + panel->height()), window.height() - 10);
}

void MediaOverlayTest::mediaCountTracksRealCanvasInsertions()
{
    QQmlEngine engine;
    QQuickWindow window;
    window.resize(640, 480);
    QString error;
    std::unique_ptr<QuickCanvasHost> host(QuickCanvasHost::create(&error));
    QVERIFY2(host, qPrintable(error));
    CanvasSessionViewModel session(
        QStringLiteral("media-count-session"), host.get(), [] {}, nullptr,
        [] { return false; }, [] { return true; });
    QSignalSpy countChanged(&session, &CanvasSessionViewModel::mediaCountChanged);
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
    QCOMPARE(qRound(panel->x() + panel->width()), window.width() - 10);
    QCOMPARE(qRound(panel->y() + panel->height()), window.height() - 10);
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

void MediaOverlayTest::canvasToolbarUsesOverlaySwitchAndSegmentedTools()
{
    QQmlEngine engine;
    QQuickWindow window;
    window.resize(240, 100);
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

    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));
    const QPoint textCenter = text->mapToScene(
        QPointF(text->width() / 2.0, text->height() / 2.0)).toPoint();
    QTest::mouseClick(&window, Qt::LeftButton, Qt::NoModifier, textCenter);
    QTRY_VERIFY(text->property("toggled").toBool());
    QVERIFY(!selection->property("toggled").toBool());
}

void MediaOverlayTest::toastMatchesLegacyBottomLeftDoubleBackground()
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
