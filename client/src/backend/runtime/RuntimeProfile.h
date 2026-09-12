#ifndef RUNTIMEPROFILE_H
#define RUNTIMEPROFILE_H

#include <QString>
#include <QVariantMap>

#include <memory>

class QSettings;

struct RuntimeProfileContext {
    int ordinal = 1;
    QString instanceId = QStringLiteral("primary");
    QString profileId = QStringLiteral("instance-1");
    QString rootPath;
    bool persistent = true;
    // Compatibility alias for callers/tests that still inspect the temporary
    // allocation directly. New code must use rootPath/RuntimeProfile::profileRoot().
    QString temporaryRoot;

    bool isSecondary() const { return ordinal > 1; }
    bool isTemporary() const { return !persistent; }
    bool isPersistent() const { return persistent; }
};

/**
 * Central path/settings boundary for one application process.
 *
 * Every writable store is derived from one versioned runtime root. The
 * primary root is persistent; secondary development roots are disposable.
 */
class RuntimeProfile final {
public:
    static void configure(const RuntimeProfileContext& context);
    static RuntimeProfileContext context();

    static QString profileRoot();
    static QString appDataLocation();
    static QString cacheLocation();
    static QString installationDataLocation();
    static QString settingsFilePath();
    static QString projectsFilePath();
    static QString identityLocation();
    static std::unique_ptr<QSettings> createSettings();
    static QVariantMap readSettings();

private:
    RuntimeProfile() = delete;
};

#endif // RUNTIMEPROFILE_H
