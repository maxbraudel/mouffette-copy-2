#include <QtTest>

#include "backend/domain/models/ClientInfo.h"

class ClientInfoDisplayTest : public QObject
{
    Q_OBJECT

private slots:
    void normalizesEveryPublicAvailabilityBadge_data();
    void normalizesEveryPublicAvailabilityBadge();
    void disconnectedPresenceOverridesStaleSessionState();
    void formatsProjectDeadlines();
    void formatsCountdownBoundaries();
    void appendsInstanceNumberForEveryProfile();
    void preservesInstanceNumberWhenDisconnected();
    void unknownLegacyOrdinalDoesNotInventPrimary();
};

void ClientInfoDisplayTest::normalizesEveryPublicAvailabilityBadge_data()
{
    QTest::addColumn<QString>("wireStatus");
    QTest::addColumn<QString>("displayStatus");

    QTest::newRow("available") << QStringLiteral("Available") << QStringLiteral("Available");
    QTest::newRow("connecting") << QStringLiteral("opening") << QStringLiteral("Connecting");
    QTest::newRow("connected") << QStringLiteral("active") << QStringLiteral("Connected");
    QTest::newRow("legacy-grace") << QStringLiteral("grace") << QStringLiteral("Disconnected");
    QTest::newRow("legacy-reconnecting") << QStringLiteral("Reconnecting") << QStringLiteral("Disconnected");
    QTest::newRow("disconnecting") << QStringLiteral("cleanup_pending") << QStringLiteral("Disconnecting");
    QTest::newRow("disconnected") << QStringLiteral("Disconnected") << QStringLiteral("Disconnected");
    QTest::newRow("unreachable") << QStringLiteral("Unreachable") << QStringLiteral("Unreachable");
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

void ClientInfoDisplayTest::disconnectedPresenceOverridesStaleSessionState()
{
    ClientInfo client(QStringLiteral("device-b"), QStringLiteral("Studio B"),
                      QStringLiteral("macOS"));
    client.setAvailabilityStatus(QStringLiteral("Connected"));
    client.setOnline(false);
    client.setHasProject(true);

    QCOMPARE(client.availabilityBadgeText(), QStringLiteral("Disconnected"));
    QVERIFY(client.getProjectDeadlineText(1'000).isEmpty());
    QVERIFY(client.getDisplayText().endsWith(QStringLiteral("— Disconnected")));
}

void ClientInfoDisplayTest::formatsProjectDeadlines()
{
    ClientInfo client(QStringLiteral("device-c"), QStringLiteral("Studio C"),
                      QStringLiteral("Windows"));
    client.setHasProject(true);
    client.setProjectMediaReleaseAtMs(30'000);
    client.setRemoteSessionCloseAtMs(60'000);
    client.setProjectDeleteAtMs(300'000);

    QCOMPARE(client.getProjectDeadlineText(1'000),
             QStringLiteral("Free RAM in 0:29 · Disconnect in 0:59 · Delete project in 4:59"));
    QCOMPARE(client.getProjectDeadlineText(30'000),
             QStringLiteral("Free RAM in 0:00 · Disconnect in 0:30 · Delete project in 4:30"));
    QCOMPARE(client.getProjectDeadlineText(30'001),
             QStringLiteral("Disconnect in 0:30 · Delete project in 4:30"));
    client.setProjectMediaReleaseAtMs(-1);
    QCOMPARE(client.getProjectDeadlineText(1'000),
             QStringLiteral("Disconnect in 0:59 · Delete project in 4:59"));
    QCOMPARE(client.getProjectDeadlineText(60'001),
             QStringLiteral("Delete project in 4:00"));
    QVERIFY(client.getProjectDeadlineText(300'001).isEmpty());

    client.setProjectMediaReleaseAtMs(30'000);
    client.setHasProject(false);
    QVERIFY(client.getProjectDeadlineText(1'000).isEmpty());
}

void ClientInfoDisplayTest::formatsCountdownBoundaries()
{
    QCOMPARE(ClientInfo::formatRemainingTime(-1), QStringLiteral("0:00"));
    QCOMPARE(ClientInfo::formatRemainingTime(0), QStringLiteral("0:00"));
    QCOMPARE(ClientInfo::formatRemainingTime(1), QStringLiteral("0:01"));
    QCOMPARE(ClientInfo::formatRemainingTime(59'001), QStringLiteral("1:00"));
    QCOMPARE(ClientInfo::formatRemainingTime(300'000), QStringLiteral("5:00"));
}

void ClientInfoDisplayTest::appendsInstanceNumberForEveryProfile()
{
    ClientInfo primary(QStringLiteral("endpoint-primary"),
                       QStringLiteral("Studio"), QStringLiteral("macOS"));
    primary.setInstanceOrdinal(1);
    QCOMPARE(primary.getInstanceDisplayName(), QStringLiteral("Studio (1)"));
    QCOMPARE(primary.getIdentityDisplayText(), QStringLiteral("(apple) Studio (1)"));

    ClientInfo secondary(QStringLiteral("endpoint-secondary"),
                         QStringLiteral("Studio"), QStringLiteral("macOS"));
    secondary.setInstanceOrdinal(3);
    QCOMPARE(secondary.getInstanceDisplayName(), QStringLiteral("Studio (3)"));
    QCOMPARE(secondary.getIdentityDisplayText(), QStringLiteral("(apple) Studio (3)"));
    secondary.setMachineName(QStringLiteral("   "));
    QCOMPARE(secondary.getInstanceDisplayName(), QStringLiteral("Unnamed client (3)"));
}

void ClientInfoDisplayTest::preservesInstanceNumberWhenDisconnected()
{
    ClientInfo client(QStringLiteral("endpoint-7"), QStringLiteral("  Studio  "),
                      QStringLiteral("Linux"));
    client.setInstanceId(QStringLiteral("instance-7"));
    client.setInstanceOrdinal(7);
    client.setOnline(false);
    client.setStatus(QStringLiteral("Disconnected"));
    client.setAvailabilityStatus(QStringLiteral("Disconnected"));
    const ClientInfo restored = ClientInfo::fromJson(client.toJson());
    QCOMPARE(restored.getInstanceDisplayName(), QStringLiteral("Studio (7)"));
    QCOMPARE(restored.getDisplayText(), QStringLiteral("(linux) Studio (7) — Disconnected"));
}

void ClientInfoDisplayTest::unknownLegacyOrdinalDoesNotInventPrimary()
{
    ClientInfo client(QStringLiteral("legacy"), QStringLiteral("Studio"), QStringLiteral("Linux"));
    client.setInstanceOrdinal(0);
    QCOMPARE(client.getInstanceDisplayName(), QStringLiteral("Studio"));
    QCOMPARE(client.getIdentityDisplayText(), QStringLiteral("(linux) Studio"));
}

QTEST_APPLESS_MAIN(ClientInfoDisplayTest)
#include "tst_ClientInfoDisplay.moc"
