#ifndef DEVICEIDENTITYSTORE_H
#define DEVICEIDENTITYSTORE_H

#include <QByteArray>
#include <QString>

#include <memory>

/**
 * Stable, self-certifying installation identity used by protocol v3.
 *
 * The private Ed25519 key is stored in the operating-system credential vault
 * when one is available.  A strictly owner-only file is the explicit fallback
 * for platforms/builds without a usable native vault.
 */
class DeviceIdentityStore final {
public:
    enum class StorageBackend {
        Uninitialized,
        NativeVault,
        OwnerOnlyFile
    };

    explicit DeviceIdentityStore(QString fallbackDirectory = QString(),
                                 bool preferNativeVault = true);
    ~DeviceIdentityStore();

    DeviceIdentityStore(const DeviceIdentityStore&) = delete;
    DeviceIdentityStore& operator=(const DeviceIdentityStore&) = delete;

    bool initialize(QString* errorMessage = nullptr);
    bool isReady() const;

    QString installationId() const;
    QByteArray publicKeyDer() const;
    QByteArray sign(const QByteArray& payload, QString* errorMessage = nullptr) const;

    StorageBackend storageBackend() const;
    QString storageBackendName() const;
    QString fallbackFilePath() const;

    static QString installationIdForPublicKey(const QByteArray& publicKeyDer);
    static QString endpointIdForInstallation(const QString& installationId,
                                             const QString& instanceId);

private:
    class Impl;
    std::unique_ptr<Impl> d;
};

#endif // DEVICEIDENTITYSTORE_H
