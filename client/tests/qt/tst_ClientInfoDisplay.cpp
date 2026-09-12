#include <QtTest>

#include "backend/domain/models/ClientInfo.h"

class ClientInfoDisplayTest : public QObject
{
    Q_OBJECT

private slots:
    void normalizesEveryPublicAvailabilityBadge_data();
    void normalizesEveryPublicAvailabilityBadge();
    void offlinePresenceOverridesStaleSessionState();
    void formatsProjectCountdowns();
    void formatsCountdownBoundaries();
};

void ClientInfoDisplayTest::normalizesEveryPublicAvailabilityBadge_data()
{
    QTest::addColumn<QString>("wireStatus");
    QTest::addColumn<QString>("displayStatus");

    QTest::newRow("available") << QStringLiteral("Available") << QStringLiteral("Available");
    QTest::newRow("connecting") << QStringLiteral("opening") << QStringLiteral("Connecting");
    QTest::newRow("connected") << QStringLiteral("active") << QStringLiteral("Connected");
    QTest::newRow("reconnecting") << QStringLiteral("grace") << QStringLiteral("Reconnecting");
    QTest::newRow("disconnecting") << QStringLiteral("cleanup_pending") << QStringLiteral("Disconnecting");
    QTest::newRow("in-use") << QStringLiteral("in_use") << QStringLiteral("In use");
    QTest::newRow("offline") << QStringLiteral("Offline") << QStringLiteral("Offline");
    QTest::newRow("unavailable") << QStringLiteral("cleanup_error") << QStringLiteral("Unavailable");
}

void ClientInfoDisplayTest::normalizesEveryPublicAvailabilityBadge()
{
    QFETCH(QString, wireStatus);
    QFETCH(QString, displayStatus);

    ClientInfo client(QStringLiteral("device-a"), QStringLiteral("Studio A"),
                      QStringLiteral("Linux"));
    client.setAvailabilityStatus(wireStatus);
    QCOMPARE(client.availabilityBadgeText(), displayStatus);
}

void ClientInfoDisplayTest::offlinePresenceOverridesStaleSessionState()
{
    ClientInfo client(QStringLiteral("device-b"), QStringLiteral("Studio B"),
                      QStringLiteral("macOS"));
    client.setAvailabilityStatus(QStringLiteral("Connected"));
    client.setOnline(false);
    client.setHasProject(true);

    QCOMPARE(client.availabilityBadgeText(), QStringLiteral("Offline"));
    QCOMPARE(client.getProjectSummaryText(1'000), QStringLiteral("Project"));
    QVERIFY(client.getDisplayText().endsWith(QStringLiteral("— Offline")));
}

void ClientInfoDisplayTest::formatsProjectCountdowns()
{
    ClientInfo client(QStringLiteral("device-c"), QStringLiteral("Studio C"),
                      QStringLiteral("Windows"));
    client.setHasProject(true);
    client.setRemoteSessionCloseAtMs(60'000);
    client.setProjectDeleteAtMs(300'000);

    QCOMPARE(client.getProjectSummaryText(1'000),
             QStringLiteral("Project · Disconnect in 0:59 · Delete project in 4:59"));
    QCOMPARE(client.getProjectSummaryText(60'001),
             QStringLiteral("Project · Delete project in 4:00"));

    client.setHasProject(false);
    QVERIFY(client.getProjectSummaryText(1'000).isEmpty());
}

void ClientInfoDisplayTest::formatsCountdownBoundaries()
{
    QCOMPARE(ClientInfo::formatRemainingTime(-1), QStringLiteral("0:00"));
    QCOMPARE(ClientInfo::formatRemainingTime(0), QStringLiteral("0:00"));
    QCOMPARE(ClientInfo::formatRemainingTime(1), QStringLiteral("0:01"));
    QCOMPARE(ClientInfo::formatRemainingTime(59'001), QStringLiteral("1:00"));
    QCOMPARE(ClientInfo::formatRemainingTime(300'000), QStringLiteral("5:00"));
}

QTEST_APPLESS_MAIN(ClientInfoDisplayTest)
#include "tst_ClientInfoDisplay.moc"
