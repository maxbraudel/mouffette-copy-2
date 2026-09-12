#ifndef RUNTIMEPROFILE_H
#define RUNTIMEPROFILE_H

#include <QString>
#include <QVariantMap>

#include <memory>

class QSettings;

struct RuntimeProfileContext {
    int ordinal = 1;
    QString instanceId = QStringLiteral("primary");
    QString temporaryRoot;

    bool isSecondary() const { return ordinal > 1; }
    bool isTemporary() const { return !temporaryRoot.isEmpty(); }
};

/**
 * Central path/settings boundary for one application process.
 *
 * The primary profile preserves the platform-native locations used by
 * previous Mouffette versions. Secondary development profiles redirect every
 * writable store beneath one disposable root owned by ApplicationInstanceManager.
 */
class RuntimeProfile final {
public:
    static void configure(const RuntimeProfileContext& context);
    static RuntimeProfileContext context();

    static QString appDataLocation();
    static QString cacheLocation();
    // Stable installation-scoped storage shared deliberately by every
    // instance. Only long-lived installation identity material belongs here.
    static QString installationDataLocation();
    static std::unique_ptr<QSettings> createSettings();
    static QVariantMap readSettings();

private:
    RuntimeProfile() = delete;
};

#endif // RUNTIMEPROFILE_H
