#ifndef SETTINGSMANAGER_H
#define SETTINGSMANAGER_H

#include <QObject>
#include <QString>

class MainWindow;
class WebSocketClient;

/**
 * [PHASE 12] SettingsManager
 * Manages application settings persistence and the settings dialog.
 * Handles:
 * - Server URL configuration
 * - Auto-upload preferences
 * - Persistent client ID generation/retrieval
 * - Settings dialog UI
 */
class SettingsManager : public QObject {
    Q_OBJECT

public:
    explicit SettingsManager(MainWindow* mainWindow, WebSocketClient* webSocketClient, QObject* parent = nullptr);
    ~SettingsManager() = default;

    // Settings dialog
    void showSettingsDialog();
    
    // Settings persistence
    void loadSettings();
    void saveSettings();
    
    // Getters
    QString getServerUrl() const { return m_serverUrlConfig; }
    bool getAutoUploadImportedMedia() const { return m_autoUploadImportedMedia; }
    QString getPersistentClientId() const { return m_persistentClientId; }
    
    // Setters
    void setServerUrl(const QString& url);
    void setAutoUploadImportedMedia(bool enabled);

signals:
    void settingsChanged();
    void serverUrlChanged(const QString& newUrl);

private:
    MainWindow* m_mainWindow;
    WebSocketClient* m_webSocketClient;
    
    // Settings values
    QString m_serverUrlConfig;
    bool m_autoUploadImportedMedia;
    QString m_persistentClientId;
    
    // Persistent client ID generation
    QString generateOrLoadPersistentClientId();
    QString getMachineId() const;
    QString getInstanceSuffix() const;
    QString getInstallFingerprint() const;
};

#endif // SETTINGSMANAGER_H
