#include "backend/media/MediaBackendBootstrap.h"

#include <QGuiApplication>
#include <QMediaDevices>
#include <QProcess>
#include <QtTest>

class MediaBackendBootstrapTest final : public QObject {
    Q_OBJECT
private slots:
    void applicationCanExitDuringInitialization()
    {
        QProcess child;
        child.start(QCoreApplication::applicationFilePath(), {QStringLiteral("--exit-during-init")});
        QVERIFY(child.waitForStarted());
        QVERIFY(child.waitForFinished(15000));
        QCOMPARE(child.exitStatus(), QProcess::NormalExit);
        QVERIFY2(child.exitCode() == 0, child.readAllStandardError().constData());
    }

    void preparationCompletesBeforeReuseAndKeepsDeviceDiscoveryCurrent()
    {
        const auto first = MediaBackendBootstrap::initialize();
        const auto second = MediaBackendBootstrap::initialize();
        QTRY_VERIFY_WITH_TIMEOUT(first.isFinished() && second.isFinished(), 15000);
        QVERIFY2(first.result().ready, qPrintable(first.result().error));
        QVERIFY(second.result().ready);
        const auto cached = MediaBackendBootstrap::initialize();
        QVERIFY(cached.isFinished());
        QVERIFY(cached.result().ready);
        const auto output = MediaBackendBootstrap::defaultAudioOutput();
        QTRY_VERIFY_WITH_TIMEOUT(output.isFinished(), 5000);
        QCOMPARE(output.result(), QMediaDevices::defaultAudioOutput());
    }
};

int main(int argc, char** argv)
{
    QGuiApplication app(argc, argv);
    if (app.arguments().contains(QStringLiteral("--exit-during-init"))) {
        MediaBackendBootstrap::initialize();
        return 0;
    }
    MediaBackendBootstrapTest test;
    return QTest::qExec(&test, argc, argv);
}
#include "tst_MediaBackendBootstrap.moc"
