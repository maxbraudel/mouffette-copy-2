#include "backend/managers/app/SettingsManager.h"
#include "backend/config/AppConfig.h"
#include "backend/runtime/RuntimeProfile.h"
#include "backend/runtime/storage/StorageVersions.h"
#include <QSettings>
#include <QDebug>
#include <algorithm>

SettingsManager::SettingsManager(QObject* parent)
    : QObject(parent)
    , m_serverUrlConfig(AppConfig::instance().serverUrl())
    , m_autoUploadImportedMedia(AppConfig::instance().autoUploadImportedMedia())
{
}

void SettingsManager::loadSettings() {
    const AppConfig& config = AppConfig::instance();
    m_serverUrlConfig = config.serverUrl();
    m_autoUploadImportedMedia = config.autoUploadImportedMedia();
    
    qDebug() << "SettingsManager: Settings loaded - URL:" << m_serverUrlConfig
             << "Auto-upload:" << m_autoUploadImportedMedia;
}

void SettingsManager::saveSettings() {
    const std::unique_ptr<QSettings> settings = RuntimeProfile::createSettings();
    settings->setValue("serverUrl", m_serverUrlConfig.isEmpty()
                                      ? AppConfig::instance().serverUrl()
                                      : m_serverUrlConfig);
    settings->setValue("autoUploadImportedMedia", m_autoUploadImportedMedia);
    settings->setValue("storage/schemaVersion", StorageVersions::Settings);
    settings->sync();
    
    qDebug() << "SettingsManager: Settings saved";
    emit settingsChanged();
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
