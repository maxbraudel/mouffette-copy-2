#ifndef RUNTIMEPROFILE_H
#define RUNTIMEPROFILE_H

#include <QString>
#include <QVariantMap>

#include <memory>
#include "AppBuildConfig.h"

class QSettings;

struct RuntimeProfileContext {
    int ordinal = 1;
    QString instanceId = QStringLiteral("primary");
    QString profileId = QStringLiteral("instance-1");
    QString rootPath;
    QString channel = QStringLiteral(MOUFFETTE_BUILD_CHANNEL);
    bool persistent = true;

    QString identityNamespace() const { return channel + QLatin1Char(':') + profileId; }

    bool isSecondary() const { return ordinal > 1; }
    bool isTemporary() const { return !persistent; }
    bool isPersistent() const { return persistent; }
};

/**
 * Central path/settings boundary for one application process.
 *
 * Every writable store is derived from one channel-specific runtime root. The
 * primary root is persistent; secondary development roots are disposable.
 */
class RuntimeProfile final {
public:
    static void configure(const RuntimeProfileContext& context);
    static RuntimeProfileContext context();

    static QString profileRoot();
    static QString persistentRoot(const QString& base, const QString& channel);
    static QString appDataLocation();
    static QString cacheLocation();
    static QString installationDataLocation();
    static QString settingsFilePath();
    static QString projectsFilePath();
    static QString historyFilePath();
    static QString identityLocation();
    static std::unique_ptr<QSettings> createSettings();
    static QVariantMap readSettings();

private:
    RuntimeProfile() = delete;
};

#endif // RUNTIMEPROFILE_H
