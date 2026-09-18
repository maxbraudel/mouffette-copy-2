#include <QFile>
#include <QJsonDocument>
#include <QSignalSpy>
#include <QScopeGuard>
#include <QTemporaryDir>
#include <QtTest>

#include "backend/domain/project/ProjectManager.h"
#include "backend/domain/project/ProjectStore.h"
#include "backend/domain/session/IncomingSessionOrphanWatchdog.h"
#include "backend/domain/workspace/WorkspaceManager.h"
#include "backend/config/AppConfig.h"
#include "backend/domain/scene/SceneTimeline.h"

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
        : QStringLiteral("instance-%1").arg(instanceOrdinal));
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

ProjectTargetReference target(const QString& endpointId,
                              const QString& name)
{
    return ProjectTargetReference::fromClientInfo(
        client(endpointId, QStringLiteral("ignored-connection"), name));
}

QString createProject(ProjectManager& manager,
                      const ProjectTargetReference& reference,
                      ProjectLifecycleState state,
                      qint64 nowMs)
{
    const QString projectId = manager.createProjectFromSnapshot(
        reference, {}, 50, 1, qMax<qint64>(1, nowMs), nowMs);
    if (!projectId.isEmpty() && state == ProjectLifecycleState::Hidden) {
        if (!manager.setHidden(reference.endpointId, nowMs)) return {};
    }
    return projectId;
}
}

class ProjectManagerTest final : public QObject {
    Q_OBJECT

private slots:
    void newProjectPersistsItsTimelineBeforeTheFirstCheckpoint()
    {
        QTemporaryDir directory;
        auto& config=AppConfig::instance();
        const AppConfig previous=config;
        const auto restoreConfig=qScopeGuard([&]{config=previous;});
        AppConfig::LoadOptions options;
        options.defaultEnvFilePath=directory.filePath(QStringLiteral("timeline.env"));
        QFile env(options.defaultEnvFilePath);QVERIFY(env.open(QIODevice::WriteOnly));
        env.close();
        options.processEnvironment.insert(QStringLiteral("MOUFFETTE_TIMELINE_MAX_DURATION_MS"),QStringLiteral("300000"));
        options.processEnvironment.insert(QStringLiteral("MOUFFETTE_TIMELINE_SLOTS_PER_SECOND"),QStringLiteral("60"));
        QString error;QVERIFY2(config.load(options,&error),qPrintable(error));
        ProjectStore store(directory.filePath(QStringLiteral("projects.json")));
        ProjectManager writer(&store);writer.stopAutomaticTimersForTesting();
        const QString endpoint=QStringLiteral("timeline-project");
        QVERIFY(!createProject(writer,target(endpoint,QStringLiteral("Timeline")),ProjectLifecycleState::Visible,1000).isEmpty());
        QList<ProjectRecord> durable;QVERIFY(store.load(&durable));QCOMPARE(durable.size(),1);
        const auto initial=durable.first().canvasStateForRestore();
        QCOMPARE(initial.value("renderSchemaVersion").toInt(),SceneTimeline::RenderSchemaVersion);
        QVERIFY(initial.value("media").isArray());QVERIFY(initial.value("media").toArray().isEmpty());
        SceneTimeline::SceneSettings settings;
        QVERIFY(SceneTimeline::SceneSettings::fromJson(initial.value("timeline").toObject(),&settings));
        QCOMPARE(settings.maxDurationMs,300000);QCOMPARE(settings.stopSlot,-1);QCOMPARE(settings.slotsPerSecond,60);

        options.processEnvironment.insert(QStringLiteral("MOUFFETTE_TIMELINE_MAX_DURATION_MS"),QStringLiteral("90000"));
        options.processEnvironment.insert(QStringLiteral("MOUFFETTE_TIMELINE_SLOTS_PER_SECOND"),QStringLiteral("24"));
        QVERIFY2(config.load(options,&error),qPrintable(error));
        ProjectManager reader(&store);reader.stopAutomaticTimersForTesting();
        reader.setNowProviderForTesting([]{return qint64(1001);});QVERIFY(reader.load());
        const auto* restored=reader.projectForTarget(endpoint);QVERIFY(restored);
        QCOMPARE(restored->canvasState.value("timeline").toObject().value("maxDurationMs").toInt(),300000);
        QCOMPARE(restored->canvasState.value("timeline").toObject().value("slotsPerSecond").toInt(),60);
        QVERIFY(!createProject(reader,target(QStringLiteral("new-project"),QStringLiteral("New")),ProjectLifecycleState::Visible,1002).isEmpty());
        QCOMPARE(reader.projectForTarget(QStringLiteral("new-project"))->canvasState.value("timeline").toObject().value("maxDurationMs").toInt(),90000);
        QCOMPARE(reader.projectForTarget(QStringLiteral("new-project"))->canvasState.value("timeline").toObject().value("slotsPerSecond").toInt(),24);
    }

    void unavailableDiscoveryRequiresAnExistingProject_data()
    {
        QTest::addColumn<QString>("status");
        QTest::newRow("degraded") << QStringLiteral("Degraded");
        QTest::newRow("reconnecting") << QStringLiteral("Reconnecting");
        QTest::newRow("disconnected") << QStringLiteral("Disconnected");
    }

    void unavailableDiscoveryRequiresAnExistingProject()
    {
        QFETCH(QString, status);
        QTemporaryDir directory;
        ProjectStore store(directory.filePath("projects.json"));
        ProjectManager manager(&store);
        manager.stopAutomaticTimersForTesting();
        ClientInfo first = client("endpoint-1", "transport-1", "Same host", 50, 1);
        ClientInfo second = client("endpoint-2", "transport-2", "Same host", 70, 2);
        QCOMPARE(manager.mergeDiscoveredClients({first, second}, 100).size(), 2);
        first.setStatus(status);
        first.setAvailabilityStatus(status);
        first.setCanAcceptSession(false);
        first.setOnline(status != QLatin1String("Disconnected"));
        auto entries = manager.mergeDiscoveredClients({first, second}, 101);
        QCOMPARE(entries.size(), 1);
        QCOMPARE(entries.first().endpointId, second.endpointId());
        QCOMPARE(manager.projectCount(), 0);

        // Even an empty project retains its authenticated screen snapshot.
        const QList<ScreenInfo> screens = first.getScreens();
        QVERIFY(!manager.createProjectFromSnapshot(
            ProjectTargetReference::fromClientInfo(first), screens, 63, 1, 100, 100).isEmpty());
        first.setScreens({});
        first.setVolumePercent(-1);
        entries = manager.mergeDiscoveredClients({first, second}, 102);
        QCOMPARE(entries.size(), 2);
        QVERIFY(entries.first().hasProject);
        QCOMPARE(entries.first().client.availabilityBadgeText(), status == QLatin1String("Reconnecting") ? QStringLiteral("Disconnected") : status);
        QCOMPARE(entries.first().client.getScreens().size(), screens.size());
        QCOMPARE(entries.first().client.getVolumePercent(), 63);

        // Local loss hides discovery-only rows but never discards a project.
        entries = manager.mergeDiscoveredClients({first, second}, 103, false);
        QCOMPARE(entries.size(), 1);
        QCOMPARE(entries.first().endpointId, first.endpointId());
        QVERIFY(manager.deleteProject(first.endpointId()));
        QVERIFY(manager.mergeDiscoveredClients({first, second}, 104, false).isEmpty());
        QCOMPARE(manager.mergeDiscoveredClients({first, second}, 105).size(), 1);

        first.setOnline(true);
        first.setCanAcceptSession(true);
        // Session presentation is not the discovery admission criterion.
        first.setAvailabilityStatus(QStringLiteral("Connecting"));
        entries = manager.mergeDiscoveredClients({first, second}, 106);
        QCOMPARE(entries.size(), 2);
        QCOMPARE(manager.projectCount(), 0);
    }

    void expiredProjectCannotKeepRetainedOfflinePresenceVisible()
    {
        QTemporaryDir directory;
        ProjectStore store(directory.filePath("projects.json"));
        ProjectManager::TimingPolicy timing;
        timing.projectHiddenRetentionMs = 100;
        timing.projectMediaHiddenTimeoutMs = 50;
        ProjectManager manager(&store, timing);
        manager.stopAutomaticTimersForTesting();
        ClientInfo peer = client("endpoint-1", "transport", "Host");
        QVERIFY(!createProject(manager, ProjectTargetReference::fromClientInfo(peer),
                               ProjectLifecycleState::Hidden, 100).isEmpty());
        peer.setOnline(false);
        peer.setStatus(QStringLiteral("Disconnected"));
        peer.setAvailabilityStatus(QStringLiteral("Disconnected"));
        QCOMPARE(manager.mergeDiscoveredClients({peer}, 199).size(), 1);
        manager.processDeadlines(200);
        QCOMPARE(manager.projectCount(), 0);
        QVERIFY(manager.mergeDiscoveredClients({peer}, 200).isEmpty());
    }

    void legacyProjectIdentityRemainsUnknownUntilPresenceThenSurvivesRestart()
    {
        const QJsonObject legacyJson{{"endpointId", "device-secondary"},
            {"machineName", "Studio"}, {"platform", "Linux"}};
        ProjectTargetReference legacy;
        QVERIFY(ProjectTargetReference::fromJson(legacyJson, &legacy));
        QCOMPARE(legacy.instanceOrdinal, 0);
        QCOMPARE(legacy.toClientInfo(false).getInstanceDisplayName(), QStringLiteral("Studio"));
        QCOMPARE(legacy.toJson(), legacyJson);

        QTemporaryDir directory;
        ProjectStore store(directory.filePath(QStringLiteral("projects.json")));
        ProjectManager writer(&store);
        writer.stopAutomaticTimersForTesting();
        QVERIFY(!createProject(writer, legacy, ProjectLifecycleState::Hidden, 1000).isEmpty());
        ClientInfo discovered = client("device-secondary", "connection-1", "Studio", 50, 3);
        discovered.setRuntimeId(QStringLiteral("123e4567-e89b-42d3-a456-426614174000"));
        const auto entries = writer.mergeDiscoveredClients({discovered}, 1001);
        QCOMPARE(entries.first().client.getInstanceDisplayName(), QStringLiteral("Studio (3)"));
        QVERIFY(writer.flush());
        const auto saved = writer.projectForTarget("device-secondary")->target.toJson();
        QCOMPARE(saved.value("instanceOrdinal").toInt(), 3);
        QCOMPARE(saved.value("instanceId").toString(), QStringLiteral("instance-3"));
        QVERIFY(!saved.contains("runtimeId"));

        ProjectManager reader(&store);
        reader.stopAutomaticTimersForTesting();
        reader.setNowProviderForTesting([] { return qint64(1002); });
        QVERIFY(reader.load());
        auto offline = reader.mergeDiscoveredClients({}, 1002);
        QCOMPARE(offline.size(), 1);
        QVERIFY(!offline.first().client.isOnline());
        QVERIFY(offline.first().client.runtimeId().isEmpty());
        QCOMPARE(offline.first().client.getInstanceDisplayName(), QStringLiteral("Studio (3)"));
        QCOMPARE(offline.first().client.installationId(), discovered.installationId());

        ClientInfo metadataOnly("device-secondary", "Renamed", "Linux");
        const auto refreshed = reader.mergeDiscoveredClients({metadataOnly}, 1003);
        QCOMPARE(refreshed.first().client.getInstanceDisplayName(), QStringLiteral("Renamed (3)"));
        offline = reader.mergeDiscoveredClients({}, 1004);
        QCOMPARE(offline.first().client.instanceId(), QStringLiteral("instance-3"));
    }

    void malformedOptionalIdentityDoesNotDiscardLegacyProject()
    {
        QJsonObject json{{"endpointId", "device"}, {"machineName", "Studio"},
            {"platform", "Linux"}, {"installationId", "installation"},
            {"instanceId", "instance-3"}, {"instanceOrdinal", 3.5}};
        ProjectTargetReference target;
        QVERIFY(ProjectTargetReference::fromJson(json, &target));
        QCOMPARE(target.instanceOrdinal, 0);
        QCOMPARE(target.toClientInfo(false).getInstanceDisplayName(), QStringLiteral("Studio"));
        json.insert("instanceOrdinal", 2147483648.0);
        QVERIFY(ProjectTargetReference::fromJson(json, &target));
        QCOMPARE(target.instanceOrdinal, 0);
        json.insert("instanceOrdinal", 3);
        json.insert("instanceId", "instance-03");
        QVERIFY(ProjectTargetReference::fromJson(json, &target));
        QCOMPARE(target.instanceOrdinal, 0);
    }

    void initTestCase()
    {
        qRegisterMetaType<ProjectRecord>();
        qRegisterMetaType<ProjectLifecycleState>();
    }

    void snapshotFreshnessDoesNotEditProject()
    {
        QTemporaryDir directory;
        ProjectStore store(directory.filePath(QStringLiteral("snapshots.json")));
        ProjectManager manager(&store);
        manager.stopAutomaticTimersForTesting();
        const auto reference = target(QStringLiteral("device"), QStringLiteral("Studio"));
        QVERIFY(!createProject(manager, reference, ProjectLifecycleState::Visible, 1000).isEmpty());
        QSignalSpy updates(&manager, &ProjectManager::projectUpdated);
        QVERIFY(manager.updateRemoteSnapshot(reference.endpointId, {}, 50, 2, 2000, 2000));
        QCOMPARE(updates.size(), 0);
        QCOMPARE(manager.projectForTarget(reference.endpointId)->updatedAtMs, qint64(1000));
        QCOMPARE(manager.projectForTarget(reference.endpointId)->snapshotRevision, quint64(2));
        QVERIFY(manager.updateRemoteSnapshot(reference.endpointId,
            {ScreenInfo(0, 1920, 1080, 0, 0, true)}, 50, 3, 3000, 3000));
        QCOMPARE(updates.size(), 1);
        QVERIFY(manager.updateRemoteSnapshot(reference.endpointId, {}, 50, 4, 4000, 4000));
        QCOMPARE(updates.size(), 2);
        QVERIFY(manager.projectForTarget(reference.endpointId)->savedScreens.isEmpty());
    }

    void pendingImportSurvivesCheckpointWithoutInventingContentIdentity()
    {
        QTemporaryDir directory;
        ProjectStore store(directory.filePath(QStringLiteral("pending.json")));
        ProjectManager writer(&store);
        writer.stopAutomaticTimersForTesting();
        QVERIFY(!createProject(writer, target(QStringLiteral("device-pending"), QStringLiteral("Studio")),
                               ProjectLifecycleState::Visible, 1000).isEmpty());
        ProjectMediaReference source;
        source.mediaId = QStringLiteral("pending-image");
        source.canonicalSourcePath = directory.filePath(QStringLiteral("image.png"));
        source.sourceIdentity = QStringLiteral("size:mtime");
        source.mediaType = QStringLiteral("image");
        source.pendingImport = true;
        QJsonObject canvas{{QStringLiteral("renderSchemaVersion"), 4},
            {QStringLiteral("media"), QJsonArray{QJsonObject{
                {QStringLiteral("mediaId"), source.mediaId}, {QStringLiteral("type"), source.mediaType},
                {QStringLiteral("fileId"), QString()}, {QStringLiteral("baseWidth"), 640},
                {QStringLiteral("baseHeight"), 480}}}}};
        QVERIFY(writer.updateCanvasState(QStringLiteral("device-pending"), canvas, {source}, {}, 1001));
        writer.checkpointVisibleProjects(1002);
        QVERIFY(writer.flush());
        ProjectManager reader(&store);
        reader.stopAutomaticTimersForTesting();
        reader.setNowProviderForTesting([] { return qint64(1003); });
        QVERIFY(reader.load());
        const auto* restored = reader.projectForTarget(QStringLiteral("device-pending"));
        QVERIFY(restored);
        QCOMPARE(restored->mediaReferences.size(), 1);
        const auto& reference = restored->mediaReferences.first();
        QVERIFY(reference.pendingImport);
        QVERIFY(reference.sha256.isEmpty());
        QVERIFY(reference.assetId.isEmpty());
        QCOMPARE(reference.canonicalSourcePath, source.canonicalSourcePath);
        QCOMPARE(restored->canvasStateForRestore().value(QStringLiteral("media")).toArray().size(), 1);
        auto legacy = source.toJson();
        legacy.remove(QStringLiteral("pendingImport"));
        ProjectMediaReference legacyReference;
        QVERIFY(ProjectMediaReference::fromJson(legacy, &legacyReference));
        QVERIFY(!legacyReference.pendingImport);
    }

    void roundTripUsesCheckpointAndStripsTransientState()
    {
        QTemporaryDir temporary;
        QVERIFY(temporary.isValid());
        ProjectStore store(temporary.filePath(QStringLiteral("projects.json")));

        ProjectManager writer(&store);
        writer.stopAutomaticTimersForTesting();
        QCOMPARE(createProject(writer, target(QStringLiteral("device-a"),
                                             QStringLiteral("Studio A")),
                                      ProjectLifecycleState::Visible,
                                      1000).isEmpty(), false);

        QJsonObject video {
            {QStringLiteral("type"), QStringLiteral("video")},
            {QStringLiteral("mediaId"), QStringLiteral("media-1")},
            {QStringLiteral("playing"), true},
            {QStringLiteral("residencyReady"), true},
            {QStringLiteral("residencyState"), QStringLiteral("ready")},
            {QStringLiteral("residencyProgress"), 1.0},
            {QStringLiteral("playbackState"), QStringLiteral("playing")},
            {QStringLiteral("uploadStatus"), QStringLiteral("uploaded")},
            {QStringLiteral("remoteSessionId"), QStringLiteral("secret-session")},
            {QStringLiteral("resumeToken"), QStringLiteral("secret-token")},
            {QStringLiteral("uploadState"), QStringLiteral("uploaded")},
            {QStringLiteral("remoteFileId"), QStringLiteral("remote-file")},
            {QStringLiteral("startPositionMs"), 4210}
        };
        QJsonObject canvas {
            {QStringLiteral("canvasSessionId"), QStringLiteral("removed-session")},
            {QStringLiteral("viewport"), QJsonObject{
                 {QStringLiteral("scale"), 1.75},
                 {QStringLiteral("panX"), 42.0},
                 {QStringLiteral("panY"), -18.0},
                 {QStringLiteral("cameraVersion"), 2},
                 {QStringLiteral("centerX"), 320.0},
                 {QStringLiteral("centerY"), -85.0},
                 {QStringLiteral("squareSceneSize"), 1600.0}}},
            {QStringLiteral("settings"), QJsonObject{
                 {QStringLiteral("snapEnabled"), true}}},
            {QStringLiteral("media"), QJsonArray{video}}
        };
        ProjectMediaReference reference;
        reference.mediaId = QStringLiteral("media-1");
        reference.assetId = QStringLiteral("asset-1");
        reference.canonicalSourcePath = QStringLiteral("/tmp/movie.mp4");
        reference.sha256 = QStringLiteral("abc123");
        reference.mediaType = QStringLiteral("video");
        const QList<ScreenInfo> savedScreens =
            client(QStringLiteral("device-a"), QStringLiteral("socket-1"),
                   QStringLiteral("Studio A")).getScreens();
        QVERIFY(writer.updateCanvasState(QStringLiteral("device-a"), canvas,
                                         {reference}, savedScreens, 1001));
        writer.checkpointVisibleProjects(15'000);
        QVERIFY(writer.flush());

        QList<ProjectRecord> durable;
        QVERIFY(store.load(&durable));
        QCOMPARE(durable.size(), 1);
        QCOMPARE(durable.first().target.endpointId, QStringLiteral("device-a"));
        QCOMPARE(durable.first().target.machineName, QStringLiteral("Studio A"));
        QCOMPARE(durable.first().savedScreens.size(), 1);
        const QJsonObject durableJson = durable.first().toJson();
        const QStringList durableTargetKeys =
            durableJson.value(QStringLiteral("target")).toObject().keys();
        QCOMPARE(QSet<QString>(durableTargetKeys.cbegin(),
                               durableTargetKeys.cend()),
                 QSet<QString>({QStringLiteral("endpointId"),
                                QStringLiteral("machineName"),
                                QStringLiteral("platform"),
                                QStringLiteral("installationId"),
                                QStringLiteral("instanceId"),
                                QStringLiteral("instanceOrdinal")}));
        QVERIFY(!durableJson.contains(QStringLiteral("status")));
        QVERIFY(!durableJson.contains(QStringLiteral("volumePercent")));
        QVERIFY(!durableJson.contains(QStringLiteral("lastSeen")));
        QVERIFY(!durableJson.contains(QStringLiteral("serverConnectionId")));
        QVERIFY(!durable.first().canvasState.contains(QStringLiteral("canvasSessionId")));
        const QJsonObject durableVideo = durable.first().canvasState.value(QStringLiteral("media"))
                                             .toArray().first().toObject();
        QVERIFY(!durableVideo.contains(QStringLiteral("playing")));
        QVERIFY(!durableVideo.contains(QStringLiteral("residencyReady")));
        QVERIFY(!durableVideo.contains(QStringLiteral("residencyState")));
        QVERIFY(!durableVideo.contains(QStringLiteral("residencyProgress")));
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
        QCOMPARE(project->savedScreens.size(), 1);
        QCOMPARE(project->canvasState.value(QStringLiteral("viewport"))
                     .toObject().value(QStringLiteral("scale")).toDouble(),
                 1.75);
        QCOMPARE(project->canvasStateForRestore().value(QStringLiteral("viewport")),
                 canvas.value(QStringLiteral("viewport")));
        QVERIFY(project->canvasState.value(QStringLiteral("settings"))
                    .toObject().value(QStringLiteral("snapEnabled")).toBool());

        const QJsonObject restoredVideo = project->canvasStateForRestore()
                                              .value(QStringLiteral("media"))
                                              .toArray().first().toObject();
        QVERIFY(!restoredVideo.contains(QStringLiteral("playing")));
        QVERIFY(!restoredVideo.contains(QStringLiteral("playbackState")));
        QVERIFY(!restoredVideo.contains(QStringLiteral("uploadStatus")));
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
        QVERIFY(!createProject(writer,
            target(QStringLiteral("device-a"), QStringLiteral("Studio A")),
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
        createProject(manager, target(QStringLiteral("device-a"),
                                     QStringLiteral("Studio A")),
                              ProjectLifecycleState::Visible, 0);
        QVERIFY(manager.setHidden(QStringLiteral("device-a"), 100));
        QVERIFY(manager.setHidden(QStringLiteral("device-a"), 10'000));
        QCOMPARE(manager.projectDeleteAtMs(QStringLiteral("device-a")), qint64(300'100));

        QSignalSpy aboutSpy(&manager, &ProjectManager::projectAboutToDelete);
        QSignalSpy deletedSpy(&manager, &ProjectManager::projectDeleted);
        QSignalSpy removedSpy(&manager, &ProjectManager::projectRemoved);
        manager.processDeadlines(200'000);
        QCOMPARE(manager.projectCount(), 1);
        manager.processDeadlines(300'100);
        QCOMPARE(aboutSpy.count(), 1);
        QCOMPARE(deletedSpy.count(), 1);
        QCOMPARE(manager.projectCount(), 0);
        QCOMPARE(removedSpy.count(), 1);
        QCOMPARE(removedSpy.last().at(2).value<ProjectManager::RemovalReason>(),
                 ProjectManager::RemovalReason::RetentionExpired);

        QList<ProjectRecord> persisted;
        QVERIFY(store.load(&persisted));
        QVERIFY(persisted.isEmpty());
        createProject(manager, target(QStringLiteral("device-a"), QStringLiteral("Studio A")),
                      ProjectLifecycleState::Visible, 400'000);
        QVERIFY(manager.deleteProject(QStringLiteral("device-a")));
        QCOMPARE(removedSpy.count(), 2);
        QCOMPARE(removedSpy.last().at(2).value<ProjectManager::RemovalReason>(),
                 ProjectManager::RemovalReason::UserDeleted);
    }

    void mediaDeadlineIsExactPerProjectAndNeverExtended()
    {
        QTemporaryDir temporary;
        ProjectStore store(temporary.filePath(QStringLiteral("projects.json")));
        ProjectManager::TimingPolicy timing;
        timing.projectMediaHiddenTimeoutMs = 1'000;
        ProjectManager manager(&store, timing);
        manager.stopAutomaticTimersForTesting();
        const QString endpoint = QStringLiteral("device-a");
        const QString projectId = createProject(manager, target(endpoint, QStringLiteral("Studio A")),
                                               ProjectLifecycleState::Hidden, 100);
        QVERIFY(!projectId.isEmpty());
        QVERIFY(!createProject(manager, target(QStringLiteral("device-visible"), QStringLiteral("Visible")),
                               ProjectLifecycleState::Visible, 100).isEmpty());
        QVERIFY(!createProject(manager, target(QStringLiteral("device-later"), QStringLiteral("Later")),
                               ProjectLifecycleState::Hidden, 500).isEmpty());
        QSignalSpy releaseSpy(&manager, &ProjectManager::projectMediaReleaseDue);
        QCOMPARE(manager.projectMediaReleaseAtMs(endpoint), qint64(1'100));
        QCOMPARE(manager.projectMediaReleaseAtMs(QStringLiteral("device-visible")), qint64(-1));
        QCOMPARE(manager.projectMediaReleaseAtMs(QStringLiteral("missing")), qint64(-1));
        QVERIFY(manager.setHidden(endpoint, 900));
        QVERIFY(manager.updateCanvasState(endpoint, {{QStringLiteral("test"), 42}}, {}, {}, 999));
        const ProjectRecord beforeRelease = *manager.projectForTarget(endpoint);
        manager.processDeadlines(1'099);
        QCOMPARE(releaseSpy.count(), 0);
        QVERIFY(!manager.projectMediaReleaseExpired(endpoint));

        manager.processDeadlines(1'100);
        QCOMPARE(releaseSpy.count(), 1);
        QCOMPARE(releaseSpy.first().at(0).toString(), projectId);
        QCOMPARE(releaseSpy.first().at(1).toString(), endpoint);
        QVERIFY(manager.projectMediaReleaseExpired(endpoint));
        QCOMPARE(manager.projectMediaReleaseAtMs(endpoint), qint64(-1));
        QCOMPARE(manager.projectForTarget(endpoint)->toJson(), beforeRelease.toJson());
        QCOMPARE(manager.projectDeleteAtMs(endpoint), qint64(300'100));
        QVERIFY(manager.setHidden(endpoint, 1'101));
        manager.processDeadlines(1'499);
        QCOMPARE(releaseSpy.count(), 1);
        manager.processDeadlines(1'500);
        QCOMPARE(releaseSpy.count(), 2);
        QVERIFY(!manager.projectMediaReleaseExpired(QStringLiteral("device-visible")));
        QCOMPARE(manager.projectCount(), 3);
    }

    void mediaDeadlineCancelsAndRearmsWhenVisibilityChanges()
    {
        QTemporaryDir temporary;
        ProjectStore store(temporary.filePath(QStringLiteral("projects.json")));
        ProjectManager::TimingPolicy timing;
        timing.projectMediaHiddenTimeoutMs = 1'000;
        ProjectManager manager(&store, timing);
        manager.stopAutomaticTimersForTesting();
        const QString endpoint = QStringLiteral("device-a");
        QVERIFY(!createProject(manager, target(endpoint, QStringLiteral("Studio A")),
                               ProjectLifecycleState::Hidden, 100).isEmpty());
        QSignalSpy releaseSpy(&manager, &ProjectManager::projectMediaReleaseDue);
        QVERIFY(manager.setVisible(endpoint, 1'099));
        manager.processDeadlines(1'100);
        QCOMPARE(releaseSpy.count(), 0);
        QCOMPARE(manager.projectMediaReleaseAtMs(endpoint), qint64(-1));

        QVERIFY(manager.setHidden(endpoint, 2'000));
        // Opening exactly at expiry consumes the deadline before resuming.
        QVERIFY(manager.setVisible(endpoint, 3'000));
        QCOMPARE(releaseSpy.count(), 1);
        QVERIFY(!manager.projectMediaReleaseExpired(endpoint));
        QVERIFY(manager.setHidden(endpoint, 4'000));
        manager.processDeadlines(5'000);
        QCOMPARE(releaseSpy.count(), 2);
        QVERIFY(manager.projectMediaReleaseExpired(endpoint));
        QVERIFY(manager.setVisible(endpoint, 5'001));
        QCOMPARE(releaseSpy.count(), 2);
        QVERIFY(!manager.projectMediaReleaseExpired(endpoint));
        QVERIFY(manager.setHidden(endpoint, 6'000));
        QVERIFY(manager.deleteProject(endpoint));
        QVERIFY(!manager.projectMediaReleaseExpired(endpoint));
        manager.processDeadlines(7'000);
        QCOMPARE(releaseSpy.count(), 2);
    }

    void mediaDeadlineReloadUsesOriginalHiddenTimestamp()
    {
        QTemporaryDir temporary;
        ProjectStore store(temporary.filePath(QStringLiteral("projects.json")));
        ProjectManager::TimingPolicy timing;
        timing.projectMediaHiddenTimeoutMs = 1'000;
        ProjectManager manager(&store, timing);
        manager.stopAutomaticTimersForTesting();
        const QString endpoint = QStringLiteral("device-a");
        QVERIFY(!createProject(manager, target(endpoint, QStringLiteral("Studio A")),
                               ProjectLifecycleState::Hidden, 100).isEmpty());
        QVERIFY(manager.flush());
        QSignalSpy releaseSpy(&manager, &ProjectManager::projectMediaReleaseDue);
        manager.processDeadlines(1'100);
        QCOMPARE(releaseSpy.count(), 1);
        QVERIFY(manager.projectMediaReleaseExpired(endpoint));

        manager.setNowProviderForTesting([] { return qint64(1'101); });
        QVERIFY(manager.load());
        QVERIFY(!manager.projectMediaReleaseExpired(endpoint));
        QCOMPARE(manager.projectMediaReleaseAtMs(endpoint), qint64(1'100));
        manager.processDeadlines(1'101);
        QCOMPARE(releaseSpy.count(), 2);
        manager.processDeadlines(1'102);
        QCOMPARE(releaseSpy.count(), 2);
    }

    void mediaDeadlineCallbacksMayReenterAndDeleteProjects()
    {
        QTemporaryDir temporary;
        ProjectStore store(temporary.filePath(QStringLiteral("projects.json")));
        ProjectManager::TimingPolicy timing;
        timing.projectMediaHiddenTimeoutMs = 1'000;
        ProjectManager manager(&store, timing);
        manager.stopAutomaticTimersForTesting();
        const QString endpoint = QStringLiteral("device-a");
        QVERIFY(!createProject(manager, target(endpoint, QStringLiteral("Studio A")),
                               ProjectLifecycleState::Hidden, 100).isEmpty());
        QSignalSpy releaseSpy(&manager, &ProjectManager::projectMediaReleaseDue);
        connect(&manager, &ProjectManager::projectMediaReleaseDue, this,
                [&](const QString&, const QString& targetEndpointId) {
            QVERIFY(manager.projectMediaReleaseExpired(targetEndpointId));
            manager.processDeadlines(1'100);
            QVERIFY(manager.deleteProject(targetEndpointId));
        });
        QVERIFY(!manager.setVisible(endpoint, 1'100));
        QCOMPARE(releaseSpy.count(), 1);
        QCOMPARE(manager.projectCount(), 0);
        QVERIFY(!manager.projectMediaReleaseExpired(endpoint));
    }

    void mediaDeadlineIsIndependentOfRemoteSession_data()
    {
        QTest::addColumn<qint64>("sessionTimeoutMs");
        QTest::newRow("session-closes-first") << qint64(1'000);
        QTest::newRow("media-releases-first") << qint64(3'000);
    }

    void mediaDeadlineIsIndependentOfRemoteSession()
    {
        QFETCH(qint64, sessionTimeoutMs);
        QTemporaryDir temporary;
        ProjectStore store(temporary.filePath(QStringLiteral("projects.json")));
        ProjectManager::TimingPolicy timing;
        timing.projectMediaHiddenTimeoutMs = 2'000;
        ProjectManager manager(&store, timing);
        manager.stopAutomaticTimersForTesting();
        const QString endpoint = QStringLiteral("device-a");
        QVERIFY(!createProject(manager, target(endpoint, QStringLiteral("Studio A")),
                               ProjectLifecycleState::Hidden, 100).isEmpty());
        WorkspaceManager sessions;
        sessions.stopAutomaticTimersForTesting();
        sessions.setRemoteSessionHiddenTimeoutMs(sessionTimeoutMs);
        sessions.getOrCreateWorkspace(endpoint, client(endpoint, QStringLiteral("socket-a"),
                                                       QStringLiteral("Studio A")));
        QVERIFY(sessions.setWorkspaceVisible(endpoint, 100));
        QVERIFY(sessions.setRemoteSessionState(endpoint, WorkspaceManager::RemoteSessionState::Active));
        QVERIFY(sessions.setWorkspaceHidden(endpoint, 100));
        QSignalSpy releaseSpy(&manager, &ProjectManager::projectMediaReleaseDue);
        QSignalSpy closeSpy(&sessions, &WorkspaceManager::remoteSessionCloseDue);

        sessions.processDeadlines(2'100);
        QCOMPARE(closeSpy.count(), sessionTimeoutMs < 2'000 ? 1 : 0);
        QCOMPARE(releaseSpy.count(), 0);
        manager.processDeadlines(2'100);
        QCOMPARE(releaseSpy.count(), 1);
        QCOMPARE(closeSpy.count(), sessionTimeoutMs < 2'000 ? 1 : 0);
        QVERIFY(manager.hasProjectForTarget(endpoint));
        QCOMPARE(manager.projectDeleteAtMs(endpoint), qint64(300'100));
    }

    void discoveryProjectionCannotReenterDeadlineDeletion()
    {
        QTemporaryDir temporary;
        ProjectStore store(temporary.filePath(QStringLiteral("projects.json")));
        ProjectManager manager(&store);
        manager.stopAutomaticTimersForTesting();

        constexpr int projectCount = 24;
        for (int index = 0; index < projectCount; ++index) {
            const QString endpointId = QStringLiteral("device-%1").arg(index);
            QVERIFY(!createProject(
                manager, target(endpointId, QStringLiteral("Studio %1").arg(index)),
                ProjectLifecycleState::Hidden, 100).isEmpty());
        }

        int callbackDepth = 0;
        int maximumCallbackDepth = 0;
        int deletedCount = 0;
        connect(&manager, &ProjectManager::projectDeleted,
                this, [&](const QString&, const QString&) {
            ++callbackDepth;
            maximumCallbackDepth = qMax(maximumCallbackDepth, callbackDepth);
            ++deletedCount;
            // List refreshes are legitimate observers of deletion. They must
            // never recursively execute more lifecycle transitions.
            manager.mergeDiscoveredClients({}, 300'100);
            --callbackDepth;
        });

        manager.processDeadlines(300'100);
        QCOMPARE(deletedCount, projectCount);
        QCOMPARE(manager.projectCount(), 0);
        QCOMPARE(maximumCallbackDepth, 1);
    }

    void cleanShutdownHideIsIdempotentAndUsesOneTimestamp()
    {
        QTemporaryDir temporary;
        ProjectStore store(temporary.filePath(QStringLiteral("projects.json")));
        ProjectManager manager(&store);
        manager.stopAutomaticTimersForTesting();
        createProject(manager, target(QStringLiteral("device-a"),
                                     QStringLiteral("Studio A")),
                              ProjectLifecycleState::Visible, 1'000);
        createProject(manager, target(QStringLiteral("device-b"),
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
        QCOMPARE(manager.projectDeleteAtMs(QStringLiteral("device-b")),
                 qint64(310'000));
    }

    void discoveryMergeKeepsDisconnectedProjectsAndRefreshesByEndpointId()
    {
        QTemporaryDir temporary;
        ProjectStore store(temporary.filePath(QStringLiteral("projects.json")));
        ProjectManager manager(&store);
        manager.stopAutomaticTimersForTesting();
        createProject(manager, target(QStringLiteral("device-a"),
                                     QStringLiteral("Old name")),
                              ProjectLifecycleState::Hidden, 10);
        QVERIFY(manager.updateSavedScreens(
            QStringLiteral("device-a"),
            client(QStringLiteral("device-a"), QStringLiteral("socket-old"),
                   QStringLiteral("Old name")).getScreens(), 11));

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
        QCOMPARE(entries.at(1).client.getVolumePercent(), 50);
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
        QCOMPARE(entries.first().client.getScreens().size(), 1);
        QCOMPARE(entries.first().client.getVolumePercent(), 50);
        const ProjectRecord* replaced = manager.projectForTarget(QStringLiteral("device-a"));
        QVERIFY(replaced);
        QCOMPARE(replaced->savedScreens.size(), 1);

        ClientInfo identityOnly(QStringLiteral("socket-identity"), QString(), QString());
        identityOnly.setEndpointId(QStringLiteral("device-a"));
        entries = manager.mergeDiscoveredClients({identityOnly}, 31);
        QCOMPARE(entries.size(), 1);
        QCOMPARE(entries.first().client.getMachineName(), QStringLiteral("New name"));
        QCOMPARE(entries.first().client.getPlatform(), QStringLiteral("Linux"));
        const ProjectRecord* presentationPreserved =
            manager.projectForTarget(QStringLiteral("device-a"));
        QVERIFY(presentationPreserved);
        QCOMPARE(presentationPreserved->target.machineName,
                 QStringLiteral("New name"));
        QCOMPARE(presentationPreserved->target.platform,
                 QStringLiteral("Linux"));

        entries = manager.mergeDiscoveredClients({}, 32);
        QCOMPARE(entries.size(), 1);
        QVERIFY(!entries.first().online);
        QCOMPARE(entries.first().client.getScreens().size(), 1);

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
        createProject(manager, target(QStringLiteral("device-a"),
                                     QStringLiteral("New name")),
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
        createProject(writer, target(QStringLiteral("device-a"),
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

    void projectRetentionBoundariesAt299And300Seconds()
    {
        QTemporaryDir temporary;
        ProjectStore store(temporary.filePath(QStringLiteral("projects.json")));
        ProjectManager manager(&store);
        manager.stopAutomaticTimersForTesting();

        createProject(manager, target(QStringLiteral("device-299"),
                                     QStringLiteral("Studio 299")),
                              ProjectLifecycleState::Visible, 0);
        QVERIFY(manager.setHidden(QStringLiteral("device-299"), 0));
        QVERIFY(manager.setVisible(QStringLiteral("device-299"), 299'999));
        QVERIFY(manager.hasProjectForTarget(QStringLiteral("device-299")));

        createProject(manager, target(QStringLiteral("device-300"),
                                     QStringLiteral("Studio 300")),
                              ProjectLifecycleState::Visible, 0);
        QVERIFY(manager.setHidden(QStringLiteral("device-300"), 0));
        QVERIFY(!manager.setVisible(QStringLiteral("device-300"), 300'000));
        QVERIFY(!manager.hasProjectForTarget(QStringLiteral("device-300")));
    }

    void remoteSessionDeadlineAt59And60SecondsIsIndependent()
    {
        WorkspaceManager sessions;
        sessions.stopAutomaticTimersForTesting();
        sessions.setRemoteSessionHiddenTimeoutMs(60'000);
        ClientInfo targetClient = client(
            QStringLiteral("device-a"), QStringLiteral("socket-a"),
            QStringLiteral("Studio A"));
        sessions.getOrCreateWorkspace(QStringLiteral("device-a"), targetClient);
        QVERIFY(sessions.setWorkspaceVisible(QStringLiteral("device-a"), 0));
        QVERIFY(sessions.setRemoteSessionState(
            QStringLiteral("device-a"), WorkspaceManager::RemoteSessionState::Active));
        QVERIFY(sessions.setWorkspaceHidden(QStringLiteral("device-a"), 0));
        QVERIFY(sessions.setWorkspaceHidden(QStringLiteral("device-a"), 30'000));
        QCOMPARE(sessions.remoteSessionCloseAtMs(QStringLiteral("device-a")),
                 qint64(60'000));

        QSignalSpy closeSpy(&sessions, &WorkspaceManager::remoteSessionCloseDue);
        sessions.processDeadlines(59'999);
        QCOMPARE(closeSpy.count(), 0);
        sessions.processDeadlines(60'000);
        QCOMPARE(closeSpy.count(), 1);
        QCOMPARE(sessions.remoteSessionState(QStringLiteral("device-a")),
                 WorkspaceManager::RemoteSessionState::Closing);
    }

    void incomingOrphanWatchdogUsesExactNonExtensibleDeadline()
    {
        IncomingSessionOrphanWatchdog watchdog;
        watchdog.setConfiguredTimeoutMs(3'000);
        watchdog.stopAutomaticTimerForTesting();
        QSignalSpy dueSpy(
            &watchdog,
            &IncomingSessionOrphanWatchdog::orphanedSessionsDue);

        QVERIFY(watchdog.arm({QStringLiteral("session-a")}, 0, 0));
        QCOMPARE(watchdog.deadlineMs(), qint64(3'000));
        // Repeated loss/hide and a raw transport reconnect have no API that
        // can renew or cancel the already captured authority deadline.
        QVERIFY(!watchdog.arm({QStringLiteral("session-b")}, 0, 1'000));
        watchdog.transportConnected();
        QCOMPARE(watchdog.deadlineMs(), qint64(3'000));
        QVERIFY(!watchdog.authenticatedSessionResumed(
            QStringLiteral("different-session"), 2'999));
        watchdog.processDeadline(2'999);
        QCOMPARE(dueSpy.count(), 0);
        watchdog.processDeadline(3'000);
        QCOMPARE(dueSpy.count(), 1);
        QVERIFY(!watchdog.active());

        QVERIFY(watchdog.arm({QStringLiteral("session-a")}, 0, 10'000));
        QCOMPARE(watchdog.deadlineMs(), qint64(13'000));
        QVERIFY(watchdog.authenticatedSessionResumed(
            QStringLiteral("session-a"), 12'999));
        watchdog.processDeadline(13'000);
        QCOMPARE(dueSpy.count(), 1);

        // At the exact deadline cleanup wins over a late resumption.
        QVERIFY(watchdog.arm({QStringLiteral("session-a")}, 0, 20'000));
        QVERIFY(!watchdog.authenticatedSessionResumed(
            QStringLiteral("session-a"), 23'000));
        QCOMPARE(dueSpy.count(), 2);

        // A longer announced server lease can only extend the local minimum.
        QVERIFY(watchdog.arm({QStringLiteral("session-a"),
                              QStringLiteral("session-b")},
                             5'000, 30'000));
        QCOMPARE(watchdog.deadlineMs(), qint64(35'000));
        QVERIFY(watchdog.authenticatedSessionResumed(
            QStringLiteral("session-a"), 34'999));
        QCOMPARE(watchdog.capturedSessionIds(),
                 QSet<QString>{QStringLiteral("session-b")});
        watchdog.processDeadline(34'999);
        QCOMPARE(dueSpy.count(), 2);
        watchdog.processDeadline(35'000);
        QCOMPARE(dueSpy.count(), 3);
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

        const QString projectId = createProject(manager,
            target(QStringLiteral("device-a"), QStringLiteral("Studio A")),
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
                                          canvasState, {}, {}, 1'100));
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
