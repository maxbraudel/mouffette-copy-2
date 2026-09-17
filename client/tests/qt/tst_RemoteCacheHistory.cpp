#include <QtTest>
#include "backend/network/RemoteCacheHistory.h"

#include <atomic>
#include <thread>
#include <vector>

class RemoteCacheHistoryTest final : public QObject {
    Q_OBJECT
private slots:
    void requiresContinuousUnchangedObservation()
    {
        qint64 now = 1000;
        RemoteCacheHistory history([&now] { return now; });
        QVERIFY(!history.eligible("scope", "original"));
        now += RemoteCacheHistory::RetentionMs - 1;
        QVERIFY(!history.eligible("scope", "original"));
        ++now;
        QVERIFY(history.eligible("scope", "original"));
        QVERIFY(!history.eligible("scope", "changed"));
        now += RemoteCacheHistory::RetentionMs - 1;
        QVERIFY(!history.eligible("scope", "changed"));
        ++now;
        QVERIFY(history.eligible("scope", "changed"));
    }

    void clockResetAndProcessRestartRenewRetention()
    {
        qint64 now = 1000;
        RemoteCacheHistory history([&now] { return now; }, 100);
        QVERIFY(!history.eligible("scope", "proof"));
        now = 1100;
        QVERIFY(history.eligible("scope", "proof"));
        now = -1;
        QVERIFY(!history.eligible("scope", "proof"));
        now = 50;
        QVERIFY(!history.eligible("scope", "proof"));
        now = 149;
        QVERIFY(!history.eligible("scope", "proof"));
        now = 150;
        QVERIFY(history.eligible("scope", "proof"));
        RemoteCacheHistory restarted([&now] { return now; }, 100);
        QVERIFY(!restarted.eligible("scope", "proof"));
    }

    void saturationNeverEvictsObservedProofs()
    {
        qint64 now = 0;
        RemoteCacheHistory history([&now] { return now; }, 100, 2);
        QVERIFY(!history.eligible("first", "proof"));
        QVERIFY(!history.eligible("second", "proof"));
        now = 100;
        QVERIFY(!history.eligible("third", "proof"));
        QCOMPARE(history.size(), history.capacity());
        QVERIFY(history.eligible("first", "proof"));
        history.forget("first");
        QVERIFY(!history.eligible("third", "proof"));
        now = 199;
        QVERIFY(!history.eligible("third", "proof"));
        now = 200;
        QVERIFY(history.eligible("third", "proof"));
        QVERIFY(history.eligible("second", "proof"));
    }

    void concurrentWorkersShareOneBoundedObservationTable()
    {
        std::atomic<qint64> now{0};
        RemoteCacheHistory history([&now] { return now.load(); }, 100, 32);
        std::vector<std::thread> workers;
        for (int worker = 0; worker < 8; ++worker) {
            workers.emplace_back([&history, worker] {
                for (int i = 0; i < 1000; ++i) {
                    history.eligible(QString::number(worker * 1000 + i), "proof");
                    history.size();
                }
            });
        }
        for (auto& worker : workers) worker.join();
        QCOMPARE(history.size(), qsizetype(32));
        now = 100;
        QVERIFY(!history.eligible("new-key", "proof"));
    }
};

QTEST_GUILESS_MAIN(RemoteCacheHistoryTest)
#include "tst_RemoteCacheHistory.moc"
