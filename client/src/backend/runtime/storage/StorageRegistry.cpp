#include "StorageRegistry.h"
#include "StorageIO.h"
#include "StorageVersions.h"
#include "migrations/settings/v0_to_v1.h"
#include "backend/domain/project/ProjectStore.h"
#include "backend/notifications/HistoryStore.h"
#include "backend/security/DeviceIdentityStore.h"

#include <QDir>
#include <QFileInfo>
#include <QJsonArray>

namespace RuntimeStorage {
namespace {
constexpr qint64 kDocumentLimit = 64 * 1024 * 1024;

Inspection inspectProjects(const QString& root, const QString& path)
{
    Inspection result = readVersionedJson(root, path, StorageVersions::Projects, kDocumentLimit);
    if (result.state != State::Current) return result;
    ProjectStore store(path);
    QList<ProjectRecord> records;
    if (!store.load(&records)) {
        result.state = store.lastReadFailure() == Failure::IoError ? State::IoError : State::Corrupt;
        result.reason = store.lastError();
    }
    return result;
}

Inspection inspectHistory(const QString& root, const QString& path)
{
    Inspection result = readVersionedJson(root, path, StorageVersions::History, kDocumentLimit);
    if (result.state != State::Current) return result;
    HistoryStore store(path);
    NotificationHistoryData records;
    if (!store.load(&records)) {
        result.state = store.lastReadFailure() == Failure::IoError ? State::IoError : State::Corrupt;
        result.reason = store.lastError();
    }
    return result;
}

QString identityDirectory(const RuntimeProfileContext& context)
{
    return QDir(context.rootPath).filePath(QStringLiteral("identity"));
}

QString identityMetadata(const RuntimeProfileContext& context)
{
    return QDir(identityDirectory(context)).filePath(QStringLiteral("storage.json"));
}

Inspection inspectIdentity(const RuntimeProfileContext& context)
{
    QJsonObject metadata;
    Inspection result = readVersionedJson(context.rootPath, identityMetadata(context),
                                         StorageVersions::Identity, 4096, &metadata);
    if (result.state != State::Current) return result;
    if (metadata.value(QStringLiteral("phase")).toString() != QLatin1String("ready"))
        return {State::Corrupt, result.version, QStringLiteral("Interrupted identity operation")};
    DeviceIdentityStore store(identityDirectory(context), context.isPersistent(), context.identityNamespace());
    QString error;
    switch (store.inspectStored(&error)) {
    case DeviceIdentityStore::ReadState::Valid: return result;
    case DeviceIdentityStore::ReadState::Missing:
        return {State::Corrupt, result.version, QStringLiteral("Identity material is missing")};
    case DeviceIdentityStore::ReadState::Corrupt: return {State::Corrupt, result.version, error};
    case DeviceIdentityStore::ReadState::IoError: return {State::IoError, result.version, error};
    }
    return {State::IoError, result.version, QStringLiteral("Cannot inspect identity")};
}

Operation resetIdentity(const RuntimeProfileContext& context)
{
    const Operation prepared = ensureDirectory(context.rootPath, identityDirectory(context));
    if (!prepared.succeeded()) return prepared;
    const QString path = identityMetadata(context);
    QJsonObject previous;
    const Inspection existing = readVersionedJson(context.rootPath, path,
                                                 StorageVersions::Identity, 4096, &previous);
    if (existing.state == State::IoError) return {Failure::IoError, existing.reason};
    const auto checkpoint = [&](const QString& phase) {
        return writeJson(context.rootPath, path,
                         {{QStringLiteral("schemaVersion"), StorageVersions::Identity},
                          {QStringLiteral("phase"), phase}});
    };
    DeviceIdentityStore store(identityDirectory(context), context.isPersistent(), context.identityNamespace());
    QString error;
    const auto stored = store.inspectStored(&error);
    if (stored == DeviceIdentityStore::ReadState::IoError) return {Failure::IoError, error};
    const bool resumeCreation = existing.state == State::Current
        && previous.value(QStringLiteral("phase")).toString() == QLatin1String("creating")
        && stored != DeviceIdentityStore::ReadState::Corrupt;
    if (!resumeCreation) {
        Operation operation = checkpoint(QStringLiteral("resetting"));
        if (!operation.succeeded()) return operation;
        if (!store.reset(&error)) return {Failure::IoError, error};
        operation = checkpoint(QStringLiteral("creating"));
        if (!operation.succeeded()) return operation;
    }
    // A crash after creation but before the ready checkpoint reuses the new
    // key. No second rotation and no backup of the previous key.
    if (!store.initialize(&error)) return {Failure::IoError, error};
    return checkpoint(QStringLiteral("ready"));
}
}

Operation purgeReceivedMedia(const RuntimeProfileContext& context)
{
    const QString cache = QDir(context.rootPath).filePath(QStringLiteral("cache"));
    Operation operation = ensureDirectory(context.rootPath, cache);
    if (!operation.succeeded()) return operation;
    const QString uploads = QDir(cache).filePath(QStringLiteral("Uploads"));
    operation = removeOwned(context.rootPath, uploads);
    if (!operation.succeeded()) return operation;
    return ensureDirectory(context.rootPath, uploads);
}

Operation clearProfileStorage(const RuntimeProfileContext& context)
{
    const QString root = QDir::cleanPath(context.rootPath);
    const QFileInfo info(root);
    if (context.rootPath.trimmed().isEmpty() || !info.isAbsolute()
        || QDir(root).isRoot() || info.isSymLink()
        || (info.exists() && !info.isDir())
        || (context.channel != QLatin1String("development")
            && context.channel != QLatin1String("production"))) {
        return {Failure::IoError, QStringLiteral("Refusing to clear an invalid runtime root: %1").arg(root)};
    }
    if (info.exists()) {
        // Remove the owned directory first. Even a substituted identity/
        // symlink must never let DeviceIdentityStore::reset follow its parent
        // to delete an external fallback key.
        const Operation removed = removeOwned(root, identityDirectory(context));
        if (!removed.succeeded()) return removed;
    }
    DeviceIdentityStore identity(identityDirectory(context), context.isPersistent(), context.identityNamespace());
    QString identityError;
    const bool identityCleared = identity.reset(&identityError);
    // The entire selected root is removed, including unregistered/obsolete
    // files. No sibling channel or external source is part of this operation.
    const Operation removed = info.exists() ? removeOwned(info.absolutePath(), root) : Operation{};
    if (!removed.succeeded()) return removed;
    if (!identityCleared) return {Failure::IoError, identityError};
    return {};
}

QList<Component> components(const RuntimeProfileContext& context)
{
    const QString root = context.rootPath;
    const QString settings = QDir(root).filePath(QStringLiteral("settings/settings.ini"));
    const QString projects = QDir(root).filePath(QStringLiteral("projects/projects-v2.json"));
    const QString history = QDir(root).filePath(QStringLiteral("notification-history-v1.json"));
    const QString cacheMetadata = QDir(root).filePath(QStringLiteral("cache/storage.json"));

    return {
        {QStringLiteral("settings"), StorageVersions::Settings,
         [=] { return readSettings(root, settings).inspection; },
         [=] { return writeSettings(root, settings,
                    {{QStringLiteral("serverUrl"), QStringLiteral("ws://localhost:8080")},
                     {QStringLiteral("autoUploadImportedMedia"), false}}, StorageVersions::Settings); },
         {{0, 1, Transition::Kind::Migrate,
           [=] { return Migrations::settingsV0ToV1(root, settings); }}}},
        {QStringLiteral("projects"), StorageVersions::Projects,
         [=] { return inspectProjects(root, projects); },
         [=] { return writeJson(root, projects,
                    {{QStringLiteral("schemaVersion"), StorageVersions::Projects},
                     {QStringLiteral("projects"), QJsonArray{}}}); },
         {{1, 4, Transition::Kind::Reset, {}},
          {2, 4, Transition::Kind::Reset, {}},
          {3, 4, Transition::Kind::Reset, {}}}},
        {QStringLiteral("history"), StorageVersions::History,
         [=] { return inspectHistory(root, history); },
         [=] { return writeJson(root, history,
                    {{QStringLiteral("schemaVersion"), StorageVersions::History},
                     {QStringLiteral("entries"), QJsonArray{}},
                     {QStringLiteral("terminalCorrelationIds"), QJsonArray{}}}); }, {}},
        {QStringLiteral("identity"), StorageVersions::Identity,
         [=] { return inspectIdentity(context); },
         [=] { return resetIdentity(context); }, {}},
        {QStringLiteral("cache"), StorageVersions::ReceivedMedia,
         [=] { return readVersionedJson(root, cacheMetadata, StorageVersions::ReceivedMedia, 4096); },
         [=] {
             const Operation purged = purgeReceivedMedia(context);
             if (!purged.succeeded()) return purged;
             return writeJson(root, cacheMetadata,
                              {{QStringLiteral("schemaVersion"), StorageVersions::ReceivedMedia}});
         }, {}}
    };
}
}
