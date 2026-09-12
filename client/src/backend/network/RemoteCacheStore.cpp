#include "backend/network/RemoteCacheStore.h"
#include "backend/runtime/RuntimeProfile.h"

#include <QCryptographicHash>
#include <QByteArrayView>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFutureWatcher>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QRegularExpression>
#include <QSaveFile>
#include <QUuid>
#include <QtConcurrentRun>

#ifdef Q_OS_WIN
#include <qt_windows.h>
#else
#include <cerrno>
#include <fcntl.h>
#include <unistd.h>
#if defined(Q_OS_MACOS)
#include <sys/stdio.h>
#elif defined(Q_OS_LINUX)
#include <sys/syscall.h>
#endif
#endif

namespace {

constexpr auto kStateDirectory = ".remote-cache-state";
constexpr auto kIntentDirectory = "intents";
constexpr auto kTombstoneDirectory = "tombstones";
constexpr auto kAssetRemovalIntentDirectory = "asset-removal-intents";
constexpr auto kAssetRemovalTombstoneDirectory = "asset-removals";
constexpr auto kCleanupErrorDirectory = "cleanup-errors";
constexpr auto kQuarantineDirectory = ".quarantine";
constexpr char kHashSeparator[] = {'\0'};
constexpr qint64 kMaximumAssetRemovalBytes = 16LL * 1024 * 1024 * 1024;
constexpr qsizetype kCompactPathTokenHexLength = 24;

const QFileDevice::Permissions kOwnerDirectoryPermissions =
    QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner;

QString compactPathToken(const QString& identity)
{
    return QString::fromLatin1(
        QCryptographicHash::hash(identity.toUtf8(), QCryptographicHash::Sha256)
            .toHex()
            .left(kCompactPathTokenHexLength));
}

QString utcNow()
{
    return QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs);
}

QString generationString(quint64 generation)
{
    return QString::number(generation);
}

bool parseGeneration(const QJsonValue& value, quint64* output)
{
    if (!output || !value.isString()) {
        return false;
    }
    bool ok = false;
    const quint64 generation = value.toString().toULongLong(&ok, 10);
    if (!ok || generation == 0 || generationString(generation) != value.toString()) {
        return false;
    }
    *output = generation;
    return true;
}

QJsonObject scopeJson(const RemoteCacheStore::Scope& scope)
{
    return {
        {QStringLiteral("senderEndpointId"), scope.senderEndpointId},
        {QStringLiteral("remoteSessionId"), scope.remoteSessionId},
        {QStringLiteral("generation"), generationString(scope.generation)}
    };
}

bool parseScope(const QJsonObject& object, RemoteCacheStore::Scope* scope)
{
    if (!scope) {
        return false;
    }
    RemoteCacheStore::Scope parsed;
    parsed.senderEndpointId = object.value(QStringLiteral("senderEndpointId")).toString();
    parsed.remoteSessionId = object.value(QStringLiteral("remoteSessionId")).toString();
    if (!parseGeneration(object.value(QStringLiteral("generation")),
                         &parsed.generation)
        || !RemoteCacheStore::isValidEndpointId(parsed.senderEndpointId)
        || !RemoteCacheStore::isValidSessionId(parsed.remoteSessionId)) {
        return false;
    }
    *scope = parsed;
    return true;
}

QString normalizedPath(const QString& path)
{
    return QDir::cleanPath(QFileInfo(path).absoluteFilePath());
}

bool isDirectChild(const QString& parentPath, const QString& childPath)
{
    const QString parent = normalizedPath(parentPath);
    const QString child = normalizedPath(childPath);
    return QFileInfo(child).absolutePath() == parent && child != parent;
}

bool syncDirectory(const QString& path)
{
#ifdef Q_OS_WIN
    Q_UNUSED(path)
    // QSaveFile and the native directory rename provide the available durable
    // guarantees on Windows.  Opening a directory for fsync is not portable.
    return true;
#else
    const QByteArray nativePath = QFile::encodeName(path);
#ifdef O_DIRECTORY
    const int descriptor = ::open(nativePath.constData(), O_RDONLY | O_DIRECTORY);
#else
    const int descriptor = ::open(nativePath.constData(), O_RDONLY);
#endif
    if (descriptor < 0) {
        return false;
    }
    const bool ok = ::fsync(descriptor) == 0;
    ::close(descriptor);
    return ok;
#endif
}

bool inspectTree(const QString& path, qint64* byteCount, QString* errorCode)
{
    QFileInfo info(path);
    if (!info.exists() && !info.isSymLink()) {
        if (errorCode) {
            *errorCode = QStringLiteral("cache_entry_missing");
        }
        return false;
    }
    if (info.isSymLink()) {
        if (errorCode) {
            *errorCode = QStringLiteral("symlink_refused");
        }
        return false;
    }
    if (info.isFile()) {
        if (byteCount) {
            *byteCount += qMax<qint64>(0, info.size());
        }
        return true;
    }
    if (!info.isDir()) {
        if (errorCode) {
            *errorCode = QStringLiteral("unsupported_cache_entry");
        }
        return false;
    }

    const QFileInfoList children = QDir(path).entryInfoList(
        QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System,
        QDir::Name);
    for (const QFileInfo& child : children) {
        if (!inspectTree(child.absoluteFilePath(), byteCount, errorCode)) {
            return false;
        }
    }
    return true;
}

bool deleteTreeWithoutFollowingLinks(const QString& path,
                                     qint64* removedBytes,
                                     QString* errorCode)
{
    QFileInfo info(path);
    if (!info.exists() && !info.isSymLink()) {
        return true;
    }
    if (info.isSymLink()) {
        if (errorCode) {
            *errorCode = QStringLiteral("symlink_refused");
        }
        return false;
    }
    if (info.isFile()) {
        const qint64 size = qMax<qint64>(0, info.size());
        QFile file(path);
        if (!file.remove()) {
            if (errorCode) {
                *errorCode = QStringLiteral("physical_delete_failed");
            }
            return false;
        }
        if (removedBytes) {
            *removedBytes += size;
        }
        return true;
    }
    if (!info.isDir()) {
        if (errorCode) {
            *errorCode = QStringLiteral("unsupported_cache_entry");
        }
        return false;
    }

    const QFileInfoList children = QDir(path).entryInfoList(
        QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System,
        QDir::Name);
    for (const QFileInfo& child : children) {
        if (!deleteTreeWithoutFollowingLinks(child.absoluteFilePath(),
                                             removedBytes,
                                             errorCode)) {
            return false;
        }
    }
    if (!QDir().rmdir(path)) {
        if (errorCode) {
            *errorCode = QStringLiteral("physical_delete_failed");
        }
        return false;
    }
    return true;
}

QString cleanupStateToString(RemoteCacheStore::SessionState state)
{
    switch (state) {
    case RemoteCacheStore::SessionState::CleanupPending:
        return QStringLiteral("pending");
    case RemoteCacheStore::SessionState::CleanupError:
        return QStringLiteral("error");
    case RemoteCacheStore::SessionState::Closed:
        return QStringLiteral("deleted");
    default:
        return QStringLiteral("pending");
    }
}

RemoteCacheStore::SessionState cleanupStateFromString(const QString& state)
{
    if (state == QLatin1String("pending")) {
        return RemoteCacheStore::SessionState::CleanupPending;
    }
    if (state == QLatin1String("error")) {
        return RemoteCacheStore::SessionState::CleanupError;
    }
    return RemoteCacheStore::SessionState::Closed;
}

bool isSafeOpaqueIdentifier(const QString& value)
{
    static const QRegularExpression expression(
        QStringLiteral("^[A-Za-z0-9_-]{1,128}$"));
    if (!expression.match(value).hasMatch()) {
        return false;
    }
    static const QRegularExpression windowsReserved(
        QStringLiteral("^(con|prn|aux|nul|com[1-9]|lpt[1-9])$"),
        QRegularExpression::CaseInsensitiveOption);
    return !windowsReserved.match(value).hasMatch();
}

bool isValidSha256(const QString& value)
{
    static const QRegularExpression expression(
        QStringLiteral("^[0-9a-f]{64}$"));
    return expression.match(value).hasMatch();
}

bool parseCanonicalNonNegativeInteger(const QJsonValue& value, qint64* output)
{
    if (!output || !value.isString()) {
        return false;
    }
    bool ok = false;
    const qint64 parsed = value.toString().toLongLong(&ok, 10);
    if (!ok || parsed < 0 || QString::number(parsed) != value.toString()) {
        return false;
    }
    *output = parsed;
    return true;
}

QJsonObject assetRemovalTupleJson(
    const RemoteCacheStore::Scope& scope,
    const RemoteCacheStore::AssetRemovalDescriptor& descriptor)
{
    QJsonObject object = scopeJson(scope);
    object.insert(QStringLiteral("removalId"), descriptor.removalId);
    object.insert(QStringLiteral("uploadId"), descriptor.uploadId);
    object.insert(QStringLiteral("assetId"), descriptor.assetId);
    object.insert(QStringLiteral("fileId"), descriptor.fileId);
    object.insert(QStringLiteral("sha256"), descriptor.sha256);
    object.insert(QStringLiteral("offset"), QString::number(descriptor.offset));
    object.insert(QStringLiteral("size"), QString::number(descriptor.size));
    object.insert(QStringLiteral("extension"), descriptor.extension);
    return object;
}

bool parseAssetRemovalDescriptor(
    const QJsonObject& object,
    RemoteCacheStore::AssetRemovalDescriptor* descriptor)
{
    if (!descriptor) {
        return false;
    }
    RemoteCacheStore::AssetRemovalDescriptor parsed;
    parsed.removalId = object.value(QStringLiteral("removalId")).toString();
    parsed.uploadId = object.value(QStringLiteral("uploadId")).toString();
    parsed.assetId = object.value(QStringLiteral("assetId")).toString();
    parsed.fileId = object.value(QStringLiteral("fileId")).toString();
    parsed.sha256 = object.value(QStringLiteral("sha256")).toString();
    parsed.extension = object.value(QStringLiteral("extension")).toString();
    if (!parseCanonicalNonNegativeInteger(
            object.value(QStringLiteral("offset")), &parsed.offset)
        || !parseCanonicalNonNegativeInteger(
            object.value(QStringLiteral("size")), &parsed.size)) {
        return false;
    }
    *descriptor = parsed;
    return true;
}

bool assetRemovalDescriptorsEqual(
    const RemoteCacheStore::AssetRemovalDescriptor& left,
    const RemoteCacheStore::AssetRemovalDescriptor& right)
{
    return left.removalId == right.removalId
        && left.uploadId == right.uploadId
        && left.assetId == right.assetId
        && left.fileId == right.fileId
        && left.sha256 == right.sha256
        && left.offset == right.offset
        && left.size == right.size
        && left.extension == right.extension;
}

#ifdef Q_OS_WIN
QString extendedWindowsPath(const QString& path)
{
    QString native = QDir::toNativeSeparators(
        QFileInfo(path).absoluteFilePath());
    if (native.startsWith(QStringLiteral("\\\\?\\"))) {
        return native;
    }
    if (native.startsWith(QStringLiteral("\\\\"))) {
        return QStringLiteral("\\\\?\\UNC\\") + native.mid(2);
    }
    return QStringLiteral("\\\\?\\") + native;
}
#endif

enum class AtomicRenameStatus {
    Renamed,
    DestinationExists,
    CrossDevice,
    Unsupported,
    Failed
};

AtomicRenameStatus atomicRenameNoReplace(const QString& source,
                                         const QString& destination)
{
#ifdef Q_OS_WIN
    const QString nativeSource = extendedWindowsPath(source);
    const QString nativeDestination = extendedWindowsPath(destination);
    if (MoveFileExW(reinterpret_cast<LPCWSTR>(nativeSource.utf16()),
                    reinterpret_cast<LPCWSTR>(nativeDestination.utf16()),
                    MOVEFILE_WRITE_THROUGH)) {
        return AtomicRenameStatus::Renamed;
    }
    const DWORD nativeError = GetLastError();
    if (nativeError == ERROR_ALREADY_EXISTS
        || nativeError == ERROR_FILE_EXISTS) {
        return AtomicRenameStatus::DestinationExists;
    }
    if (nativeError == ERROR_NOT_SAME_DEVICE) {
        return AtomicRenameStatus::CrossDevice;
    }
    if (nativeError == ERROR_CALL_NOT_IMPLEMENTED
        || nativeError == ERROR_NOT_SUPPORTED) {
        return AtomicRenameStatus::Unsupported;
    }
    return AtomicRenameStatus::Failed;
#elif defined(Q_OS_MACOS)
    const QByteArray nativeSource = QFile::encodeName(source);
    const QByteArray nativeDestination = QFile::encodeName(destination);
    if (::renamex_np(nativeSource.constData(), nativeDestination.constData(),
                     RENAME_EXCL) == 0) {
        return AtomicRenameStatus::Renamed;
    }
    const int nativeError = errno;
#elif defined(Q_OS_LINUX) && defined(SYS_renameat2)
    const QByteArray nativeSource = QFile::encodeName(source);
    const QByteArray nativeDestination = QFile::encodeName(destination);
    constexpr unsigned int kRenameNoReplace = 1U;
    if (::syscall(SYS_renameat2, AT_FDCWD, nativeSource.constData(),
                  AT_FDCWD, nativeDestination.constData(),
                  kRenameNoReplace) == 0) {
        return AtomicRenameStatus::Renamed;
    }
    const int nativeError = errno;
#else
    Q_UNUSED(source)
    Q_UNUSED(destination)
    return AtomicRenameStatus::Unsupported;
#endif

#if defined(Q_OS_MACOS) \
    || (defined(Q_OS_LINUX) && defined(SYS_renameat2))
    if (nativeError == EEXIST || nativeError == ENOTEMPTY) {
        return AtomicRenameStatus::DestinationExists;
    }
    if (nativeError == EXDEV) {
        return AtomicRenameStatus::CrossDevice;
    }
    if (nativeError == EINVAL
#ifdef ENOTSUP
        || nativeError == ENOTSUP
#endif
#ifdef EOPNOTSUPP
        || nativeError == EOPNOTSUPP
#endif
    ) {
        return AtomicRenameStatus::Unsupported;
    }
    return AtomicRenameStatus::Failed;
#endif
}

QString atomicRenameErrorCode(AtomicRenameStatus status, bool assetRemoval)
{
    switch (status) {
    case AtomicRenameStatus::DestinationExists:
        return assetRemoval
            ? QStringLiteral("asset_quarantine_destination_exists")
            : QStringLiteral("quarantine_destination_exists");
    case AtomicRenameStatus::CrossDevice:
        return assetRemoval
            ? QStringLiteral("asset_quarantine_cross_device")
            : QStringLiteral("quarantine_cross_device");
    case AtomicRenameStatus::Unsupported:
        return QStringLiteral("atomic_rename_not_supported");
    case AtomicRenameStatus::Failed:
        return assetRemoval
            ? QStringLiteral("asset_quarantine_rename_failed")
            : QStringLiteral("quarantine_rename_failed");
    case AtomicRenameStatus::Renamed:
        return {};
    }
    return assetRemoval ? QStringLiteral("asset_quarantine_rename_failed")
                        : QStringLiteral("quarantine_rename_failed");
}

} // namespace

struct RemoteCacheStore::DeleteResult {
    Tombstone tombstone;
    QString quarantineEntry;
    QString tombstoneFile;
    QString cleanupErrorFile;
    bool orphan = false;
    bool success = false;
    qint64 bytesRemoved = 0;
    QString errorCode;
};

RemoteCacheStore::RemoteCacheStore(QString rootPath, QObject* parent)
    : QObject(parent)
    , m_rootPath(normalizedPath(rootPath))
    , m_statePath(QDir(m_rootPath).filePath(QLatin1String(kStateDirectory)))
    , m_intentsPath(QDir(m_statePath).filePath(QLatin1String(kIntentDirectory)))
    , m_tombstonesPath(QDir(m_statePath).filePath(QLatin1String(kTombstoneDirectory)))
    , m_cleanupErrorsPath(QDir(m_statePath).filePath(QLatin1String(kCleanupErrorDirectory)))
    , m_assetRemovalIntentsPath(
          QDir(m_statePath).filePath(QLatin1String(kAssetRemovalIntentDirectory)))
    , m_assetRemovalTombstonesPath(
          QDir(m_statePath).filePath(QLatin1String(kAssetRemovalTombstoneDirectory)))
    , m_quarantinePath(QDir(m_rootPath).filePath(QLatin1String(kQuarantineDirectory)))
{
}

RemoteCacheStore::~RemoteCacheStore() = default;

QString RemoteCacheStore::defaultRootPath()
{
    const QString base = RuntimeProfile::cacheLocation();
    return normalizedPath(QDir(base).filePath(QStringLiteral("Uploads")));
}

bool RemoteCacheStore::isValidEndpointId(const QString& value)
{
    return isSafeOpaqueIdentifier(value);
}

bool RemoteCacheStore::isValidSessionId(const QString& value)
{
    return isSafeOpaqueIdentifier(value);
}

bool RemoteCacheStore::isValidTeardownId(const QString& value)
{
    static const QRegularExpression canonicalUuid(
        QStringLiteral("^[0-9a-f]{8}-[0-9a-f]{4}-[1-5][0-9a-f]{3}-[89ab][0-9a-f]{3}-[0-9a-f]{12}$"));
    return canonicalUuid.match(value).hasMatch();
}

bool RemoteCacheStore::isValidAssetId(const QString& value)
{
    return isSafeOpaqueIdentifier(value);
}

void RemoteCacheStore::setError(const QString& code, QString* output) const
{
    m_lastErrorCode = code;
    if (output) {
        *output = code;
    }
}

bool RemoteCacheStore::validateScope(const Scope& scope, QString* errorCode) const
{
    if (!isValidEndpointId(scope.senderEndpointId)) {
        setError(QStringLiteral("invalid_sender_endpoint_id"), errorCode);
        return false;
    }
    if (!isValidSessionId(scope.remoteSessionId)) {
        setError(QStringLiteral("invalid_remote_session_id"), errorCode);
        return false;
    }
    if (scope.generation == 0) {
        setError(QStringLiteral("invalid_remote_session_generation"), errorCode);
        return false;
    }
    return true;
}

QString RemoteCacheStore::scopeKey(const Scope& scope) const
{
    QCryptographicHash hash(QCryptographicHash::Sha256);
    hash.addData(scope.senderEndpointId.toUtf8());
    hash.addData(QByteArrayView(kHashSeparator, 1));
    hash.addData(scope.remoteSessionId.toUtf8());
    return QString::fromLatin1(hash.result().toHex());
}

QString RemoteCacheStore::scopeDirectory(const Scope& scope) const
{
    const QString senderDirectory = QDir(m_rootPath).filePath(scope.senderEndpointId);
    return QDir(senderDirectory).filePath(scope.remoteSessionId);
}

QString RemoteCacheStore::intentPath(const Scope& scope) const
{
    return QDir(m_intentsPath).filePath(scopeKey(scope) + QStringLiteral(".json"));
}

QString RemoteCacheStore::tombstonePath(const Scope& scope) const
{
    return QDir(m_tombstonesPath).filePath(scopeKey(scope) + QStringLiteral(".json"));
}

QString RemoteCacheStore::quarantineName(const Scope& scope,
                                         const QString& teardownId) const
{
    QCryptographicHash hash(QCryptographicHash::Sha256);
    hash.addData(scope.senderEndpointId.toUtf8());
    hash.addData(QByteArrayView(kHashSeparator, 1));
    hash.addData(scope.remoteSessionId.toUtf8());
    hash.addData(QByteArrayView(kHashSeparator, 1));
    hash.addData(generationString(scope.generation).toLatin1());
    hash.addData(QByteArrayView(kHashSeparator, 1));
    hash.addData(teardownId.toLatin1());
    return QStringLiteral("q-") + QString::fromLatin1(hash.result().toHex());
}

QString RemoteCacheStore::assetRemovalTombstonePath(
    const QString& removalId) const
{
    const QByteArray digest = QCryptographicHash::hash(
        removalId.toUtf8(), QCryptographicHash::Sha256).toHex();
    return QDir(m_assetRemovalTombstonesPath)
        .filePath(QString::fromLatin1(digest) + QStringLiteral(".json"));
}

QString RemoteCacheStore::assetRemovalIntentPath(const QString& removalId) const
{
    const QByteArray digest = QCryptographicHash::hash(
        removalId.toUtf8(), QCryptographicHash::Sha256).toHex();
    return QDir(m_assetRemovalIntentsPath)
        .filePath(QString::fromLatin1(digest) + QStringLiteral(".json"));
}

QString RemoteCacheStore::assetQuarantineName(const Scope& scope,
                                              const QString& assetId,
                                              const QString& removalId) const
{
    QCryptographicHash hash(QCryptographicHash::Sha256);
    hash.addData(scope.senderEndpointId.toUtf8());
    hash.addData(QByteArrayView(kHashSeparator, 1));
    hash.addData(scope.remoteSessionId.toUtf8());
    hash.addData(QByteArrayView(kHashSeparator, 1));
    hash.addData(generationString(scope.generation).toLatin1());
    hash.addData(QByteArrayView(kHashSeparator, 1));
    hash.addData(assetId.toUtf8());
    hash.addData(QByteArrayView(kHashSeparator, 1));
    hash.addData(removalId.toUtf8());
    return QStringLiteral("a-") + QString::fromLatin1(hash.result().toHex());
}

QString RemoteCacheStore::quarantinePath(const QString& entryName) const
{
    return QDir(m_quarantinePath).filePath(entryName);
}

bool RemoteCacheStore::ensurePrivateDirectory(const QString& path,
                                              QString* errorCode) const
{
    QFileInfo info(path);
    if (info.isSymLink()) {
        setError(QStringLiteral("symlink_refused"), errorCode);
        return false;
    }
    if (info.exists() && !info.isDir()) {
        setError(QStringLiteral("cache_path_not_directory"), errorCode);
        return false;
    }
    if (!info.exists() && !QDir().mkpath(path)) {
        setError(QStringLiteral("cache_directory_create_failed"), errorCode);
        return false;
    }
    info.refresh();
    if (info.isSymLink() || !info.isDir()) {
        setError(QStringLiteral("unsafe_cache_directory"), errorCode);
        return false;
    }
    if (!QFile::setPermissions(path, kOwnerDirectoryPermissions)) {
        setError(QStringLiteral("cache_permissions_failed"), errorCode);
        return false;
    }
    return true;
}

bool RemoteCacheStore::writeJsonAtomically(const QString& path,
                                           const QJsonObject& object,
                                           QString* errorCode) const
{
    const QFileInfo parentInfo(QFileInfo(path).absolutePath());
    if (!parentInfo.isDir() || parentInfo.isSymLink()
        || !isDirectChild(parentInfo.absoluteFilePath(), path)) {
        setError(QStringLiteral("unsafe_metadata_path"), errorCode);
        return false;
    }
    QFileInfo existing(path);
    if (existing.isSymLink() || (existing.exists() && !existing.isFile())) {
        setError(QStringLiteral("unsafe_metadata_entry"), errorCode);
        return false;
    }

    QSaveFile file(path);
    file.setDirectWriteFallback(false);
    if (!file.open(QIODevice::WriteOnly)) {
        setError(QStringLiteral("metadata_open_failed"), errorCode);
        return false;
    }
    const QByteArray bytes = QJsonDocument(object).toJson(QJsonDocument::Compact);
    if (file.write(bytes) != bytes.size() || !file.flush()) {
        file.cancelWriting();
        setError(QStringLiteral("metadata_write_failed"), errorCode);
        return false;
    }
    if (!file.commit()) {
        setError(QStringLiteral("metadata_commit_failed"), errorCode);
        return false;
    }
    // QSaveFile commits and syncs the file itself; sync the directory entry
    // where the platform exposes that operation.
    if (!syncDirectory(QFileInfo(path).absolutePath())) {
        setError(QStringLiteral("metadata_directory_sync_failed"), errorCode);
        return false;
    }
    return true;
}

bool RemoteCacheStore::loadJsonObject(const QString& path,
                                      QJsonObject* object,
                                      QString* errorCode) const
{
    if (!object) {
        setError(QStringLiteral("missing_metadata_output"), errorCode);
        return false;
    }
    const QFileInfo parentInfo(QFileInfo(path).absolutePath());
    if (!parentInfo.isDir() || parentInfo.isSymLink()) {
        setError(QStringLiteral("unsafe_metadata_path"), errorCode);
        return false;
    }
    QFileInfo info(path);
    if (info.isSymLink() || !info.isFile()) {
        setError(QStringLiteral("unsafe_metadata_entry"), errorCode);
        return false;
    }
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        setError(QStringLiteral("metadata_open_failed"), errorCode);
        return false;
    }
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(file.readAll(), &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
        setError(QStringLiteral("metadata_invalid"), errorCode);
        return false;
    }
    *object = document.object();
    return true;
}

bool RemoteCacheStore::initialize(QString* errorCode)
{
    m_lastErrorCode.clear();
    const QStringList privateDirectories = {
        m_rootPath,
        m_statePath,
        m_intentsPath,
        m_tombstonesPath,
        m_cleanupErrorsPath,
        m_assetRemovalIntentsPath,
        m_assetRemovalTombstonesPath,
        m_quarantinePath
    };
    for (const QString& directory : privateDirectories) {
        if (!ensurePrivateDirectory(directory, errorCode)) {
            m_initialized = false;
            return false;
        }
    }

    m_initialized = true;
    if (!recoverAssetRemovalIntents(errorCode)
        || !recoverIntents(errorCode)
        || !quarantineAbandonedSessions(errorCode)
        || !sweepQuarantine(errorCode)) {
        return false;
    }
    return true;
}

bool RemoteCacheStore::ensureSession(const Scope& scope, QString* errorCode)
{
    m_lastErrorCode.clear();
    if (!m_initialized) {
        setError(QStringLiteral("cache_store_not_initialized"), errorCode);
        return false;
    }
    if (!validateScope(scope, errorCode)) {
        return false;
    }

    const QString tombstoneFile = tombstonePath(scope);
    if (QFileInfo::exists(tombstoneFile)) {
        setError(QStringLiteral("session_terminal"), errorCode);
        return false;
    }
    if (QFileInfo::exists(intentPath(scope))) {
        setError(QStringLiteral("session_terminating"), errorCode);
        return false;
    }

    const QString senderDirectory = QDir(m_rootPath).filePath(scope.senderEndpointId);
    const QString sessionDirectory = scopeDirectory(scope);
    if (!isDirectChild(m_rootPath, senderDirectory)
        || !isDirectChild(senderDirectory, sessionDirectory)
        || !ensurePrivateDirectory(senderDirectory, errorCode)
        || !ensurePrivateDirectory(sessionDirectory, errorCode)) {
        return false;
    }

    const QString descriptorPath = QDir(sessionDirectory).filePath(QStringLiteral(".scope.json"));
    if (QFileInfo::exists(descriptorPath)) {
        QJsonObject descriptor;
        Scope existingScope;
        if (!loadJsonObject(descriptorPath, &descriptor, errorCode)
            || descriptor.value(QStringLiteral("schemaVersion")).toInt(-1) != MetadataSchemaVersion
            || !parseScope(descriptor, &existingScope)) {
            setError(QStringLiteral("scope_metadata_invalid"), errorCode);
            return false;
        }
        if (existingScope.senderEndpointId != scope.senderEndpointId
            || existingScope.remoteSessionId != scope.remoteSessionId
            || existingScope.generation > scope.generation) {
            setError(QStringLiteral("remote_session_generation_conflict"), errorCode);
            return false;
        }
        if (existingScope.generation < scope.generation) {
            descriptor.insert(QStringLiteral("generation"),
                              generationString(scope.generation));
            descriptor.insert(QStringLiteral("generationUpdatedAt"), utcNow());
            return writeJsonAtomically(descriptorPath, descriptor, errorCode);
        }
        return true;
    }

    QJsonObject descriptor = scopeJson(scope);
    descriptor.insert(QStringLiteral("schemaVersion"), MetadataSchemaVersion);
    descriptor.insert(QStringLiteral("createdAt"), utcNow());
    return writeJsonAtomically(descriptorPath, descriptor, errorCode);
}

bool RemoteCacheStore::rebindSessionGeneration(const Scope& currentScope,
                                               quint64 newGeneration,
                                               QString* errorCode)
{
    m_lastErrorCode.clear();
    if (!m_initialized) {
        setError(QStringLiteral("cache_store_not_initialized"), errorCode);
        return false;
    }
    if (!validateScope(currentScope, errorCode) || newGeneration == 0
        || newGeneration <= currentScope.generation) {
        if (m_lastErrorCode.isEmpty()) {
            setError(QStringLiteral("invalid_generation_transition"), errorCode);
        }
        return false;
    }
    if (QFileInfo::exists(intentPath(currentScope))
        || QFileInfo::exists(tombstonePath(currentScope))) {
        setError(QStringLiteral("session_terminal"), errorCode);
        return false;
    }

    const QString descriptorPath = QDir(scopeDirectory(currentScope))
                                       .filePath(QStringLiteral(".scope.json"));
    QJsonObject descriptor;
    Scope storedScope;
    if (!loadJsonObject(descriptorPath, &descriptor, errorCode)
        || descriptor.value(QStringLiteral("schemaVersion")).toInt(-1)
            != MetadataSchemaVersion
        || !parseScope(descriptor, &storedScope)
        || !(storedScope == currentScope)) {
        setError(QStringLiteral("remote_session_generation_conflict"), errorCode);
        return false;
    }
    descriptor.insert(QStringLiteral("generation"),
                      generationString(newGeneration));
    descriptor.insert(QStringLiteral("generationUpdatedAt"), utcNow());
    return writeJsonAtomically(descriptorPath, descriptor, errorCode);
}

QString RemoteCacheStore::assetPath(const Scope& scope,
                                    const QString& assetId,
                                    AssetArea area,
                                    const QString& extension,
                                    QString* errorCode)
{
    m_lastErrorCode.clear();
    if (!isValidAssetId(assetId)) {
        setError(QStringLiteral("invalid_asset_id"), errorCode);
        return {};
    }
    static const QRegularExpression extensionPattern(QStringLiteral("^[a-z0-9]{1,16}$"));
    const QString normalizedExtension = extension.toLower();
    if (extension != extension.trimmed()
        || (!normalizedExtension.isEmpty()
            && !extensionPattern.match(normalizedExtension).hasMatch())) {
        setError(QStringLiteral("invalid_asset_extension"), errorCode);
        return {};
    }
    if (!ensureSession(scope, errorCode)) {
        return {};
    }

    const QString areaName = area == AssetArea::Staging
        ? QStringLiteral("staging")
        : QStringLiteral("validated");
    const QString areaDirectory = QDir(scopeDirectory(scope)).filePath(areaName);
    if (!isDirectChild(scopeDirectory(scope), areaDirectory)
        || !ensurePrivateDirectory(areaDirectory, errorCode)) {
        return {};
    }
    // The protocol keeps the full asset identity in metadata and FileManager;
    // the physical cache name is deliberately compact for Windows path limits.
    QString fileName = area == AssetArea::Validated
        ? compactPathToken(assetId) : assetId;
    if (!normalizedExtension.isEmpty()) {
        fileName += QLatin1Char('.') + normalizedExtension;
    }
    const QString result = QDir(areaDirectory).filePath(fileName);
    if (!isDirectChild(areaDirectory, result)) {
        setError(QStringLiteral("unsafe_asset_path"), errorCode);
        return {};
    }
    const QFileInfo info(result);
    if (info.isSymLink() || (info.exists() && !info.isFile())) {
        setError(QStringLiteral("unsafe_asset_entry"), errorCode);
        return {};
    }
    return result;
}

QString RemoteCacheStore::stagingAssetPath(const Scope& scope,
                                           const QString& uploadId,
                                           const QString& assetId,
                                           const QString& extension,
                                           QString* errorCode)
{
    m_lastErrorCode.clear();
    if (!isSafeOpaqueIdentifier(uploadId)) {
        setError(QStringLiteral("invalid_upload_id"), errorCode);
        return {};
    }
    if (!isValidAssetId(assetId)) {
        setError(QStringLiteral("invalid_asset_id"), errorCode);
        return {};
    }
    static const QRegularExpression extensionPattern(
        QStringLiteral("^[a-z0-9]{1,16}$"));
    const QString normalizedExtension = extension.toLower();
    if (extension != extension.trimmed()
        || (!normalizedExtension.isEmpty()
            && !extensionPattern.match(normalizedExtension).hasMatch())) {
        setError(QStringLiteral("invalid_asset_extension"), errorCode);
        return {};
    }
    if (!ensureSession(scope, errorCode)) {
        return {};
    }

    const QString stagingDirectory =
        QDir(scopeDirectory(scope)).filePath(QStringLiteral("staging"));
    // Keep transient names compact. The regular Windows file APIs used by Qt
    // still encounter MAX_PATH on systems where long paths are not enabled;
    // spelling two full protocol identities into the staging path can make an
    // otherwise writable AppData cache fail at QFile::open(). 96-bit
    // deterministic tokens retain transfer isolation within the path budget.
    const QString uploadDirectory = QDir(stagingDirectory).filePath(
        compactPathToken(uploadId));
    if (!isDirectChild(scopeDirectory(scope), stagingDirectory)
        || !isDirectChild(stagingDirectory, uploadDirectory)
        || !ensurePrivateDirectory(stagingDirectory, errorCode)
        || !ensurePrivateDirectory(uploadDirectory, errorCode)) {
        return {};
    }

    QString fileName = compactPathToken(assetId);
    if (!normalizedExtension.isEmpty()) {
        fileName += QLatin1Char('.') + normalizedExtension;
    }
    const QString result = QDir(uploadDirectory).filePath(fileName);
    if (!isDirectChild(uploadDirectory, result)) {
        setError(QStringLiteral("unsafe_asset_path"), errorCode);
        return {};
    }
    const QFileInfo info(result);
    if (info.isSymLink() || (info.exists() && !info.isFile())) {
        setError(QStringLiteral("unsafe_asset_entry"), errorCode);
        return {};
    }
    return result;
}

RemoteCacheStore::AssetRemovalResult
RemoteCacheStore::removeValidatedAsset(const Scope& scope,
                                       const AssetRemovalDescriptor& descriptor)
{
    AssetRemovalResult result;
    result.removalId = descriptor.removalId;
    m_lastErrorCode.clear();

    QString validationError;
    static const QRegularExpression extensionPattern(
        QStringLiteral("^[a-z0-9]{1,16}$"));
    const QString normalizedExtension = descriptor.extension.trimmed().toLower();
    AssetRemovalDescriptor command = descriptor;
    command.extension = normalizedExtension;
    if (!m_initialized) {
        result.errorCode = QStringLiteral("cache_store_not_initialized");
    } else if (!validateScope(scope, &validationError)) {
        result.errorCode = validationError;
    } else if (!isValidAssetId(command.uploadId)) {
        result.errorCode = QStringLiteral("invalid_upload_id");
    } else if (!isValidAssetId(command.assetId)) {
        result.errorCode = QStringLiteral("invalid_asset_id");
    } else if (!isValidTeardownId(command.removalId)) {
        result.errorCode = QStringLiteral("invalid_removal_id");
    } else if (!isValidSha256(command.fileId)
               || !isValidSha256(command.sha256)
               || command.fileId != command.sha256) {
        result.errorCode = QStringLiteral("invalid_asset_digest");
    } else if (command.size < 1
               || command.size > kMaximumAssetRemovalBytes) {
        result.errorCode = QStringLiteral("invalid_asset_size");
    } else if (command.offset != command.size) {
        result.errorCode = QStringLiteral("invalid_asset_offset");
    } else if (descriptor.extension != descriptor.extension.trimmed()
               || !extensionPattern.match(normalizedExtension).hasMatch()) {
        result.errorCode = QStringLiteral("invalid_asset_extension");
    }
    if (!result.errorCode.isEmpty()) {
        setError(result.errorCode);
        return result;
    }

    const QString replayPath = assetRemovalTombstonePath(command.removalId);
    if (QFileInfo::exists(replayPath)) {
        QJsonObject replay;
        Scope replayScope;
        QString replayError;
        bool bytesOk = false;
        if (!loadJsonObject(replayPath, &replay, &replayError)
            || replay.value(QStringLiteral("schemaVersion")).toInt(-1)
                != MetadataSchemaVersion
            || !parseScope(replay, &replayScope)
            || replay.value(QStringLiteral("removalId")).toString()
                != command.removalId
            || replay.value(QStringLiteral("uploadId")).toString()
                != command.uploadId
            || replay.value(QStringLiteral("assetId")).toString()
                != command.assetId
            || replay.value(QStringLiteral("fileId")).toString()
                != command.fileId
            || replay.value(QStringLiteral("sha256")).toString()
                != command.sha256
            || replay.value(QStringLiteral("offset")).toString()
                != QString::number(command.offset)
            || replay.value(QStringLiteral("size")).toString()
                != QString::number(command.size)
            || replay.value(QStringLiteral("extension")).toString()
                != command.extension
            || replayScope.senderEndpointId != scope.senderEndpointId
            || replayScope.remoteSessionId != scope.remoteSessionId
            || replayScope.generation > scope.generation) {
            result.outcome = CommitOutcome::Conflict;
            result.errorCode = replayError.isEmpty()
                ? QStringLiteral("asset_removal_conflict") : replayError;
            setError(result.errorCode);
            return result;
        }
        result.quarantinedBytes = replay.value(QStringLiteral("quarantinedBytes"))
                                      .toString().toLongLong(&bytesOk);
        if (!bytesOk || result.quarantinedBytes != command.size) {
            result.outcome = CommitOutcome::CleanupError;
            result.errorCode = QStringLiteral("asset_removal_tombstone_invalid");
            setError(result.errorCode);
            return result;
        }
        if (replayScope.generation < scope.generation) {
            // A signed in-process RemoteSession resume advances generation but
            // must still replay an already committed asset quarantine. Persist
            // the monotonic advance so an old-generation packet can never be
            // accepted afterwards.
            replay.insert(QStringLiteral("generation"),
                          generationString(scope.generation));
            replay.insert(QStringLiteral("generationUpdatedAt"), utcNow());
            if (!writeJsonAtomically(replayPath, replay, &replayError)) {
                result.outcome = CommitOutcome::CleanupError;
                result.errorCode = replayError;
                setError(result.errorCode);
                return result;
            }
        }
        const QString replayQuarantineEntry =
            replay.value(QStringLiteral("quarantineEntry")).toString();
        static const QRegularExpression replayEntryPattern(
            QStringLiteral("^a-[0-9a-f]{64}$"));
        if (replayEntryPattern.match(replayQuarantineEntry).hasMatch()
            && QFileInfo::exists(quarantinePath(replayQuarantineEntry))) {
            scheduleOrphanCleanup(replayQuarantineEntry);
        }
        result.outcome = CommitOutcome::AlreadyCommitted;
        return result;
    }

    const QString validatedDirectory =
        QDir(scopeDirectory(scope)).filePath(QStringLiteral("validated"));
    const QString assetFile = QDir(validatedDirectory).filePath(
        compactPathToken(command.assetId)
        + QLatin1Char('.') + command.extension);
    const QString intentFile = assetRemovalIntentPath(command.removalId);
    QString quarantineEntry;
    QString phase;
    QJsonObject intent;
    QString transactionError;

    if (QFileInfo::exists(intentFile)) {
        Scope intentScope;
        AssetRemovalDescriptor intentCommand;
        if (!loadJsonObject(intentFile, &intent, &transactionError)
            || intent.value(QStringLiteral("schemaVersion")).toInt(-1)
                != MetadataSchemaVersion
            || !parseScope(intent, &intentScope)
            || !parseAssetRemovalDescriptor(intent, &intentCommand)
            || !(intentScope == scope)
            || !assetRemovalDescriptorsEqual(intentCommand, command)) {
            result.outcome = transactionError.isEmpty()
                ? CommitOutcome::Conflict : CommitOutcome::CleanupError;
            result.errorCode = transactionError.isEmpty()
                ? QStringLiteral("asset_removal_conflict") : transactionError;
            setError(result.errorCode);
            return result;
        }
        quarantineEntry =
            intent.value(QStringLiteral("quarantineEntry")).toString();
        phase = intent.value(QStringLiteral("phase")).toString();
    } else {
        if (!acceptsCommands(scope)) {
            result.errorCode = QStringLiteral("remote_session_not_open");
            setError(result.errorCode);
            return result;
        }

        const QFileInfo assetInfo(assetFile);
        if (!isDirectChild(scopeDirectory(scope), validatedDirectory)
            || !isDirectChild(validatedDirectory, assetFile)
            || !ownsPath(scope, assetFile)
            || assetInfo.isSymLink() || !assetInfo.isFile()) {
            result.outcome = CommitOutcome::CleanupError;
            result.errorCode = assetInfo.isSymLink()
                ? QStringLiteral("symlink_refused")
                : QStringLiteral("validated_asset_missing");
            setError(result.errorCode);
            return result;
        }
        if (assetInfo.size() != command.size) {
            result.outcome = CommitOutcome::CleanupError;
            result.errorCode = QStringLiteral("validated_asset_size_mismatch");
            setError(result.errorCode);
            return result;
        }

        if (!ensurePrivateDirectory(m_assetRemovalIntentsPath, &transactionError)
            || !ensurePrivateDirectory(m_assetRemovalTombstonesPath,
                                       &transactionError)
            || !ensurePrivateDirectory(m_quarantinePath, &transactionError)) {
            result.outcome = CommitOutcome::CleanupError;
            result.errorCode = transactionError;
            setError(result.errorCode);
            return result;
        }

        quarantineEntry =
            assetQuarantineName(scope, command.assetId, command.removalId);
        const QString initialQuarantinePath = quarantinePath(quarantineEntry);
        if (!isDirectChild(m_quarantinePath, initialQuarantinePath)
            || QFileInfo::exists(initialQuarantinePath)
            || QFileInfo(initialQuarantinePath).isSymLink()) {
            result.outcome = CommitOutcome::CleanupError;
            result.errorCode = QStringLiteral(
                "asset_quarantine_destination_exists");
            setError(result.errorCode);
            return result;
        }

        intent = assetRemovalTupleJson(scope, command);
        intent.insert(QStringLiteral("schemaVersion"), MetadataSchemaVersion);
        intent.insert(QStringLiteral("quarantineEntry"), quarantineEntry);
        intent.insert(QStringLiteral("phase"), QStringLiteral("prepared"));
        intent.insert(QStringLiteral("createdAt"), utcNow());
        if (!writeJsonAtomically(intentFile, intent, &transactionError)) {
            result.outcome = CommitOutcome::CleanupError;
            result.errorCode = transactionError;
            setError(result.errorCode);
            return result;
        }
        phase = QStringLiteral("prepared");
    }

    static const QRegularExpression quarantinePattern(
        QStringLiteral("^a-[0-9a-f]{64}$"));
    if (!quarantinePattern.match(quarantineEntry).hasMatch()
        || quarantineEntry
            != assetQuarantineName(scope, command.assetId, command.removalId)
        || (phase != QLatin1String("prepared")
            && phase != QLatin1String("quarantined"))) {
        result.outcome = CommitOutcome::CleanupError;
        result.errorCode = QStringLiteral("asset_removal_intent_invalid");
        setError(result.errorCode);
        return result;
    }

    const QString quarantinedPath = quarantinePath(quarantineEntry);
    if (!isDirectChild(m_quarantinePath, quarantinedPath)) {
        result.outcome = CommitOutcome::CleanupError;
        result.errorCode = QStringLiteral("unsafe_asset_quarantine_path");
        setError(result.errorCode);
        return result;
    }

    QFileInfo liveInfo(assetFile);
    QFileInfo quarantineInfo(quarantinedPath);
    bool liveExists = liveInfo.exists() || liveInfo.isSymLink();
    bool quarantineExists = quarantineInfo.exists()
        || quarantineInfo.isSymLink();
    if (liveExists && quarantineExists) {
        result.outcome = CommitOutcome::CleanupError;
        result.errorCode = QStringLiteral("ambiguous_asset_removal_state");
        setError(result.errorCode);
        return result;
    }
    if (phase == QLatin1String("quarantined") && liveExists) {
        result.outcome = CommitOutcome::CleanupError;
        result.errorCode = QStringLiteral("asset_removal_phase_conflict");
        setError(result.errorCode);
        return result;
    }

    if (liveExists) {
        if (!isDirectChild(scopeDirectory(scope), validatedDirectory)
            || !isDirectChild(validatedDirectory, assetFile)
            || !ownsPath(scope, assetFile)
            || liveInfo.isSymLink() || !liveInfo.isFile()) {
            result.outcome = CommitOutcome::CleanupError;
            result.errorCode = liveInfo.isSymLink()
                ? QStringLiteral("symlink_refused")
                : QStringLiteral("unsafe_validated_asset");
            setError(result.errorCode);
            return result;
        }
        if (liveInfo.size() != command.size) {
            result.outcome = CommitOutcome::CleanupError;
            result.errorCode = QStringLiteral("validated_asset_size_mismatch");
            setError(result.errorCode);
            return result;
        }
        const AtomicRenameStatus renameStatus =
            atomicRenameNoReplace(assetFile, quarantinedPath);
        if (renameStatus != AtomicRenameStatus::Renamed) {
            result.outcome = CommitOutcome::CleanupError;
            result.errorCode = atomicRenameErrorCode(renameStatus, true);
            setError(result.errorCode);
            return result;
        }
        liveExists = false;
        quarantineExists = true;
        quarantineInfo.setFile(quarantinedPath);
        quarantineInfo.refresh();
    }

    if (quarantineExists
        && (quarantineInfo.isSymLink() || !quarantineInfo.isFile()
            || quarantineInfo.size() != command.size)) {
        result.outcome = CommitOutcome::CleanupError;
        result.errorCode = quarantineInfo.isSymLink()
            ? QStringLiteral("symlink_refused")
            : QStringLiteral("asset_quarantine_invalid");
        setError(result.errorCode);
        return result;
    }

    const QFileDevice::Permissions ownerFilePermissions =
        QFileDevice::ReadOwner | QFileDevice::WriteOwner;
    if (quarantineExists
        && (!QFile::setPermissions(quarantinedPath, ownerFilePermissions)
            || !syncDirectory(validatedDirectory)
            || !syncDirectory(m_quarantinePath))) {
        result.outcome = CommitOutcome::CleanupError;
        result.errorCode = QStringLiteral("asset_quarantine_commit_failed");
        setError(result.errorCode);
        return result;
    }

    if (phase == QLatin1String("quarantined")) {
        bool intentBytesOk = false;
        const qint64 intentBytes =
            intent.value(QStringLiteral("quarantinedBytes"))
                .toString().toLongLong(&intentBytesOk);
        if (!intentBytesOk || intentBytes != command.size) {
            result.outcome = CommitOutcome::CleanupError;
            result.errorCode = QStringLiteral("asset_removal_intent_invalid");
            setError(result.errorCode);
            return result;
        }
    } else {
        intent.insert(QStringLiteral("phase"), QStringLiteral("quarantined"));
        intent.insert(QStringLiteral("quarantinedBytes"),
                      QString::number(command.size));
        intent.insert(QStringLiteral("quarantinedAt"), utcNow());
        if (!writeJsonAtomically(intentFile, intent, &transactionError)) {
            result.outcome = CommitOutcome::CleanupError;
            result.errorCode = transactionError;
            setError(result.errorCode);
            return result;
        }
    }

    result.quarantinedBytes = command.size;
    QJsonObject tombstoneObject = assetRemovalTupleJson(scope, command);
    tombstoneObject.insert(QStringLiteral("schemaVersion"), MetadataSchemaVersion);
    tombstoneObject.insert(QStringLiteral("quarantineEntry"), quarantineEntry);
    tombstoneObject.insert(QStringLiteral("quarantinedBytes"),
                           QString::number(result.quarantinedBytes));
    tombstoneObject.insert(QStringLiteral("committedAt"), utcNow());
    if (!writeJsonAtomically(replayPath, tombstoneObject, &transactionError)) {
        result.outcome = CommitOutcome::CleanupError;
        result.errorCode = transactionError;
        setError(result.errorCode);
        return result;
    }

    if (!QFile::remove(intentFile) && QFileInfo::exists(intentFile)) {
        // The committed tombstone is authoritative. A later replay/startup
        // retries only removal of this redundant durable intent.
        setError(QStringLiteral("asset_removal_intent_remove_deferred"));
    } else {
        syncDirectory(m_assetRemovalIntentsPath);
    }

    // The server also keeps a bounded replay tombstone. Bound the local side
    // independently so repeated sessions cannot grow metadata without limit.
    constexpr qsizetype kMaximumAssetRemovalTombstones = 4096;
    const QFileInfoList removalTombstones =
        QDir(m_assetRemovalTombstonesPath).entryInfoList(
            {QStringLiteral("*.json")}, QDir::Files | QDir::NoSymLinks,
            QDir::Time);
    for (qsizetype index = kMaximumAssetRemovalTombstones;
         index < removalTombstones.size(); ++index) {
        QFile::remove(removalTombstones.at(index).absoluteFilePath());
    }

    result.outcome = CommitOutcome::Committed;
    if (quarantineExists) {
        scheduleOrphanCleanup(quarantineEntry);
    }
    return result;
}

bool RemoteCacheStore::beginTeardown(const Scope& scope,
                                     const QString& teardownId,
                                     QString* errorCode)
{
    return beginTeardownInternal(scope, teardownId, false, QString(), errorCode);
}

bool RemoteCacheStore::beginProvisionalTeardown(const Scope& scope,
                                                const QString& localTeardownId,
                                                const QString& reasonCode,
                                                QString* errorCode)
{
    static const QRegularExpression reasonPattern(
        QStringLiteral("^[a-z0-9_]{1,64}$"));
    if (!reasonPattern.match(reasonCode).hasMatch()) {
        setError(QStringLiteral("invalid_teardown_reason"), errorCode);
        return false;
    }
    return beginTeardownInternal(scope, localTeardownId, true, reasonCode,
                                 errorCode);
}

bool RemoteCacheStore::beginTeardownInternal(const Scope& scope,
                                             const QString& teardownId,
                                             bool provisional,
                                             const QString& reasonCode,
                                             QString* errorCode)
{
    m_lastErrorCode.clear();
    if (!m_initialized) {
        setError(QStringLiteral("cache_store_not_initialized"), errorCode);
        return false;
    }
    if (!validateScope(scope, errorCode) || !isValidTeardownId(teardownId)) {
        if (m_lastErrorCode.isEmpty()) {
            setError(QStringLiteral("invalid_teardown_id"), errorCode);
        }
        return false;
    }

    const QString terminalFile = tombstonePath(scope);
    if (const std::optional<Tombstone> existing = tombstone(scope)) {
        if (existing->scope == scope && existing->provisional && !provisional) {
            return adoptProvisionalTombstone(scope, teardownId, errorCode);
        }
        if (existing->scope == scope && existing->teardownId == teardownId) {
            return true;
        }
        setError(QStringLiteral("terminal_teardown_conflict"), errorCode);
        return false;
    }
    if (QFileInfo::exists(terminalFile)) {
        setError(QStringLiteral("tombstone_invalid"), errorCode);
        return false;
    }

    const QString liveDescriptor = QDir(scopeDirectory(scope))
                                       .filePath(QStringLiteral(".scope.json"));
    const QFileInfo liveScopeInfo(scopeDirectory(scope));
    if (liveScopeInfo.exists() || liveScopeInfo.isSymLink()) {
        if (liveScopeInfo.isSymLink() || !liveScopeInfo.isDir()) {
            setError(liveScopeInfo.isSymLink()
                         ? QStringLiteral("symlink_refused")
                         : QStringLiteral("unsafe_live_cache"),
                     errorCode);
            return false;
        }
        QJsonObject descriptor;
        Scope storedScope;
        if (!loadJsonObject(liveDescriptor, &descriptor, errorCode)
            || descriptor.value(QStringLiteral("schemaVersion")).toInt(-1)
                != MetadataSchemaVersion
            || !parseScope(descriptor, &storedScope)
            || storedScope.senderEndpointId != scope.senderEndpointId
            || storedScope.remoteSessionId != scope.remoteSessionId
            || storedScope.generation > scope.generation) {
            setError(QStringLiteral("remote_session_generation_conflict"), errorCode);
            return false;
        }
        if (storedScope.generation < scope.generation) {
            descriptor.insert(QStringLiteral("generation"),
                              generationString(scope.generation));
            descriptor.insert(QStringLiteral("generationUpdatedAt"), utcNow());
            if (!writeJsonAtomically(liveDescriptor, descriptor, errorCode)) {
                return false;
            }
        }
    }

    const QString filePath = intentPath(scope);
    if (QFileInfo::exists(filePath)) {
        QJsonObject intent;
        Scope storedScope;
        if (!loadJsonObject(filePath, &intent, errorCode)
            || intent.value(QStringLiteral("schemaVersion")).toInt(-1) != MetadataSchemaVersion
            || !parseScope(intent, &storedScope)
            || !(storedScope == scope)) {
            setError(QStringLiteral("teardown_intent_invalid"), errorCode);
            return false;
        }
        const QString storedTeardownId =
            intent.value(QStringLiteral("teardownId")).toString();
        const bool storedProvisional =
            intent.value(QStringLiteral("provisional")).toBool(false);
        if (!isValidTeardownId(storedTeardownId)) {
            setError(QStringLiteral("teardown_intent_invalid"), errorCode);
            return false;
        }
        if (storedProvisional && !provisional) {
            // The live directory may already have been renamed when a process
            // stopped between the atomic rename and tombstone write. Keep the
            // original quarantine entry, but make the authenticated server id
            // the durable replay identity.
            intent.insert(QStringLiteral("provisionalOriginTeardownId"),
                          storedTeardownId);
            intent.insert(QStringLiteral("teardownId"), teardownId);
            intent.insert(QStringLiteral("provisional"), false);
            intent.insert(QStringLiteral("officialTeardownAdoptedAt"), utcNow());
            return writeJsonAtomically(filePath, intent, errorCode);
        }
        if (storedTeardownId != teardownId) {
            setError(QStringLiteral("terminal_teardown_conflict"), errorCode);
            return false;
        }
        // Never downgrade an official intent back to a provisional one.
        if (!storedProvisional && provisional) {
            return true;
        }
        return true;
    }

    QJsonObject intent = scopeJson(scope);
    intent.insert(QStringLiteral("schemaVersion"), MetadataSchemaVersion);
    intent.insert(QStringLiteral("teardownId"), teardownId);
    intent.insert(QStringLiteral("quarantineEntry"), quarantineName(scope, teardownId));
    intent.insert(QStringLiteral("state"), QStringLiteral("terminating"));
    intent.insert(QStringLiteral("provisional"), provisional);
    if (provisional) {
        intent.insert(QStringLiteral("reasonCode"), reasonCode);
    }
    intent.insert(QStringLiteral("createdAt"), utcNow());
    return writeJsonAtomically(filePath, intent, errorCode);
}

bool RemoteCacheStore::adoptProvisionalTombstone(
    const Scope& scope,
    const QString& officialTeardownId,
    QString* errorCode)
{
    if (!isValidTeardownId(officialTeardownId)) {
        setError(QStringLiteral("invalid_teardown_id"), errorCode);
        return false;
    }

    QJsonObject object;
    Scope storedScope;
    const QString path = tombstonePath(scope);
    if (!loadJsonObject(path, &object, errorCode)
        || object.value(QStringLiteral("schemaVersion")).toInt(-1)
            != MetadataSchemaVersion
        || !parseScope(object, &storedScope)
        || !(storedScope == scope)
        || !object.value(QStringLiteral("provisional")).toBool(false)
        || !isValidTeardownId(
            object.value(QStringLiteral("teardownId")).toString())) {
        setError(QStringLiteral("terminal_teardown_conflict"), errorCode);
        return false;
    }

    object.insert(QStringLiteral("provisionalOriginTeardownId"),
                  object.value(QStringLiteral("teardownId")).toString());
    object.insert(QStringLiteral("teardownId"), officialTeardownId);
    object.insert(QStringLiteral("provisional"), false);
    object.insert(QStringLiteral("officialTeardownAdoptedAt"), utcNow());
    return writeJsonAtomically(path, object, errorCode);
}

bool RemoteCacheStore::persistCleanupError(const Scope& scope,
                                           const QString& teardownId,
                                           const QString& errorCode,
                                           const QString& quarantineEntry) const
{
    QJsonObject record = scopeJson(scope);
    record.insert(QStringLiteral("schemaVersion"), MetadataSchemaVersion);
    record.insert(QStringLiteral("teardownId"), teardownId);
    record.insert(QStringLiteral("state"), QStringLiteral("cleanup_error"));
    record.insert(QStringLiteral("errorCode"), errorCode);
    record.insert(QStringLiteral("updatedAt"), utcNow());
    if (!quarantineEntry.isEmpty()) {
        record.insert(QStringLiteral("quarantineEntry"), quarantineEntry);
    }

    const QString existingIntentPath = intentPath(scope);
    QString ignored;
    if (QFileInfo::exists(existingIntentPath)) {
        return writeJsonAtomically(existingIntentPath, record, &ignored);
    }
    const QString name = quarantineEntry.isEmpty()
        ? scopeKey(scope) + QStringLiteral(".json")
        : quarantineEntry + QStringLiteral(".json");
    return writeJsonAtomically(QDir(m_cleanupErrorsPath).filePath(name), record, &ignored);
}

RemoteCacheStore::CommitResult RemoteCacheStore::commitTeardown(
    const Scope& scope,
    const QString& teardownId)
{
    CommitResult result;
    result.teardownId = teardownId;
    QString errorCode;
    if (!m_initialized) {
        result.errorCode = QStringLiteral("cache_store_not_initialized");
        setError(result.errorCode);
        return result;
    }
    if (!validateScope(scope, &errorCode) || !isValidTeardownId(teardownId)) {
        result.errorCode = errorCode.isEmpty()
            ? QStringLiteral("invalid_teardown_id")
            : errorCode;
        setError(result.errorCode);
        return result;
    }

    const QString terminalFile = tombstonePath(scope);
    if (const std::optional<Tombstone> existing = tombstone(scope)) {
        if (existing->scope == scope && existing->provisional
            && existing->teardownId != teardownId) {
            if (!adoptProvisionalTombstone(scope, teardownId, &errorCode)) {
                result.outcome = CommitOutcome::Conflict;
                result.errorCode = errorCode;
                return result;
            }
            const std::optional<Tombstone> adopted = tombstone(scope);
            if (!adopted || !(adopted->scope == scope)
                || adopted->teardownId != teardownId || adopted->provisional) {
                result.outcome = CommitOutcome::CleanupError;
                result.errorCode = QStringLiteral("tombstone_invalid");
                return result;
            }
            result.teardownId = adopted->teardownId;
            result.quarantinedBytes = adopted->quarantinedBytes;
            result.cleanupPending =
                adopted->state == SessionState::CleanupPending
                || adopted->state == SessionState::CleanupError;
            result.outcome = CommitOutcome::AlreadyCommitted;
            result.errorCode = adopted->errorCode;
            return result;
        }
        result.teardownId = existing->teardownId;
        result.quarantinedBytes = existing->quarantinedBytes;
        result.cleanupPending = existing->state == SessionState::CleanupPending
            || existing->state == SessionState::CleanupError;
        if (existing->scope == scope && existing->teardownId == teardownId) {
            result.outcome = CommitOutcome::AlreadyCommitted;
            result.errorCode = existing->errorCode;
        } else {
            result.outcome = CommitOutcome::Conflict;
            result.errorCode = QStringLiteral("terminal_teardown_conflict");
        }
        return result;
    }
    if (QFileInfo::exists(terminalFile)) {
        result.outcome = CommitOutcome::CleanupError;
        result.errorCode = QStringLiteral("tombstone_invalid");
        setError(result.errorCode);
        return result;
    }

    bool existingIntentIsProvisional = false;
    QString existingIntentReason;
    if (QFileInfo::exists(intentPath(scope))) {
        QJsonObject existingIntent;
        Scope intentScope;
        QString ignored;
        if (loadJsonObject(intentPath(scope), &existingIntent, &ignored)
            && parseScope(existingIntent, &intentScope)
            && intentScope == scope
            && existingIntent.value(QStringLiteral("teardownId")).toString()
                == teardownId) {
            existingIntentIsProvisional =
                existingIntent.value(QStringLiteral("provisional")).toBool(false);
            existingIntentReason =
                existingIntent.value(QStringLiteral("reasonCode")).toString();
        }
    }
    if (!beginTeardownInternal(scope, teardownId,
                               existingIntentIsProvisional,
                               existingIntentReason, &errorCode)) {
        result.outcome = errorCode == QLatin1String("terminal_teardown_conflict")
            ? CommitOutcome::Conflict
            : CommitOutcome::InvalidRequest;
        result.errorCode = errorCode;
        return result;
    }

    QJsonObject intent;
    if (!loadJsonObject(intentPath(scope), &intent, &errorCode)) {
        result.outcome = CommitOutcome::CleanupError;
        result.errorCode = errorCode;
        persistCleanupError(scope, teardownId, errorCode);
        return result;
    }
    const QString quarantineEntry = intent.value(QStringLiteral("quarantineEntry")).toString();
    const bool provisional = intent.value(QStringLiteral("provisional")).toBool(false);
    const QString reasonCode = intent.value(QStringLiteral("reasonCode")).toString();
    const QString provisionalOriginTeardownId =
        intent.value(QStringLiteral("provisionalOriginTeardownId")).toString();
    static const QRegularExpression quarantinePattern(QStringLiteral("^q-[0-9a-f]{64}$"));
    const bool matchesCurrentTeardown =
        quarantineEntry == quarantineName(scope, teardownId);
    const bool matchesProvisionalOrigin =
        !provisionalOriginTeardownId.isEmpty()
        && isValidTeardownId(provisionalOriginTeardownId)
        && quarantineEntry
            == quarantineName(scope, provisionalOriginTeardownId);
    if (!quarantinePattern.match(quarantineEntry).hasMatch()
        || (!matchesCurrentTeardown && !matchesProvisionalOrigin)) {
        result.outcome = CommitOutcome::CleanupError;
        result.errorCode = QStringLiteral("teardown_intent_invalid");
        persistCleanupError(scope, teardownId, result.errorCode);
        return result;
    }

    const QString livePath = scopeDirectory(scope);
    const QString quarantinedPath = quarantinePath(quarantineEntry);
    const QFileInfo liveInfo(livePath);
    const QFileInfo quarantineInfo(quarantinedPath);
    const bool liveExists = liveInfo.exists() || liveInfo.isSymLink();
    const bool quarantineExists = quarantineInfo.exists() || quarantineInfo.isSymLink();
    if (liveExists && quarantineExists) {
        result.outcome = CommitOutcome::CleanupError;
        result.errorCode = QStringLiteral("ambiguous_cache_state");
        persistCleanupError(scope, teardownId, result.errorCode, quarantineEntry);
        return result;
    }

    qint64 quarantinedBytes = 0;
    if (liveExists) {
        if (!isDirectChild(QFileInfo(livePath).absolutePath(), livePath)
            || !inspectTree(livePath, &quarantinedBytes, &errorCode)) {
            result.outcome = CommitOutcome::CleanupError;
            result.errorCode = errorCode.isEmpty()
                ? QStringLiteral("unsafe_live_cache")
                : errorCode;
            persistCleanupError(scope, teardownId, result.errorCode, quarantineEntry);
            return result;
        }
        if (!ensurePrivateDirectory(m_quarantinePath, &errorCode)) {
            result.outcome = CommitOutcome::CleanupError;
            result.errorCode = errorCode.isEmpty()
                ? QStringLiteral("quarantine_rename_failed")
                : errorCode;
            persistCleanupError(scope, teardownId, result.errorCode, quarantineEntry);
            return result;
        }
        const AtomicRenameStatus renameStatus =
            atomicRenameNoReplace(livePath, quarantinedPath);
        if (renameStatus != AtomicRenameStatus::Renamed) {
            result.outcome = CommitOutcome::CleanupError;
            result.errorCode = atomicRenameErrorCode(renameStatus, false);
            persistCleanupError(scope, teardownId, result.errorCode,
                                quarantineEntry);
            return result;
        }
        if (!QFile::setPermissions(quarantinedPath, kOwnerDirectoryPermissions)
            || !syncDirectory(QFileInfo(livePath).absolutePath())
            || !syncDirectory(m_quarantinePath)) {
            result.outcome = CommitOutcome::CleanupError;
            result.errorCode = QStringLiteral("quarantine_commit_failed");
            persistCleanupError(scope, teardownId, result.errorCode, quarantineEntry);
            return result;
        }
    } else if (quarantineExists) {
        if (!isDirectChild(m_quarantinePath, quarantinedPath)
            || !inspectTree(quarantinedPath, &quarantinedBytes, &errorCode)) {
            result.outcome = CommitOutcome::CleanupError;
            result.errorCode = errorCode.isEmpty()
                ? QStringLiteral("unsafe_quarantine_entry")
                : errorCode;
            persistCleanupError(scope, teardownId, result.errorCode, quarantineEntry);
            return result;
        }
        if (!QFile::setPermissions(quarantinedPath, kOwnerDirectoryPermissions)) {
            result.outcome = CommitOutcome::CleanupError;
            result.errorCode = QStringLiteral("cache_permissions_failed");
            persistCleanupError(scope, teardownId, result.errorCode, quarantineEntry);
            return result;
        }
    }

    Tombstone committed;
    committed.scope = scope;
    committed.teardownId = teardownId;
    committed.reasonCode = reasonCode;
    committed.quarantinedBytes = quarantinedBytes;
    committed.provisional = provisional;
    committed.state = (liveExists || quarantineExists)
        ? SessionState::CleanupPending
        : SessionState::Closed;

    QJsonObject tombstoneObject = scopeJson(scope);
    tombstoneObject.insert(QStringLiteral("schemaVersion"), MetadataSchemaVersion);
    tombstoneObject.insert(QStringLiteral("teardownId"), teardownId);
    tombstoneObject.insert(QStringLiteral("quarantineEntry"),
                           (liveExists || quarantineExists) ? quarantineEntry : QString());
    tombstoneObject.insert(QStringLiteral("cleanupState"),
                           cleanupStateToString(committed.state));
    tombstoneObject.insert(QStringLiteral("provisional"), provisional);
    if (!reasonCode.isEmpty()) {
        tombstoneObject.insert(QStringLiteral("reasonCode"), reasonCode);
    }
    if (!provisionalOriginTeardownId.isEmpty()) {
        tombstoneObject.insert(QStringLiteral("provisionalOriginTeardownId"),
                               provisionalOriginTeardownId);
    }
    tombstoneObject.insert(QStringLiteral("quarantinedBytes"),
                           QString::number(quarantinedBytes));
    tombstoneObject.insert(QStringLiteral("committedAt"), utcNow());
    if (!writeJsonAtomically(tombstonePath(scope), tombstoneObject, &errorCode)) {
        result.outcome = CommitOutcome::CleanupError;
        result.errorCode = errorCode;
        persistCleanupError(scope, teardownId, errorCode, quarantineEntry);
        return result;
    }

    if (!QFile::remove(intentPath(scope)) && QFileInfo::exists(intentPath(scope))) {
        // The tombstone is already the durable authority.  Keeping the intent
        // is harmless; initialize() removes it after replay.
        setError(QStringLiteral("intent_remove_deferred"));
    } else {
        syncDirectory(m_intentsPath);
    }

    result.outcome = CommitOutcome::Committed;
    result.quarantinedBytes = quarantinedBytes;
    result.cleanupPending = committed.state == SessionState::CleanupPending;
    emit logicalCommitCompleted(scope.senderEndpointId,
                                scope.remoteSessionId,
                                scope.generation,
                                teardownId,
                                quarantinedBytes);
    if (result.cleanupPending) {
        schedulePhysicalCleanup(committed, quarantineEntry);
    }
    return result;
}

std::optional<RemoteCacheStore::Tombstone> RemoteCacheStore::tombstone(
    const Scope& scope) const
{
    if (!isValidEndpointId(scope.senderEndpointId)
        || !isValidSessionId(scope.remoteSessionId)) {
        return std::nullopt;
    }
    const QString path = tombstonePath(scope);
    if (!QFileInfo::exists(path)) {
        return std::nullopt;
    }
    QJsonObject object;
    QString ignored;
    Scope storedScope;
    if (!loadJsonObject(path, &object, &ignored)
        || object.value(QStringLiteral("schemaVersion")).toInt(-1) != MetadataSchemaVersion
        || !parseScope(object, &storedScope)
        || !isValidTeardownId(object.value(QStringLiteral("teardownId")).toString())) {
        return std::nullopt;
    }

    Tombstone result;
    result.scope = storedScope;
    result.teardownId = object.value(QStringLiteral("teardownId")).toString();
    result.reasonCode = object.value(QStringLiteral("reasonCode")).toString();
    result.state = cleanupStateFromString(
        object.value(QStringLiteral("cleanupState")).toString());
    result.errorCode = object.value(QStringLiteral("errorCode")).toString();
    result.provisional = object.value(QStringLiteral("provisional")).toBool(false);
    bool bytesOk = false;
    result.quarantinedBytes = object.value(QStringLiteral("quarantinedBytes"))
                                  .toString()
                                  .toLongLong(&bytesOk);
    if (!bytesOk || result.quarantinedBytes < 0) {
        return std::nullopt;
    }
    return result;
}

RemoteCacheStore::SessionState RemoteCacheStore::state(const Scope& scope) const
{
    if (const std::optional<Tombstone> closed = tombstone(scope)) {
        return closed->state;
    }
    if (QFileInfo::exists(tombstonePath(scope))) {
        return SessionState::CleanupError;
    }
    const QString intentFile = intentPath(scope);
    if (QFileInfo::exists(intentFile)) {
        QJsonObject intent;
        QString ignored;
        if (loadJsonObject(intentFile, &intent, &ignored)
            && intent.value(QStringLiteral("state")).toString()
                == QLatin1String("cleanup_error")) {
            return SessionState::CleanupError;
        }
        return SessionState::Terminating;
    }
    const QString livePath = scopeDirectory(scope);
    const QFileInfo info(livePath);
    if (info.isDir() && !info.isSymLink()) {
        QJsonObject descriptor;
        Scope storedScope;
        QString ignored;
        const QString descriptorPath = QDir(livePath).filePath(
            QStringLiteral(".scope.json"));
        if (!loadJsonObject(descriptorPath, &descriptor, &ignored)
            || descriptor.value(QStringLiteral("schemaVersion")).toInt(-1)
                != MetadataSchemaVersion
            || !parseScope(descriptor, &storedScope)) {
            return SessionState::CleanupError;
        }
        return storedScope == scope ? SessionState::Open : SessionState::Missing;
    }
    return SessionState::Missing;
}

bool RemoteCacheStore::acceptsCommands(const Scope& scope) const
{
    return state(scope) == SessionState::Open;
}

bool RemoteCacheStore::ownsPath(const Scope& scope, const QString& candidatePath) const
{
    if (!isValidEndpointId(scope.senderEndpointId)
        || !isValidSessionId(scope.remoteSessionId)
        || scope.generation == 0 || candidatePath.isEmpty()) {
        return false;
    }
    const QString liveScope = normalizedPath(scopeDirectory(scope));
    const QString candidate = normalizedPath(candidatePath);
    QString prefix = liveScope;
    if (!prefix.endsWith(QDir::separator())) {
        prefix += QDir::separator();
    }
    if (!candidate.startsWith(prefix) || QFileInfo(liveScope).isSymLink()) {
        return false;
    }
    QJsonObject descriptor;
    Scope storedScope;
    QString ignored;
    if (!loadJsonObject(QDir(liveScope).filePath(QStringLiteral(".scope.json")),
                        &descriptor, &ignored)
        || descriptor.value(QStringLiteral("schemaVersion")).toInt(-1)
            != MetadataSchemaVersion
        || !parseScope(descriptor, &storedScope)
        || !(storedScope == scope)) {
        return false;
    }
    // Reject a symlink at any existing component below the live scope.
    QString relative = QDir(liveScope).relativeFilePath(candidate);
    QString current = liveScope;
    for (const QString& component : relative.split(QDir::separator(), Qt::SkipEmptyParts)) {
        current = QDir(current).filePath(component);
        const QFileInfo info(current);
        if (info.isSymLink()) {
            return false;
        }
    }
    return true;
}

QList<RemoteCacheStore::Scope> RemoteCacheStore::liveScopes(
    QString* errorCode) const
{
    QList<Scope> result;
    m_lastErrorCode.clear();
    if (errorCode) {
        errorCode->clear();
    }
    if (!m_initialized) {
        setError(QStringLiteral("cache_store_not_initialized"), errorCode);
        return result;
    }
    const auto recordFirstError = [this, errorCode](const QString& code) {
        if (m_lastErrorCode.isEmpty()) {
            setError(code, errorCode);
        }
    };

    const QFileInfoList senderEntries = QDir(m_rootPath).entryInfoList(
        QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System,
        QDir::Name);
    for (const QFileInfo& senderInfo : senderEntries) {
        const QString senderName = senderInfo.fileName();
        if (senderName == QLatin1String(kStateDirectory)
            || senderName == QLatin1String(kQuarantineDirectory)) {
            continue;
        }
        if (senderInfo.isSymLink()) {
            recordFirstError(QStringLiteral("symlink_refused"));
            continue;
        }
        if (!senderInfo.isDir() || !isValidEndpointId(senderName)) {
            continue;
        }

        const QFileInfoList sessionEntries =
            QDir(senderInfo.absoluteFilePath()).entryInfoList(
                QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden
                    | QDir::System,
                QDir::Name);
        for (const QFileInfo& sessionInfo : sessionEntries) {
            if (sessionInfo.isSymLink()) {
                recordFirstError(QStringLiteral("symlink_refused"));
                continue;
            }
            if (!sessionInfo.isDir()
                || !isValidSessionId(sessionInfo.fileName())) {
                continue;
            }

            const QString descriptorPath =
                QDir(sessionInfo.absoluteFilePath())
                    .filePath(QStringLiteral(".scope.json"));
            if (!QFileInfo::exists(descriptorPath)) {
                // A pre-v2 directory is not an authenticated live scope.
                continue;
            }
            QJsonObject descriptor;
            Scope scope;
            QString ignored;
            if (!loadJsonObject(descriptorPath, &descriptor, &ignored)
                || descriptor.value(QStringLiteral("schemaVersion")).toInt(-1)
                    != MetadataSchemaVersion
                || !parseScope(descriptor, &scope)
                || scope.senderEndpointId != senderName
                || scope.remoteSessionId != sessionInfo.fileName()) {
                recordFirstError(QStringLiteral("scope_metadata_invalid"));
                continue;
            }
            if (!QFileInfo::exists(intentPath(scope))
                && !QFileInfo::exists(tombstonePath(scope))) {
                result.append(scope);
            }
        }
    }
    return result;
}

QList<RemoteCacheStore::Tombstone> RemoteCacheStore::allTombstones() const
{
    QList<Tombstone> result;
    const QFileInfoList files = QDir(m_tombstonesPath).entryInfoList(
        {QStringLiteral("*.json")}, QDir::Files | QDir::NoSymLinks, QDir::Name);
    for (const QFileInfo& fileInfo : files) {
        QJsonObject object;
        Scope scope;
        QString ignored;
        if (!loadJsonObject(fileInfo.absoluteFilePath(), &object, &ignored)
            || !parseScope(object, &scope)) {
            continue;
        }
        if (const std::optional<Tombstone> parsed = tombstone(scope)) {
            result.append(*parsed);
        }
    }
    return result;
}

int RemoteCacheStore::cleanupPendingCount() const
{
    int count = 0;
    for (const Tombstone& entry : allTombstones()) {
        if (entry.state == SessionState::CleanupPending
            || entry.state == SessionState::CleanupError) {
            ++count;
        }
    }
    return count;
}

qint64 RemoteCacheStore::quarantinedBytesAwaitingDeletion() const
{
    qint64 bytes = 0;
    for (const Tombstone& entry : allTombstones()) {
        if (entry.state == SessionState::CleanupPending
            || entry.state == SessionState::CleanupError) {
            bytes += entry.quarantinedBytes;
        }
    }
    return bytes;
}

bool RemoteCacheStore::hasCleanupErrors() const
{
    for (const Tombstone& entry : allTombstones()) {
        if (entry.state == SessionState::CleanupError) {
            return true;
        }
    }
    const QFileInfoList tombstoneFiles = QDir(m_tombstonesPath).entryInfoList(
        {QStringLiteral("*.json")}, QDir::AllEntries | QDir::NoDotAndDotDot,
        QDir::Name);
    for (const QFileInfo& fileInfo : tombstoneFiles) {
        QJsonObject object;
        Scope scope;
        QString ignored;
        if (fileInfo.isSymLink() || !fileInfo.isFile()
            || !loadJsonObject(fileInfo.absoluteFilePath(), &object, &ignored)
            || !parseScope(object, &scope)
            || !tombstone(scope).has_value()) {
            return true;
        }
    }
    const QFileInfoList intentFiles = QDir(m_intentsPath).entryInfoList(
        {QStringLiteral("*.json")}, QDir::Files | QDir::NoSymLinks, QDir::Name);
    for (const QFileInfo& fileInfo : intentFiles) {
        QJsonObject object;
        QString ignored;
        if (!loadJsonObject(fileInfo.absoluteFilePath(), &object, &ignored)
            || object.value(QStringLiteral("state")).toString()
                == QLatin1String("cleanup_error")) {
            return true;
        }
    }
    if (!QDir(m_assetRemovalIntentsPath).entryList(
             {QStringLiteral("*.json")},
             QDir::Files | QDir::NoSymLinks).isEmpty()) {
        return true;
    }
    return !QDir(m_cleanupErrorsPath).entryList(
                {QStringLiteral("*.json")}, QDir::Files | QDir::NoSymLinks)
                .isEmpty();
}

bool RemoteCacheStore::receiverAdvertisementSafe(QString* errorCode) const
{
    m_lastErrorCode.clear();
    if (errorCode) errorCode->clear();
    if (!m_initialized) {
        setError(QStringLiteral("cache_store_not_initialized"), errorCode);
        return false;
    }

    // A tombstone is the durable logical authority. Its quarantine may still
    // exist (or physical deletion may have failed) without making the cache
    // remotely accessible again. Invalid tombstones and any simultaneously
    // live namespace remain fail-closed.
    const QFileInfoList tombstoneFiles = QDir(m_tombstonesPath).entryInfoList(
        {QStringLiteral("*.json")}, QDir::AllEntries | QDir::NoDotAndDotDot,
        QDir::Name);
    for (const QFileInfo& fileInfo : tombstoneFiles) {
        QJsonObject object;
        Scope storedScope;
        QString ignored;
        if (fileInfo.isSymLink() || !fileInfo.isFile()
            || !loadJsonObject(fileInfo.absoluteFilePath(), &object, &ignored)
            || object.value(QStringLiteral("schemaVersion")).toInt(-1)
                != MetadataSchemaVersion
            || !parseScope(object, &storedScope)
            || fileInfo.completeBaseName() != scopeKey(storedScope)
            || !isValidTeardownId(
                object.value(QStringLiteral("teardownId")).toString())
            || (object.value(QStringLiteral("cleanupState")).toString()
                    != QLatin1String("pending")
                && object.value(QStringLiteral("cleanupState")).toString()
                    != QLatin1String("deleted")
                && object.value(QStringLiteral("cleanupState")).toString()
                    != QLatin1String("error"))
            || !tombstone(storedScope).has_value()) {
            setError(QStringLiteral("tombstone_invalid"), errorCode);
            return false;
        }
        const QFileInfo liveScope(scopeDirectory(storedScope));
        if (liveScope.exists() || liveScope.isSymLink()) {
            setError(QStringLiteral("logical_cleanup_not_committed"), errorCode);
            return false;
        }
    }

    // An intent without a matching valid tombstone means the atomic rename /
    // logical commit did not finish. A redundant intent left after its
    // tombstone was committed is safe and will be retried on initialization.
    const QFileInfoList intentFiles = QDir(m_intentsPath).entryInfoList(
        {QStringLiteral("*.json")}, QDir::AllEntries | QDir::NoDotAndDotDot,
        QDir::Name);
    for (const QFileInfo& fileInfo : intentFiles) {
        QJsonObject object;
        Scope storedScope;
        QString ignored;
        if (fileInfo.isSymLink() || !fileInfo.isFile()
            || !loadJsonObject(fileInfo.absoluteFilePath(), &object, &ignored)
            || object.value(QStringLiteral("schemaVersion")).toInt(-1)
                != MetadataSchemaVersion
            || !parseScope(object, &storedScope)
            || fileInfo.completeBaseName() != scopeKey(storedScope)
            || !isValidTeardownId(
                object.value(QStringLiteral("teardownId")).toString())) {
            setError(QStringLiteral("teardown_intent_invalid"), errorCode);
            return false;
        }
        const QFileInfo liveScope(scopeDirectory(storedScope));
        if (liveScope.exists() || liveScope.isSymLink()
            || !tombstone(storedScope).has_value()) {
            setError(QStringLiteral("logical_cleanup_not_committed"), errorCode);
            return false;
        }
    }

    // Targeted asset removals use their own intent/tombstone transaction.
    // A session-level sweep may already have made the asset inaccessible, but
    // that does not turn an unfinished asset-removal intent into a committed
    // result. Only a complete matching tombstone permits the redundant intent
    // (and any post-commit physical deletion retry) to coexist with discovery.
    QHash<QString, QPair<Scope, AssetRemovalDescriptor>> committedRemovals;
    const QFileInfoList assetTombstoneFiles =
        QDir(m_assetRemovalTombstonesPath).entryInfoList(
            {QStringLiteral("*.json")},
            QDir::AllEntries | QDir::NoDotAndDotDot, QDir::Name);
    for (const QFileInfo& fileInfo : assetTombstoneFiles) {
        QJsonObject object;
        Scope storedScope;
        AssetRemovalDescriptor descriptor;
        qint64 quarantinedBytes = -1;
        QString ignored;
        if (fileInfo.isSymLink() || !fileInfo.isFile()
            || !loadJsonObject(fileInfo.absoluteFilePath(), &object, &ignored)
            || object.value(QStringLiteral("schemaVersion")).toInt(-1)
                != MetadataSchemaVersion
            || !parseScope(object, &storedScope)
            || !parseAssetRemovalDescriptor(object, &descriptor)
            || fileInfo.fileName()
                != QFileInfo(assetRemovalTombstonePath(descriptor.removalId))
                       .fileName()
            || object.value(QStringLiteral("quarantineEntry")).toString()
                != assetQuarantineName(storedScope, descriptor.assetId,
                                       descriptor.removalId)
            || !parseCanonicalNonNegativeInteger(
                object.value(QStringLiteral("quarantinedBytes")),
                &quarantinedBytes)
            || quarantinedBytes != descriptor.size
            || committedRemovals.contains(descriptor.removalId)) {
            setError(QStringLiteral("asset_removal_tombstone_invalid"),
                     errorCode);
            return false;
        }
        committedRemovals.insert(
            descriptor.removalId, qMakePair(storedScope, descriptor));
    }

    const QFileInfoList assetIntentFiles =
        QDir(m_assetRemovalIntentsPath).entryInfoList(
            {QStringLiteral("*.json")},
            QDir::AllEntries | QDir::NoDotAndDotDot, QDir::Name);
    for (const QFileInfo& fileInfo : assetIntentFiles) {
        QJsonObject object;
        Scope storedScope;
        AssetRemovalDescriptor descriptor;
        QString ignored;
        if (fileInfo.isSymLink() || !fileInfo.isFile()
            || !loadJsonObject(fileInfo.absoluteFilePath(), &object, &ignored)
            || object.value(QStringLiteral("schemaVersion")).toInt(-1)
                != MetadataSchemaVersion
            || !parseScope(object, &storedScope)
            || !parseAssetRemovalDescriptor(object, &descriptor)
            || fileInfo.fileName()
                != QFileInfo(assetRemovalIntentPath(descriptor.removalId))
                       .fileName()
            || (object.value(QStringLiteral("phase")).toString()
                    != QLatin1String("prepared")
                && object.value(QStringLiteral("phase")).toString()
                    != QLatin1String("quarantined"))
            || object.value(QStringLiteral("quarantineEntry")).toString()
                != assetQuarantineName(storedScope, descriptor.assetId,
                                       descriptor.removalId)) {
            setError(QStringLiteral("asset_removal_intent_invalid"), errorCode);
            return false;
        }
        const auto committed = committedRemovals.constFind(
            descriptor.removalId);
        if (committed == committedRemovals.cend()
            || !(committed->first == storedScope)
            || !assetRemovalDescriptorsEqual(committed->second, descriptor)) {
            setError(QStringLiteral("logical_cleanup_not_committed"), errorCode);
            return false;
        }
    }

    // This also detects valid live scopes without any intent/tombstone and
    // reports malformed/symlinked live metadata through enumerationError.
    QString enumerationError;
    const QList<Scope> live = liveScopes(&enumerationError);
    if (!enumerationError.isEmpty() || !live.isEmpty()) {
        setError(enumerationError.isEmpty()
                     ? QStringLiteral("live_remote_cache_scope")
                     : enumerationError,
                 errorCode);
        return false;
    }
    return true;
}

bool RemoteCacheStore::recoverAssetRemovalIntents(QString* errorCode)
{
    const QFileInfoList intentFiles =
        QDir(m_assetRemovalIntentsPath).entryInfoList(
            {QStringLiteral("*.json")},
            QDir::AllEntries | QDir::NoDotAndDotDot,
            QDir::Name);
    for (const QFileInfo& fileInfo : intentFiles) {
        if (fileInfo.isSymLink() || !fileInfo.isFile()) {
            setError(QStringLiteral("symlink_refused"), errorCode);
            return false;
        }

        QJsonObject intent;
        Scope intentScope;
        AssetRemovalDescriptor command;
        QString localError;
        if (!loadJsonObject(fileInfo.absoluteFilePath(), &intent, &localError)
            || intent.value(QStringLiteral("schemaVersion")).toInt(-1)
                != MetadataSchemaVersion
            || !parseScope(intent, &intentScope)
            || !parseAssetRemovalDescriptor(intent, &command)
            || fileInfo.completeBaseName()
                != QFileInfo(assetRemovalIntentPath(command.removalId))
                       .completeBaseName()) {
            setError(QStringLiteral("asset_removal_intent_invalid"), errorCode);
            return false;
        }

        const AssetRemovalResult recovered =
            removeValidatedAsset(intentScope, command);
        if (recovered.outcome == CommitOutcome::InvalidRequest
            || recovered.outcome == CommitOutcome::Conflict) {
            setError(recovered.errorCode.isEmpty()
                         ? QStringLiteral("asset_removal_intent_invalid")
                         : recovered.errorCode,
                     errorCode);
            return false;
        }
        if (recovered.acknowledgementSafe()
            && QFileInfo::exists(fileInfo.absoluteFilePath())) {
            // The tombstone is already authoritative. Failure to unlink this
            // redundant intent is retryable on the next process start.
            if (QFile::remove(fileInfo.absoluteFilePath())) {
                syncDirectory(m_assetRemovalIntentsPath);
            }
        }
        // CleanupError keeps the intent durable. The subsequent abandoned
        // session quarantine remains fail-closed and purges every live asset;
        // a later startup can converge once the filesystem error is gone.
    }
    return true;
}

bool RemoteCacheStore::recoverIntents(QString* errorCode)
{
    const QFileInfoList intentFiles = QDir(m_intentsPath).entryInfoList(
        {QStringLiteral("*.json")}, QDir::AllEntries | QDir::NoDotAndDotDot,
        QDir::Name);
    for (const QFileInfo& fileInfo : intentFiles) {
        if (fileInfo.isSymLink() || !fileInfo.isFile()) {
            setError(QStringLiteral("symlink_refused"), errorCode);
            return false;
        }
        QJsonObject intent;
        Scope scope;
        QString localError;
        if (!loadJsonObject(fileInfo.absoluteFilePath(), &intent, &localError)
            || intent.value(QStringLiteral("schemaVersion")).toInt(-1) != MetadataSchemaVersion
            || !parseScope(intent, &scope)) {
            setError(QStringLiteral("teardown_intent_invalid"), errorCode);
            return false;
        }
        const QString teardownId = intent.value(QStringLiteral("teardownId")).toString();
        if (!isValidTeardownId(teardownId)
            || fileInfo.completeBaseName() != scopeKey(scope)) {
            setError(QStringLiteral("teardown_intent_invalid"), errorCode);
            return false;
        }
        const CommitResult result = commitTeardown(scope, teardownId);
        if (result.outcome == CommitOutcome::InvalidRequest
            || result.outcome == CommitOutcome::Conflict) {
            setError(result.errorCode, errorCode);
            return false;
        }
        if (result.outcome == CommitOutcome::AlreadyCommitted) {
            QFile::remove(fileInfo.absoluteFilePath());
        }
        // CleanupError remains durable and keeps this scope unavailable.  It
        // must not prevent unrelated devices from starting.
    }
    return true;
}

bool RemoteCacheStore::quarantineAbandonedSessions(QString* errorCode)
{
    const QFileInfoList senderEntries = QDir(m_rootPath).entryInfoList(
        QDir::Dirs | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System,
        QDir::Name);
    for (const QFileInfo& senderInfo : senderEntries) {
        const QString senderName = senderInfo.fileName();
        if (senderName == QLatin1String(kStateDirectory)
            || senderName == QLatin1String(kQuarantineDirectory)) {
            continue;
        }
        if (senderInfo.isSymLink()) {
            setError(QStringLiteral("symlink_refused"), errorCode);
            return false;
        }
        if (!isValidEndpointId(senderName)) {
            // Unknown legacy directories are handled by the one-time v2
            // migration.  Never traverse or interpret them as a live scope.
            continue;
        }

        const QFileInfoList sessionEntries = QDir(senderInfo.absoluteFilePath()).entryInfoList(
            QDir::Dirs | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System,
            QDir::Name);
        for (const QFileInfo& sessionInfo : sessionEntries) {
            if (sessionInfo.isSymLink()) {
                setError(QStringLiteral("symlink_refused"), errorCode);
                return false;
            }
            if (!isValidSessionId(sessionInfo.fileName())) {
                continue;
            }
            const QString descriptorPath = QDir(sessionInfo.absoluteFilePath())
                                               .filePath(QStringLiteral(".scope.json"));
            QJsonObject descriptor;
            Scope scope;
            QString localError;
            if (!QFileInfo::exists(descriptorPath)) {
                // Pre-v2 cache.  The explicit one-time migration owns it; it
                // must never be mistaken for an authenticated session scope.
                continue;
            }
            if (!loadJsonObject(descriptorPath, &descriptor, &localError)
                || descriptor.value(QStringLiteral("schemaVersion")).toInt(-1)
                    != MetadataSchemaVersion
                || !parseScope(descriptor, &scope)
                || scope.senderEndpointId != senderName
                || scope.remoteSessionId != sessionInfo.fileName()) {
                setError(QStringLiteral("scope_metadata_invalid"), errorCode);
                return false;
            }
            if (QFileInfo::exists(tombstonePath(scope))) {
                continue;
            }
            if (QFileInfo::exists(intentPath(scope))) {
                // recoverIntents() already retried this terminal transaction.
                // A durable cleanup_error must not be replaced with a new id.
                continue;
            }
            const QString recoveryTeardown =
                QUuid::createUuid().toString(QUuid::WithoutBraces).toLower();
            QString beginError;
            if (!beginProvisionalTeardown(scope, recoveryTeardown,
                                          QStringLiteral("process_restart"),
                                          &beginError)) {
                setError(beginError, errorCode);
                return false;
            }
            const CommitResult result = commitTeardown(scope, recoveryTeardown);
            if (!result.acknowledgementSafe()
                && result.outcome != CommitOutcome::CleanupError) {
                setError(result.errorCode, errorCode);
                return false;
            }
        }
    }
    return true;
}

bool RemoteCacheStore::sweepQuarantine(QString* errorCode)
{
    QSet<QString> referencedEntries;
    const QFileInfoList tombstoneFiles = QDir(m_tombstonesPath).entryInfoList(
        {QStringLiteral("*.json")}, QDir::AllEntries | QDir::NoDotAndDotDot,
        QDir::Name);
    for (const QFileInfo& fileInfo : tombstoneFiles) {
        if (fileInfo.isSymLink() || !fileInfo.isFile()) {
            setError(QStringLiteral("symlink_refused"), errorCode);
            return false;
        }
        QJsonObject object;
        Scope scope;
        QString localError;
        if (!loadJsonObject(fileInfo.absoluteFilePath(), &object, &localError)
            || object.value(QStringLiteral("schemaVersion")).toInt(-1) != MetadataSchemaVersion
            || !parseScope(object, &scope)) {
            setError(QStringLiteral("tombstone_invalid"), errorCode);
            return false;
        }
        const QString entry = object.value(QStringLiteral("quarantineEntry")).toString();
        if (!entry.isEmpty()) {
            referencedEntries.insert(entry);
        }
        const std::optional<Tombstone> stored = tombstone(scope);
        if (stored && !entry.isEmpty()
            && (stored->state == SessionState::CleanupPending
                || stored->state == SessionState::CleanupError)) {
            schedulePhysicalCleanup(*stored, entry);
        }
    }

    const QFileInfoList quarantineEntries = QDir(m_quarantinePath).entryInfoList(
        QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System,
        QDir::Name);
    for (const QFileInfo& entry : quarantineEntries) {
        if (entry.isSymLink()) {
            setError(QStringLiteral("symlink_refused"), errorCode);
            return false;
        }
        if (!referencedEntries.contains(entry.fileName())) {
            scheduleOrphanCleanup(entry.fileName());
        }
    }
    return true;
}

void RemoteCacheStore::schedulePhysicalCleanup(const Tombstone& tombstoneValue,
                                               const QString& quarantineEntry)
{
    if (quarantineEntry.isEmpty() || m_scheduledEntries.contains(quarantineEntry)) {
        return;
    }
    const QString path = quarantinePath(quarantineEntry);
    if (!isDirectChild(m_quarantinePath, path)) {
        return;
    }
    if (!QFileInfo::exists(path) && !QFileInfo(path).isSymLink()) {
        QJsonObject object;
        QString ignored;
        if (loadJsonObject(tombstonePath(tombstoneValue.scope), &object, &ignored)) {
            object.insert(QStringLiteral("cleanupState"), QStringLiteral("deleted"));
            object.insert(QStringLiteral("cleanupCompletedAt"), utcNow());
            object.remove(QStringLiteral("errorCode"));
            writeJsonAtomically(tombstonePath(tombstoneValue.scope), object, &ignored);
        }
        return;
    }

    m_scheduledEntries.insert(quarantineEntry);
    DeleteResult work;
    work.tombstone = tombstoneValue;
    work.quarantineEntry = quarantineEntry;
    work.tombstoneFile = tombstonePath(tombstoneValue.scope);
    auto* watcher = new QFutureWatcher<DeleteResult>(this);
    connect(watcher, &QFutureWatcher<DeleteResult>::finished, this, [this, watcher]() {
        const DeleteResult result = watcher->result();
        watcher->deleteLater();
        finishPhysicalCleanup(result);
    });
    watcher->setFuture(QtConcurrent::run([work, path]() mutable {
        work.success = deleteTreeWithoutFollowingLinks(path,
                                                       &work.bytesRemoved,
                                                       &work.errorCode);
        return work;
    }));
}

void RemoteCacheStore::scheduleOrphanCleanup(const QString& quarantineEntry)
{
    static const QRegularExpression safeEntry(QStringLiteral("^[A-Za-z0-9_-]{1,128}$"));
    if (!safeEntry.match(quarantineEntry).hasMatch()
        || m_scheduledEntries.contains(quarantineEntry)) {
        return;
    }
    const QString path = quarantinePath(quarantineEntry);
    if (!isDirectChild(m_quarantinePath, path)) {
        return;
    }
    m_scheduledEntries.insert(quarantineEntry);
    DeleteResult work;
    work.orphan = true;
    work.quarantineEntry = quarantineEntry;
    work.cleanupErrorFile = QDir(m_cleanupErrorsPath)
                                .filePath(quarantineEntry + QStringLiteral(".json"));
    auto* watcher = new QFutureWatcher<DeleteResult>(this);
    connect(watcher, &QFutureWatcher<DeleteResult>::finished, this, [this, watcher]() {
        const DeleteResult result = watcher->result();
        watcher->deleteLater();
        finishPhysicalCleanup(result);
    });
    watcher->setFuture(QtConcurrent::run([work, path]() mutable {
        work.success = deleteTreeWithoutFollowingLinks(path,
                                                       &work.bytesRemoved,
                                                       &work.errorCode);
        return work;
    }));
}

void RemoteCacheStore::finishPhysicalCleanup(const DeleteResult& result)
{
    m_scheduledEntries.remove(result.quarantineEntry);
    if (result.orphan) {
        if (result.success) {
            QFile::remove(result.cleanupErrorFile);
            syncDirectory(m_cleanupErrorsPath);
            return;
        }
        QJsonObject errorObject {
            {QStringLiteral("schemaVersion"), MetadataSchemaVersion},
            {QStringLiteral("quarantineEntry"), result.quarantineEntry},
            {QStringLiteral("state"), QStringLiteral("cleanup_error")},
            {QStringLiteral("errorCode"), result.errorCode},
            {QStringLiteral("updatedAt"), utcNow()}
        };
        QString ignored;
        writeJsonAtomically(result.cleanupErrorFile, errorObject, &ignored);
        return;
    }

    QJsonObject object;
    QString ignored;
    if (!loadJsonObject(result.tombstoneFile, &object, &ignored)) {
        return;
    }
    object.insert(QStringLiteral("cleanupState"),
                  result.success ? QStringLiteral("deleted") : QStringLiteral("error"));
    object.insert(QStringLiteral("cleanupCompletedAt"), utcNow());
    if (result.success) {
        object.remove(QStringLiteral("errorCode"));
    } else {
        object.insert(QStringLiteral("errorCode"), result.errorCode);
    }
    if (!writeJsonAtomically(result.tombstoneFile, object, &ignored)) {
        return;
    }

    Scope closedScope;
    if (!parseScope(object, &closedScope)) {
        return;
    }
    const QString currentTeardownId =
        object.value(QStringLiteral("teardownId")).toString();
    if (!isValidTeardownId(currentTeardownId)) {
        return;
    }
    if (result.success) {
        emit physicalCleanupCompleted(closedScope.senderEndpointId,
                                      closedScope.remoteSessionId,
                                      closedScope.generation,
                                      currentTeardownId,
                                      result.bytesRemoved);
    } else {
        emit physicalCleanupFailed(closedScope.senderEndpointId,
                                   closedScope.remoteSessionId,
                                   closedScope.generation,
                                   currentTeardownId,
                                   result.errorCode);
    }
}
