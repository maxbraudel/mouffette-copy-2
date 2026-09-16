#include <QApplication>
#include <QClipboard>
#include <QJsonArray>
#include <QMimeData>
#include "backend/files/FileManager.h"
#include "backend/runtime/RuntimeProfile.h"
#include "frontend/ui/notifications/ToastNotificationSystem.h"
#include <QFile>
#include <QImage>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QQuickItem>
#include <QQuickStyle>
#include <QQuickView>
#include <QTemporaryDir>
#include <QtTest>
#include <limits>

#include "backend/domain/canvas/CanvasDocument.h"
#include "backend/domain/media/CanvasMedia.h"
#include "frontend/rendering/canvas/MediaListModel.h"
#include "frontend/rendering/canvas/QuickCanvasController.h"
#include "frontend/rendering/canvas/QuickCanvasHost.h"
#include "frontend/qml/ClientWorkspaceViewModel.h"
#ifdef Q_OS_MACOS
#include "backend/platform/macos/MacWindowManager.h"
#endif

namespace {
struct ClipboardAndToasts {
    QTemporaryDir directory;
    RuntimeProfileContext previousProfile = RuntimeProfile::context();
    ToastNotificationSystem* previousSystem = ToastNotificationSystem::instance();
    std::unique_ptr<ToastNotificationSystem> system;
    QMimeData* previousClipboard = new QMimeData;
    ClipboardAndToasts() {
        if (const auto* mime = QGuiApplication::clipboard()->mimeData())
            for (const auto& format : mime->formats())
                previousClipboard->setData(format, mime->data(format));
        auto profile = previousProfile;
        profile.rootPath = directory.path();
        RuntimeProfile::configure(profile);
        system = std::make_unique<ToastNotificationSystem>();
        ToastNotificationSystem::setInstance(system.get());
    }
    ~ClipboardAndToasts() {
        ToastNotificationSystem::setInstance(previousSystem);
        system.reset();
        RuntimeProfile::configure(previousProfile);
        QGuiApplication::clipboard()->setMimeData(previousClipboard);
    }
};

QQuickItem* findQuickItemWithProperty(QQuickItem* root, const char* propertyName,
                                      const QVariant& value)
{
    if (!root) return nullptr;
    if (root->property(propertyName) == value) return root;
    for (QQuickItem* child : root->childItems()) {
        if (QQuickItem* match = findQuickItemWithProperty(
                child, propertyName, value)) {
            return match;
        }
    }
    return nullptr;
}

struct Fixture {
    CanvasDocument document;
    QuickCanvasController controller{&document};
    QQuickView view;

    bool initialize()
    {
        // These media interaction fixtures use an explicit 1:1 camera.
        // Camera initialization and responsive framing are tested separately.
        document.setCamera(1.0, 0.0, 0.0);
        QString error;
        if (!controller.initialize(&error)) return false;
        controller.setProjectEditingEnabled(true);
        view.resize(1000, 700);
        view.setResizeMode(QQuickView::SizeRootObjectToView);
        view.setSource(QUrl(QStringLiteral(
            "qrc:/qt/qml/Mouffette/App/resources/qml/CanvasRoot.qml")));
        if (view.status() != QQuickView::Ready || !view.rootObject()) return false;
        view.rootObject()->setProperty("sessionViewModel", QVariantMap{
            {QStringLiteral("canvasController"),
             QVariant::fromValue<QObject*>(&controller)}});
        return true;
    }

    QVariantMap projected(const QString& id) const
    {
        MediaListModel* model = controller.mediaListModel();
        for (int row = 0; row < model->rowCount(); ++row) {
            const QVariantMap value = model->data(
                model->index(row), MediaListModel::ModelDataRole).toMap();
            if (value.value(QStringLiteral("mediaId")).toString() == id) return value;
        }
        return {};
    }
};

bool invokeSelect(QuickCanvasController& controller, const QString& id,
                  bool additive = false)
{
    return QMetaObject::invokeMethod(&controller, "handleMediaSelectRequested",
        Qt::DirectConnection, Q_ARG(QString, id), Q_ARG(bool, additive));
}
}

class CanvasSelectionBackendTest final : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase()
    {
        // The complete page must use the same controls as production main().
        QQuickStyle::setStyle(QStringLiteral("Basic"));
    }

    void cameraResizePreservesSquareComposition()
    {
        CanvasDocument document;
        QuickCanvasController controller(&document);
        controller.setViewportSize(1200, 800);
        QCOMPARE(controller.viewScale(), 0.8);
        QCOMPARE(controller.panX(), 600.0);
        QCOMPARE(controller.panY(), 400.0);
        document.setCameraView({320, -85}, 1200);
        controller.panBy(73, -29);
        controller.zoomAt(930, 240, 1.6);
        const QPointF center = document.cameraCenter();
        const qreal span = document.cameraSquareSceneSize();
        QSignalSpy cameraChanges(&document, &CanvasDocument::cameraChanged);
        QSignalSpy contentChanges(&document, &CanvasDocument::documentChanged);
        const QList<QSizeF> sizes{{600,400}, {1600,800}, {800,1600}, {800,800},
                                  {1,2}, {1200,800}};
        const QList<QPointF> points{{320,-85}, {120,65}, {-180,-585}, {910,650}};
        for (int cycle = 0; cycle < 100; ++cycle) {
            for (const QSizeF size : sizes) {
                controller.setViewportSize(size.width(), size.height());
                const qreal side = qMin(size.width(), size.height());
                for (const QPointF point : points) {
                    const QPointF displayed = point * controller.viewScale()
                        + QPointF(controller.panX(), controller.panY());
                    const QPointF relative = (displayed
                        - QPointF(size.width()/2, size.height()/2)) / side;
                    QVERIFY(QLineF(relative, (point - center) / span).length() < 1e-10);
                }
                QCOMPARE(document.cameraCenter(), center);
                QCOMPARE(document.cameraSquareSceneSize(), span);
            }
        }
        QCOMPARE(cameraChanges.count(), 0);
        QCOMPARE(contentChanges.count(), 0);
        const qreal previousScale = controller.viewScale();
        const QPointF previousPan(controller.panX(), controller.panY());
        const qreal nan = std::numeric_limits<qreal>::quiet_NaN();
        const qreal infinity = std::numeric_limits<qreal>::infinity();
        for (const QSizeF size : {QSizeF(0,0), QSizeF(-1,800), QSizeF(800,0),
                                  QSizeF(nan,800), QSizeF(800,infinity)})
            controller.setViewportSize(size.width(), size.height());
        QCOMPARE(controller.viewScale(), previousScale);
        QCOMPARE(QPointF(controller.panX(), controller.panY()), previousPan);
        QCOMPARE(cameraChanges.count(), 0);
    }

    void cameraZoomAnchorsAndLimitsAreRelative()
    {
        CanvasDocument document;
        QuickCanvasController controller(&document);
        controller.setViewportSize(1200, 800);
        document.setCameraView({340, -90}, 1000);
        const QPointF cursor(920, 230);
        auto sceneUnderCursor = [&] {
            return (cursor - QPointF(controller.panX(), controller.panY()))
                / controller.viewScale();
        };
        const QPointF anchor = sceneUnderCursor();
        QSignalSpy changes(&document, &CanvasDocument::cameraChanged);
        controller.zoomAt(cursor.x(), cursor.y(), 1.25);
        QCOMPARE(changes.count(), 1);
        QVERIFY(QLineF(sceneUnderCursor(), anchor).length() < 1e-9);
        QCOMPARE(document.cameraSquareSceneSize(), 800.0);
        controller.zoomAt(cursor.x(), cursor.y(), 1e10);
        QCOMPARE(document.cameraSquareSceneSize(), 100.0);
        QVERIFY(QLineF(sceneUnderCursor(), anchor).length() < 1e-9);
        controller.setViewportSize(600, 400);
        QCOMPARE(controller.viewScale(), 4.0); // Same maximum normalized zoom.
        controller.zoomAt(300, 200, 2);
        QCOMPARE(document.cameraSquareSceneSize(), 100.0);
        controller.zoomAt(300, 200, 1e-10);
        QCOMPARE(document.cameraSquareSceneSize(), 5000.0);
        QCOMPARE(controller.viewScale(), 0.08); // Below the old absolute clamp.
        controller.zoomAt(300, 200, .5);
        QCOMPARE(document.cameraSquareSceneSize(), 5000.0);

        // Fitting far beyond either manual bound must not make the next wheel
        // tick jump straight to that bound.
        document.setCameraView({}, 20000);
        controller.zoomAt(300, 200, .5);
        QCOMPARE(document.cameraSquareSceneSize(), 20000.0);
        controller.zoomAt(300, 200, 2);
        QCOMPARE(document.cameraSquareSceneSize(), 10000.0);
        document.setCameraView({}, 25);
        controller.zoomAt(300, 200, 2);
        QCOMPARE(document.cameraSquareSceneSize(), 25.0);
        controller.zoomAt(300, 200, .5);
        QCOMPARE(document.cameraSquareSceneSize(), 50.0);
    }

    void cameraFitWaitsForCanvasAndPreservesChosenViews()
    {
        CanvasDocument document;
        QuickCanvasController controller(&document);
        QQuickWindow unrelatedWindow;
        unrelatedWindow.resize(1600, 1000);
        controller.registerWindow(&unrelatedWindow);
        document.setScreens({ScreenInfo(0, 1920, 1080, 0, 0, true)});
        controller.ensureInitialFit(53);
        QVERIFY(!document.hasCamera());
        controller.setViewportSize(800, 600);
        const qreal expectedScale = qMin(694.0 / 1920, 494.0 / 1080);
        QCOMPARE(controller.viewScale(), expectedScale);
        QCOMPARE(document.cameraCenter(), QPointF(960, 540));
        QCOMPARE(controller.panX(), 400.0 - 960 * expectedScale);
        QCOMPARE(controller.panY(), 300.0 - 540 * expectedScale);
        controller.panBy(140, -45);
        const QPointF center = document.cameraCenter();
        const qreal span = document.cameraSquareSceneSize();
        document.setScreens({ScreenInfo(1, 3840, 2160, -3840, 0, true)});
        controller.ensureInitialFit();
        controller.registerWindow(&unrelatedWindow);
        QCOMPARE(document.cameraCenter(), center);
        QCOMPARE(document.cameraSquareSceneSize(), span);
        controller.recenterView(25);
        QCOMPARE(document.cameraCenter(), QPointF(1920, 1080));
        QVERIFY(qAbs(controller.viewScale() - qMin(750.0/3840, 550.0/2160)) < 1e-12);

        // User navigation before screens arrive also owns the view.
        CanvasDocument waiting;
        QuickCanvasController waitingController(&waiting);
        waitingController.setViewportSize(800, 600);
        waitingController.panBy(15, 30);
        const QPointF chosenCenter = waiting.cameraCenter();
        waiting.setScreens({ScreenInfo(0, 1920, 1080, 0, 0, true)});
        QCOMPARE(waiting.cameraCenter(), chosenCenter);
        QCOMPARE(waiting.cameraSquareSceneSize(), 1000.0);
        waitingController.resetView();
        QCOMPARE(waiting.cameraCenter(), QPointF());
        QCOMPARE(waitingController.panX(), 400.0);
        QCOMPARE(waitingController.panY(), 300.0);
    }

    void cameraProjectRoundTripAndLegacyMigration()
    {
        CanvasDocument source;
        QuickCanvasController sourceController(&source);
        sourceController.setViewportSize(1200, 800);
        source.setCameraView({320, -85}, 1600);
        QJsonObject saved = source.serializeProjectState();
        saved.insert("screens", QJsonArray{ScreenInfo(0, 1920, 1080, 0, 0, true).toJson()});
        const QJsonObject viewport = saved.value("viewport").toObject();
        QCOMPARE(viewport.value("cameraVersion").toInt(), 2);
        QCOMPARE(viewport.value("m11").toDouble(), .5);
        QVERIFY(!source.serializeSceneState().contains("viewport"));

        // Exercise restoration both before and after attaching a viewport.
        for (bool alreadyMounted : {false, true}) {
            CanvasDocument restored;
            QuickCanvasController controller(&restored);
            if (alreadyMounted) controller.setViewportSize(600, 900);
            QVERIFY(restored.restoreProjectState(saved, {}));
            controller.setViewportSize(600, 900);
            controller.ensureInitialFit();
            QCOMPARE(restored.cameraCenter(), source.cameraCenter());
            QCOMPARE(restored.cameraSquareSceneSize(), 1600.0);
            QCOMPARE(controller.viewScale(), .375);
            QCOMPARE(controller.panX(), 300.0 - 320 * .375);
            QCOMPARE(controller.panY(), 450.0 + 85 * .375);
        }
        QJsonObject legacyViewport{{"m11", 1.7}, {"dx", 32}, {"dy", -14}};
        saved.insert("viewport", legacyViewport);
        CanvasDocument legacy;
        QuickCanvasController legacyController(&legacy);
        QVERIFY(legacy.restoreProjectState(saved, {}));
        QVERIFY(legacy.hasCamera());
        QVERIFY(!legacy.hasNormalizedCamera());
        legacyController.setViewportSize(800, 600);
        QVERIFY(legacy.hasNormalizedCamera());
        QCOMPARE(legacyController.viewScale(), 1.7);
        QVERIFY(qAbs(legacyController.panX() - 32) < 1e-10);
        QVERIFY(qAbs(legacyController.panY() + 14) < 1e-10);
        QCOMPARE(legacy.cameraCenter(), QPointF((400.0-32)/1.7, (300.0+14)/1.7));
        const QPointF legacyCenter = legacy.cameraCenter();
        legacyController.setViewportSize(400, 300);
        QCOMPARE(legacyController.viewScale(), .85);
        QCOMPARE(legacy.cameraCenter(), legacyCenter);
        QCOMPARE(legacy.serializeProjectState().value("viewport").toObject()
                 .value("cameraVersion").toInt(), 2);

        // Saving an unmounted, untouched project must not suppress its first fit.
        CanvasDocument untouched, reopened;
        QVERIFY(reopened.restoreProjectState(untouched.serializeProjectState(), {}));
        QVERIFY(!reopened.hasCamera());
    }

    void cameraQmlGesturesResizeAndWorkspaceSwitch()
    {
        Fixture fixture;
        QVERIFY(fixture.initialize());
        auto* root = fixture.view.rootObject();
        fixture.document.setCameraView({}, 1000);
        QList<CanvasMedia*> labels;
        for (const QPointF position : {QPointF(-280,-180), QPointF(0,0), QPointF(280,180)}) {
            auto* media = fixture.document.addText(position,
                QStringLiteral("(%1, %2)").arg(position.x()).arg(position.y()));
            media->setTextColorOverrideEnabled(true);
            media->setTextColor(position.isNull() ? QColor("#67e8f9") : QColor("#ffffff"));
            labels.append(media);
        }
        fixture.view.show();
        QVERIFY(QTest::qWaitForWindowExposed(&fixture.view));
#ifdef Q_OS_MACOS
        MacWindowManager::activateApplicationWindow(&fixture.view);
#else
        fixture.view.requestActivate();
#endif
        QVERIFY(QTest::qWaitForWindowActive(&fixture.view));
        QTRY_COMPARE(root->size(), QSizeF(fixture.view.size()));

        const QPointF cursor(root->width() * .72, root->height() * .34);
        const QPointF before = (cursor - QPointF(fixture.controller.panX(), fixture.controller.panY()))
            / fixture.controller.viewScale();
        const qreal oldSpan = fixture.document.cameraSquareSceneSize();
        QWheelEvent zoom(cursor, fixture.view.mapToGlobal(cursor.toPoint()), {}, {0,120},
                         Qt::NoButton, Qt::NoModifier, Qt::NoScrollPhase, false);
        QCoreApplication::sendEvent(&fixture.view, &zoom);
        QVERIFY(fixture.document.cameraSquareSceneSize() < oldSpan);
        const QPointF after = (cursor - QPointF(fixture.controller.panX(), fixture.controller.panY()))
            / fixture.controller.viewScale();
        QVERIFY(QLineF(before, after).length() < 1e-8);
        const QPointF beforeScroll(fixture.controller.panX(), fixture.controller.panY());
        QWheelEvent scroll(cursor, fixture.view.mapToGlobal(cursor.toPoint()), {30,-20}, {},
                           Qt::NoButton, Qt::NoModifier, Qt::ScrollBegin, false);
        QCoreApplication::sendEvent(&fixture.view, &scroll);
        const QPointF scrollDelta = QPointF(fixture.controller.panX(), fixture.controller.panY()) - beforeScroll;
        QVERIFY2(QLineF(scrollDelta, QPointF(30,-20)).length() < 1e-8,
                 qPrintable(QString("Scroll delta: %1, %2").arg(scrollDelta.x()).arg(scrollDelta.y())));
        QWheelEvent endScroll(cursor, fixture.view.mapToGlobal(cursor.toPoint()), {}, {},
                              Qt::NoButton, Qt::NoModifier, Qt::ScrollEnd, false);
        QCoreApplication::sendEvent(&fixture.view, &endScroll);

        // Each geometry changes different native window edges; a pure move is
        // included so window position cannot leak into canvas coordinates.
        const QRect original = fixture.view.geometry();
        const QList<QRect> geometries{
            original, original.adjusted(0,0,-140,0), original.adjusted(140,0,0,0),
            original.adjusted(0,100,0,0), original.adjusted(0,0,0,-100),
            original.adjusted(120,80,0,0), original.adjusted(0,0,-220,-180),
            original.translated(15,15), original};
        const QPointF cameraCenter = fixture.document.cameraCenter();
        const qreal span = fixture.document.cameraSquareSceneSize();
        QSignalSpy cameraChanges(&fixture.document, &CanvasDocument::cameraChanged);
        connect(&fixture.document, &CanvasDocument::cameraChanged, root, [&] {
            qInfo() << "Camera changed during resize:" << fixture.document.cameraCenter()
                    << "viewport" << root->size() << "gesture" << root->property("interactionMode");
        });
        int index = 0;
        for (const QRect geometry : geometries) {
            fixture.view.setGeometry(geometry);
            QTRY_COMPARE(root->size(), QSizeF(fixture.view.size()));
            const qreal side = qMin(root->width(), root->height());
            QTRY_VERIFY(qAbs(root->property("viewScale").toReal() - side / span) < 1e-10);
            QCOMPARE(fixture.document.cameraCenter(), cameraCenter);
            QCOMPARE(fixture.document.cameraSquareSceneSize(), span);
            for (auto* media : labels) {
                auto* delegate = findQuickItemWithProperty(root, "currentMediaId", media->mediaId());
                QVERIFY(delegate);
                const QPointF relative = (delegate->mapToItem(root, QPointF())
                    - QPointF(root->width()/2, root->height()/2)) / side;
                QVERIFY(QLineF(relative, (media->position() - cameraCenter) / span).length() < 1e-8);
            }
            const QString captureDirectory = qEnvironmentVariable("MOUFFETTE_TEST_CAMERA_CAPTURE_DIR");
            if (!captureDirectory.isEmpty()) {
                QTest::qWait(60);
                const QImage capture = fixture.view.grabWindow();
                QVERIFY(!capture.isNull());
                QVERIFY(capture.save(captureDirectory + QString("/canvas-%1.png").arg(index)));
            }
            ++index;
        }
        QCOMPARE(cameraChanges.count(), 0);

        // The same root can be rebound to a different workspace and back.
        CanvasDocument otherDocument;
        QuickCanvasController otherController(&otherDocument);
        otherController.initialize();
        otherDocument.setCameraView({700,-200}, 2200);
        root->setProperty("sessionViewModel", QVariantMap{
            {QStringLiteral("canvasController"), QVariant::fromValue<QObject*>(&otherController)}});
        QCOMPARE(root->property("viewScale").toReal(),
                 qMin(root->width(), root->height()) / 2200);
        root->setProperty("sessionViewModel", QVariantMap{
            {QStringLiteral("canvasController"), QVariant::fromValue<QObject*>(&fixture.controller)}});
        QCOMPARE(fixture.document.cameraCenter(), cameraCenter);
        QCOMPARE(fixture.document.cameraSquareSceneSize(), span);
        QCOMPARE(root->property("viewScale").toReal(), qMin(root->width(), root->height()) / span);
    }

    void clipboardPreservesAuthoringState_data()
    {
        QTest::addColumn<QString>("type");
        for (const char* type : {"text", "image", "video"})
            QTest::newRow(type) << QString::fromLatin1(type);
    }

    void clipboardPreservesAuthoringState()
    {
        QFETCH(QString, type);
        ClipboardAndToasts environment;
        QSignalSpy toasts(environment.system->notificationCenter(), &NotificationCenter::toastRequested);
        CanvasDocument document;
        QuickCanvasController controller(&document);
        controller.setProjectEditingEnabled(true);
        CanvasMedia* original = type == "text"
            ? document.addText({40, 80}, QStringLiteral("Copied text"))
            : document.addPreparedFile(QString::fromUtf8(type == "video" ? TEST_VIDEO_FILE : TEST_WEBP_FILE),
                                       {320, 180}, type == "video", {40, 80});
        QVERIFY(original);
        if (original->isText()) {
            original->setFitToTextEnabled(false);
            original->setFontWeightOverrideEnabled(true);
            original->setFontWeight(700);
            original->setTextColorOverrideEnabled(true);
            original->setTextColor(Qt::cyan);
            original->setOutlineWidthPercent(17);
            original->setOutlineWidthOverrideEnabled(true);
            original->setItalic(true);
            original->setHighlightEnabled(true);
            original->setHorizontalAlignment("left");
        }
        if (original->isVideo()) {
            QTRY_VERIFY(original->player()->duration() > 3000);
            original->setPlaybackRange(500, 2500);
            original->setPositionMs(1500);
            original->setMuted(true);
            original->setVolume(.37);
            original->setRepeatEnabled(true);
        }
        original->setBaseSize({320, 180});
        original->setPosition({123.25, -56.5});
        original->setScale(1.25);
        original->setZ(4.5);
        original->setContentVisible(false);
        auto settings = original->settings();
        settings.fadeInEnabled = true;
        settings.fadeInText = "2.50";
        settings.playDelayEnabled = true;
        settings.playDelayText = "3.25";
        original->setSettings(settings);
        auto expected = document.serializeProjectState().value("media").toArray()[0].toObject();
        expected.remove("mediaId");
        const QString originalId = original->mediaId();
        controller.copySelectedMedia();
        original->setPosition({800, 900});
        controller.pasteMedia();
        QCOMPARE(document.media().size(), 2);
        auto* copy = document.selectedMedia();
        QVERIFY(copy && copy != original);
        QVERIFY(copy->mediaId() != originalId);
        QCOMPARE(copy->sourcePath(), original->sourcePath());
        auto actual = document.serializeProjectState().value("media").toArray()[1].toObject();
        actual.remove("mediaId");
        QCOMPARE(actual, expected);
        if (copy->isVideo()) {
            QVERIFY(copy->player() != original->player());
            QVERIFY(!copy->isPlaying());
            QTRY_COMPARE(copy->player()->position(), qint64(1500));
        }
        QCOMPARE(toasts.size(), 1);
        QCOMPARE(toasts.last()[0].toString(), QStringLiteral("Media pasted."));
        QVERIFY(document.removeMedia(originalId));
        controller.pasteMedia();
        QCOMPARE(document.media().size(), 2);
        QVERIFY(document.selectedMedia() != copy);
        document.setEditsLocked(true);
        controller.pasteMedia();
        controller.deleteSelectedMedia();
        QCOMPARE(document.media().size(), 2);
        document.setEditsLocked(false);
        QGuiApplication::clipboard()->setText(QStringLiteral("ordinary text"));
        controller.pasteMedia();
        QCOMPARE(document.media().size(), 2);
    }

    void clipboardGroupKeepsSharedFilesAssociated()
    {
        ClipboardAndToasts environment;
        FileManager files;
        CanvasDocument document;
        document.setFileManager(&files);
        QuickCanvasController controller(&document);
        controller.setProjectEditingEnabled(true);
        auto* first = document.addPreparedFile(QString::fromUtf8(TEST_WEBP_FILE), {320,180}, false, {});
        auto* second = document.addText({500,300}, "Group");
        const QString fileId = first->fileId();
        QVERIFY(!fileId.isEmpty());
        document.select(first->mediaId(), true);
        controller.copySelectedMedia();
        controller.pasteMedia();
        QCOMPARE(document.media().size(), 4);
        QCOMPARE(document.selectedMediaIds().size(), 2);
        QCOMPARE(files.getMediaIdsForFile(fileId).size(), 2);
        controller.deleteSelectedMedia();
        QCOMPARE(document.media().size(), 2);
        QCOMPARE(files.getMediaIdsForFile(fileId), QList<QString>{first->mediaId()});
        QVERIFY(document.mediaById(second->mediaId()));
        QVERIFY(QFile::exists(files.getFilePathForId(fileId)));
    }

    void keyboardCopyDeleteAndTextEditing()
    {
        ClipboardAndToasts environment;
        Fixture fixture;
        QVERIFY(fixture.initialize());
        auto* text = fixture.document.addText({300,200}, "Words");
        fixture.view.show();
        QVERIFY(QTest::qWaitForWindowExposed(&fixture.view));
#ifdef Q_OS_MACOS
        MacWindowManager::activateApplicationWindow(&fixture.view);
#else
        fixture.view.requestActivate();
#endif
        QVERIFY(QTest::qWaitForWindowActive(&fixture.view));
        auto* root = fixture.view.rootObject();
        root->forceActiveFocus();
        QTRY_VERIFY(root->property("mediaShortcutsEnabled").toBool());
        QTest::keyClick(&fixture.view, Qt::Key_C, Qt::ControlModifier);
        QTest::keyClick(&fixture.view, Qt::Key_V, Qt::ControlModifier);
        QTRY_COMPARE(fixture.document.media().size(), 2);
        QSignalSpy deleted(&fixture.controller, &QuickCanvasController::mediaDeleteRequested);
        QTest::keyClick(&fixture.view, Qt::Key_Backspace);
        QTRY_COMPARE(fixture.document.media().size(), 1);
        QCOMPARE(deleted.size(), 1);
#ifdef Q_OS_MACOS
        fixture.document.select(text->mediaId());
        QTest::keyClick(&fixture.view, Qt::Key_C, Qt::MetaModifier);
        QTest::keyClick(&fixture.view, Qt::Key_V, Qt::MetaModifier);
        QTRY_COMPARE(fixture.document.media().size(), 2);
        fixture.controller.deleteSelectedMedia();
        QCOMPARE(fixture.document.media().size(), 1);
        deleted.clear();
#endif
        fixture.document.select(text->mediaId());
        auto* delegate = findQuickItemWithProperty(root, "currentMediaId", text->mediaId());
        QVERIFY(delegate);
        QVERIFY(QMetaObject::invokeMethod(delegate, "beginTextEditing"));
        QTRY_VERIFY(root->property("anyMediaEditing").toBool());
        QVERIFY(!root->property("mediaShortcutsEnabled").toBool());
        QTest::keyClick(&fixture.view, Qt::Key_C, Qt::ControlModifier);
        QCOMPARE(QGuiApplication::clipboard()->text(), QStringLiteral("Words"));
        QTest::keyClick(&fixture.view, Qt::Key_Backspace);
        QCOMPARE(fixture.document.media().size(), 1);
        QTest::keyClick(&fixture.view, Qt::Key_V, Qt::ControlModifier);
        QTRY_COMPARE(text->text(), QStringLiteral("Words"));
        QTest::mouseClick(&fixture.view, Qt::LeftButton, Qt::NoModifier, {800,500});
        QTRY_VERIFY(!root->property("anyMediaEditing").toBool());
        fixture.document.select(text->mediaId());
        root->forceActiveFocus();
        QTest::keyClick(&fixture.view, Qt::Key_Delete);
        QTRY_VERIFY(fixture.document.media().isEmpty());
#ifdef Q_OS_MACOS
        QCOMPARE(deleted.size(), 1);
#else
        QCOMPARE(deleted.size(), 2);
#endif
    }

    void keyboardVideoTransportAndRangeWarnings()
    {
        ClipboardAndToasts environment;
        QSignalSpy toasts(environment.system->notificationCenter(), &NotificationCenter::toastRequested);
        Fixture fixture;
        QVERIFY(fixture.initialize());
        auto* video = fixture.document.addPreparedFile(QString::fromUtf8(TEST_VIDEO_FILE), {160,90}, true, {400,200});
        QVERIFY(video);
        QTRY_VERIFY(video->player()->duration() > 3000);
        fixture.view.show();
        QVERIFY(QTest::qWaitForWindowExposed(&fixture.view));
#ifdef Q_OS_MACOS
        MacWindowManager::activateApplicationWindow(&fixture.view);
#else
        fixture.view.requestActivate();
#endif
        QVERIFY(QTest::qWaitForWindowActive(&fixture.view));
        fixture.view.rootObject()->forceActiveFocus();
        QTRY_COMPARE(fixture.view.rootObject()->property("selectedVideoId").toString(), video->mediaId());
        QTest::keyClick(&fixture.view, Qt::Key_Space);
        QTRY_VERIFY(video->isPlaying());
        QTest::keyClick(&fixture.view, Qt::Key_Space);
        QTRY_VERIFY(!video->isPlaying());
        QTest::keyClick(&fixture.view, Qt::Key_M);
        QVERIFY(video->muted());
        QTest::keyClick(&fixture.view, Qt::Key_M);
        QVERIFY(!video->muted());
        video->setPositionMs(1000);
        QTest::keyClick(&fixture.view, Qt::Key_S);
        QCOMPARE(video->startMarkerMs(), 1000);
        video->setPositionMs(900);
        QTest::keyClick(&fixture.view, Qt::Key_E);
        QCOMPARE(video->endMarkerMs(), -1);
        QCOMPARE(toasts.last()[0].toString(), QStringLiteral("Place end after start."));
        video->setPositionMs(2000);
        QTest::keyClick(&fixture.view, Qt::Key_E);
        QCOMPARE(video->endMarkerMs(), 2000);
        QTest::keyClick(&fixture.view, Qt::Key_S);
        QCOMPARE(video->startMarkerMs(), -1);
        video->setPositionMs(2100);
        QTest::keyClick(&fixture.view, Qt::Key_S);
        QCOMPARE(video->startMarkerMs(), -1);
        QCOMPARE(toasts.last()[0].toString(), QStringLiteral("Place start before end."));
        QTest::keyClick(&fixture.view, Qt::Key_E);
        QCOMPARE(video->endMarkerMs(), -1);
        fixture.document.setEditsLocked(true);
        QTest::keyClick(&fixture.view, Qt::Key_M);
        QVERIFY(!video->muted());
        QTest::keyClick(&fixture.view, Qt::Key_Delete);
        QCOMPARE(fixture.document.media().size(), 1);
        fixture.document.setEditsLocked(false);
        QQmlComponent inputComponent(fixture.view.engine());
        inputComponent.setData("import QtQuick; TextInput { text: 'Input'; width: 200; height: 30 }", QUrl());
        std::unique_ptr<QObject> inputObject(inputComponent.create());
        auto* input = qobject_cast<QQuickItem*>(inputObject.get());
        QVERIFY(input);
        input->setParentItem(fixture.view.contentItem());
        input->forceActiveFocus();
        QTRY_VERIFY(fixture.view.rootObject()->property("textInputFocused").toBool());
        for (Qt::Key key : {Qt::Key_S, Qt::Key_E, Qt::Key_M, Qt::Key_Space,
                            Qt::Key_Delete, Qt::Key_Backspace})
            QTest::keyClick(&fixture.view, key);
        QTest::keyClick(&fixture.view, Qt::Key_A, Qt::ControlModifier);
        QTest::keyClick(&fixture.view, Qt::Key_C, Qt::ControlModifier);
        QTest::keyClick(&fixture.view, Qt::Key_V, Qt::ControlModifier);
        QCOMPARE(fixture.document.media().size(), 1);
        QVERIFY(!video->isPlaying());
        QVERIFY(!video->muted());
        QCOMPARE(video->startMarkerMs(), -1);
        QCOMPARE(video->endMarkerMs(), -1);
        auto* delegate = findQuickItemWithProperty(fixture.view.rootObject(), "currentMediaId", video->mediaId());
        QVERIFY(delegate);
        QTest::mouseClick(&fixture.view, Qt::LeftButton, Qt::NoModifier,
                         delegate->mapToScene({80,45}).toPoint());
        QTRY_VERIFY(fixture.view.rootObject()->property("mediaShortcutsEnabled").toBool());
        QTest::keyClick(&fixture.view, Qt::Key_M);
        QVERIFY(video->muted());
    }

    void documentSelectionIsTheSingleAuthority()
    {
        Fixture fixture;
        QVERIFY(fixture.initialize());
        CanvasMedia* first = fixture.document.addText({100, 100}, QStringLiteral("First"));
        CanvasMedia* second = fixture.document.addText({100, 400}, QStringLiteral("Second"));
        QVERIFY(first && second);
        QCOMPARE(fixture.controller.mediaListModel()->rowCount(), 2);

        QVERIFY(invokeSelect(fixture.controller, first->mediaId()));
        QVERIFY(first->selected());
        QVERIFY(!second->selected());
        QVERIFY(invokeSelect(fixture.controller, second->mediaId(), true));
        QCOMPARE(fixture.document.selectedMediaIds().size(), 2);

        fixture.document.select(second->mediaId());
        QVERIFY(!first->selected());
        QVERIFY(second->selected());
        QCOMPARE(fixture.controller.selectedMediaItem(), second);
    }

    void textChangesAndLateDeletedIdsAreSafe()
    {
        Fixture fixture;
        QVERIFY(fixture.initialize());
        CanvasMedia* first = fixture.document.addText({10, 20});
        CanvasMedia* survivor = fixture.document.addText({30, 40});
        const QString firstId = first->mediaId();
        const QString survivorId = survivor->mediaId();
        fixture.document.select(survivorId);

        QVERIFY(QMetaObject::invokeMethod(&fixture.controller,
            "handleTextCommitRequested", Qt::DirectConnection,
            Q_ARG(QString, firstId), Q_ARG(QString, QStringLiteral("Updated"))));
        QCOMPARE(first->text(), QStringLiteral("Updated"));
        QVERIFY(survivor->selected());

        QVERIFY(fixture.document.removeMedia(firstId));
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        QVERIFY(invokeSelect(fixture.controller, firstId));
        QVERIFY(QMetaObject::invokeMethod(&fixture.controller,
            "handleTextCommitRequested", Qt::DirectConnection,
            Q_ARG(QString, firstId), Q_ARG(QString, QStringLiteral("stale"))));
        QCOMPARE(fixture.document.selectedMediaIds(), QStringList{survivorId});
    }

    void fitToTextIsDefaultTracksContentAndToggleRefits()
    {
        Fixture fixture;
        QVERIFY(fixture.initialize());
        const QPointF creationPoint(420, 310);
        CanvasMedia* media = fixture.document.addText(
            creationPoint, QStringLiteral("Text"));
        QVERIFY(media);
        QVERIFY(media->fitToTextEnabled());
        QVERIFY(media->baseSize().width() < 400);
        QVERIFY(media->baseSize().height() < 200);
        QVERIFY(qAbs(media->sceneRect().center().x() - creationPoint.x()) < 0.01);
        QVERIFY(qAbs(media->sceneRect().center().y() - creationPoint.y()) < 0.01);

        const QSize initialSize = media->baseSize();
        const QPointF anchoredCenter = media->sceneRect().center();
        media->setText(QStringLiteral("A much longer fitted text value"));
        QVERIFY(media->baseSize().width() > initialSize.width());
        QVERIFY(qAbs(media->sceneRect().center().x() - anchoredCenter.x()) < 0.01);
        QVERIFY(qAbs(media->sceneRect().center().y() - anchoredCenter.y()) < 0.01);

        QVERIFY(QMetaObject::invokeMethod(&fixture.controller,
            "handleOverlayFitToTextToggle", Qt::DirectConnection,
            Q_ARG(QString, media->mediaId())));
        QVERIFY(!media->fitToTextEnabled());
        media->setBaseSize(QSize(310, 170));
        media->setText(QStringLiteral("X"));
        QCOMPARE(media->baseSize(), QSize(310, 170));

        QVERIFY(QMetaObject::invokeMethod(&fixture.controller,
            "handleOverlayFitToTextToggle", Qt::DirectConnection,
            Q_ARG(QString, media->mediaId())));
        QVERIFY(media->fitToTextEnabled());
        QVERIFY(media->baseSize().width() < 310);
        QVERIFY(media->baseSize().height() < 170);
    }

    void uniformAndFreeResizeCommitToDocument()
    {
        Fixture fixture;
        QVERIFY(fixture.initialize());
        CanvasMedia* media = fixture.document.addText({100, 100});
        media->setFitToTextEnabled(false);
        media->setBaseSize({400, 200});
        media->setPosition({100, 100});
        const QString id = media->mediaId();

        QVERIFY(QMetaObject::invokeMethod(&fixture.controller,
            "handleMediaResizeRequested", Qt::DirectConnection,
            Q_ARG(QString, id), Q_ARG(QString, QStringLiteral("bottom-right")),
            Q_ARG(double, 900.0), Q_ARG(double, 500.0),
            Q_ARG(bool, false), Q_ARG(bool, false)));
        QVERIFY(QMetaObject::invokeMethod(&fixture.controller,
            "handleMediaResizeEnded", Qt::DirectConnection, Q_ARG(QString, id)));
        QCOMPARE(media->scale(), 2.0);
        QCOMPARE(media->sceneRect().size(), QSizeF(800, 400));

        QVERIFY(QMetaObject::invokeMethod(&fixture.controller,
            "handleMediaResizeRequested", Qt::DirectConnection,
            Q_ARG(QString, id), Q_ARG(QString, QStringLiteral("bottom-right")),
            Q_ARG(double, 1000.0), Q_ARG(double, 750.0),
            Q_ARG(bool, false), Q_ARG(bool, true)));
        QVERIFY(QMetaObject::invokeMethod(&fixture.controller,
            "handleMediaResizeEnded", Qt::DirectConnection, Q_ARG(QString, id)));
        QCOMPARE(media->scale(), 2.0);
        QCOMPARE(media->baseSize(), QSize(450, 325));
        QCOMPARE(media->sceneRect().size(), QSizeF(900, 650));
    }

    void altResizeDisablesFitAndPreservesExistingTextScale()
    {
        Fixture fixture;
        QVERIFY(fixture.initialize());
        CanvasMedia* media = fixture.document.addText(
            {300, 200}, QStringLiteral("Scaled text"));
        QVERIFY(media);
        QVERIFY(media->fitToTextEnabled());
        media->setScale(2.25);
        const qreal scaleBefore = media->scale();
        const QRectF rectBefore = media->sceneRect();
        const QString id = media->mediaId();

        QVERIFY(QMetaObject::invokeMethod(&fixture.controller,
            "handleMediaResizeRequested", Qt::DirectConnection,
            Q_ARG(QString, id), Q_ARG(QString, QStringLiteral("right-mid")),
            Q_ARG(double, rectBefore.right() + 180.0),
            Q_ARG(double, rectBefore.center().y()),
            Q_ARG(bool, false), Q_ARG(bool, true)));

        QVERIFY(media->fitToTextEnabled()); // Preview must not mutate the draft.
        QCOMPARE(fixture.controller.liveAltResizeScale(), scaleBefore);
        QCOMPARE(fixture.controller.liveAltResizeWidth(),
                 (rectBefore.width() + 180.0) / scaleBefore);

        QVERIFY(QMetaObject::invokeMethod(&fixture.controller,
            "handleMediaResizeEnded", Qt::DirectConnection, Q_ARG(QString, id)));
        QVERIFY(!media->fitToTextEnabled());
        QCOMPARE(media->scale(), scaleBefore);
        QCOMPARE(media->sceneRect().height(), rectBefore.height());
        QVERIFY(qAbs(media->sceneRect().width()
                     - (rectBefore.width() + 180.0)) <= scaleBefore / 2.0);
    }

    void selectionResizeUsesManipulatedMedia_data()
    {
        QTest::addColumn<QString>("handle");
        QTest::addColumn<QPointF>("uv");
        QTest::addColumn<bool>("alt");
        QTest::addColumn<bool>("snap");
        const QStringList handles{"top-left", "top-mid", "top-right", "left-mid",
                                  "right-mid", "bottom-left", "bottom-mid", "bottom-right"};
        const QList<QPointF> points{{0,0}, {.5,0}, {1,0}, {0,.5}, {1,.5}, {0,1}, {.5,1}, {1,1}};
        for (int i = 0; i < handles.size(); ++i)
            for (bool alt : {false, true})
                for (bool snap : {false, true}) {
                    const QByteArray name = (handles[i] + (alt ? "-alt" : "-uniform")
                        + (snap ? "-snap" : "-free")).toUtf8();
                    QTest::newRow(name.constData()) << handles[i] << points[i] << alt << snap;
                }
    }

    void selectionResizeUsesManipulatedMedia()
    {
        QFETCH(QString, handle);
        QFETCH(QPointF, uv);
        QFETCH(bool, alt);
        QFETCH(bool, snap);
        CanvasDocument document, reference;
        QuickCanvasController controller(&document), single(&reference);
        controller.initialize(); single.initialize();
        controller.setProjectEditingEnabled(true); single.setProjectEditingEnabled(true);
        auto add = [](CanvasDocument& doc, QPointF position, QSize size, qreal scale) {
            auto* media = doc.addText(position);
            media->setFitToTextEnabled(false);
            media->setBaseSize(size); media->setScale(scale); media->setPosition(position);
            return media;
        };
        auto* active = add(document, {100,100}, {200,100}, 1);
        auto* control = add(reference, {100,100}, {200,100}, 1);
        auto* follower = add(document, {-1700,-900}, {80,120}, 1.5);
        const QRectF original = active->sceneRect(), other = follower->sceneRect();
        const QSize targetSize(400, 200);
        const QPointF targetOrigin(100 - (1 - uv.x()) * 200, 100 - (1 - uv.y()) * 100);
        auto* target = add(document, targetOrigin, targetSize, 1);
        add(reference, targetOrigin, targetSize, 1);
        document.select(active->mediaId()); document.select(follower->mediaId(), true);
        reference.select(control->mediaId());
        const QPointF point = targetOrigin + QPointF(uv.x() * 400 + 2, uv.y() * 200 + 2);
        // Toggle modifiers within the same gesture; every update must still
        // derive from the original geometry, without accumulating scale drift.
        controller.handleMediaResizeRequested(active->mediaId(), handle, point.x() + 30, point.y() + 30, !snap, !alt);
        single.handleMediaResizeRequested(control->mediaId(), handle, point.x() + 30, point.y() + 30, !snap, !alt);
        controller.handleMediaResizeRequested(active->mediaId(), handle, point.x(), point.y(), snap, alt);
        single.handleMediaResizeRequested(control->mediaId(), handle, point.x(), point.y(), snap, alt);
        if (snap) QVERIFY(!controller.snapGuidesModel().isEmpty());
        QCOMPARE(controller.snapGuidesModel(), single.snapGuidesModel());
        QCOMPARE(active->sceneRect(), original);
        QCOMPARE(follower->sceneRect(), other);
        QCOMPARE(controller.liveTransforms().size(), 2);
        controller.handleMediaResizeEnded(active->mediaId());
        single.handleMediaResizeEnded(control->mediaId());
        QCOMPARE(active->sceneRect(), control->sceneRect());
        const QRectF result = active->sceneRect();
        const qreal sx = result.width() / original.width(), sy = result.height() / original.height();
        const QPointF expectedPosition = other.topLeft() + QPointF(
            (result.x() - original.x()) * other.width() / original.width(),
            (result.y() - original.y()) * other.height() / original.height());
        QVERIFY(QLineF(follower->position(), expectedPosition).length() < .001);
        QVERIFY(qAbs(follower->sceneRect().width() - other.width() * sx) <= .76);
        QVERIFY(qAbs(follower->sceneRect().height() - other.height() * sy) <= .76);
        QCOMPARE(follower->scale(), alt ? 1.5 : 1.5 * sx);
        QCOMPARE(target->sceneRect(), QRectF(targetOrigin, targetSize));
        QVERIFY(controller.liveTransforms().isEmpty());
    }

    void selectionMoveSnapExcludesFollowersAndUsesActiveMedia()
    {
        Fixture fixture;
        QVERIFY(fixture.initialize());
        auto* active = fixture.document.addText({0,0});
        auto* follower = fixture.document.addText({0,0});
        auto* target = fixture.document.addText({0,0});
        for (auto* item : {active, follower, target}) {
            item->setFitToTextEnabled(false); item->setBaseSize({200,100});
        }
        active->setPosition({100,100}); follower->setPosition({300,200}); target->setPosition({700,400});
        fixture.document.select(active->mediaId()); fixture.document.select(follower->mediaId(), true);
        auto& c = fixture.controller;
        c.handleMediaMoveStarted(active->mediaId(), 100, 100, true);
        c.handleMediaMoveUpdated(active->mediaId(), 302, 202, true);
        QVERIFY(c.liveSnapDragMediaId().isEmpty());
        c.handleMediaMoveUpdated(active->mediaId(), 702, 402, true);
        QCOMPARE(c.liveSnapDragX(), 700.0); QCOMPARE(c.liveSnapDragY(), 400.0);
        QCOMPARE(c.liveTransforms().value(follower->mediaId()).toMap().value("x").toReal(), 900.0);
        c.handleMediaMoveEnded(active->mediaId(), 702, 402, true);
        QCOMPARE(active->position(), QPointF(700,400));
        QCOMPARE(follower->position(), QPointF(900,500));
        QCOMPARE(target->position(), QPointF(700,400));
        // A delayed release without a new gesture cannot move anything.
        c.handleMediaMoveEnded(active->mediaId(), 0, 0, false);
        QCOMPARE(active->position(), QPointF(700,400));
    }

    void groupResizePreviewReachesContentAndOverlays()
    {
        Fixture fixture;
        QVERIFY(fixture.initialize());
        auto* active = fixture.document.addText({100,100});
        auto* follower = fixture.document.addText({600,300});
        for (auto* media : {active, follower}) {
            media->setFitToTextEnabled(false); media->setBaseSize({200,100});
        }
        active->setPosition({100,100}); follower->setPosition({600,300}); follower->setScale(1.5);
        fixture.document.select(active->mediaId()); fixture.document.select(follower->mediaId(), true);
        auto* root = fixture.view.rootObject();
        QQuickItem* visual = nullptr;
        QQuickItem* overlay = nullptr;
        QTRY_VERIFY((visual = findQuickItemWithProperty(root, "currentMediaId", follower->mediaId())));
        QTRY_VERIFY((overlay = findQuickItemWithProperty(root, "mid", follower->mediaId())));
        fixture.controller.handleMediaResizeRequested(active->mediaId(), "bottom-right", 500, 250, false, true);
        QTRY_COMPARE(visual->size(), QSizeF(400,150));
        QCOMPARE(visual->scale(), 1.5);
        QCOMPARE(visual->position(), QPointF(600,300));
        QCOMPARE(overlay->property("screenW").toReal(), 600.0);
        QCOMPARE(overlay->property("screenH").toReal(), 225.0);
        QCOMPARE(follower->baseSize(), QSize(200,100));
        fixture.controller.handleMediaResizeRequested(active->mediaId(), "bottom-right", 500, 300, false, false);
        QTRY_COMPARE(visual->size(), QSizeF(200,100));
        QCOMPARE(visual->scale(), 3.0);
        QCOMPARE(overlay->property("screenH").toReal(), 300.0);
        fixture.controller.handleMediaResizeEnded(active->mediaId());
        QTRY_COMPARE(visual->scale(), follower->scale());
        QCOMPARE(overlay->property("screenW").toReal(), follower->sceneRect().width());
    }

    void sceneLockDiscardsPendingSelectionEdits()
    {
        Fixture fixture;
        QVERIFY(fixture.initialize());
        auto* active = fixture.document.addText({300,300});
        auto* follower = fixture.document.addText({600,300});
        fixture.document.select(active->mediaId()); fixture.document.select(follower->mediaId(), true);
        const QJsonObject original = fixture.document.serializeProjectState();
        auto& c = fixture.controller;
        c.handleMediaResizeRequested(active->mediaId(), "bottom-right", 500, 400, false, true);
        QVERIFY(!c.liveTransforms().isEmpty());
        fixture.document.setEditsLocked(true);
        QVERIFY(c.liveTransforms().isEmpty()); QVERIFY(!c.editingEnabled());
        c.handleMediaResizeEnded(active->mediaId());
        c.handleMediaMoveStarted(active->mediaId(), 0, 0, false);
        c.handleMediaMoveUpdated(active->mediaId(), 900, 900, false);
        c.handleMediaMoveEnded(active->mediaId(), 900, 900, false);
        c.handleTextCommitRequested(active->mediaId(), "Changed");
        c.handleOverlayVisibilityToggle(active->mediaId(), false);
        c.handleOverlayDelete(active->mediaId());
        QCOMPARE(fixture.document.serializeProjectState(), original);
        fixture.document.setEditsLocked(false);
        c.handleMediaResizeEnded(active->mediaId());
        c.handleMediaMoveEnded(active->mediaId(), 900, 900, false);
        QCOMPARE(fixture.document.serializeProjectState(), original);
        QVERIFY(c.editingEnabled());
    }

    void runningSceneRejectsNativeEdits_data()
    {
        QTest::addColumn<bool>("testScene");
        QTest::addColumn<bool>("duringDrag");
        QTest::newRow("test-before-press") << true << false;
        QTest::newRow("test-during-drag") << true << true;
        QTest::newRow("remote-before-press") << false << false;
        QTest::newRow("remote-during-drag") << false << true;
    }

    void runningSceneRejectsNativeEdits()
    {
        QFETCH(bool, testScene);
        QFETCH(bool, duringDrag);
        QString error;
        std::unique_ptr<QuickCanvasHost> host(QuickCanvasHost::create(&error));
        QVERIFY2(host, qPrintable(error));
        host->setProjectEditingEnabled(true);
        QQuickView view;
        view.resize(1000, 700);
        view.setResizeMode(QQuickView::SizeRootObjectToView);
        view.setSource(QUrl("qrc:/qt/qml/Mouffette/App/resources/qml/CanvasRoot.qml"));
        auto* root = view.rootObject();
        QVERIFY(root);
        root->setProperty("sessionViewModel", QVariantMap{{"canvasController", QVariant::fromValue<QObject*>(host->controller())}});
        auto* media = host->document()->addText({300,250}, "Locked text");
        media->setFitToTextEnabled(false); media->setBaseSize({200,100}); media->setPosition({200,200});
        const QRectF original = media->sceneRect();
        view.show();
        QVERIFY(QTest::qWaitForWindowExposed(&view));
#ifdef Q_OS_MACOS
        MacWindowManager::activateApplicationWindow(&view);
#endif
        QVERIFY(QTest::qWaitForWindowActive(&view));
        QQuickItem* item = nullptr;
        QTRY_VERIFY((item = findQuickItemWithProperty(root, "currentMediaId", media->mediaId())));
        const QPoint start = item->mapToScene({100,50}).toPoint();
        if (duringDrag) {
            QTest::mousePress(&view, Qt::LeftButton, Qt::NoModifier, start);
            QTest::mouseMove(&view, start + QPoint(40,30), 20);
            QTRY_VERIFY(root->property("mediaMoveHandlerActive").toBool());
        }
        if (testScene) {
            host->triggerTestSceneAction();
            QVERIFY(host->testSceneLaunched());
        } else {
            // This is the synchronous lock used as soon as remote prepare starts.
            host->document()->setEditsLocked(true);
        }
        QTRY_VERIFY(!root->property("editingEnabled").toBool());
        if (!duringDrag) QTest::mousePress(&view, Qt::LeftButton, Qt::NoModifier, start);
        QTest::mouseMove(&view, start + QPoint(100,60), 20);
        QTest::mouseRelease(&view, Qt::LeftButton, Qt::NoModifier, start + QPoint(100,60));
        QCOMPARE(media->sceneRect(), original);
        QCOMPARE(item->position(), original.topLeft());
        QVERIFY(!root->property("mediaMoveHandlerActive").toBool());
        // Resize handles and text activation are locked too.
        const QPoint corner = item->mapToScene({200,100}).toPoint();
        QTest::mousePress(&view, Qt::LeftButton, Qt::AltModifier, corner);
        QTest::mouseMove(&view, corner + QPoint(70,40), 20);
        QTest::mouseRelease(&view, Qt::LeftButton, Qt::AltModifier, corner + QPoint(70,40));
        QTest::mouseDClick(&view, Qt::LeftButton, Qt::NoModifier, start);
        QTest::keyClick(&view, Qt::Key_X);
        QCOMPARE(media->sceneRect(), original);
        QCOMPARE(media->text(), QStringLiteral("Locked text"));
        QVERIFY(!root->property("anyMediaEditing").toBool());
        if (testScene) host->triggerTestSceneAction();
        else host->document()->setEditsLocked(false);
        QTRY_VERIFY(root->property("editingEnabled").toBool());
        QTest::mousePress(&view, Qt::LeftButton, Qt::NoModifier, start);
        QTest::mouseMove(&view, start + QPoint(50,30), 20);
        QTest::mouseMove(&view, start + QPoint(80,40), 20);
        QTest::mouseRelease(&view, Qt::LeftButton, Qt::NoModifier, start + QPoint(80,40));
        QVERIFY(QLineF(media->position(), original.topLeft()
            + QPointF(80,40) / host->controller()->viewScale()).length() < 0.01);
    }

    void moveSnapFitsACompleteTargetBoxAndReleasesCleanly()
    {
        Fixture fixture;
        QVERIFY(fixture.initialize());
        CanvasMedia* moving = fixture.document.addText({0, 0});
        CanvasMedia* target = fixture.document.addText({0, 0});
        QVERIFY(moving && target);
        for (CanvasMedia* media : {moving, target}) {
            media->setFitToTextEnabled(false);
            media->setBaseSize({400, 200});
        }
        moving->setPosition({50, 50});
        target->setPosition({500, 300});

        fixture.controller.handleMediaMoveStarted(
            moving->mediaId(), 50, 50, true);
        fixture.controller.handleMediaMoveUpdated(
            moving->mediaId(), 504, 306, true);
        QCOMPARE(fixture.controller.liveSnapDragX(), 500.0);
        QCOMPARE(fixture.controller.liveSnapDragY(), 300.0);
        QCOMPARE(fixture.controller.snapGuidesModel().size(), 4);

        fixture.controller.handleMediaMoveUpdated(
            moving->mediaId(), 105, 105, true);
        QCOMPARE(fixture.controller.liveSnapDragX(), 100.0);
        QCOMPARE(fixture.controller.liveSnapDragY(), 100.0);
        QCOMPARE(fixture.controller.snapGuidesModel().size(), 2);

        // Staying in Shift mode but leaving every capture zone must unfreeze
        // the former target immediately.
        fixture.controller.handleMediaMoveUpdated(
            moving->mediaId(), 540, 350, true);
        QVERIFY(fixture.controller.liveSnapDragMediaId().isEmpty());
        QVERIFY(fixture.controller.snapGuidesModel().isEmpty());
        fixture.controller.handleMediaMoveEnded(
            moving->mediaId(), 540, 350, true);
        QCOMPARE(moving->position(), QPointF(540, 350));
    }

    void uniformResizeSnapsInsideMatchingTarget()
    {
        Fixture fixture;
        QVERIFY(fixture.initialize());
        CanvasMedia* moving = fixture.document.addText({0, 0});
        CanvasMedia* target = fixture.document.addText({0, 0});
        QVERIFY(moving && target);
        moving->setFitToTextEnabled(false);
        moving->setBaseSize({400, 200});
        moving->setPosition({100, 100});
        target->setFitToTextEnabled(false);
        target->setBaseSize({800, 400});
        target->setPosition({100, 100});

        fixture.controller.handleMediaResizeRequested(
            moving->mediaId(), QStringLiteral("bottom-right"),
            894, 496, true, false);
        QCOMPARE(fixture.controller.liveResizeX(), 100.0);
        QCOMPARE(fixture.controller.liveResizeY(), 100.0);
        QCOMPARE(fixture.controller.liveResizeScale(), 2.0);
        QCOMPARE(fixture.controller.snapGuidesModel().size(), 4);
        fixture.controller.handleMediaResizeEnded(moving->mediaId());

        QCOMPARE(moving->sceneRect(), target->sceneRect());
        QVERIFY(fixture.controller.snapGuidesModel().isEmpty());
    }

    void uniformAxisResizeUsesZoomStableHysteresis()
    {
        Fixture fixture;
        QVERIFY(fixture.initialize());
        fixture.document.setScreens({ScreenInfo(0, 1000, 700, 0, 0, true)});
        CanvasMedia* moving = fixture.document.addText({0, 0});
        QVERIFY(moving);
        moving->setFitToTextEnabled(false);
        moving->setBaseSize({400, 200});
        moving->setPosition({100, 100});

        fixture.controller.handleMediaResizeRequested(
            moving->mediaId(), QStringLiteral("right-mid"),
            996, 200, true, false);
        QCOMPARE(fixture.controller.liveResizeScale(), 2.25);
        QVERIFY(!fixture.controller.snapGuidesModel().isEmpty());

        // The 10 px acquisition radius has a 14 px release radius, preventing
        // one-frame chatter at the boundary.
        fixture.controller.handleMediaResizeRequested(
            moving->mediaId(), QStringLiteral("right-mid"),
            1012, 200, true, false);
        QCOMPARE(fixture.controller.liveResizeScale(), 2.25);
        fixture.controller.handleMediaResizeRequested(
            moving->mediaId(), QStringLiteral("right-mid"),
            1016, 200, true, false);
        QVERIFY(qAbs(fixture.controller.liveResizeScale() - 2.29) < 0.0001);
        QVERIFY(fixture.controller.snapGuidesModel().isEmpty());
        fixture.controller.handleMediaResizeEnded(moving->mediaId());
    }

    void altResizeSnapsArbitraryDimensionsAndAxisEdges()
    {
        Fixture fixture;
        QVERIFY(fixture.initialize());
        CanvasMedia* moving = fixture.document.addText({0, 0});
        CanvasMedia* target = fixture.document.addText({0, 0});
        QVERIFY(moving && target);
        moving->setFitToTextEnabled(false);
        moving->setBaseSize({400, 200});
        moving->setPosition({100, 100});
        target->setFitToTextEnabled(false);
        target->setBaseSize({700, 500});
        target->setPosition({100, 100});

        fixture.controller.handleMediaResizeRequested(
            moving->mediaId(), QStringLiteral("bottom-right"),
            795, 596, true, true);
        QCOMPARE(fixture.controller.liveAltResizeX(), 100.0);
        QCOMPARE(fixture.controller.liveAltResizeY(), 100.0);
        QCOMPARE(fixture.controller.liveAltResizeWidth(), 700.0);
        QCOMPARE(fixture.controller.liveAltResizeHeight(), 500.0);
        QCOMPARE(fixture.controller.snapGuidesModel().size(), 4);
        fixture.controller.handleMediaResizeEnded(moving->mediaId());
        QCOMPARE(moving->sceneRect(), target->sceneRect());

        moving->setBaseSize({400, 200});
        moving->setPosition({100, 100});
        target->setBaseSize({300, 300});
        target->setPosition({1000, 50});
        fixture.controller.handleMediaResizeRequested(
            moving->mediaId(), QStringLiteral("right-mid"),
            994, 200, true, true);
        QCOMPARE(fixture.controller.liveAltResizeX(), 100.0);
        QCOMPARE(fixture.controller.liveAltResizeWidth(), 900.0);
        QCOMPARE(fixture.controller.liveAltResizeHeight(), 200.0);
        QVERIFY(!fixture.controller.snapGuidesModel().isEmpty());
        fixture.controller.handleMediaResizeEnded(moving->mediaId());
        QCOMPARE(moving->sceneRect(), QRectF(100, 100, 900, 200));
    }

    void snapAndDropImportUseDocumentCoordinates()
    {
        Fixture fixture;
        QVERIFY(fixture.initialize());
        fixture.document.setScreens({ScreenInfo(0, 1920, 1080, 0, 0, true)});
        CanvasMedia* media = fixture.document.addText({100, 100});
        const QString id = media->mediaId();

        QVERIFY(QMetaObject::invokeMethod(&fixture.controller,
            "handleMediaMoveStarted", Qt::DirectConnection,
            Q_ARG(QString, id), Q_ARG(double, 100.0), Q_ARG(double, 100.0),
            Q_ARG(bool, true)));
        QVERIFY(QMetaObject::invokeMethod(&fixture.controller,
            "handleMediaMoveUpdated", Qt::DirectConnection,
            Q_ARG(QString, id), Q_ARG(double, 2.0), Q_ARG(double, 3.0),
            Q_ARG(bool, true)));
        QVERIFY(QMetaObject::invokeMethod(&fixture.controller,
            "handleMediaMoveEnded", Qt::DirectConnection,
            Q_ARG(QString, id), Q_ARG(double, 2.0), Q_ARG(double, 3.0),
            Q_ARG(bool, true)));
        QCOMPARE(media->position(), QPointF(0, 0));

        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString imagePath = directory.filePath(QStringLiteral("drop.png"));
        QImage image(80, 60, QImage::Format_ARGB32_Premultiplied);
        image.fill(Qt::cyan);
        QVERIFY(image.save(imagePath));
        const int before = fixture.document.media().size();
        QVERIFY(fixture.controller.beginLocalFileDrag(
            {QUrl::fromLocalFile(imagePath)}, 500, 300));
        QVERIFY(fixture.controller.commitLocalFileDrop(520, 320));
        QCOMPARE(fixture.document.media().size(), before + 1);
        CanvasMedia* imported = fixture.document.selectedMedia();
        QVERIFY(imported && !imported->isText());
        QCOMPARE(imported->baseSize(), QSize(80, 60));
        QCOMPARE(imported->sceneRect().center(), QPointF(520, 320));
    }

    void imageDropPreviewIsRetiredBeforeTheMediaMoves()
    {
        Fixture fixture;
        QVERIFY(fixture.initialize());
        fixture.view.show();
        QVERIFY(QTest::qWaitForWindowExposed(&fixture.view));

        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString imagePath = directory.filePath(QStringLiteral("handoff.png"));
        QImage image(160, 90, QImage::Format_ARGB32_Premultiplied);
        image.fill(QColor("#27a8e0"));
        QVERIFY(image.save(imagePath));

        QVERIFY(fixture.controller.beginLocalFileDrag(
            {QUrl::fromLocalFile(imagePath)}, 500, 300));
        QVERIFY(fixture.controller.dropPreviewModel()
                    .value(QStringLiteral("visible")).toBool());
        QVERIFY(fixture.controller.commitLocalFileDrop(500, 300));
        CanvasMedia* imported = fixture.document.selectedMedia();
        QVERIFY(imported && !imported->isVideo() && !imported->isText());
        QTRY_VERIFY_WITH_TIMEOUT(
            !fixture.controller.dropPreviewModel()
                 .value(QStringLiteral("visible")).toBool(),
            5000);

        const QPointF originalPosition = imported->position();
        fixture.controller.handleMediaMoveStarted(
            imported->mediaId(), originalPosition.x(), originalPosition.y(), false);
        fixture.controller.handleMediaMoveUpdated(
            imported->mediaId(), originalPosition.x() + 50,
            originalPosition.y() + 25, false);
        fixture.controller.handleMediaMoveEnded(
            imported->mediaId(), originalPosition.x() + 50,
            originalPosition.y() + 25, false);
        QCOMPARE(imported->position(), originalPosition + QPointF(50, 25));
        QVERIFY(!fixture.controller.dropPreviewModel()
                     .value(QStringLiteral("visible")).toBool());
    }

    void realMouseDragMovesProductionMedia_data()
    {
        QTest::addColumn<QString>("mediaType");
        QTest::newRow("text") << QStringLiteral("text");
        QTest::newRow("image") << QStringLiteral("image");
        QTest::newRow("video") << QStringLiteral("video");
    }

    void realMouseDragMovesProductionMedia()
    {
        QFETCH(QString, mediaType);
        Fixture fixture;
        QVERIFY(fixture.initialize());
        fixture.view.show();
        QVERIFY(QTest::qWaitForWindowExposed(&fixture.view));

        CanvasMedia* media = nullptr;
        if (mediaType == QLatin1String("text")) {
            media = fixture.document.addText(
                {300, 250}, QStringLiteral("Drag me"));
        } else {
            const bool isVideo = mediaType == QLatin1String("video");
            const QString sourcePath = isVideo
                ? QString::fromUtf8(TEST_VIDEO_FILE)
                : QString::fromUtf8(TEST_WEBP_FILE);
            QVERIFY2(QFile::exists(sourcePath), qPrintable(sourcePath));
            media = fixture.document.addPreparedFile(
                sourcePath, {240, 140}, isVideo, {180, 160});
        }
        QVERIFY(media);
        if (media->isText()) media->setFitToTextEnabled(false);
        media->setBaseSize({240, 140});
        media->setPosition({180, 160});

        auto* follower = fixture.document.addText({650, 450}, "Follower");
        const QPointF followerStart = follower->position();
        fixture.document.select(media->mediaId());
        fixture.document.select(follower->mediaId(), true);
        QQuickItem* root = qobject_cast<QQuickItem*>(fixture.view.rootObject());
        QVERIFY(root);
        QQuickItem* delegate = nullptr;
        QTRY_VERIFY_WITH_TIMEOUT(
            (delegate = findQuickItemWithProperty(
                 root, "currentMediaId", media->mediaId())) != nullptr,
            3000);

        const QPoint start = delegate->mapToScene(
            QPointF(delegate->width() * 0.5, delegate->height() * 0.5)).toPoint();
        const QPoint end = start + QPoint(90, 55);
        const QPointF originalPosition = media->position();
        QSignalSpy started(root, SIGNAL(mediaMoveStarted(QString,double,double,bool)));
        QSignalSpy updated(root, SIGNAL(mediaMoveUpdated(QString,double,double,bool)));
        QSignalSpy ended(root, SIGNAL(mediaMoveEnded(QString,double,double,bool)));

        QTest::mouseMove(&fixture.view, start);
        QTest::mousePress(&fixture.view, Qt::LeftButton, Qt::NoModifier, start);
        for (int step = 1; step <= 5; ++step) {
            QTest::mouseMove(&fixture.view,
                             start + (end - start) * step / 5,
                             10);
            QCoreApplication::processEvents();
            if (step == 1) {
                QTRY_VERIFY(root->property("mediaMoveHandlerActive").toBool());
                QCOMPARE(root->property("activeMoveMediaId").toString(),
                         media->mediaId());
                QVERIFY(delegate->property("moveHandlerActive").toBool());
            }
        }
        QCOMPARE(fixture.document.selectedMediaIds().size(), 2);
        QCOMPARE(follower->position(), followerStart);
        auto* followerItem = findQuickItemWithProperty(root, "currentMediaId", follower->mediaId());
        QVERIFY(followerItem);
        QCOMPARE(followerItem->position(), followerStart + QPointF(90,55));
        QTest::mouseRelease(&fixture.view, Qt::LeftButton,
                            Qt::NoModifier, end);
        QCoreApplication::processEvents();

        QCOMPARE(started.count(), 1);
        QVERIFY(updated.count() > 0);
        QCOMPARE(ended.count(), 1);
        QCOMPARE(media->position(), originalPosition + QPointF(90, 55));
        QCOMPARE(follower->position(), followerStart + QPointF(90, 55));
    }

    void fullPageDragSurvivesPublicationAndResize_data()
    {
        QTest::addColumn<QString>("mediaType");
        QTest::addColumn<bool>("resizeViewport");
        for (const QString type : {QString("text"), QString("image"), QString("video")}) {
            QTest::newRow(qPrintable(type)) << type << false;
            QTest::newRow(qPrintable(type + "-resized-viewport")) << type << true;
        }
    }

    void fullPageDragSurvivesPublicationAndResize()
    {
        QFETCH(QString, mediaType);
        QFETCH(bool, resizeViewport);
        QString error;
        std::unique_ptr<QuickCanvasHost> host(QuickCanvasHost::create(&error));
        QVERIFY2(host, qPrintable(error));
        host->setProjectEditingEnabled(true);
        ClientWorkspaceViewModel session(QStringLiteral("drag-session"), host.get(),
            [] {}, nullptr, [] { return false; }, [] { return true; },
            [] { return true; });
        session.setLoading(false);

        QQmlEngine engine;
        QQuickWindow window;
        window.resize(1100, 800);
        QQmlComponent component(&engine, QUrl(QStringLiteral(
            "qrc:/qt/qml/Mouffette/App/resources/qml/app/pages/CanvasPage.qml")));
        QTRY_VERIFY_WITH_TIMEOUT(!component.isLoading(), 3000);
        std::unique_ptr<QObject> pageObject(component.createWithInitialProperties({
            {QStringLiteral("controller"), QVariantMap{
                {QStringLiteral("activeWorkspace"), QVariant::fromValue(&session)},
                {QStringLiteral("canCreateProject"), false},
                {QStringLiteral("canLaunchSession"), false},
                {QStringLiteral("connectionEnabled"), false}}}}));
        auto* page = qobject_cast<QQuickItem*>(pageObject.get());
        QVERIFY2(page, qPrintable(component.errorString()));
        page->setParentItem(window.contentItem());
        page->setPosition({37, 29});
        window.show();
        QVERIFY(QTest::qWaitForWindowExposed(&window));
#ifdef Q_OS_MACOS
        MacWindowManager::activateApplicationWindow(&window);
#else
        window.requestActivate();
#endif
        QVERIFY(QTest::qWaitForWindowActive(&window));
        page->setSize(QSizeF(window.width() - 74, window.height() - 58));
        auto* root = findQuickItemWithProperty(page, "canvasController",
            QVariant::fromValue<QObject*>(host->controller()));
        QVERIFY(root);
        host->controller()->updateCamera(0.75, 250, 100);
        if (resizeViewport) {
            const QPointF center = host->document()->cameraCenter();
            const qreal span = host->document()->cameraSquareSceneSize();
            page->setSize(page->size() - QSizeF(120, 100));
            QCOMPARE(host->document()->cameraCenter(), center);
            QCOMPARE(host->document()->cameraSquareSceneSize(), span);
        }
        const qreal cameraScale = host->controller()->viewScale();
        QCOMPARE(root->property("viewScale").toReal(), cameraScale);

        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        if (mediaType == QLatin1String("text")) {
            session.setActiveTool(QStringLiteral("text"));
            const QPoint createPoint = root->mapToScene({480, 300}).toPoint();
            QTest::mouseClick(&window, Qt::LeftButton, Qt::NoModifier, createPoint);
            QTRY_COMPARE(host->document()->media().size(), 1);
            QCOMPARE(session.activeTool(), QStringLiteral("selection"));
            QTRY_VERIFY(root->property("anyMediaEditing").toBool());
            QVERIFY(!root->property("textToolActive").toBool());
            // The creation click hands focus to the editor and selects the
            // placeholder so typing immediately replaces it.
            for (const char character : QByteArray("New title"))
                QTest::keyClick(&window, character);
            QTRY_COMPARE(host->document()->selectedMedia()->text(),
                         QStringLiteral("New title"));
            // The bottom-right media panel moves with the viewport. Use an
            // uncovered canvas point at either tested size.
            const QPoint background = root->mapToScene(QPointF(24, root->height() - 24)).toPoint();
            QTest::mouseClick(&window, Qt::LeftButton, Qt::NoModifier, background);
            QTRY_VERIFY(!root->property("anyMediaEditing").toBool());
            QCOMPARE(host->document()->media().size(), 1);
            host->document()->select(host->document()->media().constFirst()->mediaId());
        } else {
            QString path;
            if (mediaType == QLatin1String("video")) {
                path = qEnvironmentVariable("MOUFFETTE_TEST_VIDEO_FILE");
                if (path.isEmpty()) path = QString::fromUtf8(TEST_VIDEO_FILE);
                if (!QFile::exists(path)) QSKIP("Optional video fixture is missing");
            } else {
                path = directory.filePath(QStringLiteral("drag.png"));
                QImage image(240, 140, QImage::Format_RGB32);
                image.fill(Qt::cyan);
                QVERIFY(image.save(path));
            }
            QVERIFY(session.beginFileDrag({QUrl::fromLocalFile(path)}, 480, 300));
            QVERIFY(session.commitFileDrop(480, 300));
            QTRY_VERIFY_WITH_TIMEOUT(!host->controller()->dropPreviewModel()
                .value(QStringLiteral("visible")).toBool(), 5000);
        }
        CanvasMedia* media = host->document()->selectedMedia();
        QVERIFY(media);
        if (media->isText()) media->setFitToTextEnabled(false);
        media->setBaseSize({240, 140});
        media->setPosition({300, 240});
        session.setSettingsVisible(true);
        QQuickItem* delegate = nullptr;
        QTRY_VERIFY((delegate = findQuickItemWithProperty(
            root, "currentMediaId", media->mediaId())) != nullptr);
        QSignalSpy started(root, SIGNAL(mediaMoveStarted(QString,double,double,bool)));
        QSignalSpy ended(root, SIGNAL(mediaMoveEnded(QString,double,double,bool)));

        // Check the live position before release, across multiple watchdog ticks
        // and synchronous document/selection publications from the real backend.
        for (int gesture = 0; gesture < 2; ++gesture) {
            host->document()->clearSelection();
            const QPointF originalPosition = media->position();
            const QPoint start = delegate->mapToScene(
                {delegate->width() / 2, delegate->height() / 2}).toPoint();
            const QPoint delta(72, 45);
            QTest::mouseMove(&window, start);
            QTest::mousePress(&window, Qt::LeftButton, Qt::NoModifier, start);
            // Let Qt cross its native drag threshold before checking tracking.
            QTest::mouseMove(&window, start + QPoint(8, 8));
            for (int step = 1; step <= 3; ++step) {
                QTest::mouseMove(&window, start + delta * step / 3);
                // A real settings change republishes modelData during the grab.
                media->setContentOpacity(step % 2 ? 0.8 : 1.0);
                QTest::qWait(150);
                QCOMPARE(ended.count(), gesture);
                QVERIFY(delegate->property("localDragging").toBool());
                QVERIFY(QLineF(delegate->position(), originalPosition
                    + QPointF(delta * step / 3) / cameraScale).length() < 0.01);
            }
            QTest::mouseRelease(&window, Qt::LeftButton, Qt::NoModifier, start + delta);
            QTRY_COMPARE(ended.count(), gesture + 1);
            QCOMPARE(started.count(), gesture + 1);
            QVERIFY(QLineF(media->position(), originalPosition
                + QPointF(delta) / cameraScale).length() < 0.01);
            QCOMPARE(root->property("activeMediaDragCount").toInt(), 0);
            QCOMPARE(root->property("interactionMode").toString(), QStringLiteral("idle"));

            if (gesture == 0) {
                const QSizeF originalSize = media->sceneRect().size();
                const QPoint corner = delegate->mapToScene(
                    {delegate->width(), delegate->height()}).toPoint();
                QTest::mouseMove(&window, corner);
                QTest::mousePress(&window, Qt::LeftButton, Qt::NoModifier, corner);
                QTest::mouseMove(&window, corner + QPoint(10, 8), 20);
                QTest::mouseMove(&window, corner + QPoint(30, 20), 20);
                QTest::mouseRelease(&window, Qt::LeftButton, Qt::NoModifier,
                    corner + QPoint(30, 20));
                QTRY_VERIFY(media->sceneRect().width() > originalSize.width());
            }
        }
    }
};

QTEST_MAIN(CanvasSelectionBackendTest)
#include "tst_CanvasSelectionBackend.moc"
