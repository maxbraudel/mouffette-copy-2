#include <QtTest>

#include "backend/security/DeviceIdentityStore.h"

#include <QFile>
#include <QTemporaryDir>
#include <QScopeGuard>
#include <QUuid>

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
    void nativeVaultNamespacesAreIndependent();
    void danglingFallbackLinkDoesNotCreateExternalKey();
    void existingIdentityReaderNeverCreatesOrRotates();
};

void DeviceIdentityStoreTest::endpointIdentityIsInstanceScoped() {
    QVERIFY(DeviceIdentityStore::instanceIdForOrdinal(0).isEmpty());
    QVERIFY(DeviceIdentityStore::instanceIdForOrdinal(-1).isEmpty());
    QCOMPARE(DeviceIdentityStore::instanceIdForOrdinal(1), QStringLiteral("primary"));
    QCOMPARE(DeviceIdentityStore::instanceIdForOrdinal(2), QStringLiteral("instance-2"));
    const QString installationId(43, QLatin1Char('i'));
    const QString primary = DeviceIdentityStore::endpointIdForInstallation(
        installationId, QStringLiteral("primary"));
    const QString secondary = DeviceIdentityStore::endpointIdForInstallation(
        installationId, DeviceIdentityStore::instanceIdForOrdinal(2));
    QCOMPARE(primary.size(), 43);
    QCOMPARE(secondary.size(), 43);
    QVERIFY(primary != secondary);
    QCOMPARE(secondary, DeviceIdentityStore::endpointIdForInstallation(
        installationId, DeviceIdentityStore::instanceIdForOrdinal(2)));
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

void DeviceIdentityStoreTest::existingIdentityReaderNeverCreatesOrRotates() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    DeviceIdentityStore missing(directory.path(), false);
    QString error;
    QVERIFY(!missing.initializeExisting(&error));
    QVERIFY(!QFileInfo::exists(missing.fallbackFilePath()));

    DeviceIdentityStore bootstrap(directory.path(), false);
    QVERIFY(bootstrap.initialize(&error));
    const QString identity = bootstrap.installationId();
    DeviceIdentityStore reader(directory.path(), false);
    QVERIFY(reader.initializeExisting(&error));
    QCOMPARE(reader.installationId(), identity);

    QFile stored(bootstrap.fallbackFilePath());
    QVERIFY(stored.open(QIODevice::WriteOnly | QIODevice::Truncate));
    QCOMPARE(stored.write("corrupt"), qint64(7));
    stored.close();
    DeviceIdentityStore corrupt(directory.path(), false);
    QVERIFY(!corrupt.initializeExisting(&error));
    QVERIFY(stored.open(QIODevice::ReadOnly));
    QCOMPARE(stored.readAll(), QByteArray("corrupt"));
    stored.close();
    QVERIFY(QFile::remove(stored.fileName()));
    DeviceIdentityStore disappeared(directory.path(), false);
    QVERIFY(!disappeared.initializeExisting(&error));
    QVERIFY(!QFileInfo::exists(stored.fileName()));
}

void DeviceIdentityStoreTest::signatureVerifiesAndRejectsTampering() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    DeviceIdentityStore identity(directory.path(), false);
    QString error;
    QVERIFY2(identity.initialize(&error), qPrintable(error));

    const QByteArray payload("mouffette-v8\nboot\nnonce\nruntime\n1");
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

void DeviceIdentityStoreTest::danglingFallbackLinkDoesNotCreateExternalKey()
{
#ifdef Q_OS_WIN
    QSKIP("QFile::link creates Windows shortcuts, not symbolic links.");
#else
    QTemporaryDir directory;
    DeviceIdentityStore identity(directory.path(), false);
    const QString external = directory.filePath("external-key.pk8");
    QVERIFY(QFile::link(external, identity.fallbackFilePath()));
    QString error;
    QCOMPARE(identity.inspectStored(&error), DeviceIdentityStore::ReadState::Corrupt);
    QVERIFY(!identity.initialize(&error));
    QVERIFY(!QFileInfo::exists(external));
    bool reset = false;
    QVERIFY2(identity.validateOrReset(&reset, &error), qPrintable(error));
    QVERIFY(reset);
    QVERIFY(!QFileInfo::exists(external));
    QVERIFY(!QFileInfo(identity.fallbackFilePath()).isSymLink());
#endif
}

void DeviceIdentityStoreTest::nativeVaultNamespacesAreIndependent()
{
#ifndef Q_OS_WIN
    QSKIP("Native Credential Manager integration runs in Windows CI; avoids macOS Keychain prompts.");
#else
    QTemporaryDir directory;
    const QString suffix = QUuid::createUuid().toString(QUuid::WithoutBraces);
    const QString devNamespace = QStringLiteral("development:test-%1").arg(suffix);
    const QString prodNamespace = QStringLiteral("production:test-%1").arg(suffix);
    DeviceIdentityStore dev(directory.filePath("dev"), true, devNamespace);
    DeviceIdentityStore prod(directory.filePath("prod"), true, prodNamespace);
    const auto cleanup = qScopeGuard([&] { dev.reset(); prod.reset(); });
    QString error;
    QVERIFY2(dev.initialize(&error), qPrintable(error));
    QVERIFY2(prod.initialize(&error), qPrintable(error));
    QCOMPARE(dev.storageBackend(), DeviceIdentityStore::StorageBackend::NativeVault);
    QCOMPARE(prod.storageBackend(), DeviceIdentityStore::StorageBackend::NativeVault);
    QVERIFY(dev.installationId() != prod.installationId());
    const QString productionId = prod.installationId();
    QVERIFY2(dev.reset(&error), qPrintable(error));
    DeviceIdentityStore restored(directory.filePath("prod"), true, prodNamespace);
    QVERIFY2(restored.initialize(&error), qPrintable(error));
    QCOMPARE(restored.installationId(), productionId);
#endif
}

QTEST_GUILESS_MAIN(DeviceIdentityStoreTest)
#include "tst_DeviceIdentityStore.moc"
