#include <QtTest>

#include "backend/domain/scene/SceneActivityModel.h"

class SceneActivityModelTest final : public QObject
{
    Q_OBJECT

private slots:
    void containsOnlyLiveRunsAndClassifiesDirection();
    void tracksDegradedSessionsAndTerminalRemoval();
    void liveIdentityAndStartTimeAreImmutable();
};

void SceneActivityModelTest::containsOnlyLiveRunsAndClassifiesDirection()
{
    SceneActivityModel model;
    model.setLocalEndpointId(QStringLiteral("local"));

    QVERIFY(model.upsertLive(QStringLiteral("run-out"), QStringLiteral("session-out"),
                             QStringLiteral("local"), QStringLiteral("target"), 2'000));
    QVERIFY(model.upsertLive(QStringLiteral("run-in"), QStringLiteral("session-in"),
                             QStringLiteral("owner"), QStringLiteral("local"), 1'000));
    QCOMPARE(model.liveCount(), 2);

    const QList<SceneActivityModel::Activity> activities = model.liveActivities();
    QCOMPARE(activities.at(0).sceneRunId, QStringLiteral("run-out"));
    QCOMPARE(activities.at(0).direction, SceneActivityModel::Direction::Outgoing);
    QCOMPARE(activities.at(0).peerEndpointId, QStringLiteral("target"));
    QCOMPARE(activities.at(1).direction, SceneActivityModel::Direction::Incoming);
    QCOMPARE(activities.at(1).peerEndpointId, QStringLiteral("owner"));

    QVERIFY(!model.upsertLive(QStringLiteral("foreign"), QStringLiteral("session"),
                              QStringLiteral("a"), QStringLiteral("b"), 3'000));
    QCOMPARE(model.liveCount(), 2);
}

void SceneActivityModelTest::tracksDegradedSessionsAndTerminalRemoval()
{
    SceneActivityModel model;
    model.setLocalEndpointId(QStringLiteral("local"));
    model.upsertLive(QStringLiteral("run-a"), QStringLiteral("session-a"),
                     QStringLiteral("local"), QStringLiteral("target-a"), 1'000);
    model.upsertLive(QStringLiteral("run-b"), QStringLiteral("session-b"),
                     QStringLiteral("owner-b"), QStringLiteral("local"), 2'000);

    QSignalSpy changed(&model, &SceneActivityModel::activitiesChanged);
    model.setSessionDegraded(QStringLiteral("session-a"), true);
    QVERIFY(model.activity(QStringLiteral("run-a")).degraded);
    QVERIFY(!model.activity(QStringLiteral("run-b")).degraded);
    QCOMPARE(changed.count(), 1);

    QCOMPARE(model.removeForSession(QStringLiteral("session-a")), 1);
    QCOMPARE(model.liveCount(), 1);
    QVERIFY(model.remove(QStringLiteral("run-b")));
    QCOMPARE(model.liveCount(), 0);
}

void SceneActivityModelTest::liveIdentityAndStartTimeAreImmutable()
{
    SceneActivityModel model;
    model.setLocalEndpointId(QStringLiteral("local"));
    QVERIFY(model.upsertLive(QStringLiteral("run-a"), QStringLiteral("session-a"),
                             QStringLiteral("local"), QStringLiteral("target-a"), 1'000));

    QVERIFY(!model.upsertLive(QStringLiteral("run-a"), QStringLiteral("session-b"),
                              QStringLiteral("local"), QStringLiteral("target-b"), 1'000));
    QVERIFY(!model.upsertLive(QStringLiteral("run-a"), QStringLiteral("session-a"),
                              QStringLiteral("local"), QStringLiteral("target-a"), 2'000));
    QCOMPARE(model.liveCount(), 1);
    QCOMPARE(model.activity(QStringLiteral("run-a")).remoteSessionId,
             QStringLiteral("session-a"));
    QCOMPARE(model.activity(QStringLiteral("run-a")).startedAtEpochMs, qint64(1'000));

    QVERIFY(model.upsertLive(QStringLiteral("run-a"), QStringLiteral("session-a"),
                             QStringLiteral("local"), QStringLiteral("target-a"), 1'000,
                             true));
    QVERIFY(model.activity(QStringLiteral("run-a")).degraded);
}

QTEST_APPLESS_MAIN(SceneActivityModelTest)
#include "tst_SceneActivityModel.moc"
