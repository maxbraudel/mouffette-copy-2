#include <QtTest>

#include "backend/runtime/RuntimeStorageBootstrap.h"
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
    context.persistent = false; // Tests must never access the user's native vault.
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
    DeviceIdentityStore identity(QDir(context.rootPath).filePath("identity"), false,
                                 context.identityNamespace());
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
    for (const QString& id : {QStringLiteral("settings"), QStringLiteral("projects"), QStringLiteral("history"),
                              QStringLiteral("identity"), QStringLiteral("cache")}) {
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
    DeviceIdentityStore identity(QDir(context.rootPath).filePath("identity"), false, context.identityNamespace());
    const QString path = component == QLatin1String("key") ? identity.fallbackFilePath() : componentPath(context, component);
    QVERIFY(writeBytes(path, "{invalid"));
    const auto result = bootstrap.run();
    QVERIFY2(result.succeeded(), qPrintable(result.cause));
    const QString resetComponent = component == QLatin1String("key") ? QStringLiteral("identity") : component;
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
    // An unreadable ancestor is an access error, not a reason to delete it.
    QVERIFY(writeBytes(QDir(directory.path()).filePath("identity"), "blocked"));
    const auto failed = bootstrap.run();
    QVERIFY(!failed.succeeded());
    QCOMPARE(failed.code, QStringLiteral("identity_storage_failed"));
    QCOMPARE(reportFor(failed, "settings").action, Action::Initialized);
    QVERIFY(seedData());
    const auto before = snapshots(context);
    QVERIFY(QFile::remove(QDir(directory.path()).filePath("identity")));
    const auto retried = bootstrap.run();
    QVERIFY2(retried.succeeded(), qPrintable(retried.cause));
    for (const char* id : {"settings", "projects", "history"}) {
        QCOMPARE(reportFor(retried, id).action, Action::Preserved);
        QCOMPARE(readBytes(componentPath(context, id)), before.value(id));
    }
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
    const auto removed = clearProfileStorage(active);
    QVERIFY2(removed.succeeded(), qPrintable(removed.reason));
    QVERIFY(!QFileInfo::exists(active.rootPath));
    QCOMPARE(snapshots(other), otherBefore);
    QCOMPARE(readBytes(externalKey), QByteArray("never remove external data"));
    QVERIFY(clearProfileStorage(active).succeeded()); // Idempotent; no recreation.
    QVERIFY(!QFileInfo::exists(active.rootPath));
    const auto nextBoot = RuntimeStorageBootstrap(active).run();
    QVERIFY(nextBoot.succeeded());
    for (const Report& report : nextBoot.components) QCOMPARE(report.action, Action::Initialized);
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
