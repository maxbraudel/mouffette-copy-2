#include "backend/security/DeviceIdentityStore.h"
#include "backend/runtime/RuntimeProfile.h"

#include <QCryptographicHash>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>
#include <QStandardPaths>

#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/x509.h>

#ifdef Q_OS_MACOS
#include <Security/Security.h>
#endif

#ifdef Q_OS_WIN
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <qt_windows.h>
#include <wincred.h>
#endif

namespace {

constexpr auto kVaultService = "app.mouffette.client.device-identity.v2";
constexpr auto kVaultAccount = "default-installation";
constexpr auto kFallbackFileName = "device-identity-v2.pk8";

QByteArray toBase64Url(const QByteArray& value) {
    return value.toBase64(QByteArray::Base64UrlEncoding
                          | QByteArray::OmitTrailingEquals);
}

QString opensslError(const QString& operation) {
    return QStringLiteral("%1 failed in OpenSSL").arg(operation);
}

#ifdef Q_OS_MACOS
bool loadNativeSecret(const QByteArray& account, QByteArray* value, QString* diagnostic) {
    UInt32 length = 0;
    void* bytes = nullptr;
    SecKeychainItemRef item = nullptr;
    const OSStatus status = SecKeychainFindGenericPassword(
        nullptr,
        static_cast<UInt32>(qstrlen(kVaultService)), kVaultService,
        static_cast<UInt32>(account.size()), account.constData(),
        &length, &bytes, &item);
    if (status == errSecItemNotFound) {
        return false;
    }
    if (status != errSecSuccess) {
        if (diagnostic) {
            *diagnostic = QStringLiteral("macOS Keychain read failed (%1)").arg(status);
        }
        return false;
    }
    if (value) {
        *value = QByteArray(static_cast<const char*>(bytes), static_cast<int>(length));
    }
    SecKeychainItemFreeContent(nullptr, bytes);
    if (item) CFRelease(item);
    return true;
}

bool saveNativeSecret(const QByteArray& account, const QByteArray& value, QString* diagnostic) {
    SecKeychainItemRef item = nullptr;
    UInt32 existingLength = 0;
    void* existingBytes = nullptr;
    OSStatus status = SecKeychainFindGenericPassword(
        nullptr,
        static_cast<UInt32>(qstrlen(kVaultService)), kVaultService,
        static_cast<UInt32>(account.size()), account.constData(),
        &existingLength, &existingBytes, &item);
    if (status == errSecSuccess) {
        SecKeychainItemFreeContent(nullptr, existingBytes);
        status = SecKeychainItemModifyAttributesAndData(
            item, nullptr, static_cast<UInt32>(value.size()), value.constData());
        if (item) CFRelease(item);
    } else if (status == errSecItemNotFound) {
        status = SecKeychainAddGenericPassword(
            nullptr,
            static_cast<UInt32>(qstrlen(kVaultService)), kVaultService,
            static_cast<UInt32>(account.size()), account.constData(),
            static_cast<UInt32>(value.size()), value.constData(), nullptr);
    }
    if (status != errSecSuccess) {
        if (diagnostic) {
            *diagnostic = QStringLiteral("macOS Keychain write failed (%1)").arg(status);
        }
        return false;
    }
    return true;
}

bool removeNativeSecret(const QByteArray& account, QString* diagnostic) {
    SecKeychainItemRef item = nullptr;
    UInt32 existingLength = 0;
    void* existingBytes = nullptr;
    const OSStatus found = SecKeychainFindGenericPassword(
        nullptr,
        static_cast<UInt32>(qstrlen(kVaultService)), kVaultService,
        static_cast<UInt32>(account.size()), account.constData(),
        &existingLength, &existingBytes, &item);
    if (found == errSecItemNotFound) return true;
    if (found != errSecSuccess) {
        if (diagnostic) {
            *diagnostic = QStringLiteral("macOS Keychain lookup failed (%1)").arg(found);
        }
        return false;
    }
    SecKeychainItemFreeContent(nullptr, existingBytes);
    const OSStatus removed = SecKeychainItemDelete(item);
    if (item) CFRelease(item);
    if (removed != errSecSuccess) {
        if (diagnostic) {
            *diagnostic = QStringLiteral("macOS Keychain delete failed (%1)").arg(removed);
        }
        return false;
    }
    return true;
}

bool removeObsoleteNativeSecret(QString* diagnostic) {
    return removeNativeSecret(QByteArray(kVaultAccount), diagnostic);
}
#elif defined(Q_OS_WIN)
std::wstring vaultTargetName(const QByteArray& account) {
    return QStringLiteral("%1:%2")
        .arg(QString::fromLatin1(kVaultService), QString::fromUtf8(account))
        .toStdWString();
}

bool loadNativeSecret(const QByteArray& account, QByteArray* value, QString* diagnostic) {
    PCREDENTIALW credential = nullptr;
    const std::wstring target = vaultTargetName(account);
    if (!CredReadW(target.c_str(), CRED_TYPE_GENERIC, 0, &credential)) {
        const DWORD code = GetLastError();
        if (code != ERROR_NOT_FOUND && diagnostic) {
            *diagnostic = QStringLiteral("Windows Credential Manager read failed (%1)")
                              .arg(code);
        }
        return false;
    }
    if (value) {
        *value = QByteArray(reinterpret_cast<const char*>(credential->CredentialBlob),
                            static_cast<int>(credential->CredentialBlobSize));
    }
    CredFree(credential);
    return true;
}

bool saveNativeSecret(const QByteArray& account, const QByteArray& value, QString* diagnostic) {
    const std::wstring target = vaultTargetName(account);
    const std::wstring username = L"Mouffette";
    CREDENTIALW credential{};
    credential.Type = CRED_TYPE_GENERIC;
    credential.TargetName = const_cast<wchar_t*>(target.c_str());
    credential.CredentialBlobSize = static_cast<DWORD>(value.size());
    credential.CredentialBlob = reinterpret_cast<LPBYTE>(
        const_cast<char*>(value.constData()));
    credential.Persist = CRED_PERSIST_LOCAL_MACHINE;
    credential.UserName = const_cast<wchar_t*>(username.c_str());
    if (!CredWriteW(&credential, 0)) {
        if (diagnostic) {
            *diagnostic = QStringLiteral("Windows Credential Manager write failed (%1)")
                              .arg(GetLastError());
        }
        return false;
    }
    return true;
}

bool removeNativeSecret(const QByteArray& account, QString* diagnostic) {
    const std::wstring target = vaultTargetName(account);
    if (CredDeleteW(target.c_str(), CRED_TYPE_GENERIC, 0)) return true;
    const DWORD code = GetLastError();
    if (code == ERROR_NOT_FOUND) return true;
    if (diagnostic) {
        *diagnostic = QStringLiteral("Windows Credential Manager delete failed (%1)")
                          .arg(code);
    }
    return false;
}

bool removeObsoleteNativeSecret(QString* diagnostic) {
    const std::wstring target = QString::fromLatin1(kVaultService).toStdWString();
    if (CredDeleteW(target.c_str(), CRED_TYPE_GENERIC, 0)) return true;
    const DWORD code = GetLastError();
    if (code == ERROR_NOT_FOUND) return true;
    if (diagnostic) {
        *diagnostic = QStringLiteral("Windows Credential Manager delete failed (%1)")
                          .arg(code);
    }
    return false;
}
#else
bool loadNativeSecret(const QByteArray&, QByteArray*, QString*) { return false; }
bool saveNativeSecret(const QByteArray&, const QByteArray&, QString*) { return false; }
bool removeNativeSecret(const QByteArray&, QString*) { return true; }
bool removeObsoleteNativeSecret(QString*) { return true; }
#endif

QByteArray accountForNamespace(const QString& runtimeNamespace) {
    const QString normalized = runtimeNamespace.trimmed().isEmpty()
        ? QStringLiteral("instance-1") : runtimeNamespace.trimmed();
    const QByteArray digest = QCryptographicHash::hash(
        normalized.toUtf8(), QCryptographicHash::Sha256).toHex().left(24);
    return QByteArrayLiteral("runtime-") + digest;
}

} // namespace

class DeviceIdentityStore::Impl {
public:
    explicit Impl(QString directory, bool preferVault, QString nameSpace)
        : fallbackDirectory(std::move(directory))
        , preferNativeVault(preferVault)
        , runtimeNamespace(std::move(nameSpace))
        , vaultAccount(accountForNamespace(runtimeNamespace)) {}

    using KeyPtr = std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)>;

    QString fallbackDirectory;
    bool preferNativeVault = true;
    QString runtimeNamespace;
    QByteArray vaultAccount;
    StorageBackend backend = StorageBackend::Uninitialized;
    KeyPtr key{nullptr, EVP_PKEY_free};
    QByteArray publicDer;
    QString installationId;

    QString resolvedFallbackPath() const {
        QString directory = fallbackDirectory;
        if (directory.isEmpty()) {
            directory = RuntimeProfile::installationDataLocation();
        }
        if (directory.isEmpty()) {
            return QString();
        }
        return QDir(directory).filePath(QString::fromLatin1(kFallbackFileName));
    }

    static KeyPtr decodePrivateKey(const QByteArray& encoded) {
        if (encoded.isEmpty() || encoded.size() > 4096) {
            return KeyPtr(nullptr, EVP_PKEY_free);
        }
        const unsigned char* const start =
            reinterpret_cast<const unsigned char*>(encoded.constData());
        const unsigned char* cursor = start;
        PKCS8_PRIV_KEY_INFO* info = d2i_PKCS8_PRIV_KEY_INFO(
            nullptr, &cursor, static_cast<long>(encoded.size()));
        if (!info) return KeyPtr(nullptr, EVP_PKEY_free);
        if (cursor != start + encoded.size()) {
            PKCS8_PRIV_KEY_INFO_free(info);
            return KeyPtr(nullptr, EVP_PKEY_free);
        }
        EVP_PKEY* raw = EVP_PKCS82PKEY(info);
        PKCS8_PRIV_KEY_INFO_free(info);
        if (!raw || EVP_PKEY_base_id(raw) != EVP_PKEY_ED25519) {
            EVP_PKEY_free(raw);
            return KeyPtr(nullptr, EVP_PKEY_free);
        }
        return KeyPtr(raw, EVP_PKEY_free);
    }

    static QByteArray encodePrivateKey(EVP_PKEY* key) {
        PKCS8_PRIV_KEY_INFO* info = EVP_PKEY2PKCS8(key);
        if (!info) return {};
        const int size = i2d_PKCS8_PRIV_KEY_INFO(info, nullptr);
        if (size <= 0) {
            PKCS8_PRIV_KEY_INFO_free(info);
            return {};
        }
        QByteArray encoded(size, Qt::Uninitialized);
        unsigned char* cursor = reinterpret_cast<unsigned char*>(encoded.data());
        const int written = i2d_PKCS8_PRIV_KEY_INFO(info, &cursor);
        PKCS8_PRIV_KEY_INFO_free(info);
        if (written != size) return {};
        return encoded;
    }

    static KeyPtr generateKey() {
        EVP_PKEY_CTX* context = EVP_PKEY_CTX_new_id(EVP_PKEY_ED25519, nullptr);
        if (!context) return KeyPtr(nullptr, EVP_PKEY_free);
        EVP_PKEY* generated = nullptr;
        const bool ok = EVP_PKEY_keygen_init(context) == 1
            && EVP_PKEY_keygen(context, &generated) == 1;
        EVP_PKEY_CTX_free(context);
        if (!ok) {
            EVP_PKEY_free(generated);
            return KeyPtr(nullptr, EVP_PKEY_free);
        }
        return KeyPtr(generated, EVP_PKEY_free);
    }

    static QByteArray encodePublicKey(EVP_PKEY* key) {
        const int size = i2d_PUBKEY(key, nullptr);
        if (size <= 0) return {};
        QByteArray encoded(size, Qt::Uninitialized);
        unsigned char* cursor = reinterpret_cast<unsigned char*>(encoded.data());
        if (i2d_PUBKEY(key, &cursor) != size) return {};
        return encoded;
    }

    bool loadFallback(QByteArray* encoded, QString* errorMessage) const {
        const QString path = resolvedFallbackPath();
        if (path.isEmpty() || !QFileInfo::exists(path)) return false;
        const QFileInfo info(path);
        if (info.isSymLink() || !info.isFile()) {
            if (errorMessage) {
                *errorMessage = QStringLiteral(
                    "Device identity fallback must be a regular, non-symlink file");
            }
            return false;
        }
        QFile file(path);
        if (!file.open(QIODevice::ReadOnly)) {
            if (errorMessage) {
                *errorMessage = QStringLiteral("Cannot read device identity file: %1")
                                    .arg(file.errorString());
            }
            return false;
        }
        const QFile::Permissions expected = QFileDevice::ReadOwner | QFileDevice::WriteOwner;
        const QFile::Permissions actual = file.permissions();
        if ((actual & (QFileDevice::ReadGroup | QFileDevice::WriteGroup
                       | QFileDevice::ReadOther | QFileDevice::WriteOther)) != 0) {
            file.close();
            if (!QFile::setPermissions(path, expected)) {
                if (errorMessage) {
                    *errorMessage = QStringLiteral(
                        "Device identity file has unsafe permissions and cannot be secured");
                }
                return false;
            }
            if (!file.open(QIODevice::ReadOnly)) return false;
        }
        *encoded = file.readAll();
        if (encoded->isEmpty() || encoded->size() > 4096) {
            if (errorMessage) {
                *errorMessage = QStringLiteral(
                    "Device identity file has an invalid size");
            }
            encoded->clear();
            return false;
        }
        return true;
    }

    bool saveFallback(const QByteArray& encoded, QString* errorMessage) const {
        const QString path = resolvedFallbackPath();
        if (path.isEmpty()) {
            if (errorMessage) *errorMessage = QStringLiteral("No writable identity directory");
            return false;
        }
        QDir directory = QFileInfo(path).dir();
        if (!directory.exists() && !directory.mkpath(QStringLiteral("."))) {
            if (errorMessage) *errorMessage = QStringLiteral("Cannot create identity directory");
            return false;
        }
        QSaveFile file(path);
        file.setDirectWriteFallback(false);
        const QFile::Permissions permissions = QFileDevice::ReadOwner | QFileDevice::WriteOwner;
        if (!file.open(QIODevice::WriteOnly)
            || !file.setPermissions(permissions)
            || file.write(encoded) != encoded.size()
            || !file.commit()) {
            if (errorMessage) {
                *errorMessage = QStringLiteral("Cannot persist device identity: %1")
                                    .arg(file.errorString());
            }
            return false;
        }
        if (!QFile::setPermissions(path, permissions)) {
            QFile::remove(path);
            if (errorMessage) {
                *errorMessage = QStringLiteral("Cannot enforce owner-only identity permissions");
            }
            return false;
        }
        return true;
    }
};

DeviceIdentityStore::DeviceIdentityStore(QString fallbackDirectory,
                                         bool preferNativeVault,
                                         QString runtimeNamespace)
    : d(std::make_unique<Impl>(std::move(fallbackDirectory), preferNativeVault,
                              runtimeNamespace.trimmed().isEmpty()
                                  ? RuntimeProfile::context().profileId
                                  : std::move(runtimeNamespace))) {}

DeviceIdentityStore::~DeviceIdentityStore() = default;

bool DeviceIdentityStore::initialize(QString* errorMessage) {
    if (d->key) return true;

    QByteArray encoded;
    QString vaultDiagnostic;
    bool loadedFromVault = false;
    if (d->preferNativeVault) {
        loadedFromVault = loadNativeSecret(d->vaultAccount, &encoded, &vaultDiagnostic);
    }

    QString fallbackDiagnostic;
    bool loadedFromFile = false;
    const bool fallbackExists = QFileInfo::exists(d->resolvedFallbackPath());
    if (!loadedFromVault) {
        loadedFromFile = d->loadFallback(&encoded, &fallbackDiagnostic);
    }

    if (!loadedFromVault && !loadedFromFile
        && (fallbackExists || !vaultDiagnostic.isEmpty())) {
        if (errorMessage) {
            *errorMessage = !fallbackDiagnostic.isEmpty()
                ? fallbackDiagnostic
                : vaultDiagnostic;
        }
        return false;
    }

    if (!encoded.isEmpty()) {
        d->key = Impl::decodePrivateKey(encoded);
        if (!d->key) {
            if (errorMessage) {
                *errorMessage = QStringLiteral("Stored device identity is not a valid Ed25519 key");
            }
            return false;
        }
        d->backend = loadedFromVault ? StorageBackend::NativeVault
                                     : StorageBackend::OwnerOnlyFile;
    } else {
        d->key = Impl::generateKey();
        if (!d->key) {
            if (errorMessage) *errorMessage = opensslError(QStringLiteral("Ed25519 key generation"));
            return false;
        }
        encoded = Impl::encodePrivateKey(d->key.get());
        if (encoded.isEmpty()) {
            if (errorMessage) *errorMessage = opensslError(QStringLiteral("Private key encoding"));
            d->key.reset();
            return false;
        }

        bool saved = false;
        if (d->preferNativeVault) {
            saved = saveNativeSecret(d->vaultAccount, encoded, &vaultDiagnostic);
            if (saved) d->backend = StorageBackend::NativeVault;
        }
        if (!saved) {
            if (!d->saveFallback(encoded, errorMessage)) {
                d->key.reset();
                return false;
            }
            d->backend = StorageBackend::OwnerOnlyFile;
        }
    }

    d->publicDer = Impl::encodePublicKey(d->key.get());
    if (d->publicDer.isEmpty()) {
        if (errorMessage) *errorMessage = opensslError(QStringLiteral("Public key encoding"));
        d->key.reset();
        return false;
    }
    d->installationId = installationIdForPublicKey(d->publicDer);
    if (d->installationId.isEmpty()) {
        if (errorMessage) *errorMessage = QStringLiteral("Cannot derive device identifier");
        d->key.reset();
        return false;
    }

    if (d->backend == StorageBackend::OwnerOnlyFile) {
        const QString why = !vaultDiagnostic.isEmpty()
            ? vaultDiagnostic
            : QStringLiteral("no supported native credential vault is available");
        qWarning().noquote()
            << QStringLiteral("Device identity uses owner-only file fallback (%1): %2")
                   .arg(why, d->resolvedFallbackPath());
    }
    return true;
}

bool DeviceIdentityStore::reset(QString* errorMessage) {
    d->key.reset();
    d->publicDer.clear();
    d->installationId.clear();
    d->backend = StorageBackend::Uninitialized;

    QString nativeError;
    if (d->preferNativeVault
        && !removeNativeSecret(d->vaultAccount, &nativeError)) {
        if (errorMessage) *errorMessage = nativeError;
        return false;
    }
    const QString path = d->resolvedFallbackPath();
    const QFileInfo info(path);
    if ((info.exists() || info.isSymLink()) && !QFile::remove(path)) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("Cannot remove invalid device identity file");
        }
        return false;
    }
    if (errorMessage) errorMessage->clear();
    return true;
}

bool DeviceIdentityStore::validateOrReset(bool* wasReset, QString* errorMessage) {
    if (wasReset) *wasReset = false;
    QString validationError;
    if (initialize(&validationError)) {
        if (errorMessage) errorMessage->clear();
        return true;
    }
    QString resetError;
    if (!reset(&resetError) || !initialize(errorMessage)) {
        if (errorMessage && errorMessage->isEmpty()) {
            *errorMessage = resetError.isEmpty() ? validationError : resetError;
        }
        return false;
    }
    if (wasReset) *wasReset = true;
    if (errorMessage) errorMessage->clear();
    return true;
}

bool DeviceIdentityStore::isReady() const { return d->key != nullptr; }
QString DeviceIdentityStore::installationId() const { return d->installationId; }
QByteArray DeviceIdentityStore::publicKeyDer() const { return d->publicDer; }

QByteArray DeviceIdentityStore::sign(const QByteArray& payload, QString* errorMessage) const {
    if (!d->key) {
        if (errorMessage) *errorMessage = QStringLiteral("Device identity is not initialized");
        return {};
    }
    EVP_MD_CTX* context = EVP_MD_CTX_new();
    if (!context) {
        if (errorMessage) *errorMessage = opensslError(QStringLiteral("Signature context allocation"));
        return {};
    }
    size_t signatureSize = 0;
    const bool sized = EVP_DigestSignInit(context, nullptr, nullptr, nullptr, d->key.get()) == 1
        && EVP_DigestSign(context, nullptr, &signatureSize,
                          reinterpret_cast<const unsigned char*>(payload.constData()),
                          static_cast<size_t>(payload.size())) == 1;
    QByteArray signature;
    if (sized && signatureSize > 0 && signatureSize <= 128) {
        signature.resize(static_cast<int>(signatureSize));
        if (EVP_DigestSign(context,
                           reinterpret_cast<unsigned char*>(signature.data()), &signatureSize,
                           reinterpret_cast<const unsigned char*>(payload.constData()),
                           static_cast<size_t>(payload.size())) != 1) {
            signature.clear();
        } else {
            signature.resize(static_cast<int>(signatureSize));
        }
    }
    EVP_MD_CTX_free(context);
    if (signature.isEmpty() && errorMessage) {
        *errorMessage = opensslError(QStringLiteral("Ed25519 signature"));
    }
    return signature;
}

DeviceIdentityStore::StorageBackend DeviceIdentityStore::storageBackend() const {
    return d->backend;
}

QString DeviceIdentityStore::storageBackendName() const {
    switch (d->backend) {
    case StorageBackend::NativeVault: return QStringLiteral("native-vault");
    case StorageBackend::OwnerOnlyFile: return QStringLiteral("owner-only-file");
    case StorageBackend::Uninitialized: break;
    }
    return QStringLiteral("uninitialized");
}

QString DeviceIdentityStore::fallbackFilePath() const { return d->resolvedFallbackPath(); }

QString DeviceIdentityStore::installationIdForPublicKey(const QByteArray& publicKeyDer) {
    if (publicKeyDer.isEmpty()) return {};
    return QString::fromLatin1(toBase64Url(
        QCryptographicHash::hash(publicKeyDer, QCryptographicHash::Sha256)));
}

QString DeviceIdentityStore::endpointIdForInstallation(const QString& installationId,
                                                        const QString& instanceId) {
    if (installationId.isEmpty() || instanceId.isEmpty()) return {};
    const QByteArray material = QByteArrayLiteral("mouffette-endpoint-v1\n")
        + installationId.toUtf8() + QByteArrayLiteral("\n") + instanceId.toUtf8();
    return QString::fromLatin1(toBase64Url(
        QCryptographicHash::hash(material, QCryptographicHash::Sha256)));
}

bool DeviceIdentityStore::removeObsoleteInstallationIdentity(QString* errorMessage) {
    QString nativeError;
    if (!removeObsoleteNativeSecret(&nativeError)) {
        if (errorMessage) *errorMessage = nativeError;
        return false;
    }
    QString directory = QStandardPaths::writableLocation(
        QStandardPaths::AppLocalDataLocation);
    if (directory.isEmpty()) {
        directory = QDir(QDir::homePath()).filePath(
            QStringLiteral(".mouffette/installation"));
    }
    const QString path = QDir(directory).filePath(
        QString::fromLatin1(kFallbackFileName));
    const QFileInfo info(path);
    if ((info.exists() || info.isSymLink()) && !QFile::remove(path)) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("Cannot remove obsolete device identity file");
        }
        return false;
    }
    if (errorMessage) errorMessage->clear();
    return true;
}
