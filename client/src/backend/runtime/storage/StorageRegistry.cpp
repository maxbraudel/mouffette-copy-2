#include "StorageRegistry.h"
#include "StorageIO.h"
#include "StorageVersions.h"
#include "migrations/settings/v0_to_v1.h"
#include "migrations/projects/v6_to_v7.h"
#include "backend/domain/project/ProjectStore.h"
#include "backend/notifications/HistoryStore.h"

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
    const QString installation = QDir::cleanPath(RuntimeProfile::resolvedInstallationRoot(context));
    if (installation == root || installation.startsWith(root + QLatin1Char('/')))
        return {Failure::IoError, QStringLiteral("The profile contains shared installation identity; refusing removal.")};
    // The entire selected root is removed, including unregistered/obsolete
    // files. No sibling channel or external source is part of this operation.
    const Operation removed = info.exists() ? removeOwned(info.absolutePath(), root) : Operation{};
    if (!removed.succeeded()) return removed;
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
                     {QStringLiteral("autoUploadImportedMedia"), false},
                     {QStringLiteral("appAlwaysOnTop"), true}}, StorageVersions::Settings); },
         {{0, 1, Transition::Kind::Migrate,
           [=] { return Migrations::settingsV0ToV1(root, settings); }}}},
        {QStringLiteral("projects"), StorageVersions::Projects,
         [=] { return inspectProjects(root, projects); },
         [=] { return writeJson(root, projects,
                    {{QStringLiteral("schemaVersion"), StorageVersions::Projects},
                     {QStringLiteral("projects"), QJsonArray{}}}); },
         {{1, 6, Transition::Kind::Reset, {}},
          {2, 6, Transition::Kind::Reset, {}},
          {3, 6, Transition::Kind::Reset, {}},
          {4, 6, Transition::Kind::Reset, {}},
          {5, 6, Transition::Kind::Reset, {}},
          {6, 7, Transition::Kind::Migrate,
           [=] { return Migrations::projectsV6ToV7(root, projects); }}}},
        {QStringLiteral("history"), StorageVersions::History,
         [=] { return inspectHistory(root, history); },
         [=] { return writeJson(root, history,
                    {{QStringLiteral("schemaVersion"), StorageVersions::History},
                     {QStringLiteral("entries"), QJsonArray{}},
                     {QStringLiteral("terminalCorrelationIds"), QJsonArray{}}}); }, {}},
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
