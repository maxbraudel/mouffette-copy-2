#include "backend/domain/project/ProjectScreenPreviewStore.h"

#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtTest>

namespace {
QVideoFrame frame(Qt::GlobalColor color)
{
    QImage image(96, 64, QImage::Format_RGBA8888);
    image.fill(color);
    return QVideoFrame(image);
}

QColor color(const QList<QVariant>& event)
{
    return qvariant_cast<QImage>(event.at(2)).pixelColor(20, 20);
}

QStringList snapshots(const QString& directory)
{
    QStringList result;
    QDirIterator iterator(directory, {QStringLiteral("*.png")}, QDir::Files,
                          QDirIterator::Subdirectories);
    while (iterator.hasNext()) result.append(iterator.next());
    return result;
}
}

class ProjectScreenPreviewStoreTest final : public QObject {
    Q_OBJECT
private slots:
    void latestFramesSurviveRestartAndRemainIsolated()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        {
            ProjectScreenPreviewStore store(directory.path());
            store.retain("project-a", 0, frame(Qt::red));
            store.retain("project-a", 0, frame(Qt::green));
            store.retain("project-a", 3, frame(Qt::blue));
            store.retain("project-b", 0, frame(Qt::yellow));
            store.flush();
            // A new frame while the worker is busy must win at shutdown too.
            store.retain("project-a", 0, frame(Qt::cyan));
        }
        QCOMPARE(snapshots(directory.path()).size(), 3);
        ProjectScreenPreviewStore reader(directory.path());
        QSignalSpy restored(&reader, &ProjectScreenPreviewStore::frameRestored);
        reader.restore("project-a", {0, 3, 9});
        reader.restore("project-b", {0});
        reader.waitForDone();
        QTRY_COMPARE(restored.count(), 3);
        QHash<QString, QColor> colors;
        for (const auto& event : restored)
            colors.insert(event.at(0).toString() + QString::number(event.at(1).toInt()), color(event));
        QCOMPARE(colors.value("project-a0"), QColor(Qt::cyan));
        QCOMPARE(colors.value("project-a3"), QColor(Qt::blue));
        QCOMPARE(colors.value("project-b0"), QColor(Qt::yellow));
    }

    void restoreUsesPendingLatestAndDoesNotOverwriteANewerLiveFrame()
    {
        QTemporaryDir directory;
        ProjectScreenPreviewStore store(directory.path());
        QSignalSpy restored(&store, &ProjectScreenPreviewStore::frameRestored);
        store.retain("project", 0, frame(Qt::red));
        store.waitForDone();
        store.retain("project", 0, frame(Qt::green));
        store.restore("project", {0, 0});
        store.restore("project", {0}); // Coalesce duplicate pending reads.
        store.waitForDone();
        QTRY_COMPARE(restored.count(), 1);
        QCOMPARE(color(restored.first()), QColor(Qt::green));
        restored.clear();

        store.restore("project", {0});
        store.waitForDone(); // Completion is queued on the GUI thread.
        store.retain("project", 0, frame(Qt::blue));
        QCoreApplication::processEvents();
        QVERIFY(restored.isEmpty());
        store.restore("project", {0});
        store.waitForDone();
        QTRY_COMPARE(restored.count(), 1);
        QCOMPARE(color(restored.first()), QColor(Qt::blue));
    }

    void clearFencesWritesRestoresAndAnImmediateRestart()
    {
        QTemporaryDir directory;
        ProjectScreenPreviewStore store(directory.path());
        QSignalSpy restored(&store, &ProjectScreenPreviewStore::frameRestored);
        store.retain("project", 0, frame(Qt::red));
        store.waitForDone();
        store.restore("project", {0});
        store.waitForDone();
        store.retain("project", 0, frame(Qt::green));
        store.flush();
        QVERIFY(store.clearAll());
        // The manifest invalidates old pixels before deferred cleanup runs.
        ProjectScreenPreviewStore restarted(directory.path());
        QSignalSpy afterRestart(&restarted, &ProjectScreenPreviewStore::frameRestored);
        restarted.restore("project", {0});
        restarted.waitForDone();
        store.waitForDone();
        QCoreApplication::processEvents();
        QVERIFY(restored.isEmpty());
        QVERIFY(afterRestart.isEmpty());
        QVERIFY(snapshots(directory.path()).isEmpty());

        // Showing the preview does not resurrect anything. A fresh stream can.
        store.restore("project", {0});
        store.waitForDone();
        QCoreApplication::processEvents();
        QVERIFY(restored.isEmpty());
        store.retain("project", 0, frame(Qt::blue));
        store.restore("project", {0});
        store.waitForDone();
        QTRY_COMPARE(restored.count(), 1);
        QCOMPARE(color(restored.first()), QColor(Qt::blue));
        QCOMPARE(snapshots(directory.path()).size(), 1);
    }

    void removingAndPruningProjectsFenceQueuedWork()
    {
        QTemporaryDir directory;
        {
            ProjectScreenPreviewStore store(directory.path());
            QSignalSpy restored(&store, &ProjectScreenPreviewStore::frameRestored);
            store.retain("remove", 0, frame(Qt::red));
            store.retain("prune", 0, frame(Qt::green));
            store.retain("keep", 2, frame(Qt::blue));
            store.flush();
            store.restore("remove", {0});
            store.removeProject("remove");
            store.restore("prune", {0});
            store.pruneProjects({"keep"});
            store.waitForDone();
            QCoreApplication::processEvents();
            QVERIFY(restored.isEmpty());
        }
        QCOMPARE(snapshots(directory.path()).size(), 1);
        ProjectScreenPreviewStore restarted(directory.path());
        QSignalSpy restored(&restarted, &ProjectScreenPreviewStore::frameRestored);
        restarted.restore("remove", {0});
        restarted.restore("prune", {0});
        restarted.restore("keep", {2});
        restarted.waitForDone();
        QTRY_COMPARE(restored.count(), 1);
        QCOMPARE(restored.first().at(0).toString(), QStringLiteral("keep"));
        QCOMPARE(color(restored.first()), QColor(Qt::blue));
    }

    void diskOnlyRestoreIsCancelledByPruning()
    {
        QTemporaryDir directory;
        {
            ProjectScreenPreviewStore writer(directory.path());
            writer.retain("prune", 0, frame(Qt::red));
        }
        ProjectScreenPreviewStore store(directory.path());
        QSignalSpy restored(&store, &ProjectScreenPreviewStore::frameRestored);
        store.restore("prune", {0});
        store.waitForDone();
        store.pruneProjects({});
        store.waitForDone();
        QCoreApplication::processEvents();
        QVERIFY(restored.isEmpty());
        QVERIFY(snapshots(directory.path()).isEmpty());
    }

    void startupPrunesUnreachableGenerationsAfterInterruptedCleanup()
    {
        QTemporaryDir directory;
        const QString abandoned = directory.filePath("36f34ee1-f204-4f1a-8c95-8e82cd0ba351");
        QVERIFY(QDir().mkpath(abandoned));
        QFile orphan(QDir(abandoned).filePath("orphan.png"));
        QVERIFY(orphan.open(QIODevice::WriteOnly));
        QVERIFY(orphan.write("obsolete") > 0);
        orphan.close();
        ProjectScreenPreviewStore store(directory.path());
        store.retain("keep", 0, frame(Qt::green));
        store.waitForDone();
        store.pruneProjects({"keep"});
        store.waitForDone();
        QVERIFY(!QFileInfo::exists(abandoned));
        QCOMPARE(snapshots(directory.path()).size(), 1);
    }

    void invalidInputsAndCorruptFilesAreIgnored()
    {
        QTemporaryDir directory;
        ProjectScreenPreviewStore store(directory.path());
        const auto pixels = frame(Qt::red);
        store.retain({}, 0, pixels);
        store.retain("   ", 0, pixels);
        store.retain(QString(4097, QLatin1Char('a')), 0, pixels);
        store.retain("project", -1, pixels);
        store.retain("project", 1000001, pixels);
        store.retain("project", 0, {});
        store.waitForDone();
        QVERIFY(snapshots(directory.path()).isEmpty());
        // Arbitrary project identifiers cannot become filesystem paths.
        store.retain("../../outside", 0, pixels);
        store.waitForDone();
        const auto files = snapshots(directory.path());
        QCOMPARE(files.size(), 1);
        QVERIFY(files.first().startsWith(directory.path() + QLatin1Char('/')));
        QVERIFY(!files.first().contains("outside"));
        QFile corrupt(files.first());
        QVERIFY(corrupt.open(QIODevice::WriteOnly | QIODevice::Truncate));
        QCOMPARE(corrupt.write("invalid PNG"), 11);
        corrupt.close();
        QSignalSpy restored(&store, &ProjectScreenPreviewStore::frameRestored);
        store.restore("../../outside", {0, -1, 1000001});
        store.restore("missing", {0});
        store.waitForDone();
        QCoreApplication::processEvents();
        QVERIFY(restored.isEmpty());
    }

    void failedDurableClearDisablesRestorationUntilRetry()
    {
        QTemporaryDir directory;
        ProjectScreenPreviewStore store(directory.path());
        store.retain("project", 0, frame(Qt::red));
        store.waitForDone();
        QSignalSpy errors(&store, &ProjectScreenPreviewStore::persistenceError);
        QSignalSpy restored(&store, &ProjectScreenPreviewStore::frameRestored);
        const QString manifest = directory.filePath("current.json");
        QVERIFY(QFile::remove(manifest));
        QVERIFY(QDir().mkpath(manifest));
        QVERIFY(!store.clearAll());
        QCOMPARE(errors.count(), 1);
        store.restore("project", {0});
        store.retain("project", 0, frame(Qt::blue));
        store.waitForDone();
        QCoreApplication::processEvents();
        QVERIFY(restored.isEmpty());
        QVERIFY(QDir(manifest).removeRecursively());
        QVERIFY(store.clearAll());
        store.waitForDone();
        QVERIFY(snapshots(directory.path()).isEmpty());
    }

    void symlinkedProjectDirectoryIsNotReadOrTraversed()
    {
#ifdef Q_OS_WIN
        QSKIP("Creating directory symlinks requires additional Windows privileges.");
#else
        QTemporaryDir directory;
        QTemporaryDir outside;
        QString projectDirectory;
        {
            ProjectScreenPreviewStore store(directory.path());
            store.retain("project", 0, frame(Qt::red));
            store.waitForDone();
            const auto files = snapshots(directory.path());
            QCOMPARE(files.size(), 1);
            projectDirectory = QFileInfo(files.first()).absolutePath();
            QVERIFY(QFile::copy(files.first(), outside.filePath("0.png")));
            QVERIFY(QDir(projectDirectory).removeRecursively());
        }
        QVERIFY(QFile::link(outside.path(), projectDirectory));
        ProjectScreenPreviewStore store(directory.path());
        QSignalSpy restored(&store, &ProjectScreenPreviewStore::frameRestored);
        QSignalSpy errors(&store, &ProjectScreenPreviewStore::persistenceError);
        store.restore("project", {0});
        store.waitForDone();
        QCoreApplication::processEvents();
        QVERIFY(restored.isEmpty());
        store.retain("project", 0, frame(Qt::blue));
        store.waitForDone();
        QTRY_VERIFY(!errors.isEmpty());
        QCOMPARE(QImage(outside.filePath("0.png")).pixelColor(20, 20), QColor(Qt::red));
        store.pruneProjects({});
        store.waitForDone();
        QVERIFY(QFileInfo::exists(outside.filePath("0.png")));
        QVERIFY(!QFileInfo(projectDirectory).isSymLink());
#endif
    }
};

QTEST_GUILESS_MAIN(ProjectScreenPreviewStoreTest)
#include "tst_ProjectScreenPreviewStore.moc"
