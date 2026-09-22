#ifndef SETTINGSMANAGER_H
#define SETTINGSMANAGER_H

#include <QObject>
#include <QString>
#include <QByteArray>

/** Non-visual application settings and persistence service. */
class SettingsManager : public QObject {
    Q_OBJECT

public:
    explicit SettingsManager(QObject* parent = nullptr);
    ~SettingsManager() = default;

    // Settings persistence
    void loadSettings();
    void saveSettings();
    
    // Getters
    QString getServerUrl() const { return m_serverUrlConfig; }
    bool getAutoUploadImportedMedia() const { return m_autoUploadImportedMedia; }
    bool getAppAlwaysOnTop() const { return m_appAlwaysOnTop; }
    bool getScreenSharingEnabled() const { return m_screenSharingEnabled; }
    bool getScreenContentVisible() const { return m_screenContentVisible; }
    bool getAudioSharingEnabled() const { return m_audioSharingEnabled; }
    bool getSystemAudioEnabled() const { return m_systemAudioEnabled; }
    QString username() const { return m_username; }
    QByteArray profilePictureJpeg() const { return m_profilePictureJpeg; }
    bool commitSettings(const QString& serverUrl, bool autoUpload, bool alwaysOnTop,
                        const QString& username, const QByteArray& profilePictureJpeg,
                        QString* error = nullptr);
    bool commitSettings(const QString& serverUrl, bool autoUpload, bool alwaysOnTop,
                        const QString& username, const QByteArray& profilePictureJpeg,
                        bool screenSharingEnabled, QString* error = nullptr);
    bool commitSettings(const QString& serverUrl, bool autoUpload, bool alwaysOnTop,
                        const QString& username, const QByteArray& profilePictureJpeg,
                        bool screenSharingEnabled, bool audioSharingEnabled, QString* error = nullptr);
    
    // Setters
    void setServerUrl(const QString& url);
    void setAutoUploadImportedMedia(bool enabled);
    void setAppAlwaysOnTop(bool enabled);
    bool setScreenContentVisible(bool visible, QString* error = nullptr);
    bool setSystemAudioEnabled(bool enabled, QString* error = nullptr);

signals:
    void settingsChanged();
    void serverUrlChanged(const QString& newUrl);
    void screenSharingEnabledChanged(bool enabled);
    void screenContentVisibleChanged(bool visible);
    void audioSharingEnabledChanged(bool enabled);
    void systemAudioEnabledChanged(bool enabled);

private:
    // Settings values
    QString m_serverUrlConfig;
    bool m_autoUploadImportedMedia;
    bool m_appAlwaysOnTop = true;
    // Sharing the desktop always requires this instance's explicit opt-in.
    bool m_screenSharingEnabled = false;
    // Viewer preference shared by all projects in this client profile.
    bool m_screenContentVisible = true;
    bool m_audioSharingEnabled = false;
    bool m_systemAudioEnabled = true;
    QString m_username;
    QByteArray m_profilePictureJpeg;
};

#endif // SETTINGSMANAGER_H
