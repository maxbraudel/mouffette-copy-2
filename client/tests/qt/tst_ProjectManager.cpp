#include <QFile>
#include <QJsonDocument>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtTest>

#include "backend/domain/project/ProjectManager.h"
#include "backend/domain/project/ProjectStore.h"

namespace {
ClientInfo client(const QString& endpointId,
                  const QString& connectionId,
                  const QString& name,
                  int volume = 50,
                  int instanceOrdinal = 1)
{
    ClientInfo result(connectionId, name, QStringLiteral("Linux"));
    result.setInstallationId(QStringLiteral("installation-") + endpointId);
    result.setEndpointId(endpointId);
    result.setInstanceId(instanceOrdinal == 1
        ? QStringLiteral("primary")
        : QStringLiteral("11111111-2222-4333-8444-555555555555"));
    result.setInstanceOrdinal(instanceOrdinal);
    result.setVolumePercent(volume);
    ScreenInfo screen(7, 1920, 1080, -120, 0, true);
    ScreenInfo::UIZone zone;
    zone.type = QStringLiteral("taskbar");
    zone.x = 0;
    zone.y = 1040;
    zone.width = 1920;
    zone.height = 40;
    screen.uiZones.append(zone);
    result.setScreens({screen});
    return result;
}

ClientSnapshot snapshot(const QString& endpointId,
                        const QString& connectionId,
                        const QString& name,
                        qint64 seenAt = 0,
                        int instanceOrdinal = 1)
{
    return ClientSnapshot::fromClientInfo(
        client(endpointId, connectionId, name, 50, instanceOrdinal), seenAt);
}
}

class ProjectManagerTest final : public QObject {
    Q_OBJECT

private slots:
    void initTestCase()
    {
        qRegisterMetaType<ProjectRecord>();
        qRegisterMetaType<ProjectLifecycleState>();
    }

    void roundTripUsesCheckpointAndStripsTransientState()
    {
        QTemporaryDir temporary;
        QVERIFY(temporary.isValid());
        ProjectStore store(temporary.filePath(QStringLiteral("projects.json")));

        ProjectManager writer(&store);
        writer.stopAutomaticTimersForTesting();
        QCOMPARE(writer.ensureProject(snapshot(QStringLiteral("device-a"),
                                               QStringLiteral("socket-1"),
                                               QStringLiteral("Studio A"),
                                               1000,
                                               2),
                                      ProjectLifecycleState::Visible,
                                      1000).isEmpty(), false);

        QJsonObject video {
            {QStringLiteral("type"), QStringLiteral("video")},
            {QStringLiteral("mediaId"), QStringLiteral("media-1")},
            {QStringLiteral("playing"), true},
            {QStringLiteral("playbackState"), QStringLiteral("playing")},
            {QStringLiteral("uploadStatus"), QStringLiteral("uploaded")},
            {QStringLiteral("remoteSessionId"), QStringLiteral("secret-session")},
            {QStringLiteral("resumeToken"), QStringLiteral("secret-token")},
            {QStringLiteral("uploadState"), QStringLiteral("uploaded")},
            {QStringLiteral("remoteFileId"), QStringLiteral("remote-file")},
            {QStringLiteral("startPositionMs"), 4210}
        };
        QJsonObject canvas {
            {QStringLiteral("canvasSessionId"), QStringLiteral("legacy-session")},
            {QStringLiteral("media"), QJsonArray{video}}
        };
        ProjectMediaReference reference;
        reference.mediaId = QStringLiteral("media-1");
        reference.assetId = QStringLiteral("asset-1");
        reference.canonicalSourcePath = QStringLiteral("/tmp/movie.mp4");
        reference.sha256 = QStringLiteral("abc123");
        reference.mediaType = QStringLiteral("video");
        QVERIFY(writer.updateCanvasState(QStringLiteral("device-a"), canvas, {reference}, 1001));
        writer.checkpointVisibleProjects(15'000);
        QVERIFY(writer.flush());

        QList<ProjectRecord> durable;
        QVERIFY(store.load(&durable));
        QCOMPARE(durable.size(), 1);
        QCOMPARE(durable.first().clientSnapshot.installationId,
                 QStringLiteral("installation-device-a"));
        QCOMPARE(durable.first().clientSnapshot.instanceOrdinal, 2);
        QCOMPARE(durable.first().clientSnapshot.toClientInfo(false).getIdentityDisplayText(),
                 QStringLiteral("(linux) Studio A — Instance 2"));
        QVERIFY(!durable.first().clientSnapshot.toJson().contains(
            QStringLiteral("serverConnectionId")));
        QVERIFY(!durable.first().canvasState.contains(QStringLiteral("canvasSessionId")));
        const QJsonObject durableVideo = durable.first().canvasState.value(QStringLiteral("media"))
                                             .toArray().first().toObject();
        QVERIFY(!durableVideo.contains(QStringLiteral("playing")));
        QVERIFY(!durableVideo.contains(QStringLiteral("uploadStatus")));
        QVERIFY(!durableVideo.contains(QStringLiteral("remoteSessionId")));
        QVERIFY(!durableVideo.contains(QStringLiteral("resumeToken")));
        QVERIFY(!durableVideo.contains(QStringLiteral("uploadState")));
        QVERIFY(!durableVideo.contains(QStringLiteral("remoteFileId")));

        ProjectManager restored(&store);
        restored.stopAutomaticTimersForTesting();
        restored.setNowProviderForTesting([] { return qint64(15'100); });
        QVERIFY(restored.load());
        const ProjectRecord* project = restored.projectForTarget(QStringLiteral("device-a"));
        QVERIFY(project);
        QCOMPARE(project->state, ProjectLifecycleState::Hidden);
        QCOMPARE(project->hiddenAtMs, qint64(15'000));
        QCOMPARE(project->mediaReferences.size(), 1);

        const QJsonObject restoredVideo = project->canvasStateForRestore()
                                              .value(QStringLiteral("media"))
                                              .toArray().first().toObject();
        QCOMPARE(restoredVideo.value(QStringLiteral("playing")).toBool(), false);
        QCOMPARE(restoredVideo.value(QStringLiteral("playbackState")).toString(),
                 QStringLiteral("paused"));
        QCOMPARE(restoredVideo.value(QStringLiteral("uploadStatus")).toString(),
                 QStringLiteral("not_uploaded"));
        QCOMPARE(restoredVideo.value(QStringLiteral("startPositionMs")).toInt(), 4210);
    }

    void rejectsTruncatedUnknownSchemaAndFractionalTimestamps()
    {
        QTemporaryDir temporary;
        QVERIFY(temporary.isValid());
        const QString path = temporary.filePath(QStringLiteral("projects.json"));
        ProjectStore store(path);
        QList<ProjectRecord> loaded;

        const auto overwrite = [&path](const QByteArray& bytes) {
            QFile file(path);
            if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) return false;
            return file.write(bytes) == bytes.size();
        };

        QVERIFY(overwrite(QByteArrayLiteral("{truncated")));
        QVERIFY(!store.load(&loaded));

        QVERIFY(overwrite(QJsonDocument(QJsonObject{
            {QStringLiteral("schemaVersion"), 99},
            {QStringLiteral("projects"), QJsonArray{}}
        }).toJson(QJsonDocument::Compact)));
        QVERIFY(!store.load(&loaded));

        ProjectManager writer(&store);
        writer.stopAutomaticTimersForTesting();
        QVERIFY(!writer.ensureProject(
            snapshot(QStringLiteral("device-a"), QStringLiteral("socket-a"),
                     QStringLiteral("Studio A")),
            ProjectLifecycleState::Hidden, 1'000).isEmpty());
        QVERIFY(writer.flush());

        QFile validFile(path);
        QVERIFY(validFile.open(QIODevice::ReadOnly));
        QJsonObject root = QJsonDocument::fromJson(validFile.readAll()).object();
        validFile.close();
        QJsonArray projects = root.value(QStringLiteral("projects")).toArray();
        QJsonObject project = projects.first().toObject();
        project.insert(QStringLiteral("createdAtMs"), 1'000.5);
        projects.replace(0, project);
        root.insert(QStringLiteral("projects"), projects);
        QVERIFY(overwrite(QJsonDocument(root).toJson(QJsonDocument::Compact)));
        QVERIFY(!store.load(&loaded));
    }

    void hiddenDeadlinesAreExactAndNeverExtended()
    {
        QTemporaryDir temporary;
        ProjectStore store(temporary.filePath(QStringLiteral("projects.json")));
        ProjectManager manager(&store);
        manager.stopAutomaticTimersForTesting();
        manager.ensureProject(snapshot(QStringLiteral("device-a"), QStringLiteral("socket-1"),
                                       QStringLiteral("Studio A")),
                              ProjectLifecycleState::Visible, 0);
        QVERIFY(manager.setHidden(QStringLiteral("device-a"), 100));
        QVERIFY(manager.setHidden(QStringLiteral("device-a"), 10'000));
        QCOMPARE(manager.remoteSessionCloseAtMs(QStringLiteral("device-a")), qint64(60'100));
        QCOMPARE(manager.projectDeleteAtMs(QStringLiteral("device-a")), qint64(300'100));

        QSignalSpy closeSpy(&manager, &ProjectManager::remoteSessionCloseDue);
        QSignalSpy aboutSpy(&manager, &ProjectManager::projectAboutToDelete);
        QSignalSpy deletedSpy(&manager, &ProjectManager::projectDeleted);
        manager.processDeadlines(60'099);
        QCOMPARE(closeSpy.count(), 0);
        manager.processDeadlines(60'100);
        QCOMPARE(closeSpy.count(), 1);
        manager.processDeadlines(200'000);
        QCOMPARE(closeSpy.count(), 1);
        QCOMPARE(manager.projectCount(), 1);
        manager.processDeadlines(300'100);
        QCOMPARE(aboutSpy.count(), 1);
        QCOMPARE(deletedSpy.count(), 1);
        QCOMPARE(manager.projectCount(), 0);

        QList<ProjectRecord> persisted;
        QVERIFY(store.load(&persisted));
        QVERIFY(persisted.isEmpty());
    }

    void cleanShutdownHideIsIdempotentAndUsesOneTimestamp()
    {
        QTemporaryDir temporary;
        ProjectStore store(temporary.filePath(QStringLiteral("projects.json")));
        ProjectManager manager(&store);
        manager.stopAutomaticTimersForTesting();
        manager.ensureProject(snapshot(QStringLiteral("device-a"),
                                       QStringLiteral("socket-a"),
                                       QStringLiteral("Studio A")),
                              ProjectLifecycleState::Visible, 1'000);
        manager.ensureProject(snapshot(QStringLiteral("device-b"),
                                       QStringLiteral("socket-b"),
                                       QStringLiteral("Studio B")),
                              ProjectLifecycleState::Visible, 2'000);

        manager.markAllHidden(10'000);
        manager.markAllHidden(42'000); // Repeated quit/hide cannot grant more time.

        const ProjectRecord* first =
            manager.projectForTarget(QStringLiteral("device-a"));
        const ProjectRecord* second =
            manager.projectForTarget(QStringLiteral("device-b"));
        QVERIFY(first);
        QVERIFY(second);
        QCOMPARE(first->state, ProjectLifecycleState::Hidden);
        QCOMPARE(second->state, ProjectLifecycleState::Hidden);
        QCOMPARE(first->hiddenAtMs, qint64(10'000));
        QCOMPARE(second->hiddenAtMs, qint64(10'000));
        QCOMPARE(manager.remoteSessionCloseAtMs(QStringLiteral("device-a")),
                 qint64(70'000));
        QCOMPARE(manager.projectDeleteAtMs(QStringLiteral("device-b")),
                 qint64(310'000));
    }

    void discoveryMergeKeepsOfflineProjectsAndRefreshesByEndpointId()
    {
        QTemporaryDir temporary;
        ProjectStore store(temporary.filePath(QStringLiteral("projects.json")));
        ProjectManager manager(&store);
        manager.stopAutomaticTimersForTesting();
        manager.ensureProject(snapshot(QStringLiteral("device-a"), QStringLiteral("socket-old"),
                                       QStringLiteral("Old name"), 1),
                              ProjectLifecycleState::Hidden, 10);

        QList<ProjectClientEntry> entries = manager.mergeDiscoveredClients({
            client(QStringLiteral("device-a"), QStringLiteral("socket-new"), QStringLiteral("New name"), 88),
            client(QStringLiteral("device-b"), QStringLiteral("socket-b"), QStringLiteral("Available B"), 20)
        }, 20);
        QCOMPARE(entries.size(), 2);
        QCOMPARE(entries.at(0).endpointId, QStringLiteral("device-a"));
        QVERIFY(entries.at(0).online);
        QVERIFY(entries.at(0).hasProject);
        QCOMPARE(entries.at(0).client.getMachineName(), QStringLiteral("New name"));
        QCOMPARE(entries.at(1).endpointId, QStringLiteral("device-b"));
        QVERIFY(entries.at(1).online);
        QVERIFY(!entries.at(1).hasProject);

        // A vanished from discovery but its project and refreshed snapshot stay.
        entries = manager.mergeDiscoveredClients({
            client(QStringLiteral("device-b"), QStringLiteral("socket-b2"), QStringLiteral("Available B"), 21)
        }, 30);
        QCOMPARE(entries.size(), 2);
        QCOMPARE(entries.at(1).endpointId, QStringLiteral("device-a"));
        QVERIFY(!entries.at(1).online);
        QVERIFY(entries.at(1).hasProject);
        QCOMPARE(entries.at(1).client.getMachineName(), QStringLiteral("New name"));
        QCOMPARE(entries.at(1).client.getVolumePercent(), 88);
        QCOMPARE(entries.at(1).client.getScreens().first().uiZones.first().type,
                 QStringLiteral("taskbar"));

        ClientInfo returnedWithoutDisplays(
            QStringLiteral("socket-empty"), QStringLiteral("New name"),
            QStringLiteral("Linux"));
        returnedWithoutDisplays.setEndpointId(QStringLiteral("device-a"));
        returnedWithoutDisplays.setScreens({});
        returnedWithoutDisplays.setVolumePercent(-1);
        entries = manager.mergeDiscoveredClients({returnedWithoutDisplays}, 31);
        QCOMPARE(entries.size(), 1); // B was discovery-only, so it vanishes.
        QCOMPARE(entries.first().endpointId, QStringLiteral("device-a"));
        QVERIFY(entries.first().client.getScreens().isEmpty());
        QCOMPARE(entries.first().client.getVolumePercent(), -1);
        const ProjectRecord* replaced = manager.projectForTarget(QStringLiteral("device-a"));
        QVERIFY(replaced);
        QVERIFY(replaced->clientSnapshot.screens.isEmpty());
        QCOMPARE(replaced->clientSnapshot.volumePercent, -1);

        entries = manager.mergeDiscoveredClients({}, 32);
        QCOMPARE(entries.size(), 1);
        QVERIFY(!entries.first().online);
        QVERIFY(entries.first().client.getScreens().isEmpty());

        // Deleting a Project does not hide an authenticated device that is
        // still online: it becomes a plain Available discovery row.
        QVERIFY(manager.deleteProject(QStringLiteral("device-a")));
        entries = manager.mergeDiscoveredClients({
            client(QStringLiteral("device-a"), QStringLiteral("socket-a3"), QStringLiteral("New name"), 89),
            client(QStringLiteral("device-b"), QStringLiteral("socket-b3"), QStringLiteral("Available B"))
        }, 35);
        QCOMPARE(entries.size(), 2);
        QCOMPARE(entries.first().endpointId, QStringLiteral("device-a"));
        QVERIFY(entries.first().online);
        QVERIFY(!entries.first().hasProject);
        QCOMPARE(entries.first().client.availabilityBadgeText(), QStringLiteral("Available"));

        // Names are display metadata only. A different authenticated identity
        // with the same name cannot revive or absorb the deleted Project.
        entries = manager.mergeDiscoveredClients({
            client(QStringLiteral("device-c"), QStringLiteral("socket-c"), QStringLiteral("New name"))
        }, 36);
        QCOMPARE(entries.size(), 1);
        QCOMPARE(entries.first().endpointId, QStringLiteral("device-c"));
        QVERIFY(!entries.first().hasProject);

        // Recreate the offline branch to prove deletion removes the row when
        // no authenticated discovery entry remains.
        manager.ensureProject(snapshot(QStringLiteral("device-a"), QStringLiteral("socket-a4"),
                                       QStringLiteral("New name"), 37),
                              ProjectLifecycleState::Hidden, 37);
        QVERIFY(manager.deleteProject(QStringLiteral("device-a")));
        entries = manager.mergeDiscoveredClients({
            client(QStringLiteral("device-b"), QStringLiteral("socket-b4"), QStringLiteral("Available B"))
        }, 40);
        QCOMPARE(entries.size(), 1);
        QCOMPARE(entries.first().endpointId, QStringLiteral("device-b"));
    }

    void crashCheckpointDoesNotGrantFreshRetentionWindow()
    {
        QTemporaryDir temporary;
        ProjectStore store(temporary.filePath(QStringLiteral("projects.json")));
        ProjectManager writer(&store);
        writer.stopAutomaticTimersForTesting();
        writer.ensureProject(snapshot(QStringLiteral("device-a"), QStringLiteral("socket-a"),
                                       QStringLiteral("Studio A")),
                             ProjectLifecycleState::Visible, 0);
        writer.checkpointVisibleProjects(15'000);
        QVERIFY(writer.flush());

        ProjectManager reader(&store);
        reader.stopAutomaticTimersForTesting();
        reader.setNowProviderForTesting([] { return qint64(315'000); });
        QVERIFY(reader.load());
        QCOMPARE(reader.projectCount(), 0); // Exactly checkpoint + 5 minutes.
    }

    void restoreBoundariesAt59Seconds299SecondsAnd300Seconds()
    {
        QTemporaryDir temporary;
        ProjectStore store(temporary.filePath(QStringLiteral("projects.json")));
        ProjectManager manager(&store);
        manager.stopAutomaticTimersForTesting();

        manager.ensureProject(snapshot(QStringLiteral("device-59"),
                                       QStringLiteral("socket-59"),
                                       QStringLiteral("Studio 59")),
                              ProjectLifecycleState::Visible, 0);
        QVERIFY(manager.setHidden(QStringLiteral("device-59"), 0));
        QSignalSpy closeSpy(&manager, &ProjectManager::remoteSessionCloseDue);
        QSignalSpy restoredSpy(
            &manager, &ProjectManager::projectRestoredAfterSessionDeadline);
        QVERIFY(manager.setVisible(QStringLiteral("device-59"), 59'999));
        QCOMPARE(closeSpy.count(), 0);
        QCOMPARE(restoredSpy.count(), 0);

        manager.ensureProject(snapshot(QStringLiteral("device-60"),
                                       QStringLiteral("socket-60"),
                                       QStringLiteral("Studio 60")),
                              ProjectLifecycleState::Visible, 0);
        QVERIFY(manager.setHidden(QStringLiteral("device-60"), 0));
        QVERIFY(manager.setVisible(QStringLiteral("device-60"), 60'000));
        QCOMPARE(closeSpy.count(), 1);
        QCOMPARE(closeSpy.first().at(1).toString(), QStringLiteral("device-60"));
        QCOMPARE(restoredSpy.count(), 1);
        QCOMPARE(restoredSpy.first().at(1).toString(),
                 QStringLiteral("device-60"));

        manager.ensureProject(snapshot(QStringLiteral("device-299"),
                                       QStringLiteral("socket-299"),
                                       QStringLiteral("Studio 299")),
                              ProjectLifecycleState::Visible, 0);
        QVERIFY(manager.setHidden(QStringLiteral("device-299"), 0));
        QVERIFY(manager.setVisible(QStringLiteral("device-299"), 299'999));
        QVERIFY(manager.hasProjectForTarget(QStringLiteral("device-299")));
        QCOMPARE(restoredSpy.count(), 2);
        QCOMPARE(restoredSpy.last().at(1).toString(),
                 QStringLiteral("device-299"));

        manager.ensureProject(snapshot(QStringLiteral("device-300"),
                                       QStringLiteral("socket-300"),
                                       QStringLiteral("Studio 300")),
                              ProjectLifecycleState::Visible, 0);
        QVERIFY(manager.setHidden(QStringLiteral("device-300"), 0));
        QVERIFY(!manager.setVisible(QStringLiteral("device-300"), 300'000));
        QVERIFY(!manager.hasProjectForTarget(QStringLiteral("device-300")));
    }

    void failedAtomicDeleteKeepsExactProjectAndRetriesDirtyAutosave()
    {
        QTemporaryDir temporary;
        QVERIFY(temporary.isValid());

        const QString storeDirectoryName = QStringLiteral("project-store");
        const QString backupDirectoryName = QStringLiteral("project-store-backup");
        const QString storeDirectory = temporary.filePath(storeDirectoryName);
        QVERIFY(QDir().mkpath(storeDirectory));

        ProjectStore store(QDir(storeDirectory).filePath(QStringLiteral("projects.json")));
        ProjectManager::TimingPolicy timing;
        timing.autosaveDelayMs = 25;
        timing.checkpointIntervalMs = 60'000;
        timing.deadlinePollIntervalMs = 60'000;
        ProjectManager manager(&store, timing);

        const QString projectId = manager.ensureProject(
            snapshot(QStringLiteral("device-a"), QStringLiteral("socket-a"),
                     QStringLiteral("Studio A")),
            ProjectLifecycleState::Visible, 1'000);
        QVERIFY(!projectId.isEmpty());
        QVERIFY(manager.flush());

        // Leave a real unsaved change behind so the retry must preserve and
        // eventually persist it even though the requested deletion failed.
        const QJsonObject canvasState {
            {QStringLiteral("viewport"), QJsonObject {
                 {QStringLiteral("x"), 42},
                 {QStringLiteral("y"), -7}
             }}
        };
        QVERIFY(manager.updateCanvasState(QStringLiteral("device-a"),
                                          canvasState, {}, 1'100));
        const ProjectRecord beforeFailure =
            *manager.projectForTarget(QStringLiteral("device-a"));

        // Move the valid directory aside and replace it with a regular file.
        // The configured QSaveFile parent can then neither exist as a
        // directory nor be created, which is deterministic even as root.
        QDir root(temporary.path());
        QVERIFY(root.rename(storeDirectoryName, backupDirectoryName));
        QFile blocker(storeDirectory);
        QVERIFY(blocker.open(QIODevice::WriteOnly));
        QCOMPARE(blocker.write("blocked"), qint64(7));
        blocker.close();

        QSignalSpy errorSpy(&manager, &ProjectManager::persistenceError);
        QSignalSpy aboutSpy(&manager, &ProjectManager::projectAboutToDelete);
        QSignalSpy deletedSpy(&manager, &ProjectManager::projectDeleted);
        QSignalSpy changedSpy(&manager, &ProjectManager::projectsChanged);

        QVERIFY(!manager.deleteProject(QStringLiteral("device-a")));
        QCOMPARE(aboutSpy.count(), 1);
        QCOMPARE(deletedSpy.count(), 0);
        QCOMPARE(changedSpy.count(), 0);
        QCOMPARE(errorSpy.count(), 1);
        QVERIFY(!manager.lastError().isEmpty());
        QCOMPARE(manager.projectCount(), 1);
        QVERIFY(manager.hasProjectForTarget(QStringLiteral("device-a")));
        const ProjectRecord* retained =
            manager.projectForTarget(QStringLiteral("device-a"));
        QVERIFY(retained);
        QCOMPARE(retained->toJson(), beforeFailure.toJson());
        QCOMPARE(manager.projectById(projectId)->targetEndpointId,
                 QStringLiteral("device-a"));

        // Repair storage. The save failure must have rearmed the delayed
        // autosave, which persists the pre-existing canvas update while the
        // failed deletion remains cancelled.
        QVERIFY(QFile::remove(storeDirectory));
        QVERIFY(root.rename(backupDirectoryName, storeDirectoryName));
        QTRY_VERIFY_WITH_TIMEOUT(manager.lastError().isEmpty(), 2'000);

        QList<ProjectRecord> persisted;
        QVERIFY(store.load(&persisted));
        QCOMPARE(persisted.size(), 1);
        QCOMPARE(persisted.first().projectId, projectId);
        QCOMPARE(persisted.first().canvasState, beforeFailure.canvasState);

        QVERIFY(manager.deleteProject(QStringLiteral("device-a")));
        QCOMPARE(deletedSpy.count(), 1);
        QCOMPARE(manager.projectCount(), 0);
        QVERIFY(store.load(&persisted));
        QVERIFY(persisted.isEmpty());
    }
};

QTEST_GUILESS_MAIN(ProjectManagerTest)
#include "tst_ProjectManager.moc"
