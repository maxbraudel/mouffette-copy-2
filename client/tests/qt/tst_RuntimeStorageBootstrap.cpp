#include <QtTest>

#include "backend/runtime/RuntimeStorageBootstrap.h"
#include "backend/runtime/InstallationIdentityBootstrap.h"
#include "backend/runtime/storage/StorageIO.h"
#include "backend/runtime/storage/StorageRegistry.h"
#include "backend/security/DeviceIdentityStore.h"
#include "backend/domain/project/ProjectStore.h"
#include "backend/notifications/HistoryStore.h"

#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLockFile>
#include <QProcess>
#include <QSettings>
#include <QTemporaryDir>
#include <QTextStream>
#include <QScopeGuard>

using namespace RuntimeStorage;

namespace {
RuntimeProfileContext temporaryContext(const QString& root,
                                      const QString& channel = QStringLiteral("development"))
{
    RuntimeProfileContext context;
    context.ordinal = 2;
    context.instanceId = QStringLiteral("storage-test");
    context.profileId = QStringLiteral("storage-test");
    context.rootPath = root;
    context.channel = channel;
    context.persistent = false;
    context.useNativeIdentityVault = false;
    context.installationRootPath = root.endsWith(QStringLiteral("instance-1"))
        ? QDir(QFileInfo(root).absolutePath()).filePath(QStringLiteral("installation"))
        : QDir(root).filePath(QStringLiteral("installation"));
    return context;
}

bool writeBytes(const QString& path, const QByteArray& bytes)
{
    if (!QDir().mkpath(QFileInfo(path).absolutePath())) return false;
    QFile file(path);
    return file.open(QIODevice::WriteOnly | QIODevice::Truncate) && file.write(bytes) == bytes.size();
}

QByteArray readBytes(const QString& path)
{
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
}

QString componentPath(const RuntimeProfileContext& context, const QString& id)
{
    if (id == QLatin1String("identity"))
        return QDir(RuntimeProfile::resolvedInstallationRoot(context)).filePath(QStringLiteral("storage.json"));
    const QMap<QString, QString> paths{
        {"settings", "settings/settings.ini"}, {"projects", "projects/projects-v2.json"},
        {"history", "notification-history-v1.json"}, {"identity", "identity/storage.json"},
        {"cache", "cache/storage.json"}};
    return QDir(context.rootPath).filePath(paths.value(id));
}

Report reportFor(const RuntimeStorageBootstrap::Result& result, const QString& component)
{
    for (const Report& report : result.components)
        if (report.component == component) return report;
    Report missing;
    missing.failure = Failure::InvalidData;
    return missing;
}

QMap<QString, QByteArray> snapshots(const RuntimeProfileContext& context)
{
    QMap<QString, QByteArray> result;
    for (const Component& component : components(context))
        result.insert(component.id, readBytes(componentPath(context, component.id)));
    DeviceIdentityStore identity(RuntimeProfile::resolvedInstallationRoot(context), false,
                                 context.identityNamespace());
    result.insert("identity", readBytes(componentPath(context, "identity")));
    result.insert("key", readBytes(identity.fallbackFilePath()));
    return result;
}

bool seedData()
{
    {
        auto settings = RuntimeProfile::createSettings();
        settings->setValue("serverUrl", "ws://127.0.0.1:9123");
        settings->setValue("autoUploadImportedMedia", true);
        settings->sync();
        if (settings->status() != QSettings::NoError) return false;
    }
    ProjectRecord project;
    project.projectId = "test-project";
    project.targetEndpointId = "test-endpoint";
    project.target.endpointId = project.targetEndpointId;
    project.target.machineName = "Studio";
    project.createdAtMs = project.updatedAtMs = project.snapshotCapturedAtMs = 1000;
    project.snapshotRevision = 1;
    if (!ProjectStore().save({project})) return false;
    NotificationEntry entry;
    entry.id = "123e4567-e89b-42d3-a456-426614174000";
    entry.timestampMs = 1000;
    entry.message = "Persisted notification";
    NotificationHistoryData history;
    history.entries.append(entry);
    return HistoryStore().save(history);
}

QJsonObject launchWorker(const QString& root, const QString& mode, const QString& appVersion)
{
    QProcess worker;
    worker.start(QCoreApplication::applicationFilePath(), {"--storage-worker", root, mode, appVersion});
    if (!worker.waitForStarted(5000) || !worker.waitForFinished(15000)
        || worker.exitStatus() != QProcess::NormalExit || worker.exitCode() != 0) {
        qWarning().noquote() << worker.errorString() << worker.readAllStandardError();
        return {};
    }
    return QJsonDocument::fromJson(worker.readAllStandardOutput()).object();
}
}

class RuntimeStorageBootstrapTest final : public QObject {
    Q_OBJECT
private slots:
    void freshProfileAndLegacyManifestAreSilent();
    void realProcessRestartsPreserveData();
    void unversionedSettingsMigration_data();
    void unversionedSettingsMigration();
    void componentMismatch_data();
    void componentMismatch();
    void corruptComponents_data();
    void corruptComponents();
    void missingComponentDoesNotResetOthers();
    void interruptedIdentityCreationReusesKey();
    void independentComponentsSurviveLaterFailure();
    void migrationChainResumesAfterIoFailure();
    void migrationPolicies_data();
    void migrationPolicies();
    void channelProfilesAreIndependent();
    void lockContentionAndUnsafePaths();
    void externalLinksAreNotFollowed();
    void inaccessibleStorageIsNotReset();
    void clearProfileRemovesWholeDirectory_data();
    void clearProfileRemovesWholeDirectory();
    void clearProfileRejectsUnsafeRoots();
    void migratesPrimaryIdentityWithoutRotation();
    void sharedIdentityCorruptionWithoutMetadataIsNotReset();
    void sharedIdentityRejectsAncestorLinks();
    void missingLegacyIdentityWithCheckpointIsNotReplaced_data();
    void missingLegacyIdentityWithCheckpointIsNotReplaced();
    void inaccessibleLegacyIdentityIsNotReplaced();
};

void RuntimeStorageBootstrapTest::freshProfileAndLegacyManifestAreSilent()
{
    QTemporaryDir directory;
    const auto context = temporaryContext(directory.path());
    QVERIFY(writeBytes(QDir(directory.path()).filePath("storage-manifest.json"), "broken old manifest"));
    QVERIFY(writeBytes(QDir(directory.path()).filePath("sentinel"), "unowned"));
    RuntimeStorageBootstrap bootstrap(context);
    const auto first = bootstrap.run();
    QVERIFY2(first.succeeded(), qPrintable(first.cause));
    QVERIFY(!first.hadReset());
    QCOMPARE(first.components.size(), 5);
    for (const Report& report : first.components) QCOMPARE(report.action, Action::Initialized);
    const auto before = snapshots(context);
    const auto second = bootstrap.run();
    QVERIFY2(second.succeeded(), qPrintable(second.cause));
    for (const Report& report : second.components) QCOMPARE(report.action, Action::Preserved);
    QCOMPARE(snapshots(context), before);
    QCOMPARE(readBytes(QDir(directory.path()).filePath("sentinel")), QByteArray("unowned"));
    QCOMPARE(readBytes(QDir(directory.path()).filePath("storage-manifest.json")), QByteArray("broken old manifest"));
}

void RuntimeStorageBootstrapTest::realProcessRestartsPreserveData()
{
    QTemporaryDir directory;
    const QJsonObject first = launchWorker(directory.path(), "seed", "1.0.0");
    QVERIFY(!first.isEmpty());
    const auto context = temporaryContext(directory.path());
    const auto before = snapshots(context);
    const QString cached = QDir(directory.path()).filePath("cache/Uploads/sender/session/file.png");
    QVERIFY(writeBytes(cached, "received media"));
    for (const QString& version : {QStringLiteral("1.0.0"), QStringLiteral("99.2.0"), QStringLiteral("0.1.0")}) {
        const QJsonObject next = launchWorker(directory.path(), "check", version);
        QVERIFY(!next.isEmpty());
        QCOMPARE(next.value("identity"), first.value("identity"));
        QCOMPARE(next.value("serverUrl").toString(), QStringLiteral("ws://127.0.0.1:9123"));
        QVERIFY(next.value("autoUpload").toBool());
        for (const QJsonValue& action : next.value("actions").toArray()) QCOMPARE(action.toString(), QStringLiteral("preserved"));
        QCOMPARE(snapshots(context), before);
        QVERIFY(!QFileInfo::exists(cached));
    }
}

void RuntimeStorageBootstrapTest::unversionedSettingsMigration_data()
{
    QTest::addColumn<QByteArray>("boolean");
    QTest::addColumn<bool>("expected");
    QTest::newRow("true") << QByteArray("true") << true;
    QTest::newRow("false") << QByteArray("false") << false;
    QTest::newRow("one") << QByteArray("1") << true;
    QTest::newRow("zero") << QByteArray("0") << false;
}

void RuntimeStorageBootstrapTest::unversionedSettingsMigration()
{
    QFETCH(QByteArray, boolean);
    QFETCH(bool, expected);
    QTemporaryDir directory;
    const auto context = temporaryContext(directory.path());
    RuntimeStorageBootstrap bootstrap(context);
    QVERIFY(bootstrap.run().succeeded());
    const auto before = snapshots(context);
    const QString path = componentPath(context, "settings");
    QVERIFY(writeBytes(path, "[General]\nserverUrl=ws://127.0.0.1:9191\nautoUploadImportedMedia=" + boolean
                       + "\noptionalFutureSetting=retained\n"));
    const auto result = bootstrap.run();
    QVERIFY2(result.succeeded(), qPrintable(result.cause));
    const Report report = reportFor(result, "settings");
    QCOMPARE(report.action, Action::Migrated);
    QCOMPARE(report.foundVersion, 0);
    QCOMPARE(report.expectedVersion, 1);
    const SettingsData stored = readSettings(context.rootPath, path);
    QCOMPARE(stored.inspection.state, State::Current);
    QCOMPARE(stored.values.value("autoUploadImportedMedia").toBool(), expected);
    QVERIFY(stored.values.value("appAlwaysOnTop").toBool());
    QCOMPARE(stored.values.value("serverUrl").toString(), QStringLiteral("ws://127.0.0.1:9191"));
    QCOMPARE(stored.values.value("optionalFutureSetting").toString(), QStringLiteral("retained"));
    const auto after = snapshots(context);
    for (auto it = before.cbegin(); it != before.cend(); ++it)
        if (it.key() != QLatin1String("settings")) QCOMPARE(after.value(it.key()), it.value());
    QCOMPARE(reportFor(bootstrap.run(), "settings").action, Action::Preserved);
}

void RuntimeStorageBootstrapTest::componentMismatch_data()
{
    QTest::addColumn<QString>("component");
    QTest::addColumn<int>("version");
    for (const QString& id : {QStringLiteral("settings"), QStringLiteral("projects"), QStringLiteral("history"), QStringLiteral("cache")}) {
        QTest::newRow(qPrintable(id + "-future")) << id << 99;
        QTest::newRow(qPrintable(id + "-unknown-old")) << id << 0;
    }
    QTest::newRow("projects-explicit-reset") << QStringLiteral("projects") << 2;
}

void RuntimeStorageBootstrapTest::componentMismatch()
{
    QFETCH(QString, component);
    QFETCH(int, version);
    QTemporaryDir directory;
    const auto context = temporaryContext(directory.path());
    RuntimeStorageBootstrap bootstrap(context);
    QVERIFY(bootstrap.run().succeeded());
    QVERIFY(seedData());
    const auto before = snapshots(context);
    const QString path = componentPath(context, component);
    if (component == QLatin1String("settings")) {
        auto values = readSettings(context.rootPath, path).values;
        QVERIFY(writeSettings(context.rootPath, path, values, version).succeeded());
    } else {
        QJsonObject object = QJsonDocument::fromJson(readBytes(path)).object();
        object.insert("schemaVersion", version);
        QVERIFY(writeJson(context.rootPath, path, object).succeeded());
    }
    const auto result = bootstrap.run();
    QVERIFY2(result.succeeded(), qPrintable(result.cause));
    const bool migration = component == QLatin1String("settings") && version == 0;
    QCOMPARE(reportFor(result, component).action, migration ? Action::Migrated : Action::Reset);
    QCOMPARE(reportFor(result, component).foundVersion, version);
    const auto after = snapshots(context);
    for (auto it = before.cbegin(); it != before.cend(); ++it) {
        if (it.key() == component || (component == QLatin1String("identity") && it.key() == QLatin1String("key"))) continue;
        QCOMPARE(after.value(it.key()), it.value());
    }
    const auto next = bootstrap.run();
    QVERIFY(next.succeeded());
    for (const Report& report : next.components) QCOMPARE(report.action, Action::Preserved);
}

void RuntimeStorageBootstrapTest::corruptComponents_data()
{
    QTest::addColumn<QString>("component");
    for (const char* id : {"settings", "projects", "history", "identity", "cache", "key"})
        QTest::newRow(id) << QString::fromLatin1(id);
}

void RuntimeStorageBootstrapTest::corruptComponents()
{
    QFETCH(QString, component);
    QTemporaryDir directory;
    const auto context = temporaryContext(directory.path());
    RuntimeStorageBootstrap bootstrap(context);
    QVERIFY(bootstrap.run().succeeded());
    QVERIFY(seedData());
    const auto before = snapshots(context);
    DeviceIdentityStore identity(RuntimeProfile::resolvedInstallationRoot(context), false, context.identityNamespace());
    const QString path = component == QLatin1String("key") ? identity.fallbackFilePath() : componentPath(context, component);
    QVERIFY(writeBytes(path, "{invalid"));
    const auto result = bootstrap.run();
    if (component == QLatin1String("key")) {
        QVERIFY(!result.succeeded());
        QCOMPARE(result.code, QStringLiteral("installation_identity_failed"));
        QCOMPARE(readBytes(path), QByteArray("{invalid"));
        return;
    }
    QVERIFY2(result.succeeded(), qPrintable(result.cause));
    if (component == QLatin1String("identity")) {
        QVERIFY(!result.hadReset());
        QCOMPARE(snapshots(context).value("key"), before.value("key"));
        return;
    }
    const QString resetComponent = component;
    QCOMPARE(result.resetCategories, QStringList{resetComponent});
    const auto after = snapshots(context);
    for (auto it = before.cbegin(); it != before.cend(); ++it) {
        if (it.key() == resetComponent || (resetComponent == QLatin1String("identity") && it.key() == QLatin1String("key"))) continue;
        QCOMPARE(after.value(it.key()), it.value());
    }
    QVERIFY(!bootstrap.run().hadReset());
}

void RuntimeStorageBootstrapTest::missingComponentDoesNotResetOthers()
{
    QTemporaryDir directory;
    const auto context = temporaryContext(directory.path());
    RuntimeStorageBootstrap bootstrap(context);
    QVERIFY(bootstrap.run().succeeded());
    QVERIFY(seedData());
    const auto before = snapshots(context);
    QVERIFY(QFile::remove(componentPath(context, "history")));
    const auto result = bootstrap.run();
    QVERIFY(result.succeeded());
    QCOMPARE(reportFor(result, "history").action, Action::Initialized);
    QVERIFY(!result.hadReset());
    const auto after = snapshots(context);
    for (auto it = before.cbegin(); it != before.cend(); ++it)
        if (it.key() != QLatin1String("history")) QCOMPARE(after.value(it.key()), it.value());
}

void RuntimeStorageBootstrapTest::interruptedIdentityCreationReusesKey()
{
    QTemporaryDir directory;
    const auto context = temporaryContext(directory.path());
    RuntimeStorageBootstrap bootstrap(context);
    QVERIFY(bootstrap.run().succeeded());
    const auto before = snapshots(context);
    // Represents a crash after the new key is durable, before 'ready'.
    QVERIFY(writeJson(context.rootPath, componentPath(context, "identity"),
                      {{"schemaVersion", StorageVersions::Identity}, {"phase", "creating"}}).succeeded());
    const auto result = launchWorker(directory.path(), "check", "1.0.0");
    QVERIFY(!result.isEmpty());
    QCOMPARE(snapshots(context), before);
    QCOMPARE(reportFor(bootstrap.run(), "identity").action, Action::Preserved);
}

void RuntimeStorageBootstrapTest::independentComponentsSurviveLaterFailure()
{
    QTemporaryDir directory;
    const auto context = temporaryContext(directory.path());
    RuntimeStorageBootstrap bootstrap(context);
    QVERIFY(writeBytes(context.installationRootPath, "blocked"));
    const auto failed = bootstrap.run();
    QVERIFY(!failed.succeeded());
    QCOMPARE(failed.code, QStringLiteral("installation_identity_failed"));
    QVERIFY(!QFileInfo::exists(componentPath(context, "settings")));
    QVERIFY(QFile::remove(context.installationRootPath));
    const auto retried = bootstrap.run();
    QVERIFY2(retried.succeeded(), qPrintable(retried.cause));
    QVERIFY(seedData());
    const auto before = snapshots(context);
    const auto next = bootstrap.run();
    QVERIFY(next.succeeded());
    QCOMPARE(snapshots(context), before);
}

void RuntimeStorageBootstrapTest::migrationChainResumesAfterIoFailure()
{
    QTemporaryDir directory;
    const QString root = directory.path();
    const QString path = directory.filePath("fixture.json");
    QVERIFY(writeJson(root, path, {{"schemaVersion", 1}, {"value", 7}}).succeeded());
    int firstCalls = 0;
    bool failSecond = true;
    Component fixture{"fixture", 3,
        [&] { return readVersionedJson(root, path, 3, 4096); },
        [&] { return writeJson(root, path, {{"schemaVersion", 3}, {"value", 0}}); },
        {{1, 2, Transition::Kind::Migrate, [&] {
            ++firstCalls;
            return writeJson(root, path, {{"schemaVersion", 2}, {"value", 8}});
        }}, {2, 3, Transition::Kind::Migrate, [&] {
            if (failSecond) return Operation{Failure::IoError, "simulated disk failure"};
            return writeJson(root, path, {{"schemaVersion", 3}, {"value", 9}});
        }}}};
    const Report failure = upgrade(fixture);
    QCOMPARE(failure.failure, Failure::IoError);
    QCOMPARE(readVersionedJson(root, path, 3, 4096).version, 2);
    failSecond = false;
    const Report retry = upgrade(fixture);
    QCOMPARE(retry.action, Action::Migrated);
    QCOMPARE(firstCalls, 1);
    QCOMPARE(QJsonDocument::fromJson(readBytes(path)).object().value("value").toInt(), 9);
    QCOMPARE(upgrade(fixture).action, Action::Preserved);
}

void RuntimeStorageBootstrapTest::migrationPolicies_data()
{
    QTest::addColumn<QString>("policy");
    for (const char* policy : {"reset-barrier", "missing-path", "invalid-data", "downgrade", "bad-registry"})
        QTest::newRow(policy) << QString::fromLatin1(policy);
}

void RuntimeStorageBootstrapTest::migrationPolicies()
{
    QFETCH(QString, policy);
    QTemporaryDir directory;
    const QString root = directory.path();
    const QString path = directory.filePath("fixture.json");
    QVERIFY(writeJson(root, path, {{"schemaVersion", policy == QLatin1String("downgrade") ? 99 : 1}, {"value", 7}}).succeeded());
    int migrated = 0;
    Component fixture{"fixture", 3,
        [&] { return readVersionedJson(root, path, 3, 4096); },
        [&] { return writeJson(root, path, {{"schemaVersion", 3}, {"value", 0}}); },
        {{1, 2, Transition::Kind::Migrate, [&] {
            ++migrated;
            if (policy == QLatin1String("invalid-data")) return Operation{Failure::InvalidData, "cannot convert"};
            return writeJson(root, path, {{"schemaVersion", 2}, {"value", 8}});
        }}}};
    if (policy == QLatin1String("reset-barrier"))
        fixture.transitions.append({2, 3, Transition::Kind::Reset, {}});
    else if (policy == QLatin1String("invalid-data"))
        fixture.transitions.append({2, 3, Transition::Kind::Migrate, [&] { return Operation{}; }});
    else if (policy == QLatin1String("bad-registry"))
        fixture.transitions.append(fixture.transitions.first());
    const Report result = upgrade(fixture);
    if (policy == QLatin1String("bad-registry")) {
        QCOMPARE(result.failure, Failure::InvalidData);
        QCOMPARE(readVersionedJson(root, path, 3, 4096).version, 1);
    } else {
        QCOMPARE(result.action, Action::Reset);
        QCOMPARE(QJsonDocument::fromJson(readBytes(path)).object().value("value").toInt(), 0);
    }
    QCOMPARE(migrated, policy == QLatin1String("invalid-data") ? 1 : 0);
}

void RuntimeStorageBootstrapTest::channelProfilesAreIndependent()
{
    QTemporaryDir directory;
    const auto dev = temporaryContext(RuntimeProfile::persistentRoot(directory.path(), "development"), "development");
    const auto prod = temporaryContext(RuntimeProfile::persistentRoot(directory.path(), "production"), "production");
    QVERIFY(dev.rootPath != prod.rootPath);
    QVERIFY(dev.identityNamespace() != prod.identityNamespace());
    QVERIFY(dev.rootPath.endsWith("runtimes/development/instance-1"));
    QVERIFY(prod.rootPath.endsWith("runtimes/production/instance-1"));
    const QString legacy = directory.filePath("runtimes/instance-1/settings/settings.ini");
    QVERIFY(writeBytes(legacy, "legacy data"));
    RuntimeStorageBootstrap devBoot(dev), prodBoot(prod);
    QVERIFY(devBoot.run().succeeded());
    QVERIFY(seedData());
    const auto devBefore = snapshots(dev);
    QVERIFY(prodBoot.run().succeeded());
    QVERIFY(!RuntimeProfile::readSettings().value("autoUploadImportedMedia").toBool());
    QCOMPARE(snapshots(dev), devBefore);
    QVERIFY(snapshots(prod).value("key") != devBefore.value("key"));
    QCOMPARE(readBytes(legacy), QByteArray("legacy data"));
}

void RuntimeStorageBootstrapTest::lockContentionAndUnsafePaths()
{
    QTemporaryDir directory;
    const auto context = temporaryContext(directory.path());
    QLockFile lock(directory.filePath("bootstrap.lock"));
    lock.setStaleLockTime(0);
    QVERIFY(lock.tryLock(0));
    const auto blocked = RuntimeStorageBootstrap(context).run();
    QVERIFY(!blocked.succeeded());
    QCOMPARE(blocked.code, QStringLiteral("runtime_locked"));
    QVERIFY(!QFileInfo::exists(componentPath(context, "settings")));
    lock.unlock();
    QVERIFY(RuntimeStorageBootstrap(context).run().succeeded());
    const QString badRoot = directory.filePath("not-a-directory");
    QVERIFY(writeBytes(badRoot, "blocked"));
    QCOMPARE(RuntimeStorageBootstrap(temporaryContext(badRoot)).run().code, QStringLiteral("runtime_root_unavailable"));
}

void RuntimeStorageBootstrapTest::externalLinksAreNotFollowed()
{
#ifdef Q_OS_WIN
    QSKIP("Windows QFile::link creates shortcuts; symbolic-link coverage runs on macOS.");
#else
    QTemporaryDir directory;
    const QString external = directory.filePath("external");
    QVERIFY(QDir().mkpath(external));
    const QString sentinel = QDir(external).filePath("keep");
    QVERIFY(writeBytes(sentinel, "external data"));
    const auto context = temporaryContext(directory.filePath("runtime"));
    RuntimeStorageBootstrap bootstrap(context);
    QVERIFY(bootstrap.run().succeeded());
    const QString projects = componentPath(context, "projects");
    QVERIFY(QFile::remove(projects));
    QVERIFY(QFile::link(sentinel, projects));
    QVERIFY(QFile::link(external, QDir(context.rootPath).filePath("cache/Uploads/link")));
    QVERIFY(bootstrap.run().succeeded());
    QCOMPARE(readBytes(sentinel), QByteArray("external data"));
    const QString settingsDir = QDir(context.rootPath).filePath("settings");
    QVERIFY(QDir(settingsDir).removeRecursively());
    QVERIFY(QFile::link(external, settingsDir));
    const auto unsafe = bootstrap.run();
    QVERIFY(!unsafe.succeeded());
    QCOMPARE(reportFor(unsafe, "settings").failure, Failure::IoError);
    QCOMPARE(readBytes(sentinel), QByteArray("external data"));
#endif
}

void RuntimeStorageBootstrapTest::inaccessibleStorageIsNotReset()
{
#ifdef Q_OS_WIN
    QSKIP("POSIX permission test; Windows exercises I/O failure and locks in other cases.");
#else
    QTemporaryDir directory;
    const auto context = temporaryContext(directory.path());
    RuntimeStorageBootstrap bootstrap(context);
    QVERIFY(bootstrap.run().succeeded());
    const QString path = componentPath(context, "projects");
    const QByteArray before = readBytes(path);
    QVERIFY(QFile::setPermissions(path, QFileDevice::WriteOwner));
    const auto result = bootstrap.run();
    QVERIFY(QFile::setPermissions(path, QFileDevice::ReadOwner | QFileDevice::WriteOwner));
    QVERIFY(!result.succeeded());
    QCOMPARE(reportFor(result, "projects").failure, Failure::IoError);
    QCOMPARE(readBytes(path), before);
    // Force QSaveFile's temporary-file creation to fail: the original remains.
    const QString parent = QFileInfo(path).absolutePath();
    QVERIFY(QFile::setPermissions(parent, QFileDevice::ReadOwner | QFileDevice::ExeOwner));
    const Operation written = writeFile(context.rootPath, path, "replacement");
    QVERIFY(QFile::setPermissions(parent, QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner));
    QCOMPARE(written.failure, Failure::IoError);
    QCOMPARE(readBytes(path), before);
#endif
}

void RuntimeStorageBootstrapTest::clearProfileRemovesWholeDirectory_data()
{
    QTest::addColumn<QString>("channel");
    QTest::newRow("development") << QStringLiteral("development");
    QTest::newRow("production") << QStringLiteral("production");
}

void RuntimeStorageBootstrapTest::clearProfileRemovesWholeDirectory()
{
    QFETCH(QString, channel);
    QTemporaryDir directory;
    const QString otherChannel = channel == QLatin1String("development")
        ? QStringLiteral("production") : QStringLiteral("development");
    const auto other = temporaryContext(RuntimeProfile::persistentRoot(directory.path(), otherChannel), otherChannel);
    const auto active = temporaryContext(RuntimeProfile::persistentRoot(directory.path(), channel), channel);
    QVERIFY(RuntimeStorageBootstrap(other).run().succeeded());
    const auto otherBefore = snapshots(other);
    QVERIFY(RuntimeStorageBootstrap(active).run().succeeded());
    QVERIFY(seedData());
    QVERIFY(writeBytes(QDir(active.rootPath).filePath("unknown/.hidden/file"), "also remove me"));
    const QString external = directory.filePath("external");
    const QString externalKey = QDir(external).filePath("device-identity-v2.pk8");
    QVERIFY(writeBytes(externalKey, "never remove external data"));
#ifndef Q_OS_WIN
    QVERIFY(QDir(QDir(active.rootPath).filePath("identity")).removeRecursively());
    QVERIFY(QFile::link(external, QDir(active.rootPath).filePath("identity")));
    QVERIFY(QFile::link(external, QDir(active.rootPath).filePath("cache/Uploads/external")));
#endif
    const QByteArray installationKey = snapshots(active).value("key");
    const auto removed = clearProfileStorage(active);
    QVERIFY2(removed.succeeded(), qPrintable(removed.reason));
    QVERIFY(!QFileInfo::exists(active.rootPath));
    QCOMPARE(snapshots(other), otherBefore);
    QCOMPARE(readBytes(externalKey), QByteArray("never remove external data"));
    QVERIFY(clearProfileStorage(active).succeeded()); // Idempotent; no recreation.
    QVERIFY(!QFileInfo::exists(active.rootPath));
    const auto nextBoot = RuntimeStorageBootstrap(active).run();
    QVERIFY(nextBoot.succeeded());
    for (const Report& report : nextBoot.components)
        QCOMPARE(report.action, report.component == QLatin1String("identity") ? Action::Preserved : Action::Initialized);
    QCOMPARE(snapshots(active).value("key"), installationKey);
}

void RuntimeStorageBootstrapTest::clearProfileRejectsUnsafeRoots()
{
    QVERIFY(!clearProfileStorage(temporaryContext(QString())).succeeded());
    QVERIFY(!clearProfileStorage(temporaryContext(QDir::rootPath())).succeeded());
    QVERIFY(!clearProfileStorage(temporaryContext(QStringLiteral("relative"))).succeeded());
#ifndef Q_OS_WIN
    QTemporaryDir directory;
    const QString external = directory.filePath("external");
    QVERIFY(writeBytes(QDir(external).filePath("sentinel"), "keep"));
    const QString link = directory.filePath("runtime");
    QVERIFY(QFile::link(external, link));
    QVERIFY(!clearProfileStorage(temporaryContext(link)).succeeded());
    QCOMPARE(readBytes(QDir(external).filePath("sentinel")), QByteArray("keep"));
#endif
}

void RuntimeStorageBootstrapTest::migratesPrimaryIdentityWithoutRotation()
{
    QTemporaryDir directory;
    auto primary = temporaryContext(directory.filePath("primary"));
    primary.ordinal = 1;
    primary.instanceId = QStringLiteral("primary");
    primary.installationRootPath = directory.filePath("installation");
    primary.legacyPrimaryRootPath = primary.rootPath;
    DeviceIdentityStore legacy(QDir(primary.rootPath).filePath("identity"), false,
                               primary.identityNamespace());
    QString error;
    QVERIFY2(legacy.initialize(&error), qPrintable(error));
    const QString installation = legacy.installationId();
    const QString endpoint = DeviceIdentityStore::endpointIdForInstallation(installation, "primary");
    const QByteArray originalKey = readBytes(legacy.fallbackFilePath());

    // A secondary can be the first launcher after the upgrade. It adopts #1's
    // key before touching its own disposable profile.
    auto secondary = primary;
    secondary.rootPath = directory.filePath("secondary");
    secondary.ordinal = 2;
    secondary.instanceId = QStringLiteral("instance-2");
    secondary.profileId = QStringLiteral("temporary-uuid");
    QVERIFY2(RuntimeStorageBootstrap(secondary).run().succeeded(), qPrintable(error));
    DeviceIdentityStore shared(primary.installationRootPath, false, primary.identityNamespace());
    QVERIFY(shared.initialize(&error));
    QCOMPARE(shared.installationId(), installation);
    QCOMPARE(readBytes(shared.fallbackFilePath()), originalKey);
    QVERIFY(RuntimeStorageBootstrap(primary).run().succeeded());
    QCOMPARE(DeviceIdentityStore::endpointIdForInstallation(shared.installationId(), "primary"), endpoint);
    QVERIFY(clearProfileStorage(primary).succeeded());
    QVERIFY(!QFileInfo::exists(primary.rootPath));
    QVERIFY(QFileInfo::exists(secondary.rootPath));
    QCOMPARE(readBytes(shared.fallbackFilePath()), originalKey);
    QVERIFY(RuntimeStorageBootstrap(primary).run().succeeded());
    QCOMPARE(readBytes(shared.fallbackFilePath()), originalKey);
    QVERIFY(clearProfileStorage(secondary).succeeded());
    QCOMPARE(readBytes(shared.fallbackFilePath()), originalKey);
}

void RuntimeStorageBootstrapTest::sharedIdentityCorruptionWithoutMetadataIsNotReset()
{
    QTemporaryDir directory;
    auto context = temporaryContext(directory.filePath("runtime"));
    context.installationRootPath = directory.filePath("installation");
    QVERIFY(RuntimeStorageBootstrap(context).run().succeeded());
    DeviceIdentityStore identity(context.installationRootPath, false, context.identityNamespace());
    QVERIFY(QFile::remove(componentPath(context, "identity")));
    QVERIFY(writeBytes(identity.fallbackFilePath(), "corrupt installation key"));
    const auto result = RuntimeStorageBootstrap(context).run();
    QVERIFY(!result.succeeded());
    QCOMPARE(result.code, QStringLiteral("installation_identity_failed"));
    QCOMPARE(readBytes(identity.fallbackFilePath()), QByteArray("corrupt installation key"));
    QVERIFY(!QFileInfo::exists(componentPath(context, "identity")));
}

void RuntimeStorageBootstrapTest::sharedIdentityRejectsAncestorLinks()
{
#ifdef Q_OS_WIN
    QSKIP("Windows QFile::link creates shortcuts, not directory symlinks.");
#else
    QTemporaryDir directory;
    const QString external = directory.filePath("external");
    QVERIFY(QDir().mkpath(external));
    const QString link = directory.filePath("link");
    QVERIFY(QFile::link(external, link));
    auto context = temporaryContext(directory.filePath("runtime"));
    context.installationRootPath = QDir(link).filePath("installation");
    QString error;
    QVERIFY(!InstallationIdentityBootstrap::prepare(context, &error));
    QVERIFY(!QFileInfo::exists(QDir(external).filePath("installation")));
    context.installationRootPath = directory.filePath("installation");
    context.legacyPrimaryRootPath = QDir(link).filePath("old-primary");
    QVERIFY(!InstallationIdentityBootstrap::prepare(context, &error));
    DeviceIdentityStore identity(context.installationRootPath, false, context.identityNamespace());
    QVERIFY(!QFileInfo::exists(identity.fallbackFilePath()));
#endif
}

void RuntimeStorageBootstrapTest::missingLegacyIdentityWithCheckpointIsNotReplaced_data()
{
    QTest::addColumn<QByteArray>("checkpoint");
    QTest::newRow("ready") << QByteArray(R"({"schemaVersion":1,"phase":"ready"})");
    QTest::newRow("corrupt") << QByteArray("{invalid checkpoint");
}

void RuntimeStorageBootstrapTest::missingLegacyIdentityWithCheckpointIsNotReplaced()
{
    QFETCH(QByteArray, checkpoint);
    QTemporaryDir directory;
    auto context = temporaryContext(directory.filePath("secondary"));
    context.installationRootPath = directory.filePath("installation");
    context.legacyPrimaryRootPath = directory.filePath("primary");
    const QString legacyDirectory = QDir(context.legacyPrimaryRootPath).filePath("identity");
    const QString legacyCheckpoint = QDir(legacyDirectory).filePath("storage.json");
    QVERIFY(writeBytes(legacyCheckpoint, checkpoint));

    const auto result = RuntimeStorageBootstrap(context).run();
    QVERIFY(!result.succeeded());
    QCOMPARE(result.code, QStringLiteral("installation_identity_failed"));
    QCOMPARE(readBytes(legacyCheckpoint), checkpoint);
    DeviceIdentityStore shared(context.installationRootPath, false, context.identityNamespace());
    DeviceIdentityStore legacy(legacyDirectory, false, context.identityNamespace());
    QVERIFY(!QFileInfo::exists(shared.fallbackFilePath()));
    QVERIFY(!QFileInfo::exists(legacy.fallbackFilePath()));
    QVERIFY(!QFileInfo::exists(componentPath(context, "identity")));
}

void RuntimeStorageBootstrapTest::inaccessibleLegacyIdentityIsNotReplaced()
{
#ifdef Q_OS_WIN
    QSKIP("This test requires POSIX directory permissions.");
#else
    QTemporaryDir directory;
    auto context = temporaryContext(directory.filePath("secondary"));
    context.installationRootPath = directory.filePath("installation");
    context.legacyPrimaryRootPath = directory.filePath("primary");
    const QString legacyDirectory = QDir(context.legacyPrimaryRootPath).filePath("identity");
    const QString checkpoint = QDir(legacyDirectory).filePath("storage.json");
    const QByteArray original(R"({"schemaVersion":1,"phase":"ready"})");
    QVERIFY(writeBytes(checkpoint, original));
    const auto permissions = QFile::permissions(legacyDirectory);
    const auto restore = qScopeGuard([legacyDirectory, permissions] {
        QFile::setPermissions(legacyDirectory, permissions);
    });
    // Stat of the directory still succeeds, while looking up its checkpoint
    // or key yields EACCES. It must not be mistaken for a fresh installation.
    QVERIFY(QFile::setPermissions(legacyDirectory, QFileDevice::ReadOwner | QFileDevice::WriteOwner));
    QFile probe(checkpoint);
    if (probe.open(QIODevice::ReadOnly)) {
        probe.close();
        QSKIP("The current user can bypass directory permissions.");
    }
    const auto result = RuntimeStorageBootstrap(context).run();
    QVERIFY(!result.succeeded());
    QCOMPARE(result.code, QStringLiteral("installation_identity_failed"));
    DeviceIdentityStore shared(context.installationRootPath, false, context.identityNamespace());
    QVERIFY(!QFileInfo::exists(shared.fallbackFilePath()));
    QVERIFY(!QFileInfo::exists(componentPath(context, "identity")));
    QVERIFY(QFile::setPermissions(legacyDirectory, permissions));
    QCOMPARE(readBytes(checkpoint), original);
#endif
}

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    const QStringList args = app.arguments();
    if (args.size() == 5 && args.at(1) == QLatin1String("--storage-worker")) {
        QCoreApplication::setApplicationVersion(args.at(4));
        const auto context = temporaryContext(args.at(2));
        const auto result = RuntimeStorageBootstrap(context).run();
        if (!result.succeeded()) return 2;
        if (args.at(3) == QLatin1String("seed") && !seedData()) return 3;
        DeviceIdentityStore identity(RuntimeProfile::identityLocation(), false, context.identityNamespace());
        if (!identity.initialize()) return 4;
        QJsonArray actions;
        for (const Report& report : result.components) actions.append(actionName(report.action));
        const auto settings = RuntimeProfile::readSettings();
        QJsonObject output{{"identity", identity.installationId()},
                           {"serverUrl", settings.value("serverUrl").toString()},
                           {"autoUpload", settings.value("autoUploadImportedMedia").toBool()},
                           {"actions", actions}};
        QTextStream(stdout) << QJsonDocument(output).toJson(QJsonDocument::Compact) << Qt::endl;
        return 0;
    }
    RuntimeStorageBootstrapTest test;
    return QTest::qExec(&test, argc, argv);
}
#include "tst_RuntimeStorageBootstrap.moc"
