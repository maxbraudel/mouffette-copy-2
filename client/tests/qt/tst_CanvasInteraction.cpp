#include "frontend/rendering/canvas/CanvasQmlTypes.h"
#include "frontend/rendering/canvas/MediaListModel.h"
#include "frontend/rendering/canvas/TextOutlineItem.h"

#include <QEvent>
#include <QFontDatabase>
#include <QGuiApplication>
#include <QMouseEvent>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QQuickWindow>
#include <QPointer>
#include <QRectF>
#include <QRegularExpression>
#include <QSignalSpy>
#include <QStyleHints>
#include <QTest>
#include <QtQuick/private/qquicktextedit_p.h>

// A deterministic host for the real canvas. Only backend storage/publication
// is simulated: all picking, grabs, selection, editing and drag handlers are
// the production QML, driven by actual window mouse events.
class CanvasFixture : public QObject {
    Q_OBJECT
public:
    MediaListModel model;
    QQmlEngine engine;
    QQuickWindow window;
    std::unique_ptr<QObject> object;
    QQuickItem* root = nullptr;
    QVariantList media;
    QStringList selected;
    int commitCount = 0;
    int selectionRequestCount = 0;
    int textCreateCount = 0;
    int clearRequestCount = 0;
    QString error;
    bool emulateResizePublication = false;
    int resizePublicationCount = 0;
    QString pendingResizeId;
    QRectF originalResizeRect;
    QRectF pendingResizeRect;
    qreal resizeBaseWidth = 0.0;

    bool initialize()
    {
        QQmlComponent component(&engine, QUrl::fromLocalFile(TEST_SOURCE_DIR "/resources/qml/CanvasRoot.qml"));
        if (component.isLoading()) {
            QSignalSpy status(&component, &QQmlComponent::statusChanged);
            status.wait(5000);
        }
        if (!component.isReady()) { error = component.errorString(); return false; }
        object.reset(component.create());
        root = qobject_cast<QQuickItem*>(object.get());
        if (!root) { error = component.errorString(); return false; }
        window.resize(1000, 760);
        root->setSize(window.size());
        root->setParentItem(window.contentItem());
        root->setProperty("mediaListModel", QVariant::fromValue(&model));
        connect(root, SIGNAL(mediaSelectRequested(QString,bool)), this, SLOT(select(QString,bool)));
        connect(root, SIGNAL(clearSelectionRequested()), this, SLOT(clear()));
        connect(root, SIGNAL(textCommitRequested(QString,QString)), this, SLOT(commit(QString,QString)));
        connect(root, SIGNAL(textLiveUpdateRequested(QString,QString)), this, SLOT(liveText(QString,QString)));
        connect(root, SIGNAL(mediaMoveEnded(QString,double,double,bool)), this, SLOT(move(QString,double,double,bool)));
        connect(root, SIGNAL(mediaResizeRequested(QString,QString,double,double,bool,bool)),
                this, SLOT(captureResize(QString,QString,double,double,bool,bool)));
        connect(root, SIGNAL(mediaResizeEnded(QString)), this, SLOT(publishResize(QString)));
        connect(root, SIGNAL(textCreateRequested(double,double)), this, SLOT(createText(double,double)));
        window.show();
        if (!QTest::qWaitForWindowExposed(&window)) return false;
        // macOS may clamp the requested size to the available logical screen
        // (notably with QT_SCALE_FACTOR=2). Render and click in that real size.
        root->setSize(window.size());
        window.installEventFilter(this);
        return true;
    }

    bool eventFilter(QObject* watched, QEvent* event) override
    {
        if (watched == &window && (event->type() == QEvent::MouseButtonPress
                                  || event->type() == QEvent::MouseButtonRelease
                                  || event->type() == QEvent::MouseButtonDblClick)) {
            const QPointF position = static_cast<QMouseEvent*>(event)->position();
            QTest::qVerify(QRectF(QPointF(), window.size()).contains(position),
                          "Native click is inside the exposed window",
                          qPrintable(QString("point=(%1,%2), actual window=%3x%4")
                              .arg(position.x()).arg(position.y()).arg(window.width()).arg(window.height())),
                          __FILE__, __LINE__);
        }
        return QObject::eventFilter(watched, event);
    }

    QPoint backgroundPoint() const
    {
        // Fixtures reserve the top-left corner: media begin at (100,150),
        // and their floating controls stay below it, even in a clamped window.
        return root->mapToScene(QPointF(24, 24)).toPoint();
    }

    void add(const QString& id, const QString& type, qreal x, qreal y)
    {
        media.append(QVariantMap {{"mediaId", id}, {"mediaType", type},
            {"x", x}, {"y", y}, {"width", 280}, {"height", 170}, {"scale", 1.0}, {"z", 1},
            {"contentVisible", true}, {"contentOpacity", 1.0}, {"displayName", id},
            {"textContent", "Canvas text"}, {"textFontPixelSize", 40},
            {"textHighlightEnabled", true}, {"textHighlightColor", "#ffff00"}});
        publish();
    }

    void publish()
    {
        model.updateFromList(media);
        root->setProperty("mediaModel", media);
        publishSelection();
        QCoreApplication::processEvents();
    }

    void publishSelection()
    {
        QVariantList chrome;
        for (const QVariant& value : media) {
            const auto entry = value.toMap();
            if (selected.contains(entry.value("mediaId").toString())) chrome.append(entry);
        }
        root->setProperty("selectionChromeModel", chrome);
    }

    QVariantMap entry(const QString& id) const
    {
        for (const QVariant& value : media) {
            const auto item = value.toMap();
            if (item.value("mediaId") == id) return item;
        }
        return {};
    }

    void change(const QString& id, const QVariantMap& values)
    {
        for (auto& value : media) {
            auto item = value.toMap();
            if (item.value("mediaId") != id) continue;
            for (auto it = values.cbegin(); it != values.cend(); ++it) item[it.key()] = it.value();
            value = item;
        }
        publish();
    }

    void remove(const QString& id)
    {
        selected.removeAll(id);
        for (qsizetype i = media.size(); i > 0; --i) {
            if (media.at(i - 1).toMap().value("mediaId") == id) media.removeAt(i - 1);
        }
        publish();
    }

    QQuickItem* visual(const QString& id) const
    {
        QList<QQuickItem*> pending {root};
        while (!pending.isEmpty()) {
            auto* item = pending.takeLast();
            if (item->property("mediaId").toString() == id
                && item->metaObject()->indexOfProperty("editing") >= 0) return item;
            pending.append(item->childItems());
        }
        return nullptr;
    }

    QQuickItem* mediaDelegate(const QString& id) const
    {
        QList<QQuickItem*> pending {root};
        while (!pending.isEmpty()) {
            auto* item = pending.takeLast();
            if (item->property("currentMediaId").toString() == id
                && item->metaObject()->indexOfProperty("moveHandlerActive") >= 0)
                return item;
            pending.append(item->childItems());
        }
        return nullptr;
    }

    void click(QPoint point, Qt::KeyboardModifiers modifiers = Qt::NoModifier)
    {
        QTest::mouseClick(&window, Qt::LeftButton, modifiers, point);
        QCoreApplication::processEvents();
    }

    void drag(QPoint start, QPoint end)
    {
        QTest::mouseMove(&window, start);
        QCoreApplication::processEvents();
        QTest::mousePress(&window, Qt::LeftButton, Qt::NoModifier, start);
        QCoreApplication::processEvents();
        for (int step = 1; step <= 4; ++step) {
            QTest::mouseMove(&window, start + (end - start) * step / 4);
            QCoreApplication::processEvents();
        }
        QTest::mouseRelease(&window, Qt::LeftButton, Qt::NoModifier, end);
        QCoreApplication::processEvents();
    }

    void doubleClick(QPoint point)
    {
        QTest::qWait(QGuiApplication::styleHints()->mouseDoubleClickInterval() + 20);
        QTest::mouseDClick(&window, Qt::LeftButton, Qt::NoModifier, point);
        QCoreApplication::processEvents();
    }

public slots:
    void select(const QString& id, bool additive)
    {
        ++selectionRequestCount;
        if (!additive) selected.clear();
        if (!selected.contains(id)) selected.append(id);
        publishSelection();
    }
    void clear() { ++clearRequestCount; selected.clear(); publishSelection(); }
    void liveText(const QString& id, const QString& text)
    {
        bool found = false;
        for (auto& value : media) {
            auto entry = value.toMap();
            if (entry.value("mediaId") == id) { entry["textContent"] = text; value = entry; found = true; }
        }
        if (found) publish();
    }
    void commit(const QString& id, const QString& text) { ++commitCount; liveText(id, text); }
    void move(const QString& id, double x, double y, bool)
    {
        bool found = false;
        for (auto& value : media) {
            auto entry = value.toMap();
            if (entry.value("mediaId") == id) { entry["x"] = x; entry["y"] = y; value = entry; found = true; }
        }
        if (found) publish();
    }
    void captureResize(const QString& id, const QString& handle,
                       double x, double y, bool, bool altPressed)
    {
        if (!emulateResizePublication) return;
        if (handle != QStringLiteral("bottom-right") || altPressed) {
            error = QStringLiteral("Unexpected resize variant in test backend");
            return;
        }

        const QVariantMap item = entry(id);
        if (item.isEmpty()) {
            error = QStringLiteral("Resize requested for absent media %1").arg(id);
            return;
        }
        if (pendingResizeId.isEmpty()) {
            pendingResizeId = id;
            resizeBaseWidth = item.value("width").toDouble();
            const qreal scale = item.value("scale").toDouble();
            originalResizeRect = QRectF(item.value("x").toDouble(),
                                        item.value("y").toDouble(),
                                        resizeBaseWidth * scale,
                                        item.value("height").toDouble() * scale);
        }
        if (pendingResizeId != id) {
            error = QStringLiteral("Concurrent resize in test backend");
            return;
        }

        const qreal widthFactor = (x - originalResizeRect.left())
                                / qMax<qreal>(1.0, originalResizeRect.width());
        const qreal heightFactor = (y - originalResizeRect.top())
                                 / qMax<qreal>(1.0, originalResizeRect.height());
        const qreal factor = qMax<qreal>(1.0 / qMax(originalResizeRect.width(),
                                                    originalResizeRect.height()),
                                         qMax(widthFactor, heightFactor));
        pendingResizeRect = QRectF(originalResizeRect.topLeft(),
                                   originalResizeRect.size() * factor);
        root->setProperty("liveResizeActive", true);
        root->setProperty("liveResizeMediaId", id);
        root->setProperty("liveResizeX", pendingResizeRect.x());
        root->setProperty("liveResizeY", pendingResizeRect.y());
        root->setProperty("liveResizeScale", pendingResizeRect.width() / resizeBaseWidth);
    }
    void publishResize(const QString& id)
    {
        if (!emulateResizePublication) return;
        if (id != pendingResizeId || pendingResizeRect.isEmpty()) {
            error = QStringLiteral("Resize ended without pending geometry for %1").arg(id);
            return;
        }

        const QRectF committed = pendingResizeRect;
        const qreal baseWidth = resizeBaseWidth;
        pendingResizeId.clear();
        originalResizeRect = {};
        pendingResizeRect = {};
        resizeBaseWidth = 0.0;
        root->setProperty("liveResizeActive", false);
        root->setProperty("liveResizeMediaId", QString());
        root->setProperty("liveResizeScale", 1.0);
        ++resizePublicationCount;
        change(id, {{"x", committed.x()}, {"y", committed.y()},
                    {"scale", committed.width() / baseWidth}});
    }
    void createText(double viewX, double viewY)
    {
        const auto id = QString("created-%1").arg(++textCreateCount);
        const qreal scale = root->property("viewScale").toDouble();
        add(id, "text", (viewX - root->property("panX").toDouble()) / scale,
            (viewY - root->property("panY").toDouble()) / scale);
        select(id, false);
        root->setProperty("textToolActive", false);
    }
};

class CanvasInteractionTest : public QObject {
    Q_OBJECT
private slots:
    void initTestCase()
    {
        registerCanvasQmlTypes();
        QVERIFY(QFontDatabase::addApplicationFont(TEST_SOURCE_DIR "/resources/fonts/impact.ttf") >= 0);
    }

    void init()
    {
        QTest::failOnWarning(QRegularExpression(
            "TypeError:|ReferenceError:|Binding loop detected|Object set as mask|QQuickItem::stackAfter"
            "|\\[QuickCanvas\\]\\[InputCoordinator\\]"));
    }

    void reselectMedia_data()
    {
        QTest::addColumn<QString>("type");
        for (const char* type : {"text", "image", "video"}) QTest::newRow(type) << QString::fromLatin1(type);
    }
    void reselectMedia()
    {
        QFETCH(QString, type);
        CanvasFixture scene;
        QVERIFY2(scene.initialize(), qPrintable(scene.error));
        scene.add("a", type, 100, 150);
        for (int i = 0; i < 3; ++i) {
            scene.click({240, 230});
            QCOMPARE(scene.selected, QStringList {"a"});
            scene.click(scene.backgroundPoint());
            QVERIFY(scene.selected.isEmpty());
        }
    }

    void primaryPressSelectsTextBeforeRelease_data()
    {
        QTest::addColumn<qreal>("outlinePercent");
        QTest::newRow("borderless") << qreal(0);
        QTest::newRow("thick-border") << qreal(100);
    }

    void primaryPressSelectsTextBeforeRelease()
    {
        QFETCH(qreal, outlinePercent);
        CanvasFixture scene;
        QVERIFY2(scene.initialize(), qPrintable(scene.error));
        scene.add("text", "text", 100, 150);
        scene.change("text", {{"textOutlineWidthPercent", outlinePercent},
                               {"textOutlineWidthPx", qreal(40)}});

        QSignalSpy selected(scene.root, SIGNAL(mediaSelectRequested(QString,bool)));
        QTest::mousePress(&scene.window, Qt::LeftButton, Qt::NoModifier, {240, 230});
        QCoreApplication::processEvents();

        // Selection is a press decision. It must not depend on whether the
        // delegate's child PointerHandler happens to activate before or after
        // the global gesture router on a particular renderer/frame.
        QCOMPARE(scene.selected, QStringList {"text"});
        QCOMPARE(selected.size(), 1);

        QTest::mouseRelease(&scene.window, Qt::LeftButton, Qt::NoModifier, {240, 230});
        QCoreApplication::processEvents();
        QCOMPARE(selected.size(), 1);
        QCOMPARE(scene.root->property("interactionMode").toString(), QString("idle"));
    }

    void externalDeselectionEndsEditing()
    {
        CanvasFixture scene;
        QVERIFY2(scene.initialize(), qPrintable(scene.error));
        scene.add("a", "text", 100, 150);
        scene.doubleClick({240, 230});
        auto* text = scene.visual("a");
        QVERIFY(text);
        QVERIFY(text->property("editing").toBool());
        auto* editor = text->findChild<QQuickTextEdit*>();
        QVERIFY(editor);
        QCOMPARE(editor->text(), QString("Canvas text"));
        scene.clear(); // Inspector/tool/backend selection change, not canvas click.
        QCoreApplication::processEvents();
        QVERIFY2(!text->property("editing").toBool(), "Deselected text retained a hidden editor and blocked reselection");
        QVERIFY(!scene.root->property("anyMediaEditing").toBool());
        scene.click({240, 230});
        QCOMPARE(scene.selected, QStringList {"a"});
        scene.doubleClick({240, 230});
        QVERIFY(text->property("editing").toBool());
        QCOMPARE(editor->text(), QString("Canvas text"));
    }

    void editingPreservesContentAndPublishesKeystrokes()
    {
        CanvasFixture scene;
        QVERIFY2(scene.initialize(), qPrintable(scene.error));
        scene.add("a", "text", 100, 150);
        const QString original = QString::fromUtf8("Texte accentué, ligne 1\nDeuxième ligne");
        scene.change("a", {{"textContent", original}});
        scene.doubleClick({240, 230});
        auto* visual = scene.visual("a");
        QVERIFY(visual && visual->property("editing").toBool());
        auto* editor = visual->findChild<QQuickTextEdit*>();
        QVERIFY(editor);
        QCOMPARE(editor->text(), original);
        editor->setCursorPosition(editor->text().size());
        QTest::keyClick(&scene.window, Qt::Key_X);
        QCoreApplication::processEvents();
        QCOMPARE(editor->text(), original + "x");
        QCOMPARE(scene.entry("a").value("textContent").toString(), original + "x");
        scene.clear();
        QVERIFY(!visual->property("editing").toBool());
        QCOMPARE(editor->text(), original + "x");
        QCOMPARE(scene.commitCount, 1);
        scene.doubleClick({240, 230});
        QVERIFY(visual->property("editing").toBool());
        QCOMPARE(editor->text(), original + "x");
    }

    void textToolCreationCanBeReselectedAndEdited()
    {
        CanvasFixture scene;
        QVERIFY2(scene.initialize(), qPrintable(scene.error));
        scene.root->setProperty("textToolActive", true);
        scene.click({100, 150});
        QCOMPARE(scene.textCreateCount, 1);
        QCOMPARE(scene.selected, QStringList {"created-1"});
        QVERIFY(!scene.root->property("textToolActive").toBool());
        QCOMPARE(scene.root->property("interactionMode").toString(), QString("idle"));
        auto* visual = scene.visual("created-1");
        QVERIFY(visual);
        QVERIFY(!visual->property("editing").toBool());
        scene.click(scene.backgroundPoint());
        QVERIFY(scene.selected.isEmpty());
        scene.click({240, 230});
        QCOMPARE(scene.selected, QStringList {"created-1"});
        scene.doubleClick({240, 230});
        QVERIFY(visual->property("editing").toBool());
        auto* editor = visual->findChild<QQuickTextEdit*>();
        QVERIFY(editor);
        QCOMPARE(editor->text(), QString("Canvas text"));
        scene.click(scene.backgroundPoint());
        QVERIFY(scene.selected.isEmpty());
        QVERIFY(!visual->property("editing").toBool());
        scene.click({240, 230});
        QCOMPARE(scene.selected, QStringList {"created-1"});
    }

    void switchingTextHasOnlyOneEditor()
    {
        CanvasFixture scene;
        QVERIFY2(scene.initialize(), qPrintable(scene.error));
        scene.add("a", "text", 100, 150);
        scene.add("b", "text", 550, 150);
        scene.doubleClick({240, 230});
        auto* a = scene.visual("a");
        auto* b = scene.visual("b");
        QVERIFY(a && b);
        QVERIFY(a->property("editing").toBool());
        scene.click({690, 230});
        QVERIFY2(!a->property("editing").toBool(), "Clicking another media did not finish the previous editor");
        QCOMPARE(scene.selected, QStringList {"b"});
        scene.doubleClick({690, 230});
        QVERIFY(b->property("editing").toBool());
        QVERIFY(!a->property("editing").toBool());
        scene.click(scene.backgroundPoint());
        QVERIFY(!scene.root->property("anyMediaEditing").toBool());
        scene.click({240, 230});
        QCOMPARE(scene.selected, QStringList {"a"});
    }

    void dragAndReselectMedia_data() { reselectMedia_data(); }

    void dragAndReselectMedia()
    {
        QFETCH(QString, type);
        CanvasFixture scene;
        QVERIFY2(scene.initialize(), qPrintable(scene.error));
        scene.add("a", type, 100, 150);
        QSignalSpy started(scene.root, SIGNAL(mediaMoveStarted(QString,double,double,bool)));
        QSignalSpy ended(scene.root, SIGNAL(mediaMoveEnded(QString,double,double,bool)));
        scene.drag({240, 230}, {312, 266});
        QCOMPARE(started.size(), 1);
        QCOMPARE(ended.size(), 1);
        QCOMPARE(scene.entry("a").value("x").toDouble(), 172.0);
        QCOMPARE(scene.entry("a").value("y").toDouble(), 186.0);
        QCOMPARE(scene.root->property("interactionMode").toString(), QString("idle"));
        QCOMPARE(scene.root->property("activeMediaDragCount").toInt(), 0);
        QVERIFY(scene.root->property("liveDragMediaId").toString().isEmpty());
        scene.click(scene.backgroundPoint());
        QVERIFY(scene.selected.isEmpty());
        scene.click({312, 266});
        QCOMPARE(scene.selected, QStringList {"a"});
        QCOMPARE(scene.root->property("panX").toDouble(), 0.0);
        QCOMPARE(scene.root->property("panY").toDouble(), 0.0);
    }

    void overlappingMediaUseVisibleTopmostDelegate()
    {
        CanvasFixture scene;
        QVERIFY2(scene.initialize(), qPrintable(scene.error));
        scene.add("image", "image", 100, 150);
        scene.add("text", "text", 100, 150);
        scene.add("video", "video", 100, 150);
        scene.click({240, 230});
        QCOMPARE(scene.selected, QStringList {"video"}); // Equal z: later sibling is above.
        QCOMPARE(scene.selectionRequestCount, 1);
        scene.clear();
        scene.change("video", {{"contentVisible", false}});
        scene.click({240, 230});
        QCOMPARE(scene.selected, QStringList {"text"});
        QCOMPARE(scene.selectionRequestCount, 2);
        scene.clear();
        scene.change("text", {{"contentOpacity", 0.0}});
        scene.click({240, 230});
        QCOMPARE(scene.selected, QStringList {"image"});
        QCOMPARE(scene.selectionRequestCount, 3);
        scene.clear();
        scene.change("video", {{"contentVisible", true}, {"z", 3}});
        scene.change("text", {{"contentOpacity", 1.0}, {"z", 5}});
        scene.click({240, 230});
        QCOMPARE(scene.selected, QStringList {"text"});
        QCOMPARE(scene.selectionRequestCount, 4);
    }

    void staleHandleAfterDeselectDoesNotCaptureBody()
    {
        CanvasFixture scene;
        QVERIFY2(scene.initialize(), qPrintable(scene.error));
        scene.add("a", "text", 100, 150);
        scene.click({240, 230});
        QTest::mouseMove(&scene.window, {380, 320}); // Old bottom-right resize handle.
        QCoreApplication::processEvents();
        scene.clear();
        scene.change("a", {{"width", 440}, {"height", 300}});
        // No intervening hover event: that same point is now in the item body.
        QSignalSpy resized(scene.root, SIGNAL(mediaResizeRequested(QString,QString,double,double,bool,bool)));
        QSignalSpy resizeEnded(scene.root, SIGNAL(mediaResizeEnded(QString)));
        scene.click({380, 320});
        QCOMPARE(scene.selected, QStringList {"a"});
        QCOMPARE(resized.size(), 0);
        QCOMPARE(resizeEnded.size(), 0);
        QCOMPARE(scene.root->property("interactionMode").toString(), QString("idle"));
    }

    void actualResizeHandleOwnsGesture_data()
    {
        QTest::addColumn<QPoint>("pressPoint");
        QTest::addColumn<bool>("sendHover");
        QTest::newRow("center-with-hover") << QPoint(380, 320) << true;
        QTest::newRow("outside-half-without-hover") << QPoint(388, 328) << false;
    }

    void actualResizeHandleOwnsGesture()
    {
        QFETCH(QPoint, pressPoint);
        QFETCH(bool, sendHover);
        CanvasFixture scene;
        QVERIFY2(scene.initialize(), qPrintable(scene.error));
        scene.add("a", "image", 100, 150);
        scene.click({240, 230});
        QSignalSpy resized(scene.root, SIGNAL(mediaResizeRequested(QString,QString,double,double,bool,bool)));
        QSignalSpy resizeEnded(scene.root, SIGNAL(mediaResizeEnded(QString)));
        QSignalSpy moved(scene.root, SIGNAL(mediaMoveStarted(QString,double,double,bool)));
        if (sendHover) {
            QTest::mouseMove(&scene.window, pressPoint);
            QCoreApplication::processEvents();
        }
        QTest::mousePress(&scene.window, Qt::LeftButton, Qt::NoModifier, pressPoint);
        QCoreApplication::processEvents();
        QTest::mouseMove(&scene.window, {400, 340});
        QCoreApplication::processEvents();
        // Exercise the watchdog while the real handler owns the gesture: it
        // must distinguish this from the injected orphaned-resize test.
        QVERIFY(QMetaObject::invokeMethod(scene.root, "reconcileInputCoordinatorState",
                                          Q_ARG(QVariant, QStringLiteral("test-active-resize"))));
        const QString activeMode = scene.root->property("interactionMode").toString();
        const QString activeOwner = scene.root->property("interactionOwnerId").toString();
        QTest::mouseMove(&scene.window, {420, 355});
        QCoreApplication::processEvents();
        QTest::mouseRelease(&scene.window, Qt::LeftButton, Qt::NoModifier, {420, 355});
        QCoreApplication::processEvents();
        QCOMPARE(activeMode, QString("resize"));
        QCOMPARE(activeOwner, QString("a"));
        QVERIFY(!resized.isEmpty());
        QCOMPARE(resized.last().at(0).toString(), QString("a"));
        QCOMPARE(resized.last().at(1).toString(), QString("bottom-right"));
        QCOMPARE(resized.last().at(2).toDouble(), 380.0 + 420 - pressPoint.x());
        QCOMPARE(resized.last().at(3).toDouble(), 320.0 + 355 - pressPoint.y());
        QCOMPARE(resizeEnded.size(), 1);
        QCOMPARE(resizeEnded.first().at(0).toString(), QString("a"));
        QCOMPARE(moved.size(), 0);
        QCOMPARE(scene.clearRequestCount, 0);
        QCOMPARE(scene.root->property("interactionMode").toString(), QString("idle"));
        QVERIFY(scene.root->property("interactionOwnerId").toString().isEmpty());
        scene.clear();
        scene.click({240, 230});
        QCOMPARE(scene.selected, QStringList {"a"});
    }

    void orphanedInactiveResizeIsRecoveredBeforeMediaInput()
    {
        CanvasFixture scene;
        QVERIFY2(scene.initialize(), qPrintable(scene.error));
        scene.add("a", "text", 100, 150);
        scene.click({240, 230});
        auto* coordinator = scene.root->findChild<QObject*>("canvasInputCoordinator");
        auto* chrome = scene.root->findChild<QQuickItem*>("canvasSelectionChrome");
        QVERIFY(coordinator);
        QVERIFY(chrome);
        QSignalSpy ended(scene.root, SIGNAL(mediaResizeEnded(QString)));

        // Reproduce a native ungrab whose QML active=false callback was lost:
        // the logical resize/interacting latches survive, but no DragHandler
        // actually owns a point anymore.
        coordinator->setProperty("primaryGestureActive", true);
        coordinator->setProperty("primaryOwnerKind", "handle");
        coordinator->setProperty("primaryOwnerMediaId", "a");
        coordinator->setProperty("mode", "resize");
        coordinator->setProperty("ownerId", "a");
        chrome->setProperty("activeResizeMediaId", "a");
        chrome->setProperty("activeResizeHandleId", "bottom-right");
        chrome->setProperty("interacting", true);
        QVERIFY(!chrome->property("resizeHandlerActive").toBool());

        QTRY_COMPARE_WITH_TIMEOUT(scene.root->property("interactionMode").toString(),
                                  QString("idle"), 600);
        QTRY_VERIFY_WITH_TIMEOUT(!chrome->property("interacting").toBool(), 600);
        QVERIFY(!coordinator->property("primaryGestureActive").toBool());
        QCOMPARE(ended.size(), 1);

        scene.drag({240, 230}, {300, 270});
        QCOMPARE(scene.entry("a").value("x").toDouble(), 160.0);
        QCOMPARE(scene.entry("a").value("y").toDouble(), 190.0);
        scene.doubleClick({300, 270});
        QTRY_VERIFY(scene.visual("a")->property("editing").toBool());
    }

    void orphanedInactiveMoveIsRecoveredBeforeMediaInput()
    {
        CanvasFixture scene;
        QVERIFY2(scene.initialize(), qPrintable(scene.error));
        scene.add("a", "text", 100, 150);
        auto* coordinator = scene.root->findChild<QObject*>("canvasInputCoordinator");
        QVERIFY(coordinator);
        QSignalSpy ended(scene.root, SIGNAL(mediaMoveEnded(QString,double,double,bool)));

        coordinator->setProperty("primaryGestureActive", true);
        coordinator->setProperty("primaryOwnerKind", "media");
        coordinator->setProperty("primaryOwnerMediaId", "a");
        coordinator->setProperty("mode", "move");
        coordinator->setProperty("ownerId", "a");
        scene.root->setProperty("activeMediaDragCount", 1);
        scene.root->setProperty("liveDragMediaId", "a");

        QTRY_COMPARE_WITH_TIMEOUT(scene.root->property("interactionMode").toString(),
                                  QString("idle"), 600);
        QCOMPARE(scene.root->property("activeMediaDragCount").toInt(), 0);
        QVERIFY(scene.root->property("liveDragMediaId").toString().isEmpty());
        QVERIFY(!coordinator->property("primaryGestureActive").toBool());
        QCOMPARE(ended.size(), 1);

        scene.drag({240, 230}, {300, 270});
        QCOMPARE(scene.entry("a").value("x").toDouble(), 160.0);
        QCOMPARE(scene.entry("a").value("y").toDouble(), 190.0);
        scene.doubleClick({300, 270});
        QTRY_VERIFY(scene.visual("a")->property("editing").toBool());
    }

    void resizedTextCanMoveAndEditAfterGeometryPublication()
    {
        CanvasFixture scene;
        QVERIFY2(scene.initialize(), qPrintable(scene.error));
        scene.emulateResizePublication = true;
        scene.add("a", "text", 100, 150);
        scene.click({240, 230});

        // Resize publication is synchronous from mediaResizeEnded, matching
        // QuickCanvasController's re-entrant model update.
        scene.drag({380, 320}, {440, 365});
        QVERIFY2(scene.error.isEmpty(), qPrintable(scene.error));
        QCOMPARE(scene.resizePublicationCount, 1);
        const double resizedScale = scene.entry("a").value("scale").toDouble();
        QVERIFY(resizedScale > 1.2);
        QCOMPARE(scene.root->property("interactionMode").toString(), QString("idle"));

        scene.drag({240, 230}, {300, 270});
        QCOMPARE(scene.entry("a").value("x").toDouble(), 160.0);
        QCOMPARE(scene.entry("a").value("y").toDouble(), 190.0);
        QCOMPARE(scene.entry("a").value("scale").toDouble(), resizedScale);
        scene.doubleClick({300, 270});
        QTRY_VERIFY(scene.visual("a")->property("editing").toBool());
    }

    void tinySelectedTextKeepsEditableDraggableCenter()
    {
        CanvasFixture scene;
        QVERIFY2(scene.initialize(), qPrintable(scene.error));
        scene.add("tiny", "text", 100, 150);
        scene.change("tiny", {{"width", 32.0}, {"height", 24.0}});
        const QPoint center(116, 162);
        scene.click(center);
        QCOMPARE(scene.selected, QStringList {"tiny"});

        QSignalSpy resized(scene.root, SIGNAL(mediaResizeRequested(QString,QString,double,double,bool,bool)));
        QSignalSpy moved(scene.root, SIGNAL(mediaMoveStarted(QString,double,double,bool)));
        scene.drag(center, {136, 177});
        QCOMPARE(moved.size(), 1);
        QCOMPARE(resized.size(), 0);
        QCOMPARE(scene.entry("tiny").value("x").toDouble(), 120.0);
        QCOMPARE(scene.entry("tiny").value("y").toDouble(), 165.0);

        scene.doubleClick({136, 177});
        QTRY_VERIFY(scene.visual("tiny")->property("editing").toBool());
    }

    void hidingWindowDuringResizeReleasesCanvas()
    {
        CanvasFixture scene;
        QVERIFY2(scene.initialize(), qPrintable(scene.error));
        scene.add("a", "text", 100, 150);
        scene.click({240, 230});
        auto* chrome = scene.root->findChild<QQuickItem*>("canvasSelectionChrome");
        QVERIFY(chrome);

        QTest::mousePress(&scene.window, Qt::LeftButton, Qt::NoModifier, {380, 320});
        QCoreApplication::processEvents();
        QTest::mouseMove(&scene.window, {420, 350});
        QCoreApplication::processEvents();
        QCOMPARE(scene.root->property("interactionMode").toString(), QString("resize"));
        QVERIFY(chrome->property("interacting").toBool());

        // Window deactivation/hide is a common way for the native release to
        // be consumed outside the QQuickWindow.
        scene.window.hide();
        QCoreApplication::processEvents();
        QTRY_COMPARE_WITH_TIMEOUT(scene.root->property("interactionMode").toString(),
                                  QString("idle"), 600);
        QVERIFY(!chrome->property("interacting").toBool());

        scene.window.show();
        QVERIFY(QTest::qWaitForWindowExposed(&scene.window));
        scene.root->setSize(scene.window.size());
        QTest::mouseRelease(&scene.window, Qt::LeftButton, Qt::NoModifier, {420, 350});
        QCoreApplication::processEvents();

        scene.drag({240, 230}, {280, 260});
        QCOMPARE(scene.entry("a").value("x").toDouble(), 140.0);
        QCOMPARE(scene.entry("a").value("y").toDouble(), 180.0);
        scene.doubleClick({280, 260});
        QTRY_VERIFY(scene.visual("a")->property("editing").toBool());
    }

    void windowDeactivateCancelsNativeResizeGrab()
    {
        CanvasFixture scene;
        QVERIFY2(scene.initialize(), qPrintable(scene.error));
        scene.add("a", "text", 100, 150);
        scene.click({240, 230});
        auto* chrome = scene.root->findChild<QQuickItem*>("canvasSelectionChrome");
        QVERIFY(chrome);
        QSignalSpy ended(scene.root, SIGNAL(mediaResizeEnded(QString)));

        QTest::mousePress(&scene.window, Qt::LeftButton, Qt::NoModifier, {380, 320});
        QCoreApplication::processEvents();
        QTest::mouseMove(&scene.window, {420, 350});
        QCoreApplication::processEvents();
        QCOMPARE(scene.root->property("interactionMode").toString(), QString("resize"));
        QVERIFY(chrome->property("resizeHandlerActive").toBool());

        // Exercise Qt's native cancellation path directly, independently of
        // the QML window-visibility fallback.
        QEvent deactivate(QEvent::WindowDeactivate);
        QCoreApplication::sendEvent(&scene.window, &deactivate);
        QCoreApplication::processEvents();
        QVERIFY(!chrome->property("resizeHandlerActive").toBool());
        QVERIFY(!chrome->property("interacting").toBool());
        QCOMPARE(scene.root->property("interactionMode").toString(), QString("idle"));
        QCOMPARE(ended.size(), 1);

        // Reset QTest's synthetic device after Qt has canceled the grab.
        QTest::mouseRelease(&scene.window, Qt::LeftButton, Qt::NoModifier, {420, 350});
        QCoreApplication::processEvents();
        QCOMPARE(ended.size(), 1);

        scene.drag({240, 230}, {280, 260});
        QCOMPARE(scene.entry("a").value("x").toDouble(), 140.0);
        QCOMPARE(scene.entry("a").value("y").toDouble(), 180.0);
        scene.doubleClick({280, 260});
        QTRY_VERIFY(scene.visual("a")->property("editing").toBool());
    }

    void hidingWindowDuringMoveReleasesCanvas()
    {
        CanvasFixture scene;
        QVERIFY2(scene.initialize(), qPrintable(scene.error));
        scene.add("a", "text", 100, 150);
        QSignalSpy started(scene.root, SIGNAL(mediaMoveStarted(QString,double,double,bool)));
        QSignalSpy ended(scene.root, SIGNAL(mediaMoveEnded(QString,double,double,bool)));

        QTest::mousePress(&scene.window, Qt::LeftButton, Qt::NoModifier, {240, 230});
        QCoreApplication::processEvents();
        QTest::mouseMove(&scene.window, {280, 250});
        QCoreApplication::processEvents();
        QCOMPARE(scene.root->property("interactionMode").toString(), QString("move"));
        QCOMPARE(started.size(), 1);

        scene.window.hide();
        QCoreApplication::processEvents();
        QTRY_COMPARE_WITH_TIMEOUT(scene.root->property("interactionMode").toString(),
                                  QString("idle"), 600);
        QCOMPARE(scene.root->property("activeMediaDragCount").toInt(), 0);
        QVERIFY(scene.root->property("liveDragMediaId").toString().isEmpty());
        QCOMPARE(ended.size(), 1);

        // Reset the synthetic device while the window is hidden. Real window
        // systems cancel the pointing sequence at this boundary; QTest keeps
        // its injected button state until an explicit release.
        QTest::mouseRelease(&scene.window, Qt::LeftButton, Qt::NoModifier, {280, 250});
        QCoreApplication::processEvents();
        scene.window.show();
        QVERIFY(QTest::qWaitForWindowExposed(&scene.window));
        scene.root->setSize(scene.window.size());

        scene.drag({240, 230}, {300, 270});
        QCOMPARE(started.size(), 2);
        QCOMPARE(ended.size(), 2);
        QVERIFY(scene.entry("a").value("x").toDouble() > 140.0);
        QVERIFY(scene.entry("a").value("y").toDouble() > 170.0);
        scene.doubleClick({300, 270});
        QTRY_VERIFY(scene.visual("a")->property("editing").toBool());
    }

    void explicitLifecycleAbandonFinishesMoveSynchronously()
    {
        CanvasFixture scene;
        QVERIFY2(scene.initialize(), qPrintable(scene.error));
        scene.add("a", "text", 100, 150);
        auto* delegate = scene.mediaDelegate("a");
        QVERIFY(delegate);
        QSignalSpy ended(scene.root, SIGNAL(mediaMoveEnded(QString,double,double,bool)));

        QTest::mousePress(&scene.window, Qt::LeftButton, Qt::NoModifier, {240, 230});
        QCoreApplication::processEvents();
        QTest::mouseMove(&scene.window, {280, 250});
        QCoreApplication::processEvents();
        QCOMPARE(scene.root->property("interactionMode").toString(), QString("move"));
        QVERIFY(delegate->property("moveHandlerActive").toBool());

        QVERIFY(QMetaObject::invokeMethod(scene.root, "abandonPointerInteractions",
                                          Q_ARG(QVariant, QStringLiteral("test"))));
        // These assertions intentionally precede processEvents(): the logical
        // transaction must finish synchronously rather than wait for Qt's
        // native ungrab notification or the watchdog.
        QCOMPARE(scene.root->property("interactionMode").toString(), QString("idle"));
        QCOMPARE(scene.root->property("activeMediaDragCount").toInt(), 0);
        QVERIFY(scene.root->property("liveDragMediaId").toString().isEmpty());
        QCOMPARE(ended.size(), 1);

        // QTest retains its injected button state until an explicit release.
        QTest::mouseRelease(&scene.window, Qt::LeftButton, Qt::NoModifier, {280, 250});
        QCoreApplication::processEvents();
        QTRY_VERIFY(!delegate->property("moveHandlerActive").toBool());
        QCOMPARE(ended.size(), 1);

        scene.drag({240, 230}, {300, 270});
        QCOMPARE(ended.size(), 2);
        scene.doubleClick({300, 270});
        QTRY_VERIFY(scene.visual("a")->property("editing").toBool());
    }

    void emptyCanvasClearsSelectionExactlyOnce()
    {
        CanvasFixture scene;
        QVERIFY2(scene.initialize(), qPrintable(scene.error));
        scene.add("a", "text", 100, 150);
        scene.doubleClick({240, 230});
        QVERIFY(scene.visual("a")->property("editing").toBool());
        scene.click(scene.backgroundPoint());
        QVERIFY(scene.selected.isEmpty());
        QCOMPARE(scene.clearRequestCount, 1);
        QCOMPARE(scene.commitCount, 1);
        QVERIFY(!scene.root->property("anyMediaEditing").toBool());
    }

    void removingActiveEditorReleasesSession()
    {
        CanvasFixture scene;
        QVERIFY2(scene.initialize(), qPrintable(scene.error));
        scene.add("a", "text", 100, 150);
        scene.add("b", "text", 550, 150);
        scene.doubleClick({240, 230});
        QPointer<QQuickItem> removedEditor(scene.visual("a"));
        QVERIFY(removedEditor && removedEditor->property("editing").toBool());
        scene.remove("a");
        QVERIFY(!removedEditor || !removedEditor->property("editing").toBool());
        QVERIFY(!scene.root->property("anyMediaEditing").toBool());
        scene.click({690, 230});
        QCOMPARE(scene.selected, QStringList {"b"});
        scene.doubleClick({690, 230});
        QVERIFY(scene.visual("b")->property("editing").toBool());
        QCOMPARE(scene.root->property("interactionMode").toString(), QString("idle"));
    }

    void transformedCanvasUsesVisualGeometry()
    {
        CanvasFixture scene;
        QVERIFY2(scene.initialize(), qPrintable(scene.error));
        scene.add("a", "image", 100, 150);
        // The viewport is offset inside its window and transformed by the
        // camera. Input must use the same coordinate mapping as rendering.
        scene.root->setPosition({50, 30});
        scene.root->setSize({900, 700});
        scene.root->setProperty("panX", 75.0);
        scene.root->setProperty("panY", 50.0);
        scene.root->setProperty("viewScale", 0.6);
        QCoreApplication::processEvents();
        scene.drag({269, 218}, {329, 248});
        QCOMPARE(scene.selected, QStringList {"a"});
        QVERIFY(qAbs(scene.entry("a").value("x").toDouble() - 200.0) < 0.01);
        QVERIFY(qAbs(scene.entry("a").value("y").toDouble() - 200.0) < 0.01);
        QCOMPARE(scene.root->property("panX").toDouble(), 75.0);
        QCOMPARE(scene.root->property("panY").toDouble(), 50.0);
        scene.clear();
        scene.click({329, 248});
        QCOMPARE(scene.selected, QStringList {"a"});
    }

    void middleButtonPanSurvivesWatchdogAndModelPublication()
    {
        CanvasFixture scene;
        QVERIFY2(scene.initialize(), qPrintable(scene.error));
        scene.add("a", "image", 100, 150);
        QSignalSpy moved(scene.root, SIGNAL(mediaMoveStarted(QString,double,double,bool)));
        QTest::mousePress(&scene.window, Qt::MiddleButton, Qt::NoModifier, {240, 230});
        QCoreApplication::processEvents();
        QTest::mouseMove(&scene.window, {264, 246});
        QCoreApplication::processEvents();
        QTest::qWait(260); // More than two 120 ms watchdog intervals.
        const QString heldMode = scene.root->property("interactionMode").toString();
        scene.change("a", {{"displayName", "renamed during camera pan"}});
        const QString publishedMode = scene.root->property("interactionMode").toString();
        QTest::mouseMove(&scene.window, {312, 270});
        QCoreApplication::processEvents();
        QTest::mouseRelease(&scene.window, Qt::MiddleButton, Qt::NoModifier, {312, 270});
        QCoreApplication::processEvents();
        QCOMPARE(heldMode, QString("pan"));
        QCOMPARE(publishedMode, QString("pan"));
        QCOMPARE(scene.root->property("panX").toDouble(), 72.0);
        QCOMPARE(scene.root->property("panY").toDouble(), 40.0);
        QCOMPARE(scene.root->property("interactionMode").toString(), QString("idle"));
        QCOMPARE(scene.root->property("activeMediaDragCount").toInt(), 0);
        QCOMPARE(moved.size(), 0);
        scene.click({312, 270});
        QCOMPARE(scene.selected, QStringList {"a"});
    }

    void deletingDraggedMediaReleasesOwnership()
    {
        CanvasFixture scene;
        QVERIFY2(scene.initialize(), qPrintable(scene.error));
        scene.add("a", "text", 100, 150);
        scene.add("b", "image", 550, 150);
        QTest::mousePress(&scene.window, Qt::LeftButton, Qt::NoModifier, {240, 230});
        QCoreApplication::processEvents();
        QTest::mouseMove(&scene.window, {280, 250});
        QCoreApplication::processEvents();
        const QString startedMode = scene.root->property("interactionMode").toString();
        scene.remove("a");
        QTest::mouseRelease(&scene.window, Qt::LeftButton, Qt::NoModifier, {280, 250});
        QCoreApplication::processEvents();
        QCOMPARE(startedMode, QString("move"));
        QCOMPARE(scene.root->property("interactionMode").toString(), QString("idle"));
        QCOMPARE(scene.root->property("activeMediaDragCount").toInt(), 0);
        QVERIFY(scene.root->property("interactionOwnerId").toString().isEmpty());
        QVERIFY(scene.root->property("liveDragMediaId").toString().isEmpty());
        QVERIFY(!scene.root->property("liveSnapDragActive").toBool());
        scene.click({690, 230});
        QCOMPARE(scene.selected, QStringList {"b"});
        scene.drag({690, 230}, {710, 250});
        QCOMPARE(scene.entry("b").value("x").toDouble(), 570.0);
        QCOMPARE(scene.entry("b").value("y").toDouble(), 170.0);
    }

    void deletingResizedMediaReleasesOwnership()
    {
        CanvasFixture scene;
        QVERIFY2(scene.initialize(), qPrintable(scene.error));
        scene.add("a", "image", 100, 150);
        scene.add("b", "image", 550, 150);
        scene.click({240, 230});
        QSignalSpy ended(scene.root, SIGNAL(mediaResizeEnded(QString)));
        QTest::mousePress(&scene.window, Qt::LeftButton, Qt::NoModifier, {380, 320});
        QCoreApplication::processEvents();
        QTest::mouseMove(&scene.window, {400, 340});
        QCoreApplication::processEvents();
        const QString startedMode = scene.root->property("interactionMode").toString();
        scene.remove("a");
        // Complete the actual native gesture: the global resize handler can
        // outlive the delegate which was removed while it held the pointer.
        QTest::mouseRelease(&scene.window, Qt::LeftButton, Qt::NoModifier, {400, 340});
        QCoreApplication::processEvents();
        QCOMPARE(startedMode, QString("resize"));
        QCOMPARE(scene.root->property("interactionMode").toString(), QString("idle"));
        QVERIFY(scene.root->property("interactionOwnerId").toString().isEmpty());
        QCOMPARE(ended.size(), 1);
        QCOMPARE(ended.first().at(0).toString(), QString("a"));
        scene.click({690, 230});
        QCOMPARE(scene.selected, QStringList {"b"});
        scene.drag({690, 230}, {710, 250});
        QCOMPARE(scene.entry("b").value("x").toDouble(), 570.0);
        QCOMPARE(scene.entry("b").value("y").toDouble(), 170.0);
    }

    void shiftSelectionAndEditorTransition()
    {
        CanvasFixture scene;
        QVERIFY2(scene.initialize(), qPrintable(scene.error));
        scene.add("a", "text", 100, 150);
        scene.add("b", "text", 550, 150);
        scene.click({240, 230});
        scene.click({690, 230}, Qt::ShiftModifier);
        QCOMPARE(scene.selected, (QStringList {"a", "b"}));
        QCOMPARE(scene.selectionRequestCount, 2);
        scene.doubleClick({240, 230});
        auto* a = scene.visual("a");
        auto* b = scene.visual("b");
        QVERIFY(a && b && a->property("editing").toBool());
        scene.click({690, 230}, Qt::ShiftModifier);
        QCOMPARE(scene.selected, (QStringList {"a", "b"}));
        QVERIFY(!a->property("editing").toBool());
        QVERIFY(!scene.root->property("anyMediaEditing").toBool());
        QCOMPARE(scene.commitCount, 1);
        scene.doubleClick({690, 230});
        QVERIFY(b->property("editing").toBool());
        QVERIFY(!a->property("editing").toBool());
        QCOMPARE(scene.selected, QStringList {"b"});
        scene.clear();
        QVERIFY(!scene.root->property("anyMediaEditing").toBool());
    }
};

QTEST_MAIN(CanvasInteractionTest)
#include "tst_CanvasInteraction.moc"
