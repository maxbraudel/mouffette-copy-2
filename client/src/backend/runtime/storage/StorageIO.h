#pragma once

#include "StorageUpgradeEngine.h"
#include <QByteArray>
#include <QJsonObject>
#include <QVariantMap>

namespace RuntimeStorage {
// Every path must be owned by the supplied root. Ancestor symlinks are refused;
// an owned leaf symlink can be removed, but is never read or traversed.
Operation checkPath(const QString& root, const QString& path);
Operation ensureDirectory(const QString& root, const QString& path);
Operation removeOwned(const QString& root, const QString& path);
Inspection readFile(const QString& root, const QString& path, qint64 limit, QByteArray* bytes);
Operation writeFile(const QString& root, const QString& path, const QByteArray& bytes);
Operation writeJson(const QString& root, const QString& path, const QJsonObject& object);
Inspection readVersionedJson(const QString& root, const QString& path, int expected,
                             qint64 limit, QJsonObject* object = nullptr);
int jsonVersion(const QJsonValue& value);

struct SettingsData {
    Inspection inspection;
    QVariantMap values;
};
SettingsData readSettings(const QString& root, const QString& path);
Operation writeSettings(const QString& root, const QString& path,
                        const QVariantMap& values, int version);
}
