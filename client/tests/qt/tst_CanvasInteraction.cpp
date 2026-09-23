#include "frontend/rendering/canvas/CanvasQmlTypes.h"
#include "frontend/rendering/canvas/MediaListModel.h"
#include "frontend/rendering/canvas/TextOutlineItem.h"
#include "shared/rendering/MediaFrameSource.h"

#include <QEvent>
#include <QCursor>
#include <QFontDatabase>
#include <QGuiApplication>
#include <QMouseEvent>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QQuickWindow>
#include <QPointer>
#include <QPalette>
#include <QRectF>
#include <QRegularExpression>
#include <QScopeGuard>
#include <QSignalSpy>
#include <QStyleHints>
#include <QTest>
#include <QtGui/private/qpointingdevice_p.h>
#include <QtQuick/private/qquicktextedit_p.h>

static QQuickItem* namedVisual(QQuickItem* root, const QString& name)
{
    if (!root) return nullptr;
    if (root->objectName() == name) return root;
    for (auto* child : root->childItems())
        if (auto* found = namedVisual(child, name)) return found;
    return nullptr;
}

static bool sameColor(QColor actual, QColor expected)
{
    return actual.isValid() && qAbs(actual.red() - expected.red()) <= 1
        && qAbs(actual.green() - expected.green()) <= 1
        && qAbs(actual.blue() - expected.blue()) <= 1;
}

static QColor blended(QColor foreground, QColor background, qreal opacity)
{
    return QColor(qRound(foreground.red() * opacity + background.red() * (1 - opacity)),
                  qRound(foreground.green() * opacity + background.green() * (1 - opacity)),
                  qRound(foreground.blue() * opacity + background.blue() * (1 - opacity)));
}

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
    QStringList selectionHistory;
    QString primarySelection;
    int commitCount = 0;
    int selectionRequestCount = 0;
    int textCreateCount = 0;
    int clearRequestCount = 0;
    int nativeDoubleClickCount = 0;
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
        // Exposure can precede native activation. Starting gestures during
        // that transition loses keyboard focus or cancels the new pointer grab.
        window.requestActivate();
        if (!QTest::qWaitForWindowActive(&window)) return false;
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
            if (event->type() == QEvent::MouseButtonDblClick) ++nativeDoubleClickCount;
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
        media.append(QVariantMap {{"rowKey", id}, {"mediaId", id}, {"mediaType", type},
            {"x", x}, {"y", y}, {"width", 280}, {"height", 170}, {"scale", 1.0}, {"z", 1},
            {"contentVisible", true}, {"contentOpacity", 1.0},
            {"displayName", id},
            {"sourceUrl", ""}, {"residencyReady", true},
            {"textContent", "Canvas text"}, {"textFontPixelSize", 40},
            {"textFontFamily", "Impact"}, {"textFontWeight", 400},
            {"textItalic", false}, {"textUnderline", false},
            {"textUppercase", false}, {"textColor", "#ffffffff"},
            {"textOutlineWidthPx", 0.0}, {"textOutlineColor", "#ff000000"},
            {"textHorizontalAlignment", "center"},
            {"textVerticalAlignment", "center"}, {"fitToTextEnabled", false},
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
            if (!selected.contains(entry.value("mediaId").toString())) continue;
            // Match QuickCanvasController::publishSelection: chrome and
            // floating controls consume scene rectangles, not base sizes.
            const qreal scale = entry.value("scale", 1.0).toReal();
            chrome.append(QVariantMap {
                {"mediaId", entry.value("mediaId")},
                {"isPrimary", entry.value("mediaId").toString() == primarySelection},
                {"x", entry.value("x")}, {"y", entry.value("y")},
                {"width", entry.value("width").toReal() * scale},
                {"height", entry.value("height").toReal() * scale}});
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
        selectionHistory.removeAll(id);
        if (primarySelection == id) primarySelection = selectionHistory.isEmpty() ? QString() : selectionHistory.last();
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

    QQuickItem* checker(const QString& id) const
    {
        return namedVisual(mediaDelegate(id), QStringLiteral("mediaTransparencyCheckerboard"));
    }

    QColor pixelAt(QPoint canvasPoint)
    {
        const QImage frame = window.grabWindow();
        if (frame.isNull()) return {};
        const QPointF p = root->mapToScene(canvasPoint);
        return frame.pixelColor(qRound(p.x() * frame.width() / window.width()),
                                qRound(p.y() * frame.height() / window.height()));
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

    void doubleClick(QPoint point, Qt::KeyboardModifiers modifiers = Qt::NoModifier)
    {
        // Advance the synthetic input clock too: qWait alone does not separate
        // taps for platforms whose native double-click interval exceeds 500ms.
        QTest::mouseMove(&window, point,
            QGuiApplication::styleHints()->mouseDoubleClickInterval() + 20);
        QCoreApplication::processEvents();
        QTest::mouseDClick(&window, Qt::LeftButton, modifiers, point);
        QCoreApplication::processEvents();
    }

public slots:
    void select(const QString& id, bool additive)
    {
        ++selectionRequestCount;
        if (!additive && !selected.contains(id)) { selected.clear(); selectionHistory.clear(); }
        if (!selected.contains(id)) selected.append(id);
        selectionHistory.removeAll(id); selectionHistory.append(id); primarySelection = id;
        publishSelection();
    }
    void clear() { ++clearRequestCount; selected.clear(); selectionHistory.clear(); primarySelection.clear(); publishSelection(); }
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
        // This standalone target loads the canvas from source rather than
        // embedding the application's QML module.
        qmlRegisterSingletonType(
            QUrl::fromLocalFile(TEST_SOURCE_DIR "/resources/qml/app/Theme.qml"),
            "Mouffette.App", 1, 0, "Theme");
        QVERIFY(QFontDatabase::addApplicationFont(TEST_SOURCE_DIR "/resources/fonts/impact.ttf") >= 0);
    }

    void init()
    {
        QTest::failOnWarning(QRegularExpression(
            "TypeError:|ReferenceError:|Binding loop detected|Object set as mask|QQuickItem::stackAfter"
            "|\\[QuickCanvas\\]\\[InputCoordinator\\]"));
    }

    void resizeHandleCursors_data()
    {
        reselectMedia_data();
    }

    void resizeHandleCursors()
    {
        QFETCH(QString, type);
        CanvasFixture scene;
        QVERIFY2(scene.initialize(), qPrintable(scene.error));
        scene.add("a", type, 100, 150);
        scene.select("a", false);
        const QList<QPair<QPoint, Qt::CursorShape>> handles {
            {{100, 150}, Qt::SizeFDiagCursor}, {{240, 150}, Qt::SizeVerCursor},
            {{380, 150}, Qt::SizeBDiagCursor}, {{100, 235}, Qt::SizeHorCursor},
            {{380, 235}, Qt::SizeHorCursor}, {{100, 320}, Qt::SizeBDiagCursor},
            {{240, 320}, Qt::SizeVerCursor}, {{380, 320}, Qt::SizeFDiagCursor}
        };
        for (const auto& handle : handles) {
            QTest::mouseMove(&scene.window, handle.first);
            QTRY_COMPARE(scene.window.cursor().shape(), handle.second);
        }
        QTest::mouseMove(&scene.window, {240, 235});
        QTRY_COMPARE(scene.window.cursor().shape(), Qt::ArrowCursor);
        QTest::mouseMove(&scene.window, scene.backgroundPoint());
        QTRY_COMPARE(scene.window.cursor().shape(), Qt::ArrowCursor);

        // Keep the resize cursor even after the pointer leaves its original hitbox.
        QTest::mousePress(&scene.window, Qt::LeftButton, Qt::NoModifier, {380, 320});
        const auto release = qScopeGuard([&] {
            QTest::mouseRelease(&scene.window, Qt::LeftButton, Qt::NoModifier, {430, 360});
        });
        QTest::mouseMove(&scene.window, {430, 360});
        QTRY_COMPARE(scene.root->property("interactionMode").toString(), QString("resize"));
        QTRY_COMPARE(scene.window.cursor().shape(), Qt::SizeFDiagCursor);
    }

    void textEditingAndCreationCursors()
    {
        CanvasFixture scene;
        QVERIFY2(scene.initialize(), qPrintable(scene.error));
        scene.add("a", "text", 100, 150);
        scene.doubleClick({240, 235});
        QTRY_VERIFY(scene.visual("a")->property("editing").toBool());
        QTest::mouseMove(&scene.window, {250, 235});
        QTRY_COMPARE(scene.window.cursor().shape(), Qt::IBeamCursor);
        QTest::mouseMove(&scene.window, scene.backgroundPoint());
        QTRY_COMPARE(scene.window.cursor().shape(), Qt::ArrowCursor);
        scene.click(scene.backgroundPoint());
        QTRY_VERIFY(!scene.visual("a")->property("editing").toBool());
        scene.root->setProperty("textToolActive", true);
        // Tool switches must refresh feedback without another pointer movement.
        QTRY_COMPARE(scene.window.cursor().shape(), Qt::IBeamCursor);
        scene.root->setProperty("textToolActive", false);
        QTRY_COMPARE(scene.window.cursor().shape(), Qt::ArrowCursor);
    }

    void leftButtonBackgroundPan_data()
    {
        QTest::addColumn<bool>("touchpad");
        QTest::addColumn<QString>("state");
        for (bool touchpad : {false, true}) {
            for (const auto& state : {QString("idle"), QString("selected"),
                                      QString("editing")}) {
                QTest::newRow(qPrintable((touchpad ? "touchpad-" : "mouse-") + state))
                    << touchpad << state;
            }
        }
    }

    void leftButtonBackgroundPan()
    {
        QFETCH(bool, touchpad);
        QFETCH(QString, state);
        CanvasFixture scene;
        QVERIFY2(scene.initialize(), qPrintable(scene.error));
        scene.add("a", "text", 100, 150);
        if (state == "selected") scene.select("a", false);
        if (state == "editing") {
            scene.doubleClick({240, 235});
            QTRY_VERIFY(scene.visual("a")->property("editing").toBool());
        }

        // Cocoa can label button-driven mouse events with the trackpad device.
        auto* device = const_cast<QPointingDevice*>(QPointingDevice::primaryPointingDevice());
        auto* deviceState = QPointingDevicePrivate::get(device);
        const auto originalType = deviceState->deviceType;
        const auto restoreDevice = qScopeGuard([&] { deviceState->deviceType = originalType; });
        deviceState->deviceType = touchpad ? QInputDevice::DeviceType::TouchPad
                                          : QInputDevice::DeviceType::Mouse;
        const QPoint start = scene.backgroundPoint();
        const QPoint end = start + QPoint(80, 48);
        QTest::mouseMove(&scene.window, start);
        QTRY_COMPARE(scene.window.cursor().shape(), Qt::ArrowCursor);
        QTest::mousePress(&scene.window, Qt::LeftButton, Qt::NoModifier, start);
        auto release = qScopeGuard([&] {
            QTest::mouseRelease(&scene.window, Qt::LeftButton, Qt::NoModifier, end);
        });
        QTest::mouseMove(&scene.window, start + QPoint(2, 2));
        QTRY_COMPARE(scene.window.cursor().shape(), Qt::ArrowCursor);
        QCOMPARE(scene.root->property("panX").toDouble(), 0.0);
        QTest::mouseMove(&scene.window, start + QPoint(40, 24));
        QTRY_COMPARE(scene.root->property("interactionMode").toString(), QString("pan"));
        QTRY_COMPARE(scene.window.cursor().shape(), Qt::ClosedHandCursor);
        QTest::qWait(260); // Keep the native gesture alive across the watchdog.
        QTest::mouseMove(&scene.window, end);
        QTRY_COMPARE(scene.root->property("panX").toDouble(), 80.0);
        QTRY_COMPARE(scene.root->property("panY").toDouble(), 48.0);
        QTest::mouseRelease(&scene.window, Qt::LeftButton, Qt::NoModifier, end);
        release.dismiss();
        QTRY_COMPARE(scene.root->property("interactionMode").toString(), QString("idle"));
        QTRY_COMPARE(scene.window.cursor().shape(), Qt::ArrowCursor);
        QVERIFY(!scene.root->property("anyMediaEditing").toBool());
        QVERIFY(scene.selected.isEmpty());
        QCOMPARE(scene.entry("a").value("x").toDouble(), 100.0);
        QCOMPARE(scene.entry("a").value("y").toDouble(), 150.0);
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
        scene.change("text", {{"textOutlineWidthPx",
                               outlinePercent > 0 ? qreal(40) : qreal(0)}});

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

    void selectedBorderedTextRetainsMasksDuringCameraZoom()
    {
        CanvasFixture scene;
        QVERIFY2(scene.initialize(), qPrintable(scene.error));
        scene.add("text", "text", 100, 150);
        scene.change("text", {{"textOutlineWidthPx", 40 * 0.30},
                              {"textContent", "CAMERA ZOOM"}});
        scene.click({240, 230});
        QCOMPARE(scene.selected, QStringList {"text"});
        auto* visual = scene.visual("text");
        QVERIFY(visual);
        auto* outline = visual->findChild<TextOutlineItem*>();
        QVERIFY(outline);
        auto* edit = qobject_cast<QQuickTextEdit*>(outline->source());
        QVERIFY(edit);
        QVERIFY(!scene.window.grabWindow().isNull());
        const QSizeF editorSize = edit->size();
        const QVariantMap original = scene.entry("text");
        const qint64 cacheBytes = outline->statistics().cachedMaskBytes;
        QVERIFY(cacheBytes > 0);
        const QPointF anchor(240, 235);
        const QPointF screenAnchor(scene.window.width() / 2, scene.window.height() / 2);
        for (int i = 1; i <= 30; ++i) {
            const qreal scale = 1 + i * 0.30;
            scene.root->setProperty("viewScale", scale);
            scene.root->setProperty("panX", screenAnchor.x() - anchor.x() * scale);
            scene.root->setProperty("panY", screenAnchor.y() - anchor.y() * scale);
            QVERIFY(!scene.window.grabWindow().isNull());
            QCOMPARE(outline->statistics().generatedGlyphs, 0);
            QCOMPARE(outline->statistics().cachedMaskBytes, cacheBytes);
            QCOMPARE(outline->statistics().refinementJobsApplied, 0);
            QVERIFY(!outline->rasterUpdatesDeferred());
            QCOMPARE(scene.selected, QStringList {"text"});
        }
        QTRY_VERIFY_WITH_TIMEOUT(!outline->qualityRefinementPending(), 5000);
        QCOMPARE(outline->statistics().refinementJobsApplied, 1);
        QCOMPARE(edit->size(), editorSize);
        QCOMPARE(scene.entry("text"), original);
        QCOMPARE(scene.commitCount, 0);
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

    void editingMousePlacesCaretAndSelectsText_data()
    {
        QTest::addColumn<qreal>("mediaScale");
        QTest::addColumn<qreal>("viewScale");
        QTest::addColumn<QPointF>("pan");
        QTest::addColumn<bool>("reclassifyAsTouchpad");
        QTest::addColumn<QPointF>("mediaPosition");
        QTest::newRow("identity")
            << qreal(1.0) << qreal(1.0) << QPointF() << false << QPointF(100, 150);
        QTest::newRow("enlarged-zoom-out-panned")
            << qreal(2.0) << qreal(0.75) << QPointF(35.0, -20.0) << false << QPointF(100, 150);
        QTest::newRow("enlarged-zoom-in-panned")
            << qreal(1.6) << qreal(1.4) << QPointF(-25.0, 15.0) << false << QPointF(100, 150);
        QTest::newRow("mouse-reclassified-as-trackpad-after-scroll")
            << qreal(1.0) << qreal(1.0) << QPointF() << true << QPointF(100, 150);
        QTest::newRow("enlarged-zoomed-reclassified-trackpad")
            << qreal(1.6) << qreal(1.4) << QPointF(-25.0, 15.0) << true << QPointF(100, 150);
        QTest::newRow("zoomed-out-media-beyond-content-root-bounds")
            << qreal(3.0) << qreal(0.3) << QPointF(-200.0, -20.0) << false << QPointF(1500, 900);
        QTest::newRow("zoomed-out-negative-world-position")
            << qreal(3.0) << qreal(0.3) << QPointF(700.0, 520.0) << false << QPointF(-1500, -900);
        QTest::newRow("text-crosses-content-root-bottom-edge")
            << qreal(3.0) << qreal(0.3) << QPointF(220.0, 70.0) << false << QPointF(100, 600);
    }

    void editingMousePlacesCaretAndSelectsText()
    {
        QFETCH(qreal, mediaScale);
        QFETCH(qreal, viewScale);
        QFETCH(QPointF, pan);
        QFETCH(bool, reclassifyAsTouchpad);
        QFETCH(QPointF, mediaPosition);
        const auto* device = QPointingDevice::primaryPointingDevice();
        auto* deviceState = QPointingDevicePrivate::get(const_cast<QPointingDevice*>(device));
        const auto originalDeviceType = deviceState->deviceType;
        const auto restoreDevice = qScopeGuard([&] { deviceState->deviceType = originalDeviceType; });
        deviceState->deviceType = QInputDevice::DeviceType::Mouse;
        CanvasFixture scene;
        QVERIFY2(scene.initialize(), qPrintable(scene.error));
        scene.root->setProperty("screensModel", QVariantList {QVariantMap {
            {"x", 0}, {"y", 0}, {"width", 2880}, {"height", 1800},
            {"primary", true}, {"screenId", 1}, {"pixelWidth", 2880}, {"pixelHeight", 1800}}});
        scene.root->setProperty("viewScale", viewScale);
        scene.root->setProperty("panX", pan.x());
        scene.root->setProperty("panY", pan.y());
        QSignalSpy emptyFrames(&scene.window, &QQuickWindow::frameSwapped);
        scene.window.update();
        QTRY_VERIFY_WITH_TIMEOUT(!emptyFrames.isEmpty(), 2000);
        // Real authoring starts with a framed screen and pointer interaction
        // before media are added. Exercise cached ancestor clipping in that
        // order, rather than invalidating every transform after insertion.
        QTest::mouseMove(&scene.window, {250, 250});
        scene.click({250, 250});
        scene.add("text", "text", mediaPosition.x(), mediaPosition.y());
        const QString original = QStringLiteral("ALPHA BRAVO\nCHARLIE DELTA\nECHO FOXTROT");
        scene.change("text", {{"textContent", original},
                              {"width", 320}, {"height", 170}, {"scale", mediaScale},
                              {"textFontPixelSize", 22},
                              {"textHorizontalAlignment", "left"},
                              {"textVerticalAlignment", "top"},
                              {"fitToTextEnabled", false}});
        QCoreApplication::processEvents();
        QSignalSpy frames(&scene.window, &QQuickWindow::frameSwapped);
        scene.window.update();
        QTRY_VERIFY_WITH_TIMEOUT(!frames.isEmpty(), 2000);
        auto* visual = scene.visual("text");
        QVERIFY(visual);
        auto* editor = visual->findChild<QQuickTextEdit*>();
        QVERIFY(editor);
        const auto characterPoint = [editor](int position) {
            const QRectF caret = editor->positionToRectangle(position);
            return editor->mapToScene(QPointF(caret.x(), caret.center().y())).toPoint();
        };
        scene.doubleClick(characterPoint(3));
        QTRY_VERIFY(visual->property("editing").toBool());
        QVERIFY(editor->hasActiveFocus());

        if (reclassifyAsTouchpad) {
            // QCocoa reclassifies this same primary device after its first
            // precise wheel event (trackpads and Magic Mouse alike).
            deviceState->deviceType = QInputDevice::DeviceType::TouchPad;
            const QPoint point = characterPoint(3);
            QWheelEvent wheel(point, scene.window.mapToGlobal(point), {0, 12}, {},
                              Qt::NoButton, Qt::NoModifier, Qt::ScrollUpdate,
                              false, Qt::MouseEventNotSynthesized, device);
            QCoreApplication::sendEvent(&scene.window, &wheel);
            QCoreApplication::processEvents();
        }

        QSignalSpy moved(scene.root, SIGNAL(mediaMoveStarted(QString,double,double,bool)));
        QSignalSpy resized(scene.root, SIGNAL(mediaResizeRequested(QString,QString,double,double,bool,bool)));
        const QVariantMap geometryBefore = scene.entry("text");

        // Creation selects all text. A click at that selection's existing
        // cursor endpoint must still collapse it, even though the cursor's
        // numerical position does not change.
        editor->selectAll();
        QCOMPARE(editor->selectedText(), original);
        const QPoint selectionEnd = characterPoint(editor->length());
        QTest::mouseMove(&scene.window, selectionEnd,
            QGuiApplication::styleHints()->mouseDoubleClickInterval() + 20);
        scene.click(selectionEnd);
        QCOMPARE(editor->cursorPosition(), editor->length());
        QCOMPARE(editor->selectedText(), QString());

        // These clicks occur after editing has begun. They must reach the
        // existing TextEdit rather than the canvas selection/movement layer.
        for (int position : {6, 17, 31}) {
            const QPoint point = characterPoint(position);
            QVERIFY(QRect(QPoint(), scene.window.size()).contains(point));
            QTest::mouseMove(&scene.window, point,
                QGuiApplication::styleHints()->mouseDoubleClickInterval() + 20);
            scene.click(point);
            QCOMPARE(editor->cursorPosition(), position);
            QCOMPARE(editor->selectedText(), QString());
            QVERIFY(visual->property("editing").toBool());
            QVERIFY(editor->hasActiveFocus());
        }

        // Both directions cross a newline. Check the exact selection rather
        // than just its presence, including which endpoint owns the caret.
        const QPair<int, int> selections[] {{3, 20}, {31, 15}};
        for (const auto& selection : selections) {
            const QPoint start = characterPoint(selection.first);
            const QPoint end = characterPoint(selection.second);
            QTest::mouseMove(&scene.window, start,
                QGuiApplication::styleHints()->mouseDoubleClickInterval() + 20);
            scene.drag(start, end);
            const int first = qMin(selection.first, selection.second);
            const int last = qMax(selection.first, selection.second);
            QCOMPARE(editor->selectionStart(), first);
            QCOMPARE(editor->selectionEnd(), last);
            QCOMPARE(editor->selectedText(), original.mid(first, last - first));
            QCOMPARE(editor->cursorPosition(), selection.second);
            QVERIFY(visual->property("editing").toBool());
            QVERIFY(editor->hasActiveFocus());
        }

        QTest::keyClick(&scene.window, Qt::Key_X);
        QString replaced = original;
        replaced.replace(15, 31 - 15, QStringLiteral("x"));
        QTRY_COMPARE(editor->text(), replaced);
        QCOMPARE(scene.entry("text").value("textContent").toString(), replaced);
        QCOMPARE(editor->cursorPosition(), 16);
        QCOMPARE(editor->selectedText(), QString());

        // Native text selection keeps its grab when the pointer leaves the
        // media. It must not turn into a canvas move or end the edit session.
        const QPoint outside = visual->mapToScene(
            QPointF(visual->width() + 8, visual->height() + 8)).toPoint();
        QVERIFY(QRect(QPoint(), scene.window.size()).contains(outside));
        QVERIFY(!visual->boundingRect().contains(visual->mapFromScene(outside)));
        const QPoint start = characterPoint(6);
        QTest::mouseMove(&scene.window, start,
            QGuiApplication::styleHints()->mouseDoubleClickInterval() + 20);
        scene.drag(start, outside);
        QCOMPARE(editor->selectionStart(), 6);
        QCOMPARE(editor->selectionEnd(), replaced.size());
        QCOMPARE(editor->selectedText(), replaced.mid(6));
        QVERIFY(visual->property("editing").toBool());
        QVERIFY(editor->hasActiveFocus());
        QCOMPARE(scene.commitCount, 0);
        QCOMPARE(moved.size(), 0);
        QCOMPARE(resized.size(), 0);
        for (const QString& property : {QStringLiteral("x"), QStringLiteral("y"),
                                      QStringLiteral("width"), QStringLiteral("height"),
                                      QStringLiteral("scale")})
            QCOMPARE(scene.entry("text").value(property), geometryBefore.value(property));
    }

    void editingShiftClickAndDoubleClickSelectText()
    {
        CanvasFixture scene;
        QVERIFY2(scene.initialize(), qPrintable(scene.error));
        scene.add("text", "text", 100, 150);
        auto* visual = scene.visual("text");
        QVERIFY(visual);
        auto* editor = visual->findChild<QQuickTextEdit*>();
        QVERIFY(editor);
        const auto characterPoint = [editor](int position) {
            const QRectF caret = editor->positionToRectangle(position);
            return editor->mapToScene(QPointF(caret.x(), caret.center().y())).toPoint();
        };
        scene.doubleClick(characterPoint(2));
        QTRY_VERIFY(visual->property("editing").toBool());
        scene.click(characterPoint(1));
        scene.click(characterPoint(4), Qt::ShiftModifier);
        QCOMPARE(editor->selectionStart(), 1);
        QCOMPARE(editor->selectionEnd(), 4);
        QCOMPARE(editor->selectedText(), QStringLiteral("anv"));
        QCOMPARE(editor->cursorPosition(), 4);

        scene.doubleClick(characterPoint(9));
        QCOMPARE(editor->selectedText(), QStringLiteral("text"));
        QCOMPARE(editor->selectionStart(), 7);
        QCOMPARE(editor->selectionEnd(), 11);
        QVERIFY(visual->property("editing").toBool());
        QVERIFY(editor->hasActiveFocus());
        QCOMPARE(scene.commitCount, 0);
    }

    void doubleClickPlacesCaretNearestPointer_data()
    {
        QTest::addColumn<qreal>("viewScale");
        QTest::addColumn<qreal>("panX");
        QTest::addColumn<qreal>("panY");
        QTest::addColumn<QString>("text");
        QTest::addColumn<int>("targetPosition");
        QTest::newRow("identity-single-line")
            << qreal(1.0) << qreal(0.0) << qreal(0.0)
            << QStringLiteral("ABCDEFGHIJKLMN") << 5;
        QTest::newRow("zoomed-panned-multiline")
            << qreal(1.6) << qreal(35.0) << qreal(-20.0)
            << QStringLiteral("FIRST LINE\nSECOND LINE\nTHIRD LINE") << 17;
    }

    void doubleClickPlacesCaretNearestPointer()
    {
        QFETCH(qreal, viewScale);
        QFETCH(qreal, panX);
        QFETCH(qreal, panY);
        QFETCH(QString, text);
        QFETCH(int, targetPosition);
        CanvasFixture scene;
        QVERIFY2(scene.initialize(), qPrintable(scene.error));
        scene.add("a", "text", 100, 150);
        scene.change("a", {{"textContent", text},
                           {"textHorizontalAlignment", "left"},
                           {"textVerticalAlignment", "top"},
                           {"fitToTextEnabled", false}});
        scene.root->setProperty("viewScale", viewScale);
        scene.root->setProperty("panX", panX);
        scene.root->setProperty("panY", panY);
        QCoreApplication::processEvents();

        auto* visual = scene.visual("a");
        QVERIFY(visual);
        auto* editor = visual->findChild<QQuickTextEdit*>();
        QVERIFY(editor);
        const QRectF targetCursor = editor->positionToRectangle(targetPosition);
        const QPointF editorPoint(targetCursor.x(), targetCursor.center().y());
        const int expectedPosition = editor->positionAt(editorPoint.x(), editorPoint.y());
        QVERIFY(expectedPosition > 0);
        QVERIFY(expectedPosition < editor->length());
        const QPoint clickPoint = editor->mapToScene(editorPoint).toPoint();
        QVERIFY(QRect(QPoint(), scene.window.size()).contains(clickPoint));

        scene.doubleClick(clickPoint);
        QTRY_VERIFY(visual->property("editing").toBool());
        QCOMPARE(editor->cursorPosition(), expectedPosition);
        QVERIFY(editor->cursorPosition() != editor->length());
    }

    void doubleClickEditsEntireScaledTextBody_data()
    {
        QTest::addColumn<qreal>("mediaScale");
        QTest::addColumn<qreal>("viewScale");
        QTest::addColumn<bool>("fitToText");
        QTest::addColumn<bool>("alreadySelected");
        QTest::addColumn<bool>("partiallyClipped");
        for (bool selected : {false, true}) {
            const QByteArray suffix = selected ? "-selected" : "-unselected";
            QTest::newRow(("unscaled-zoom-out-fixed-box" + suffix).constData())
                << qreal(1.0) << qreal(0.75) << false << selected << false;
            QTest::newRow(("unscaled-zoom-in-fit-text" + suffix).constData())
                << qreal(1.0) << qreal(1.6) << true << selected << false;
            QTest::newRow(("enlarged-zoom-out-fit-text" + suffix).constData())
                << qreal(4.0) << qreal(0.75) << true << selected << false;
            QTest::newRow(("enlarged-zoom-in-fixed-box" + suffix).constData())
                << qreal(4.0) << qreal(1.6) << false << selected << false;
            QTest::newRow(("enlarged-beyond-viewport" + suffix).constData())
                << qreal(4.0) << qreal(1.6) << true << selected << true;
        }
    }

    void doubleClickEditsEntireScaledTextBody()
    {
        QFETCH(qreal, mediaScale);
        QFETCH(qreal, viewScale);
        QFETCH(bool, fitToText);
        QFETCH(bool, alreadySelected);
        QFETCH(bool, partiallyClipped);
        CanvasFixture scene;
        QVERIFY2(scene.initialize(), qPrintable(scene.error));

        // Keep the displayed body inside even a DPI-clamped window while
        // varying the media transform independently of the camera transform.
        // Testing only viewScale misses errors in delegate-local hit testing.
        const QRectF displayedBody = partiallyClipped
            ? QRectF(-scene.window.width() * 0.60, -scene.window.height() * 0.50,
                     scene.window.width() * 1.50, scene.window.height() * 1.40)
            : QRectF(scene.window.width() * 0.15, scene.window.height() * 0.20,
                     scene.window.width() * 0.60, scene.window.height() * 0.55);
        const qreal panX = 23.0;
        const qreal panY = -17.0;
        const qreal effectiveScale = mediaScale * viewScale;
        scene.root->setProperty("viewScale", viewScale);
        scene.root->setProperty("panX", panX);
        scene.root->setProperty("panY", panY);
        scene.add("text", "text", (displayedBody.x() - panX) / viewScale,
                  (displayedBody.y() - panY) / viewScale);
        const QString content = QStringLiteral("FIRST LINE\nSECOND LINE\nTHIRD LINE");
        scene.change("text", {{"width", displayedBody.width() / effectiveScale},
                              {"height", displayedBody.height() / effectiveScale},
                              {"scale", mediaScale},
                              {"textFontPixelSize", qRound(displayedBody.height()
                                                          / (5.0 * effectiveScale))},
                              {"textContent", content},
                              {"textHorizontalAlignment", "left"},
                              {"textVerticalAlignment", "top"},
                              {"fitToTextEnabled", fitToText}});
        auto* visual = scene.visual("text");
        QVERIFY(visual);
        auto* editor = visual->findChild<QQuickTextEdit*>();
        QVERIFY(editor);

        // The reported bug leaves only a strip near the upper edge editable.
        // Include empty body space as well as glyphs: the whole media is a
        // double-click target, with selection handles kept out of these points.
        const QPointF positions[] {{0.25, 0.10}, {0.50, 0.50}, {0.75, 0.90}};
        for (const QPointF& fraction : positions) {
            scene.click(QPoint(scene.window.width() * 0.95, scene.window.height() * 0.95));
            QVERIFY(!visual->property("editing").toBool());
            if (alreadySelected) scene.select("text", false);

            const QRectF visibleBody = visual->mapRectToScene(visual->boundingRect())
                .intersected(QRectF(QPointF(), scene.window.size()));
            const QPoint point = (visibleBody.topLeft()
                + QPointF(visibleBody.width() * fraction.x(),
                          visibleBody.height() * fraction.y())).toPoint();
            const QPointF localPoint = editor->mapFromScene(point);
            const int expectedCursor = editor->positionAt(localPoint.x(), localPoint.y());
            const int doubleClicksBefore = scene.nativeDoubleClickCount;
            scene.doubleClick(point);
            QCOMPARE(scene.nativeDoubleClickCount, doubleClicksBefore + 1);
            QTRY_VERIFY2_WITH_TIMEOUT(visual->property("editing").toBool(),
                qPrintable(QString("Text body at (%1, %2), media scale %3, view scale %4 did not enter editing")
                    .arg(fraction.x()).arg(fraction.y()).arg(mediaScale).arg(viewScale)), 1000);
            QCOMPARE(scene.selected, QStringList {"text"});
            QVERIFY(editor->hasActiveFocus());
            QCOMPARE(editor->cursorPosition(), expectedCursor);
            QCOMPARE(editor->selectionStart(), editor->selectionEnd());
            QCOMPARE(editor->text(), content);
        }
    }

    void nearbyClicksOnDifferentMediaDoNotEnterEditing()
    {
        CanvasFixture scene;
        QVERIFY2(scene.initialize(), qPrintable(scene.error));
        scene.add("a", "text", 100, 150);
        scene.change("a", {{"width", 140.0}});
        scene.add("b", "text", 240, 150);
        scene.change("b", {{"width", 140.0}, {"z", 2}});
        QVERIFY(QGuiApplication::styleHints()->mouseDoubleClickDistance() > 1);

        // Adjacent text bodies are only one logical pixel apart here. Keep
        // clear of the selected item's corner and middle resize handles.
        const int nativeDoubleClicksBefore = scene.nativeDoubleClickCount;
        QTest::mouseMove(&scene.window, {239, 190},
            QGuiApplication::styleHints()->mouseDoubleClickInterval() + 20);
        QTest::mouseClick(&scene.window, Qt::LeftButton, Qt::NoModifier, {239, 190}, 30);
        QCOMPARE(scene.selected, QStringList {"a"});
        QTest::mouseMove(&scene.window, {240, 190});
        QTest::mouseClick(&scene.window, Qt::LeftButton, Qt::NoModifier, {240, 190}, 30);
        QCOMPARE(scene.nativeDoubleClickCount, nativeDoubleClicksBefore + 1);
        QCOMPARE(scene.selected, QStringList {"b"});
        QVERIFY(!scene.root->property("anyMediaEditing").toBool());
        QVERIFY(!scene.visual("a")->property("editing").toBool());
        QVERIFY(!scene.visual("b")->property("editing").toBool());

        scene.doubleClick({310, 230});
        QTRY_VERIFY(scene.visual("b")->property("editing").toBool());
        QVERIFY(!scene.visual("a")->property("editing").toBool());
    }

    void shiftDoubleClickPreservesMultipleSelection()
    {
        CanvasFixture scene;
        QVERIFY2(scene.initialize(), qPrintable(scene.error));
        scene.add("a", "text", 100, 150);
        scene.change("a", {{"width", 160.0}});
        scene.add("b", "text", 300, 150);
        scene.change("b", {{"width", 160.0}});

        for (bool alreadySelected : {false, true}) {
            scene.click(scene.backgroundPoint());
            scene.click({180, 230});
            if (alreadySelected) scene.click({380, 230}, Qt::ShiftModifier);
            scene.doubleClick({380, 230}, Qt::ShiftModifier);
            QCOMPARE(scene.selected, (QStringList {"a", "b"}));
            QTRY_VERIFY(scene.visual("b")->property("editing").toBool());
            QVERIFY(!scene.visual("a")->property("editing").toBool());
        }
    }

    void onlyPrimarySelectionHasResizeHandlesAndActions()
    {
        CanvasFixture scene;
        QVERIFY2(scene.initialize(), qPrintable(scene.error));
        scene.add("a", "text", 100, 150);
        scene.change("a", {{"width", 160.0}});
        scene.add("b", "text", 350, 150);
        scene.change("b", {{"width", 160.0}});
        scene.select("a", false);
        scene.select("b", true);
        QCOMPARE(scene.root->property("primarySelectedMediaId").toString(), QString("b"));
        QTest::mouseMove(&scene.window, {260, 320});
        QTRY_COMPARE(scene.window.cursor().shape(), Qt::ArrowCursor);
        QTest::mouseMove(&scene.window, {510, 320});
        QTRY_COMPARE(scene.window.cursor().shape(), Qt::SizeFDiagCursor);
        scene.click({180, 230});
        QCOMPARE(scene.selected, (QStringList {"a", "b"}));
        QCOMPARE(scene.primarySelection, QString("a"));
        QTest::mouseMove(&scene.window, {510, 320});
        QTRY_COMPARE(scene.window.cursor().shape(), Qt::ArrowCursor);
        QTest::mouseMove(&scene.window, {260, 320});
        QTRY_COMPARE(scene.window.cursor().shape(), Qt::SizeFDiagCursor);
        scene.remove("a");
        QCOMPARE(scene.primarySelection, QString("b"));
        QCOMPARE(scene.root->property("primarySelectedMediaId").toString(), QString("b"));
    }

    void doubleClickOnResizeHandleDoesNotEnterEditing()
    {
        CanvasFixture scene;
        QVERIFY2(scene.initialize(), qPrintable(scene.error));
        scene.add("text", "text", 100, 150);
        scene.click({240, 230});
        QSignalSpy resized(scene.root, SIGNAL(mediaResizeRequested(QString,QString,double,double,bool,bool)));
        QSignalSpy moved(scene.root, SIGNAL(mediaMoveStarted(QString,double,double,bool)));
        scene.doubleClick({380, 320});
        QVERIFY(!scene.root->property("anyMediaEditing").toBool());
        QCOMPARE(resized.size(), 0);
        QCOMPARE(moved.size(), 0);

        scene.drag({380, 320}, {420, 355});
        QVERIFY(!resized.isEmpty());
        QCOMPARE(moved.size(), 0);
        QVERIFY(!scene.root->property("anyMediaEditing").toBool());
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
        QVERIFY(!scene.root->property("anyMediaEditing").toBool());
        scene.click(scene.backgroundPoint());
        QVERIFY(scene.selected.isEmpty());
        scene.click({312, 266});
        QCOMPARE(scene.selected, QStringList {"a"});
        QCOMPARE(scene.root->property("panX").toDouble(), 0.0);
        QCOMPARE(scene.root->property("panY").toDouble(), 0.0);
    }

    void checkerFollowsPrimarySelection()
    {
        CanvasFixture scene;
        QVERIFY2(scene.initialize(), qPrintable(scene.error));
        scene.add("a", "text", 100, 150);
        scene.add("b", "text", 300, 150);
        scene.change("a", {{"contentVisible", false}});
        scene.change("b", {{"contentOpacity", 0.0}});
        QVERIFY(!scene.checker("a") && !scene.checker("b"));
        scene.select("a", false);
        QTRY_VERIFY(scene.checker("a"));
        QVERIFY(scene.checker("a")->isVisible());
        scene.select("b", true);
        QTRY_VERIFY(!scene.checker("a") && scene.checker("b"));
        QCOMPARE(scene.selected.size(), 2);
        scene.select("a", false);
        QTRY_VERIFY(scene.checker("a") && !scene.checker("b"));
        scene.change("a", {{"clipActive", false}});
        QTRY_VERIFY(scene.checker("a") && scene.checker("a")->isVisible());
        scene.change("a", {{"clipActive", true}});
        QTRY_VERIFY(scene.checker("a"));
        scene.clear();
        QTRY_VERIFY(!scene.checker("a") && !scene.checker("b"));
    }

    void invisiblePrimaryRemainsDraggable_data()
    {
        QTest::addColumn<QString>("type");
        QTest::addColumn<bool>("hidden");
        QTest::addColumn<bool>("inactive");
        for (const QString type : {"image", "video", "text"}) {
            QTest::newRow(qPrintable(type + "-hidden")) << type << true << false;
            QTest::newRow(qPrintable(type + "-transparent")) << type << false << false;
            QTest::newRow(qPrintable(type + "-inactive")) << type << false << true;
        }
    }

    void invisiblePrimaryRemainsDraggable()
    {
        QFETCH(QString, type);
        QFETCH(bool, hidden);
        QFETCH(bool, inactive);
        CanvasFixture scene;
        QVERIFY2(scene.initialize(), qPrintable(scene.error));
        scene.add("behind", "image", 100, 150);
        scene.add("primary", type, 100, 150);
        scene.change("primary", {{"contentVisible", !hidden},
                                 {"contentOpacity", hidden || inactive ? 1.0 : 0.0},
                                 {"clipActive", !inactive}});
        scene.select("primary", false);
        QTRY_VERIFY(scene.checker("primary") && scene.checker("primary")->isVisible());
        if (inactive)
            QVERIFY(!namedVisual(scene.mediaDelegate("primary"), "canvasMediaContent")->isVisible());
        if (type == "text") {
            scene.doubleClick({240, 230});
            QVERIFY(!scene.root->property("anyMediaEditing").toBool());
        }
        scene.drag({240, 230}, {270, 245});
        QCOMPARE(scene.entry("primary").value("x").toDouble(), 130.0);
        QCOMPARE(scene.entry("primary").value("y").toDouble(), 165.0);
        QCOMPARE(scene.primarySelection, QString("primary"));
        QCOMPARE(scene.root->property("interactionMode").toString(), QString("idle"));
        // A deselected invisible body must not intercept the underlying media.
        scene.click(scene.backgroundPoint());
        scene.click({240, 230});
        QCOMPARE(scene.primarySelection, QString("behind"));
    }

    void hidingTextFinishesEditing_data()
    {
        QTest::addColumn<bool>("hidden");
        QTest::newRow("eye") << true;
        QTest::newRow("opacity") << false;
    }

    void hidingTextFinishesEditing()
    {
        QFETCH(bool, hidden);
        CanvasFixture scene;
        QVERIFY2(scene.initialize(), qPrintable(scene.error));
        scene.add("text", "text", 100, 150);
        scene.doubleClick({240, 230});
        QVERIFY(scene.root->property("anyMediaEditing").toBool());
        scene.change("text", {{"contentVisible", !hidden}, {"contentOpacity", hidden ? 1.0 : 0.0}});
        QTRY_VERIFY(!scene.root->property("anyMediaEditing").toBool());
        QCOMPARE(scene.commitCount, 1);
        scene.doubleClick({240, 230});
        QVERIFY(!scene.root->property("anyMediaEditing").toBool());
        scene.change("text", {{"contentVisible", true}, {"contentOpacity", 1.0}});
        scene.doubleClick({240, 230});
        QVERIFY(scene.root->property("anyMediaEditing").toBool());
    }

    void checkerStaysInViewportCoordinates()
    {
        CanvasFixture scene;
        QVERIFY2(scene.initialize(), qPrintable(scene.error));
        scene.add("a", "text", 100, 150);
        scene.change("a", {{"contentOpacity", 0.0}});
        scene.select("a", false);
        QTRY_VERIFY(scene.checker("a"));
        const QColor a = scene.checker("a")->property("colorA").value<QColor>();
        const QColor b = scene.checker("a")->property("colorB").value<QColor>();
        const auto verify = [&] {
            auto* checker = scene.checker("a");
            QVERIFY(checker);
            QVERIFY(checker->width() <= scene.root->width());
            QVERIFY(checker->height() <= scene.root->height());
            for (QPoint p : {QPoint(115, 179), QPoint(123, 179), QPoint(115, 187), QPoint(123, 187)}) {
                const QColor cell = ((p.x() / 8 + p.y() / 8) % 2) ? b : a;
                const QColor expected = blended(cell, scene.root->property("color").value<QColor>(), 0.5);
                QTRY_VERIFY(sameColor(scene.pixelAt(p), expected));
            }
        };
        // Include a nonzero viewport origin in the window: the grid is anchored
        // to the canvas viewport, not the window framebuffer or the media.
        scene.root->setPosition({7, 9});
        scene.root->setSize(scene.window.size() - QSize(14, 18));
        for (qreal zoom : {0.5, 1.0, 2.75}) {
            scene.root->setProperty("viewScale", zoom);
            scene.root->setProperty("panX", 80.0 - 100 * zoom);
            scene.root->setProperty("panY", 140.0 - 150 * zoom);
            verify();
            scene.root->setProperty("panX", 83.25 - 100 * zoom);
            scene.root->setProperty("panY", 141.5 - 150 * zoom);
            verify();
        }
        scene.root->setProperty("viewScale", 1.0);
        scene.root->setProperty("panX", 0.0);
        scene.root->setProperty("panY", 0.0);
        scene.root->setProperty("liveResizeMediaId", "a");
        scene.root->setProperty("liveResizeX", 80.0);
        scene.root->setProperty("liveResizeY", 140.0);
        scene.root->setProperty("liveResizeScale", 1.75);
        scene.root->setProperty("liveResizeActive", true);
        verify();
        scene.root->setProperty("liveResizeActive", false);
        scene.root->setProperty("liveAltResizeMediaId", "a");
        scene.root->setProperty("liveAltResizeX", -500000.0);
        scene.root->setProperty("liveAltResizeY", -250000.0);
        scene.root->setProperty("liveAltResizeWidth", 1000000.0);
        scene.root->setProperty("liveAltResizeHeight", 750000.0);
        scene.root->setProperty("liveAltResizeScale", 2.0);
        scene.root->setProperty("liveAltResizeActive", true);
        verify();
        QCOMPARE(scene.checker("a")->size(), scene.root->size());
        scene.root->setProperty("liveAltResizeActive", false);
        scene.change("a", {{"x", -500000.0}, {"y", -250000.0},
                           {"width", 1000000.0}, {"height", 750000.0}, {"scale", 2.0}});
        verify();
        QCOMPARE(scene.checker("a")->size(), scene.root->size());
        scene.change("a", {{"x", 1000000.0}, {"y", 1000000.0}});
        QVERIFY(!scene.checker("a")->isVisible());
    }

    void checkerUsesApplicationTheme()
    {
        const QPalette original = QGuiApplication::palette();
        const auto restore = qScopeGuard([&] { QGuiApplication::setPalette(original); });
        CanvasFixture scene;
        QVERIFY2(scene.initialize(), qPrintable(scene.error));
        scene.add("a", "text", 100, 150);
        scene.change("a", {{"contentOpacity", 0.0}});
        scene.select("a", false);
        QTRY_VERIFY(scene.checker("a"));
        for (bool dark : {false, true}) {
            QPalette palette = original;
            palette.setColor(QPalette::Base, dark ? Qt::black : Qt::white);
            palette.setColor(QPalette::Text, dark ? Qt::white : Qt::black);
            QGuiApplication::setPalette(palette);
            QTRY_VERIFY(sameColor(scene.pixelAt({115, 179}),
                blended(QColor(dark ? "#383838" : "#D8D8D8"),
                        scene.root->property("color").value<QColor>(), 0.5)));
            QTRY_VERIFY(sameColor(scene.pixelAt({123, 179}),
                blended(QColor(dark ? "#484848" : "#ECECEC"),
                        scene.root->property("color").value<QColor>(), 0.5)));
        }
    }

    void checkerCompositesContentAndKeepsPaintOrder_data()
    {
        QTest::addColumn<QString>("type");
        QTest::newRow("image") << QString("image");
        QTest::newRow("video-frame") << QString("video");
    }

    void checkerCompositesContentAndKeepsPaintOrder()
    {
        QFETCH(QString, type);
        RemoteVideoFrameSource foreground, background;
        QImage pixels(16, 16, QImage::Format_ARGB32_Premultiplied);
        pixels.fill(Qt::red);
        background.setFrame(pixels);
        CanvasFixture scene;
        QVERIFY2(scene.initialize(), qPrintable(scene.error));
        scene.add("behind", "image", 100, 150);
        scene.change("behind", {{"z", 0}, {"residentFrameSource", QVariant::fromValue(&background)}});
        scene.add("primary", type, 100, 150);
        scene.change("primary", {{type == "image" ? "residentFrameSource" : "remoteFrameSource",
                                  QVariant::fromValue(&foreground)}});
        scene.select("primary", false);
        QTRY_VERIFY(scene.checker("primary"));
        const QColor cell = blended(scene.checker("primary")->property("colorA").value<QColor>(),
                                    QColor(Qt::red), 0.5);
        const auto composite = [&](qreal alpha) {
            return QColor(qRound(cell.red() * (1 - alpha)),
                          qRound(255 * alpha + cell.green() * (1 - alpha)),
                          qRound(cell.blue() * (1 - alpha)));
        };
        for (int alpha : {255, 128, 0}) {
            pixels.fill(QColor(0, 255, 0, alpha));
            foreground.setFrame(pixels);
            QTRY_VERIFY(sameColor(scene.pixelAt({115, 179}), composite(alpha / 255.0)));
        }
        pixels.fill(Qt::green);
        foreground.setFrame(pixels);
        scene.change("primary", {{"contentOpacity", 0.5}});
        QTRY_VERIFY(sameColor(scene.pixelAt({115, 179}), composite(0.5)));
        scene.change("primary", {{"contentOpacity", 0.0}});
        QTRY_VERIFY(sameColor(scene.pixelAt({115, 179}), cell));
        scene.change("behind", {{"z", 2}});
        QTRY_COMPARE(scene.pixelAt({115, 179}), QColor(Qt::red));
        scene.change("behind", {{"z", 0}});
        QTRY_VERIFY(sameColor(scene.pixelAt({115, 179}), cell));
        scene.clear();
        QTRY_COMPARE(scene.pixelAt({115, 179}), QColor(Qt::red));

        // The shared media renderer must never introduce a checker into a
        // passive output, including a selected/transparent source occurrence.
        scene.root->setVisible(false);
        scene.window.setColor(Qt::red);
        auto span = scene.entry("primary");
        const QVariantMap projection{{"destX", 100}, {"destY", 150}, {"destWidth", 280},
                                     {"destHeight", 170}, {"sourceX", 0}, {"sourceY", 0},
                                     {"sourceWidth", 1}, {"sourceHeight", 1}, {"renderVisible", true},
                                     {"renderOpacity", 0.5}, {"selected", true}, {"spanId", "span"}};
        for (auto it = projection.cbegin(); it != projection.cend(); ++it)
            span[it.key()] = it.value();
        MediaListModel remoteModel;
        remoteModel.updateFromList({span});
        QQmlComponent component(&scene.engine, QUrl::fromLocalFile(TEST_SOURCE_DIR "/resources/qml/RemoteSceneRoot.qml"));
        std::unique_ptr<QObject> remote(component.createWithInitialProperties({
            {"mediaListModel", QVariant::fromValue(&remoteModel)}}));
        auto* remoteItem = qobject_cast<QQuickItem*>(remote.get());
        QVERIFY2(remoteItem, qPrintable(component.errorString()));
        remoteItem->setParentItem(scene.window.contentItem());
        remoteItem->setSize(scene.window.size());
        QVERIFY(!namedVisual(remoteItem, "mediaTransparencyCheckerboard"));
        QTRY_VERIFY(sameColor(scene.pixelAt({115, 179}), QColor(128, 128, 0)));
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

    void selectionChromeRendersAboveCoveredMedia()
    {
        CanvasFixture scene;
        QVERIFY2(scene.initialize(), qPrintable(scene.error));
        scene.add("a", "image", 140, 190);
        scene.change("a", {{"width", 160}, {"height", 100}});
        scene.add("b", "image", 100, 150);
        scene.change("b", {{"z", 1000000}});
        scene.select("a", false);

        // Opaque content on the production foreground delegate makes actual
        // occlusion observable, without an asynchronous image/video decoder.
        QQmlComponent fill(&scene.engine);
        fill.setData("import QtQuick; Rectangle { anchors.fill: parent; color: 'red'; z: 100 }", QUrl());
        std::unique_ptr<QObject> fillObject(fill.create());
        auto* fillItem = qobject_cast<QQuickItem*>(fillObject.get());
        QVERIFY2(fillItem, qPrintable(fill.errorString()));
        fillItem->setParentItem(scene.mediaDelegate("b"));
        QQmlComponent palette(&scene.engine);
        palette.setData("import QtQuick; import Mouffette.App as AppStyle; QtObject { "
                        "property color borderColor: AppStyle.Theme.selectionBorder; "
                        "property color handleColor: AppStyle.Theme.selectionHandle }", QUrl());
        std::unique_ptr<QObject> colors(palette.create());
        QVERIFY2(colors, qPrintable(palette.errorString()));
        const QColor border = colors->property("borderColor").value<QColor>();
        const QColor handle = colors->property("handleColor").value<QColor>();

        const auto pixel = [&scene](QPointF point) {
            const QImage frame = scene.window.grabWindow();
            if (frame.isNull()) return QColor();
            return frame.pixelColor(qRound(point.x() * frame.width() / scene.window.width()),
                                    qRound(point.y() * frame.height() / scene.window.height()));
        };
        for (qreal zoom : {1.0, 1.5, 0.75}) {
            scene.root->setProperty("viewScale", zoom);
            scene.root->setProperty("panX", 200.0 - 220.0 * zoom);
            scene.root->setProperty("panY", 230.0 - 240.0 * zoom);
            auto* a = scene.mediaDelegate("a");
            QVERIFY(a);
            QTRY_COMPARE(pixel(a->mapToScene(QPointF(45, 0)) + QPointF(0, 0.1)), border);
            for (QPointF corner : {QPointF(0, 0), QPointF(160, 0),
                                   QPointF(0, 100), QPointF(160, 100),
                                   QPointF(80, 0), QPointF(80, 100),
                                   QPointF(0, 50), QPointF(160, 50)})
                QCOMPARE(pixel(a->mapToScene(corner)), handle);
            QCOMPARE(pixel(a->mapToScene(QPointF(80, 50))), QColor(Qt::red));
        }
        scene.clear();
        QTRY_COMPARE(pixel(scene.mediaDelegate("a")->mapToScene(QPointF(0, 0))), QColor(Qt::red));
    }

    void selectedCoveredMediaKeepsInputPriority_data() { reselectMedia_data(); }

    void selectedCoveredMediaKeepsInputPriority()
    {
        QFETCH(QString, type);
        CanvasFixture scene;
        QVERIFY2(scene.initialize(), qPrintable(scene.error));
        scene.add("a", type, 140, 190);
        scene.change("a", {{"width", 160}, {"height", 100}, {"z", 10}});
        scene.add("b", "text", 100, 150);
        scene.click({220, 240});
        QCOMPARE(scene.selected, QStringList {"a"});
        // Sending the selected item behind B must change only its paint order.
        scene.change("a", {{"z", -1000000}});
        scene.click({220, 240});
        QCOMPARE(scene.selected, QStringList {"a"});
        QSignalSpy moved(scene.root, SIGNAL(mediaMoveEnded(QString,double,double,bool)));
        scene.drag({220, 240}, {244, 252});
        QCOMPARE(moved.size(), 1);
        QCOMPARE(moved.first().at(0).toString(), QString("a"));
        QCOMPARE(scene.entry("a").value("x").toDouble(), 164.0);
        QCOMPARE(scene.entry("a").value("y").toDouble(), 202.0);
        QCOMPARE(scene.entry("a").value("z").toDouble(), -1000000.0);
        QCOMPARE(scene.entry("b").value("x").toDouble(), 100.0);
        QCOMPARE(scene.entry("b").value("y").toDouble(), 150.0);

        scene.emulateResizePublication = true;
        scene.drag({324, 302}, {340, 312});
        QCOMPARE(scene.resizePublicationCount, 1);
        QVERIFY2(scene.error.isEmpty(), qPrintable(scene.error));
        QCOMPARE(scene.selected, QStringList {"a"});
        QCOMPARE(scene.entry("a").value("scale").toDouble(), 1.1);
        QCOMPARE(moved.size(), 1);
        QCOMPARE(scene.root->property("interactionMode").toString(), QString("idle"));

        // An exposed part of B remains selectable. Clearing selection restores
        // ordinary visual stacking in the shared area as well.
        scene.click({115, 230});
        QCOMPARE(scene.selected, QStringList {"b"});
        scene.clear();
        scene.click({244, 252});
        QCOMPARE(scene.selected, QStringList {"b"});
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
        QCOMPARE(scene.selected, (QStringList {"a", "b"}));
        QCOMPARE(scene.primarySelection, QString("b"));
        scene.clear();
        QVERIFY(!scene.root->property("anyMediaEditing").toBool());
    }
    void remoteScreensRemainBehindCanvasMedia_data()
    {
        QTest::addColumn<QString>("type");
        for (const auto* type : {"image", "video", "text"})
            QTest::newRow(type) << QString::fromLatin1(type);
    }

    void remoteScreensRemainBehindCanvasMedia()
    {
        QFETCH(QString, type);
        CanvasFixture scene;
        QVERIFY2(scene.initialize(), qPrintable(scene.error));
        RemoteVideoFrameSource foreground;
        QImage pixels(280, 170, QImage::Format_RGBA8888);
        pixels.fill(Qt::red);
        foreground.setFrame(pixels);
        scene.add("spanning", type, 100, 150);
        scene.change("spanning", {{type == "image" ? "residentFrameSource" : "remoteFrameSource",
                                  QVariant::fromValue(&foreground)}});
        RemoteVideoFrameSource screenFrame;
        scene.root->setProperty("screensModel", QVariantList{
            QVariantMap{{"x", 140}, {"y", 100}, {"width", 60}, {"height", 220}, {"primary", false},
                        {"frameSource", QVariant::fromValue(&screenFrame)}},
            QVariantMap{{"x", 240}, {"y", 100}, {"width", 60}, {"height", 220}, {"primary", false}}});
        for (qreal scale : {1.0, 1.5}) {
            scene.root->setProperty("viewScale", scale);
            scene.root->setProperty("panX", 20.0);
            scene.root->setProperty("panY", 10.0);
            const auto point = [scale](int x, int y) { return QPoint(qRound(x * scale + 20), qRound(y * scale + 10)); };
            QTest::qWait(50);
            const auto background = scene.pixelAt(point(90, 230));
            const auto outside = scene.pixelAt(point(120, 230));
            const auto gap = scene.pixelAt(point(220, 230));
            const auto inside = scene.pixelAt(point(170, 230));
            const auto emptyScreen = scene.pixelAt(point(170, 130));
            QVERIFY(inside != background);
            // Receiving, replacing and clearing desktop frames only changes
            // the background. Media remain visible across screens and gaps.
            for (const QColor color : {QColor(Qt::blue), QColor(Qt::green)}) {
                pixels.fill(color);
                screenFrame.setFrame(pixels);
                QTRY_COMPARE(scene.pixelAt(point(170, 130)), color);
                QCOMPARE(scene.pixelAt(point(170, 230)), inside);
                QCOMPARE(scene.pixelAt(point(120, 230)), outside);
                QCOMPARE(scene.pixelAt(point(220, 230)), gap);
                screenFrame.clear();
                QTRY_COMPARE(scene.pixelAt(point(170, 130)), emptyScreen);
                QCOMPARE(scene.pixelAt(point(170, 230)), inside);
            }
            // The media's own visibility setting still reveals the desktop.
            pixels.fill(Qt::blue);
            screenFrame.setFrame(pixels);
            scene.change("spanning", {{"contentVisible", false}});
            QTRY_COMPARE(scene.pixelAt(point(170, 230)), QColor(Qt::blue));
            QCOMPARE(scene.pixelAt(point(120, 230)), background);
            scene.change("spanning", {{"contentVisible", true}});
            QTRY_COMPARE(scene.pixelAt(point(170, 230)), inside);
            screenFrame.clear();
        }
    }

};

QTEST_MAIN(CanvasInteractionTest)
#include "tst_CanvasInteraction.moc"
