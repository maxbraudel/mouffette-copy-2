#include <QtTest>
#include <QBuffer>
#include <QFile>
#include <QImageReader>
#include <QPainter>
#include <QTemporaryDir>
#include <QPromise>
#include <QQmlEngine>
#include <QQmlComponent>

#include "backend/domain/profile/ProfileImage.h"
#include "backend/domain/profile/ClientProfileCache.h"
#include "backend/managers/app/SettingsManager.h"
#include "backend/runtime/RuntimeProfile.h"
#include "backend/runtime/storage/StorageIO.h"
#include "backend/runtime/storage/StorageVersions.h"
#include "frontend/qml/ApplicationController.h"
#include "frontend/qml/QmlRuntime.h"
#include "frontend/rendering/canvas/CanvasQmlTypes.h"
#include "backend/domain/project/ProjectModel.h"

namespace {
QByteArray makeJpeg(const QColor& color = Qt::red)
{
    QImage image(250, 250, QImage::Format_RGB32);
    image.fill(color);
    QByteArray jpeg;
    QBuffer output(&jpeg);
    output.open(QIODevice::WriteOnly);
    image.save(&output, "JPEG", 90);
    return jpeg;
}
ClientInfo peer(const QString& username, const QByteArray& jpeg)
{
    ClientInfo info(QStringLiteral("endpoint-a"), QStringLiteral("Studio"), QStringLiteral("macOS"));
    info.setInstanceOrdinal(3);
    info.setUsername(username);
    info.setProfilePictureHash(ProfileImage::hash(jpeg));
    return info;
}
}

class ClientProfileTest final : public QObject
{
    Q_OBJECT
private slots:
    void usernameValidation()
    {
        QString normalized, error;
        QVERIFY(ProfileImage::normalizeUsername(QStringLiteral("  Équipe Max  "), &normalized));
        QCOMPARE(normalized, QStringLiteral("Équipe Max"));
        const QString emoji = QString::fromUtf8("🦨");
        QVERIFY(ProfileImage::normalizeUsername(emoji.repeated(64), &normalized));
        QVERIFY(!ProfileImage::normalizeUsername(emoji.repeated(65), &normalized, &error));
        QVERIFY(!ProfileImage::normalizeUsername(QStringLiteral("Max\n"), &normalized));
        QVERIFY(!ProfileImage::normalizeUsername(QStringLiteral("Ma\tx"), &normalized));
        QVERIFY(ProfileImage::normalizeUsername(QStringLiteral("   "), &normalized));
        QVERIFY(normalized.isEmpty());
    }

    void convertsAndCrops_data()
    {
        QTest::addColumn<QByteArray>("format");
        QTest::addColumn<QSize>("size");
        QTest::newRow("landscape-png") << QByteArray("PNG") << QSize(400, 200);
        QTest::newRow("portrait-jpeg") << QByteArray("JPEG") << QSize(200, 400);
        QTest::newRow("webp") << QByteArray("WEBP") << QSize(400, 200);
        QTest::newRow("small-square") << QByteArray("PNG") << QSize(16, 16);
    }

    void convertsAndCrops()
    {
        QFETCH(QByteArray, format);
        QFETCH(QSize, size);
        QTemporaryDir dir;
        QImage image(size, QImage::Format_ARGB32);
        image.fill(Qt::red);
        const int side = qMin(size.width(), size.height());
        {
            QPainter painter(&image);
            painter.fillRect(QRect((size.width() - side)/2, (size.height() - side)/2, side, side), Qt::green);
        }
        const QString path = dir.filePath(QStringLiteral("photo.") + QString::fromLatin1(format.toLower()));
        QVERIFY2(image.save(path, format.constData()), format.constData());
        QByteArray jpeg;
        QString error;
        QVERIFY2(ProfileImage::importFile(QUrl::fromLocalFile(path), &jpeg, &error), qPrintable(error));
        QImage decoded;
        QVERIFY(ProfileImage::decodeJpeg(jpeg, &decoded));
        QCOMPARE(decoded.size(), QSize(250, 250));
        QVERIFY(jpeg.size() <= ProfileImage::MaximumJpegBytes);
        for (const QPoint point : {QPoint(10, 10), QPoint(125, 125), QPoint(240, 240)}) {
            const QColor pixel = decoded.pixelColor(point);
            QVERIFY(pixel.green() > 230);
            QVERIFY(pixel.red() < 20);
        }
    }

    void transparencyAndOrientation()
    {
        QTemporaryDir dir;
        QImage transparent(200, 400, QImage::Format_ARGB32);
        transparent.fill(Qt::transparent);
        const QString png = dir.filePath(QStringLiteral("transparent.png"));
        QVERIFY(transparent.save(png));
        QByteArray jpeg;
        QVERIFY(ProfileImage::importFile(QUrl::fromLocalFile(png), &jpeg));
        QImage white;
        QVERIFY(ProfileImage::decodeJpeg(jpeg, &white));
        QVERIFY(white.pixelColor(125, 125).red() > 245);

        QImage original(100, 100, QImage::Format_RGB32);
        original.fill(Qt::blue);
        { QPainter painter(&original); painter.fillRect(0, 0, 100, 50, Qt::red); }
        QByteArray oriented;
        QBuffer output(&oriented);
        output.open(QIODevice::WriteOnly);
        QVERIFY(original.save(&output, "JPEG", 95));
        // EXIF orientation 6: a 90-degree clockwise rotation.
        oriented.insert(2, QByteArray::fromHex("ffe1002245786966000049492a0008000000010012010300010000000600000000000000"));
        const QString path = dir.filePath(QStringLiteral("oriented.jpg"));
        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly));
        QCOMPARE(file.write(oriented), oriented.size());
        file.close();
        QVERIFY(ProfileImage::importFile(QUrl::fromLocalFile(path), &jpeg));
        QImage rotated;
        QVERIFY(ProfileImage::decodeJpeg(jpeg, &rotated));
        QVERIFY(rotated.pixelColor(220, 125).red() > 230);
        QVERIFY(rotated.pixelColor(30, 125).blue() > 230);
    }

    void rejectsInvalidImagesWithoutChangingOutput()
    {
        QTemporaryDir dir;
        QByteArray output("previous picture");
        const QByteArray previous = output;
        const QString path = dir.filePath(QStringLiteral("broken.png"));
        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write("not a picture");
        file.close();
        QVERIFY(!ProfileImage::importFile(QUrl::fromLocalFile(path), &output));
        QCOMPARE(output, previous);
        QVERIFY(file.open(QIODevice::WriteOnly));
        QVERIFY(file.resize(ProfileImage::MaximumImportBytes + 1));
        file.close();
        QVERIFY(!ProfileImage::importFile(QUrl::fromLocalFile(path), &output));
        QCOMPARE(output, previous);
        QVERIFY(!ProfileImage::importFile(QUrl(QStringLiteral("https://example.com/avatar.jpg")), &output));
        QVERIFY(!ProfileImage::decodeJpeg(QByteArray(129 * 1024, 'x')));
        QVERIFY(!ProfileImage::decodeJpeg(QByteArray("not JPEG")));
    }

    void animatedWebpUsesFirstFrame()
    {
        // Two lossless frames, red then blue, generated as a tiny test fixture.
        const QByteArray animated = QByteArray::fromBase64(
            "UklGRoQAAABXRUJQVlA4WAoAAAACAAAAJwAAEwAAQU5JTQYAAAAAAAAAAABBTk1GKAAAAAAAAAAAACcAABMAAGQAAAJWUDhMDwAAAC8nwAQABxD9j/4HIqL/AQBBTk1GKAAAAAAAAAAAACcAABMAAGQAAABWUDhMDwAAAC8nwAQABxDR//4HIqL/AQA=");
        QTemporaryDir dir;
        const QString path = dir.filePath(QStringLiteral("animated.webp"));
        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly));
        QCOMPARE(file.write(animated), animated.size());
        file.close();
        QImageReader reader(path);
        QCOMPARE(reader.imageCount(), 2);
        QByteArray jpeg;
        QString error;
        QVERIFY2(ProfileImage::importFile(QUrl::fromLocalFile(path), &jpeg, &error), qPrintable(error));
        QImage result;
        QVERIFY(ProfileImage::decodeJpeg(jpeg, &result));
        QVERIFY(result.pixelColor(125, 125).red() > 230);
        QVERIFY(result.pixelColor(125, 125).blue() < 20);
    }

    void persistenceIsAtomicAndProfileScoped()
    {
        const auto previousContext = RuntimeProfile::context();
        const auto restore = qScopeGuard([&] { RuntimeProfile::configure(previousContext); });
        QTemporaryDir dir;
        RuntimeProfileContext context;
        context.rootPath = dir.filePath(QStringLiteral("primary"));
        QVERIFY(QDir().mkpath(context.rootPath));
        QVERIFY(QFile::setPermissions(context.rootPath,
            QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner));
        RuntimeProfile::configure(context);
        SettingsManager settings;
        const QByteArray jpeg = makeJpeg();
        QString error;
        QVERIFY2(settings.commitSettings(QStringLiteral("ws://localhost:8080"), true, false,
                                        QStringLiteral("Max"), jpeg, &error), qPrintable(error));
        SettingsManager reloaded;
        reloaded.loadSettings();
        QCOMPARE(reloaded.username(), QStringLiteral("Max"));
        QCOMPARE(reloaded.profilePictureJpeg(), jpeg);
        const auto before = RuntimeProfile::readSettings();
        QVERIFY(!settings.commitSettings(QStringLiteral("ws://localhost:8080"), false, true,
                                         QStringLiteral("Invalid\n"), {}, &error));
        QCOMPARE(RuntimeProfile::readSettings(), before);
        QCOMPARE(settings.username(), QStringLiteral("Max"));
        // A blocked settings parent must neither apply nor announce changes.
        const QString originalRoot = context.rootPath;
        const QString blocker = dir.filePath(QStringLiteral("blocked"));
        QFile file(blocker); QVERIFY(file.open(QIODevice::WriteOnly)); file.write("blocked"); file.close();
        context.rootPath = blocker;
        RuntimeProfile::configure(context);
        QSignalSpy changes(&settings, &SettingsManager::settingsChanged);
        QVERIFY(!settings.commitSettings(QStringLiteral("ws://localhost:8081"), false, true,
                                         QStringLiteral("Changed"), {}, &error));
        QCOMPARE(changes.count(), 0);
        QCOMPARE(settings.username(), QStringLiteral("Max"));
        context.rootPath = dir.filePath(QStringLiteral("secondary"));
        RuntimeProfile::configure(context);
        SettingsManager secondary;
        secondary.loadSettings();
        QVERIFY(secondary.username().isEmpty());
        QVERIFY(secondary.profilePictureJpeg().isEmpty());
        context.rootPath = originalRoot;
        RuntimeProfile::configure(context);
        QVERIFY(settings.commitSettings(QStringLiteral("ws://localhost:8080"), false, true, {}, {}, &error));
        reloaded.loadSettings();
        QVERIFY(reloaded.username().isEmpty());
        QVERIFY(reloaded.profilePictureJpeg().isEmpty());
    }

    void screenSharingIsOptInAtomicAndProfileScoped()
    {
        const auto previousContext = RuntimeProfile::context();
        const auto restore = qScopeGuard([&] { RuntimeProfile::configure(previousContext); });
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        RuntimeProfileContext context;
        context.rootPath = directory.filePath(QStringLiteral("primary"));
        QVERIFY(QDir().mkpath(context.rootPath));
        QVERIFY(QFile::setPermissions(context.rootPath,
            QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner));
        RuntimeProfile::configure(context);
        SettingsManager settings;
        settings.loadSettings();
        QVERIFY(!settings.getScreenSharingEnabled());
        QSignalSpy changes(&settings, &SettingsManager::screenSharingEnabledChanged);
        QString error;
        QVERIFY2(settings.commitSettings(QStringLiteral("ws://localhost:8080"), true, true,
                                          {}, {}, true, &error), qPrintable(error));
        QVERIFY(settings.getScreenSharingEnabled());
        QCOMPARE(changes.count(), 1);
        QCOMPARE(changes.at(0).at(0).toBool(), true);
        SettingsManager reloaded;
        reloaded.loadSettings();
        QVERIFY(reloaded.getScreenSharingEnabled());
        // An invalid unrelated field cannot partially enable or revoke sharing.
        QVERIFY(!settings.commitSettings(QStringLiteral("invalid"), true, true,
                                           {}, {}, false, &error));
        QVERIFY(settings.getScreenSharingEnabled());
        QCOMPARE(changes.count(), 1);
        reloaded.loadSettings();
        QVERIFY(reloaded.getScreenSharingEnabled());
        // A failed atomic disk write must not revoke consent in memory or
        // announce a change that cannot survive an application restart.
        const QString originalRoot = context.rootPath;
        const QString blocker = directory.filePath(QStringLiteral("blocked"));
        QFile file(blocker);
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write("blocked");
        file.close();
        context.rootPath = blocker;
        RuntimeProfile::configure(context);
        QVERIFY(!settings.commitSettings(QStringLiteral("ws://localhost:8080"), true, true,
                                          {}, {}, false, &error));
        QVERIFY(settings.getScreenSharingEnabled());
        QCOMPARE(changes.count(), 1);
        context.rootPath = originalRoot;
        RuntimeProfile::configure(context);
        // Older callers changing other preferences must preserve the opt-in.
        QVERIFY(settings.commitSettings(QStringLiteral("ws://localhost:8080"), false, false,
                                         {}, {}, &error));
        reloaded.loadSettings();
        QVERIFY(reloaded.getScreenSharingEnabled());
        QVERIFY(settings.commitSettings(QStringLiteral("ws://localhost:8080"), false, false,
                                         {}, {}, false, &error));
        QCOMPARE(changes.count(), 2);
        QCOMPARE(changes.at(1).at(0).toBool(), false);
        reloaded.loadSettings();
        QVERIFY(!reloaded.getScreenSharingEnabled());
        auto malformed = RuntimeProfile::readSettings();
        malformed.insert(QStringLiteral("screenSharingEnabled"), QStringLiteral("unexpected"));
        QVERIFY(RuntimeStorage::writeSettings(RuntimeProfile::profileRoot(),
            RuntimeProfile::settingsFilePath(), malformed, StorageVersions::Settings).succeeded());
        reloaded.loadSettings();
        QVERIFY(!reloaded.getScreenSharingEnabled());
        context.rootPath = directory.filePath(QStringLiteral("secondary"));
        RuntimeProfile::configure(context);
        reloaded.loadSettings();
        QVERIFY(!reloaded.getScreenSharingEnabled());
    }

    void screenContentVisibilityIsAtomicAndProfileScoped()
    {
        const auto previousContext = RuntimeProfile::context();
        const auto restore = qScopeGuard([&] { RuntimeProfile::configure(previousContext); });
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        RuntimeProfileContext context;
        context.rootPath = directory.filePath(QStringLiteral("primary"));
        QVERIFY(QDir().mkpath(context.rootPath));
        QVERIFY(QFile::setPermissions(context.rootPath,
            QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner));
        RuntimeProfile::configure(context);
        SettingsManager settings;
        settings.loadSettings();
        QVERIFY(settings.getScreenContentVisible());
        QVERIFY(!settings.getScreenSharingEnabled());
        QSignalSpy visibilityChanges(&settings, &SettingsManager::screenContentVisibleChanged);
        QSignalSpy settingsChanges(&settings, &SettingsManager::settingsChanged);
        QString error;
        // The toolbar can be used before the settings dialog is ever saved.
        QVERIFY2(settings.setScreenContentVisible(false, &error), qPrintable(error));
        QVERIFY(!settings.getScreenContentVisible());
        QVERIFY(!settings.getScreenSharingEnabled());
        QCOMPARE(visibilityChanges.count(), 1);
        QCOMPARE(visibilityChanges.at(0).at(0).toBool(), false);
        QCOMPARE(settingsChanges.count(), 1);
        QCOMPARE(RuntimeStorage::readSettings(RuntimeProfile::profileRoot(),
            RuntimeProfile::settingsFilePath()).inspection.state, RuntimeStorage::State::Current);
        SettingsManager reloaded;
        reloaded.loadSettings();
        QVERIFY(!reloaded.getScreenContentVisible());
        QVERIFY(settings.setScreenContentVisible(false, &error));
        QCOMPARE(visibilityChanges.count(), 1);
        QCOMPARE(settingsChanges.count(), 1);

        // Saving unrelated preferences and publishing consent keeps the viewer choice.
        const QByteArray jpeg = makeJpeg();
        QVERIFY2(settings.commitSettings(QStringLiteral("ws://localhost:8080"), true, false,
            QStringLiteral("Max"), jpeg, true, &error), qPrintable(error));
        QVERIFY(settings.getScreenSharingEnabled());
        settings.saveSettings();
        reloaded.loadSettings();
        QVERIFY(!reloaded.getScreenContentVisible());
        QVERIFY(reloaded.getScreenSharingEnabled());
        const auto saved = RuntimeProfile::readSettings();

        // An unwritable destination must not change memory or announce success.
        const QString originalRoot = context.rootPath;
        const QString blocker = directory.filePath(QStringLiteral("blocked"));
        QFile file(blocker);
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write("blocked");
        file.close();
        context.rootPath = blocker;
        RuntimeProfile::configure(context);
        const int savedChanges = settingsChanges.count();
        QVERIFY(!settings.setScreenContentVisible(true, &error));
        QVERIFY(!error.isEmpty());
        QVERIFY(!settings.getScreenContentVisible());
        QCOMPARE(visibilityChanges.count(), 1);
        QCOMPARE(settingsChanges.count(), savedChanges);

        context.rootPath = directory.filePath(QStringLiteral("secondary"));
        RuntimeProfile::configure(context);
        reloaded.loadSettings();
        QVERIFY(reloaded.getScreenContentVisible());
        QVERIFY(!reloaded.getScreenSharingEnabled());
        context.rootPath = originalRoot;
        RuntimeProfile::configure(context);
        QCOMPARE(RuntimeProfile::readSettings(), saved);
        reloaded.loadSettings();
        QVERIFY(!reloaded.getScreenContentVisible());

        QVERIFY2(settings.setScreenContentVisible(true, &error), qPrintable(error));
        QCOMPARE(visibilityChanges.count(), 2);
        QCOMPARE(visibilityChanges.at(1).at(0).toBool(), true);
        auto updated = RuntimeProfile::readSettings();
        QVERIFY(updated.take(QStringLiteral("screenContentVisible")).toBool());
        auto unchanged = saved;
        unchanged.remove(QStringLiteral("screenContentVisible"));
        QCOMPARE(updated, unchanged);
        reloaded.loadSettings();
        QVERIFY(reloaded.getScreenContentVisible());
        QVERIFY(reloaded.getScreenSharingEnabled());
        QCOMPARE(reloaded.username(), QStringLiteral("Max"));
        QCOMPARE(reloaded.profilePictureJpeg(), jpeg);
    }

    void peerCacheIsTransientAndIgnoresStalePictures()
    {
        ClientProfileCache cache;
        int requests = 0;
        cache.setPictureRequester([&](const QString&, const QString&) { return QString::number(++requests); });
        const QByteArray red = makeJpeg(), blue = makeJpeg(Qt::blue);
        ClientInfo current = peer(QStringLiteral("Max"), red);
        cache.observe({current});
        QCOMPARE(cache.apply(current).getInstanceDisplayName(), QStringLiteral("Max (3)"));
        QCOMPARE(cache.pictureSource(current.endpointId()), ClientProfileCache::defaultSource());
        cache.requestPicture(current.endpointId());
        cache.requestPicture(current.endpointId());
        QCOMPARE(requests, 1);
        current.setProfilePictureHash(ProfileImage::hash(blue));
        cache.observe({current});
        cache.acceptPicture(QStringLiteral("1"), current.endpointId(), ProfileImage::hash(red), red);
        QCOMPARE(cache.imageBytes(), 0);
        cache.requestPicture(current.endpointId());
        QCOMPARE(requests, 2);
        cache.acceptPicture(QStringLiteral("2"), current.endpointId(), ProfileImage::hash(blue), blue);
        QVERIFY(cache.pictureSource(current.endpointId()).startsWith(QLatin1String("image://profiles/")));
        ClientInfo offline(current.endpointId(), QStringLiteral("Studio"), QStringLiteral("macOS"));
        offline.setInstanceOrdinal(3);
        offline.setOnline(false);
        cache.observe({offline});
        QCOMPARE(cache.apply(offline).getInstanceDisplayName(), QStringLiteral("Max (3)"));
        ClientProfileCache restarted;
        restarted.observe({offline});
        QCOMPARE(restarted.apply(offline).getInstanceDisplayName(), QStringLiteral("Studio (3)"));
        QCOMPARE(restarted.pictureSource(offline.endpointId()), ClientProfileCache::defaultSource());
        current.setUsername({});
        current.setProfilePictureHash({});
        cache.observe({current});
        QCOMPARE(cache.apply(current).getInstanceDisplayName(), QStringLiteral("Studio (3)"));
        QCOMPARE(cache.pictureSource(current.endpointId()), ClientProfileCache::defaultSource());
        // Durable project references never copy presentation profiles.
        const auto json = ProjectTargetReference::fromClientInfo(peer(QStringLiteral("Private name"), red)).toJson();
        QVERIFY(!json.contains(QStringLiteral("username")));
        QVERIFY(!json.contains(QStringLiteral("profilePictureHash")));
    }

    void controllerCommitsDraftOnlyOnSave()
    {
        const auto previousContext = RuntimeProfile::context();
        const auto restore = qScopeGuard([&] { RuntimeProfile::configure(previousContext); });
        QTemporaryDir dir;
        RuntimeProfileContext context;
        context.rootPath = dir.filePath(QStringLiteral("runtime"));
        context.installationRootPath = dir.filePath(QStringLiteral("installation"));
        RuntimeProfile::configure(context);
        registerCanvasQmlTypes();
        QQmlEngine engine;
        QmlRuntime::setEngine(&engine);
        ApplicationController controller(context,
            {QStringLiteral("profile-test"), QStringLiteral("--server-url=ws://127.0.0.1:1")},
            nullptr, [] {
                QPromise<MediaBackendBootstrap::Result> promise;
                promise.start(); promise.addResult({true, {}}); promise.finish();
                return promise.future();
            });
        controller.start();
        QTRY_VERIFY_WITH_TIMEOUT(controller.ready(), 8000);
        const QString path = dir.filePath(QStringLiteral("photo.png"));
        QImage image(400, 200, QImage::Format_ARGB32); image.fill(Qt::yellow);
        QVERIFY(image.save(path));
        const auto savedBefore = RuntimeProfile::readSettings();
        controller.beginProfileEdit();
        QVERIFY(controller.importProfilePicture(QUrl::fromLocalFile(path)).isEmpty());
        QVERIFY(controller.settingsProfilePictureDraftSource().startsWith(QLatin1String("image://profiles/")));
        QQmlComponent preview(&engine);
        preview.setData(QStringLiteral("import QtQuick\nImage { width: 72; height: 72; source: \"%1\" }")
                            .arg(controller.settingsProfilePictureDraftSource()).toUtf8(), QUrl());
        QScopedPointer<QObject> previewImage(preview.create());
        QVERIFY2(previewImage, qPrintable(preview.errorString()));
        QTRY_COMPARE(previewImage->property("status").toInt(), 1); // Image.Ready
        QCOMPARE(controller.settingsProfilePictureSource(), ClientProfileCache::defaultSource());
        QCOMPARE(RuntimeProfile::readSettings(), savedBefore);
        controller.cancelProfileEdit();
        QCOMPARE(controller.settingsProfilePictureDraftSource(), ClientProfileCache::defaultSource());
        controller.beginProfileEdit();
        QVERIFY(controller.importProfilePicture(QUrl::fromLocalFile(path)).isEmpty());
        QVERIFY(controller.saveSettings(controller.settingsServerUrl(), false, true, QStringLiteral("Max")).isEmpty());
        QCOMPARE(controller.settingsUsername(), QStringLiteral("Max"));
        const auto committed = RuntimeProfile::readSettings();
        QVERIFY(!committed.value(QStringLiteral("profilePictureJpeg")).toString().isEmpty());
        controller.beginProfileEdit();
        controller.removeProfilePicture();
        QVERIFY(!controller.saveSettings(controller.settingsServerUrl(), false, true, QStringLiteral("Max\n")).isEmpty());
        QCOMPARE(RuntimeProfile::readSettings(), committed);
        controller.cancelProfileEdit();
        QVERIFY(controller.settingsProfilePictureDraftSource().startsWith(QLatin1String("image://profiles/")));
        controller.beginProfileEdit();
        controller.removeProfilePicture();
        QVERIFY(controller.saveSettings(controller.settingsServerUrl(), false, true, {}).isEmpty());
        QVERIFY(controller.settingsUsername().isEmpty());
        QCOMPARE(controller.settingsProfilePictureSource(), ClientProfileCache::defaultSource());
    }

    void peerImageCacheIsBounded()
    {
        ClientProfileCache cache;
        int requests = 0;
        cache.setPictureRequester([&](const QString&, const QString&) { return QString::number(++requests); });
        for (int i = 0; i < 300; ++i) {
            const QByteArray jpeg = makeJpeg(QColor::fromRgb((i * 678913) & 0xffffff));
            auto client = peer(QStringLiteral("Same name"), jpeg);
            client.setEndpointId(QStringLiteral("peer-%1").arg(i));
            cache.observe({client});
            cache.requestPicture(client.endpointId());
            cache.acceptPicture(QString::number(requests), client.endpointId(), ProfileImage::hash(jpeg), jpeg);
            QVERIFY(cache.imageBytes() <= 64 * 1024 * 1024);
        }
        QVERIFY(requests > 268);
        QVERIFY(cache.imageBytes() > 60 * 1024 * 1024);
        cache.clearPeers();
        QCOMPARE(cache.imageBytes(), 0);
    }
};

QTEST_MAIN(ClientProfileTest)
#include "tst_ClientProfile.moc"
