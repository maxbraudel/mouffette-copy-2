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
    QString username() const { return m_username; }
    QByteArray profilePictureJpeg() const { return m_profilePictureJpeg; }
    bool commitSettings(const QString& serverUrl, bool autoUpload, bool alwaysOnTop,
                        const QString& username, const QByteArray& profilePictureJpeg,
                        QString* error = nullptr);
    
    // Setters
    void setServerUrl(const QString& url);
    void setAutoUploadImportedMedia(bool enabled);
    void setAppAlwaysOnTop(bool enabled);

signals:
    void settingsChanged();
    void serverUrlChanged(const QString& newUrl);

private:
    // Settings values
    QString m_serverUrlConfig;
    bool m_autoUploadImportedMedia;
    bool m_appAlwaysOnTop = true;
    QString m_username;
    QByteArray m_profilePictureJpeg;
};

#endif // SETTINGSMANAGER_H
