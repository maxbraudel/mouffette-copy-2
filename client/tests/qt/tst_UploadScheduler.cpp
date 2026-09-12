#include <QtTest>

#include "backend/network/UploadScheduler.h"

class UploadSchedulerTest final : public QObject
{
    Q_OBJECT

private:
    static UploadScheduler::UploadRequest request(const QString &sessionId,
                                                  const QString &uploadId,
                                                  quint64 generation = 1)
    {
        return {sessionId, generation, uploadId, QStringLiteral("asset-") + uploadId};
    }

private slots:
    void enforcesGlobalAndPerSessionLimits();
    void schedulesSessionsRoundRobin();
    void gatesAdmissionOnActiveState();
    void terminalSessionPurgesAndCannotResurrect();
    void rejectsDuplicatesAndStaleCompletion();
    void supportsDeterministicCapacityChanges();
};

void UploadSchedulerTest::enforcesGlobalAndPerSessionLimits()
{
    UploadScheduler scheduler(2);
    scheduler.setSessionState(QStringLiteral("A"), UploadScheduler::SessionState::Active);
    scheduler.setSessionState(QStringLiteral("B"), UploadScheduler::SessionState::Active);
    scheduler.setSessionState(QStringLiteral("C"), UploadScheduler::SessionState::Active);

    QVector<UploadScheduler::UploadRequest> started;
    connect(&scheduler, &UploadScheduler::uploadStartRequested,
            this, [&started](const auto &upload) { started.append(upload); });

    QCOMPARE(scheduler.enqueue(request(QStringLiteral("A"), QStringLiteral("A1"))),
             UploadScheduler::EnqueueResult::Enqueued);
    scheduler.enqueue(request(QStringLiteral("A"), QStringLiteral("A2")));
    scheduler.enqueue(request(QStringLiteral("B"), QStringLiteral("B1")));
    scheduler.enqueue(request(QStringLiteral("C"), QStringLiteral("C1")));

    QCOMPARE(scheduler.activeCount(), 2);
    QCOMPARE(started.size(), 2);
    QCOMPARE(started.at(0).uploadId, QStringLiteral("A1"));
    QCOMPARE(started.at(1).uploadId, QStringLiteral("B1"));
    QVERIFY(!scheduler.isUploadActive(QStringLiteral("A"), 1, QStringLiteral("A2")));

    QVERIFY(scheduler.completeUpload(QStringLiteral("B"), 1, QStringLiteral("B1")));
    QCOMPARE(started.size(), 3);
    QCOMPARE(started.at(2).uploadId, QStringLiteral("C1"));

    QVERIFY(scheduler.completeUpload(QStringLiteral("A"), 1, QStringLiteral("A1")));
    QCOMPARE(started.size(), 4);
    QCOMPARE(started.at(3).uploadId, QStringLiteral("A2"));
}

void UploadSchedulerTest::schedulesSessionsRoundRobin()
{
    UploadScheduler scheduler(1);
    scheduler.setSessionState(QStringLiteral("A"), UploadScheduler::SessionState::Active);
    scheduler.setSessionState(QStringLiteral("B"), UploadScheduler::SessionState::Active);

    QStringList starts;
    connect(&scheduler, &UploadScheduler::uploadStartRequested,
            this, [&starts](const auto &upload) { starts.append(upload.uploadId); });

    scheduler.enqueue(request(QStringLiteral("A"), QStringLiteral("A1")));
    scheduler.enqueue(request(QStringLiteral("A"), QStringLiteral("A2")));
    scheduler.enqueue(request(QStringLiteral("A"), QStringLiteral("A3")));
    scheduler.enqueue(request(QStringLiteral("B"), QStringLiteral("B1")));
    scheduler.enqueue(request(QStringLiteral("B"), QStringLiteral("B2")));

    QVERIFY(scheduler.completeUpload(QStringLiteral("A"), 1, QStringLiteral("A1")));
    QVERIFY(scheduler.completeUpload(QStringLiteral("B"), 1, QStringLiteral("B1")));
    QVERIFY(scheduler.completeUpload(QStringLiteral("A"), 1, QStringLiteral("A2")));
    QVERIFY(scheduler.completeUpload(QStringLiteral("B"), 1, QStringLiteral("B2")));

    QCOMPARE(starts, QStringList({QStringLiteral("A1"), QStringLiteral("B1"),
                                  QStringLiteral("A2"), QStringLiteral("B2"),
                                  QStringLiteral("A3")}));
}

void UploadSchedulerTest::gatesAdmissionOnActiveState()
{
    UploadScheduler scheduler;
    QSignalSpy starts(&scheduler, &UploadScheduler::uploadStartRequested);

    scheduler.setSessionState(QStringLiteral("S"), UploadScheduler::SessionState::Opening);
    scheduler.enqueue(request(QStringLiteral("S"), QStringLiteral("one")));
    QCOMPARE(starts.size(), 0);

    scheduler.setSessionState(QStringLiteral("S"), UploadScheduler::SessionState::Active);
    QCOMPARE(starts.size(), 1);

    scheduler.setSessionState(QStringLiteral("S"), UploadScheduler::SessionState::Grace);
    scheduler.enqueue(request(QStringLiteral("S"), QStringLiteral("two")));
    QVERIFY(scheduler.completeUpload(QStringLiteral("S"), 1, QStringLiteral("one")));
    QCOMPARE(starts.size(), 1);

    scheduler.setSessionState(QStringLiteral("S"), UploadScheduler::SessionState::Active);
    QCOMPARE(starts.size(), 2);
}

void UploadSchedulerTest::terminalSessionPurgesAndCannotResurrect()
{
    UploadScheduler scheduler;
    scheduler.setSessionState(QStringLiteral("S"), UploadScheduler::SessionState::Active);
    scheduler.enqueue(request(QStringLiteral("S"), QStringLiteral("active")));
    scheduler.enqueue(request(QStringLiteral("S"), QStringLiteral("queued")));

    QSignalSpy cancellations(&scheduler, &UploadScheduler::uploadCancelled);
    QSignalSpy purges(&scheduler, &UploadScheduler::sessionPurged);
    QVERIFY(scheduler.setSessionState(QStringLiteral("S"),
                                      UploadScheduler::SessionState::Terminating));

    QCOMPARE(scheduler.activeCount(), 0);
    QCOMPARE(scheduler.queuedCount(QStringLiteral("S")), 0);
    QCOMPARE(cancellations.size(), 2);
    QCOMPARE(cancellations.at(0).at(1).toBool(), true);
    QCOMPARE(cancellations.at(1).at(1).toBool(), false);
    QCOMPARE(purges.size(), 1);
    QCOMPARE(purges.at(0).at(1).toInt(), 1);
    QCOMPARE(purges.at(0).at(2).toBool(), true);

    QVERIFY(!scheduler.setSessionState(QStringLiteral("S"),
                                       UploadScheduler::SessionState::Active));
    QCOMPARE(scheduler.enqueue(request(QStringLiteral("S"), QStringLiteral("late"))),
             UploadScheduler::EnqueueResult::SessionTerminal);

    // Advancing through terminal cleanup states is still legal and idempotent.
    QVERIFY(scheduler.setSessionState(QStringLiteral("S"),
                                      UploadScheduler::SessionState::CleanupPending));
    QVERIFY(scheduler.setSessionState(QStringLiteral("S"),
                                      UploadScheduler::SessionState::Closed));
    QCOMPARE(purges.size(), 1);
}

void UploadSchedulerTest::rejectsDuplicatesAndStaleCompletion()
{
    UploadScheduler scheduler;
    scheduler.setSessionState(QStringLiteral("S"), UploadScheduler::SessionState::Active);
    const auto upload = request(QStringLiteral("S"), QStringLiteral("once"), 7);

    QCOMPARE(scheduler.enqueue(upload), UploadScheduler::EnqueueResult::Enqueued);
    QCOMPARE(scheduler.enqueue(upload), UploadScheduler::EnqueueResult::Duplicate);
    QVERIFY(!scheduler.completeUpload(QStringLiteral("S"), 6, QStringLiteral("once")));
    QVERIFY(scheduler.isUploadActive(QStringLiteral("S"), 7, QStringLiteral("once")));
    QVERIFY(scheduler.completeUpload(QStringLiteral("S"), 7, QStringLiteral("once")));
    QCOMPARE(scheduler.enqueue(upload), UploadScheduler::EnqueueResult::Duplicate);
    auto replayedOnNewGeneration = upload;
    replayedOnNewGeneration.connectionGeneration = 8;
    QCOMPARE(scheduler.enqueue(replayedOnNewGeneration),
             UploadScheduler::EnqueueResult::Duplicate);

    auto invalid = upload;
    invalid.uploadId.clear();
    QCOMPARE(scheduler.enqueue(invalid), UploadScheduler::EnqueueResult::InvalidRequest);
}

void UploadSchedulerTest::supportsDeterministicCapacityChanges()
{
    UploadScheduler scheduler(1);
    scheduler.setSessionState(QStringLiteral("A"), UploadScheduler::SessionState::Active);
    scheduler.setSessionState(QStringLiteral("B"), UploadScheduler::SessionState::Active);
    scheduler.enqueue(request(QStringLiteral("A"), QStringLiteral("A1")));
    scheduler.enqueue(request(QStringLiteral("B"), QStringLiteral("B1")));
    QCOMPARE(scheduler.activeCount(), 1);

    QVERIFY(!scheduler.setMaximumConcurrentUploads(0));
    QCOMPARE(scheduler.maximumConcurrentUploads(), 1);
    QVERIFY(scheduler.setMaximumConcurrentUploads(2));
    QCOMPARE(scheduler.activeCount(), 2);

    // Reducing the limit never interrupts already admitted work.
    QVERIFY(scheduler.setMaximumConcurrentUploads(1));
    QCOMPARE(scheduler.activeCount(), 2);
}

QTEST_GUILESS_MAIN(UploadSchedulerTest)
#include "tst_UploadScheduler.moc"
