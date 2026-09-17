#include "StorageIO.h"
#include "StorageVersions.h"
#include "backend/config/AppConfig.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QSaveFile>
#include <QSettings>
#include <QTemporaryDir>
#include <cmath>
#include <limits>

namespace RuntimeStorage {
namespace {
Operation ioError(const QString& path, const QString& detail)
{
    return {Failure::IoError, QStringLiteral("%1: %2").arg(path, detail)};
}

bool removeEntry(const QString& path)
{
    const QFileInfo info(path);
    if (info.isSymLink() || info.isFile()) return QFile::remove(path);
    if (!info.exists()) return true;
    if (!info.isDir()) return false;
    for (const QFileInfo& entry : QDir(path).entryInfoList(
             QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System)) {
        if (!removeEntry(entry.absoluteFilePath())) return false;
    }
    return QDir().rmdir(path);
}
}

Operation checkPath(const QString& root, const QString& path)
{
    const QString base = QDir::cleanPath(QFileInfo(root).absoluteFilePath());
    const QString target = QDir::cleanPath(QFileInfo(path).absoluteFilePath());
    if (!target.startsWith(base + QLatin1Char('/')))
        return ioError(path, QStringLiteral("Path is outside its runtime root"));
    const QFileInfo rootInfo(base);
    if (rootInfo.isSymLink() || !rootInfo.isDir())
        return ioError(root, QStringLiteral("Runtime root is not a private directory"));
    QString parent = QFileInfo(target).absolutePath();
    while (parent != base) {
        const QFileInfo info(parent);
        if (info.isSymLink() || (info.exists() && !info.isDir()))
            return ioError(parent, QStringLiteral("Unsafe storage ancestor"));
        parent = info.absolutePath();
    }
    return {};
}

Operation ensureDirectory(const QString& root, const QString& path)
{
    const Operation safe = checkPath(root, path);
    if (!safe.succeeded()) return safe;
    const QFileInfo info(path);
    if (info.isSymLink() || (info.exists() && !info.isDir()))
        return ioError(path, QStringLiteral("Expected a private directory"));
    if (!QDir().mkpath(path)) return ioError(path, QStringLiteral("Cannot create directory"));
    return {};
}

Operation removeOwned(const QString& root, const QString& path)
{
    const Operation safe = checkPath(root, path);
    if (!safe.succeeded()) return safe;
    if (!removeEntry(path)) return ioError(path, QStringLiteral("Cannot remove storage entry"));
    return {};
}

Inspection readFile(const QString& root, const QString& path, qint64 limit, QByteArray* bytes)
{
    const Operation safe = checkPath(root, path);
    if (!safe.succeeded()) return {State::IoError, -1, safe.reason};
    const QFileInfo info(path);
    if (info.isSymLink()) return {State::Corrupt, -1, QStringLiteral("Storage is a symbolic link")};
    if (!info.exists()) return {State::Missing, -1, QStringLiteral("Storage is absent")};
    if (!info.isFile()) return {State::Corrupt, -1, QStringLiteral("Storage is not a regular file")};
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return {State::IoError, -1, file.errorString()};
    if (file.size() > limit) return {State::Corrupt, -1, QStringLiteral("Storage exceeds its size limit")};
    *bytes = file.read(limit + 1);
    if (file.error() != QFileDevice::NoError) return {State::IoError, -1, file.errorString()};
    if (bytes->size() > limit) return {State::Corrupt, -1, QStringLiteral("Storage exceeds its size limit")};
    return {State::Current, -1, {}};
}

Operation writeFile(const QString& root, const QString& path, const QByteArray& bytes)
{
    const Operation safe = checkPath(root, path);
    if (!safe.succeeded()) return safe;
    const QString parent = QFileInfo(path).absolutePath();
    if (QDir::cleanPath(parent) != QDir::cleanPath(root)) {
        const Operation prepared = ensureDirectory(root, parent);
        if (!prepared.succeeded()) return prepared;
    }
    const QFileInfo info(path);
    if (info.isSymLink() || (info.exists() && !info.isFile())) {
        const Operation removed = removeOwned(root, path);
        if (!removed.succeeded()) return removed;
    }
    QSaveFile file(path);
    file.setDirectWriteFallback(false);
    if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size() || !file.commit())
        return ioError(path, file.errorString());
    return {};
}

Operation writeJson(const QString& root, const QString& path, const QJsonObject& object)
{
    return writeFile(root, path, QJsonDocument(object).toJson(QJsonDocument::Compact));
}

int jsonVersion(const QJsonValue& value)
{
    if (!value.isDouble()) return -1;
    const double number = value.toDouble(-1);
    return std::isfinite(number) && number >= 0
            && number <= std::numeric_limits<int>::max() && std::floor(number) == number
        ? static_cast<int>(number) : -1;
}

Inspection readVersionedJson(const QString& root, const QString& path, int expected,
                             qint64 limit, QJsonObject* object)
{
    QByteArray bytes;
    Inspection result = readFile(root, path, limit, &bytes);
    if (result.state != State::Current) return result;
    QJsonParseError error;
    const QJsonDocument document = QJsonDocument::fromJson(bytes, &error);
    if (error.error != QJsonParseError::NoError || !document.isObject())
        return {State::Corrupt, -1, QStringLiteral("Invalid storage JSON: %1").arg(error.errorString())};
    if (object) *object = document.object();
    const int version = jsonVersion(document.object().value(QStringLiteral("schemaVersion")));
    if (version < 0) return {State::Corrupt, -1, QStringLiteral("Invalid schema version")};
    return {version == expected ? State::Current : State::Incompatible, version,
            version == expected ? QString() : QStringLiteral("Storage version differs")};
}

SettingsData readSettings(const QString& root, const QString& path)
{
    SettingsData result;
    QByteArray bytes;
    result.inspection = readFile(root, path, 1024 * 1024, &bytes);
    if (result.inspection.state != State::Current) return result;
    // Parse a snapshot: QSettings caches values by filename across instances.
    // A fresh temporary name ensures validation observes disk, including after
    // an atomic migration, rather than an in-process QVariant cache.
    QTemporaryDir snapshotDir;
    if (!snapshotDir.isValid()) {
        result.inspection = {State::IoError, -1, snapshotDir.errorString()};
        return result;
    }
    const QString snapshotPath = snapshotDir.filePath(QStringLiteral("settings.ini"));
    QFile snapshot(snapshotPath);
    if (!snapshot.open(QIODevice::WriteOnly) || snapshot.write(bytes) != bytes.size()
        || !snapshot.flush()) {
        result.inspection = {State::IoError, -1, snapshot.errorString()};
        return result;
    }
    snapshot.close();
    QSettings settings(snapshotPath, QSettings::IniFormat);
    settings.setFallbacksEnabled(false);
    const QStringList keys = settings.allKeys();
    if (settings.status() != QSettings::NoError) {
        result.inspection = {settings.status() == QSettings::AccessError ? State::IoError : State::Corrupt,
                             -1, QStringLiteral("Cannot parse settings INI")};
        return result;
    }
    int version = 0;
    if (settings.contains(QStringLiteral("storage/schemaVersion"))) {
        const QString text = settings.value(QStringLiteral("storage/schemaVersion")).toString();
        bool ok = false;
        version = text.toInt(&ok);
        if (!ok || version < 0 || QString::number(version) != text) {
            result.inspection = {State::Corrupt, -1, QStringLiteral("Invalid settings schema version")};
            return result;
        }
    }
    if (version != 0 && version != StorageVersions::Settings) {
        result.inspection = {State::Incompatible, version, QStringLiteral("Settings version differs")};
        return result;
    }
    QString error;
    const QString url = settings.value(QStringLiteral("serverUrl")).toString();
    const QString boolean = settings.value(QStringLiteral("autoUploadImportedMedia"))
                                .toString().trimmed().toLower();
    if (!settings.contains(QStringLiteral("serverUrl"))
        || !settings.contains(QStringLiteral("autoUploadImportedMedia"))
        || !AppConfig::validateServerUrl(url, nullptr, &error)
        || (boolean != QLatin1String("true") && boolean != QLatin1String("false")
            && boolean != QLatin1String("1") && boolean != QLatin1String("0"))) {
        result.inspection = {State::Corrupt, version, QStringLiteral("Invalid settings values: %1").arg(error)};
        return result;
    }
    const QString onTop = settings.value(QStringLiteral("appAlwaysOnTop"), true)
                              .toString().trimmed().toLower();
    if (onTop != QLatin1String("true") && onTop != QLatin1String("false")
        && onTop != QLatin1String("1") && onTop != QLatin1String("0")) {
        result.inspection = {State::Corrupt, version, QStringLiteral("Invalid appAlwaysOnTop value")};
        return result;
    }
    // Unknown optional keys are forward-compatible; never erase the whole
    // settings store simply because a newer writer added a key.
    for (const QString& key : keys) {
        if (!key.startsWith(QLatin1String("storage/")))
            result.values.insert(key, settings.value(key));
    }
    result.values.insert(QStringLiteral("appAlwaysOnTop"),
                         onTop == QLatin1String("true") || onTop == QLatin1String("1"));
    result.values.insert(QStringLiteral("serverUrl"), url);
    result.values.insert(QStringLiteral("autoUploadImportedMedia"),
                         boolean == QLatin1String("true") || boolean == QLatin1String("1"));
    result.inspection = {version == StorageVersions::Settings ? State::Current : State::Incompatible,
                         version, version == 0 ? QStringLiteral("Unversioned settings") : QString()};
    return result;
}

Operation writeSettings(const QString& root, const QString& path,
                        const QVariantMap& values, int version)
{
    QTemporaryDir snapshotDir;
    if (!snapshotDir.isValid()) return {Failure::IoError, snapshotDir.errorString()};
    const QString temporaryPath = snapshotDir.filePath(QStringLiteral("settings.ini"));
    {
        QSettings settings(temporaryPath, QSettings::IniFormat);
        settings.setAtomicSyncRequired(true);
        for (auto it = values.cbegin(); it != values.cend(); ++it)
            if (!it.key().startsWith(QLatin1String("storage/"))) settings.setValue(it.key(), it.value());
        settings.setValue(QStringLiteral("storage/schemaVersion"), version);
        settings.sync();
        if (settings.status() != QSettings::NoError)
            return {Failure::IoError, QStringLiteral("Cannot serialize settings")};
    }
    QFile encoded(temporaryPath);
    if (!encoded.open(QIODevice::ReadOnly)) return {Failure::IoError, encoded.errorString()};
    const QByteArray bytes = encoded.readAll();
    if (encoded.error() != QFileDevice::NoError) return {Failure::IoError, encoded.errorString()};
    return writeFile(root, path, bytes);
}
}
