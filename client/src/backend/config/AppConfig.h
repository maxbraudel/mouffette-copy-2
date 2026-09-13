#ifndef APPCONFIG_H
#define APPCONFIG_H

#include <QProcessEnvironment>
#include <QString>
#include <QStringList>
#include <QUrl>
#include <QVariantMap>

#include <map>

/**
 * Typed, source-aware runtime configuration for the desktop client.
 *
 * Precedence is deliberately fixed:
 *   compiled defaults < .env file < QSettings < process environment < CLI
 *   < production override.
 *
 * The bundled .env is the normal file layer. MOUFFETTE_ENV_FILE or
 * --env-file can replace that layer completely, which is useful for packaged
 * deployments without making build-time constants the operational API.
 */
class AppConfig final {
public:
    enum class Key {
        ServerUrl,
        RemoteSessionHiddenTimeoutMs,
        ProjectHiddenRetentionMs,
        UploadConcurrency,
        AutoUploadImportedMedia,
        QtMediaBackend,
        AllowMultipleInstances,
        CursorDebug,
        RuntimeDiagnostics,
        MigrationTelemetry,
        CanvasProfiling
    };

    struct LoadOptions {
        QStringList arguments;
        QProcessEnvironment processEnvironment;
        QVariantMap settings;
        QString defaultEnvFilePath = QStringLiteral(":/config/client.env");
        QString productionEnvFilePath = QStringLiteral(":/config/client.production.env");
        bool applyProductionOverride = false;
    };

    AppConfig();

    static AppConfig& instance();

    // Loads the real process/QSettings sources. Safe to call before QApplication.
    bool initialize(const QStringList& arguments, QString* errorMessage = nullptr);

    // Loads only compiled defaults, env files, process environment and CLI.
    // This is the only startup entry point allowed before the runtime storage
    // bootstrap has validated the profile-specific settings file.
    bool initializePreApplication(const QStringList& arguments,
                                  QString* errorMessage = nullptr);

    // Reloads the normal runtime sources with an explicitly selected settings
    // profile. Secondary development instances use this after their isolated
    // temporary profile has been allocated.
    bool initializeWithSettings(const QStringList& arguments,
                                const QVariantMap& settings,
                                QString* errorMessage = nullptr);

    // Public to keep precedence and validation testable without mutating the
    // process environment or the user's native QSettings store.
    bool load(const LoadOptions& options, QString* errorMessage = nullptr);

    // Applies the same URL validation to values edited at runtime as the
    // startup configuration loader. Both ws:// and wss:// are accepted for
    // every valid host.
    static bool validateServerUrl(const QString& value,
                                  QUrl* normalized = nullptr,
                                  QString* errorMessage = nullptr);

    void applyPreApplicationEnvironment() const;

    bool isLoaded() const { return m_loaded; }
    QString loadedEnvFilePath() const { return m_loadedEnvFilePath; }
    QString provenance(Key key) const;

    QString serverUrl() const { return m_serverUrl.toString(QUrl::FullyEncoded); }
    QUrl serverUrlValue() const { return m_serverUrl; }
    qint64 remoteSessionHiddenTimeoutMs() const { return m_remoteSessionHiddenTimeoutMs; }
    qint64 projectHiddenRetentionMs() const { return m_projectHiddenRetentionMs; }
    int uploadConcurrency() const { return m_uploadConcurrency; }
    bool autoUploadImportedMedia() const { return m_autoUploadImportedMedia; }
    QString qtMediaBackend() const { return m_qtMediaBackend; }
    bool allowMultipleInstances() const { return m_allowMultipleInstances; }
    bool cursorDebug() const { return m_cursorDebug; }
    bool runtimeDiagnostics() const { return m_runtimeDiagnostics; }
    bool migrationTelemetry() const { return m_migrationTelemetry; }
    bool canvasProfiling() const { return m_canvasProfiling; }

private:
    void resetToCompiledDefaults();

    bool m_loaded = false;
    QString m_loadedEnvFilePath;
    std::map<Key, QString> m_provenance;

    QUrl m_serverUrl;
    qint64 m_remoteSessionHiddenTimeoutMs = 60000;
    qint64 m_projectHiddenRetentionMs = 300000;
    int m_uploadConcurrency = 2;
    bool m_autoUploadImportedMedia = false;
    QString m_qtMediaBackend = QStringLiteral("ffmpeg");
    bool m_allowMultipleInstances = false;
    bool m_cursorDebug = false;
    bool m_runtimeDiagnostics = false;
    bool m_migrationTelemetry = false;
    bool m_canvasProfiling = false;
};

#endif // APPCONFIG_H
