#include "backend/managers/app/SettingsManager.h"
#include "backend/config/AppConfig.h"
#include "backend/runtime/RuntimeProfile.h"
#include "backend/runtime/storage/StorageVersions.h"
#include "backend/runtime/storage/StorageIO.h"
#include "backend/domain/profile/ProfileImage.h"
#include <QSettings>
#include <QDebug>
#include <algorithm>

SettingsManager::SettingsManager(QObject* parent)
    : QObject(parent)
    , m_serverUrlConfig(AppConfig::instance().serverUrl())
    , m_autoUploadImportedMedia(AppConfig::instance().autoUploadImportedMedia())
    , m_appAlwaysOnTop(AppConfig::instance().appAlwaysOnTop())
{
}

void SettingsManager::loadSettings() {
    const AppConfig& config = AppConfig::instance();
    m_serverUrlConfig = config.serverUrl();
    m_autoUploadImportedMedia = config.autoUploadImportedMedia();
    m_appAlwaysOnTop = config.appAlwaysOnTop();
    const auto settings = RuntimeProfile::createSettings();
    if (!ProfileImage::normalizeUsername(settings->value(QStringLiteral("username")).toString(), &m_username))
        m_username.clear();
    const QByteArray encoded = settings->value(QStringLiteral("profilePictureJpeg")).toByteArray();
    m_profilePictureJpeg = QByteArray::fromBase64(encoded, QByteArray::AbortOnBase64DecodingErrors);
    if (!ProfileImage::decodeJpeg(m_profilePictureJpeg)) m_profilePictureJpeg.clear();
    
    qDebug() << "SettingsManager: Settings loaded - URL:" << m_serverUrlConfig
             << "Auto-upload:" << m_autoUploadImportedMedia;
}

void SettingsManager::saveSettings() {
    QString error;
    if (!commitSettings(m_serverUrlConfig, m_autoUploadImportedMedia, m_appAlwaysOnTop,
                        m_username, m_profilePictureJpeg, &error))
        qWarning().noquote() << "Cannot save settings:" << error;
}

bool SettingsManager::commitSettings(const QString& serverUrl, bool autoUpload, bool alwaysOnTop,
                                     const QString& username, const QByteArray& jpeg, QString* error)
{
    QUrl normalizedUrl;
    QString normalizedUsername;
    if (!AppConfig::validateServerUrl(serverUrl, &normalizedUrl, error)
        || !ProfileImage::normalizeUsername(username, &normalizedUsername, error)) return false;
    if (!jpeg.isEmpty() && !ProfileImage::decodeJpeg(jpeg)) {
        if (error) *error = QStringLiteral("The profile picture must be a valid 250 × 250 JPEG.");
        return false;
    }
    const auto previous = RuntimeStorage::readSettings(RuntimeProfile::profileRoot(), RuntimeProfile::settingsFilePath());
    if (previous.inspection.state != RuntimeStorage::State::Current
        && previous.inspection.state != RuntimeStorage::State::Missing) {
        if (error) *error = previous.inspection.reason;
        return false;
    }
    QVariantMap values = previous.values; // Preserve forward-compatible optional settings.
    const QString canonical = normalizedUrl.toString(QUrl::FullyEncoded);
    values.insert(QStringLiteral("serverUrl"), canonical);
    values.insert(QStringLiteral("autoUploadImportedMedia"), autoUpload);
    values.insert(QStringLiteral("appAlwaysOnTop"), alwaysOnTop);
    values.insert(QStringLiteral("username"), normalizedUsername);
    values.insert(QStringLiteral("profilePictureJpeg"), QString::fromLatin1(jpeg.toBase64()));
    const auto committed = RuntimeStorage::writeSettings(RuntimeProfile::profileRoot(),
        RuntimeProfile::settingsFilePath(), values, StorageVersions::Settings);
    if (!committed.succeeded()) {
        if (error) *error = committed.reason;
        return false;
    }
    const bool urlChanged = canonical != m_serverUrlConfig;
    m_serverUrlConfig = canonical;
    m_autoUploadImportedMedia = autoUpload;
    m_appAlwaysOnTop = alwaysOnTop;
    m_username = normalizedUsername;
    m_profilePictureJpeg = jpeg;
    if (urlChanged) emit serverUrlChanged(canonical);
    emit settingsChanged();
    return true;
}

void SettingsManager::setServerUrl(const QString& url) {
    QUrl normalized;
    QString error;
    if (!AppConfig::validateServerUrl(url, &normalized, &error)) {
        qWarning().noquote() << error;
        return;
    }
    const QString canonical = normalized.toString(QUrl::FullyEncoded);
    if (m_serverUrlConfig != canonical) {
        m_serverUrlConfig = canonical;
        const std::unique_ptr<QSettings> settings = RuntimeProfile::createSettings();
        settings->setValue("serverUrl", m_serverUrlConfig);
        settings->setValue("storage/schemaVersion", StorageVersions::Settings);
        settings->sync();
        emit serverUrlChanged(canonical);
    }
}

void SettingsManager::setAutoUploadImportedMedia(bool enabled) {
    if (m_autoUploadImportedMedia != enabled) {
        m_autoUploadImportedMedia = enabled;
        const std::unique_ptr<QSettings> settings = RuntimeProfile::createSettings();
        settings->setValue("autoUploadImportedMedia", m_autoUploadImportedMedia);
        settings->setValue("storage/schemaVersion", StorageVersions::Settings);
        settings->sync();
    }
}

void SettingsManager::setAppAlwaysOnTop(bool enabled) {
    m_appAlwaysOnTop = enabled;
}
