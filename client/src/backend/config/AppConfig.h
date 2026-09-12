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
 *   compiled defaults < .env file < QSettings < process environment < CLI.
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
        UseQuickCanvasRenderer,
        QtMediaBackend,
        InstanceSuffix,
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
    };

    AppConfig();

    static AppConfig& instance();

    // Loads the real process/QSettings sources. Safe to call before QApplication.
    bool initialize(const QStringList& arguments, QString* errorMessage = nullptr);

    // Public to keep precedence and validation testable without mutating the
    // process environment or the user's native QSettings store.
    bool load(const LoadOptions& options, QString* errorMessage = nullptr);

    // Applies the same transport-security policy to values edited at runtime
    // as the startup configuration loader (ws:// only on local/private hosts).
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
    bool useQuickCanvasRenderer() const { return m_useQuickCanvasRenderer; }
    QString qtMediaBackend() const { return m_qtMediaBackend; }
    QString instanceSuffix() const { return m_instanceSuffix; }
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
    bool m_useQuickCanvasRenderer = true;
    QString m_qtMediaBackend = QStringLiteral("ffmpeg");
    QString m_instanceSuffix;
    bool m_cursorDebug = false;
    bool m_runtimeDiagnostics = false;
    bool m_migrationTelemetry = false;
    bool m_canvasProfiling = false;
};

#endif // APPCONFIG_H
