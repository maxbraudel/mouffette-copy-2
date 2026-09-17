#ifndef SETTINGSMANAGER_H
#define SETTINGSMANAGER_H

#include <QObject>
#include <QString>

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
};

#endif // SETTINGSMANAGER_H
