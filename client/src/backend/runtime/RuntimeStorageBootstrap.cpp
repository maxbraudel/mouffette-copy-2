#include "backend/runtime/RuntimeStorageBootstrap.h"

#include "AppBuildConfig.h"
#include "backend/config/AppConfig.h"
#include "backend/domain/project/ProjectStore.h"
#include "backend/security/DeviceIdentityStore.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QSaveFile>
#include <QSettings>
#include <QStandardPaths>
#include <QSet>

#include <utility>

namespace {
constexpr auto kManifestFormat = "mouffette-runtime";

QString persistenceName(const RuntimeProfileContext& context)
{
    return context.isPersistent() ? QStringLiteral("persistent")
                                  : QStringLiteral("temporary");
}

bool removeEntry(const QFileInfo& info)
{
    if (!info.exists() && !info.isSymLink()) return true;
    if (info.isSymLink() || info.isFile()) return QFile::remove(info.absoluteFilePath());
    return info.isDir() && QDir(info.absoluteFilePath()).removeRecursively();
}

bool ensurePrivateDirectory(const QString& path)
{
    const QFileInfo info(path);
    if (info.exists() || info.isSymLink()) {
        return info.isDir() && !info.isSymLink();
    }
    return QDir().mkpath(path);
}

bool resetRuntimeContents(const RuntimeProfileContext& context)
{
    const QDir root(context.rootPath);
    if (!root.exists()) return QDir().mkpath(context.rootPath);
    const QFileInfo rootInfo(context.rootPath);
    if (!rootInfo.isDir() || rootInfo.isSymLink()) return false;
    const QFileInfoList entries = root.entryInfoList(
        QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System);
    for (const QFileInfo& entry : entries) {
        if (context.isTemporary() && entry.fileName() == QStringLiteral("active.lock")) {
            continue;
        }
        if (!removeEntry(entry)) return false;
    }
    return true;
}

bool writeManifest(const RuntimeProfileContext& context)
{
    QJsonObject manifest;
    manifest.insert(QStringLiteral("format"), QString::fromLatin1(kManifestFormat));
    manifest.insert(QStringLiteral("storageVersion"), RuntimeStorageBootstrap::StorageVersion);
    manifest.insert(QStringLiteral("profileId"), context.profileId);
    manifest.insert(QStringLiteral("persistence"), persistenceName(context));
    const QByteArray encoded = QJsonDocument(manifest).toJson(QJsonDocument::Compact);
    QSaveFile file(QDir(context.rootPath).filePath(QStringLiteral("storage-manifest.json")));
    file.setDirectWriteFallback(false);
    return file.open(QIODevice::WriteOnly)
        && file.write(encoded) == encoded.size()
        && file.commit();
}

enum class ManifestState { Valid, Missing, Invalid, VersionMismatch };

ManifestState inspectManifest(const RuntimeProfileContext& context, QString* foundVersion)
{
    const QString path = QDir(context.rootPath).filePath(
        QStringLiteral("storage-manifest.json"));
    const QFileInfo info(path);
    if (!info.exists() && !info.isSymLink()) {
        if (foundVersion) *foundVersion = QStringLiteral("missing");
        return ManifestState::Missing;
    }
    if (!info.isFile() || info.isSymLink()) {
        if (foundVersion) *foundVersion = QStringLiteral("invalid");
        return ManifestState::Invalid;
    }
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly) || file.size() > 64 * 1024) {
        if (foundVersion) *foundVersion = QStringLiteral("invalid");
        return ManifestState::Invalid;
    }
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(file.readAll(), &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
        if (foundVersion) *foundVersion = QStringLiteral("invalid");
        return ManifestState::Invalid;
    }
    const QJsonObject manifest = document.object();
    const QSet<QString> expectedKeys{
        QStringLiteral("format"), QStringLiteral("storageVersion"),
        QStringLiteral("profileId"), QStringLiteral("persistence")};
    const QStringList manifestKeys = manifest.keys();
    if (QSet<QString>(manifestKeys.cbegin(), manifestKeys.cend()) != expectedKeys
        || !manifest.value(QStringLiteral("format")).isString()
        || !manifest.value(QStringLiteral("storageVersion")).isDouble()
        || !manifest.value(QStringLiteral("profileId")).isString()
        || !manifest.value(QStringLiteral("persistence")).isString()) {
        if (foundVersion) *foundVersion = QStringLiteral("invalid");
        return ManifestState::Invalid;
    }
    const QJsonValue versionValue = manifest.value(QStringLiteral("storageVersion"));
    const double rawVersion = versionValue.toDouble(-1.0);
    if (foundVersion) *foundVersion = QString::number(rawVersion, 'g', 16);
    if (rawVersion != RuntimeStorageBootstrap::StorageVersion) {
        return ManifestState::VersionMismatch;
    }
    if (manifest.value(QStringLiteral("format")).toString()
            != QString::fromLatin1(kManifestFormat)
        || manifest.value(QStringLiteral("profileId")).toString() != context.profileId
        || manifest.value(QStringLiteral("persistence")).toString()
            != persistenceName(context)) {
        return ManifestState::Invalid;
    }
    return ManifestState::Valid;
}

bool resetSettingsToDefaults()
{
    const QFileInfo info(RuntimeProfile::settingsFilePath());
    if ((info.exists() || info.isSymLink()) && !removeEntry(info)) return false;
    if (!ensurePrivateDirectory(info.absolutePath())) return false;
    QSettings settings(info.absoluteFilePath(), QSettings::IniFormat);
    settings.setValue(QStringLiteral("serverUrl"), QStringLiteral("ws://localhost:8080"));
    settings.setValue(QStringLiteral("autoUploadImportedMedia"), false);
    settings.setValue(QStringLiteral("useQuickCanvasRenderer"), true);
    settings.sync();
    return settings.status() == QSettings::NoError;
}

bool settingsAreValid()
{
    const QFileInfo info(RuntimeProfile::settingsFilePath());
    if (!info.exists() && !info.isSymLink()) return false;
    if (!info.isFile() || info.isSymLink() || info.size() > 1024 * 1024) return false;
    QSettings settings(info.absoluteFilePath(), QSettings::IniFormat);
    if (settings.status() != QSettings::NoError) return false;
    const QSet<QString> allowed{
        QStringLiteral("serverUrl"), QStringLiteral("autoUploadImportedMedia"),
        QStringLiteral("useQuickCanvasRenderer")};
    const QStringList keys = settings.allKeys();
    for (const QString& key : keys) {
        if (!allowed.contains(key)) return false;
    }
    if (!settings.contains(QStringLiteral("serverUrl"))
        || !settings.contains(QStringLiteral("autoUploadImportedMedia"))
        || !settings.contains(QStringLiteral("useQuickCanvasRenderer"))) {
        return false;
    }
    const QVariant serverUrl = settings.value(QStringLiteral("serverUrl"));
    const QVariant autoUpload = settings.value(QStringLiteral("autoUploadImportedMedia"));
    const QVariant quickCanvas = settings.value(QStringLiteral("useQuickCanvasRenderer"));
    QString ignored;
    return serverUrl.metaType().id() == QMetaType::QString
        && autoUpload.metaType().id() == QMetaType::Bool
        && quickCanvas.metaType().id() == QMetaType::Bool
        && AppConfig::validateServerUrl(serverUrl.toString(), nullptr, &ignored);
}

bool resetProjectsToEmpty()
{
    const QFileInfo info(RuntimeProfile::projectsFilePath());
    if ((info.exists() || info.isSymLink()) && !removeEntry(info)) return false;
    ProjectStore store(info.absoluteFilePath());
    return store.save({});
}

bool projectsAreValid()
{
    const QFileInfo info(RuntimeProfile::projectsFilePath());
    if (!info.exists() && !info.isSymLink()) return false;
    if (!info.isFile() || info.isSymLink() || info.size() > 64 * 1024 * 1024) return false;
    ProjectStore store(info.absoluteFilePath());
    QList<ProjectRecord> projects;
    return store.load(&projects);
}

bool removeLegacyStorage(QString* error)
{
    QSettings legacy(QStringLiteral("Mouffette"),
                     QStringLiteral(MOUFFETTE_SETTINGS_APPLICATION));
    const QString legacySettingsPath = legacy.fileName();
    legacy.clear();
    legacy.sync();
    if (legacy.status() != QSettings::NoError) {
        if (error) *error = QStringLiteral("legacy_settings_reset_failed");
        return false;
    }
    if (!legacySettingsPath.isEmpty()
        && !removeEntry(QFileInfo(legacySettingsPath))) {
        if (error) *error = QStringLiteral("legacy_settings_reset_failed");
        return false;
    }

    const QString oldData = QStandardPaths::writableLocation(
        QStandardPaths::AppDataLocation);
    const QString oldCache = QStandardPaths::writableLocation(
        QStandardPaths::CacheLocation);
    const QList<QString> legacyPaths{
        QDir(oldData).filePath(QStringLiteral("projects-v2.json")),
        QDir(oldData).filePath(QStringLiteral("history-v1.json")),
        QDir(oldCache).filePath(QStringLiteral("Mouffette/Uploads"))};
    for (const QString& path : legacyPaths) {
        if (!removeEntry(QFileInfo(path))) {
            if (error) *error = QStringLiteral("legacy_storage_reset_failed");
            return false;
        }
    }
    if (!DeviceIdentityStore::removeLegacyInstallationIdentity(error)) return false;
    return true;
}

void appendCategory(QStringList* categories, const QString& value)
{
    if (!categories->contains(value)) categories->append(value);
}
}

RuntimeStorageBootstrap::RuntimeStorageBootstrap(RuntimeProfileContext context)
    : m_context(std::move(context))
{
}

QString RuntimeStorageBootstrap::stageLabel(Stage stage)
{
    switch (stage) {
    case Stage::PreparingRuntime: return QStringLiteral("Preparing runtime");
    case Stage::ValidatingManifest: return QStringLiteral("Validating storage manifest");
    case Stage::PurgingCache: return QStringLiteral("Purging received-media cache");
    case Stage::ValidatingSettings: return QStringLiteral("Validating settings");
    case Stage::ValidatingProjects: return QStringLiteral("Validating projects");
    case Stage::ValidatingIdentity: return QStringLiteral("Validating device identity");
    }
    return QStringLiteral("Preparing Mouffette");
}

RuntimeStorageBootstrap::Result RuntimeStorageBootstrap::run(
    const ProgressCallback& progress)
{
    Result result;
    const auto fail = [&result](const QString& code, const QString& cause) {
        result.status = Status::RecoverableFailure;
        result.code = code;
        result.cause = cause;
        return result;
    };
    const auto report = [&progress](Stage stage) {
        if (progress) progress(stage);
    };

    report(Stage::PreparingRuntime);
    if (m_context.rootPath.trimmed().isEmpty()
        || !ensurePrivateDirectory(m_context.rootPath)) {
        return fail(QStringLiteral("runtime_root_unavailable"),
                    QStringLiteral("The runtime directory could not be prepared."));
    }
    RuntimeProfile::configure(m_context);

    report(Stage::ValidatingManifest);
    const ManifestState manifestState = inspectManifest(m_context, &result.foundVersion);
    const bool manifestMustBeWritten = manifestState != ManifestState::Valid;
    if (manifestState != ManifestState::Valid) {
        result.cause = manifestState == ManifestState::Missing
            ? QStringLiteral("The storage manifest was missing.")
            : manifestState == ManifestState::VersionMismatch
                ? QStringLiteral("The storage format version was incompatible.")
                : QStringLiteral("The storage manifest was invalid.");
        QString identityError;
        DeviceIdentityStore currentIdentity(RuntimeProfile::identityLocation(),
                                            m_context.isPersistent(),
                                            m_context.profileId);
        if (!currentIdentity.reset(&identityError)) {
            return fail(QStringLiteral("runtime_identity_reset_failed"),
                        QStringLiteral("The runtime identity could not be reset."));
        }
        if (!resetRuntimeContents(m_context)) {
            return fail(QStringLiteral("runtime_reset_failed"),
                        QStringLiteral("The incompatible runtime could not be reset."));
        }
        if (m_context.isPersistent() && manifestState == ManifestState::Missing) {
            QString legacyError;
            if (!removeLegacyStorage(&legacyError)) {
                return fail(legacyError, QStringLiteral(
                    "The previous storage layout could not be removed."));
            }
        }
        result.resetCategories = {
            QStringLiteral("settings"), QStringLiteral("projects"),
            QStringLiteral("cache"), QStringLiteral("identity")};
    }

    report(Stage::PurgingCache);
    const QString uploadsPath = QDir(RuntimeProfile::cacheLocation())
                                    .filePath(QStringLiteral("Uploads"));
    if (!removeEntry(QFileInfo(uploadsPath)) || !ensurePrivateDirectory(uploadsPath)) {
        return fail(QStringLiteral("cache_purge_failed"),
                    QStringLiteral("The received-media cache could not be purged."));
    }

    report(Stage::ValidatingSettings);
    if (!settingsAreValid()) {
        if (!resetSettingsToDefaults()) {
            return fail(QStringLiteral("settings_reset_failed"),
                        QStringLiteral("The settings store could not be reset."));
        }
        appendCategory(&result.resetCategories, QStringLiteral("settings"));
        if (result.cause.isEmpty()) {
            result.cause = QStringLiteral("The settings store was invalid.");
        }
    }

    report(Stage::ValidatingProjects);
    if (!projectsAreValid()) {
        if (!resetProjectsToEmpty()) {
            return fail(QStringLiteral("projects_reset_failed"),
                        QStringLiteral("The project store could not be reset."));
        }
        appendCategory(&result.resetCategories, QStringLiteral("projects"));
        if (result.cause.isEmpty()) {
            result.cause = QStringLiteral("The project store was invalid.");
        }
    }

    report(Stage::ValidatingIdentity);
    if (!ensurePrivateDirectory(RuntimeProfile::identityLocation())) {
        const QFileInfo identityInfo(RuntimeProfile::identityLocation());
        if (!removeEntry(identityInfo)
            || !ensurePrivateDirectory(RuntimeProfile::identityLocation())) {
            return fail(QStringLiteral("identity_directory_failed"),
                        QStringLiteral("The identity store could not be prepared."));
        }
        appendCategory(&result.resetCategories, QStringLiteral("identity"));
        if (result.cause.isEmpty()) {
            result.cause = QStringLiteral("The runtime identity was invalid.");
        }
    }
    DeviceIdentityStore identity(RuntimeProfile::identityLocation(),
                                 m_context.isPersistent(), m_context.profileId);
    bool identityReset = false;
    QString identityError;
    if (!identity.validateOrReset(&identityReset, &identityError)) {
        return fail(QStringLiteral("identity_initialization_failed"),
                    QStringLiteral("The runtime identity could not be validated."));
    }
    if (identityReset) {
        appendCategory(&result.resetCategories, QStringLiteral("identity"));
        if (result.cause.isEmpty()) {
            result.cause = QStringLiteral("The runtime identity was invalid.");
        }
    }

    // Commit the manifest only after every component has reached a valid
    // state. A failed first attempt therefore retries the complete reset
    // instead of mistaking a partially-created runtime for a healthy one.
    if (manifestMustBeWritten && !writeManifest(m_context)) {
        return fail(QStringLiteral("manifest_write_failed"),
                    QStringLiteral("The storage manifest could not be created."));
    }

    result.code.clear();
    result.status = result.resetCategories.isEmpty()
        ? Status::Success : Status::SuccessWithReset;
    return result;
}
