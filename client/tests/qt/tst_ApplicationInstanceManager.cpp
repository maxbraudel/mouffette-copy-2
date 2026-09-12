#include <QtTest>

#include "backend/runtime/ApplicationInstanceManager.h"
#include "backend/runtime/RuntimeProfile.h"

#include <QDir>
#include <QFile>
#include <QLockFile>
#include <QProcess>
#include <QSignalSpy>
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
    void singleInstanceLaunchRequestsActivation();
    void cleansOnlyAbandonedTemporaryProfiles();
    void redirectsSecondaryWritableState();
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
    const QString secondRoot = second->profile().temporaryRoot;
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
    QVERIFY(ordinals == expected);

    for (const auto& worker : workers) worker->closeWriteChannel();
    for (const auto& worker : workers) {
        QVERIFY(worker->waitForFinished(5000));
        QCOMPARE(worker->exitStatus(), QProcess::NormalExit);
        QCOMPARE(worker->exitCode(), 0);
    }
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
    context.temporaryRoot = root.path();
    RuntimeProfile::configure(context);

    QVERIFY(RuntimeProfile::appDataLocation().startsWith(root.path()));
    QVERIFY(RuntimeProfile::cacheLocation().startsWith(root.path()));
    const std::unique_ptr<QSettings> settings = RuntimeProfile::createSettings();
    settings->setValue(QStringLiteral("serverUrl"), QStringLiteral("ws://localhost:8080"));
    settings->sync();
    QCOMPARE(RuntimeProfile::readSettings().value(QStringLiteral("serverUrl")).toString(),
             QStringLiteral("ws://localhost:8080"));
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
