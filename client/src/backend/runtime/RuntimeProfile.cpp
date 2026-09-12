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
    if (g_context.isTemporary()) {
        const QString path = QDir(g_context.temporaryRoot).filePath(QStringLiteral("data"));
        QDir().mkpath(path);
        return path;
    }
    const QString platform = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    return platform.isEmpty() ? persistentFallback(QStringLiteral("data")) : platform;
}

QString RuntimeProfile::cacheLocation()
{
    if (g_context.isTemporary()) {
        const QString path = QDir(g_context.temporaryRoot).filePath(QStringLiteral("cache"));
        QDir().mkpath(path);
        return path;
    }
    const QString platform = QStandardPaths::writableLocation(QStandardPaths::CacheLocation);
    return platform.isEmpty() ? persistentFallback(QStringLiteral("cache")) : platform;
}

QString RuntimeProfile::installationDataLocation()
{
    const QString platform =
        QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
    const QString path = platform.isEmpty()
        ? persistentFallback(QStringLiteral("installation")) : platform;
    QDir().mkpath(path);
    return path;
}

std::unique_ptr<QSettings> RuntimeProfile::createSettings()
{
    if (g_context.isTemporary()) {
        const QString path = QDir(g_context.temporaryRoot).filePath(QStringLiteral("settings.ini"));
        return std::make_unique<QSettings>(path, QSettings::IniFormat);
    }
    return std::make_unique<QSettings>(QStringLiteral("Mouffette"),
                                       QStringLiteral(MOUFFETTE_SETTINGS_APPLICATION));
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
