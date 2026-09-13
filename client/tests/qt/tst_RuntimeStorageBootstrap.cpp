#include <QtTest>

#include "backend/runtime/RuntimeStorageBootstrap.h"
#include "backend/runtime/RuntimeProfile.h"
#include "backend/security/DeviceIdentityStore.h"

#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSettings>
#include <QTemporaryDir>

namespace {
RuntimeProfileContext temporaryContext(const QString& root,
                                       const QString& id = QStringLiteral("instance-2-test"))
{
    RuntimeProfileContext context;
    context.ordinal = 2;
    context.instanceId = id;
    context.profileId = id;
    context.rootPath = root;
    context.temporaryRoot = root;
    context.persistent = false;
    return context;
}

bool writeBytes(const QString& path, const QByteArray& bytes)
{
    if (!QDir().mkpath(QFileInfo(path).absolutePath())) return false;
    QFile file(path);
    return file.open(QIODevice::WriteOnly | QIODevice::Truncate)
        && file.write(bytes) == bytes.size();
}

QJsonObject validManifest(const RuntimeProfileContext& context, int version = 1)
{
    return QJsonObject{
        {QStringLiteral("format"), QStringLiteral("mouffette-runtime")},
        {QStringLiteral("storageVersion"), version},
        {QStringLiteral("profileId"), context.profileId},
        {QStringLiteral("persistence"), QStringLiteral("temporary")}};
}
}

class RuntimeStorageBootstrapTest final : public QObject {
    Q_OBJECT

private slots:
    void manifestCases_data();
    void manifestCases();
    void normalBootPreservesDurableStateAndPurgesCache();
    void corruptComponentsAreResetIndependently();
    void mismatchedStorageNeverTouchesExternalSource();
    void runtimesHaveIndependentSettingsCachesAndIdentities();
    void unsafeRuntimeRootFailsClosed();
};

void RuntimeStorageBootstrapTest::manifestCases_data()
{
    QTest::addColumn<QString>("kind");
    QTest::addColumn<QString>("foundVersion");
    QTest::newRow("missing") << QStringLiteral("missing") << QStringLiteral("missing");
    QTest::newRow("truncated") << QStringLiteral("truncated") << QStringLiteral("invalid");
    QTest::newRow("wrong-type") << QStringLiteral("wrong-type") << QStringLiteral("invalid");
    QTest::newRow("wrong-version") << QStringLiteral("wrong-version") << QStringLiteral("99");
}

void RuntimeStorageBootstrapTest::manifestCases()
{
    QFETCH(QString, kind);
    QFETCH(QString, foundVersion);
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const RuntimeProfileContext context = temporaryContext(
        QDir(directory.path()).filePath(QStringLiteral("runtime")));
    QVERIFY(QDir().mkpath(context.rootPath));
    const QString manifestPath = QDir(context.rootPath).filePath(
        QStringLiteral("storage-manifest.json"));
    if (kind == QStringLiteral("truncated")) {
        QVERIFY(writeBytes(manifestPath, QByteArrayLiteral("{\"storageVersion\":")));
    } else if (kind == QStringLiteral("wrong-type")) {
        QJsonObject manifest = validManifest(context);
        manifest.insert(QStringLiteral("storageVersion"), QStringLiteral("1"));
        QVERIFY(writeBytes(manifestPath,
                           QJsonDocument(manifest).toJson(QJsonDocument::Compact)));
    } else if (kind == QStringLiteral("wrong-version")) {
        QVERIFY(writeBytes(manifestPath,
                           QJsonDocument(validManifest(context, 99))
                               .toJson(QJsonDocument::Compact)));
    }
    QVERIFY(writeBytes(QDir(context.rootPath).filePath(QStringLiteral("sentinel")),
                       QByteArrayLiteral("old-runtime")));

    RuntimeStorageBootstrap bootstrap(context);
    const auto result = bootstrap.run();
    QVERIFY(result.succeeded());
    QVERIFY(result.hadReset());
    QCOMPARE(result.foundVersion, foundVersion);
    QCOMPARE(result.resetCategories,
             QStringList({QStringLiteral("settings"), QStringLiteral("projects"),
                          QStringLiteral("cache"), QStringLiteral("identity")}));
    QVERIFY(!QFileInfo::exists(
        QDir(context.rootPath).filePath(QStringLiteral("sentinel"))));
    QVERIFY(QFileInfo::exists(manifestPath));
    QVERIFY(QFileInfo::exists(RuntimeProfile::settingsFilePath()));
    QVERIFY(QFileInfo::exists(RuntimeProfile::projectsFilePath()));
}

void RuntimeStorageBootstrapTest::normalBootPreservesDurableStateAndPurgesCache()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const RuntimeProfileContext context = temporaryContext(directory.path());
    RuntimeStorageBootstrap bootstrap(context);
    QVERIFY(bootstrap.run().succeeded());

    QSettings settings(RuntimeProfile::settingsFilePath(), QSettings::IniFormat);
    settings.setValue(QStringLiteral("serverUrl"), QStringLiteral("ws://127.0.0.1:9000"));
    settings.setValue(QStringLiteral("autoUploadImportedMedia"), true);
    settings.setValue(QStringLiteral("useQuickCanvasRenderer"), false);
    settings.sync();
    const QByteArray projectsBefore = [&] {
        QFile file(RuntimeProfile::projectsFilePath());
        return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
    }();
    DeviceIdentityStore first(RuntimeProfile::identityLocation(), false, context.profileId);
    QString error;
    QVERIFY2(first.initialize(&error), qPrintable(error));
    const QString identityBefore = first.installationId();
    const QString cached = QDir(RuntimeProfile::cacheLocation()).filePath(
        QStringLiteral("Uploads/sender/session/staging/file.png"));
    QVERIFY(writeBytes(cached, QByteArrayLiteral("cache")));

    const auto result = bootstrap.run();
    QVERIFY(result.succeeded());
    QVERIFY(!result.hadReset());
    QVERIFY(!RuntimeProfile::readSettings().contains(
        QStringLiteral("useQuickCanvasRenderer")));
    QVERIFY(!QFileInfo::exists(cached));
    QCOMPARE(RuntimeProfile::readSettings().value(QStringLiteral("serverUrl")).toString(),
             QStringLiteral("ws://127.0.0.1:9000"));
    QFile projects(RuntimeProfile::projectsFilePath());
    QVERIFY(projects.open(QIODevice::ReadOnly));
    QCOMPARE(projects.readAll(), projectsBefore);
    DeviceIdentityStore restored(RuntimeProfile::identityLocation(), false, context.profileId);
    QVERIFY2(restored.initialize(&error), qPrintable(error));
    QCOMPARE(restored.installationId(), identityBefore);
}

void RuntimeStorageBootstrapTest::corruptComponentsAreResetIndependently()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const RuntimeProfileContext context = temporaryContext(directory.path());
    RuntimeStorageBootstrap bootstrap(context);
    QVERIFY(bootstrap.run().succeeded());

    QVERIFY(writeBytes(RuntimeProfile::settingsFilePath(),
                       QByteArrayLiteral("serverUrl=@Invalid()\n")));
    auto result = bootstrap.run();
    QVERIFY(result.succeeded());
    QCOMPARE(result.resetCategories, QStringList{QStringLiteral("settings")});

    QVERIFY(writeBytes(RuntimeProfile::projectsFilePath(), QByteArrayLiteral("{")));
    result = bootstrap.run();
    QVERIFY(result.succeeded());
    QCOMPARE(result.resetCategories, QStringList{QStringLiteral("projects")});

    DeviceIdentityStore identity(RuntimeProfile::identityLocation(), false, context.profileId);
    QString error;
    QVERIFY2(identity.initialize(&error), qPrintable(error));
    QVERIFY(writeBytes(identity.fallbackFilePath(), QByteArrayLiteral("corrupt-key")));
    QVERIFY(QFile::setPermissions(identity.fallbackFilePath(),
                                  QFileDevice::ReadOwner | QFileDevice::WriteOwner));
    result = bootstrap.run();
    QVERIFY(result.succeeded());
    QCOMPARE(result.resetCategories, QStringList{QStringLiteral("identity")});
    DeviceIdentityStore repaired(RuntimeProfile::identityLocation(), false, context.profileId);
    QVERIFY2(repaired.initialize(&error), qPrintable(error));
    QVERIFY(repaired.isReady());
}

void RuntimeStorageBootstrapTest::mismatchedStorageNeverTouchesExternalSource()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString external = QDir(directory.path()).filePath(QStringLiteral("source.mp4"));
    QVERIFY(writeBytes(external, QByteArrayLiteral("external-source")));
    const QString root = QDir(directory.path()).filePath(QStringLiteral("runtime"));
    const RuntimeProfileContext context = temporaryContext(root);
    QVERIFY(QDir().mkpath(root));
    QVERIFY(writeBytes(QDir(root).filePath(QStringLiteral("storage-manifest.json")),
                       QJsonDocument(validManifest(context, 0))
                           .toJson(QJsonDocument::Compact)));
    RuntimeStorageBootstrap bootstrap(context);
    QVERIFY(bootstrap.run().succeeded());
    QFile source(external);
    QVERIFY(source.open(QIODevice::ReadOnly));
    QCOMPARE(source.readAll(), QByteArrayLiteral("external-source"));
}

void RuntimeStorageBootstrapTest::runtimesHaveIndependentSettingsCachesAndIdentities()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const RuntimeProfileContext firstContext = temporaryContext(
        QDir(directory.path()).filePath(QStringLiteral("one")), QStringLiteral("runtime-one"));
    const RuntimeProfileContext secondContext = temporaryContext(
        QDir(directory.path()).filePath(QStringLiteral("two")), QStringLiteral("runtime-two"));
    RuntimeStorageBootstrap firstBootstrap(firstContext);
    QVERIFY(firstBootstrap.run().succeeded());
    QSettings firstSettings(RuntimeProfile::settingsFilePath(), QSettings::IniFormat);
    firstSettings.setValue(QStringLiteral("serverUrl"), QStringLiteral("ws://127.0.0.1:8001"));
    firstSettings.sync();
    DeviceIdentityStore firstIdentity(RuntimeProfile::identityLocation(), false,
                                      firstContext.profileId);
    QString error;
    QVERIFY2(firstIdentity.initialize(&error), qPrintable(error));

    RuntimeStorageBootstrap secondBootstrap(secondContext);
    QVERIFY(secondBootstrap.run().succeeded());
    DeviceIdentityStore secondIdentity(RuntimeProfile::identityLocation(), false,
                                       secondContext.profileId);
    QVERIFY2(secondIdentity.initialize(&error), qPrintable(error));
    QVERIFY(firstIdentity.installationId() != secondIdentity.installationId());
    QVERIFY(RuntimeProfile::settingsFilePath().startsWith(secondContext.rootPath));

    RuntimeProfile::configure(firstContext);
    QCOMPARE(RuntimeProfile::readSettings().value(QStringLiteral("serverUrl")).toString(),
             QStringLiteral("ws://127.0.0.1:8001"));
    QVERIFY(RuntimeProfile::settingsFilePath().startsWith(firstContext.rootPath));
}

void RuntimeStorageBootstrapTest::unsafeRuntimeRootFailsClosed()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString blockedRoot = QDir(directory.path()).filePath(QStringLiteral("runtime"));
    QVERIFY(writeBytes(blockedRoot, QByteArrayLiteral("not-a-directory")));
    RuntimeStorageBootstrap bootstrap(temporaryContext(blockedRoot));
    const auto result = bootstrap.run();
    QVERIFY(!result.succeeded());
    QCOMPARE(result.status,
             RuntimeStorageBootstrap::Status::RecoverableFailure);
    QCOMPARE(result.code, QStringLiteral("runtime_root_unavailable"));
}

QTEST_GUILESS_MAIN(RuntimeStorageBootstrapTest)
#include "tst_RuntimeStorageBootstrap.moc"
