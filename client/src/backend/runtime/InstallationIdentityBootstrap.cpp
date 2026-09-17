#include "InstallationIdentityBootstrap.h"

#include "backend/runtime/storage/StorageIO.h"
#include "backend/runtime/storage/StorageVersions.h"
#include "backend/security/DeviceIdentityStore.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonObject>
#include <QLockFile>
#include <QSaveFile>

#include <filesystem>
#include <system_error>

namespace {
bool safeDirectoryAncestors(const QString& path)
{
    const QString absolute = QDir::cleanPath(QFileInfo(path).absoluteFilePath());
    QString anchor = QDir::rootPath();
    for (const QString& trusted : {QDir::homePath(), QDir::tempPath()}) {
        const QString candidate = QDir::cleanPath(trusted);
        if (absolute == candidate || absolute.startsWith(candidate + QLatin1Char('/'))) {
            if (candidate.size() > anchor.size()) anchor = candidate;
        }
    }
    QString current = absolute;
    while (current != anchor) {
        const QFileInfo info(current);
        if (info.isSymLink() || (info.exists() && !info.isDir())) return false;
        const QString parent = info.absolutePath();
        if (parent == current) break;
        current = parent;
    }
    return true;
}
}

bool InstallationIdentityBootstrap::prepare(const RuntimeProfileContext& context,
                                            QString* error, RuntimeStorage::Report* report)
{
    using namespace RuntimeStorage;
    const QString root = RuntimeProfile::resolvedInstallationRoot(context);
    Report result{QStringLiteral("identity"), -1, StorageVersions::Identity,
                  Action::None, {}, Failure::None};
    const auto fail = [&](const QString& reason) {
        if (error) *error = reason;
        result.failure = Failure::IoError;
        result.reason = reason;
        if (report) *report = result;
        return false;
    };
    const QFileInfo rootInfo(root);
    if (root.isEmpty() || !rootInfo.isAbsolute() || QDir(root).isRoot()
        || !safeDirectoryAncestors(root)
        || !QDir().mkpath(root))
        return fail(QStringLiteral("The installation identity directory is unavailable."));
    const QString lockPath = QDir(root).filePath(QStringLiteral("identity.lock"));
    if (QFileInfo(lockPath).isSymLink())
        return fail(QStringLiteral("The installation identity lock is unsafe."));
    QLockFile lock(lockPath);
    lock.setStaleLockTime(0);
    if (!lock.tryLock(5000))
        return fail(QStringLiteral("Another process is preparing the installation identity."));

    const QString metadataPath = QDir(root).filePath(QStringLiteral("storage.json"));
    QJsonObject metadata;
    const Inspection previous = readVersionedJson(root, metadataPath,
        StorageVersions::Identity, 4096, &metadata);
    result.foundVersion = previous.version;
    if (previous.state == State::IoError || previous.state == State::Incompatible
        || QFileInfo(metadataPath).isSymLink()) return fail(previous.reason);

    DeviceIdentityStore identity(root, context.useNativeIdentityVault, context.identityNamespace());
    QString identityError;
    const auto state = identity.inspectStored(&identityError);
    if (state == DeviceIdentityStore::ReadState::Corrupt
        || state == DeviceIdentityStore::ReadState::IoError)
        return fail(identityError);

    bool migrated = false;
    if (state == DeviceIdentityStore::ReadState::Missing) {
        // A durable ready checkpoint proves that a key used to exist. Losing
        // it is not permission to silently rotate every endpoint on the host.
        if (previous.state != State::Missing)
            return fail(QStringLiteral("The installation identity material is missing; it was not replaced."));
        const QString legacyRoot = context.legacyPrimaryRootPath.isEmpty()
            ? context.rootPath : context.legacyPrimaryRootPath;
        const QString legacyDirectory = QDir(legacyRoot).filePath(QStringLiteral("identity"));
        if (!legacyRoot.isEmpty() && QDir::cleanPath(legacyDirectory) != QDir::cleanPath(root)) {
            if (!safeDirectoryAncestors(legacyDirectory))
                return fail(QStringLiteral("The previous primary identity directory is unsafe."));
            DeviceIdentityStore legacy(legacyDirectory, context.useNativeIdentityVault,
                                       context.identityNamespace());
            const auto legacyState = legacy.inspectStored(&identityError);
            if (legacyState == DeviceIdentityStore::ReadState::Corrupt
                || legacyState == DeviceIdentityStore::ReadState::IoError) return fail(identityError);
            if (legacyState == DeviceIdentityStore::ReadState::Missing) {
                const QString checkpointPath = QDir(legacyDirectory).filePath(QStringLiteral("storage.json"));
#ifdef Q_OS_WIN
                const std::filesystem::path nativeCheckpoint(checkpointPath.toStdWString());
#else
                const std::filesystem::path nativeCheckpoint(QFile::encodeName(checkpointPath).constData());
#endif
                std::error_code checkpointError;
                const auto checkpointStatus = std::filesystem::symlink_status(nativeCheckpoint, checkpointError);
                // StorageIO requires an existing directory root. Probe true
                // absence first, preserving EACCES and all other I/O errors:
                // QFileInfo::exists() alone cannot distinguish them from ENOENT.
                if (checkpointError && checkpointError != std::errc::no_such_file_or_directory)
                    return fail(QStringLiteral("Cannot inspect the previous primary identity checkpoint: %1")
                                    .arg(QString::fromStdString(checkpointError.message())));
                // The old primary profile can still be the only evidence of
                // an established identity. A lost key must not become a new
                // installation merely because migration has not run yet.
                if (checkpointStatus.type() != std::filesystem::file_type::not_found) {
                    const Inspection legacyCheckpoint = readVersionedJson(
                        legacyDirectory, checkpointPath, StorageVersions::Identity, 4096);
                    if (legacyCheckpoint.state == State::IoError) return fail(legacyCheckpoint.reason);
                    return fail(QStringLiteral("The previous primary identity material is missing; it was not replaced."));
                }
            }
            if (legacyState == DeviceIdentityStore::ReadState::Valid) {
                if (!legacy.initialize(&identityError)) return fail(identityError);
                // Native-vault accounts deliberately keep their historical
                // channel:instance-1 namespace. Only the file fallback moves.
                if (legacy.storageBackend() == DeviceIdentityStore::StorageBackend::OwnerOnlyFile) {
                    QFile source(legacy.fallbackFilePath());
                    if (!source.open(QIODevice::ReadOnly)) return fail(source.errorString());
                    const QByteArray bytes = source.readAll();
                    QSaveFile destination(identity.fallbackFilePath());
                    destination.setDirectWriteFallback(false);
                    if (!destination.open(QIODevice::WriteOnly)
                        || !destination.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner)
                        || destination.write(bytes) != bytes.size() || !destination.commit())
                        return fail(destination.errorString());
                }
                if (!identity.initialize(&identityError)) return fail(identityError);
                if (identity.installationId() != legacy.installationId())
                    return fail(QStringLiteral("The migrated installation identity does not match the primary identity."));
                migrated = true;
            }
        }
    }
    if (!identity.initialize(&identityError)) return fail(identityError);
    const QString recordedIdentity = metadata.value(QStringLiteral("installationId")).toString();
    if (!recordedIdentity.isEmpty() && recordedIdentity != identity.installationId())
        return fail(QStringLiteral("The installation identity differs from its durable checkpoint."));
    const QJsonObject ready{
        {QStringLiteral("schemaVersion"), StorageVersions::Identity},
        {QStringLiteral("phase"), QStringLiteral("ready")},
        {QStringLiteral("installationId"), identity.installationId()}
    };
    if (metadata != ready) {
        const Operation committed = writeJson(root, metadataPath, ready);
        if (!committed.succeeded()) return fail(committed.reason);
    }
    result.action = migrated ? Action::Migrated
        : previous.state == State::Missing ? Action::Initialized
        : metadata == ready ? Action::Preserved : Action::Migrated;
    if (report) *report = result;
    if (error) error->clear();
    return true;
}
