#include <QtTest>

#include "backend/runtime/ApplicationInstanceManager.h"
#include "backend/runtime/RuntimeProfile.h"

#include <QDir>
#include <QFile>
#include <QLockFile>
#include <QProcess>
#include <QSignalSpy>
#include <QScopeGuard>
#include <QDateTime>
#include <QSettings>
#include <QTemporaryDir>
#include <QTextStream>

#include <memory>
#include <algorithm>
#include <vector>

class ApplicationInstanceManagerTest final : public QObject {
    Q_OBJECT

private slots:
    void allocatesAndReusesUnboundedSlots();
    void allocatesConcurrentSlotsAtomically();
    void preservesLiveWorkerAndRecoversCrashedSlot();
    void singleInstanceLaunchRequestsActivation();
    void cleansOnlyAbandonedTemporaryProfiles();
    void redirectsSecondaryWritableState();
    void releasesProfileLockWithoutReleasingInstanceSlot();
};

void ApplicationInstanceManagerTest::allocatesAndReusesUnboundedSlots()
{
    QTemporaryDir root;
    QVERIFY(root.isValid());

    ApplicationInstanceManager first(QStringLiteral("allocation"), true, root.path());
    QString error;
    QCOMPARE(first.start(&error), ApplicationInstanceManager::StartResult::Started);
    QCOMPARE(first.profile().ordinal, 1);
    QCOMPARE(first.profile().instanceId, QStringLiteral("primary"));

    auto second = std::make_unique<ApplicationInstanceManager>(
        QStringLiteral("allocation"), true, root.path());
    QCOMPARE(second->start(&error), ApplicationInstanceManager::StartResult::Started);
    QCOMPARE(second->profile().ordinal, 2);
    QVERIFY(second->profile().isTemporary());
    const QString secondRoot = second->profile().rootPath;
    QVERIFY(QDir(secondRoot).exists());

    ApplicationInstanceManager third(QStringLiteral("allocation"), true, root.path());
    QCOMPARE(third.start(&error), ApplicationInstanceManager::StartResult::Started);
    QCOMPARE(third.profile().ordinal, 3);
    QVERIFY(third.profile().instanceId != second->profile().instanceId);

    second.reset();
    QVERIFY(!QDir(secondRoot).exists());
    ApplicationInstanceManager replacement(QStringLiteral("allocation"), true, root.path());
    QCOMPARE(replacement.start(&error), ApplicationInstanceManager::StartResult::Started);
    QCOMPARE(replacement.profile().ordinal, 2);
}

void ApplicationInstanceManagerTest::allocatesConcurrentSlotsAtomically()
{
    QTemporaryDir root;
    QVERIFY(root.isValid());
    ApplicationInstanceManager primary(QStringLiteral("concurrent"), true, root.path());
    QString error;
    QCOMPARE(primary.start(&error), ApplicationInstanceManager::StartResult::Started);

    constexpr int workerCount = 6;
    std::vector<int> ordinals;
    QStringList errors;
    std::vector<std::unique_ptr<QProcess>> workers;
    workers.reserve(workerCount);

    for (int index = 0; index < workerCount; ++index) {
        auto worker = std::make_unique<QProcess>();
        worker->setProgram(QCoreApplication::applicationFilePath());
        worker->setArguments({QStringLiteral("--instance-worker"), root.path()});
        worker->start();
        QVERIFY2(worker->waitForStarted(5000), qPrintable(worker->errorString()));
        workers.push_back(std::move(worker));
    }

    // Release every distinct process through the same start barrier. Each
    // worker keeps its manager alive until its stdin is closed below.
    for (const auto& worker : workers) {
        QCOMPARE(worker->write("go\n"), qint64(3));
        QVERIFY(worker->waitForBytesWritten(1000));
    }
    for (const auto& worker : workers) {
        QVERIFY2(worker->waitForReadyRead(5000), qPrintable(worker->errorString()));
        const QString response = QString::fromUtf8(worker->readLine()).trimmed();
        bool ordinalOk = false;
        const int ordinal = response.toInt(&ordinalOk);
        if (ordinalOk) ordinals.push_back(ordinal);
        else errors.append(response.isEmpty() ? worker->readAllStandardError() : response);
    }

    QVERIFY2(errors.isEmpty(), qPrintable(errors.join(QStringLiteral("; "))));
    std::sort(ordinals.begin(), ordinals.end());
    const std::vector<int> expected{2, 3, 4, 5, 6, 7};
    QStringList observed;
    for (const int ordinal : ordinals) observed.append(QString::number(ordinal));
    QVERIFY2(ordinals == expected, qPrintable(QStringLiteral("Allocated slots: %1")
        .arg(observed.join(QStringLiteral(", ")))));

    for (const auto& worker : workers) worker->closeWriteChannel();
    for (const auto& worker : workers) {
        QVERIFY(worker->waitForFinished(5000));
        QCOMPARE(worker->exitStatus(), QProcess::NormalExit);
        QCOMPARE(worker->exitCode(), 0);
    }
}

void ApplicationInstanceManagerTest::preservesLiveWorkerAndRecoversCrashedSlot()
{
    QTemporaryDir root;
    QVERIFY(root.isValid());
    ApplicationInstanceManager primary(QStringLiteral("concurrent"), true, root.path());
    QString error;
    QCOMPARE(primary.start(&error), ApplicationInstanceManager::StartResult::Started);
    QProcess worker;
    worker.setProgram(QCoreApplication::applicationFilePath());
    worker.setArguments({QStringLiteral("--instance-worker"), root.path()});
    worker.start();
    QVERIFY2(worker.waitForStarted(5000), qPrintable(worker.errorString()));
    const auto releaseWorker = qScopeGuard([&worker] {
        if (worker.state() == QProcess::NotRunning) return;
        worker.closeWriteChannel();
        if (!worker.waitForFinished(5000)) {
            worker.kill();
            worker.waitForFinished(5000);
        }
    });
    QCOMPARE(worker.write("go\n"), qint64(3));
    QVERIFY(worker.waitForBytesWritten(1000));
    QVERIFY(worker.waitForReadyRead(5000));
    QCOMPARE(worker.readLine().trimmed(), QByteArrayLiteral("2"));
    const qint64 workerPid = worker.processId();
    const QString slotPath = QDir(root.path()).filePath(QStringLiteral("slot-2.lock"));
    QLockFile slot(slotPath);
    qint64 ownerPid = 0;
    QString ownerHost;
    QString ownerApplication;
    QVERIFY(slot.getLockInfo(&ownerPid, &ownerHost, &ownerApplication));
    QCOMPARE(ownerPid, workerPid);
#ifndef Q_OS_WIN
    // Windows denies write access to a live QLockFile. On Unix also exercise
    // contention after the ordinary stale-time threshold has elapsed.
    QFile ageSlot(slotPath);
    QVERIFY(ageSlot.open(QIODevice::ReadWrite));
    QVERIFY(ageSlot.setFileTime(QDateTime::currentDateTime().addSecs(-60),
                               QFileDevice::FileModificationTime));
    ageSlot.close();
#endif
    const QDir profiles(QDir(root.path()).filePath(QStringLiteral("profiles")));
    const auto oldProfiles = profiles.entryList({QStringLiteral("instance-2-*")}, QDir::Dirs);
    QCOMPARE(oldProfiles.size(), 1);
    const QString workerProfile = profiles.filePath(oldProfiles.first());
    {
        ApplicationInstanceManager contender(QStringLiteral("concurrent"), true, root.path());
        QCOMPARE(contender.start(&error), ApplicationInstanceManager::StartResult::Started);
        QCOMPARE(contender.profile().ordinal, 3);
        QVERIFY(slot.getLockInfo(&ownerPid, &ownerHost, &ownerApplication));
        QCOMPARE(ownerPid, workerPid);
        QVERIFY(QDir(workerProfile).exists());
    }
    // Kill only the child spawned above: its slot/profile locks remain on disk.
    worker.kill();
    QVERIFY(worker.waitForFinished(5000));
    QCOMPARE(worker.exitStatus(), QProcess::CrashExit);
    QVERIFY(QFileInfo::exists(slotPath));
    ApplicationInstanceManager recovered(QStringLiteral("concurrent"), true, root.path());
    QCOMPARE(recovered.start(&error), ApplicationInstanceManager::StartResult::Started);
    QCOMPARE(recovered.profile().ordinal, 2);
    QVERIFY(!QDir(workerProfile).exists());
    QVERIFY(slot.getLockInfo(&ownerPid, &ownerHost, &ownerApplication));
    QCOMPARE(ownerPid, QCoreApplication::applicationPid());
}

void ApplicationInstanceManagerTest::singleInstanceLaunchRequestsActivation()
{
    QTemporaryDir root;
    QVERIFY(root.isValid());
    ApplicationInstanceManager primary(QStringLiteral("single"), false, root.path());
    QString error;
    QCOMPARE(primary.start(&error), ApplicationInstanceManager::StartResult::Started);
    QSignalSpy activation(&primary, &ApplicationInstanceManager::activationRequested);

    ApplicationInstanceManager duplicate(QStringLiteral("single"), false, root.path());
    QCOMPARE(duplicate.start(&error),
             ApplicationInstanceManager::StartResult::ActivatedExisting);
    QTRY_COMPARE_WITH_TIMEOUT(activation.count(), 1, 2000);
}

void ApplicationInstanceManagerTest::cleansOnlyAbandonedTemporaryProfiles()
{
    QTemporaryDir root;
    QVERIFY(root.isValid());
    const QString profiles = QDir(root.path()).filePath(QStringLiteral("profiles"));
    const QString abandoned = QDir(profiles).filePath(QStringLiteral("instance-8-abandoned"));
    const QString active = QDir(profiles).filePath(QStringLiteral("instance-9-active"));
    QVERIFY(QDir().mkpath(abandoned));
    QVERIFY(QDir().mkpath(active));
    QLockFile activeLock(QDir(active).filePath(QStringLiteral("active.lock")));
    QVERIFY(activeLock.tryLock(0));

    {
        ApplicationInstanceManager manager(QStringLiteral("cleanup"), true, root.path());
        QString error;
        QCOMPARE(manager.start(&error), ApplicationInstanceManager::StartResult::Started);
        QVERIFY(!QDir(abandoned).exists());
        QVERIFY(QDir(active).exists());
    }
    activeLock.unlock();
    QDir(active).removeRecursively();
}

void ApplicationInstanceManagerTest::redirectsSecondaryWritableState()
{
    QTemporaryDir root;
    QVERIFY(root.isValid());
    RuntimeProfileContext context;
    context.ordinal = 2;
    context.instanceId = QStringLiteral("11111111-2222-4333-8444-555555555555");
    context.profileId = QStringLiteral("instance-2-test");
    context.rootPath = root.path();
    context.persistent = false;
    RuntimeProfile::configure(context);

    QVERIFY(RuntimeProfile::appDataLocation().startsWith(root.path()));
    QVERIFY(RuntimeProfile::cacheLocation().startsWith(root.path()));
    QCOMPARE(RuntimeProfile::profileRoot(), QDir::cleanPath(root.path()));
    QVERIFY(RuntimeProfile::settingsFilePath().contains(QStringLiteral("settings/settings.ini")));
    QVERIFY(RuntimeProfile::projectsFilePath().contains(QStringLiteral("projects/projects-v2.json")));
    const std::unique_ptr<QSettings> settings = RuntimeProfile::createSettings();
    settings->setValue(QStringLiteral("serverUrl"), QStringLiteral("ws://localhost:8080"));
    settings->sync();
    QCOMPARE(RuntimeProfile::readSettings().value(QStringLiteral("serverUrl")).toString(),
             QStringLiteral("ws://localhost:8080"));
}

void ApplicationInstanceManagerTest::releasesProfileLockWithoutReleasingInstanceSlot()
{
    QTemporaryDir root;
    ApplicationInstanceManager primary(QStringLiteral("clear-profile"), true, root.path());
    QString error;
    QCOMPARE(primary.start(&error), ApplicationInstanceManager::StartResult::Started);
    QLockFile profileLock(QDir(primary.profile().rootPath).filePath(QStringLiteral("active.lock")));
    QVERIFY(!profileLock.tryLock(0));
    primary.releaseProfileLockForRemoval();
    QVERIFY(profileLock.tryLock(0));
    profileLock.unlock();
    ApplicationInstanceManager secondary(QStringLiteral("clear-profile"), true, root.path());
    QCOMPARE(secondary.start(&error), ApplicationInstanceManager::StartResult::Started);
    QCOMPARE(secondary.profile().ordinal, 2);
}

int main(int argc, char** argv)
{
    QCoreApplication application(argc, argv);
    const QStringList arguments = application.arguments();
    if (arguments.size() == 3 && arguments.at(1) == QStringLiteral("--instance-worker")) {
        QFile input;
        if (!input.open(stdin, QIODevice::ReadOnly)) return 10;
        if (QTextStream(&input).readLine() != QStringLiteral("go")) return 11;

        ApplicationInstanceManager manager(
            QStringLiteral("concurrent"), true, arguments.at(2));
        QString error;
        const auto result = manager.start(&error);
        QTextStream output(stdout);
        if (result != ApplicationInstanceManager::StartResult::Started) {
            output << QStringLiteral("ERROR: ") << error << Qt::endl;
            return 12;
        }
        output << manager.profile().ordinal << Qt::endl;
        input.readAll(); // Keep the slot locked until the parent verifies all workers.
        return 0;
    }

    ApplicationInstanceManagerTest test;
    return QTest::qExec(&test, argc, argv);
}

#include "tst_ApplicationInstanceManager.moc"
