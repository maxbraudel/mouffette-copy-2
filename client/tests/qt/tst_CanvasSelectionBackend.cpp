#include <QApplication>
#include <QClipboard>
#include <QDir>
#include <QElapsedTimer>
#include <QJsonArray>
#include <QMimeData>
#include <QMouseEvent>
#include "backend/files/FileManager.h"
#include "backend/media/MediaResidencyManager.h"
#include "backend/media/MediaDecoder.h"
#include "backend/media/MediaBackendBootstrap.h"
#include "backend/runtime/RuntimeProfile.h"
#include "frontend/ui/notifications/ToastNotificationSystem.h"
#include <QFile>
#include <QFutureWatcher>
#include <QImage>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QQuickItem>
#include <QQuickStyle>
#include <QQuickView>
#include <QScopeGuard>
#include <QSemaphore>
#include <QStyleHints>
#include <QTemporaryDir>
#include <QThreadPool>
#include <QTimer>
#include <QtConcurrent/QtConcurrentRun>
#include <QtTest>
#include <limits>
#include <array>
#include <cmath>
#include <QtQuick/private/qquickpinchhandler_p.h>
#include <QtQuick/private/qquicktextedit_p.h>

#include "backend/domain/canvas/CanvasDocument.h"
#include "backend/config/AppConfig.h"
#include "backend/domain/media/CanvasMedia.h"
#include "backend/domain/project/ProjectModel.h"
#include "frontend/rendering/canvas/MediaListModel.h"
#include "frontend/rendering/canvas/QuickCanvasController.h"
#include "frontend/rendering/canvas/QuickCanvasHost.h"
#include "frontend/rendering/canvas/TextOutlineItem.h"
#include "frontend/rendering/remote/RemoteVideoFrameItem.h"
#include "shared/rendering/MediaFrameSource.h"
#include "frontend/qml/ClientWorkspaceViewModel.h"
#include "frontend/qml/MediaSettingsViewModel.h"
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
    void init()
    {
        MediaResidencyManager::instance().setMemorySnapshotForTesting(
            {8ULL << 30, 6ULL << 30, 512ULL << 20, false, 0});
    }

    void cleanup()
    {
        MediaResidencyManager::instance().clearMemorySnapshotForTesting();
    }

    void initTestCase()
    {
        // The complete page must use the same controls as production main().
        QQuickStyle::setStyle(QStringLiteral("Basic"));
    }

    void frameSourcePublishesOnlyChangedContentAndAvailability()
    {
        RemoteVideoFrameSource source;
        QSignalSpy frames(&source, &RemoteVideoFrameSource::frameChanged);
        QSignalSpy availability(&source, &RemoteVideoFrameSource::hasFrameChanged);
        source.clear();
        source.setFrame({});
        QCOMPARE(frames.count(), 0);
        QCOMPARE(availability.count(), 0);

        QImage image(32, 16, QImage::Format_RGBA8888);
        image.fill(Qt::cyan);
        source.setFrame(image);
        QVERIFY(source.hasFrame());
        QCOMPARE(frames.count(), 1);
        QCOMPARE(availability.count(), 1);

        const QImage shared = image;
        source.setFrame(image);
        source.setFrame(shared);
        QCOMPARE(source.frame().cacheKey(), image.cacheKey());
        QCOMPARE(frames.count(), 1);
        QCOMPARE(availability.count(), 1);

        // A content edit detaches the shared image and must still reach renderers.
        image.setPixelColor(0, 0, Qt::magenta);
        source.setFrame(image);
        QCOMPARE(source.frame().pixelColor(0, 0), QColor(Qt::magenta));
        QCOMPARE(frames.count(), 2);
        QCOMPARE(availability.count(), 1);

        source.setFrame({});
        QVERIFY(!source.hasFrame());
        QCOMPARE(frames.count(), 3);
        QCOMPARE(availability.count(), 2);
        source.clear();
        QCOMPARE(frames.count(), 3);
        QCOMPARE(availability.count(), 2);
    }

    void imageGeometryAndDuplicateImportsPreserveResidentFrame()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString originalPath = directory.filePath(QStringLiteral("image.png"));
        const QString duplicatePath = directory.filePath(QStringLiteral("copy.png"));
        QImage image(128, 64, QImage::Format_RGBA8888);
        image.fill(Qt::cyan);
        QVERIFY(image.save(originalPath));
        QVERIFY(QFile::copy(originalPath, duplicatePath));

        CanvasMedia media(CanvasMedia::Type::Image, image.size());
        media.setSourcePath(originalPath);
        QTRY_VERIFY_WITH_TIMEOUT(media.residencyReady(), 10000);
        auto& residency = MediaResidencyManager::instance();
        const auto asset = residency.asset(media.residencyOwnerId());
        QVERIFY(asset);
        auto* source = qobject_cast<RemoteVideoFrameSource*>(
            media.toModelMap().value(QStringLiteral("residentFrameSource")).value<QObject*>());
        QVERIFY(source);
        const qint64 cacheKey = source->frame().cacheKey();
        const quint64 residentBytes = asset->residentBytes;
        QSignalSpy frames(source, &RemoteVideoFrameSource::frameChanged);
        QSignalSpy availability(source, &RemoteVideoFrameSource::hasFrameChanged);
        QSignalSpy residencyChanges(&media, &CanvasMedia::residencyChanged);

        for (qreal scale : {10000.0, 0.01, 20.0, 1.0}) {
            media.setScale(scale);
            media.setBaseSize({200000, 100000});
            media.setPosition({-150000, -75000});
            media.setBaseSize(image.size());
        }
        QCoreApplication::processEvents();
        QCOMPARE(residencyChanges.count(), 0);
        QCOMPARE(residency.asset(media.residencyOwnerId()), asset);
        QCOMPARE(source->frame().size(), image.size());
        QCOMPARE(source->frame().cacheKey(), cacheKey);
        QCOMPARE(asset->residentBytes, residentBytes);
        QCOMPARE(frames.count(), 0);
        QCOMPARE(availability.count(), 0);

        // Content deduplication republishes the shared asset to existing owners.
        // Its identical pixels must not invalidate their renderer textures.
        CanvasMedia duplicate(CanvasMedia::Type::Image, image.size());
        duplicate.setSourcePath(duplicatePath);
        QTRY_VERIFY_WITH_TIMEOUT(duplicate.residencyReady(), 10000);
        QCOMPARE(residency.asset(duplicate.residencyOwnerId()), asset);
        QVERIFY(residencyChanges.count() > 0);
        QCOMPARE(source->frame().cacheKey(), cacheKey);
        QCOMPARE(frames.count(), 0);
        QCOMPARE(availability.count(), 0);
    }

    void pendingMetadataImportDoesNotWaitForBulkWorkers_data()
    {
        QTest::addColumn<bool>("video");
        QTest::newRow("image") << false;
        QTest::newRow("video") << true;
    }

    void pendingMetadataImportDoesNotWaitForBulkWorkers()
    {
        QFETCH(bool, video);
        QTemporaryDir directory;
        const QString path = video ? QString::fromUtf8(TEST_VIDEO_FILE)
            : directory.filePath(QStringLiteral("queued-behind-video.png"));
        if (!video) {
            QImage image(96, 54, QImage::Format_ARGB32);
            image.fill(Qt::cyan);
            QVERIFY(image.save(path));
        }
        const auto geometry = MediaDecoder::inspectGeometry(path);
        QVERIFY(geometry.accepted());

        QThreadPool* pool = QThreadPool::globalInstance();
        const int previousThreadCount = pool->maxThreadCount();
        QSemaphore started;
        QSemaphore releaseWorker;
        QFuture<void> blocker;
        const auto restorePool = qScopeGuard([&] {
            // QVERIFY/QCOMPARE return early on failure; always release the
            // worker before its referenced semaphores leave this scope.
            releaseWorker.release();
            blocker.waitForFinished();
            pool->setMaxThreadCount(previousThreadCount);
        });
        pool->setMaxThreadCount(1);
        blocker = QtConcurrent::run(pool, [&] {
            started.release();
            releaseWorker.acquire();
        });
        QTRY_VERIFY_WITH_TIMEOUT(started.available() == 1, 5000);

        // Simulate a full video validation occupying every bulk worker. The
        // canvas shell only needs metadata and must still be adopted now.
        CanvasDocument document;
        const QPointF center(431.25, -62.5);
        const QString id = document.queueFileImport(path, center);
        QVERIFY(!id.isEmpty());
        QTRY_VERIFY_WITH_TIMEOUT(document.mediaById(id) != nullptr, 5000);
        CanvasMedia* media = document.mediaById(id);
        QCOMPARE(media->sceneRect().size(), QSizeF(geometry.displaySize));
        QCOMPARE(media->sceneRect().center(), center);
        QVERIFY(media->selected());
        QVERIFY(!document.hasPendingImports());
        QVERIFY(!media->residencyReady());
        QVERIFY(!blocker.isFinished());
        if (video) {
            QVERIFY(media->player());
            media->setMuted(true);
            media->setVolume(0.27);
            QVERIFY(media->muted());
            QVERIFY(qAbs(media->volume() - 0.27) < 0.001);
        }
    }

    void pendingMetadataImportSurvivesProjectRoundTrip()
    {
        QTemporaryDir directory;
        const QString path = directory.filePath(QStringLiteral("pending.png"));
        QImage image(96, 54, QImage::Format_ARGB32);
        image.fill(Qt::cyan);
        QVERIFY(image.save(path));
        const QPointF center(431.25, -62.5);
        auto original = std::make_unique<CanvasDocument>();
        const QString id = original->queueFileImport(path, center);
        QVERIFY(!id.isEmpty());
        QVERIFY(original->hasPendingImports());
        QVERIFY(original->media().isEmpty());
        ProjectRecord saved;
        saved.canvasState = original->serializeProjectState();
        const auto pending = saved.toJson().value(QStringLiteral("canvasState"))
            .toObject().value(QStringLiteral("pendingImports")).toArray();
        QCOMPARE(pending.size(), 1);
        QCOMPARE(pending.first().toObject().value(QStringLiteral("mediaId")).toString(), id);
        QVERIFY(!pending.first().toObject().value(QStringLiteral("sourceSignature")).toString().isEmpty());
        QVERIFY(!original->serializeSceneState().contains(QStringLiteral("pendingImports")));
        original.reset(); // Closing the document cannot publish its old callback.

        CanvasDocument restored;
        QStringList skipped;
        QVERIFY(restored.restoreProjectState(saved.canvasStateForRestore(), {}, &skipped));
        QVERIFY(skipped.isEmpty());
        QVERIFY(restored.hasPendingImports());
        QTRY_VERIFY_WITH_TIMEOUT(!restored.hasPendingImports(), 5000);
        QCOMPARE(restored.media().size(), 1);
        CanvasMedia* media = restored.mediaById(id);
        QVERIFY(media);
        QCOMPARE(media->baseSize(), QSize(96, 54));
        QCOMPARE(media->sceneRect().center(), center);
        QVERIFY(media->selected());
        QVERIFY(!restored.serializeProjectState().contains(QStringLiteral("pendingImports")));
        QTRY_VERIFY_WITH_TIMEOUT(media->residencyReady(), 5000);
    }

    void clearingPendingMetadataImportDiscardsCompletion()
    {
        QTemporaryDir directory;
        const QString path = directory.filePath(QStringLiteral("cancel.png"));
        QImage image(80, 50, QImage::Format_ARGB32);
        image.fill(Qt::green);
        QVERIFY(image.save(path));
        CanvasDocument document;
        QSignalSpy added(&document, &CanvasDocument::mediaAdded);
        const QString id = document.queueFileImport(path, {50, 50});
        QVERIFY(!id.isEmpty());
        QVERIFY(document.hasPendingImports());
        document.clear();
        QVERIFY(!document.hasPendingImports());
        QTRY_VERIFY_WITH_TIMEOUT(document.findChildren<QFutureWatcherBase*>().isEmpty(), 5000);
        QCOMPARE(added.count(), 0);
        QVERIFY(document.media().isEmpty());
        QVERIFY(!document.serializeProjectState().contains(QStringLiteral("pendingImports")));

        const QString retry = document.queueFileImport(path, {70, 90});
        QVERIFY(!retry.isEmpty());
        QVERIFY(retry != id);
        QTRY_VERIFY_WITH_TIMEOUT(document.mediaById(retry), 5000);
        QCOMPARE(document.media().size(), 1);
        QCOMPARE(document.mediaById(retry)->sceneRect().center(), QPointF(70, 90));
    }

    void pendingMetadataRestoreRejectsChangedSource()
    {
        QTemporaryDir directory;
        const QString path = directory.filePath(QStringLiteral("changed.png"));
        QImage original(80, 50, QImage::Format_ARGB32);
        original.fill(Qt::red);
        QVERIFY(original.save(path));
        CanvasDocument document;
        const QString id = document.queueFileImport(path, {0, 0});
        const QJsonObject saved = document.serializeProjectState();
        document.clear();
        QVERIFY(QFile::remove(path));
        QImage replacement(17, 9, QImage::Format_ARGB32);
        replacement.fill(Qt::blue);
        QVERIFY(replacement.save(path));
        CanvasDocument restored;
        QStringList skipped;
        QVERIFY(restored.restoreProjectState(saved, {}, &skipped));
        QCOMPARE(skipped, QStringList{id});
        QVERIFY(!restored.hasPendingImports());
        QVERIFY(restored.media().isEmpty());
    }

    void pendingMetadataImportDefersWhileDocumentLocked()
    {
        QTemporaryDir directory;
        const QString path = directory.filePath(QStringLiteral("locked.png"));
        QImage image(40, 30, QImage::Format_ARGB32);
        image.fill(Qt::yellow);
        QVERIFY(image.save(path));
        CanvasDocument document;
        const QString id = document.queueFileImport(path, {23, 42});
        QVERIFY(!id.isEmpty());
        document.setEditsLocked(true);
        QTRY_VERIFY_WITH_TIMEOUT(document.findChildren<QFutureWatcherBase*>().isEmpty(), 5000);
        QVERIFY(document.hasPendingImports());
        QVERIFY(document.media().isEmpty());
        document.setEditsLocked(false);
        QTRY_VERIFY_WITH_TIMEOUT(document.mediaById(id), 5000);
        QVERIFY(!document.hasPendingImports());
        QCOMPARE(document.mediaById(id)->sceneRect().center(), QPointF(23, 42));
    }

    void textCreationSizeFollowsCameraSquare_data()
    {
        QTest::addColumn<int>("percent");
        QTest::newRow("default") << 8;
        QTest::newRow("custom") << 5;
        QTest::newRow("minimum") << 1;
        QTest::newRow("maximum") << 100;
    }

    void textCreationSizeFollowsCameraSquare()
    {
        QFETCH(int, percent);
        const AppConfig previous = AppConfig::instance();
        const auto restoreConfig = qScopeGuard([&] { AppConfig::instance() = previous; });
        AppConfig::LoadOptions options;
        options.defaultEnvFilePath = QString();
        options.processEnvironment = QProcessEnvironment();
        options.arguments = {"test", QString("--canvas-text-initial-height-percent=%1").arg(percent)};
        QString error;
        QVERIFY2(AppConfig::instance().load(options, &error), qPrintable(error));
        CanvasDocument document;
        QuickCanvasController controller(&document);
        controller.setProjectEditingEnabled(true);
        auto* legacy = document.addText({100, 200});
        QCOMPARE(legacy->scale(), 1.0);
        const QRectF legacyRect = legacy->sceneRect();
        const QList<QSizeF> sizes{{1200, 800}, {600, 400}, {400, 900}, {400, 900}, {400, 900}};
        const QList<qreal> spans{2000, 2000, 2000, 1000, 100};
        QList<CanvasMedia*> created;
        for (qsizetype i = 0; i < sizes.size(); ++i) {
            controller.setViewportSize(sizes[i].width(), sizes[i].height());
            document.setCameraView({320, -85}, spans[i]);
            const qreal height = spans[i] * percent / 100.0;
            const QPointF click(310, 220);
            const QPointF center = (click - QPointF(controller.panX(), controller.panY()))
                / controller.viewScale();
            qreal publishedHeight = 0;
            const auto connection = connect(&document, &CanvasDocument::mediaAdded,
                &document, [&](CanvasMedia* media) {
                    publishedHeight = media->sceneRect().height();
                });
            controller.handleTextCreateRequested(click.x(), click.y());
            disconnect(connection);
            auto* media = document.selectedMedia();
            QVERIFY(media && media != legacy);
            QVERIFY(qAbs(publishedHeight - height) < 1e-8);
            QVERIFY(qAbs(media->sceneRect().height() - height) < 1e-8);
            QVERIFY(QLineF(media->sceneRect().center(), center).length() < 1e-8);
            QVERIFY(qAbs(media->sceneRect().height() * controller.viewScale()
                / qMin(sizes[i].width(), sizes[i].height()) - percent / 100.0) < 1e-8);
            QVERIFY(media->fitToTextEnabled());
            QCOMPARE(media->fontPixelSize(), legacy->fontPixelSize());
            created.append(media);
            QCOMPARE(legacy->sceneRect(), legacyRect);
        }
        QCOMPARE(created[0]->sceneRect().height(), created[1]->sceneRect().height());
        QCOMPARE(created[1]->sceneRect().height(), created[2]->sceneRect().height());
        QCOMPARE(created[2]->sceneRect().height(), 2 * created[3]->sceneRect().height());
        auto* edited = created.last();
        const qreal scale = edited->scale();
        const qreal originalHeight = edited->sceneRect().height();
        edited->setText("First line\nSecond line");
        QCOMPARE(edited->scale(), scale);
        QVERIFY(edited->sceneRect().height() > originalHeight);
        const QJsonObject saved = document.serializeProjectState();
        CanvasDocument restored;
        QVERIFY(restored.restoreProjectState(saved, {}));
        QCOMPARE(restored.media().size(), document.media().size());
        for (auto* media : document.media()) {
            auto* copy = restored.mediaById(media->mediaId());
            QVERIFY(copy);
            QCOMPARE(copy->scale(), media->scale());
            QCOMPARE(copy->sceneRect(), media->sceneRect());
            QCOMPARE(copy->text(), media->text());
            QVERIFY(copy->fitToTextEnabled());
        }
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

    void nativePinchUsesOnlyEachMovementDelta()
    {
        QPointingDevice trackpad("test trackpad", 0xCAFE,
            QInputDevice::DeviceType::TouchPad, QPointingDevice::PointerType::Finger,
            QInputDevice::Capability::Position | QInputDevice::Capability::PixelScroll, 2, 0);
        Fixture fixture;
        QVERIFY(fixture.initialize());
        fixture.view.show();
        QVERIFY(QTest::qWaitForWindowExposed(&fixture.view));
#ifdef Q_OS_MACOS
        MacWindowManager::activateApplicationWindow(&fixture.view);
#else
        fixture.view.requestActivate();
#endif
        QVERIFY(QTest::qWaitForWindowActive(&fixture.view));
        auto* pinch = fixture.view.rootObject()->findChild<QQuickPinchHandler*>();
        QVERIFY(pinch);
        fixture.document.setCameraView({230, -80}, 1500);
        const QPointF cursor(fixture.view.width() * .63, fixture.view.height() * .42);
        auto sceneUnderCursor = [&] {
            return (cursor - QPointF(fixture.controller.panX(), fixture.controller.panY()))
                / fixture.controller.viewScale();
        };
        auto sendGesture = [&](Qt::NativeGestureType type, qreal value = 0.0) {
            QNativeGestureEvent event(type, &trackpad, 2, cursor, cursor,
                fixture.view.mapToGlobal(cursor.toPoint()), value, {});
            QCoreApplication::sendEvent(&fixture.view, &event);
        };
        // A tiny second pinch must not reapply the scale accumulated by the
        // first one. Wheel zoom and viewport resize between pinches must not
        // change the interpretation of subsequent native magnification deltas.
        const QList<QList<qreal>> gestures{{-.2, -.1}, {-.01}, {.01, .02}, {-.005, .005}};
        for (const auto& deltas : gestures) {
            qreal expectedScale = fixture.controller.viewScale();
            const QPointF anchor = sceneUnderCursor();
            sendGesture(Qt::BeginNativeGesture);
            QVERIFY(pinch->active());
            QCOMPARE(fixture.controller.viewScale(), expectedScale);
            for (qreal delta : deltas) {
                sendGesture(Qt::ZoomNativeGesture, delta);
                expectedScale *= 1.0 + delta;
                QVERIFY2(qAbs(fixture.controller.viewScale() / expectedScale - 1.0) < 1e-9,
                    qPrintable(QString("Pinch delta %1: expected scale %2, got %3")
                        .arg(delta).arg(expectedScale).arg(fixture.controller.viewScale())));
                QVERIFY(QLineF(sceneUnderCursor(), anchor).length() < 1e-8);
            }
            sendGesture(Qt::EndNativeGesture);
            QVERIFY(!pinch->active());
            QVERIFY(qAbs(fixture.controller.viewScale() / expectedScale - 1.0) < 1e-9);
            fixture.controller.zoomAt(cursor.x(), cursor.y(), 1.03);
            fixture.view.resize(fixture.view.width() - 5, fixture.view.height() - 3);
        }
    }

    void altScrollScalesSelectionAroundItsCenter_data()
    {
        QTest::addColumn<QString>("mediaType");
        QTest::addColumn<bool>("trackpad");
        QTest::addColumn<bool>("naturalScrolling");
        QTest::addColumn<bool>("overMedia");
        for (const QString type : {QStringLiteral("text"), QStringLiteral("image"), QStringLiteral("video")})
            for (int device = 0; device < 3; ++device)
                for (bool over : {false, true})
                    QTest::newRow(qPrintable(type + QString("-%1-%2").arg(device).arg(over)))
                        << type << (device > 0) << (device == 2) << over;
    }

    void altScrollScalesSelectionAroundItsCenter()
    {
        QFETCH(QString, mediaType);
        QFETCH(bool, trackpad);
        QFETCH(bool, naturalScrolling);
        QFETCH(bool, overMedia);
        QPointingDevice device("test scroll device", 0xCB01,
            trackpad ? QInputDevice::DeviceType::TouchPad : QInputDevice::DeviceType::Mouse,
            trackpad ? QPointingDevice::PointerType::Finger : QPointingDevice::PointerType::Generic,
            QInputDevice::Capability::Position | QInputDevice::Capability::Scroll
                | QInputDevice::Capability::PixelScroll, 2, 3);
        Fixture fixture;
        QVERIFY(fixture.initialize());
        QTemporaryDir directory;
        const bool video = mediaType == "video";
        const QString path = video ? QString::fromUtf8(TEST_VIDEO_FILE) : directory.filePath("image.png");
        QImage image(160, 90, QImage::Format_ARGB32_Premultiplied);
        image.fill(Qt::cyan);
        if (!video) QVERIFY(image.save(path));
        CanvasMedia* media = mediaType == "text"
            ? fixture.document.addText({250, 220}, "Scale me")
            : fixture.document.addPreparedFile(path, image.size(), video, {250, 220});
        QVERIFY(media);
        media->setFitToTextEnabled(false);
        media->setBaseSize({160, 90});
        media->setScale(1.5);
        fixture.document.select(media->mediaId(), false);
        fixture.view.show();
        QVERIFY(QTest::qWaitForWindowExposed(&fixture.view));
        QCoreApplication::processEvents();
        const QRectF original = media->sceneRect();
        const QSize baseSize = media->baseSize();
        const qreal scale = media->scale();
        const auto camera = fixture.document.cameraCenter();
        const auto span = fixture.document.cameraSquareSceneSize();
        const QPointF cursor = overMedia
            ? original.center() * fixture.controller.viewScale()
                + QPointF(fixture.controller.panX(), fixture.controller.panY())
            : QPointF(fixture.view.width() - 35, fixture.view.height() - 35);
        auto scroll = [&](int direction, Qt::ScrollPhase phase) {
            const int sign = direction * (naturalScrolling ? -1 : 1);
            QWheelEvent event(cursor, fixture.view.mapToGlobal(cursor.toPoint()),
                trackpad ? QPoint(0, sign * 24) : QPoint(), QPoint(0, sign * 120),
                Qt::NoButton, Qt::AltModifier, trackpad ? phase : Qt::NoScrollPhase,
                naturalScrolling, Qt::MouseEventNotSynthesized, &device);
            QCoreApplication::sendEvent(&fixture.view, &event);
        };
        scroll(1, Qt::ScrollBegin);
        auto preview = [&] {
            return fixture.controller.liveTransforms().value(media->mediaId()).toMap();
        };
        QTRY_VERIFY(preview().value("scale").toReal() > scale);
        QCOMPARE(media->scale(), scale);
        const QVariantMap enlarged = preview();
        const QRectF enlargedRect(enlarged.value("x").toReal(), enlarged.value("y").toReal(),
            enlarged.value("width").toReal() * enlarged.value("scale").toReal(),
            enlarged.value("height").toReal() * enlarged.value("scale").toReal());
        QVERIFY(QLineF(enlargedRect.center(), original.center()).length() < 1e-8);
        QCOMPARE(media->baseSize(), baseSize);
        QVERIFY(!media->fitToTextEnabled());
        scroll(-1, Qt::ScrollUpdate);
        QTRY_VERIFY(qAbs(preview().value("scale").toReal() - scale) < 1e-8);
        scroll(0, Qt::ScrollEnd);
        QTRY_VERIFY(fixture.controller.liveTransforms().isEmpty());
        QVERIFY(qAbs(media->scale() - scale) < 1e-8);
        QVERIFY(QLineF(media->position(), original.topLeft()).length() < 1e-8);

        if (trackpad) {
            QWheelEvent horizontal(cursor, fixture.view.mapToGlobal(cursor.toPoint()), {30, 0}, {120, 120},
                Qt::NoButton, Qt::AltModifier, Qt::ScrollBegin, naturalScrolling,
                Qt::MouseEventNotSynthesized, &device);
            QCoreApplication::sendEvent(&fixture.view, &horizontal);
            scroll(0, Qt::ScrollEnd);
            QVERIFY(qAbs(media->scale() - scale) < 1e-8);
        }
        fixture.document.clearSelection();
        scroll(1, Qt::ScrollBegin);
        scroll(0, Qt::ScrollEnd);
        QVERIFY(qAbs(media->scale() - scale) < 1e-8);
        fixture.document.select(media->mediaId(), false);
        fixture.document.setEditsLocked(true);
        scroll(1, Qt::ScrollBegin);
        scroll(0, Qt::ScrollEnd);
        QVERIFY(qAbs(media->scale() - scale) < 1e-8);
        QCOMPARE(fixture.document.cameraCenter(), camera);
        QCOMPARE(fixture.document.cameraSquareSceneSize(), span);
    }

    void selectionScaleGestureCoalescesFramesWithoutPublishingDocument()
    {
        Fixture fixture;
        QVERIFY(fixture.initialize());
        auto* first = fixture.document.addText({10, 20}, "First");
        auto* second = fixture.document.addText({400, 300}, "Second");
        first->setOutlineWidthOverrideEnabled(true);
        first->setOutlineWidthPercent(3.0);
        first->setBaseSize({200, 100});
        second->setBaseSize({150, 80});
        second->setScale(2.0);
        fixture.document.select(first->mediaId(), false);
        fixture.document.select(second->mediaId(), true);
        fixture.view.show();
        QVERIFY(QTest::qWaitForWindowExposed(&fixture.view));
        QCoreApplication::processEvents();
        // Loader-created visuals follow QQuickItem parenting; their QObject
        // ownership can stay with a QML component instead of the canvas root.
        QList<TextOutlineItem*> outlines;
        auto collectOutlines = [&](auto&& collect, QQuickItem* item) -> void {
            if (auto* outline = qobject_cast<TextOutlineItem*>(item))
                outlines.append(outline);
            for (auto* child : item->childItems()) collect(collect, child);
        };
        collectOutlines(collectOutlines, fixture.view.rootObject());
        QCOMPARE(outlines.size(), 2);
        for (auto* outline : outlines) QVERIFY(!outline->rasterUpdatesDeferred());
        const QRectF firstRect = first->sceneRect(), secondRect = second->sceneRect();
        QSignalSpy documentChanges(&fixture.document, &CanvasDocument::documentChanged);
        QSignalSpy snapshots(&fixture.controller, &QuickCanvasController::mediaSnapshotChanged);
        QSignalSpy selection(&fixture.controller, &QuickCanvasController::selectionChromeModelChanged);
        QSignalSpy modelChanges(fixture.controller.mediaListModel(), &QAbstractItemModel::dataChanged);
        QSignalSpy previews(&fixture.controller, &QuickCanvasController::liveTransformsChanged);
        QSignalSpy presentation(&fixture.controller, &QuickCanvasController::presentationChanged);
        for (int i = 0; i < 100; ++i)
            fixture.controller.updateSelectionScaleGesture(1.001, true);
        QCOMPARE(previews.count(), 0);
        QCOMPARE(documentChanges.count(), 0);
        QCOMPARE(snapshots.count(), 0);
        QCOMPARE(selection.count(), 0);
        QCOMPARE(modelChanges.count(), 0);
        QTRY_COMPARE(previews.count(), 1);
        for (auto* outline : outlines) QVERIFY(outline->rasterUpdatesDeferred());
        QCOMPARE(presentation.count(), 0);
        const QVariantMap preview = fixture.controller.liveTransforms().value(first->mediaId()).toMap();
        QVERIFY(qAbs(preview.value("scale").toReal() - std::pow(1.001, 100)) < 1e-10);
        QCOMPARE(first->sceneRect(), firstRect);
        QCOMPARE(second->sceneRect(), secondRect);

        bool completeGeometryOnly = true;
        const auto geometryConnection = connect(&fixture.document, &CanvasDocument::mediaChanged, this, [&](const QString&) {
            completeGeometryOnly &= QLineF(first->sceneRect().center(), firstRect.center()).length() < 1e-8;
            completeGeometryOnly &= QLineF(second->sceneRect().center(), secondRect.center()).length() < 1e-8;
        });
        for (int i = 0; i < 100; ++i)
            fixture.controller.updateSelectionScaleGesture(1.001, true);
        QCOMPARE(previews.count(), 1);
        QCOMPARE(documentChanges.count(), 0);
        // End can arrive before the next frame. It must include every packet.
        fixture.controller.finishSelectionScaleGesture();
        QVERIFY(qAbs(first->scale() - std::pow(1.001, 200)) < 1e-10);
        QVERIFY(qAbs(second->scale() - 2.0 * first->scale()) < 1e-10);
        QVERIFY(completeGeometryOnly);
        QCOMPARE(documentChanges.count(), 2);
        QCOMPARE(snapshots.count(), 2);
        QCOMPARE(selection.count(), 2);
        QCOMPARE(modelChanges.count(), 2);
        QCOMPARE(previews.count(), 3); // final preview, then clear
        QVERIFY(fixture.controller.liveTransforms().isEmpty());
        for (auto* outline : outlines) QVERIFY(!outline->rasterUpdatesDeferred());
        fixture.controller.finishSelectionScaleGesture();
        QCOMPARE(documentChanges.count(), 2);
        disconnect(geometryConnection);
    }

    void altScrollCommitsOnNativeEndOrModifierRelease_data()
    {
        QTest::addColumn<bool>("releaseAlt");
        QTest::newRow("end-without-alt") << false;
        QTest::newRow("alt-key-release") << true;
    }

    void altScrollCommitsOnNativeEndOrModifierRelease()
    {
        QFETCH(bool, releaseAlt);
        Fixture fixture;
        QVERIFY(fixture.initialize());
        auto* media = fixture.document.addText({250, 220}, "Scale me");
        fixture.document.select(media->mediaId(), false);
        fixture.view.show();
        QVERIFY(QTest::qWaitForWindowExposed(&fixture.view));
        fixture.view.rootObject()->forceActiveFocus();
        const QPointF cursor(950, 650);
        const auto camera = fixture.document.cameraCenter();
        const auto span = fixture.document.cameraSquareSceneSize();
        QWheelEvent begin(cursor, fixture.view.mapToGlobal(cursor.toPoint()), {0, 24}, {},
            Qt::NoButton, Qt::AltModifier, Qt::ScrollBegin, false);
        QCoreApplication::sendEvent(&fixture.view, &begin);
        QCOMPARE(media->scale(), 1.0);
        if (releaseAlt) {
            QTest::keyRelease(&fixture.view, Qt::Key_Alt);
        } else {
            QWheelEvent end(cursor, fixture.view.mapToGlobal(cursor.toPoint()), {}, {},
                Qt::NoButton, Qt::NoModifier, Qt::ScrollEnd, false);
            QCoreApplication::sendEvent(&fixture.view, &end);
        }
        QVERIFY(qAbs(media->scale() - std::exp(24 * 0.003)) < 1e-10);
        QVERIFY(fixture.controller.liveTransforms().isEmpty());
        QCOMPARE(fixture.document.cameraCenter(), camera);
        QCOMPARE(fixture.document.cameraSquareSceneSize(), span);
    }

    void selectionScaleGestureLifecycleAndMouseWheelFallback()
    {
        CanvasDocument document;
        QuickCanvasController controller(&document);
        QVERIFY(controller.initialize());
        controller.setProjectEditingEnabled(true);
        auto* first = document.addText({10, 20}, "First");
        auto* second = document.addText({400, 300}, "Second");
        document.select(first->mediaId(), false);
        // A finite delta can still overflow geometry. It must not capture a
        // dormant transaction that later uses outdated starting values.
        controller.updateSelectionScaleGesture(std::numeric_limits<qreal>::max(), true);
        first->setScale(1.5);
        controller.updateSelectionScaleGesture(2.0, true);
        controller.finishSelectionScaleGesture();
        QCOMPARE(first->scale(), 3.0);
        first->setScale(1.0);
        const QRectF original = first->sceneRect();
        controller.updateSelectionScaleGesture(2.0, false);
        QCOMPARE(first->sceneRect(), original);
        QTRY_COMPARE(first->scale(), 2.0);
        QVERIFY(controller.liveTransforms().isEmpty());
        QVERIFY(QLineF(first->sceneRect().center(), original.center()).length() < 1e-8);

        controller.updateSelectionScaleGesture(1.5, true);
        controller.handleMediaSelectRequested(second->mediaId(), false);
        QCOMPARE(first->scale(), 3.0);
        QCOMPARE(second->scale(), 1.0);
        QVERIFY(controller.liveTransforms().isEmpty());

        controller.updateSelectionScaleGesture(2.0, true);
        QTRY_VERIFY(!controller.liveTransforms().isEmpty());
        document.setEditsLocked(true);
        QVERIFY(controller.liveTransforms().isEmpty());
        controller.finishSelectionScaleGesture();
        QCOMPARE(second->scale(), 1.0);
        document.setEditsLocked(false);
        controller.updateSelectionScaleGesture(2.0, true);
        controller.setProjectEditingEnabled(false);
        controller.finishSelectionScaleGesture();
        QCOMPARE(second->scale(), 1.0);
        controller.setProjectEditingEnabled(true);

        controller.updateSelectionScaleGesture(2.0, true);
        document.removeMedia(second->mediaId());
        controller.finishSelectionScaleGesture();
        QVERIFY(controller.liveTransforms().isEmpty());
        QCOMPARE(first->scale(), 3.0);
    }

    void centeredSelectionScalingPreservesGroupGeometryAndRejectsInvalidInput()
    {
        Fixture fixture;
        QVERIFY(fixture.initialize());
        auto* first = fixture.document.addText({10, 20}, "First");
        auto* second = fixture.document.addText({400, 300}, "Second");
        first->setBaseSize({200, 100});
        second->setBaseSize({150, 80});
        second->setScale(2.0);
        fixture.document.select(first->mediaId(), false);
        fixture.document.select(second->mediaId(), true);
        const QRectF firstRect = first->sceneRect(), secondRect = second->sceneRect();
        fixture.controller.scaleSelectionBy(1.25);
        QCOMPARE(first->scale(), 1.25);
        QCOMPARE(second->scale(), 2.5);
        QCOMPARE(first->sceneRect().center(), firstRect.center());
        QCOMPARE(second->sceneRect().center(), secondRect.center());
        QVERIFY(first->fitToTextEnabled() && second->fitToTextEnabled());
        for (qreal invalid : {0.0, -1.0, std::numeric_limits<qreal>::infinity(),
                              std::numeric_limits<qreal>::quiet_NaN()})
            fixture.controller.scaleSelectionBy(invalid);
        QCOMPARE(first->scale(), 1.25);
        fixture.controller.setProjectEditingEnabled(false);
        fixture.controller.scaleSelectionBy(2.0);
        QCOMPARE(first->scale(), 1.25);
        fixture.controller.setProjectEditingEnabled(true);
        fixture.controller.scaleSelectionBy(1e-9);
        QVERIFY(first->sceneRect().height() >= 1.0);
        QVERIFY(second->sceneRect().height() >= 1.0);
        const qreal minimum = first->scale();
        fixture.controller.scaleSelectionBy(0.5);
        QCOMPARE(first->scale(), minimum);
        QVERIFY(QLineF(first->sceneRect().center(), firstRect.center()).length() < 1e-8);
    }

    void trackpadControlScrollZoomsAtCursor_data()
    {
        QTest::addColumn<int>("direction");
        QTest::addColumn<bool>("naturalScrolling");
        QTest::addColumn<bool>("pixelDeltas");
        for (int direction : {-1, 1})
            for (bool natural : {false, true})
                for (bool pixels : {false, true})
                    QTest::newRow(qPrintable(QString("%1-%2-%3")
                        .arg(direction > 0 ? "up" : "down")
                        .arg(natural ? "natural" : "standard")
                        .arg(pixels ? "pixels" : "angles"))) << direction << natural << pixels;
    }

    void trackpadControlScrollZoomsAtCursor()
    {
        QFETCH(int, direction);
        QFETCH(bool, naturalScrolling);
        QFETCH(bool, pixelDeltas);
        QPointingDevice trackpad("test trackpad", 0xCAFE,
            QInputDevice::DeviceType::TouchPad, QPointingDevice::PointerType::Finger,
            QInputDevice::Capability::Position | QInputDevice::Capability::PixelScroll, 2, 0);
        Fixture fixture;
        QVERIFY(fixture.initialize());
        fixture.view.show();
        QVERIFY(QTest::qWaitForWindowExposed(&fixture.view));
#ifdef Q_OS_MACOS
        MacWindowManager::activateApplicationWindow(&fixture.view);
        constexpr Qt::KeyboardModifier control = Qt::MetaModifier;
#else
        fixture.view.requestActivate();
        constexpr Qt::KeyboardModifier control = Qt::ControlModifier;
#endif
        QVERIFY(QTest::qWaitForWindowActive(&fixture.view));
        fixture.document.setCameraView({120, -50}, 1000);
        const QPointF cursor(fixture.view.width() * .7, fixture.view.height() * .3);
        auto sceneUnderCursor = [&] {
            return (cursor - QPointF(fixture.controller.panX(), fixture.controller.panY()))
                / fixture.controller.viewScale();
        };
        const QPointF anchor = sceneUnderCursor();
        const qreal originalScale = fixture.controller.viewScale();
        auto scroll = [&](int fingerDirection, Qt::ScrollPhase phase) {
            const int sign = fingerDirection * (naturalScrolling ? -1 : 1);
            QWheelEvent event(cursor, fixture.view.mapToGlobal(cursor.toPoint()),
                pixelDeltas ? QPoint(0, sign * 24) : QPoint(), QPoint(0, sign * 120),
                Qt::NoButton, control, phase, naturalScrolling, Qt::MouseEventNotSynthesized, &trackpad);
            QCoreApplication::sendEvent(&fixture.view, &event);
        };
        scroll(direction, Qt::ScrollBegin);
        QVERIFY(direction > 0 ? fixture.controller.viewScale() > originalScale
                              : fixture.controller.viewScale() < originalScale);
        QVERIFY(QLineF(sceneUnderCursor(), anchor).length() < 1e-8);
        scroll(-direction, Qt::ScrollUpdate);
        QVERIFY(qAbs(fixture.controller.viewScale() / originalScale - 1.0) < 1e-9);
        QVERIFY(QLineF(sceneUnderCursor(), anchor).length() < 1e-8);
        scroll(0, Qt::ScrollEnd);
        QVERIFY(qAbs(fixture.controller.viewScale() / originalScale - 1.0) < 1e-9);

        // A horizontal high-resolution packet must not fall back to a stale
        // vertical angle delta and inadvertently zoom.
        QSignalSpy changes(&fixture.document, &CanvasDocument::cameraChanged);
        QWheelEvent horizontal(cursor, fixture.view.mapToGlobal(cursor.toPoint()), {30,0}, {120,120},
            Qt::NoButton, control, Qt::ScrollBegin, false, Qt::MouseEventNotSynthesized, &trackpad);
        QCoreApplication::sendEvent(&fixture.view, &horizontal);
        scroll(0, Qt::ScrollEnd);
        QCOMPARE(changes.count(), 0);
    }

    void pinchExcludesWheelAndOrdinaryScrollStillPans()
    {
        QPointingDevice trackpad("test trackpad", 0xCAFE,
            QInputDevice::DeviceType::TouchPad, QPointingDevice::PointerType::Finger,
            QInputDevice::Capability::Position | QInputDevice::Capability::PixelScroll, 2, 0);
        QPointingDevice mouse("test mouse", 0xCAFF,
            QInputDevice::DeviceType::Mouse, QPointingDevice::PointerType::Generic,
            QInputDevice::Capability::Position | QInputDevice::Capability::Scroll, 1, 3);
        Fixture fixture;
        QVERIFY(fixture.initialize());
        fixture.view.show();
        QVERIFY(QTest::qWaitForWindowExposed(&fixture.view));
#ifdef Q_OS_MACOS
        MacWindowManager::activateApplicationWindow(&fixture.view);
        constexpr Qt::KeyboardModifier control = Qt::MetaModifier;
#else
        fixture.view.requestActivate();
        constexpr Qt::KeyboardModifier control = Qt::ControlModifier;
#endif
        QVERIFY(QTest::qWaitForWindowActive(&fixture.view));
        fixture.document.setCameraView({120,-50}, 1000);
        const QPointF cursor(fixture.view.width() * .7, fixture.view.height() * .3);
        auto native = [&](Qt::NativeGestureType type, qreal value = 0.0) {
            QNativeGestureEvent event(type, &trackpad, 2, cursor, cursor,
                fixture.view.mapToGlobal(cursor.toPoint()), value, {});
            QCoreApplication::sendEvent(&fixture.view, &event);
        };
        auto scroll = [&](Qt::KeyboardModifiers modifiers, bool end = false) {
            QWheelEvent event(cursor, fixture.view.mapToGlobal(cursor.toPoint()),
                end ? QPoint() : QPoint(30, -20), {}, Qt::NoButton, modifiers,
                end ? Qt::ScrollEnd : Qt::ScrollBegin, false, Qt::MouseEventNotSynthesized, &trackpad);
            QCoreApplication::sendEvent(&fixture.view, &event);
        };
        auto wheel = [&] {
            QWheelEvent event(cursor, fixture.view.mapToGlobal(cursor.toPoint()), {}, {0,120},
                Qt::NoButton, Qt::NoModifier, Qt::NoScrollPhase, false, Qt::MouseEventNotSynthesized, &mouse);
            QCoreApplication::sendEvent(&fixture.view, &event);
        };
        native(Qt::BeginNativeGesture);
        native(Qt::ZoomNativeGesture, .01);
        const QPointF center = fixture.document.cameraCenter();
        const qreal span = fixture.document.cameraSquareSceneSize();
        scroll(Qt::NoModifier);
        scroll(Qt::NoModifier, true);
        scroll(control);
        scroll(control, true);
        wheel();
        QCOMPARE(fixture.document.cameraCenter(), center);
        QCOMPARE(fixture.document.cameraSquareSceneSize(), span);
        native(Qt::EndNativeGesture);

        const qreal scale = fixture.controller.viewScale();
        scroll(Qt::NoModifier);
        scroll(Qt::NoModifier, true);
        QCOMPARE(fixture.document.cameraSquareSceneSize(), span);
        QVERIFY(QLineF(fixture.document.cameraCenter(),
                      center - QPointF(30,-20) / scale).length() < 1e-8);
#ifdef Q_OS_MACOS
        // The physical Command key must not substitute for Control.
        const QPointF beforeCommand = fixture.document.cameraCenter();
        scroll(Qt::ControlModifier);
        scroll(Qt::ControlModifier, true);
        QCOMPARE(fixture.document.cameraSquareSceneSize(), span);
        QVERIFY(QLineF(fixture.document.cameraCenter(),
                      beforeCommand - QPointF(30,-20) / scale).length() < 1e-8);
#endif
        wheel();
        QVERIFY(fixture.controller.viewScale() > scale);
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
        QTRY_VERIFY_WITH_TIMEOUT(original->residencyReady(), 30000);
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
        QTRY_VERIFY_WITH_TIMEOUT(first->residencyReady(), 10000);
        const QString fileId = first->fileId();
        QVERIFY(!fileId.isEmpty());
        document.select(first->mediaId(), true);
        controller.copySelectedMedia();
        controller.pasteMedia();
        QCOMPARE(document.media().size(), 4);
        QCOMPARE(document.selectedMediaIds().size(), 2);
        QTRY_COMPARE(files.getMediaIdsForFile(fileId).size(), 2);
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
#ifdef Q_OS_MACOS
        QTest::keyClick(&fixture.view, Qt::Key_Backspace);
        QTest::keyClick(&fixture.view, Qt::Key_Delete);
        QTest::keyClick(&fixture.view, Qt::Key_Backspace, Qt::MetaModifier);
        QCOMPARE(fixture.document.media().size(), 2);
        QCOMPARE(deleted.size(), 0);
        QTest::keyClick(&fixture.view, Qt::Key_Backspace, Qt::ControlModifier);
#else
        QTest::keyClick(&fixture.view, Qt::Key_Backspace);
#endif
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
        QVERIFY(QMetaObject::invokeMethod(root, "requestTextEditing",
            Q_ARG(QVariant, text->mediaId()), Q_ARG(QVariant, true),
            Q_ARG(QVariant, false), Q_ARG(QVariant, QVariant()), Q_ARG(QVariant, QVariant())));
        QTRY_VERIFY(root->property("anyMediaEditing").toBool());
        QVERIFY(!root->property("mediaShortcutsEnabled").toBool());
        QTest::keyClick(&fixture.view, Qt::Key_C, Qt::ControlModifier);
        QCOMPARE(QGuiApplication::clipboard()->text(), QStringLiteral("Words"));
        QTest::keyClick(&fixture.view, Qt::Key_Backspace);
        QCOMPARE(fixture.document.media().size(), 1);
        QTest::keyClick(&fixture.view, Qt::Key_V, Qt::ControlModifier);
        QTRY_COMPARE(text->text(), QStringLiteral("Words"));
#ifdef Q_OS_MACOS
        QTest::keyClick(&fixture.view, Qt::Key_Backspace, Qt::ControlModifier);
        QCOMPARE(fixture.document.media().size(), 1);
#endif
        QTest::mouseClick(&fixture.view, Qt::LeftButton, Qt::NoModifier, {800,500});
        QTRY_VERIFY(!root->property("anyMediaEditing").toBool());
        fixture.document.select(text->mediaId());
        root->forceActiveFocus();
#ifdef Q_OS_MACOS
        QTest::keyClick(&fixture.view, Qt::Key_Backspace, Qt::ControlModifier);
#else
        QTest::keyClick(&fixture.view, Qt::Key_Delete);
#endif
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
#ifdef Q_OS_MACOS
        QTest::keyClick(&fixture.view, Qt::Key_Backspace, Qt::ControlModifier);
#endif
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
#ifdef Q_OS_MACOS
        QTest::keyClick(&fixture.view, Qt::Key_Backspace, Qt::ControlModifier);
#endif
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
        const QPointF originalViewPosition = item->position();
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
        QCOMPARE(item->position(), originalViewPosition);
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
        QTRY_COMPARE(fixture.document.media().size(), before + 1);
        CanvasMedia* imported = fixture.document.selectedMedia();
        QVERIFY(imported && !imported->isText());
        QCOMPARE(imported->baseSize(), QSize(80, 60));
        QCOMPARE(imported->sceneRect().center(), QPointF(520, 320));
    }

    void imageDropDoesNotCreateMediaUntilDropAndRemainsMovable()
    {
        Fixture fixture;
        QVERIFY(fixture.initialize());
        fixture.view.show();
        QVERIFY(QTest::qWaitForWindowExposed(&fixture.view));

        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString imagePath = directory.filePath(QStringLiteral("resident.png"));
        QImage image(160, 90, QImage::Format_ARGB32_Premultiplied);
        image.fill(QColor("#27a8e0"));
        QVERIFY(image.save(imagePath));

        QVERIFY(fixture.controller.beginLocalFileDrag(
            {QUrl::fromLocalFile(imagePath)}, 500, 300));
        QVERIFY(fixture.document.media().isEmpty());
        QVERIFY(fixture.controller.commitLocalFileDrop(500, 300));
        QTRY_COMPARE(fixture.document.media().size(), 1);
        CanvasMedia* imported = fixture.document.selectedMedia();
        QVERIFY(imported && !imported->isVideo() && !imported->isText());
        QTRY_VERIFY_WITH_TIMEOUT(imported->residencyReady(), 5000);

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
        QVERIFY(imported->residencyReady());
    }

    void droppedMediaReportsFirstSkeletonTiming_data()
    {
        QTest::addColumn<QString>("mediaType");
        QTest::addColumn<bool>("backendPrepared");
        QTest::newRow("large-png") << QStringLiteral("large-png") << false;
        QTest::newRow("webp") << QStringLiteral("webp") << false;
        QTest::newRow("video") << QStringLiteral("video") << false;
        QTest::newRow("video-prepared") << QStringLiteral("video") << true;
    }

    void droppedMediaReportsFirstSkeletonTiming()
    {
        QFETCH(QString, mediaType);
        QFETCH(bool, backendPrepared);
        if (backendPrepared) {
            const auto preparation = MediaBackendBootstrap::initialize();
            QTRY_VERIFY_WITH_TIMEOUT(preparation.isFinished(), 15000);
            QVERIFY2(preparation.result().ready, qPrintable(preparation.result().error));
        }
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        QString path;
        if (mediaType == QLatin1String("large-png")) {
            path = directory.filePath(QStringLiteral("large-drop.png"));
            // Generate before timing, and release the source pixels before
            // importing. At DPR 2 this also exercises a large painted surface.
            QImage source(7680, 4320, QImage::Format_RGB32);
            source.fill(Qt::cyan);
            QVERIFY(source.save(path));
        } else if (mediaType == QLatin1String("video")) {
            path = qEnvironmentVariable("MOUFFETTE_TEST_VIDEO_FILE");
            if (path.isEmpty()) path = QString::fromUtf8(TEST_VIDEO_FILE);
        } else {
            path = QString::fromUtf8(TEST_WEBP_FILE);
        }
        QVERIFY2(QFile::exists(path), qPrintable(path));
        auto& memory = MediaResidencyManager::instance();
        memory.setMemorySnapshotForTesting({8ULL << 30, 0, 512ULL << 20, false, 0});
        Fixture fixture;
        QVERIFY(fixture.initialize());
        fixture.view.show();
        QVERIFY(QTest::qWaitForWindowExposed(&fixture.view));
#ifdef Q_OS_MACOS
        MacWindowManager::activateApplicationWindow(&fixture.view);
#else
        fixture.view.requestActivate();
#endif
        QVERIFY(QTest::qWaitForWindowActive(&fixture.view));
        const QPointF dropPoint(500, 350);
        const QImage emptyFrame = fixture.view.grabWindow();
        QVERIFY(!emptyFrame.isNull());
        QElapsedTimer elapsed;
        qint64 mediaAddedMs = -1;
        qint64 skeletonFrameMs = -1;
        qint64 presentedFrameMs = -1;
        qint64 lastHeartbeatMs = 0;
        qint64 maxHeartbeatGapMs = 0;
        int heartbeatCount = 0;
        bool capturePending = false;
        QImage skeletonFrame;
        QObject observer;
        QTimer heartbeat;
        heartbeat.setTimerType(Qt::PreciseTimer);
        heartbeat.setInterval(5);
        connect(&heartbeat, &QTimer::timeout, &observer, [&] {
            const qint64 now = elapsed.elapsed();
            maxHeartbeatGapMs = qMax(maxHeartbeatGapMs, now - lastHeartbeatMs);
            lastHeartbeatMs = now;
            ++heartbeatCount;
        });
        connect(&fixture.document, &CanvasDocument::mediaAdded, &observer,
                [&](CanvasMedia* media) {
            mediaAddedMs = elapsed.elapsed();
            // Import publishes only the lightweight model/facade. Platform
            // sinks and audio must never initialize synchronously on adoption.
            if (media->isVideo()) {
                QVERIFY(media->player());
                QVERIFY(!media->videoSink());
                QVERIFY(!media->audioOutput());
            }
        });
        connect(&fixture.view, &QQuickWindow::frameSwapped, &observer, [&] {
            if (skeletonFrameMs >= 0 || capturePending || mediaAddedMs < 0) return;
            auto* skeleton = findQuickItemWithProperty(
                fixture.view.rootObject(), "objectName", "mediaLoadingSkeleton");
            if (!skeleton || !skeleton->isVisible()) return;
            capturePending = true;
            presentedFrameMs = elapsed.elapsed();
            // Read back pixels outside the frame callback. The measured first
            // frame and heartbeat gap include this final readback, consistently
            // across cases; model insertion alone does not count as a display.
            QTimer::singleShot(0, &observer, [&] {
                skeletonFrame = fixture.view.grabWindow();
                skeletonFrameMs = elapsed.elapsed();
                maxHeartbeatGapMs = qMax(maxHeartbeatGapMs, skeletonFrameMs - lastHeartbeatMs);
                heartbeat.stop();
            });
        });
        elapsed.start();
        heartbeat.start();
        QVERIFY(fixture.controller.beginLocalFileDrag(
            {QUrl::fromLocalFile(path)}, dropPoint.x(), dropPoint.y()));
        const qint64 commitStartedNs = elapsed.nsecsElapsed();
        QVERIFY(fixture.controller.commitLocalFileDrop(dropPoint.x(), dropPoint.y()));
        const qreal commitMs = (elapsed.nsecsElapsed() - commitStartedNs) / 1000000.0;
        QTRY_VERIFY_WITH_TIMEOUT(skeletonFrameMs >= 0, 10000);
        qInfo() << "Drop timing ms:" << "backendPrepared" << backendPrepared << "commit" << commitMs
                << "mediaAdded" << mediaAddedMs << "presentedFrame" << presentedFrameMs
                << "skeletonFrame" << skeletonFrameMs
                << "maxHeartbeatGap" << maxHeartbeatGapMs << "heartbeats" << heartbeatCount
                << "DPR" << fixture.view.devicePixelRatio();
        QVERIFY(!skeletonFrame.isNull());
        const auto colorAtDrop = [&](const QImage& frame) {
            return frame.pixelColor(qRound(dropPoint.x() * frame.width() / fixture.view.width()),
                                    qRound(dropPoint.y() * frame.height() / fixture.view.height()));
        };
        const QColor before = colorAtDrop(emptyFrame);
        const QColor shown = colorAtDrop(skeletonFrame);
        // Loading chrome is lighter in dark mode and darker in light mode.
        // Verify a visible rendered change without assuming a dark canvas.
        const int colorDifference = qMax(qAbs(shown.red() - before.red()),
            qMax(qAbs(shown.green() - before.green()), qAbs(shown.blue() - before.blue())));
        QVERIFY2(colorDifference > 10,
                 "The rendered frame must contain the loading skeleton at the drop location");
        QPointer<CanvasMedia> media = fixture.document.selectedMedia();
        QVERIFY(media && !media->residencyReady());
        // RAM admission is still refused: an empty shell must not allocate a
        // full-size painted image/video surface behind its loading skeleton.
        QVERIFY(fixture.view.rootObject()->findChildren<RemoteVideoFrameItem*>().isEmpty());
        QCOMPARE(media->sceneRect().center(), dropPoint);
        const QString owner = media->residencyOwnerId();
        const QString artifactDir = qEnvironmentVariable("MOUFFETTE_OVERLAY_ARTIFACT_DIR");
        if (!artifactDir.isEmpty()) {
            QVERIFY(QDir().mkpath(artifactDir));
            QVERIFY(skeletonFrame.save(QDir(artifactDir).filePath(
                QStringLiteral("drop-skeleton-%1.png").arg(mediaType))));
        }
        // Removing a loading shell cancels its lease before RAM is restored.
        fixture.controller.deleteSelectedMedia();
        memory.setMemorySnapshotForTesting({8ULL << 30, 6ULL << 30, 512ULL << 20, false, 0});
        memory.sampleNow();
        memory.sampleNow();
        QTRY_VERIFY(media.isNull());
        QTRY_VERIFY_WITH_TIMEOUT(!memory.hasBackgroundWorkForPath(path), 10000);
        QVERIFY(fixture.document.media().isEmpty());
        QVERIFY(!fixture.document.hasPendingImports());
        QVERIFY(!memory.asset(owner));
    }

    void coldMediaKeepsEditableShell_data()
    {
        QTest::addColumn<bool>("video");
        QTest::addColumn<bool>("alt");
        QTest::newRow("image-resize") << false << false;
        QTest::newRow("image-alt-resize") << false << true;
        QTest::newRow("video-resize") << true << false;
        QTest::newRow("video-alt-resize") << true << true;
    }

    void coldMediaKeepsEditableShell()
    {
        QFETCH(bool, video);
        QFETCH(bool, alt);
        auto& memory = MediaResidencyManager::instance();
        struct ResetMemory { ~ResetMemory() { MediaResidencyManager::instance().clearMemorySnapshotForTesting(); } } reset;
        memory.setMemorySnapshotForTesting({8ULL << 30, 0, 512ULL << 20, false, 0});
        Fixture fixture;
        QVERIFY(fixture.initialize());
        fixture.view.show();
        QVERIFY(QTest::qWaitForWindowExposed(&fixture.view));
#ifdef Q_OS_MACOS
        MacWindowManager::activateApplicationWindow(&fixture.view);
#else
        fixture.view.requestActivate();
#endif
        QVERIFY(QTest::qWaitForWindowActive(&fixture.view));
        QTemporaryDir directory;
        const QString path = video ? QString::fromUtf8(TEST_VIDEO_FILE)
                                   : directory.filePath(QStringLiteral("cold.png"));
        QImage image(160, 90, QImage::Format_ARGB32_Premultiplied);
        image.fill(Qt::cyan);
        if (!video) QVERIFY(image.save(path));
        QVERIFY(QFile::exists(path));
        auto* media = fixture.document.addPreparedFile(path, image.size(), video, {100, 100});
        QVERIFY(media && !media->residencyReady());
        media->setScale(2.0);
        QTRY_VERIFY(!media->residencyState().isEmpty());
        auto* root = fixture.view.rootObject();
        const auto clickTopAction = [&](const QString& icon) {
            // Model changes can recreate the overlay; resolve each button
            // after the row has been positioned for the current selection.
            QTest::qWait(20);
            auto* overlay = findQuickItemWithProperty(root, "objectName", "mediaTopOverlay");
            auto* button = findQuickItemWithProperty(overlay, "iconSource", icon);
            if (!button || !button->isVisible() || !button->isEnabled()) return false;
            QTest::mouseClick(&fixture.view, Qt::LeftButton, Qt::NoModifier,
                             button->mapToScene({button->width() / 2, button->height() / 2}).toPoint());
            return true;
        };
        QVERIFY(clickTopAction(QStringLiteral("qrc:/icons/icons/visibility-on.svg")));
        QTRY_VERIFY(!media->contentVisible());
        QVERIFY(clickTopAction(QStringLiteral("qrc:/icons/icons/visibility-off.svg")));
        QTRY_VERIFY(media->contentVisible());
        const qreal originalZ = media->z();
        QVERIFY(clickTopAction(QStringLiteral("qrc:/icons/icons/arrow-up.svg")));
        QTRY_VERIFY(media->z() > originalZ);
        const qreal raisedZ = media->z();
        QVERIFY(clickTopAction(QStringLiteral("qrc:/icons/icons/arrow-down.svg")));
        QTRY_VERIFY(media->z() < raisedZ);
        QVERIFY(!media->residencyReady());
        QVERIFY(clickTopAction(QStringLiteral("qrc:/icons/icons/delete.svg")));
        QTRY_VERIFY(fixture.document.media().isEmpty());

        media = fixture.document.addPreparedFile(path, image.size(), video, {100, 100});
        QVERIFY(media && !media->residencyReady());
        media->setScale(2.0);
        MediaSettingsViewModel settings;
        settings.setController(&fixture.controller);
        QVERIFY(!settings.available());
        auto* top = findQuickItemWithProperty(fixture.view.rootObject(), "objectName", "mediaTopOverlay");
        QVERIFY(top);
        QVERIFY(top->property("actionsAvailable").toBool());
        auto* videoControls = findQuickItemWithProperty(root, "objectName", "videoMuteButton");
        QVERIFY(videoControls && !videoControls->isVisible() && !videoControls->isEnabled());
        auto* chrome = findQuickItemWithProperty(fixture.view.rootObject(), "objectName", "selectionChromeVisual");
        QVERIFY(chrome && chrome->isVisible());
        QCOMPARE(chrome->size(), QSizeF(320, 180));
        auto* skeleton = findQuickItemWithProperty(fixture.view.rootObject(), "objectName", "mediaLoadingSkeleton");
        QVERIFY(skeleton && skeleton->isVisible());
        QCOMPARE(skeleton->size(), QSizeF(160, 90));
        const QString artifactDir = qEnvironmentVariable("MOUFFETTE_OVERLAY_ARTIFACT_DIR");
        if (!artifactDir.isEmpty()) {
            QVERIFY(QDir().mkpath(artifactDir));
            QVERIFY(fixture.view.grabWindow().save(QDir(artifactDir).filePath(
                QStringLiteral("media-skeleton-%1.png").arg(QString::fromLatin1(QTest::currentDataTag())))));
        }
        QSignalSpy persistentChanges(&fixture.document, &CanvasDocument::documentChanged);
        memory.sampleNow();
        QCOMPARE(persistentChanges.size(), 0);
        // Drive the actual handle with native events so both QML hit testing
        // and the backend transaction must accept the loading media.
        QPointer<QQuickItem> delegate = findQuickItemWithProperty(root, "currentMediaId", media->mediaId());
        QVERIFY(delegate);
        const QPoint start = chrome->mapToScene({chrome->width(), chrome->height()}).toPoint();
        const QPoint delta(160, alt ? 50 : 90);
        const auto modifiers = alt ? Qt::AltModifier : Qt::NoModifier;
        QSignalSpy resizeRequested(root, SIGNAL(mediaResizeRequested(QString,QString,double,double,bool,bool)));
        QTest::mousePress(&fixture.view, Qt::LeftButton, modifiers, start);
        for (int step = 1; step <= 4; ++step) {
            const QPoint point = start + delta * step / 4;
            QMouseEvent move(QEvent::MouseMove, QPointF(point), QPointF(point),
                QPointF(fixture.view.mapToGlobal(point)), Qt::NoButton, Qt::LeftButton, modifiers);
            QCoreApplication::sendEvent(&fixture.view, &move);
            QTest::qWait(10);
        }
        QVERIFY(!resizeRequested.isEmpty());
        QCOMPARE(resizeRequested.last().at(5).toBool(), alt);
        QTest::mouseRelease(&fixture.view, Qt::LeftButton, modifiers, start + delta);
        QCOMPARE(media->scale(), alt ? 2.0 : 3.0);
        QCOMPARE(media->baseSize(), alt ? QSize(240, 115) : QSize(160, 90));
        QCOMPARE(media->position(), QPointF(100, 100));
        QTRY_COMPARE(skeleton->size(), QSizeF(media->baseSize()));
        QVERIFY(skeleton->isVisible());
        QVERIFY(!settings.available());
        top = findQuickItemWithProperty(root, "objectName", "mediaTopOverlay");
        QVERIFY(top);
        QVERIFY(top->property("actionsAvailable").toBool());
        videoControls = findQuickItemWithProperty(root, "objectName", "videoMuteButton");
        QVERIFY(videoControls && !videoControls->isVisible() && !videoControls->isEnabled());
        fixture.controller.handleMediaMoveStarted(media->mediaId(), 100, 100, false);
        fixture.controller.handleMediaMoveUpdated(media->mediaId(), 130, 120, false);
        fixture.controller.handleMediaMoveEnded(media->mediaId(), 130, 120, false);
        QCOMPARE(media->position(), QPointF(130, 120));
        const QRectF editedRect = media->sceneRect();
        const QImage loadingFrame = fixture.view.grabWindow();
        QVERIFY(!loadingFrame.isNull());
        QImage fadingFrame;
        bool capturePending = false;
        QObject revealObserver;
        const QPointer<QQuickItem> surface = skeleton->parentItem();
        // Inspect rendered pixels, not only the animated QML property: a
        // texture that appears after its fade has elapsed must fail this check.
        connect(&fixture.view, &QQuickWindow::afterAnimating, &revealObserver, [&] {
            if (!surface || !fadingFrame.isNull() || capturePending) return;
            const qreal progress = surface->property("revealProgress").toReal();
            if (progress < 0.2 || progress > 0.7) return;
            capturePending = true;
            // grabWindow performs its own synchronization, outside the
            // afterAnimating callback of the current frame.
            QTimer::singleShot(0, &revealObserver, [&] {
                capturePending = false;
                if (!surface) return;
                const qreal current = surface->property("revealProgress").toReal();
                if (current >= 0.2 && current <= 0.7)
                    fadingFrame = fixture.view.grabWindow();
            });
        });
        // Loading only replaces the skeleton content; user geometry survives.
        memory.setMemorySnapshotForTesting({8ULL << 30, 6ULL << 30, 512ULL << 20, false, 0});
        memory.sampleNow();
        memory.sampleNow();
        QTRY_VERIFY_WITH_TIMEOUT(media->residencyReady(), 5000);
        QTRY_VERIFY(!skeleton->isVisible());
        videoControls = findQuickItemWithProperty(root, "objectName", "videoMuteButton");
        QVERIFY(videoControls);
        QCOMPARE(videoControls->isVisible(), video);
        QCOMPARE(videoControls->isEnabled(), video);
        QVERIFY2(!fadingFrame.isNull(), "Media must render a visible intermediate fade frame");
        const QImage readyFrame = fixture.view.grabWindow();
        QVERIFY(!readyFrame.isNull());
        const auto channelsAt = [&](const QImage& frame, const QPointF& point) {
            const QColor color = frame.pixelColor(
                qBound(0, qRound(point.x() * frame.width() / fixture.view.width()), frame.width() - 1),
                qBound(0, qRound(point.y() * frame.height() / fixture.view.height()), frame.height() - 1));
            return std::array<int, 3>{color.red(), color.green(), color.blue()};
        };
        int blendedPixels = 0;
        for (int y = 1; y <= 4; ++y) {
            for (int x = 1; x <= 4; ++x) {
                const QPointF point = delegate->mapToScene(
                    {delegate->width() * x / 5, delegate->height() * y / 5});
                const auto loading = channelsAt(loadingFrame, point);
                const auto fading = channelsAt(fadingFrame, point);
                const auto ready = channelsAt(readyFrame, point);
                int channel = 0;
                for (int c = 1; c < 3; ++c)
                    if (qAbs(ready[c] - loading[c]) > qAbs(ready[channel] - loading[channel])) channel = c;
                const int difference = ready[channel] - loading[channel];
                if (qAbs(difference) < 100) continue;
                const qreal fraction = qreal(fading[channel] - loading[channel]) / difference;
                if (fraction > 0.15 && fraction < 0.85) ++blendedPixels;
            }
        }
        QVERIFY2(blendedPixels >= 4, "Rendered media must be blended between its skeleton and fully visible content");
        if (!artifactDir.isEmpty()) {
            const QString tag = QString::fromLatin1(QTest::currentDataTag());
            QVERIFY(fadingFrame.save(QDir(artifactDir).filePath(QStringLiteral("media-fading-%1.png").arg(tag))));
            QVERIFY(readyFrame.save(QDir(artifactDir).filePath(QStringLiteral("media-ready-%1.png").arg(tag))));
        }
        QCOMPARE(media->sceneRect(), editedRect);
        QCOMPARE(findQuickItemWithProperty(root, "currentMediaId", media->mediaId()), delegate.data());
        fixture.controller.deleteSelectedMedia();
        QVERIFY(fixture.document.media().isEmpty());
    }

    void restoredDocumentsKeepIndependentResidencyLeases_data()
    {
        QTest::addColumn<bool>("closeOriginal");
        QTest::newRow("close-original") << true;
        QTest::newRow("close-restored") << false;
    }

    void restoredDocumentsKeepIndependentResidencyLeases()
    {
        QFETCH(bool, closeOriginal);
        QTemporaryDir directory;
        const QString path = directory.filePath(QStringLiteral("shared.png"));
        QImage image(160, 90, QImage::Format_ARGB32_Premultiplied);
        image.fill(Qt::cyan);
        QVERIFY(image.save(path));
        CanvasDocument original;
        auto* first = original.addPreparedFile(path, image.size(), false, {});
        QTRY_VERIFY_WITH_TIMEOUT(first->residencyReady(), 5000);
        CanvasDocument restored;
        QVERIFY(restored.restoreProjectState(original.serializeProjectState(), {{first->mediaId(), path}}));
        auto* second = restored.mediaById(first->mediaId());
        QVERIFY(second);
        QCOMPARE(first->mediaId(), second->mediaId());
        QVERIFY(first->residencyOwnerId() != second->residencyOwnerId());
        QTRY_VERIFY_WITH_TIMEOUT(second->residencyReady(), 5000);
        auto& memory = MediaResidencyManager::instance();
        QCOMPARE(memory.asset(first->residencyOwnerId()), memory.asset(second->residencyOwnerId()));
        const auto occurrences = [&memory](const QString& owner) {
            for (const QVariant& value : memory.assets()) {
                const auto entry = value.toMap();
                if (entry.value("owners").toStringList().contains(owner))
                    return entry.value("occurrences").toInt();
            }
            return 0;
        };
        QCOMPARE(occurrences(first->residencyOwnerId()), 2);
        auto* survivor = closeOriginal ? second : first;
        (closeOriginal ? original : restored).clear();
        QVERIFY(survivor->residencyReady());
        QCOMPARE(occurrences(survivor->residencyOwnerId()), 1);
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        QVERIFY(survivor->residencyReady());
    }

    void residentMediaSurvivesCanvasNavigation_data()
    {
        QTest::addColumn<bool>("video");
        QTest::newRow("image") << false;
        QTest::newRow("video") << true;
    }

    void residentMediaSurvivesCanvasNavigation()
    {
        QFETCH(bool, video);
        QString error;
        std::unique_ptr<QuickCanvasHost> host(QuickCanvasHost::create(&error));
        QVERIFY2(host, qPrintable(error));
        host->setProjectEditingEnabled(true);
        host->document()->setCamera(1.0, 0.0, 0.0);
        QTemporaryDir directory;
        const QString path = video ? QString::fromUtf8(TEST_VIDEO_FILE)
                                   : directory.filePath(QStringLiteral("navigation.png"));
        QImage image(160, 90, QImage::Format_ARGB32_Premultiplied);
        image.fill(Qt::cyan);
        if (!video) QVERIFY(image.save(path));
        auto* media = host->document()->addPreparedFile(path, image.size(), video, {600, 300});
        QVERIFY(media);
        QTRY_VERIFY_WITH_TIMEOUT(media->residencyReady(), 5000);
        if (video) {
            QTRY_VERIFY_WITH_TIMEOUT(media->firstFramePrimed(), 10000);
            media->setPositionMs(250);
            QTRY_VERIFY_WITH_TIMEOUT(media->player()->preparedAt(250), 5000);
        }
        auto& memory = MediaResidencyManager::instance();
        auto residentAsset = memory.asset(media->residencyOwnerId());
        auto* player = media->player();
        const qint64 position = media->positionMs();
        QSignalSpy residencyChanges(media, &CanvasMedia::residencyChanged);

        ClientWorkspaceViewModel session(QStringLiteral("navigation-session"), host.get(),
            [] {}, nullptr, [] { return false; }, [] { return true; }, [] { return true; });
        session.setLoading(false);
        QQmlEngine engine;
        QQuickWindow window;
        window.resize(1100, 800);
        QQmlComponent component(&engine, QUrl(QStringLiteral(
            "qrc:/qt/qml/Mouffette/App/resources/qml/app/pages/CanvasPage.qml")));
        QTRY_VERIFY_WITH_TIMEOUT(!component.isLoading(), 3000);
        const auto controller = [&](bool active) {
            return QVariantMap{{QStringLiteral("activeWorkspace"),
                QVariant::fromValue<QObject*>(active ? &session : nullptr)}};
        };
        std::unique_ptr<QObject> pageObject(component.createWithInitialProperties({
            {QStringLiteral("controller"), controller(false)}}));
        auto* page = qobject_cast<QQuickItem*>(pageObject.get());
        QVERIFY2(page, qPrintable(component.errorString()));
        page->setParentItem(window.contentItem());
        page->setSize(window.size());

        // Exercise the actual page Loader: navigation destroys only the view,
        // while the workspace, asset, player and paused cursor remain alive.
        for (int visit = 0; visit < 3; ++visit) {
            bool sawLoadingFrame = false;
            QObject observer;
            connect(&window, &QQuickWindow::afterAnimating, &observer, [&] {
                auto* loading = findQuickItemWithProperty(page, "objectName", "mediaLoadingSkeleton");
                if (loading) sawLoadingFrame |= loading->isVisible();
            });
            page->setProperty("controller", controller(true));
            auto* skeleton = findQuickItemWithProperty(page, "objectName", "mediaLoadingSkeleton");
            QVERIFY(skeleton);
            const QPointer<QQuickItem> surface = skeleton->parentItem();
            QVERIFY(surface);
            QTRY_VERIFY_WITH_TIMEOUT(surface->property("contentReady").toBool(), 1000);
            QCOMPARE(surface->property("revealProgress").toReal(), 1.0);
            QVERIFY(!skeleton->isVisible());
            window.show();
            QVERIFY(QTest::qWaitForWindowExposed(&window));
            const QImage frame = window.grabWindow();
            QVERIFY(!frame.isNull());
            QTest::qWait(250);
            QVERIFY2(!sawLoadingFrame, "Resident media must not replay its loading reveal on navigation");
            // Compare actual content pixels with the settled view as well as
            // opacity: a late texture must not pass by merely skipping a fade.
            const QImage settledFrame = window.grabWindow();
            auto* delegate = findQuickItemWithProperty(page, "currentMediaId", media->mediaId());
            QVERIFY(delegate);
            for (int y = 1; y <= 3; ++y) {
                for (int x = 1; x <= 3; ++x) {
                    const QPointF point = delegate->mapToScene(
                        {delegate->width() * x / 4, delegate->height() * y / 4});
                    const QPoint pixel(qRound(point.x() * frame.width() / window.width()),
                                       qRound(point.y() * frame.height() / window.height()));
                    QVERIFY(frame.rect().contains(pixel));
                    QCOMPARE(frame.pixelColor(pixel), settledFrame.pixelColor(pixel));
                    if (!video) QCOMPARE(frame.pixelColor(pixel), QColor(Qt::cyan));
                }
            }
            QCOMPARE(memory.asset(media->residencyOwnerId()), residentAsset);
            QCOMPARE(media->player(), player);
            QCOMPARE(media->positionMs(), position);
            QCOMPARE(residencyChanges.count(), 0);
            page->setProperty("controller", controller(false));
            QTRY_VERIFY(surface.isNull());
            QVERIFY(media->residencyReady());
        }

        // A real eviction on the same visual must still show loading and fade
        // after readmission. The test must not keep the evicted asset alive.
        residentAsset.reset();
        page->setProperty("controller", controller(true));
        auto* skeleton = findQuickItemWithProperty(page, "objectName", "mediaLoadingSkeleton");
        QVERIFY(skeleton);
        const QPointer<QQuickItem> surface = skeleton->parentItem();
        QTRY_COMPARE(surface->property("revealProgress").toReal(), 1.0);
        memory.setMemorySnapshotForTesting({8ULL << 30, 0, 512ULL << 20, false, 0});
        memory.sampleNow();
        QTRY_VERIFY(!media->residencyReady());
        QTRY_VERIFY(skeleton->isVisible());
        QCOMPARE(surface->property("revealProgress").toReal(), 0.0);
        QVERIFY(page->findChildren<RemoteVideoFrameItem*>().isEmpty());
        bool sawPartialOpacity = false;
        QObject observer;
        connect(&window, &QQuickWindow::afterAnimating, &observer, [&] {
            if (!surface) return;
            const qreal progress = surface->property("revealProgress").toReal();
            sawPartialOpacity |= progress > 0 && progress < 1;
        });
        memory.setMemorySnapshotForTesting({8ULL << 30, 6ULL << 30, 512ULL << 20, false, 0});
        memory.sampleNow();
        memory.sampleNow();
        QTRY_VERIFY_WITH_TIMEOUT(media->residencyReady(), 5000);
        QTRY_COMPARE_WITH_TIMEOUT(surface->property("revealProgress").toReal(), 1.0, 5000);
        QVERIFY2(sawPartialOpacity, "A real residency reload must still animate its loading reveal");
        QVERIFY(!skeleton->isVisible());
        QCOMPARE(media->player(), player);
        QCOMPARE(media->positionMs(), position);
    }

    void mediaSurfaceDoesNotAnimateOutputRebinding_data()
    {
        QTest::addColumn<bool>("local");
        QTest::newRow("canvas") << true;
        QTest::newRow("remote") << false;
    }

    void mediaSurfaceDoesNotAnimateOutputRebinding()
    {
        QFETCH(bool, local);
        QQmlEngine engine;
        QQuickWindow window;
        window.resize(320, 180);
        QQmlComponent component(&engine, QUrl(QStringLiteral(
            "qrc:/qt/qml/Mouffette/App/resources/qml/MediaSurface.qml")));
        QTRY_VERIFY_WITH_TIMEOUT(!component.isLoading(), 3000);
        std::unique_ptr<QObject> object(component.createWithInitialProperties({
            {QStringLiteral("requireInitialSkeleton"), local}}));
        auto* surface = qobject_cast<QQuickItem*>(object.get());
        QVERIFY2(surface, qPrintable(component.errorString()));
        surface->setParentItem(window.contentItem());
        surface->setSize(window.size());
        window.show();
        QVERIFY(QTest::qWaitForWindowExposed(&window));
        surface->setProperty("residencyReady", true);
        surface->setProperty("contentReady", true);
        if (!local) QCOMPARE(surface->property("revealProgress").toReal(), 1.0);
        QTRY_COMPARE(surface->property("revealProgress").toReal(), 1.0);

        // Replacing a GPU surface/sink can temporarily clear contentReady.
        // Residency did not change, so the completed fade must not replay.
        surface->setProperty("contentReady", false);
        QCOMPARE(surface->property("revealProgress").toReal(), 0.0);
        surface->setProperty("contentReady", true);
        QCOMPARE(surface->property("revealProgress").toReal(), 1.0);
    }

    void cachedMediaWaitsForInitialSkeletonBeforeShowingControls()
    {
        QTemporaryDir directory;
        const QString path = directory.filePath(QStringLiteral("cached.png"));
        QImage image(160, 90, QImage::Format_ARGB32_Premultiplied);
        image.fill(Qt::cyan);
        QVERIFY(image.save(path));
        CanvasDocument warmDocument;
        auto* warm = warmDocument.addPreparedFile(path, image.size(), false, {});
        QTRY_VERIFY_WITH_TIMEOUT(warm->residencyReady(), 5000);

        Fixture fixture;
        QVERIFY(fixture.initialize());
        auto* media = fixture.document.addPreparedFile(path, image.size(), false, {100, 100});
        QTRY_VERIFY_WITH_TIMEOUT(media->residencyReady(), 5000);
        auto* top = findQuickItemWithProperty(fixture.view.rootObject(), "objectName", "mediaTopOverlay");
        auto* chrome = findQuickItemWithProperty(fixture.view.rootObject(), "objectName", "selectionChromeVisual");
        auto* skeleton = findQuickItemWithProperty(fixture.view.rootObject(), "objectName", "mediaLoadingSkeleton");
        QVERIFY(top && chrome && skeleton);
        QVERIFY(top->property("actionsAvailable").toBool());
        QVERIFY(chrome->isVisible());
        QVERIFY(skeleton->isVisible());

        bool sawPartialOpacity = false;
        QObject revealObserver;
        const QPointer<QQuickItem> surface = skeleton->parentItem();
        connect(&fixture.view, &QQuickWindow::afterAnimating, &revealObserver, [&] {
            if (!surface) return;
            const qreal progress = surface->property("revealProgress").toReal();
            sawPartialOpacity |= progress > 0 && progress < 1;
        });
        fixture.view.show();
        QVERIFY(QTest::qWaitForWindowExposed(&fixture.view));
        QTRY_VERIFY(top->property("actionsAvailable").toBool());
        QTRY_VERIFY(chrome->isVisible());
        QTRY_VERIFY(!skeleton->isVisible());
        QVERIFY2(sawPartialOpacity, "Cached media must fade in after the initial skeleton frame");
    }

    void testSceneRequiresEveryCanvasMediaResident()
    {
        auto& memory = MediaResidencyManager::instance();
        struct ResetMemory { ~ResetMemory() { MediaResidencyManager::instance().clearMemorySnapshotForTesting(); } } reset;
        memory.setMemorySnapshotForTesting({8ULL << 30, 0, 512ULL << 20, false, 0});
        QString error;
        std::unique_ptr<QuickCanvasHost> host(QuickCanvasHost::create(&error));
        QVERIFY2(host, qPrintable(error));
        host->setProjectEditingEnabled(true);
        host->document()->addText({100, 100}, "Ready text");
        QVERIFY(host->testSceneActionEnabled());
        QTemporaryDir directory;
        const QString path = directory.filePath("cold.png");
        QImage image(80, 60, QImage::Format_RGB32);
        image.fill(Qt::cyan);
        QVERIFY(image.save(path));
        auto* cold = host->document()->addPreparedFile(path, image.size(), false, {100, 100});
        QVERIFY(cold && !cold->residencyReady());
        QVERIFY(!host->testSceneActionEnabled());
        host->triggerTestSceneAction();
        QVERIFY(!host->testSceneLaunched());
        QVERIFY(host->document()->removeMedia(cold->mediaId()));
        QVERIFY(host->testSceneActionEnabled());
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

    void pendingTextEditingRequestRespectsLifetime_data()
    {
        QTest::addColumn<QString>("interruption");
        for (const QString name : {"deselect", "delete", "lock", "lock-unlock",
                                   "workspace-roundtrip", "newer-editor"})
            QTest::newRow(qPrintable(name)) << name;
    }

    void pendingTextEditingRequestRespectsLifetime()
    {
        QFETCH(QString, interruption);
        Fixture fixture;
        QVERIFY(fixture.initialize());
        fixture.view.show();
        QVERIFY(QTest::qWaitForWindowExposed(&fixture.view));
#ifdef Q_OS_MACOS
        MacWindowManager::activateApplicationWindow(&fixture.view);
#else
        fixture.view.requestActivate();
#endif
        QVERIFY(QTest::qWaitForWindowActive(&fixture.view));
        auto* root = fixture.view.rootObject();
        auto* a = fixture.document.addText({300, 200}, "First");
        auto* b = fixture.document.addText({500, 200}, "Second");
        const QString aId = a->mediaId();
        const auto request = [&](const QString& id) {
            return QMetaObject::invokeMethod(root, "requestTextEditing",
                Q_ARG(QVariant, id), Q_ARG(QVariant, false), Q_ARG(QVariant, false),
                Q_ARG(QVariant, 300.0), Q_ARG(QVariant, 200.0));
        };
        QVERIFY(request(aId));
        QVERIFY(!root->property("anyMediaEditing").toBool());
        if (interruption == "deselect") fixture.document.clearSelection();
        else if (interruption == "delete") QVERIFY(fixture.document.removeMedia(aId));
        else if (interruption == "lock" || interruption == "lock-unlock") {
            fixture.document.setEditsLocked(true);
            if (interruption == "lock-unlock") fixture.document.setEditsLocked(false);
        } else if (interruption == "workspace-roundtrip") {
            root->setProperty("sessionViewModel", QVariant());
            root->setProperty("sessionViewModel", QVariantMap{
                {"canvasController", QVariant::fromValue<QObject*>(&fixture.controller)}});
        } else if (interruption == "newer-editor") {
            QVERIFY(request(b->mediaId()));
        }
        QCoreApplication::processEvents();
        QTest::qWait(20);
        if (interruption == "newer-editor") {
            QTRY_VERIFY(root->property("anyMediaEditing").toBool());
            auto* editor = root->property("currentEditingMediaItem").value<QQuickItem*>();
            QVERIFY(editor);
            QCOMPARE(editor->property("mediaId").toString(), b->mediaId());
        } else {
            QVERIFY(!root->property("anyMediaEditing").toBool());
        }
    }

    void fullPageTextCreationAndDoubleClick_data()
    {
        QTest::addColumn<bool>("preselected");
        QTest::addColumn<bool>("resizeViewport");
        QTest::addColumn<qreal>("cameraScale");
        QTest::addColumn<qreal>("bodyY");
        QTest::addColumn<qreal>("enlargement");
        QTest::newRow("unselected") << false << false << qreal(0.75) << qreal(-1) << qreal(1);
        QTest::newRow("selected") << true << false << qreal(0.75) << qreal(-1) << qreal(1);
        QTest::newRow("unselected-zoomed-resized") << false << true << qreal(1.6) << qreal(-1) << qreal(1);
        QTest::newRow("selected-zoomed-resized") << true << true << qreal(1.6) << qreal(-1) << qreal(1);
        QTest::newRow("enlarged-top") << false << false << qreal(0.75) << qreal(0.15) << qreal(4);
        QTest::newRow("enlarged-middle") << true << false << qreal(0.75) << qreal(0.5) << qreal(4);
        QTest::newRow("enlarged-bottom-zoomed-resized") << true << true << qreal(1.6) << qreal(0.85) << qreal(4);
        QTest::newRow("scene-beyond-viewport-bounds") << false << false << qreal(0.25) << qreal(-1) << qreal(1);
        QTest::newRow("scene-beyond-viewport-bounds-enlarged") << true << true << qreal(0.25) << qreal(0.85) << qreal(4);
    }

    void fullPageTextCreationAndDoubleClick()
    {
        QFETCH(bool, preselected);
        QFETCH(bool, resizeViewport);
        QFETCH(qreal, cameraScale);
        QFETCH(qreal, bodyY);
        QFETCH(qreal, enlargement);
        QString error;
        std::unique_ptr<QuickCanvasHost> host(QuickCanvasHost::create(&error));
        QVERIFY2(host, qPrintable(error));
        host->setProjectEditingEnabled(true);
        host->document()->setScreens({ScreenInfo(0, 2880, 1800, 0, 0, true)});
        ClientWorkspaceViewModel session(QStringLiteral("text-session"), host.get(),
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
        host->controller()->updateCamera(cameraScale, 130, 90);
        if (resizeViewport)
            page->setSize(page->size() - QSizeF(80, 60));

        session.setActiveTool(QStringLiteral("text"));
        const QPoint createPoint = root->mapToScene(
            {root->width() * 0.45, root->height() * 0.45}).toPoint();
        QSignalSpy initialFrames(&window, &QQuickWindow::frameSwapped);
        window.update();
        QTRY_VERIFY(!initialFrames.isEmpty());
        QTest::mouseMove(&window, createPoint);
        QTest::mouseClick(&window, Qt::LeftButton, Qt::NoModifier, createPoint);
        QTRY_COMPARE(host->document()->media().size(), 1);
        QTRY_VERIFY(root->property("anyMediaEditing").toBool());
        QCOMPARE(session.activeTool(), QStringLiteral("selection"));
        auto* media = host->document()->media().constFirst();
        auto* delegate = findQuickItemWithProperty(root, "currentMediaId", media->mediaId());
        QVERIFY(delegate);
        auto* editor = delegate->findChild<QQuickTextEdit*>();
        QVERIFY(editor);
        QVERIFY(editor->hasActiveFocus());
        for (char character : QByteArray("First line")) QTest::keyClick(&window, character);
        QTest::keyClick(&window, Qt::Key_Return);
        for (char character : QByteArray("Second line")) QTest::keyClick(&window, character);
        const QString original = QStringLiteral("First line\nSecond line");
        QTRY_COMPARE(media->text(), original);

        const QPoint background = root->mapToScene({24, root->height() - 24}).toPoint();
        QTest::mouseClick(&window, Qt::LeftButton, Qt::NoModifier, background);
        QTRY_VERIFY(!root->property("anyMediaEditing").toBool());
        QVERIFY(!editor->hasActiveFocus());
        if (enlargement != 1) {
            host->document()->select(media->mediaId());
            const QRectF rect = media->sceneRect();
            const qreal originalScale = media->scale();
            host->controller()->handleMediaResizeRequested(media->mediaId(), "bottom-right",
                rect.x() + rect.width() * enlargement,
                rect.y() + rect.height() * enlargement, false, false);
            host->controller()->handleMediaResizeEnded(media->mediaId());
            QVERIFY(qAbs(media->scale() - originalScale * enlargement) < 1e-8);
            // Keep the enlarged body in the visible canvas at either DPI.
            const QPointF center = (QPointF(root->width() / 2, root->height() / 2)
                - QPointF(host->controller()->panX(), host->controller()->panY()))
                / host->controller()->viewScale();
            media->setPosition(center - QPointF(media->sceneRect().width() / 2,
                                                media->sceneRect().height() / 2));
            host->document()->clearSelection();
            QCoreApplication::processEvents();
        }
        // Hit the rendered geometry, after both the resize and QTextDocument
        // layout have been polished, as a user clicking the visible item does.
        QSignalSpy frames(&window, &QQuickWindow::frameSwapped);
        window.update();
        QTRY_VERIFY(!frames.isEmpty());
        const QRectF caret = editor->positionToRectangle(15);
        const QPointF editorPoint(caret.x(), caret.center().y());
        const QPoint clickPoint = bodyY < 0 ? editor->mapToScene(editorPoint).toPoint()
            : delegate->mapToScene({delegate->width() * 0.4, delegate->height() * bodyY}).toPoint();
        const int expectedPosition = editor->positionAt(
            editor->mapFromScene(clickPoint).x(), editor->mapFromScene(clickPoint).y());
        QVERIFY(QRect(QPoint(), window.size()).contains(clickPoint));
        if (preselected) {
            QTest::mouseClick(&window, Qt::LeftButton, Qt::NoModifier, clickPoint);
            QCOMPARE(host->document()->selectedMedia(), media);
            QVERIFY(!root->property("anyMediaEditing").toBool());
        }
        QTest::mouseMove(&window, clickPoint,
            QGuiApplication::styleHints()->mouseDoubleClickInterval() + 20);
        // Two full native press/release cycles, not a direct call into the editor.
        QTest::mouseClick(&window, Qt::LeftButton, Qt::NoModifier, clickPoint, 30);
        QTest::mouseClick(&window, Qt::LeftButton, Qt::NoModifier, clickPoint, 30);
        QTRY_VERIFY_WITH_TIMEOUT(root->property("anyMediaEditing").toBool(), 1500);
        QVERIFY(editor->hasActiveFocus());
        QCOMPARE(editor->cursorPosition(), expectedPosition);
        QCOMPARE(editor->selectedText(), QString());
        QCOMPARE(media->text(), original);

        // Once editing, native clicks and drags must reach the text document,
        // including after fit-to-text publication, media resize and camera zoom.
        const QPointF mediaPosition = media->position();
        const qreal mediaScale = media->scale();
        const QPointF cameraPan(host->controller()->panX(), host->controller()->panY());
        const auto characterPoint = [&](int position) {
            const QRectF rect = editor->positionToRectangle(position);
            return editor->mapToScene({rect.x(), rect.center().y()}).toPoint();
        };
        for (int position : {2, 16}) {
            const QPoint point = characterPoint(position);
            QVERIFY(QRect(QPoint(), window.size()).contains(point));
            QTest::mouseMove(&window, point,
                QGuiApplication::styleHints()->mouseDoubleClickInterval() + 20);
            QTest::mouseClick(&window, Qt::LeftButton, Qt::NoModifier, point);
            QTRY_COMPARE(editor->cursorPosition(), position);
            QCOMPARE(editor->selectedText(), QString());
            QVERIFY(root->property("anyMediaEditing").toBool());
            QVERIFY(editor->hasActiveFocus());
        }
        for (bool reverse : {false, true}) {
            const QPoint start = characterPoint(reverse ? 18 : 3);
            const QPoint end = characterPoint(reverse ? 3 : 18);
            QTest::mouseMove(&window, start,
                QGuiApplication::styleHints()->mouseDoubleClickInterval() + 20);
            QTest::mousePress(&window, Qt::LeftButton, Qt::NoModifier, start);
            for (int step = 1; step <= 6; ++step) {
                QTest::mouseMove(&window, start + (end - start) * step / 6, 20);
                QCoreApplication::processEvents();
            }
            QTest::mouseRelease(&window, Qt::LeftButton, Qt::NoModifier, end);
            QTRY_COMPARE(editor->selectionStart(), 3);
            QCOMPARE(editor->selectionEnd(), 18);
            QCOMPARE(editor->selectedText(), original.mid(3, 15));
            QVERIFY(root->property("anyMediaEditing").toBool());
            QVERIFY(editor->hasActiveFocus());
            QCOMPARE(media->position(), mediaPosition);
            QCOMPARE(media->scale(), mediaScale);
            QCOMPARE(QPointF(host->controller()->panX(), host->controller()->panY()), cameraPan);
            QCOMPARE(media->text(), original);
        }
        QTest::keyClick(&window, Qt::Key_X);
        QString expected = original;
        expected.replace(3, 15, QLatin1Char('x'));
        QTRY_COMPARE(media->text(), expected);
        QTest::mouseClick(&window, Qt::LeftButton, Qt::NoModifier, background);
        QTRY_VERIFY(!root->property("anyMediaEditing").toBool());
        QCOMPARE(media->text(), expected);

        // A page-level settings control can overlap text. Its double-click
        // must never open the media underneath the floating panel.
        media->setFitToTextEnabled(false);
        media->setBaseSize({600, 600});
        media->setScale(1.0 / host->controller()->viewScale());
        media->setPosition(-QPointF(host->controller()->panX(), host->controller()->panY())
                           / host->controller()->viewScale());
        host->document()->select(media->mediaId());
        session.setSettingsVisible(true);
        QQuickItem* panel = nullptr;
        QTRY_VERIFY((panel = findQuickItemWithProperty(page, "objectName", "canvasSceneElementPanel")));
        QTRY_VERIFY(panel->isVisible());
        const QPoint tabPoint = panel->mapToScene({panel->width() * 0.25, 20}).toPoint();
        QTest::mouseMove(&window, tabPoint);
        QTest::mouseDClick(&window, Qt::LeftButton, Qt::NoModifier, tabPoint);
        QCoreApplication::processEvents();
        QVERIFY(!root->property("anyMediaEditing").toBool());
        QCOMPARE(media->text(), expected);
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
            QTRY_VERIFY_WITH_TIMEOUT(host->document()->selectedMedia(), 5000);
            QTRY_VERIFY_WITH_TIMEOUT(host->document()->selectedMedia()->residencyReady(), 30000);
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
                const QPointF liveScenePosition(delegate->property("effectiveLocalX").toReal(),
                                                delegate->property("effectiveLocalY").toReal());
                QVERIFY(QLineF(liveScenePosition, originalPosition
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
