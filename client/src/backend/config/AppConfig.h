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
        ProjectMediaHiddenTimeoutMs,
        ProjectHiddenRetentionMs,
        IncomingSessionOrphanTimeoutMs,
        UploadIdleTimeoutMs,
        ConnectionSyncTimeoutMs,
        SessionRetryBaseMs,
        SessionRetryMaxMs,
        ControlRequestRetryMs,
        UploadChannelRetryBaseMs,
        UploadChannelRetryMaxMs,
        UploadChannelAttemptTimeoutMs,
        DeferredCleanupRetryMaxMs,
        ConnectionAttemptTimeoutMs,
        ReconnectFastStepMs,
        ReconnectFastMaxMs,
        ReconnectBaseMs,
        ReconnectMaxMs,
        ReconnectJitterPercent,
        ReconnectStableResetMs,
        LeaseHealthCheckIntervalMs,
        SessionDeadlinePollIntervalMs,
        ProjectAutosaveDelayMs,
        ProjectCheckpointIntervalMs,
        ProjectDeadlinePollIntervalMs,
        ScreenChangeDebounceMs,
        SystemVolumePollIntervalMs,
        FileWatchDebounceMs,
        SceneActivityRefreshIntervalMs,
        ClientCountdownRefreshIntervalMs,
        VideoStatePublishIntervalMs,
        VideoSnapshotIntervalMs,
        RemoteWindowShowDelayMs,
        MediaProbeTimeoutMs,
        IncomingUploadCompletionTtlMs,
        DeferredCleanupRetryMs,
        SceneLaunchTimeoutMarginMs,
        InstanceActivationConnectTimeoutMs,
        InstanceActivationAckTimeoutMs,
        InstanceActivationRetryIntervalMs,
        ControlledDisconnectDrainTimeoutMs,
        ProcessStopTimeoutMs,
        UploadActionMinIntervalMs,
        UploadCancelGuardMs,
        SceneSeekPositionToleranceMs,
        SceneStartFrameToleranceMs,
        SceneDecoderSyncToleranceMs,
        SceneVideoSyncPositionToleranceMs,
        SceneVideoSyncTransitMaxMs,
        SceneAuthoritativeSeekGuardMs,
        SceneRepeatTriggerGuardMs,
        UiContentFadeDurationMs,
        UiSpinnerRotationDurationMs,
        UiScrollbarHideDelayMs,
        UiInputWatchdogIntervalMs,
        UiSnapFreezeCleanupDelayMs,
        TimelineMaxDurationMs,
        TimelineSlotsPerSecond,
        TimelineDefaultClipDurationSlots,
        TimelineHeightPx,
        TimelineRulerHeightPx,
        TimelineClipTrackHeightPx,
        TimelineMinTracksAbove,
        TimelineMinTracksBelow,
        TimelineKeyframeSizePx,
        TimelineOtherKeyframeOpacityPercent,
        TimelineSnapDistancePx,
        TimelineInitialViewDurationMs,
        CanvasTextInitialHeightPercent,
        RemoteCursorDiameterPx,
        ToastDefaultDurationMs,
        ToastInfoDurationMs,
        ToastWarningDurationMs,
        ToastErrorDurationMs,
        ToastAnimationDurationMs,
        MediaRamReservePercent,
        MediaRamReserveMinMiB,
        UploadConcurrency,
        AutoUploadImportedMedia,
        AppAlwaysOnTop,
        QtMediaBackend,
        AllowMultipleInstances,
        CursorDebug,
        RuntimeDiagnostics,
        CanvasProfiling,
        RemoteMediaRetentionMs,
        RemoteMediaCacheMaxMiB,
        NetworkDiagnostics,
        NetworkDiagnosticsVerbose
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
    qint64 projectMediaHiddenTimeoutMs() const { return m_projectMediaHiddenTimeoutMs; }
    qint64 projectHiddenRetentionMs() const { return m_projectHiddenRetentionMs; }
    qint64 incomingSessionOrphanTimeoutMs() const { return m_incomingSessionOrphanTimeoutMs; }
    int uploadIdleTimeoutMs() const { return m_uploadIdleTimeoutMs; }
    int connectionSyncTimeoutMs() const { return m_connectionSyncTimeoutMs; }
    int sessionRetryBaseMs() const { return m_sessionRetryBaseMs; }
    int sessionRetryMaxMs() const { return m_sessionRetryMaxMs; }
    int controlRequestRetryMs() const { return m_controlRequestRetryMs; }
    int uploadChannelRetryBaseMs() const { return m_uploadChannelRetryBaseMs; }
    int uploadChannelRetryMaxMs() const { return m_uploadChannelRetryMaxMs; }
    int uploadChannelAttemptTimeoutMs() const { return m_uploadChannelAttemptTimeoutMs; }
    int deferredCleanupRetryMaxMs() const { return m_deferredCleanupRetryMaxMs; }
    int connectionAttemptTimeoutMs() const { return m_connectionAttemptTimeoutMs; }
    int reconnectStableResetMs() const { return m_reconnectStableResetMs; }
    int reconnectFastStepMs() const { return m_reconnectFastStepMs; }
    int reconnectFastMaxMs() const { return m_reconnectFastMaxMs; }
    int reconnectBaseMs() const { return m_reconnectBaseMs; }
    int reconnectMaxMs() const { return m_reconnectMaxMs; }
    int reconnectJitterPercent() const { return m_reconnectJitterPercent; }
    int leaseHealthCheckIntervalMs() const { return m_leaseHealthCheckIntervalMs; }
    int sessionDeadlinePollIntervalMs() const { return m_sessionDeadlinePollIntervalMs; }
    int projectAutosaveDelayMs() const { return m_projectAutosaveDelayMs; }
    int projectCheckpointIntervalMs() const { return m_projectCheckpointIntervalMs; }
    int projectDeadlinePollIntervalMs() const { return m_projectDeadlinePollIntervalMs; }
    int screenChangeDebounceMs() const { return m_screenChangeDebounceMs; }
    int systemVolumePollIntervalMs() const { return m_systemVolumePollIntervalMs; }
    int fileWatchDebounceMs() const { return m_fileWatchDebounceMs; }
    int sceneActivityRefreshIntervalMs() const { return m_sceneActivityRefreshIntervalMs; }
    int clientCountdownRefreshIntervalMs() const {
        return m_clientCountdownRefreshIntervalMs;
    }
    int videoStatePublishIntervalMs() const { return m_videoStatePublishIntervalMs; }
    int videoSnapshotIntervalMs() const { return m_videoSnapshotIntervalMs; }
    int remoteWindowShowDelayMs() const { return m_remoteWindowShowDelayMs; }
    int mediaProbeTimeoutMs() const { return m_mediaProbeTimeoutMs; }
    int incomingUploadCompletionTtlMs() const { return m_incomingUploadCompletionTtlMs; }
    int deferredCleanupRetryMs() const { return m_deferredCleanupRetryMs; }
    int sceneLaunchTimeoutMarginMs() const { return m_sceneLaunchTimeoutMarginMs; }
    int instanceActivationConnectTimeoutMs() const { return m_instanceActivationConnectTimeoutMs; }
    int instanceActivationAckTimeoutMs() const { return m_instanceActivationAckTimeoutMs; }
    int instanceActivationRetryIntervalMs() const { return m_instanceActivationRetryIntervalMs; }
    int controlledDisconnectDrainTimeoutMs() const { return m_controlledDisconnectDrainTimeoutMs; }
    int processStopTimeoutMs() const { return m_processStopTimeoutMs; }
    int uploadActionMinIntervalMs() const { return m_uploadActionMinIntervalMs; }
    int uploadCancelGuardMs() const { return m_uploadCancelGuardMs; }
    int sceneSeekPositionToleranceMs() const { return m_sceneSeekPositionToleranceMs; }
    int sceneStartFrameToleranceMs() const { return m_sceneStartFrameToleranceMs; }
    int sceneDecoderSyncToleranceMs() const { return m_sceneDecoderSyncToleranceMs; }
    int sceneVideoSyncPositionToleranceMs() const { return m_sceneVideoSyncPositionToleranceMs; }
    int sceneVideoSyncTransitMaxMs() const { return m_sceneVideoSyncTransitMaxMs; }
    int sceneAuthoritativeSeekGuardMs() const { return m_sceneAuthoritativeSeekGuardMs; }
    int sceneRepeatTriggerGuardMs() const { return m_sceneRepeatTriggerGuardMs; }
    int uiContentFadeDurationMs() const { return m_uiContentFadeDurationMs; }
    int uiSpinnerRotationDurationMs() const { return m_uiSpinnerRotationDurationMs; }
    int uiScrollbarHideDelayMs() const { return m_uiScrollbarHideDelayMs; }
    int uiInputWatchdogIntervalMs() const { return m_uiInputWatchdogIntervalMs; }
    int uiSnapFreezeCleanupDelayMs() const { return m_uiSnapFreezeCleanupDelayMs; }
    int timelineMaxDurationMs() const { return m_timelineMaxDurationMs; }
    int timelineSlotsPerSecond() const { return m_timelineSlotsPerSecond; }
    int timelineDefaultClipDurationSlots() const { return m_timelineDefaultClipDurationSlots; }
    int timelineHeightPx() const { return m_timelineHeightPx; }
    int timelineRulerHeightPx() const { return m_timelineRulerHeightPx; }
    int timelineClipTrackHeightPx() const { return m_timelineClipTrackHeightPx; }
    int timelineMinTracksAbove() const { return m_timelineMinTracksAbove; }
    int timelineMinTracksBelow() const { return m_timelineMinTracksBelow; }
    int timelineKeyframeSizePx() const { return m_timelineKeyframeSizePx; }
    int timelineOtherKeyframeOpacityPercent() const { return m_timelineOtherKeyframeOpacityPercent; }
    int timelineSnapDistancePx() const { return m_timelineSnapDistancePx; }
    int timelineInitialViewDurationMs() const { return m_timelineInitialViewDurationMs; }
    int remoteCursorDiameterPx() const { return m_remoteCursorDiameterPx; }
    int canvasTextInitialHeightPercent() const { return m_canvasTextInitialHeightPercent; }
    int toastDefaultDurationMs() const { return m_toastDefaultDurationMs; }
    int toastInfoDurationMs() const { return m_toastInfoDurationMs; }
    int toastWarningDurationMs() const { return m_toastWarningDurationMs; }
    int toastErrorDurationMs() const { return m_toastErrorDurationMs; }
    int toastAnimationDurationMs() const { return m_toastAnimationDurationMs; }
    int mediaRamReservePercent() const { return m_mediaRamReservePercent; }
    int mediaRamReserveMinMiB() const { return m_mediaRamReserveMinMiB; }
    int uploadConcurrency() const { return m_uploadConcurrency; }
    bool autoUploadImportedMedia() const { return m_autoUploadImportedMedia; }
    bool appAlwaysOnTop() const { return m_appAlwaysOnTop; }
    QString qtMediaBackend() const { return m_qtMediaBackend; }
    bool allowMultipleInstances() const { return m_allowMultipleInstances; }
    bool cursorDebug() const { return m_cursorDebug; }
    bool runtimeDiagnostics() const { return m_runtimeDiagnostics; }
    bool canvasProfiling() const { return m_canvasProfiling; }

    int remoteMediaRetentionMs() const { return m_remoteMediaRetentionMs; }
    int remoteMediaCacheMaxMiB() const { return m_remoteMediaCacheMaxMiB; }
    bool networkDiagnostics() const { return m_networkDiagnostics; }
    bool networkDiagnosticsVerbose() const { return m_networkDiagnosticsVerbose; }

private:
    void resetToCompiledDefaults();

    bool m_loaded = false;
    QString m_loadedEnvFilePath;
    std::map<Key, QString> m_provenance;

    QUrl m_serverUrl;
    qint64 m_remoteSessionHiddenTimeoutMs = 60000;
    qint64 m_projectMediaHiddenTimeoutMs = 60000;
    qint64 m_projectHiddenRetentionMs = 300000;
    qint64 m_incomingSessionOrphanTimeoutMs = 3000;
    int m_uploadIdleTimeoutMs = 45000;
    int m_connectionSyncTimeoutMs = 10000;
    int m_sessionRetryBaseMs = 1000;
    int m_sessionRetryMaxMs = 5000;
    int m_controlRequestRetryMs = 1000;
    int m_uploadChannelRetryBaseMs = 1000;
    int m_uploadChannelRetryMaxMs = 5000;
    int m_uploadChannelAttemptTimeoutMs = 10000;
    int m_deferredCleanupRetryMaxMs = 30000;
    int m_connectionAttemptTimeoutMs = 10000;
    int m_reconnectStableResetMs = 30000;
    int m_reconnectFastStepMs = 250;
    int m_reconnectFastMaxMs = 750;
    int m_reconnectBaseMs = 1000;
    int m_reconnectMaxMs = 5000;
    int m_reconnectJitterPercent = 20;
    int m_leaseHealthCheckIntervalMs = 100;
    int m_sessionDeadlinePollIntervalMs = 250;
    int m_projectAutosaveDelayMs = 300;
    int m_projectCheckpointIntervalMs = 15000;
    int m_projectDeadlinePollIntervalMs = 250;
    int m_screenChangeDebounceMs = 150;
    int m_systemVolumePollIntervalMs = 1200;
    int m_fileWatchDebounceMs = 500;
    int m_sceneActivityRefreshIntervalMs = 1000;
    int m_clientCountdownRefreshIntervalMs = 1000;
    int m_videoStatePublishIntervalMs = 50;
    int m_videoSnapshotIntervalMs = 1000;
    int m_remoteWindowShowDelayMs = 10;
    int m_mediaProbeTimeoutMs = 5000;
    int m_incomingUploadCompletionTtlMs = 60000;
    int m_deferredCleanupRetryMs = 1000;
    int m_sceneLaunchTimeoutMarginMs = 1000;
    int m_instanceActivationConnectTimeoutMs = 100;
    int m_instanceActivationAckTimeoutMs = 250;
    int m_instanceActivationRetryIntervalMs = 25;
    int m_controlledDisconnectDrainTimeoutMs = 10000;
    int m_processStopTimeoutMs = 100;
    int m_uploadActionMinIntervalMs = 300;
    int m_uploadCancelGuardMs = 1000;
    int m_sceneSeekPositionToleranceMs = 120;
    int m_sceneStartFrameToleranceMs = 25;
    int m_sceneDecoderSyncToleranceMs = 25;
    int m_sceneVideoSyncPositionToleranceMs = 400;
    int m_sceneVideoSyncTransitMaxMs = 2000;
    int m_sceneAuthoritativeSeekGuardMs = 250;
    int m_sceneRepeatTriggerGuardMs = 500;
    int m_uiContentFadeDurationMs = 200;
    int m_uiSpinnerRotationDurationMs = 900;
    int m_uiScrollbarHideDelayMs = 500;
    int m_uiInputWatchdogIntervalMs = 120;
    int m_uiSnapFreezeCleanupDelayMs = 300;
    int m_toastDefaultDurationMs = 4000;
    int m_toastInfoDurationMs = 2000;
    int m_toastWarningDurationMs = 3500;
    int m_toastErrorDurationMs = 5000;
    int m_toastAnimationDurationMs = 300;
    int m_timelineMaxDurationMs = 180000;
    int m_timelineSlotsPerSecond = 30;
    int m_timelineDefaultClipDurationSlots = 30;
    int m_timelineHeightPx = 240;
    int m_timelineRulerHeightPx = 28;
    int m_timelineClipTrackHeightPx = 48;
    int m_timelineMinTracksAbove = 10;
    int m_timelineMinTracksBelow = 10;
    int m_timelineKeyframeSizePx = 10;
    int m_timelineOtherKeyframeOpacityPercent = 30;
    int m_timelineSnapDistancePx = 10;
    int m_timelineInitialViewDurationMs = 15000;
    int m_canvasTextInitialHeightPercent = 8;
    int m_remoteCursorDiameterPx = 30;
    int m_mediaRamReservePercent = 0;
    int m_mediaRamReserveMinMiB = 548;
    int m_uploadConcurrency = 2;
    bool m_autoUploadImportedMedia = false;
    bool m_appAlwaysOnTop = true;
    QString m_qtMediaBackend = QStringLiteral("ffmpeg");
    bool m_allowMultipleInstances = false;
    bool m_cursorDebug = false;
    bool m_runtimeDiagnostics = false;
    bool m_canvasProfiling = false;
    int m_remoteMediaRetentionMs = 600000;
    int m_remoteMediaCacheMaxMiB = 10240;
    bool m_networkDiagnostics = true;
    bool m_networkDiagnosticsVerbose = false;
};

#endif // APPCONFIG_H
