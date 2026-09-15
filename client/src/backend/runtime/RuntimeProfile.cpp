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
    return QDir(base).filePath(QStringLiteral("runtimes/instance-1"));
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
    return identityLocation();
}

QString RuntimeProfile::profileRoot()
{
    if (!g_context.rootPath.isEmpty()) {
        return QDir::cleanPath(g_context.rootPath);
    }
    return QDir::cleanPath(defaultPersistentRoot());
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
    return QDir(profileRoot()).filePath(QStringLiteral("identity"));
}

std::unique_ptr<QSettings> RuntimeProfile::createSettings()
{
    return std::make_unique<QSettings>(settingsFilePath(), QSettings::IniFormat);
}

QVariantMap RuntimeProfile::readSettings()
{
    const std::unique_ptr<QSettings> settings = createSettings();
    QVariantMap values;
    for (const QString& key : settings->allKeys()) {
        values.insert(key, settings->value(key));
    }
    return values;
}
