#include <QtTest>

#include "backend/network/RemoteSessionCoordinator.h"
#include "backend/network/SceneRunCoordinator.h"

namespace {
const QString kOwner(43, QLatin1Char('A'));
const QString kTarget(43, QLatin1Char('B'));
const QString kThird(43, QLatin1Char('C'));
const QString kFileHash(64, QLatin1Char('a'));

QJsonObject sessionEnvelope(quint64 generation = 1,
                            const QString& phase = QStringLiteral("Active"),
                            const QString& type = QStringLiteral("remote_session_opened"))
{
    QJsonObject envelope{
        {QStringLiteral("type"), type},
        {QStringLiteral("remoteSessionId"), QStringLiteral("remote_session_1")},
        {QStringLiteral("generation"), static_cast<double>(generation)},
        {QStringLiteral("ownerConnectionGeneration"), static_cast<double>(generation)},
        {QStringLiteral("targetConnectionGeneration"), static_cast<double>(generation)},
        {QStringLiteral("ownerEndpointId"), kOwner},
        {QStringLiteral("targetEndpointId"), kTarget},
        {QStringLiteral("resumeToken"), QStringLiteral("memory_only_token")},
        {QStringLiteral("phase"), phase}
    };
    if (phase == QLatin1String("Terminating")
        || phase == QLatin1String("CleanupPending")
        || phase == QLatin1String("Closed")) {
        envelope.remove(QStringLiteral("resumeToken"));
    }
    return envelope;
}

QJsonArray manifest()
{
    return QJsonArray{QJsonObject{
        {QStringLiteral("assetId"), QStringLiteral("asset_1")},
        {QStringLiteral("extension"), QStringLiteral("png")},
        {QStringLiteral("fileId"), kFileHash},
        {QStringLiteral("mediaIds"), QJsonArray{QStringLiteral("media_1")}},
        {QStringLiteral("sha256"), kFileHash},
        {QStringLiteral("size"), 12}
    }};
}

QJsonObject scene()
{
    return QJsonObject{
        {QStringLiteral("screens"), QJsonArray{QJsonObject{
             {QStringLiteral("id"), 0},
             {QStringLiteral("width"), 1920},
             {QStringLiteral("height"), 1080}
         }}},
        {QStringLiteral("media"), QJsonArray{QJsonObject{
             {QStringLiteral("mediaId"), QStringLiteral("media_1")},
             {QStringLiteral("type"), QStringLiteral("image")},
             {QStringLiteral("fileId"), kFileHash}
         }}},
        {QStringLiteral("renderSchemaVersion"), 2}
    };
}
}

class SceneRunCoordinatorTest final : public QObject
{
    Q_OBJECT

private slots:
    void canonicalDigestMatchesServerImplementation()
    {
        QString error;
        const QJsonArray normalized = SceneRunCoordinator::normalizeManifest(manifest(), &error);
        QVERIFY2(error.isEmpty(), qPrintable(error));
        QCOMPARE(SceneRunCoordinator::computeDigest(1, normalized, scene()),
                 QStringLiteral("d56dbb37d4abbf271d0de59f37ad590e0e6b21d256842e6486578a0a7b8a6aa9"));
    }

    void sessionIdentityAndResumeTokenAreMemoryOnlyBindings()
    {
        RemoteSessionCoordinator sessions;
        sessions.setLocalEndpointId(kOwner);
        QVERIFY(sessions.upsert(sessionEnvelope()));
        QCOMPARE(sessions.forPeer(kTarget).remoteSessionId,
                 QStringLiteral("remote_session_1"));

        QJsonObject resumed = sessionEnvelope(2);
        resumed.remove(QStringLiteral("resumeToken"));
        QVERIFY(sessions.upsert(resumed));
        QCOMPARE(sessions.byId(QStringLiteral("remote_session_1")).resumeToken,
                 QStringLiteral("memory_only_token"));

        QJsonObject impostor = sessionEnvelope();
        impostor.insert(QStringLiteral("ownerEndpointId"), QString(64, QLatin1Char('f')));
        QVERIFY(!sessions.upsert(impostor));

        QJsonObject missingTransportBinding = sessionEnvelope();
        missingTransportBinding.remove(QStringLiteral("ownerConnectionGeneration"));
        QVERIFY(!sessions.upsert(missingTransportBinding));
    }

    void sessionEnumerationIncludesBothLocalRoles()
    {
        RemoteSessionCoordinator sessions;
        sessions.setLocalEndpointId(kOwner);
        QVERIFY(sessions.upsert(sessionEnvelope()));

        QJsonObject incoming = sessionEnvelope(4);
        incoming.insert(QStringLiteral("remoteSessionId"),
                        QStringLiteral("remote_session_incoming"));
        incoming.insert(QStringLiteral("ownerEndpointId"), kThird);
        incoming.insert(QStringLiteral("targetEndpointId"), kOwner);
        QVERIFY(sessions.upsert(incoming));

        const QList<RemoteSessionCoordinator::Binding> all = sessions.all();
        QCOMPARE(all.size(), 2);
        QVERIFY(!sessions.forPeer(kTarget).remoteSessionId.isEmpty());
        const auto incomingBinding = sessions.forPeer(kThird);
        QCOMPARE(incomingBinding.ownerEndpointId, kThird);
        QCOMPARE(incomingBinding.targetEndpointId, kOwner);
    }

    void oppositeDirectionsToTheSamePeerRemainIndependent()
    {
        RemoteSessionCoordinator sessions;
        sessions.setLocalEndpointId(kOwner);
        QVERIFY(sessions.upsert(sessionEnvelope(), 1));

        QJsonObject reverse = sessionEnvelope();
        reverse.insert(QStringLiteral("remoteSessionId"),
                       QStringLiteral("remote_session_reverse"));
        reverse.insert(QStringLiteral("ownerEndpointId"), kTarget);
        reverse.insert(QStringLiteral("targetEndpointId"), kOwner);
        QVERIFY(sessions.upsert(reverse, 1));

        QCOMPARE(sessions.outgoingForPeer(kTarget).remoteSessionId,
                 QStringLiteral("remote_session_1"));
        QCOMPARE(sessions.incomingForPeer(kTarget).remoteSessionId,
                 QStringLiteral("remote_session_reverse"));
        QCOMPARE(sessions.forPeer(kTarget).remoteSessionId,
                 QStringLiteral("remote_session_1"));
        QCOMPARE(sessions.all().size(), 2);

        QJsonObject secondIncoming = reverse;
        secondIncoming.insert(QStringLiteral("remoteSessionId"),
                              QStringLiteral("remote_session_second_incoming"));
        secondIncoming.insert(QStringLiteral("ownerEndpointId"), kThird);
        QVERIFY(!sessions.upsert(secondIncoming, 1));
        QCOMPARE(sessions.all().size(), 2);

        sessions.remove(QStringLiteral("remote_session_reverse"));
        QVERIFY(sessions.incomingForPeer(kTarget).remoteSessionId.isEmpty());
        QCOMPARE(sessions.outgoingForPeer(kTarget).remoteSessionId,
                 QStringLiteral("remote_session_1"));
    }

    void immutableRunRejectsWrongDigestAndIllegalTransitions()
    {
        SceneRunCoordinator coordinator;
        coordinator.setPrepareTimeoutMs(15000);
        coordinator.setLocalEndpointId(kOwner);
        coordinator.upsertSession(sessionEnvelope());
        SceneRunCoordinator::Run run;
        QString error;
        QVERIFY(coordinator.createOutgoingRun(kTarget, 1, manifest(), scene(), &run, &error));
        QCOMPARE(run.phase, SceneRunCoordinator::Phase::Preparing);

        QJsonObject prepared{
            {QStringLiteral("type"), QStringLiteral("prepared")},
            {QStringLiteral("remoteSessionId"), run.remoteSessionId},
            {QStringLiteral("generation"), static_cast<double>(run.generation)},
            {QStringLiteral("sceneRunId"), run.sceneRunId},
            {QStringLiteral("revision"), static_cast<double>(run.revision)},
            {QStringLiteral("digest"), QString(64, QLatin1Char('b'))},
            {QStringLiteral("ownerEndpointId"), kOwner},
            {QStringLiteral("targetEndpointId"), kTarget},
            {QStringLiteral("allPrepared"), true}
        };
        QVERIFY(!coordinator.acceptInboundEnvelope(prepared, &error));
        prepared.insert(QStringLiteral("digest"), run.digest);
        QJsonObject spoofedOwner = prepared;
        spoofedOwner.insert(QStringLiteral("ownerEndpointId"), kThird);
        QVERIFY(!coordinator.acceptInboundEnvelope(spoofedOwner, &error));
        QCOMPARE(coordinator.run(run.sceneRunId).phase,
                 SceneRunCoordinator::Phase::Preparing);
        QVERIFY(coordinator.acceptInboundEnvelope(prepared, &error));

        QJsonObject commit = prepared;
        commit.insert(QStringLiteral("type"), QStringLiteral("commit"));
        commit.insert(QStringLiteral("startEpochMs"), 1000);
        commit.insert(QStringLiteral("startServerMonotonicMs"), 500);
        QVERIFY(!coordinator.acceptInboundEnvelope(commit, &error));
    }

    void graceBindingIsResumableButRejectsNewSceneCommands()
    {
        SceneRunCoordinator coordinator;
        coordinator.setLocalEndpointId(kOwner);
        QVERIFY(coordinator.upsertSession(sessionEnvelope()));
        QVERIFY(coordinator.upsertSession(sessionEnvelope(
            1, QStringLiteral("Grace"),
            QStringLiteral("remote_session_lease_state"))));

        const auto binding = coordinator.sessionForPeer(kTarget);
        QCOMPARE(binding.phase, QStringLiteral("Grace"));
        QVERIFY(!binding.resumeToken.isEmpty());
        QVERIFY(!binding.active);

        SceneRunCoordinator::Run run;
        QString error;
        QVERIFY(!coordinator.createOutgoingRun(
            kTarget, 1, manifest(), scene(), &run, &error));
        QVERIFY(!error.isEmpty());
    }

    void remoteSessionEnvelopeTransitionsAreStrictlyCorrelated()
    {
        SceneRunCoordinator coordinator;
        coordinator.setLocalEndpointId(kOwner);
        QVERIFY(coordinator.upsertSession(sessionEnvelope(), 1));

        // Exact duplicate delivery is idempotent.
        QVERIFY(coordinator.upsertSession(sessionEnvelope(), 1));

        QJsonObject openedAsResume = sessionEnvelope(2);
        openedAsResume.insert(QStringLiteral("ownerConnectionGeneration"), 2);
        openedAsResume.insert(QStringLiteral("targetConnectionGeneration"), 1);
        QVERIFY(!coordinator.upsertSession(openedAsResume, 2));

        QJsonObject wrongParty = sessionEnvelope();
        wrongParty.insert(QStringLiteral("targetEndpointId"), kThird);
        QVERIFY(!coordinator.upsertSession(wrongParty, 1));
        QCOMPARE(coordinator.sessionForPeer(kTarget).targetEndpointId, kTarget);

        QJsonObject conflictingTransport = sessionEnvelope();
        conflictingTransport.insert(QStringLiteral("targetConnectionGeneration"), 2);
        QVERIFY(!coordinator.upsertSession(conflictingTransport, 1));

        QJsonObject grace = sessionEnvelope(
            1, QStringLiteral("Grace"),
            QStringLiteral("remote_session_lease_state"));
        QVERIFY(coordinator.upsertSession(grace, 1));

        QJsonObject resumed = sessionEnvelope(
            2, QStringLiteral("Active"),
            QStringLiteral("remote_session_resumed"));
        resumed.insert(QStringLiteral("ownerConnectionGeneration"), 2);
        resumed.insert(QStringLiteral("targetConnectionGeneration"), 1);
        QVERIFY(coordinator.upsertSession(resumed, 2));

        // A delayed generation-1 Grace event cannot roll generation 2 back.
        QVERIFY(!coordinator.upsertSession(grace, 2));
        QCOMPARE(coordinator.sessionForPeer(kTarget).generation, quint64(2));
        QCOMPARE(coordinator.sessionForPeer(kTarget).phase,
                 QStringLiteral("Active"));

        QJsonObject unknownTerminating = sessionEnvelope(
            1, QStringLiteral("Terminating"),
            QStringLiteral("remote_session_terminating"));
        unknownTerminating.insert(QStringLiteral("remoteSessionId"),
                                  QStringLiteral("unknown_session"));
        unknownTerminating.insert(QStringLiteral("teardownId"),
                                  QStringLiteral("teardown_1"));
        QVERIFY(!coordinator.upsertSession(unknownTerminating, 2));
    }

    void restartedTargetMayAcceptOnlyCorrelatedTerminalCatchup()
    {
        SceneRunCoordinator coordinator;
        coordinator.setLocalEndpointId(kTarget);
        QJsonObject catchup = sessionEnvelope(
            7, QStringLiteral("CleanupPending"),
            QStringLiteral("remote_session_terminating"));
        catchup.insert(QStringLiteral("ownerConnectionGeneration"), 4);
        catchup.insert(QStringLiteral("targetConnectionGeneration"), 9);
        catchup.insert(QStringLiteral("teardownId"),
                       QStringLiteral("teardown_recovery"));
        catchup.remove(QStringLiteral("resumeToken"));

        QVERIFY(coordinator.upsertSession(catchup, 9));
        const auto binding = coordinator.sessionForPeer(kOwner);
        QCOMPARE(binding.phase, QStringLiteral("CleanupPending"));
        QVERIFY(!binding.active);
        QVERIFY(binding.resumeToken.isEmpty());

        QJsonObject wrongTransport = catchup;
        wrongTransport.insert(QStringLiteral("remoteSessionId"),
                              QStringLiteral("other_terminal"));
        wrongTransport.insert(QStringLiteral("targetConnectionGeneration"), 8);
        QVERIFY(!coordinator.upsertSession(wrongTransport, 9));
    }

    void targetTerminalCatchupMayRebindOnlyItsAuthenticatedTransport()
    {
        SceneRunCoordinator coordinator;
        coordinator.setLocalEndpointId(kTarget);
        QVERIFY(coordinator.upsertSession(sessionEnvelope(), 1));

        QJsonObject terminating = sessionEnvelope(
            1, QStringLiteral("Terminating"),
            QStringLiteral("remote_session_terminating"));
        terminating.insert(QStringLiteral("teardownId"),
                           QStringLiteral("teardown_reconnect"));
        QVERIFY(coordinator.upsertSession(terminating, 1));

        QJsonObject catchup = terminating;
        catchup.insert(QStringLiteral("phase"), QStringLiteral("CleanupPending"));
        catchup.insert(QStringLiteral("targetConnectionGeneration"), 2);
        catchup.remove(QStringLiteral("resumeToken"));
        QVERIFY(coordinator.upsertSession(catchup, 2));
        QCOMPARE(coordinator.sessionForPeer(kOwner).targetConnectionGeneration,
                 quint64(2));

        QJsonObject closed = catchup;
        closed.insert(QStringLiteral("type"),
                      QStringLiteral("remote_session_closed"));
        closed.insert(QStringLiteral("phase"), QStringLiteral("Closed"));
        closed.insert(QStringLiteral("cleanupState"),
                      QStringLiteral("confirmed"));
        QVERIFY(coordinator.removeSession(closed, 2));
        QVERIFY(coordinator.sessionForPeer(kOwner).remoteSessionId.isEmpty());
    }

    void targetCanCatchUpDirectlyFromActiveToTerminal()
    {
        SceneRunCoordinator coordinator;
        coordinator.setLocalEndpointId(kTarget);
        QVERIFY(coordinator.upsertSession(sessionEnvelope(), 1));

        QJsonObject catchup = sessionEnvelope(
            1, QStringLiteral("CleanupPending"),
            QStringLiteral("remote_session_terminating"));
        catchup.insert(QStringLiteral("targetConnectionGeneration"), 2);
        catchup.insert(QStringLiteral("teardownId"),
                       QStringLiteral("teardown_missed_transition"));
        catchup.remove(QStringLiteral("resumeToken"));
        QVERIFY(coordinator.upsertSession(catchup, 2));
        const auto binding = coordinator.remoteSessions()->incomingForPeer(kOwner);
        QCOMPARE(binding.phase, QStringLiteral("CleanupPending"));
        QCOMPARE(binding.targetConnectionGeneration, quint64(2));

        QJsonObject rewritesPeerTransport = catchup;
        rewritesPeerTransport.insert(QStringLiteral("ownerConnectionGeneration"), 2);
        rewritesPeerTransport.insert(QStringLiteral("targetConnectionGeneration"), 3);
        QVERIFY(!coordinator.upsertSession(rewritesPeerTransport, 3));
    }

    void ownerTerminalReplayAndClosedCatchupResolveWithoutResurrection()
    {
        SceneRunCoordinator replayed;
        replayed.setLocalEndpointId(kOwner);
        QVERIFY(replayed.upsertSession(sessionEnvelope(), 1));

        QJsonObject terminal = sessionEnvelope(
            1, QStringLiteral("CleanupPending"),
            QStringLiteral("remote_session_terminating"));
        terminal.insert(QStringLiteral("ownerConnectionGeneration"), 2);
        terminal.insert(QStringLiteral("teardownId"),
                        QStringLiteral("teardown_owner_replay"));
        terminal.remove(QStringLiteral("resumeToken"));
        QVERIFY(replayed.upsertSession(terminal, 2));
        QCOMPARE(replayed.remoteSessions()->outgoingForPeer(kTarget).phase,
                 QStringLiteral("CleanupPending"));

        QJsonObject closed = terminal;
        closed.insert(QStringLiteral("type"),
                      QStringLiteral("remote_session_closed"));
        closed.insert(QStringLiteral("phase"), QStringLiteral("Closed"));
        closed.insert(QStringLiteral("cleanupState"),
                      QStringLiteral("confirmed"));
        QVERIFY(replayed.removeSession(closed, 2));
        QVERIFY(replayed.remoteSessions()->outgoingForPeer(kTarget)
                    .remoteSessionId.isEmpty());
        QVERIFY(!replayed.removeSession(closed, 2));
        QVERIFY(!replayed.upsertSession(terminal, 2));

        SceneRunCoordinator missedTerminal;
        missedTerminal.setLocalEndpointId(kOwner);
        QVERIFY(missedTerminal.upsertSession(sessionEnvelope(), 1));
        QVERIFY(missedTerminal.removeSession(closed, 2));
        QVERIFY(missedTerminal.sessionForPeer(kTarget).remoteSessionId.isEmpty());

        SceneRunCoordinator restartedOwner;
        restartedOwner.setLocalEndpointId(kOwner);
        QVERIFY(restartedOwner.removeSession(closed, 2));
        QVERIFY(!restartedOwner.removeSession(closed, 2));

        SceneRunCoordinator restartedTarget;
        restartedTarget.setLocalEndpointId(kTarget);
        QVERIFY(!restartedTarget.removeSession(closed, 1));
    }

    void protocolIntegersMustBeExactAndSafeBeforeMutation()
    {
        SceneRunCoordinator coordinator;
        coordinator.setPrepareTimeoutMs(15000);
        coordinator.setLocalEndpointId(kOwner);

        QJsonObject fractionalSession = sessionEnvelope();
        fractionalSession.insert(QStringLiteral("generation"), 1.5);
        QVERIFY(!coordinator.upsertSession(fractionalSession, 1));
        QVERIFY(coordinator.sessions().isEmpty());
        QVERIFY(coordinator.upsertSession(sessionEnvelope(), 1));

        SceneRunCoordinator::Run run;
        QString error;
        QVERIFY(coordinator.createOutgoingRun(
            kTarget, 1, manifest(), scene(), &run, &error));
        QJsonObject prepared{
            {QStringLiteral("type"), QStringLiteral("prepared")},
            {QStringLiteral("remoteSessionId"), run.remoteSessionId},
            {QStringLiteral("generation"), 1},
            {QStringLiteral("sceneRunId"), run.sceneRunId},
            {QStringLiteral("revision"), 1},
            {QStringLiteral("digest"), run.digest},
            {QStringLiteral("ownerEndpointId"), kOwner},
            {QStringLiteral("targetEndpointId"), kTarget},
            {QStringLiteral("allPrepared"), true}
        };

        QJsonObject fractionalRevision = prepared;
        fractionalRevision.insert(QStringLiteral("revision"), 1.25);
        QVERIFY(!coordinator.acceptInboundEnvelope(fractionalRevision, &error));
        QCOMPARE(coordinator.run(run.sceneRunId).phase,
                 SceneRunCoordinator::Phase::Preparing);
        QVERIFY(coordinator.acceptInboundEnvelope(prepared, &error));

        QJsonObject armed = prepared;
        armed.insert(QStringLiteral("type"), QStringLiteral("armed"));
        QVERIFY(coordinator.acceptInboundEnvelope(armed, &error));
        QCOMPARE(coordinator.run(run.sceneRunId).phase,
                 SceneRunCoordinator::Phase::Armed);

        QJsonObject commit = armed;
        commit.insert(QStringLiteral("type"), QStringLiteral("commit"));
        commit.insert(QStringLiteral("startEpochMs"), 1000.5);
        commit.insert(QStringLiteral("startServerMonotonicMs"), 500);
        QVERIFY(!coordinator.acceptInboundEnvelope(commit, &error));
        QCOMPARE(coordinator.run(run.sceneRunId).phase,
                 SceneRunCoordinator::Phase::Armed);
        QCOMPARE(coordinator.run(run.sceneRunId).startEpochMs, qint64(0));

        commit.insert(QStringLiteral("startEpochMs"), 1000);
        QVERIFY(coordinator.acceptInboundEnvelope(commit, &error));
        QCOMPARE(coordinator.run(run.sceneRunId).startEpochMs, qint64(1000));

        QJsonObject snapshot = commit;
        snapshot.insert(QStringLiteral("type"), QStringLiteral("state_snapshot"));
        snapshot.insert(QStringLiteral("sequence"), 1.5);
        QVERIFY(!coordinator.acceptInboundEnvelope(snapshot, &error));
        QCOMPARE(coordinator.run(run.sceneRunId).lastSnapshotSequence, quint64(0));
        snapshot.insert(QStringLiteral("sequence"), 1);
        QVERIFY(coordinator.acceptInboundEnvelope(snapshot, &error));
        QCOMPARE(coordinator.run(run.sceneRunId).lastSnapshotSequence, quint64(1));

        QJsonArray fractionalManifest = manifest();
        QJsonObject fractionalAsset = fractionalManifest.first().toObject();
        fractionalAsset.insert(QStringLiteral("size"), 12.5);
        fractionalManifest.replace(0, fractionalAsset);
        QVERIFY(SceneRunCoordinator::normalizeManifest(
                    fractionalManifest, &error).isEmpty());
        QVERIFY(!error.isEmpty());
    }

    void remoteSessionCloseMustMatchTheCurrentBinding()
    {
        SceneRunCoordinator coordinator;
        coordinator.setLocalEndpointId(kOwner);
        QVERIFY(coordinator.upsertSession(sessionEnvelope(), 1));

        QJsonObject terminating = sessionEnvelope(
            1, QStringLiteral("Terminating"),
            QStringLiteral("remote_session_terminating"));
        terminating.insert(QStringLiteral("teardownId"),
                           QStringLiteral("teardown_1"));
        QVERIFY(coordinator.upsertSession(terminating, 1));

        QJsonObject closed = terminating;
        closed.insert(QStringLiteral("type"),
                      QStringLiteral("remote_session_closed"));
        closed.insert(QStringLiteral("phase"), QStringLiteral("Closed"));
        closed.insert(QStringLiteral("cleanupState"),
                      QStringLiteral("confirmed"));

        QJsonObject stale = closed;
        stale.insert(QStringLiteral("generation"), 2);
        QVERIFY(!coordinator.removeSession(stale, 1));
        QVERIFY(!coordinator.sessionForPeer(kTarget).remoteSessionId.isEmpty());

        QJsonObject wrongParty = closed;
        wrongParty.insert(QStringLiteral("targetEndpointId"), kThird);
        QVERIFY(!coordinator.removeSession(wrongParty, 1));
        QVERIFY(!coordinator.sessionForPeer(kTarget).remoteSessionId.isEmpty());

        QJsonObject wrongTeardown = closed;
        wrongTeardown.insert(QStringLiteral("teardownId"),
                             QStringLiteral("teardown_2"));
        QVERIFY(!coordinator.removeSession(wrongTeardown, 1));

        QJsonObject uncommitted = closed;
        uncommitted.remove(QStringLiteral("cleanupState"));
        QVERIFY(!coordinator.removeSession(uncommitted, 1));

        QVERIFY(coordinator.removeSession(closed, 1));
        QVERIFY(coordinator.sessionForPeer(kTarget).remoteSessionId.isEmpty());
        // A late duplicate is an ignored no-op, never a second removal.
        QVERIFY(!coordinator.removeSession(closed, 1));
    }

    void preparationFailsClosedUntilServerPolicyArrives()
    {
        SceneRunCoordinator coordinator;
        coordinator.setLocalEndpointId(kOwner);
        coordinator.upsertSession(sessionEnvelope());
        SceneRunCoordinator::Run run;
        QString error;
        QVERIFY(!coordinator.createOutgoingRun(
            kTarget, 1, manifest(), scene(), &run, &error));
        QVERIFY(error.contains(QStringLiteral("policy"), Qt::CaseInsensitive));

        QJsonObject inbound{
            {QStringLiteral("type"), QStringLiteral("scene_prepare")},
            {QStringLiteral("remoteSessionId"), QStringLiteral("remote_session_1")},
            {QStringLiteral("generation"), 1},
            {QStringLiteral("sceneRunId"), QStringLiteral("run_1")},
            {QStringLiteral("revision"), 1},
            {QStringLiteral("digest"), SceneRunCoordinator::computeDigest(
                 1, SceneRunCoordinator::normalizeManifest(manifest()), scene())},
            {QStringLiteral("ownerEndpointId"), kOwner},
            {QStringLiteral("targetEndpointId"), kTarget},
            {QStringLiteral("manifest"), manifest()},
            {QStringLiteral("scene"), scene()}
        };
        QVERIFY(!coordinator.acceptInboundEnvelope(inbound, &error));
        coordinator.setPrepareTimeoutMs(15000);
        QVERIFY(coordinator.acceptInboundEnvelope(inbound, &error));
    }

    void checklistContainsAllRequiredPreparationStages()
    {
        const QJsonArray checklist = SceneRunCoordinator::createLocalChecklist(scene());
        QSet<QString> stages;
        for (const QJsonValue& value : checklist) {
            const QJsonObject item = value.toObject();
            QVERIFY(item.value(QStringLiteral("ready")).toBool());
            stages.insert(item.value(QStringLiteral("stage")).toString());
        }
        QVERIFY(stages.contains(QStringLiteral("screen_render_graph_ready")));
        QVERIFY(stages.contains(QStringLiteral("file_validated")));
        QVERIFY(stages.contains(QStringLiteral("image_decoded")));
        QVERIFY(stages.contains(QStringLiteral("image_texture_ready")));
    }
};

QTEST_APPLESS_MAIN(SceneRunCoordinatorTest)
#include "tst_SceneRunCoordinator.moc"
