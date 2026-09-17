#include "backend/runtime/RuntimeProfile.h"
#include "AppBuildConfig.h"

#include <QDir>
#include <QSettings>
#include <QStandardPaths>

namespace {
RuntimeProfileContext g_context;

QString persistentFallback(const QString& leaf)
{
    return QDir(QDir::homePath()).filePath(QStringLiteral(".mouffette/%1").arg(leaf));
}

QString defaultPersistentRoot()
{
    const QString platform =
        QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    const QString base = platform.isEmpty()
        ? persistentFallback(QStringLiteral("data")) : platform;
    return RuntimeProfile::persistentRoot(base, QStringLiteral(MOUFFETTE_BUILD_CHANNEL));
}
}

void RuntimeProfile::configure(const RuntimeProfileContext& context)
{
    g_context = context;
}

RuntimeProfileContext RuntimeProfile::context()
{
    return g_context;
}

QString RuntimeProfile::appDataLocation()
{
    return profileRoot();
}

QString RuntimeProfile::cacheLocation()
{
    return QDir(profileRoot()).filePath(QStringLiteral("cache"));
}

QString RuntimeProfile::installationDataLocation()
{
    return resolvedInstallationRoot(g_context);
}

QString RuntimeProfile::profileRoot()
{
    if (!g_context.rootPath.isEmpty()) {
        return QDir::cleanPath(g_context.rootPath);
    }
    return QDir::cleanPath(defaultPersistentRoot());
}

QString RuntimeProfile::persistentRoot(const QString& base, const QString& channel)
{
    // Only compiled channels are accepted; callers cannot supply path segments.
    if (channel != QLatin1String("development") && channel != QLatin1String("production"))
        return {};
    return QDir(base).filePath(QStringLiteral("runtimes/%1/instance-1").arg(channel));
}

QString RuntimeProfile::persistentInstallationRoot(const QString& base, const QString& channel)
{
    if (channel != QLatin1String("development") && channel != QLatin1String("production"))
        return {};
    return QDir(base).filePath(QStringLiteral("installations/%1").arg(channel));
}

QString RuntimeProfile::resolvedInstallationRoot(const RuntimeProfileContext& context)
{
    if (!context.installationRootPath.isEmpty())
        return QDir::cleanPath(context.installationRootPath);
    // Embedded/test contexts retain all writes under their explicit root.
    // Real application profiles always provide a separate installation root.
    if (!context.rootPath.isEmpty())
        return QDir(context.rootPath).filePath(QStringLiteral("installation"));
    const QString platform = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    return persistentInstallationRoot(platform.isEmpty()
        ? persistentFallback(QStringLiteral("data")) : platform, context.channel);
}

QString RuntimeProfile::settingsFilePath()
{
    return QDir(profileRoot()).filePath(QStringLiteral("settings/settings.ini"));
}

QString RuntimeProfile::projectsFilePath()
{
    return QDir(profileRoot()).filePath(QStringLiteral("projects/projects-v2.json"));
}

QString RuntimeProfile::identityLocation()
{
    return installationDataLocation();
}

QString RuntimeProfile::historyFilePath()
{
    return QDir(profileRoot()).filePath(QStringLiteral("notification-history-v1.json"));
}

std::unique_ptr<QSettings> RuntimeProfile::createSettings()
{
    return std::make_unique<QSettings>(settingsFilePath(), QSettings::IniFormat);
}

QVariantMap RuntimeProfile::readSettings()
{
    const std::unique_ptr<QSettings> settings = createSettings();
    settings->sync();
    QVariantMap values;
    for (const QString& key : settings->allKeys()) {
        if (key.startsWith(QLatin1String("storage/"))) continue;
        values.insert(key, settings->value(key));
    }
    return values;
}
