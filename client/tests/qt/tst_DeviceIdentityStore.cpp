#include <QtTest>

#include "backend/security/DeviceIdentityStore.h"

#include <QFile>
#include <QTemporaryDir>

#include <openssl/evp.h>
#include <openssl/x509.h>

class DeviceIdentityStoreTest : public QObject {
    Q_OBJECT

private slots:
    void identityIsStableAndOwnerOnly();
    void endpointIdentityIsInstanceScoped();
    void signatureVerifiesAndRejectsTampering();
    void corruptStoredIdentityIsNotSilentlyRotated();
    void storedIdentityWithTrailingDataIsRejected();
};

void DeviceIdentityStoreTest::endpointIdentityIsInstanceScoped() {
    const QString installationId(43, QLatin1Char('i'));
    const QString primary = DeviceIdentityStore::endpointIdForInstallation(
        installationId, QStringLiteral("primary"));
    const QString secondary = DeviceIdentityStore::endpointIdForInstallation(
        installationId, QStringLiteral("123e4567-e89b-42d3-a456-426614174000"));
    QCOMPARE(primary.size(), 43);
    QCOMPARE(secondary.size(), 43);
    QVERIFY(primary != secondary);
    QCOMPARE(primary, DeviceIdentityStore::endpointIdForInstallation(
                          installationId, QStringLiteral("primary")));
}

void DeviceIdentityStoreTest::identityIsStableAndOwnerOnly() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());

    DeviceIdentityStore first(directory.path(), false);
    QString error;
    QVERIFY2(first.initialize(&error), qPrintable(error));
    QCOMPARE(first.storageBackend(), DeviceIdentityStore::StorageBackend::OwnerOnlyFile);
    QCOMPARE(first.installationId().size(), 43);
    QVERIFY(!first.publicKeyDer().isEmpty());

    const QFileInfo stored(first.fallbackFilePath());
    QVERIFY(stored.exists());
#ifndef Q_OS_WIN
    const QFile::Permissions forbidden = QFileDevice::ReadGroup | QFileDevice::WriteGroup
        | QFileDevice::ReadOther | QFileDevice::WriteOther;
    QCOMPARE(stored.permissions() & forbidden, QFile::Permissions{});
#endif

    DeviceIdentityStore restored(directory.path(), false);
    QVERIFY2(restored.initialize(&error), qPrintable(error));
    QCOMPARE(restored.installationId(), first.installationId());
    QCOMPARE(restored.publicKeyDer(), first.publicKeyDer());
}

void DeviceIdentityStoreTest::signatureVerifiesAndRejectsTampering() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    DeviceIdentityStore identity(directory.path(), false);
    QString error;
    QVERIFY2(identity.initialize(&error), qPrintable(error));

    const QByteArray payload("mouffette-v3\nboot\nnonce\nruntime\nprimary");
    const QByteArray signature = identity.sign(payload, &error);
    QCOMPARE(signature.size(), 64);

    const QByteArray publicDer = identity.publicKeyDer();
    const unsigned char* cursor = reinterpret_cast<const unsigned char*>(publicDer.constData());
    EVP_PKEY* publicKey = d2i_PUBKEY(nullptr, &cursor, publicDer.size());
    QVERIFY(publicKey != nullptr);
    EVP_MD_CTX* context = EVP_MD_CTX_new();
    QVERIFY(context != nullptr);
    QCOMPARE(EVP_DigestVerifyInit(context, nullptr, nullptr, nullptr, publicKey), 1);
    QCOMPARE(EVP_DigestVerify(
                 context,
                 reinterpret_cast<const unsigned char*>(signature.constData()), signature.size(),
                 reinterpret_cast<const unsigned char*>(payload.constData()), payload.size()),
             1);
    EVP_MD_CTX_free(context);

    context = EVP_MD_CTX_new();
    QVERIFY(context != nullptr);
    QCOMPARE(EVP_DigestVerifyInit(context, nullptr, nullptr, nullptr, publicKey), 1);
    const QByteArray tampered = payload + '!';
    QCOMPARE(EVP_DigestVerify(
                 context,
                 reinterpret_cast<const unsigned char*>(signature.constData()), signature.size(),
                 reinterpret_cast<const unsigned char*>(tampered.constData()), tampered.size()),
             0);
    EVP_MD_CTX_free(context);
    EVP_PKEY_free(publicKey);
}

void DeviceIdentityStoreTest::corruptStoredIdentityIsNotSilentlyRotated() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    DeviceIdentityStore initial(directory.path(), false);
    QString error;
    QVERIFY2(initial.initialize(&error), qPrintable(error));

    QFile file(initial.fallbackFilePath());
    QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
    QCOMPARE(file.write("not-a-private-key"), qint64(17));
    file.close();
    QVERIFY(QFile::setPermissions(file.fileName(), QFileDevice::ReadOwner | QFileDevice::WriteOwner));

    DeviceIdentityStore corrupted(directory.path(), false);
    QVERIFY(!corrupted.initialize(&error));
    QVERIFY(error.contains(QStringLiteral("valid Ed25519")));
    QFile unchanged(file.fileName());
    QVERIFY(unchanged.open(QIODevice::ReadOnly));
    QCOMPARE(unchanged.readAll(), QByteArray("not-a-private-key"));
}

void DeviceIdentityStoreTest::storedIdentityWithTrailingDataIsRejected() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    DeviceIdentityStore initial(directory.path(), false);
    QString error;
    QVERIFY2(initial.initialize(&error), qPrintable(error));

    QFile file(initial.fallbackFilePath());
    QVERIFY(file.open(QIODevice::Append));
    QCOMPARE(file.write("trailing-data"), qint64(13));
    file.close();

    DeviceIdentityStore restored(directory.path(), false);
    QVERIFY(!restored.initialize(&error));
    QVERIFY(error.contains(QStringLiteral("valid Ed25519")));
}

QTEST_GUILESS_MAIN(DeviceIdentityStoreTest)
#include "tst_DeviceIdentityStore.moc"
