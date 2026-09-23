#include "backend/managers/app/SettingsManager.h"
#include "backend/config/AppConfig.h"
#include "backend/runtime/RuntimeProfile.h"
#include "backend/runtime/storage/StorageVersions.h"
#include "backend/runtime/storage/StorageIO.h"
#include "backend/domain/profile/ProfileImage.h"
#include <QSettings>
#include <QDebug>
#include <algorithm>
#include <utility>

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
    // QSettings serializes booleans as "true"/"false". An unrelated nonempty
    // string must not accidentally become desktop-sharing consent.
    m_screenSharingEnabled = settings->value(QStringLiteral("screenSharingEnabled"), false)
                                 .toString() == QLatin1String("true");
    m_screenContentVisible = settings->value(QStringLiteral("screenContentVisible"), true).toBool();
    // The former consent checkbox explicitly covered both screen and audio.
    // Preserve that choice only when this profile predates separate consent.
    m_audioSharingEnabled = settings->value(QStringLiteral("audioSharingEnabled"), m_screenSharingEnabled)
                                .toString() == QLatin1String("true");
    m_systemAudioEnabled = settings->value(QStringLiteral("systemAudioEnabled"),
        !settings->value(QStringLiteral("remoteAudioMuted"), false).toBool()).toBool();
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
    return commitSettings(serverUrl, autoUpload, alwaysOnTop, username, jpeg,
                          m_screenSharingEnabled, m_audioSharingEnabled, error);
}

bool SettingsManager::commitSettings(const QString& serverUrl, bool autoUpload, bool alwaysOnTop,
                                     const QString& username, const QByteArray& jpeg,
                                     bool screenSharingEnabled, QString* error)
{
    return commitSettings(serverUrl, autoUpload, alwaysOnTop, username, jpeg,
                          screenSharingEnabled, m_audioSharingEnabled, error);
}

bool SettingsManager::commitSettings(const QString& serverUrl, bool autoUpload, bool alwaysOnTop,
                                     const QString& username, const QByteArray& jpeg,
                                     bool screenSharingEnabled, bool audioSharingEnabled, QString* error)
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
    values.insert(QStringLiteral("screenSharingEnabled"), screenSharingEnabled);
    values.insert(QStringLiteral("audioSharingEnabled"), audioSharingEnabled);
    values.insert(QStringLiteral("systemAudioEnabled"), m_systemAudioEnabled);
    values.remove(QStringLiteral("remoteAudioMuted"));
    values.insert(QStringLiteral("username"), normalizedUsername);
    values.insert(QStringLiteral("profilePictureJpeg"), QString::fromLatin1(jpeg.toBase64()));
    const auto committed = RuntimeStorage::writeSettings(RuntimeProfile::profileRoot(),
        RuntimeProfile::settingsFilePath(), values, StorageVersions::Settings);
    if (!committed.succeeded()) {
        if (error) *error = committed.reason;
        return false;
    }
    const bool urlChanged = canonical != m_serverUrlConfig;
    const bool sharingChanged = screenSharingEnabled != m_screenSharingEnabled;
    const bool audioChanged = audioSharingEnabled != m_audioSharingEnabled;
    m_serverUrlConfig = canonical;
    m_autoUploadImportedMedia = autoUpload;
    m_appAlwaysOnTop = alwaysOnTop;
    m_screenSharingEnabled = screenSharingEnabled;
    m_audioSharingEnabled = audioSharingEnabled;
    m_username = normalizedUsername;
    m_profilePictureJpeg = jpeg;
    if (urlChanged) emit serverUrlChanged(canonical);
    if (sharingChanged) emit screenSharingEnabledChanged(screenSharingEnabled);
    if (audioChanged) emit audioSharingEnabledChanged(audioSharingEnabled);
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

bool SettingsManager::setScreenContentVisible(bool visible, QString* error)
{
    if (m_screenContentVisible == visible) return true;
    if (visible && m_screenContentEnableGuard && !m_screenContentEnableGuard(error)) return false;
    const auto previous = RuntimeStorage::readSettings(RuntimeProfile::profileRoot(),
                                                       RuntimeProfile::settingsFilePath());
    if (previous.inspection.state != RuntimeStorage::State::Current
        && previous.inspection.state != RuntimeStorage::State::Missing) {
        if (error) *error = previous.inspection.reason;
        return false;
    }
    QVariantMap values = previous.values;
    if (previous.inspection.state == RuntimeStorage::State::Missing) {
        values.insert(QStringLiteral("serverUrl"), m_serverUrlConfig);
        values.insert(QStringLiteral("autoUploadImportedMedia"), m_autoUploadImportedMedia);
        values.insert(QStringLiteral("appAlwaysOnTop"), m_appAlwaysOnTop);
        values.insert(QStringLiteral("screenSharingEnabled"), m_screenSharingEnabled);
        values.insert(QStringLiteral("audioSharingEnabled"), m_audioSharingEnabled);
        values.insert(QStringLiteral("systemAudioEnabled"), m_systemAudioEnabled);
        values.insert(QStringLiteral("username"), m_username);
        values.insert(QStringLiteral("profilePictureJpeg"), QString::fromLatin1(m_profilePictureJpeg.toBase64()));
    }
    values.insert(QStringLiteral("screenContentVisible"), visible);
    const auto committed = RuntimeStorage::writeSettings(RuntimeProfile::profileRoot(),
        RuntimeProfile::settingsFilePath(), values, StorageVersions::Settings);
    if (!committed.succeeded()) {
        if (error) *error = committed.reason;
        return false;
    }
    m_screenContentVisible = visible;
    emit screenContentVisibleChanged(visible);
    emit settingsChanged();
    return true;
}

void SettingsManager::setScreenContentEnableGuard(std::function<bool(QString*)> guard)
{
    m_screenContentEnableGuard = std::move(guard);
}

bool SettingsManager::setSystemAudioEnabled(bool enabled, QString* error)
{
    if (m_systemAudioEnabled == enabled) return true;
    const auto previous = RuntimeStorage::readSettings(RuntimeProfile::profileRoot(),
                                                       RuntimeProfile::settingsFilePath());
    if (previous.inspection.state != RuntimeStorage::State::Current
        && previous.inspection.state != RuntimeStorage::State::Missing) {
        if (error) *error = previous.inspection.reason;
        return false;
    }
    QVariantMap values = previous.values;
    if (previous.inspection.state == RuntimeStorage::State::Missing) {
        values.insert(QStringLiteral("serverUrl"), m_serverUrlConfig);
        values.insert(QStringLiteral("autoUploadImportedMedia"), m_autoUploadImportedMedia);
        values.insert(QStringLiteral("appAlwaysOnTop"), m_appAlwaysOnTop);
        values.insert(QStringLiteral("screenSharingEnabled"), m_screenSharingEnabled);
        values.insert(QStringLiteral("audioSharingEnabled"), m_audioSharingEnabled);
        values.insert(QStringLiteral("systemAudioEnabled"), m_systemAudioEnabled);
        values.insert(QStringLiteral("screenContentVisible"), m_screenContentVisible);
        values.insert(QStringLiteral("username"), m_username);
        values.insert(QStringLiteral("profilePictureJpeg"), QString::fromLatin1(m_profilePictureJpeg.toBase64()));
    }
    values.insert(QStringLiteral("systemAudioEnabled"), enabled);
    values.remove(QStringLiteral("remoteAudioMuted"));
    const auto committed = RuntimeStorage::writeSettings(RuntimeProfile::profileRoot(),
        RuntimeProfile::settingsFilePath(), values, StorageVersions::Settings);
    if (!committed.succeeded()) {
        if (error) *error = committed.reason;
        return false;
    }
    m_systemAudioEnabled = enabled;
    emit systemAudioEnabledChanged(enabled);
    emit settingsChanged();
    return true;
}
