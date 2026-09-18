#include "backend/config/AppConfig.h"
#include "backend/runtime/RuntimeProfile.h"
#include "AppBuildConfig.h"

#include <QFile>
#include <QDebug>
#include <QMap>
#include <QRegularExpression>
#include <QSettings>
#include <QTextStream>

#include <array>

namespace {

using Key = AppConfig::Key;

struct SettingSpec {
    Key key;
    const char* envName;
    const char* cliName;
    const char* settingsName;
    const char* defaultValue;
    bool boolean;
};

constexpr std::array<SettingSpec, 90> kSpecs{{
    {Key::ServerUrl, "MOUFFETTE_SERVER_URL", "server-url", "serverUrl", "ws://localhost:8080", false},
    {Key::RemoteSessionHiddenTimeoutMs, "MOUFFETTE_REMOTE_SESSION_HIDDEN_TIMEOUT_MS", "remote-session-hidden-timeout-ms", nullptr, "60000", false},
    {Key::ProjectMediaHiddenTimeoutMs, "MOUFFETTE_PROJECT_MEDIA_HIDDEN_TIMEOUT_MS", "project-media-hidden-timeout-ms", nullptr, "60000", false},
    {Key::ProjectHiddenRetentionMs, "MOUFFETTE_PROJECT_HIDDEN_RETENTION_MS", "project-hidden-retention-ms", nullptr, "300000", false},
    {Key::IncomingSessionOrphanTimeoutMs, "MOUFFETTE_INCOMING_SESSION_ORPHAN_TIMEOUT_MS", "incoming-session-orphan-timeout-ms", nullptr, "3000", false},
    {Key::UploadIdleTimeoutMs, "MOUFFETTE_UPLOAD_IDLE_TIMEOUT_MS", "upload-idle-timeout-ms", nullptr, "45000", false},
    {Key::ConnectionSyncTimeoutMs, "MOUFFETTE_CONNECTION_SYNC_TIMEOUT_MS", "connection-sync-timeout-ms", nullptr, "10000", false},
    {Key::SessionRetryBaseMs, "MOUFFETTE_SESSION_RETRY_BASE_MS", "session-retry-base-ms", nullptr, "1000", false},
    {Key::SessionRetryMaxMs, "MOUFFETTE_SESSION_RETRY_MAX_MS", "session-retry-max-ms", nullptr, "5000", false},
    {Key::ControlRequestRetryMs, "MOUFFETTE_CONTROL_REQUEST_RETRY_MS", "control-request-retry-ms", nullptr, "1000", false},
    {Key::UploadChannelRetryBaseMs, "MOUFFETTE_UPLOAD_CHANNEL_RETRY_BASE_MS", "upload-channel-retry-base-ms", nullptr, "1000", false},
    {Key::UploadChannelRetryMaxMs, "MOUFFETTE_UPLOAD_CHANNEL_RETRY_MAX_MS", "upload-channel-retry-max-ms", nullptr, "5000", false},
    {Key::UploadChannelAttemptTimeoutMs, "MOUFFETTE_UPLOAD_CHANNEL_ATTEMPT_TIMEOUT_MS", "upload-channel-attempt-timeout-ms", nullptr, "10000", false},
    {Key::DeferredCleanupRetryMaxMs, "MOUFFETTE_DEFERRED_CLEANUP_RETRY_MAX_MS", "deferred-cleanup-retry-max-ms", nullptr, "30000", false},
    {Key::ConnectionAttemptTimeoutMs, "MOUFFETTE_CONNECTION_ATTEMPT_TIMEOUT_MS", "connection-attempt-timeout-ms", nullptr, "10000", false},
    {Key::ReconnectFastStepMs, "MOUFFETTE_RECONNECT_FAST_STEP_MS", "reconnect-fast-step-ms", nullptr, "250", false},
    {Key::ReconnectFastMaxMs, "MOUFFETTE_RECONNECT_FAST_MAX_MS", "reconnect-fast-max-ms", nullptr, "750", false},
    {Key::ReconnectBaseMs, "MOUFFETTE_RECONNECT_BASE_MS", "reconnect-base-ms", nullptr, "1000", false},
    {Key::ReconnectMaxMs, "MOUFFETTE_RECONNECT_MAX_MS", "reconnect-max-ms", nullptr, "5000", false},
    {Key::ReconnectStableResetMs, "MOUFFETTE_RECONNECT_STABLE_RESET_MS", "reconnect-stable-reset-ms", nullptr, "30000", false},
    {Key::ReconnectJitterPercent, "MOUFFETTE_RECONNECT_JITTER_PERCENT", "reconnect-jitter-percent", nullptr, "20", false},
    {Key::LeaseHealthCheckIntervalMs, "MOUFFETTE_LEASE_HEALTH_CHECK_INTERVAL_MS", "lease-health-check-interval-ms", nullptr, "100", false},
    {Key::SessionDeadlinePollIntervalMs, "MOUFFETTE_SESSION_DEADLINE_POLL_INTERVAL_MS", "session-deadline-poll-interval-ms", nullptr, "250", false},
    {Key::ProjectAutosaveDelayMs, "MOUFFETTE_PROJECT_AUTOSAVE_DELAY_MS", "project-autosave-delay-ms", nullptr, "300", false},
    {Key::ProjectCheckpointIntervalMs, "MOUFFETTE_PROJECT_CHECKPOINT_INTERVAL_MS", "project-checkpoint-interval-ms", nullptr, "15000", false},
    {Key::ProjectDeadlinePollIntervalMs, "MOUFFETTE_PROJECT_DEADLINE_POLL_INTERVAL_MS", "project-deadline-poll-interval-ms", nullptr, "250", false},
    {Key::ScreenChangeDebounceMs, "MOUFFETTE_SCREEN_CHANGE_DEBOUNCE_MS", "screen-change-debounce-ms", nullptr, "150", false},
    {Key::SystemVolumePollIntervalMs, "MOUFFETTE_SYSTEM_VOLUME_POLL_INTERVAL_MS", "system-volume-poll-interval-ms", nullptr, "1200", false},
    {Key::FileWatchDebounceMs, "MOUFFETTE_FILE_WATCH_DEBOUNCE_MS", "file-watch-debounce-ms", nullptr, "500", false},
    {Key::SceneActivityRefreshIntervalMs, "MOUFFETTE_SCENE_ACTIVITY_REFRESH_INTERVAL_MS", "scene-activity-refresh-interval-ms", nullptr, "1000", false},
    {Key::ClientCountdownRefreshIntervalMs, "MOUFFETTE_CLIENT_COUNTDOWN_REFRESH_INTERVAL_MS", "client-countdown-refresh-interval-ms", nullptr, "1000", false},
    {Key::VideoStatePublishIntervalMs, "MOUFFETTE_VIDEO_STATE_PUBLISH_INTERVAL_MS", "video-state-publish-interval-ms", nullptr, "50", false},
    {Key::VideoSnapshotIntervalMs, "MOUFFETTE_VIDEO_SNAPSHOT_INTERVAL_MS", "video-snapshot-interval-ms", nullptr, "1000", false},
    {Key::RemoteWindowShowDelayMs, "MOUFFETTE_REMOTE_WINDOW_SHOW_DELAY_MS", "remote-window-show-delay-ms", nullptr, "10", false},
    {Key::MediaProbeTimeoutMs, "MOUFFETTE_MEDIA_PROBE_TIMEOUT_MS", "media-probe-timeout-ms", nullptr, "5000", false},
    {Key::IncomingUploadCompletionTtlMs, "MOUFFETTE_INCOMING_UPLOAD_COMPLETION_TTL_MS", "incoming-upload-completion-ttl-ms", nullptr, "60000", false},
    {Key::DeferredCleanupRetryMs, "MOUFFETTE_DEFERRED_CLEANUP_RETRY_MS", "deferred-cleanup-retry-ms", nullptr, "1000", false},
    {Key::SceneLaunchTimeoutMarginMs, "MOUFFETTE_SCENE_LAUNCH_TIMEOUT_MARGIN_MS", "scene-launch-timeout-margin-ms", nullptr, "1000", false},
    {Key::InstanceActivationConnectTimeoutMs, "MOUFFETTE_INSTANCE_ACTIVATION_CONNECT_TIMEOUT_MS", "instance-activation-connect-timeout-ms", nullptr, "100", false},
    {Key::InstanceActivationAckTimeoutMs, "MOUFFETTE_INSTANCE_ACTIVATION_ACK_TIMEOUT_MS", "instance-activation-ack-timeout-ms", nullptr, "250", false},
    {Key::InstanceActivationRetryIntervalMs, "MOUFFETTE_INSTANCE_ACTIVATION_RETRY_INTERVAL_MS", "instance-activation-retry-interval-ms", nullptr, "25", false},
    {Key::ControlledDisconnectDrainTimeoutMs, "MOUFFETTE_CONTROLLED_DISCONNECT_DRAIN_TIMEOUT_MS", "controlled-disconnect-drain-timeout-ms", nullptr, "10000", false},
    {Key::ProcessStopTimeoutMs, "MOUFFETTE_PROCESS_STOP_TIMEOUT_MS", "process-stop-timeout-ms", nullptr, "100", false},
    {Key::UploadActionMinIntervalMs, "MOUFFETTE_UPLOAD_ACTION_MIN_INTERVAL_MS", "upload-action-min-interval-ms", nullptr, "300", false},
    {Key::UploadCancelGuardMs, "MOUFFETTE_UPLOAD_CANCEL_GUARD_MS", "upload-cancel-guard-ms", nullptr, "1000", false},
    {Key::SceneSeekPositionToleranceMs, "MOUFFETTE_SCENE_SEEK_POSITION_TOLERANCE_MS", "scene-seek-position-tolerance-ms", nullptr, "120", false},
    {Key::SceneStartFrameToleranceMs, "MOUFFETTE_SCENE_START_FRAME_TOLERANCE_MS", "scene-start-frame-tolerance-ms", nullptr, "25", false},
    {Key::SceneDecoderSyncToleranceMs, "MOUFFETTE_SCENE_DECODER_SYNC_TOLERANCE_MS", "scene-decoder-sync-tolerance-ms", nullptr, "25", false},
    {Key::SceneVideoSyncPositionToleranceMs, "MOUFFETTE_SCENE_VIDEO_SYNC_POSITION_TOLERANCE_MS", "scene-video-sync-position-tolerance-ms", nullptr, "400", false},
    {Key::SceneVideoSyncTransitMaxMs, "MOUFFETTE_SCENE_VIDEO_SYNC_TRANSIT_MAX_MS", "scene-video-sync-transit-max-ms", nullptr, "2000", false},
    {Key::SceneAuthoritativeSeekGuardMs, "MOUFFETTE_SCENE_AUTHORITATIVE_SEEK_GUARD_MS", "scene-authoritative-seek-guard-ms", nullptr, "250", false},
    {Key::SceneRepeatTriggerGuardMs, "MOUFFETTE_SCENE_REPEAT_TRIGGER_GUARD_MS", "scene-repeat-trigger-guard-ms", nullptr, "500", false},
    {Key::UiContentFadeDurationMs, "MOUFFETTE_UI_CONTENT_FADE_DURATION_MS", "ui-content-fade-duration-ms", nullptr, "200", false},
    {Key::UiSpinnerRotationDurationMs, "MOUFFETTE_UI_SPINNER_ROTATION_DURATION_MS", "ui-spinner-rotation-duration-ms", nullptr, "900", false},
    {Key::UiScrollbarHideDelayMs, "MOUFFETTE_UI_SCROLLBAR_HIDE_DELAY_MS", "ui-scrollbar-hide-delay-ms", nullptr, "500", false},
    {Key::UiInputWatchdogIntervalMs, "MOUFFETTE_UI_INPUT_WATCHDOG_INTERVAL_MS", "ui-input-watchdog-interval-ms", nullptr, "120", false},
    {Key::UiSnapFreezeCleanupDelayMs, "MOUFFETTE_UI_SNAP_FREEZE_CLEANUP_DELAY_MS", "ui-snap-freeze-cleanup-delay-ms", nullptr, "300", false},
    {Key::TimelineMaxDurationMs, "MOUFFETTE_TIMELINE_MAX_DURATION_MS", "timeline-max-duration-ms", nullptr, "180000", false},
    {Key::TimelineSlotsPerSecond, "MOUFFETTE_TIMELINE_SLOTS_PER_SECOND", "timeline-slots-per-second", nullptr, "30", false},
    {Key::TimelineDefaultClipDurationSlots, "MOUFFETTE_TIMELINE_DEFAULT_CLIP_DURATION_SLOTS", "timeline-default-clip-duration-slots", nullptr, "30", false},
    {Key::TimelineHeightPx, "MOUFFETTE_TIMELINE_HEIGHT_PX", "timeline-height-px", nullptr, "240", false},
    {Key::TimelineRulerHeightPx, "MOUFFETTE_TIMELINE_RULER_HEIGHT_PX", "timeline-ruler-height-px", nullptr, "28", false},
    {Key::TimelineClipTrackHeightPx, "MOUFFETTE_TIMELINE_CLIP_TRACK_HEIGHT_PX", "timeline-clip-track-height-px", nullptr, "48", false},
    {Key::TimelineMinTracksAbove, "MOUFFETTE_TIMELINE_MIN_TRACKS_ABOVE", "timeline-min-tracks-above", nullptr, "10", false},
    {Key::TimelineMinTracksBelow, "MOUFFETTE_TIMELINE_MIN_TRACKS_BELOW", "timeline-min-tracks-below", nullptr, "10", false},
    {Key::TimelineKeyframeSizePx, "MOUFFETTE_TIMELINE_KEYFRAME_SIZE_PX", "timeline-keyframe-size-px", nullptr, "10", false},
    {Key::TimelineOtherKeyframeOpacityPercent, "MOUFFETTE_TIMELINE_OTHER_KEYFRAME_OPACITY_PERCENT", "timeline-other-keyframe-opacity-percent", nullptr, "30", false},
    {Key::TimelineSnapDistancePx, "MOUFFETTE_TIMELINE_SNAP_DISTANCE_PX", "timeline-snap-distance-px", nullptr, "10", false},
    {Key::TimelineInitialViewDurationMs, "MOUFFETTE_TIMELINE_INITIAL_VIEW_DURATION_MS", "timeline-initial-view-duration-ms", nullptr, "15000", false},
    {Key::RemoteCursorDiameterPx, "MOUFFETTE_REMOTE_CURSOR_DIAMETER_PX", "remote-cursor-diameter-px", nullptr, "30", false},
    {Key::CanvasTextInitialHeightPercent, "MOUFFETTE_CANVAS_TEXT_INITIAL_HEIGHT_PERCENT", "canvas-text-initial-height-percent", nullptr, "8", false},
    {Key::ToastDefaultDurationMs, "MOUFFETTE_TOAST_DEFAULT_DURATION_MS", "toast-default-duration-ms", nullptr, "4000", false},
    {Key::ToastInfoDurationMs, "MOUFFETTE_TOAST_INFO_DURATION_MS", "toast-info-duration-ms", nullptr, "2000", false},
    {Key::ToastWarningDurationMs, "MOUFFETTE_TOAST_WARNING_DURATION_MS", "toast-warning-duration-ms", nullptr, "3500", false},
    {Key::ToastErrorDurationMs, "MOUFFETTE_TOAST_ERROR_DURATION_MS", "toast-error-duration-ms", nullptr, "5000", false},
    {Key::ToastAnimationDurationMs, "MOUFFETTE_TOAST_ANIMATION_DURATION_MS", "toast-animation-duration-ms", nullptr, "300", false},
    {Key::MediaRamReservePercent, "MOUFFETTE_MEDIA_RAM_RESERVE_PERCENT", "media-ram-reserve-percent", nullptr, "0", false},
    {Key::MediaRamReserveMinMiB, "MOUFFETTE_MEDIA_RAM_RESERVE_MIN_MIB", "media-ram-reserve-min-mib", nullptr, "548", false},
    {Key::UploadConcurrency, "MOUFFETTE_UPLOAD_CONCURRENCY", "upload-concurrency", nullptr, "2", false},
    {Key::AutoUploadImportedMedia, "MOUFFETTE_AUTO_UPLOAD_IMPORTED_MEDIA", "auto-upload-imported-media", "autoUploadImportedMedia", "false", true},
    {Key::AppAlwaysOnTop, "MOUFFETTE_APP_ALWAYS_ON_TOP", "app-always-on-top", "appAlwaysOnTop", "true", true},
    {Key::QtMediaBackend, "QT_MEDIA_BACKEND", "media-backend", nullptr, "ffmpeg", false},
    {Key::AllowMultipleInstances, "MOUFFETTE_ALLOW_MULTIPLE_INSTANCES", "allow-multiple-instances", nullptr, "false", true},
    {Key::CursorDebug, "MOUFFETTE_CURSOR_DEBUG", "cursor-debug", nullptr, "false", true},
    {Key::RuntimeDiagnostics, "MOUFFETTE_RUNTIME_DIAGNOSTICS", "runtime-diagnostics", nullptr, "false", true},
    {Key::CanvasProfiling, "MOUFFETTE_CANVAS_PROFILING", "canvas-profiling", nullptr, "false", true},
    {Key::RemoteMediaRetentionMs, "MOUFFETTE_REMOTE_MEDIA_RETENTION_MS", "remote-media-retention-ms", nullptr, "600000", false},
    {Key::RemoteMediaCacheMaxMiB, "MOUFFETTE_REMOTE_MEDIA_CACHE_MAX_MIB", "remote-media-cache-max-mib", nullptr, "10240", false},
    {Key::NetworkDiagnostics, "MOUFFETTE_NETWORK_DIAGNOSTICS", "network-diagnostics", nullptr, "true", true},
    {Key::NetworkDiagnosticsVerbose, "MOUFFETTE_NETWORK_DIAGNOSTICS_VERBOSE", "network-diagnostics-verbose", nullptr, "false", true},
}};

struct RawValue {
    QString value;
    QString source;
};

using RawValues = std::map<Key, RawValue>;

const SettingSpec* specForEnvName(const QString& name) {
    for (const SettingSpec& spec : kSpecs) {
        if (name == QLatin1String(spec.envName)) {
            return &spec;
        }
    }
    return nullptr;
}

const SettingSpec* specForCliName(const QString& name) {
    for (const SettingSpec& spec : kSpecs) {
        if (name == QLatin1String(spec.cliName)) {
            return &spec;
        }
    }
    return nullptr;
}

QString keyName(Key key) {
    for (const SettingSpec& spec : kSpecs) {
        if (spec.key == key) {
            return QLatin1String(spec.envName);
        }
    }
    return QStringLiteral("unknown");
}

bool setError(QString* output, const QString& message) {
    if (output) {
        *output = message;
    }
    return false;
}

bool parseBoolean(const RawValue& raw, const QString& name, bool& output, QString* error) {
    const QString normalized = raw.value.trimmed().toLower();
    if (normalized == QStringLiteral("true") || normalized == QStringLiteral("1")
        || normalized == QStringLiteral("yes") || normalized == QStringLiteral("on")) {
        output = true;
        return true;
    }
    if (normalized == QStringLiteral("false") || normalized == QStringLiteral("0")
        || normalized == QStringLiteral("no") || normalized == QStringLiteral("off")) {
        output = false;
        return true;
    }
    return setError(error, QStringLiteral("%1 from %2 must be a boolean, got '%3'")
                               .arg(name, raw.source, raw.value));
}

bool parseInteger(const RawValue& raw,
                  const QString& name,
                  qint64 minimum,
                  qint64 maximum,
                  qint64& output,
                  QString* error) {
    bool ok = false;
    const qint64 parsed = raw.value.trimmed().toLongLong(&ok, 10);
    if (!ok || parsed < minimum || parsed > maximum) {
        return setError(error, QStringLiteral("%1 from %2 must be an integer in [%3, %4], got '%5'")
                                   .arg(name, raw.source)
                                   .arg(minimum)
                                   .arg(maximum)
                                   .arg(raw.value));
    }
    output = parsed;
    return true;
}

bool parseServerUrl(const RawValue& raw, QUrl& output, QString* error) {
    const QUrl url(raw.value.trimmed(), QUrl::StrictMode);
    const QString scheme = url.scheme().toLower();
    if (!url.isValid() || (scheme != QStringLiteral("ws") && scheme != QStringLiteral("wss"))
        || url.host().isEmpty() || !url.userInfo().isEmpty() || url.hasFragment()) {
        return setError(error, QStringLiteral("MOUFFETTE_SERVER_URL from %1 is not a valid ws/wss URL")
                                   .arg(raw.source));
    }
    const int port = url.port(-1);
    if (port == 0 || port > 65535) {
        return setError(error, QStringLiteral("MOUFFETTE_SERVER_URL from %1 has an invalid port")
                                   .arg(raw.source));
    }
    output = url;
    return true;
}

bool decodeQuotedValue(const QString& raw, QString& output, QString* error, int lineNumber) {
    if (raw.isEmpty() || (raw.front() != QLatin1Char('\'') && raw.front() != QLatin1Char('"'))) {
        QString unquoted = raw.trimmed();
        for (int i = 1; i < unquoted.size(); ++i) {
            if (unquoted.at(i) == QLatin1Char('#') && unquoted.at(i - 1).isSpace()) {
                unquoted = unquoted.left(i).trimmed();
                break;
            }
        }
        output = unquoted;
        return true;
    }

    const QChar quote = raw.front();
    int closing = -1;
    bool escaped = false;
    for (int i = 1; i < raw.size(); ++i) {
        const QChar current = raw.at(i);
        if (quote == QLatin1Char('"') && !escaped && current == QLatin1Char('\\')) {
            escaped = true;
            continue;
        }
        if (!escaped && current == quote) {
            closing = i;
            break;
        }
        escaped = false;
    }
    if (closing < 0 || (!raw.mid(closing + 1).trimmed().isEmpty()
        && !raw.mid(closing + 1).trimmed().startsWith(QLatin1Char('#')))) {
        return setError(error, QStringLiteral("Malformed quoted value in env file at line %1").arg(lineNumber));
    }

    QString decoded = raw.mid(1, closing - 1);
    if (quote == QLatin1Char('"')) {
        decoded.replace(QStringLiteral("\\n"), QStringLiteral("\n"));
        decoded.replace(QStringLiteral("\\r"), QStringLiteral("\r"));
        decoded.replace(QStringLiteral("\\t"), QStringLiteral("\t"));
        decoded.replace(QStringLiteral("\\\""), QStringLiteral("\""));
        decoded.replace(QStringLiteral("\\\\"), QStringLiteral("\\"));
    }
    output = decoded;
    return true;
}

bool readEnvFile(const QString& path, QMap<QString, QString>& values, QString* error) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        return setError(error, QStringLiteral("Cannot read env file '%1': %2").arg(path, file.errorString()));
    }

    QTextStream stream(&file);
    int lineNumber = 0;
    while (!stream.atEnd()) {
        QString line = stream.readLine();
        ++lineNumber;
        if (lineNumber == 1 && !line.isEmpty() && line.front() == QChar(0xfeff)) {
            line.remove(0, 1);
        }
        line = line.trimmed();
        if (line.isEmpty() || line.startsWith(QLatin1Char('#'))) {
            continue;
        }
        if (line.startsWith(QStringLiteral("export "))) {
            line = line.mid(7).trimmed();
        }
        const int equals = line.indexOf(QLatin1Char('='));
        if (equals <= 0) {
            return setError(error, QStringLiteral("Malformed env entry at %1:%2").arg(path).arg(lineNumber));
        }

        const QString name = line.left(equals).trimmed();
        static const QRegularExpression namePattern(QStringLiteral("^[A-Za-z_][A-Za-z0-9_]*$"));
        if (!namePattern.match(name).hasMatch()) {
            return setError(error, QStringLiteral("Invalid env key at %1:%2").arg(path).arg(lineNumber));
        }
        if (!specForEnvName(name)) {
            if (name.startsWith(QStringLiteral("MOUFFETTE_")) || name.startsWith(QStringLiteral("QT_"))) {
                qWarning().noquote() << QStringLiteral("Unknown client config key '%1' at %2:%3")
                                            .arg(name, path)
                                            .arg(lineNumber);
            }
            continue;
        }
        if (values.contains(name)) {
            return setError(error, QStringLiteral("Duplicate env key '%1' at %2:%3")
                                       .arg(name, path)
                                       .arg(lineNumber));
        }

        QString decoded;
        if (!decodeQuotedValue(line.mid(equals + 1).trimmed(), decoded, error, lineNumber)) {
            return false;
        }
        values.insert(name, decoded);
    }
    return true;
}

struct ParsedArguments {
    QMap<Key, QString> values;
    QString envFile;
    bool envFileSpecified = false;
};

bool parseArguments(const QStringList& arguments, ParsedArguments& parsed, QString* error) {
    for (int i = 0; i < arguments.size(); ++i) {
        const QString argument = arguments.at(i);
        if (!argument.startsWith(QStringLiteral("--"))) {
            continue;
        }

        QString name;
        QString value;
        const int equals = argument.indexOf(QLatin1Char('='));
        if (equals >= 0) {
            name = argument.mid(2, equals - 2);
            value = argument.mid(equals + 1);
        } else {
            name = argument.mid(2);
        }

        const bool isEnvFile = name == QStringLiteral("env-file");
        const SettingSpec* spec = isEnvFile ? nullptr : specForCliName(name);
        if (!isEnvFile && !spec) {
            continue; // Preserve Qt and future application arguments.
        }

        if (equals < 0) {
            if (spec && spec->boolean
                && (i + 1 >= arguments.size() || arguments.at(i + 1).startsWith(QStringLiteral("--")))) {
                value = QStringLiteral("true");
            } else if (i + 1 < arguments.size()) {
                value = arguments.at(++i);
            } else {
                return setError(error, QStringLiteral("Command-line option --%1 requires a value").arg(name));
            }
        }

        if (isEnvFile) {
            if (value.trimmed().isEmpty()) {
                return setError(error, QStringLiteral("Command-line option --env-file requires a non-empty path"));
            }
            parsed.envFile = value;
            parsed.envFileSpecified = true;
        } else {
            parsed.values.insert(spec->key, value);
        }
    }
    return true;
}

} // namespace

AppConfig::AppConfig() {
    resetToCompiledDefaults();
}

AppConfig& AppConfig::instance() {
    static AppConfig config;
    return config;
}

bool AppConfig::validateServerUrl(const QString& value,
                                  QUrl* normalized,
                                  QString* errorMessage)
{
    QUrl parsed;
    if (!parseServerUrl(
            RawValue{value, QStringLiteral("runtime settings")},
            parsed, errorMessage)) {
        return false;
    }
    if (normalized) {
        *normalized = parsed;
    }
    return true;
}

void AppConfig::resetToCompiledDefaults() {
    m_loaded = false;
    m_loadedEnvFilePath.clear();
    m_provenance.clear();
    for (const SettingSpec& spec : kSpecs) {
        m_provenance[spec.key] = QStringLiteral("compiled-default");
    }
    m_serverUrl = QUrl(QStringLiteral("ws://localhost:8080"));
    m_remoteSessionHiddenTimeoutMs = 60000;
    m_projectMediaHiddenTimeoutMs = 60000;
    m_projectHiddenRetentionMs = 300000;
    m_incomingSessionOrphanTimeoutMs = 3000;
    m_uploadIdleTimeoutMs = 45000;
    m_connectionSyncTimeoutMs = 10000;
    m_sessionRetryBaseMs = 1000;
    m_sessionRetryMaxMs = 5000;
    m_controlRequestRetryMs = 1000;
    m_uploadChannelRetryBaseMs = 1000;
    m_uploadChannelRetryMaxMs = 5000;
    m_uploadChannelAttemptTimeoutMs = 10000;
    m_deferredCleanupRetryMaxMs = 30000;
    m_connectionAttemptTimeoutMs = 10000;
    m_reconnectStableResetMs = 30000;
    m_reconnectFastStepMs = 250;
    m_reconnectFastMaxMs = 750;
    m_reconnectBaseMs = 1000;
    m_reconnectMaxMs = 5000;
    m_reconnectJitterPercent = 20;
    m_leaseHealthCheckIntervalMs = 100;
    m_sessionDeadlinePollIntervalMs = 250;
    m_projectAutosaveDelayMs = 300;
    m_projectCheckpointIntervalMs = 15000;
    m_projectDeadlinePollIntervalMs = 250;
    m_screenChangeDebounceMs = 150;
    m_systemVolumePollIntervalMs = 1200;
    m_fileWatchDebounceMs = 500;
    m_sceneActivityRefreshIntervalMs = 1000;
    m_clientCountdownRefreshIntervalMs = 1000;
    m_videoStatePublishIntervalMs = 50;
    m_videoSnapshotIntervalMs = 1000;
    m_remoteWindowShowDelayMs = 10;
    m_mediaProbeTimeoutMs = 5000;
    m_incomingUploadCompletionTtlMs = 60000;
    m_deferredCleanupRetryMs = 1000;
    m_sceneLaunchTimeoutMarginMs = 1000;
    m_instanceActivationConnectTimeoutMs = 100;
    m_instanceActivationAckTimeoutMs = 250;
    m_instanceActivationRetryIntervalMs = 25;
    m_controlledDisconnectDrainTimeoutMs = 10000;
    m_processStopTimeoutMs = 100;
    m_uploadActionMinIntervalMs = 300;
    m_uploadCancelGuardMs = 1000;
    m_sceneSeekPositionToleranceMs = 120;
    m_sceneStartFrameToleranceMs = 25;
    m_sceneDecoderSyncToleranceMs = 25;
    m_sceneVideoSyncPositionToleranceMs = 400;
    m_sceneVideoSyncTransitMaxMs = 2000;
    m_sceneAuthoritativeSeekGuardMs = 250;
    m_sceneRepeatTriggerGuardMs = 500;
    m_uiContentFadeDurationMs = 200;
    m_uiSpinnerRotationDurationMs = 900;
    m_uiScrollbarHideDelayMs = 500;
    m_uiInputWatchdogIntervalMs = 120;
    m_uiSnapFreezeCleanupDelayMs = 300;
    m_toastDefaultDurationMs = 4000;
    m_toastInfoDurationMs = 2000;
    m_toastWarningDurationMs = 3500;
    m_toastErrorDurationMs = 5000;
    m_toastAnimationDurationMs = 300;
    m_mediaRamReservePercent = 0;
    m_timelineMaxDurationMs = 180000;
    m_timelineSlotsPerSecond = 30;
    m_timelineDefaultClipDurationSlots = 30;
    m_timelineHeightPx = 240;
    m_timelineRulerHeightPx = 28;
    m_timelineClipTrackHeightPx = 48;
    m_timelineMinTracksAbove = 10;
    m_timelineMinTracksBelow = 10;
    m_timelineKeyframeSizePx = 10;
    m_timelineOtherKeyframeOpacityPercent = 30;
    m_timelineSnapDistancePx = 10;
    m_timelineInitialViewDurationMs = 15000;
    m_canvasTextInitialHeightPercent = 8;
    m_remoteCursorDiameterPx = 30;
    m_mediaRamReserveMinMiB = 548;
    m_uploadConcurrency = 2;
    m_autoUploadImportedMedia = false;
    m_appAlwaysOnTop = true;
    m_qtMediaBackend = QStringLiteral("ffmpeg");
    m_allowMultipleInstances = false;
    m_cursorDebug = false;
    m_runtimeDiagnostics = false;
    m_canvasProfiling = false;
    m_remoteMediaRetentionMs = 600000;
    m_remoteMediaCacheMaxMiB = 10240;
    m_networkDiagnostics = true;
    m_networkDiagnosticsVerbose = false;
}

bool AppConfig::initialize(const QStringList& arguments, QString* errorMessage) {
    LoadOptions options;
    options.arguments = arguments;
    options.processEnvironment = QProcessEnvironment::systemEnvironment();

    const std::unique_ptr<QSettings> settings = RuntimeProfile::createSettings();
    for (const SettingSpec& spec : kSpecs) {
        if (!spec.settingsName) {
            continue;
        }
        const QString settingsName = QLatin1String(spec.settingsName);
        if (settings->contains(settingsName)) {
            options.settings.insert(settingsName, settings->value(settingsName));
        }
    }
    options.applyProductionOverride =
        QLatin1String(MOUFFETTE_BUILD_CHANNEL) == QStringLiteral("production");
    return load(options, errorMessage);
}

bool AppConfig::initializePreApplication(const QStringList& arguments,
                                         QString* errorMessage) {
    return initializeWithSettings(arguments, {}, errorMessage);
}

bool AppConfig::initializeWithSettings(const QStringList& arguments,
                                       const QVariantMap& settings,
                                       QString* errorMessage) {
    LoadOptions options;
    options.arguments = arguments;
    options.processEnvironment = QProcessEnvironment::systemEnvironment();
    options.settings = settings;
    options.applyProductionOverride =
        QLatin1String(MOUFFETTE_BUILD_CHANNEL) == QStringLiteral("production");
    return load(options, errorMessage);
}

bool AppConfig::load(const LoadOptions& options, QString* errorMessage) {
    ParsedArguments cli;
    if (!parseArguments(options.arguments, cli, errorMessage)) {
        return false;
    }

    QString envFilePath = options.defaultEnvFilePath;
    QString envFileSource = QStringLiteral("embedded-env:%1").arg(envFilePath);
    if (options.processEnvironment.contains(QStringLiteral("MOUFFETTE_ENV_FILE"))) {
        envFilePath = options.processEnvironment.value(QStringLiteral("MOUFFETTE_ENV_FILE")).trimmed();
        envFileSource = QStringLiteral("process:MOUFFETTE_ENV_FILE:%1").arg(envFilePath);
        if (envFilePath.isEmpty()) {
            return setError(errorMessage, QStringLiteral("MOUFFETTE_ENV_FILE must not be empty"));
        }
    }
    if (cli.envFileSpecified) {
        envFilePath = cli.envFile.trimmed();
        envFileSource = QStringLiteral("cli:--env-file:%1").arg(envFilePath);
    }

    RawValues rawValues;
    for (const SettingSpec& spec : kSpecs) {
        rawValues[spec.key] = {QString::fromLatin1(spec.defaultValue), QStringLiteral("compiled-default")};
    }

    if (!envFilePath.isEmpty()) {
        QMap<QString, QString> envFileValues;
        if (!readEnvFile(envFilePath, envFileValues, errorMessage)) {
            return false;
        }
        for (auto it = envFileValues.cbegin(); it != envFileValues.cend(); ++it) {
            const SettingSpec* spec = specForEnvName(it.key());
            rawValues[spec->key] = {it.value(), envFileSource};
        }
    }

    for (const SettingSpec& spec : kSpecs) {
        if (!spec.settingsName) {
            continue;
        }
        const QString settingsName = QLatin1String(spec.settingsName);
        if (options.settings.contains(settingsName)) {
            rawValues[spec.key] = {
                options.settings.value(settingsName).toString(),
                QStringLiteral("QSettings:%1").arg(settingsName)
            };
        }
    }

    for (const SettingSpec& spec : kSpecs) {
        const QString envName = QLatin1String(spec.envName);
        if (options.processEnvironment.contains(envName)) {
            rawValues[spec.key] = {
                options.processEnvironment.value(envName),
                QStringLiteral("process:%1").arg(envName)
            };
        }
    }
    for (const QString& envName : options.processEnvironment.keys()) {
        if (envName == QLatin1String("MOUFFETTE_ENV_FILE")) {
            continue;
        }
        if ((envName.startsWith(QStringLiteral("MOUFFETTE_"))
             || envName.startsWith(QStringLiteral("QT_")))
            && !specForEnvName(envName)) {
            qWarning().noquote() << QStringLiteral("Unknown client process config key '%1'")
                                        .arg(envName);
        }
    }

    for (auto it = cli.values.cbegin(); it != cli.values.cend(); ++it) {
        const SettingSpec* spec = nullptr;
        for (const SettingSpec& candidate : kSpecs) {
            if (candidate.key == it.key()) {
                spec = &candidate;
                break;
            }
        }
        rawValues[it.key()] = {
            it.value(),
            QStringLiteral("cli:--%1").arg(QLatin1String(spec->cliName))
        };
    }

    // Production policy is an explicit, key-wise final override. An empty
    // file changes nothing; a key present here cannot be weakened by runtime
    // settings, process variables or command-line flags.
    if (options.applyProductionOverride && !options.productionEnvFilePath.isEmpty()) {
        QMap<QString, QString> productionValues;
        if (!readEnvFile(options.productionEnvFilePath, productionValues, errorMessage)) {
            return false;
        }
        const QString source = QStringLiteral("production-override:%1")
                                   .arg(options.productionEnvFilePath);
        for (auto it = productionValues.cbegin(); it != productionValues.cend(); ++it) {
            const SettingSpec* spec = specForEnvName(it.key());
            rawValues[spec->key] = {it.value(), source};
        }
    }

    AppConfig candidate;
    candidate.m_loadedEnvFilePath = envFilePath;
    for (const auto& pair : rawValues) {
        candidate.m_provenance[pair.first] = pair.second.source;
    }

    if (!parseServerUrl(rawValues.at(Key::ServerUrl), candidate.m_serverUrl, errorMessage)) {
        return false;
    }
    if (!parseInteger(rawValues.at(Key::RemoteSessionHiddenTimeoutMs),
                      keyName(Key::RemoteSessionHiddenTimeoutMs), 1000, 86400000,
                      candidate.m_remoteSessionHiddenTimeoutMs, errorMessage)) {
        return false;
    }
    if (!parseInteger(rawValues.at(Key::ProjectMediaHiddenTimeoutMs),
                      keyName(Key::ProjectMediaHiddenTimeoutMs), 1000, 86400000,
                      candidate.m_projectMediaHiddenTimeoutMs, errorMessage)) {
        return false;
    }
    if (!parseInteger(rawValues.at(Key::ProjectHiddenRetentionMs),
                      keyName(Key::ProjectHiddenRetentionMs), 60000, 2592000000LL,
                      candidate.m_projectHiddenRetentionMs, errorMessage)) {
        return false;
    }
    if (candidate.m_projectHiddenRetentionMs <= candidate.m_remoteSessionHiddenTimeoutMs) {
        return setError(errorMessage,
                        QStringLiteral("MOUFFETTE_PROJECT_HIDDEN_RETENTION_MS must be greater than "
                                       "MOUFFETTE_REMOTE_SESSION_HIDDEN_TIMEOUT_MS"));
    }
    if (candidate.m_projectHiddenRetentionMs <= candidate.m_projectMediaHiddenTimeoutMs) {
        return setError(errorMessage,
                        QStringLiteral("MOUFFETTE_PROJECT_HIDDEN_RETENTION_MS must be greater than "
                                       "MOUFFETTE_PROJECT_MEDIA_HIDDEN_TIMEOUT_MS"));
    }
    if (!parseInteger(rawValues.at(Key::IncomingSessionOrphanTimeoutMs),
                      keyName(Key::IncomingSessionOrphanTimeoutMs), 1000, 86400000,
                      candidate.m_incomingSessionOrphanTimeoutMs, errorMessage)) {
        return false;
    }
    qint64 uploadIdleTimeoutMs = 0;
    if (!parseInteger(rawValues.at(Key::UploadIdleTimeoutMs),
                      keyName(Key::UploadIdleTimeoutMs), 5000, 600000,
                      uploadIdleTimeoutMs, errorMessage)) {
        return false;
    }
    candidate.m_uploadIdleTimeoutMs = static_cast<int>(uploadIdleTimeoutMs);

    const auto parseIntSetting = [&](Key key, qint64 minimum, qint64 maximum,
                                     int* destination) {
        qint64 parsed = 0;
        if (!parseInteger(rawValues.at(key), keyName(key), minimum, maximum,
                          parsed, errorMessage)) {
            return false;
        }
        *destination = static_cast<int>(parsed);
        return true;
    };
    if (!parseIntSetting(Key::RemoteMediaRetentionMs, 1000, 86400000, &candidate.m_remoteMediaRetentionMs)
        || !parseIntSetting(Key::RemoteMediaCacheMaxMiB, 1, 1048576, &candidate.m_remoteMediaCacheMaxMiB)
        || !parseIntSetting(Key::CanvasTextInitialHeightPercent, 1, 100,
                         &candidate.m_canvasTextInitialHeightPercent)
        || !parseIntSetting(Key::TimelineMaxDurationMs, 1, 604800000, &candidate.m_timelineMaxDurationMs)
        || !parseIntSetting(Key::TimelineSlotsPerSecond, 1, 240, &candidate.m_timelineSlotsPerSecond)
        || !parseIntSetting(Key::TimelineDefaultClipDurationSlots, 1, 145152000, &candidate.m_timelineDefaultClipDurationSlots)
        || !parseIntSetting(Key::TimelineHeightPx, 120, 1200, &candidate.m_timelineHeightPx)
        || !parseIntSetting(Key::TimelineRulerHeightPx, 16, 160, &candidate.m_timelineRulerHeightPx)
        || !parseIntSetting(Key::TimelineClipTrackHeightPx, 24, 600, &candidate.m_timelineClipTrackHeightPx)
        || !parseIntSetting(Key::TimelineMinTracksAbove, 0, 9999, &candidate.m_timelineMinTracksAbove)
        || !parseIntSetting(Key::TimelineMinTracksBelow, 0, 9999, &candidate.m_timelineMinTracksBelow)
        || !parseIntSetting(Key::TimelineKeyframeSizePx, 4, 64, &candidate.m_timelineKeyframeSizePx)
        || !parseIntSetting(Key::TimelineOtherKeyframeOpacityPercent, 0, 100, &candidate.m_timelineOtherKeyframeOpacityPercent)
        || !parseIntSetting(Key::TimelineSnapDistancePx, 0, 100, &candidate.m_timelineSnapDistancePx)
        || !parseIntSetting(Key::TimelineInitialViewDurationMs, 1, 604800000, &candidate.m_timelineInitialViewDurationMs)
        || !parseIntSetting(Key::RemoteCursorDiameterPx, 4, 256,
                            &candidate.m_remoteCursorDiameterPx)
        || !parseIntSetting(Key::MediaRamReservePercent, 0, 100,
                         &candidate.m_mediaRamReservePercent)
        || !parseIntSetting(Key::MediaRamReserveMinMiB, 0, 2147483647,
                            &candidate.m_mediaRamReserveMinMiB)
        || !parseIntSetting(Key::ConnectionSyncTimeoutMs, 1000, 120000, &candidate.m_connectionSyncTimeoutMs)
        || !parseIntSetting(Key::SessionRetryBaseMs, 50, 120000, &candidate.m_sessionRetryBaseMs)
        || !parseIntSetting(Key::SessionRetryMaxMs, 50, 600000, &candidate.m_sessionRetryMaxMs)
        || !parseIntSetting(Key::ControlRequestRetryMs, 50, 60000, &candidate.m_controlRequestRetryMs)
        || !parseIntSetting(Key::UploadChannelRetryBaseMs, 50, 120000, &candidate.m_uploadChannelRetryBaseMs)
        || !parseIntSetting(Key::UploadChannelRetryMaxMs, 50, 600000, &candidate.m_uploadChannelRetryMaxMs)
        || !parseIntSetting(Key::UploadChannelAttemptTimeoutMs, 1000, 120000, &candidate.m_uploadChannelAttemptTimeoutMs)
        || !parseIntSetting(Key::DeferredCleanupRetryMaxMs, 100, 600000, &candidate.m_deferredCleanupRetryMaxMs)
        || !parseIntSetting(Key::ConnectionAttemptTimeoutMs, 250, 120000,
                         &candidate.m_connectionAttemptTimeoutMs)
        || !parseIntSetting(Key::ReconnectFastStepMs, 10, 10000,
                            &candidate.m_reconnectFastStepMs)
        || !parseIntSetting(Key::ReconnectFastMaxMs, 10, 30000,
                            &candidate.m_reconnectFastMaxMs)
        || !parseIntSetting(Key::ReconnectBaseMs, 50, 120000,
                            &candidate.m_reconnectBaseMs)
        || !parseIntSetting(Key::ReconnectMaxMs, 50, 600000,
                            &candidate.m_reconnectMaxMs)
        || !parseIntSetting(Key::ReconnectStableResetMs, 1000, 600000,
                            &candidate.m_reconnectStableResetMs)
        || !parseIntSetting(Key::ReconnectJitterPercent, 0, 50,
                            &candidate.m_reconnectJitterPercent)
        || !parseIntSetting(Key::LeaseHealthCheckIntervalMs, 10, 5000,
                            &candidate.m_leaseHealthCheckIntervalMs)
        || !parseIntSetting(Key::SessionDeadlinePollIntervalMs, 25, 5000,
                            &candidate.m_sessionDeadlinePollIntervalMs)
        || !parseIntSetting(Key::ProjectAutosaveDelayMs, 0, 60000,
                            &candidate.m_projectAutosaveDelayMs)
        || !parseIntSetting(Key::ProjectCheckpointIntervalMs, 1000, 3600000,
                            &candidate.m_projectCheckpointIntervalMs)
        || !parseIntSetting(Key::ProjectDeadlinePollIntervalMs, 25, 5000,
                            &candidate.m_projectDeadlinePollIntervalMs)
        || !parseIntSetting(Key::ScreenChangeDebounceMs, 10, 5000,
                            &candidate.m_screenChangeDebounceMs)
        || !parseIntSetting(Key::SystemVolumePollIntervalMs, 100, 60000,
                            &candidate.m_systemVolumePollIntervalMs)
        || !parseIntSetting(Key::FileWatchDebounceMs, 10, 10000,
                            &candidate.m_fileWatchDebounceMs)
        || !parseIntSetting(Key::SceneActivityRefreshIntervalMs, 100, 10000,
                            &candidate.m_sceneActivityRefreshIntervalMs)
        || !parseIntSetting(Key::ClientCountdownRefreshIntervalMs, 100, 5000,
                            &candidate.m_clientCountdownRefreshIntervalMs)
        || !parseIntSetting(Key::VideoStatePublishIntervalMs, 10, 1000,
                            &candidate.m_videoStatePublishIntervalMs)
        || !parseIntSetting(Key::VideoSnapshotIntervalMs, 50, 10000,
                            &candidate.m_videoSnapshotIntervalMs)
        || !parseIntSetting(Key::RemoteWindowShowDelayMs, 1, 1000,
                            &candidate.m_remoteWindowShowDelayMs)
        || !parseIntSetting(Key::MediaProbeTimeoutMs, 500, 120000,
                            &candidate.m_mediaProbeTimeoutMs)
        || !parseIntSetting(Key::IncomingUploadCompletionTtlMs, 1000, 3600000,
                            &candidate.m_incomingUploadCompletionTtlMs)
        || !parseIntSetting(Key::DeferredCleanupRetryMs, 100, 60000,
                            &candidate.m_deferredCleanupRetryMs)
        || !parseIntSetting(Key::SceneLaunchTimeoutMarginMs, 0, 30000,
                            &candidate.m_sceneLaunchTimeoutMarginMs)
        || !parseIntSetting(Key::InstanceActivationConnectTimeoutMs, 10, 5000,
                            &candidate.m_instanceActivationConnectTimeoutMs)
        || !parseIntSetting(Key::InstanceActivationAckTimeoutMs, 10, 10000,
                            &candidate.m_instanceActivationAckTimeoutMs)
        || !parseIntSetting(Key::InstanceActivationRetryIntervalMs, 1, 1000,
                            &candidate.m_instanceActivationRetryIntervalMs)
        || !parseIntSetting(Key::ControlledDisconnectDrainTimeoutMs, 1000, 120000,
                            &candidate.m_controlledDisconnectDrainTimeoutMs)
        || !parseIntSetting(Key::ProcessStopTimeoutMs, 0, 10000,
                            &candidate.m_processStopTimeoutMs)
        || !parseIntSetting(Key::UploadActionMinIntervalMs, 0, 10000,
                            &candidate.m_uploadActionMinIntervalMs)
        || !parseIntSetting(Key::UploadCancelGuardMs, 0, 30000,
                            &candidate.m_uploadCancelGuardMs)
        || !parseIntSetting(Key::SceneSeekPositionToleranceMs, 0, 5000,
                            &candidate.m_sceneSeekPositionToleranceMs)
        || !parseIntSetting(Key::SceneStartFrameToleranceMs, 0, 5000,
                            &candidate.m_sceneStartFrameToleranceMs)
        || !parseIntSetting(Key::SceneDecoderSyncToleranceMs, 0, 5000,
                            &candidate.m_sceneDecoderSyncToleranceMs)
        || !parseIntSetting(Key::SceneVideoSyncPositionToleranceMs, 0, 10000,
                            &candidate.m_sceneVideoSyncPositionToleranceMs)
        || !parseIntSetting(Key::SceneVideoSyncTransitMaxMs, 0, 30000,
                            &candidate.m_sceneVideoSyncTransitMaxMs)
        || !parseIntSetting(Key::SceneAuthoritativeSeekGuardMs, 0, 10000,
                            &candidate.m_sceneAuthoritativeSeekGuardMs)
        || !parseIntSetting(Key::SceneRepeatTriggerGuardMs, 0, 10000,
                            &candidate.m_sceneRepeatTriggerGuardMs)
        || !parseIntSetting(Key::UiContentFadeDurationMs, 0, 5000,
                            &candidate.m_uiContentFadeDurationMs)
        || !parseIntSetting(Key::UiSpinnerRotationDurationMs, 100, 10000,
                            &candidate.m_uiSpinnerRotationDurationMs)
        || !parseIntSetting(Key::UiScrollbarHideDelayMs, 0, 10000,
                            &candidate.m_uiScrollbarHideDelayMs)
        || !parseIntSetting(Key::UiInputWatchdogIntervalMs, 25, 5000,
                            &candidate.m_uiInputWatchdogIntervalMs)
        || !parseIntSetting(Key::UiSnapFreezeCleanupDelayMs, 25, 10000,
                            &candidate.m_uiSnapFreezeCleanupDelayMs)
        || !parseIntSetting(Key::ToastDefaultDurationMs, 100, 60000,
                            &candidate.m_toastDefaultDurationMs)
        || !parseIntSetting(Key::ToastInfoDurationMs, 100, 60000,
                            &candidate.m_toastInfoDurationMs)
        || !parseIntSetting(Key::ToastWarningDurationMs, 100, 60000,
                            &candidate.m_toastWarningDurationMs)
        || !parseIntSetting(Key::ToastErrorDurationMs, 100, 60000,
                            &candidate.m_toastErrorDurationMs)
        || !parseIntSetting(Key::ToastAnimationDurationMs, 0, 5000,
                            &candidate.m_toastAnimationDurationMs)) {
        return false;
    }
    if (qint64(candidate.m_timelineMaxDurationMs) * candidate.m_timelineSlotsPerSecond < 1000) {
        return setError(errorMessage, QStringLiteral("Timeline maximum duration must contain at least one complete slot"));
    }
    if (candidate.m_sessionRetryMaxMs < candidate.m_sessionRetryBaseMs
        || candidate.m_uploadChannelRetryMaxMs < candidate.m_uploadChannelRetryBaseMs
        || candidate.m_deferredCleanupRetryMaxMs < candidate.m_deferredCleanupRetryMs
        || candidate.m_reconnectFastMaxMs < candidate.m_reconnectFastStepMs
        || candidate.m_reconnectMaxMs < candidate.m_reconnectBaseMs) {
        return setError(errorMessage,
                        QStringLiteral("Reconnect maximum delays must be greater than or equal to their base delays"));
    }

    qint64 uploadConcurrency = 0;
    if (!parseInteger(rawValues.at(Key::UploadConcurrency), keyName(Key::UploadConcurrency),
                      1, 2, uploadConcurrency, errorMessage)) {
        return false;
    }
    candidate.m_uploadConcurrency = static_cast<int>(uploadConcurrency);

    if (!parseBoolean(rawValues.at(Key::NetworkDiagnostics), keyName(Key::NetworkDiagnostics), candidate.m_networkDiagnostics, errorMessage)
        || !parseBoolean(rawValues.at(Key::NetworkDiagnosticsVerbose), keyName(Key::NetworkDiagnosticsVerbose), candidate.m_networkDiagnosticsVerbose, errorMessage)
        || !parseBoolean(rawValues.at(Key::AutoUploadImportedMedia),
                      keyName(Key::AutoUploadImportedMedia),
                      candidate.m_autoUploadImportedMedia, errorMessage)
        || !parseBoolean(rawValues.at(Key::AppAlwaysOnTop),
                         keyName(Key::AppAlwaysOnTop),
                         candidate.m_appAlwaysOnTop, errorMessage)
        || !parseBoolean(rawValues.at(Key::AllowMultipleInstances),
                         keyName(Key::AllowMultipleInstances),
                         candidate.m_allowMultipleInstances, errorMessage)
        || !parseBoolean(rawValues.at(Key::CursorDebug), keyName(Key::CursorDebug),
                         candidate.m_cursorDebug, errorMessage)
        || !parseBoolean(rawValues.at(Key::RuntimeDiagnostics), keyName(Key::RuntimeDiagnostics),
                         candidate.m_runtimeDiagnostics, errorMessage)
        || !parseBoolean(rawValues.at(Key::CanvasProfiling), keyName(Key::CanvasProfiling),
                         candidate.m_canvasProfiling, errorMessage)) {
        return false;
    }

    candidate.m_qtMediaBackend = rawValues.at(Key::QtMediaBackend).value.trimmed();
    static const QRegularExpression backendPattern(QStringLiteral("^[A-Za-z0-9_-]{1,64}$"));
    if (!backendPattern.match(candidate.m_qtMediaBackend).hasMatch()) {
        return setError(errorMessage, QStringLiteral("QT_MEDIA_BACKEND from %1 is invalid")
                                          .arg(rawValues.at(Key::QtMediaBackend).source));
    }

    candidate.m_loaded = true;
    *this = candidate;
    if (errorMessage) {
        errorMessage->clear();
    }
    return true;
}

void AppConfig::applyPreApplicationEnvironment() const {
    qputenv("QT_MEDIA_BACKEND", m_qtMediaBackend.toUtf8());
}

QString AppConfig::provenance(Key key) const {
    const auto it = m_provenance.find(key);
    return it == m_provenance.end() ? QStringLiteral("unknown") : it->second;
}
