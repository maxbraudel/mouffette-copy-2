#include <QtTest>
#include <QTemporaryDir>
#include <QFile>
#include <QDir>
#include <QJsonDocument>
#include "backend/network/NetworkDiagnostics.h"

class NetworkDiagnosticsTest : public QObject {
    Q_OBJECT
private slots:
    void boundedRotationAndRedaction() {
        QTemporaryDir dir;
        NetworkDiagnostics::Options options;
        options.fileBytes = 1024;
        NetworkDiagnostics logger(dir.path(), options);
        for (int i = 0; i < 48; ++i) {
            logger.enqueue("socket_closed", {{"channel", "data"}, {"closeCode", 1006},
                {"remoteSessionId", "session-123"}, {"reason", "channel_unavailable"},
                {"token", "never-log-token"}, {"fileName", "private.png"},
                {"path", "/private/hidden"}, {"data", "secret-payload"}}, true);
            QTest::qWait(2);
        }
        QTRY_COMPARE_WITH_TIMEOUT(QDir(dir.path()).entryList({"network.*.jsonl"}, QDir::Files).size(), 3, 2000);
        for (const auto& name : QDir(dir.path()).entryList(QDir::Files)) {
            QFile file(dir.filePath(name));
            QVERIFY(file.open(QIODevice::ReadOnly));
            const auto bytes = file.readAll();
            QVERIFY(bytes.size() <= 1024);
            QVERIFY(!bytes.contains("never-log-token"));
            QVERIFY(!bytes.contains("private.png"));
            QVERIFY(!bytes.contains("secret-payload"));
            for (const auto& row : bytes.split('\n')) {
                if (row.isEmpty()) continue;
                const auto object = QJsonDocument::fromJson(row).object();
                QVERIFY(!object.isEmpty());
                QVERIFY(object.contains("utc"));
                QVERIFY(object.contains("monotonicMs"));
            }
        }
    }
    void saturatedAndUnavailableDiskNeverBlockProducer() {
        QTemporaryDir dir;
        QFile blocker(dir.filePath("blocked"));
        QVERIFY(blocker.open(QIODevice::WriteOnly));
        blocker.close();
        NetworkDiagnostics::Options options;
        options.queueCapacity = 2;
        NetworkDiagnostics logger(blocker.fileName() + "/logs", options);
        QElapsedTimer elapsed;
        elapsed.start();
        for (int i = 0; i < 100000; ++i)
            logger.enqueue("telemetry", {{"count", i}}, true);
        QVERIFY2(elapsed.elapsed() < 2000, "Producer blocked on unavailable log directory");
        QVERIFY(logger.droppedCount() > 0);
    }
    void disabledDoesNotCreateFiles() {
        QTemporaryDir dir;
        NetworkDiagnostics::Options options;
        options.enabled = false;
        NetworkDiagnostics logger(dir.filePath("disabled"), options);
        logger.enqueue("telemetry", {}, true);
        QVERIFY(!QDir(dir.filePath("disabled")).exists());
    }
};
QTEST_GUILESS_MAIN(NetworkDiagnosticsTest)
#include "tst_NetworkDiagnostics.moc"
