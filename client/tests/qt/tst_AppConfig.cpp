#include <QtTest>

#include "backend/config/AppConfig.h"

#include <QFile>
#include <QTemporaryDir>

namespace {

QString writeEnvFile(QTemporaryDir& directory, const QString& name, const QByteArray& contents) {
    const QString path = directory.filePath(name);
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        return QString();
    }
    if (file.write(contents) != contents.size()) {
        return QString();
    }
    file.close();
    return path;
}

AppConfig::LoadOptions isolatedOptions(const QString& envPath) {
    AppConfig::LoadOptions options;
    options.arguments = {QStringLiteral("tst_AppConfig")};
    options.processEnvironment = QProcessEnvironment();
    options.defaultEnvFilePath = envPath;
    return options;
}

} // namespace

class AppConfigTest final : public QObject {
    Q_OBJECT

private slots:
    void loadsEmbeddedDefaults();
    void screenPreviewDefaults();
    void proxySettingsAreValidatedAndCredentialsStayPrivate();
    void screenPreviewSourcesAndReload();
    void screenPreviewRejectsInvalidSettings();
    void screenPreviewValidatesRelatedSettings();
    void validatesRetentionAndDiagnostics();
    void configuresTimeline();
    void configuresTimelineClipResizeZones();
    void configuresMinimumTimelineTracks();
    void appAlwaysOnTopIsOptionalAndPersistent();
    void configuresMediaRamReserve();
    void configuresProjectMediaHiddenTimeout();
    void configuresCanvasTextInitialHeight();
    void compiledDefaultDisablesMultipleInstances();
    void appliesDocumentedPrecedence();
    void commandLineEnvFileReplacesProcessSelection();
    void parsesFalseBooleansAsFalse();
    void appliesProductionOverrideLastPerKey();
    void emptyProductionOverrideChangesNothing();
    void acceptsPublicPlainWebSocketUrl();
    void acceptsBothWebSocketSchemesForRuntimeUrlEdits();
    void rejectsInvalidHiddenDeadlineOrdering();
    void validatesIncomingSessionOrphanTimeoutFromCli();
    void warnsAndIgnoresUnknownNamespacedEnvKey();
};

void AppConfigTest::proxySettingsAreValidatedAndCredentialsStayPrivate() {
    AppConfig config;
    auto options = isolatedOptions(QString());
    QString error;
    QVERIFY2(config.load(options, &error), qPrintable(error));
    QCOMPARE(config.proxyType(), QStringLiteral("system"));
    QVERIFY(config.proxyHost().isEmpty());
    QCOMPARE(config.proxyPort(), 8080);
    QVERIFY(config.proxyUser().isEmpty());
    QVERIFY(config.proxyPassword().isEmpty());
    QVERIFY(AppConfig::isSensitive(AppConfig::Key::ProxyUser));
    QVERIFY(AppConfig::isSensitive(AppConfig::Key::ProxyPassword));
    QVERIFY(!AppConfig::isSensitive(AppConfig::Key::ProxyType));
    QTemporaryDir directory;
    options.defaultEnvFilePath = writeEnvFile(directory, "proxy.env",
        "MOUFFETTE_PROXY_TYPE=http\n"
        "MOUFFETTE_PROXY_HOST=secureproxy.example\n"
        "MOUFFETTE_PROXY_PORT=3128\n"
        "MOUFFETTE_PROXY_USER=example-user\n"
        "MOUFFETTE_PROXY_PASSWORD='  private fixture value  '\n");
    QVERIFY(!options.defaultEnvFilePath.isEmpty());
    QVERIFY2(config.load(options, &error), qPrintable(error));
    QCOMPARE(config.proxyType(), QStringLiteral("http"));
    QCOMPARE(config.proxyHost(), QStringLiteral("secureproxy.example"));
    QCOMPARE(config.proxyPort(), 3128);
    QCOMPARE(config.proxyPassword(), QStringLiteral("  private fixture value  "));
    options.processEnvironment.insert("MOUFFETTE_PROXY_PASSWORD", "process fixture");
    QVERIFY2(config.load(options, &error), qPrintable(error));
    QCOMPARE(config.proxyPassword(), QStringLiteral("process fixture"));
    QCOMPARE(config.provenance(AppConfig::Key::ProxyPassword), QStringLiteral("process:MOUFFETTE_PROXY_PASSWORD"));
    const struct { const char* key; const char* value; } invalid[] = {
        {"MOUFFETTE_PROXY_TYPE", "https"}, {"MOUFFETTE_PROXY_TYPE", "system"},
        {"MOUFFETTE_PROXY_TYPE", "none"}, {"MOUFFETTE_PROXY_HOST", ""},
        {"MOUFFETTE_PROXY_HOST", "http://secureproxy.example"},
        {"MOUFFETTE_PROXY_HOST", "fixture-user:fixture-password@secureproxy.example"},
        {"MOUFFETTE_PROXY_HOST", "proxy example"}, {"MOUFFETTE_PROXY_PORT", "0"},
        {"MOUFFETTE_PROXY_PORT", "65536"}, {"MOUFFETTE_PROXY_PORT", "abc"},
        {"MOUFFETTE_PROXY_USER", ""}, {"MOUFFETTE_PROXY_USER", "sensitive-user\n"},
        {"MOUFFETTE_PROXY_PASSWORD", "sensitive-password\n"}
    };
    for (const auto& entry : invalid) {
        auto invalidOptions = options;
        invalidOptions.processEnvironment.insert(entry.key, entry.value);
        QVERIFY2(!config.load(invalidOptions, &error), entry.key);
        QVERIFY(error.contains("MOUFFETTE_PROXY_"));
        QVERIFY(!error.contains("fixture-password"));
        QVERIFY(!error.contains("sensitive-user"));
        QVERIFY(!error.contains("sensitive-password"));
        QCOMPARE(config.proxyHost(), QStringLiteral("secureproxy.example"));
        QCOMPARE(config.proxyPassword(), QStringLiteral("process fixture"));
    }
    options.processEnvironment.insert("MOUFFETTE_PROXY_TYPE", "socks5");
    options.processEnvironment.insert("MOUFFETTE_PROXY_HOST", "2001:db8::1");
    options.processEnvironment.insert("MOUFFETTE_PROXY_PORT", "1080");
    options.processEnvironment.insert("MOUFFETTE_PROXY_PASSWORD", QString(128, QChar(0xe9)));
    QVERIFY(!config.load(options, &error)); // 256 UTF-8 bytes exceeds RFC 1929.
    QVERIFY(error.contains("255 UTF-8 bytes"));
    options.processEnvironment.insert("MOUFFETTE_PROXY_PASSWORD", QString(127, QChar(0xe9)));
    QVERIFY2(config.load(options, &error), qPrintable(error));
    QCOMPARE(config.proxyType(), QStringLiteral("socks5"));
    QCOMPARE(config.proxyHost(), QStringLiteral("2001:db8::1"));
    QCOMPARE(config.proxyPort(), 1080);
    QVERIFY2(config.load(isolatedOptions(QString()), &error), qPrintable(error));
    QCOMPARE(config.proxyType(), QStringLiteral("system"));
    QVERIFY(config.proxyPassword().isEmpty());
}

void AppConfigTest::screenPreviewDefaults() {
    AppConfig config;
    const auto verifyDefaults = [](const AppConfig& value) {
        QVERIFY(value.screenAdaptiveEnabled());
        QCOMPARE(value.screenMaxEdge(), 3840);
        QCOMPARE(value.screenMaxFps(), 30);
        QCOMPARE(value.screenIdleIntervalMs(), 1000);
        QCOMPARE(value.screenMinBitrateKbps(), 128);
        QCOMPARE(value.screenInitialBitrateKbps(), 1200);
        QCOMPARE(value.screenMaxBitrateKbps(), 12000);
        QCOMPARE(value.screenUploadBitrateKbps(), 1000);
        QCOMPARE(value.screenFeedbackIntervalMs(), 500);
        QCOMPARE(value.screenRecoveryHoldMs(), 5000);
        QCOMPARE(value.screenQueueTargetMs(), 150);
        QCOMPARE(value.screenAckTimeoutMs(), 3000);
        QCOMPARE(value.screenKeyframeIntervalMs(), 4000);
        QCOMPARE(value.screenMaxBufferedKiB(), 2048);
        QCOMPARE(value.screenMaxInflightFrames(), 64);
        QCOMPARE(value.screenDecodeQueueMs(), 200);
        QCOMPARE(value.screenViewportDebounceMs(), 200);
        QCOMPARE(value.screenViewportOversamplePercent(), 125);
        QCOMPARE(value.screenRetryInitialMs(), 500);
        QCOMPARE(value.screenRetryMaxMs(), 10000);
        QCOMPARE(value.screenFirstFrameTimeoutMs(), 15000);
        QCOMPARE(value.screenStaleTimeoutMs(), 8000);
        QCOMPARE(value.screenSoftwarePreset(), QStringLiteral("veryfast"));
    };
    verifyDefaults(config);
    QString error;
    QVERIFY2(config.load(isolatedOptions(":/config/client.env"), &error), qPrintable(error));
    verifyDefaults(config);
    QVERIFY(config.provenance(AppConfig::Key::ScreenMaxBitrateKbps).startsWith("embedded-env:"));
}

void AppConfigTest::screenPreviewSourcesAndReload() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString path = writeEnvFile(directory, "screen.env",
        "MOUFFETTE_SCREEN_ADAPTIVE_ENABLED=false\n"
        "MOUFFETTE_SCREEN_MAX_EDGE=2560\n"
        "MOUFFETTE_SCREEN_MAX_FPS=60\n"
        "MOUFFETTE_SCREEN_IDLE_INTERVAL_MS=1500\n"
        "MOUFFETTE_SCREEN_MIN_BITRATE_KBPS=64\n"
        "MOUFFETTE_SCREEN_INITIAL_BITRATE_KBPS=2000\n"
        "MOUFFETTE_SCREEN_MAX_BITRATE_KBPS=20000\n"
        "MOUFFETTE_SCREEN_UPLOAD_BITRATE_KBPS=500\n"
        "MOUFFETTE_SCREEN_FEEDBACK_INTERVAL_MS=250\n"
        "MOUFFETTE_SCREEN_RECOVERY_HOLD_MS=6000\n"
        "MOUFFETTE_SCREEN_QUEUE_TARGET_MS=200\n"
        "MOUFFETTE_SCREEN_ACK_TIMEOUT_MS=4000\n"
        "MOUFFETTE_SCREEN_KEYFRAME_INTERVAL_MS=5000\n"
        "MOUFFETTE_SCREEN_MAX_BUFFERED_KIB=512\n"
        "MOUFFETTE_SCREEN_MAX_INFLIGHT_FRAMES=8\n"
        "MOUFFETTE_SCREEN_DECODE_QUEUE_MS=300\n"
        "MOUFFETTE_SCREEN_VIEWPORT_DEBOUNCE_MS=400\n"
        "MOUFFETTE_SCREEN_VIEWPORT_OVERSAMPLE_PERCENT=150\n"
        "MOUFFETTE_SCREEN_RETRY_INITIAL_MS=1000\n"
        "MOUFFETTE_SCREEN_RETRY_MAX_MS=20000\n"
        "MOUFFETTE_SCREEN_FIRST_FRAME_TIMEOUT_MS=20000\n"
        "MOUFFETTE_SCREEN_STALE_TIMEOUT_MS=10000\n"
        "MOUFFETTE_SCREEN_SOFTWARE_PRESET=fast\n");
    QVERIFY(!path.isEmpty());
    auto options = isolatedOptions(QString());
    options.processEnvironment.insert("MOUFFETTE_ENV_FILE", path);
    AppConfig config;
    QString error;
    QVERIFY2(config.load(options, &error), qPrintable(error));
    QVERIFY(!config.screenAdaptiveEnabled());
    QCOMPARE(config.screenMaxEdge(), 2560);
    QCOMPARE(config.screenMaxFps(), 60);
    QCOMPARE(config.screenIdleIntervalMs(), 1500);
    QCOMPARE(config.screenMinBitrateKbps(), 64);
    QCOMPARE(config.screenInitialBitrateKbps(), 2000);
    QCOMPARE(config.screenMaxBitrateKbps(), 20000);
    QCOMPARE(config.screenUploadBitrateKbps(), 500);
    QCOMPARE(config.screenFeedbackIntervalMs(), 250);
    QCOMPARE(config.screenRecoveryHoldMs(), 6000);
    QCOMPARE(config.screenQueueTargetMs(), 200);
    QCOMPARE(config.screenAckTimeoutMs(), 4000);
    QCOMPARE(config.screenKeyframeIntervalMs(), 5000);
    QCOMPARE(config.screenMaxBufferedKiB(), 512);
    QCOMPARE(config.screenMaxInflightFrames(), 8);
    QCOMPARE(config.screenDecodeQueueMs(), 300);
    QCOMPARE(config.screenViewportDebounceMs(), 400);
    QCOMPARE(config.screenViewportOversamplePercent(), 150);
    QCOMPARE(config.screenRetryInitialMs(), 1000);
    QCOMPARE(config.screenRetryMaxMs(), 20000);
    QCOMPARE(config.screenFirstFrameTimeoutMs(), 20000);
    QCOMPARE(config.screenStaleTimeoutMs(), 10000);
    QCOMPARE(config.screenSoftwarePreset(), QStringLiteral("fast"));
    QCOMPARE(config.loadedEnvFilePath(), path);
    QCOMPARE(config.provenance(AppConfig::Key::ScreenMaxFps), "process:MOUFFETTE_ENV_FILE:" + path);

    options.processEnvironment.insert("MOUFFETTE_SCREEN_MAX_FPS", "24");
    options.processEnvironment.insert("MOUFFETTE_SCREEN_ADAPTIVE_ENABLED", "true");
    QVERIFY2(config.load(options, &error), qPrintable(error));
    QCOMPARE(config.screenMaxFps(), 24);
    QVERIFY(config.screenAdaptiveEnabled());
    QCOMPARE(config.provenance(AppConfig::Key::ScreenMaxFps), QStringLiteral("process:MOUFFETTE_SCREEN_MAX_FPS"));
    options.arguments << "--screen-max-fps=15" << "--screen-adaptive-enabled=false";
    QVERIFY2(config.load(options, &error), qPrintable(error));
    QCOMPARE(config.screenMaxFps(), 15);
    QVERIFY(!config.screenAdaptiveEnabled());
    QCOMPARE(config.provenance(AppConfig::Key::ScreenMaxFps), QStringLiteral("cli:--screen-max-fps"));
    options.productionEnvFilePath = writeEnvFile(directory, "production.env", "MOUFFETTE_SCREEN_MAX_FPS=12\n");
    options.applyProductionOverride = true;
    QVERIFY2(config.load(options, &error), qPrintable(error));
    QCOMPARE(config.screenMaxFps(), 12);
    QCOMPARE(config.provenance(AppConfig::Key::ScreenMaxFps), "production-override:" + options.productionEnvFilePath);

    // External files are read again on explicit load; omitted settings reset to defaults.
    QCOMPARE(writeEnvFile(directory, "screen.env", "MOUFFETTE_SCREEN_MAX_FPS=10\n"), path);
    options = isolatedOptions(path);
    QVERIFY2(config.load(options, &error), qPrintable(error));
    QCOMPARE(config.screenMaxFps(), 10);
    QCOMPARE(config.screenMaxEdge(), 3840);
    QCOMPARE(config.screenMaxBitrateKbps(), 12000);
    QCOMPARE(config.screenSoftwarePreset(), QStringLiteral("veryfast"));
    QVERIFY(config.screenAdaptiveEnabled());
    options.defaultEnvFilePath.clear();
    QVERIFY2(config.load(options, &error), qPrintable(error));
    QCOMPARE(config.screenMaxFps(), 30);
    QCOMPARE(config.provenance(AppConfig::Key::ScreenMaxFps), QStringLiteral("compiled-default"));
}

void AppConfigTest::screenPreviewRejectsInvalidSettings() {
    const struct { const char* name; int minimum; int maximum; } limits[] = {
        {"MAX_EDGE", 320, 3840}, {"MAX_FPS", 1, 60}, {"IDLE_INTERVAL_MS", 250, 4000},
        {"MIN_BITRATE_KBPS", 32, 10000}, {"INITIAL_BITRATE_KBPS", 32, 100000},
        {"MAX_BITRATE_KBPS", 32, 100000}, {"UPLOAD_BITRATE_KBPS", 32, 100000},
        {"FEEDBACK_INTERVAL_MS", 100, 2000}, {"RECOVERY_HOLD_MS", 1000, 60000},
        {"QUEUE_TARGET_MS", 50, 1000}, {"ACK_TIMEOUT_MS", 1000, 15000},
        {"KEYFRAME_INTERVAL_MS", 500, 10000}, {"MAX_BUFFERED_KIB", 32, 8192},
        {"MAX_INFLIGHT_FRAMES", 1, 256}, {"DECODE_QUEUE_MS", 50, 2000},
        {"VIEWPORT_DEBOUNCE_MS", 50, 2000}, {"VIEWPORT_OVERSAMPLE_PERCENT", 100, 200},
        {"RETRY_INITIAL_MS", 100, 10000}, {"RETRY_MAX_MS", 100, 60000},
        {"FIRST_FRAME_TIMEOUT_MS", 1000, 60000}, {"STALE_TIMEOUT_MS", 2000, 60000}
    };
    AppConfig config;
    QString error;
    auto options = isolatedOptions(QString());
    options.arguments << "--screen-max-edge=1280";
    QVERIFY2(config.load(options, &error), qPrintable(error));
    const QString validSource = config.provenance(AppConfig::Key::ScreenMaxEdge);
    options.arguments.removeLast();
    for (const auto& limit : limits) {
        const QString key = "MOUFFETTE_SCREEN_" + QString::fromLatin1(limit.name);
        for (const QString& value : {QString::number(limit.minimum - 1), QString::number(limit.maximum + 1),
                                     QStringLiteral("1.5"), QStringLiteral("abc"), QStringLiteral("999999999999999999999")}) {
            options.processEnvironment.insert(key, value);
            QVERIFY2(!config.load(options, &error), qPrintable(key + "=" + value));
            QVERIFY2(error.contains(key), qPrintable(error));
            QCOMPARE(config.screenMaxEdge(), 1280); // Failed reload must remain atomic.
            QCOMPARE(config.provenance(AppConfig::Key::ScreenMaxEdge), validSource);
            QVERIFY(config.isLoaded());
        }
        options.processEnvironment.remove(key);
    }
    options.processEnvironment.insert("MOUFFETTE_SCREEN_ADAPTIVE_ENABLED", "sometimes");
    QVERIFY(!config.load(options, &error));
    QVERIFY(error.contains("MOUFFETTE_SCREEN_ADAPTIVE_ENABLED"));
    options.processEnvironment.remove("MOUFFETTE_SCREEN_ADAPTIVE_ENABLED");
    options.processEnvironment.insert("MOUFFETTE_SCREEN_MAX_EDGE", "1281");
    QVERIFY(!config.load(options, &error));
    QVERIFY(error.contains("MOUFFETTE_SCREEN_MAX_EDGE"));
    options.processEnvironment.remove("MOUFFETTE_SCREEN_MAX_EDGE");
    for (const QString& preset : {QStringLiteral(""), QStringLiteral("slow"), QStringLiteral("FAST"), QStringLiteral("fast;anything")}) {
        options.processEnvironment.insert("MOUFFETTE_SCREEN_SOFTWARE_PRESET", preset);
        QVERIFY(!config.load(options, &error));
        QVERIFY(error.contains("MOUFFETTE_SCREEN_SOFTWARE_PRESET"));
    }
    for (const QString& preset : {QStringLiteral("ultrafast"), QStringLiteral("superfast"), QStringLiteral("veryfast"),
                                   QStringLiteral("faster"), QStringLiteral("fast")}) {
        options.processEnvironment.insert("MOUFFETTE_SCREEN_SOFTWARE_PRESET", preset);
        QVERIFY2(config.load(options, &error), qPrintable(error));
        QCOMPARE(config.screenSoftwarePreset(), preset);
    }
}

void AppConfigTest::screenPreviewValidatesRelatedSettings() {
    const struct { const char* env; const char* expectedKey; } invalid[] = {
        {"MOUFFETTE_SCREEN_MIN_BITRATE_KBPS=1300\n", "INITIAL_BITRATE_KBPS"},
        {"MOUFFETTE_SCREEN_MAX_BITRATE_KBPS=1100\n", "INITIAL_BITRATE_KBPS"},
        {"MOUFFETTE_SCREEN_UPLOAD_BITRATE_KBPS=64\n", "UPLOAD_BITRATE_KBPS"},
        {"MOUFFETTE_SCREEN_UPLOAD_BITRATE_KBPS=13000\n", "UPLOAD_BITRATE_KBPS"},
        {"MOUFFETTE_SCREEN_QUEUE_TARGET_MS=1000\nMOUFFETTE_SCREEN_ACK_TIMEOUT_MS=1000\n", "QUEUE_TARGET_MS"},
        {"MOUFFETTE_SCREEN_RETRY_INITIAL_MS=600\nMOUFFETTE_SCREEN_RETRY_MAX_MS=500\n", "RETRY_INITIAL_MS"},
        {"MOUFFETTE_SCREEN_IDLE_INTERVAL_MS=2000\nMOUFFETTE_SCREEN_STALE_TIMEOUT_MS=3999\n", "STALE_TIMEOUT_MS"}
    };
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    AppConfig config;
    QString error;
    for (const auto& entry : invalid) {
        const auto path = writeEnvFile(directory, "invalid-screen.env", entry.env);
        QVERIFY(!path.isEmpty());
        QVERIFY(!config.load(isolatedOptions(path), &error));
        QVERIFY2(error.contains(QString::fromLatin1(entry.expectedKey)), qPrintable(error));
        QCOMPARE(config.screenInitialBitrateKbps(), 1200);
        QVERIFY(!config.isLoaded());
    }
    // Equality is valid for bitrate budgets, retry bounds and the freshness margin.
    const auto path = writeEnvFile(directory, "boundary-screen.env",
        "MOUFFETTE_SCREEN_MAX_BUFFERED_KIB=8192\n"
        "MOUFFETTE_SCREEN_MAX_INFLIGHT_FRAMES=256\n"
        "MOUFFETTE_SCREEN_MIN_BITRATE_KBPS=32\n"
        "MOUFFETTE_SCREEN_INITIAL_BITRATE_KBPS=32\n"
        "MOUFFETTE_SCREEN_MAX_BITRATE_KBPS=32\n"
        "MOUFFETTE_SCREEN_UPLOAD_BITRATE_KBPS=32\n"
        "MOUFFETTE_SCREEN_QUEUE_TARGET_MS=999\n"
        "MOUFFETTE_SCREEN_ACK_TIMEOUT_MS=1000\n"
        "MOUFFETTE_SCREEN_RETRY_INITIAL_MS=100\n"
        "MOUFFETTE_SCREEN_RETRY_MAX_MS=100\n"
        "MOUFFETTE_SCREEN_IDLE_INTERVAL_MS=4000\n"
        "MOUFFETTE_SCREEN_STALE_TIMEOUT_MS=8000\n");
    QVERIFY(!path.isEmpty());
    QVERIFY2(config.load(isolatedOptions(path), &error), qPrintable(error));
    QCOMPARE(config.screenMaxBitrateKbps(), 32);
    QCOMPARE(config.screenMaxBufferedKiB(), 8192);
    QCOMPARE(config.screenMaxInflightFrames(), 256);
    QCOMPARE(config.screenRetryMaxMs(), 100);
    QCOMPARE(config.screenIdleIntervalMs() * 2, config.screenStaleTimeoutMs());
}

void AppConfigTest::validatesRetentionAndDiagnostics() {
    QTemporaryDir directory;
    auto options = isolatedOptions(writeEnvFile(directory, "network.env", ""));
    AppConfig config;
    QString error;
    QVERIFY(config.load(options, &error));
    QCOMPARE(config.remoteMediaRetentionMs(), 600000);
    QCOMPARE(config.remoteMediaCacheMaxMiB(), 10240);
    QVERIFY(config.networkDiagnostics());
    QVERIFY(!config.networkDiagnosticsVerbose());
    options.processEnvironment.insert("MOUFFETTE_REMOTE_MEDIA_RETENTION_MS", "120000");
    options.processEnvironment.insert("MOUFFETTE_REMOTE_MEDIA_CACHE_MAX_MIB", "256");
    options.processEnvironment.insert("MOUFFETTE_NETWORK_DIAGNOSTICS", "false");
    options.processEnvironment.insert("MOUFFETTE_NETWORK_DIAGNOSTICS_VERBOSE", "true");
    QVERIFY(config.load(options, &error));
    QCOMPARE(config.remoteMediaRetentionMs(), 120000);
    QCOMPARE(config.remoteMediaCacheMaxMiB(), 256);
    QVERIFY(!config.networkDiagnostics());
    QVERIFY(config.networkDiagnosticsVerbose());
    options.processEnvironment.insert("MOUFFETTE_REMOTE_MEDIA_RETENTION_MS", "0");
    QVERIFY(!config.load(options, &error));
}

void AppConfigTest::configuresMinimumTimelineTracks() {
    AppConfig config;
    QCOMPARE(config.timelineMinTracksAbove(), 10);
    QCOMPARE(config.timelineMinTracksBelow(), 10);
    QTemporaryDir directory;
    const auto path = writeEnvFile(directory, "tracks.env",
        "MOUFFETTE_TIMELINE_MIN_TRACKS_ABOVE=12\n"
        "MOUFFETTE_TIMELINE_MIN_TRACKS_BELOW=4\n");
    QVERIFY(!path.isEmpty());
    auto options = isolatedOptions(path);
    QString error;
    QVERIFY2(config.load(options, &error), qPrintable(error));
    QCOMPARE(config.timelineMinTracksAbove(), 12);
    QCOMPARE(config.timelineMinTracksBelow(), 4);
    for (const auto* key : {"MOUFFETTE_TIMELINE_MIN_TRACKS_ABOVE", "MOUFFETTE_TIMELINE_MIN_TRACKS_BELOW"}) {
        for (const auto* value : {"0", "9999"}) {
            options.processEnvironment.insert(key, value);
            QVERIFY2(config.load(options, &error), qPrintable(error));
            QCOMPARE(key == QStringLiteral("MOUFFETTE_TIMELINE_MIN_TRACKS_ABOVE")
                ? config.timelineMinTracksAbove() : config.timelineMinTracksBelow(), QString(value).toInt());
        }
        for (const auto* value : {"-1", "10000", "1.5", "abc"}) {
            options.processEnvironment.insert(key, value);
            QVERIFY(!config.load(options, &error));
            QVERIFY(error.contains(key));
        }
        options.processEnvironment.remove(key);
    }
}

void AppConfigTest::configuresTimeline() {
    QTemporaryDir directory;
    const QString path = writeEnvFile(directory, QStringLiteral("timeline.env"),
        "MOUFFETTE_TIMELINE_MAX_DURATION_MS=240000\n"
        "MOUFFETTE_TIMELINE_OTHER_KEYFRAME_OPACITY_PERCENT=45\n"
        "MOUFFETTE_TIMELINE_SNAP_DISTANCE_PX=0\n");
    QVERIFY(!path.isEmpty());
    auto options = isolatedOptions(path);
    AppConfig config;
    QString error;
    QVERIFY2(config.load(options, &error), qPrintable(error));
    QCOMPARE(config.timelineMaxDurationMs(), 240000);
    QCOMPARE(config.timelineSlotsPerSecond(), 30);
    QCOMPARE(config.timelineDefaultClipDurationSlots(), 30);
    QCOMPARE(config.timelineOtherKeyframeOpacityPercent(), 45);
    QCOMPARE(config.timelineSnapDistancePx(), 0);
    QCOMPARE(config.timelineAutoScrollSpeedPxPerSecond(), 96);
    QCOMPARE(config.timelineHeightPx(), 240);
    QCOMPARE(config.timelineRulerHeightPx(), 28);
    QCOMPARE(config.timelineClipTrackHeightPx(), 48);
    QCOMPARE(config.timelineKeyframeSizePx(), 10);
    QCOMPARE(config.timelineInitialViewDurationMs(), 15000);
    const QString scrollSpeedKey = QStringLiteral("MOUFFETTE_TIMELINE_AUTO_SCROLL_SPEED_PX_PER_SECOND");
    for (const QString speed : {QStringLiteral("0"), QStringLiteral("48"), QStringLiteral("2000")}) {
        options.processEnvironment.insert(scrollSpeedKey, speed);
        QVERIFY2(config.load(options, &error), qPrintable(error));
        QCOMPARE(config.timelineAutoScrollSpeedPxPerSecond(), speed.toInt());
    }
    for (const QString speed : {QStringLiteral("-1"), QStringLiteral("2001"), QStringLiteral("1.5"), QStringLiteral("abc")}) {
        options.processEnvironment.insert(scrollSpeedKey, speed);
        QVERIFY(!config.load(options, &error));
        QVERIFY(error.contains(scrollSpeedKey));
    }
    options.processEnvironment.remove(scrollSpeedKey);
    for (const QString length : {QStringLiteral("1"), QStringLiteral("12"), QStringLiteral("145152000")}) {
        options.processEnvironment.insert(QStringLiteral("MOUFFETTE_TIMELINE_DEFAULT_CLIP_DURATION_SLOTS"), length);
        QVERIFY2(config.load(options, &error), qPrintable(error));
        QCOMPARE(config.timelineDefaultClipDurationSlots(), length.toInt());
    }
    for (const QString length : {QStringLiteral("0"), QStringLiteral("-1"), QStringLiteral("1.5"),
                               QStringLiteral("abc"), QStringLiteral("145152001")}) {
        options.processEnvironment.insert(QStringLiteral("MOUFFETTE_TIMELINE_DEFAULT_CLIP_DURATION_SLOTS"), length);
        QVERIFY(!config.load(options, &error));
        QVERIFY(error.contains(QStringLiteral("MOUFFETTE_TIMELINE_DEFAULT_CLIP_DURATION_SLOTS")));
    }
    options.processEnvironment.remove(QStringLiteral("MOUFFETTE_TIMELINE_DEFAULT_CLIP_DURATION_SLOTS"));
    options.processEnvironment.insert(QStringLiteral("MOUFFETTE_TIMELINE_OTHER_KEYFRAME_OPACITY_PERCENT"), QStringLiteral("101"));
    QVERIFY(!config.load(options, &error));
    QVERIFY(error.contains(QStringLiteral("MOUFFETTE_TIMELINE_OTHER_KEYFRAME_OPACITY_PERCENT")));
    options.processEnvironment.remove(QStringLiteral("MOUFFETTE_TIMELINE_OTHER_KEYFRAME_OPACITY_PERCENT"));
    for (const QString rate : {QStringLiteral("1"), QStringLiteral("60"), QStringLiteral("240")}) {
        options.processEnvironment.insert(QStringLiteral("MOUFFETTE_TIMELINE_SLOTS_PER_SECOND"), rate);
        QVERIFY(config.load(options, &error)); QCOMPARE(config.timelineSlotsPerSecond(), rate.toInt());
    }
    for (const QString rate : {QStringLiteral("0"), QStringLiteral("241"), QStringLiteral("29.97")}) {
        options.processEnvironment.insert(QStringLiteral("MOUFFETTE_TIMELINE_SLOTS_PER_SECOND"), rate);
        QVERIFY(!config.load(options, &error));
    }
    options.processEnvironment.insert(QStringLiteral("MOUFFETTE_TIMELINE_SLOTS_PER_SECOND"), QStringLiteral("30"));
    options.processEnvironment.insert(QStringLiteral("MOUFFETTE_TIMELINE_MAX_DURATION_MS"), QStringLiteral("33"));
    QVERIFY(!config.load(options, &error)); QVERIFY(error.contains(QStringLiteral("complete slot")));
    options.processEnvironment.insert(QStringLiteral("MOUFFETTE_TIMELINE_MAX_DURATION_MS"), QStringLiteral("34"));
    QVERIFY(config.load(options, &error));
    options.processEnvironment.insert(QStringLiteral("MOUFFETTE_TIMELINE_MAX_DURATION_MS"), QStringLiteral("0"));
    QVERIFY(!config.load(options, &error));
}

void AppConfigTest::configuresTimelineClipResizeZones() {
    AppConfig config;
    QCOMPARE(config.timelineClipResizeHandleWidthPx(), 8);
    QCOMPARE(config.timelineClipJointResizeHandleWidthPx(), 8);
    QCOMPARE(config.timelineClipJointMinResizeWidthPx(), 24);
    QCOMPARE(config.timelineClipMinResizeWidthPx(), 24);
    QTemporaryDir directory;
    const auto path = writeEnvFile(directory, "resize.env",
        "MOUFFETTE_TIMELINE_CLIP_RESIZE_HANDLE_WIDTH_PX=12\n"
        "MOUFFETTE_TIMELINE_CLIP_JOINT_RESIZE_HANDLE_WIDTH_PX=6\n"
        "MOUFFETTE_TIMELINE_CLIP_JOINT_MIN_RESIZE_WIDTH_PX=60\n"
        "MOUFFETTE_TIMELINE_CLIP_MIN_RESIZE_WIDTH_PX=40\n");
    QVERIFY(!path.isEmpty());
    auto options = isolatedOptions(path);
    QString error;
    QVERIFY2(config.load(options, &error), qPrintable(error));
    QCOMPARE(config.timelineClipResizeHandleWidthPx(), 12);
    QCOMPARE(config.timelineClipJointResizeHandleWidthPx(), 6);
    QCOMPARE(config.timelineClipJointMinResizeWidthPx(), 60);
    QCOMPARE(config.timelineClipMinResizeWidthPx(), 40);
    for (const bool handle : {true, false}) {
        const QString key = handle ? "MOUFFETTE_TIMELINE_CLIP_RESIZE_HANDLE_WIDTH_PX"
                                   : "MOUFFETTE_TIMELINE_CLIP_MIN_RESIZE_WIDTH_PX";
        if (handle) options.processEnvironment.insert("MOUFFETTE_TIMELINE_CLIP_JOINT_RESIZE_HANDLE_WIDTH_PX", "1");
        for (const int value : {handle ? 1 : 0, handle ? 100 : 1000}) {
            options.processEnvironment.insert(key, QString::number(value));
            QVERIFY2(config.load(options, &error), qPrintable(error));
            QCOMPARE(handle ? config.timelineClipResizeHandleWidthPx() : config.timelineClipMinResizeWidthPx(), value);
        }
        for (const QString value : {handle ? QString("0") : QString("-1"),
                                    handle ? QString("101") : QString("1001"), QString("1.5"), QString("abc")}) {
            options.processEnvironment.insert(key, value);
            QVERIFY(!config.load(options, &error));
            QVERIFY(error.contains(key));
        }
        options.processEnvironment.remove(key);
        options.processEnvironment.remove("MOUFFETTE_TIMELINE_CLIP_JOINT_RESIZE_HANDLE_WIDTH_PX");
    }
    const QString jointKey = "MOUFFETTE_TIMELINE_CLIP_JOINT_RESIZE_HANDLE_WIDTH_PX";
    const QString normalKey = "MOUFFETTE_TIMELINE_CLIP_RESIZE_HANDLE_WIDTH_PX";
    options.processEnvironment.insert(normalKey, "100");
    for (const auto* value : {"1", "100"}) {
        options.processEnvironment.insert(jointKey, value);
        QVERIFY2(config.load(options, &error), qPrintable(error));
        QCOMPARE(config.timelineClipJointResizeHandleWidthPx(), QString(value).toInt());
    }
    for (const auto* value : {"0", "101", "1.5", "abc"}) {
        options.processEnvironment.insert(jointKey, value);
        QVERIFY(!config.load(options, &error)); QVERIFY(error.contains(jointKey));
    }
    options.processEnvironment.insert(normalKey, "12");
    for (const auto* value : {"6", "12", "20", "100"}) {
        options.processEnvironment.insert(jointKey, value);
        QVERIFY2(config.load(options, &error), qPrintable(error));
        QCOMPARE(config.timelineClipJointResizeHandleWidthPx(), QString(value).toInt());
    }
    options.processEnvironment.insert(jointKey, "11");
    QVERIFY2(config.load(options, &error), qPrintable(error));
    options.processEnvironment.remove(normalKey); options.processEnvironment.remove(jointKey);
    const QString minimumJointKey = "MOUFFETTE_TIMELINE_CLIP_JOINT_MIN_RESIZE_WIDTH_PX";
    for (const auto* value : {"0", "24", "1000"}) {
        options.processEnvironment.insert(minimumJointKey, value);
        QVERIFY2(config.load(options, &error), qPrintable(error));
        QCOMPARE(config.timelineClipJointMinResizeWidthPx(), QString(value).toInt());
        QCOMPARE(config.timelineClipMinResizeWidthPx(), 40);
    }
    for (const auto* value : {"-1", "1001", "1.5", "abc"}) {
        options.processEnvironment.insert(minimumJointKey, value);
        QVERIFY(!config.load(options, &error)); QVERIFY(error.contains(minimumJointKey));
    }
    options.processEnvironment.remove(minimumJointKey);
    // Loading a fresh configuration restores compiled defaults, without stale overrides.
    options.defaultEnvFilePath.clear();
    QVERIFY2(config.load(options, &error), qPrintable(error));
    QCOMPARE(config.timelineClipResizeHandleWidthPx(), 8);
    QCOMPARE(config.timelineClipJointResizeHandleWidthPx(), 8);
    QCOMPARE(config.timelineClipJointMinResizeWidthPx(), 24);
    QCOMPARE(config.timelineClipMinResizeWidthPx(), 24);
}

void AppConfigTest::appAlwaysOnTopIsOptionalAndPersistent() {
    AppConfig config;
    AppConfig::LoadOptions options;
    options.arguments = {QStringLiteral("test")};
    options.processEnvironment = QProcessEnvironment();
    QString error;
    QVERIFY2(config.load(options, &error), qPrintable(error));
    QVERIFY(config.appAlwaysOnTop());
    options.settings.insert(QStringLiteral("appAlwaysOnTop"), false);
    QVERIFY2(config.load(options, &error), qPrintable(error));
    QVERIFY(!config.appAlwaysOnTop());
    options.settings.insert(QStringLiteral("appAlwaysOnTop"), true);
    QVERIFY2(config.load(options, &error), qPrintable(error));
    QVERIFY(config.appAlwaysOnTop());
    options.settings.insert(QStringLiteral("appAlwaysOnTop"), QStringLiteral("invalid"));
    QVERIFY(!config.load(options, &error));
}

void AppConfigTest::loadsEmbeddedDefaults() {
    AppConfig config;
    AppConfig::LoadOptions options;
    options.arguments = {QStringLiteral("tst_AppConfig")};
    options.processEnvironment = QProcessEnvironment();

    QString error;
    QVERIFY2(config.load(options, &error), qPrintable(error));
    QCOMPARE(config.serverUrl(), QStringLiteral("ws://localhost:8080"));
    QCOMPARE(config.mediaRamReservePercent(), 0);
    // The checked-in client/.env explicitly overrides the compiled reserve.
    QCOMPARE(config.mediaRamReserveMinMiB(), 512);
    QVERIFY(config.provenance(AppConfig::Key::MediaRamReserveMinMiB)
                .startsWith(QStringLiteral("embedded-env:")));
    QCOMPARE(config.remoteSessionHiddenTimeoutMs(), qint64(120000));
    QCOMPARE(config.projectMediaHiddenTimeoutMs(), qint64(0));
    QCOMPARE(config.projectHiddenRetentionMs(), qint64(240000));
    QCOMPARE(config.incomingSessionOrphanTimeoutMs(), qint64(5000));
    QCOMPARE(config.connectionAttemptTimeoutMs(), 10000);
    QCOMPARE(config.reconnectStableResetMs(), 30000);
    QCOMPARE(config.uploadActionMinIntervalMs(), 300);
    QCOMPARE(config.uploadCancelGuardMs(), 1000);
    QCOMPARE(config.sceneSeekPositionToleranceMs(), 120);
    QCOMPARE(config.sceneStartFrameToleranceMs(), 25);
    QCOMPARE(config.sceneDecoderSyncToleranceMs(), 25);
    QCOMPARE(config.sceneVideoSyncPositionToleranceMs(), 400);
    QCOMPARE(config.sceneVideoSyncTransitMaxMs(), 2000);
    QCOMPARE(config.sceneAuthoritativeSeekGuardMs(), 250);
    QCOMPARE(config.sceneRepeatTriggerGuardMs(), 500);
    QCOMPARE(config.uiContentFadeDurationMs(), 200);
    QCOMPARE(config.uiSpinnerRotationDurationMs(), 900);
    QCOMPARE(config.uiScrollbarHideDelayMs(), 500);
    QCOMPARE(config.uiInputWatchdogIntervalMs(), 120);
    QCOMPARE(config.uiSnapFreezeCleanupDelayMs(), 300);
    QCOMPARE(config.canvasTextInitialHeightPercent(), 10);
    QCOMPARE(config.clientCountdownRefreshIntervalMs(), 1000);
    QCOMPARE(config.toastDefaultDurationMs(), 4000);
    QCOMPARE(config.toastInfoDurationMs(), 2000);
    QCOMPARE(config.toastWarningDurationMs(), 3500);
    QCOMPARE(config.toastErrorDurationMs(), 5000);
    QCOMPARE(config.toastAnimationDurationMs(), 300);
    QCOMPARE(config.uploadConcurrency(), 2);
    QVERIFY(config.allowMultipleInstances());
    QVERIFY(!config.cursorDebug());
    QCOMPARE(config.loadedEnvFilePath(), QStringLiteral(":/config/client.env"));
    QVERIFY(config.provenance(AppConfig::Key::ServerUrl).startsWith(QStringLiteral("embedded-env:")));
}

void AppConfigTest::configuresMediaRamReserve() {
    QTemporaryDir directory;
    const QString path = writeEnvFile(directory, "ram.env",
        "MOUFFETTE_MEDIA_RAM_RESERVE_PERCENT=30\n"
        "MOUFFETTE_MEDIA_RAM_RESERVE_MIN_MIB=4096\n");
    QVERIFY(!path.isEmpty());
    auto options = isolatedOptions(path);
    AppConfig config;
    QString error;
    QVERIFY2(config.load(options, &error), qPrintable(error));
    QCOMPARE(config.mediaRamReservePercent(), 30);
    QCOMPARE(config.mediaRamReserveMinMiB(), 4096);
    for (const auto& value : {QStringLiteral("-1"), QStringLiteral("101"), QStringLiteral("abc")}) {
        options.processEnvironment.insert("MOUFFETTE_MEDIA_RAM_RESERVE_PERCENT", value);
        QVERIFY(!config.load(options, &error));
        QVERIFY(error.contains("MOUFFETTE_MEDIA_RAM_RESERVE_PERCENT"));
        QCOMPARE(config.mediaRamReservePercent(), 30); // Failed reload is atomic.
    }
    options.processEnvironment.insert("MOUFFETTE_MEDIA_RAM_RESERVE_PERCENT", "0");
    options.processEnvironment.insert("MOUFFETTE_MEDIA_RAM_RESERVE_MIN_MIB", "-1");
    QVERIFY(!config.load(options, &error));
    QVERIFY(error.contains("MOUFFETTE_MEDIA_RAM_RESERVE_MIN_MIB"));
    options.processEnvironment.insert("MOUFFETTE_MEDIA_RAM_RESERVE_MIN_MIB", "0");
    QVERIFY2(config.load(options, &error), qPrintable(error));
    QCOMPARE(config.mediaRamReservePercent(), 0);
    QCOMPARE(config.mediaRamReserveMinMiB(), 0);
}

void AppConfigTest::configuresProjectMediaHiddenTimeout() {
    AppConfig config;
    auto options = isolatedOptions(QString());
    QString error;
    QVERIFY2(config.load(options, &error), qPrintable(error));
    QCOMPARE(config.projectMediaHiddenTimeoutMs(), qint64(0));

    QTemporaryDir directory;
    options.defaultEnvFilePath = writeEnvFile(directory, "media-timeout.env",
        "MOUFFETTE_PROJECT_MEDIA_HIDDEN_TIMEOUT_MS=15000\n");
    QVERIFY(!options.defaultEnvFilePath.isEmpty());
    QVERIFY2(config.load(options, &error), qPrintable(error));
    QCOMPARE(config.projectMediaHiddenTimeoutMs(), qint64(15000));
    options.processEnvironment.insert("MOUFFETTE_PROJECT_MEDIA_HIDDEN_TIMEOUT_MS", "30000");
    QVERIFY2(config.load(options, &error), qPrintable(error));
    QCOMPARE(config.projectMediaHiddenTimeoutMs(), qint64(30000));

    // Media expiry may precede or follow session expiry independently.
    options.arguments << "--project-media-hidden-timeout-ms=90000";
    QVERIFY2(config.load(options, &error), qPrintable(error));
    QCOMPARE(config.projectMediaHiddenTimeoutMs(), qint64(90000));
    QCOMPARE(config.remoteSessionHiddenTimeoutMs(), qint64(60000));
    QCOMPARE(config.provenance(AppConfig::Key::ProjectMediaHiddenTimeoutMs),
             QStringLiteral("cli:--project-media-hidden-timeout-ms"));
    options.arguments.removeLast();

    for (const QString value : {"-1", "999", "86400001", "1.5", "abc", "300000"}) {
        options.processEnvironment.insert("MOUFFETTE_PROJECT_MEDIA_HIDDEN_TIMEOUT_MS", value);
        QVERIFY(!config.load(options, &error));
        QVERIFY(error.contains("MOUFFETTE_PROJECT_MEDIA_HIDDEN_TIMEOUT_MS"));
        QCOMPARE(config.projectMediaHiddenTimeoutMs(), qint64(90000)); // Atomic failure.
    }
    options.processEnvironment.insert("MOUFFETTE_PROJECT_HIDDEN_RETENTION_MS", "86400001");
    for (const QString value : {"0", "1000", "86400000"}) {
        options.processEnvironment.insert("MOUFFETTE_PROJECT_MEDIA_HIDDEN_TIMEOUT_MS", value);
        QVERIFY2(config.load(options, &error), qPrintable(error));
        QCOMPARE(config.projectMediaHiddenTimeoutMs(), value.toLongLong());
    }
}

void AppConfigTest::configuresCanvasTextInitialHeight() {
    AppConfig config;
    auto options = isolatedOptions(QString());
    QString error;
    QVERIFY2(config.load(options, &error), qPrintable(error));
    QCOMPARE(config.canvasTextInitialHeightPercent(), 8);

    QTemporaryDir directory;
    options.defaultEnvFilePath = writeEnvFile(directory, "text.env",
        "MOUFFETTE_CANVAS_TEXT_INITIAL_HEIGHT_PERCENT=12\n");
    QVERIFY(!options.defaultEnvFilePath.isEmpty());
    QVERIFY2(config.load(options, &error), qPrintable(error));
    QCOMPARE(config.canvasTextInitialHeightPercent(), 12);
    options.processEnvironment.insert("MOUFFETTE_CANVAS_TEXT_INITIAL_HEIGHT_PERCENT", "5");
    QVERIFY2(config.load(options, &error), qPrintable(error));
    QCOMPARE(config.canvasTextInitialHeightPercent(), 5);
    options.arguments << "--canvas-text-initial-height-percent=9";
    QVERIFY2(config.load(options, &error), qPrintable(error));
    QCOMPARE(config.canvasTextInitialHeightPercent(), 9);
    QCOMPARE(config.provenance(AppConfig::Key::CanvasTextInitialHeightPercent),
             QStringLiteral("cli:--canvas-text-initial-height-percent"));
    options.arguments.removeLast();
    for (const QString value : {"0", "-1", "101", "8.5", "abc"}) {
        options.processEnvironment.insert("MOUFFETTE_CANVAS_TEXT_INITIAL_HEIGHT_PERCENT", value);
        QVERIFY(!config.load(options, &error));
        QVERIFY(error.contains("MOUFFETTE_CANVAS_TEXT_INITIAL_HEIGHT_PERCENT"));
        QCOMPARE(config.canvasTextInitialHeightPercent(), 9); // Atomic failed reload.
    }
    for (int value : {1, 100}) {
        options.processEnvironment.insert("MOUFFETTE_CANVAS_TEXT_INITIAL_HEIGHT_PERCENT",
                                          QString::number(value));
        QVERIFY2(config.load(options, &error), qPrintable(error));
        QCOMPARE(config.canvasTextInitialHeightPercent(), value);
    }
}

void AppConfigTest::compiledDefaultDisablesMultipleInstances() {
    AppConfig config;
    AppConfig::LoadOptions options = isolatedOptions(QString());

    QString error;
    QVERIFY2(config.load(options, &error), qPrintable(error));
    QVERIFY(!config.allowMultipleInstances());
    QCOMPARE(config.mediaRamReservePercent(), 0);
    QCOMPARE(config.mediaRamReserveMinMiB(), 512);
    QCOMPARE(config.provenance(AppConfig::Key::MediaRamReserveMinMiB),
             QStringLiteral("compiled-default"));
    QCOMPARE(config.provenance(AppConfig::Key::AllowMultipleInstances),
             QStringLiteral("compiled-default"));
}

void AppConfigTest::appliesDocumentedPrecedence() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString envPath = writeEnvFile(directory, QStringLiteral("client.env"),
        "MOUFFETTE_SERVER_URL=ws://10.0.0.5:8081\n"
        "MOUFFETTE_AUTO_UPLOAD_IMPORTED_MEDIA=false\n");
    QVERIFY(!envPath.isEmpty());

    AppConfig::LoadOptions options = isolatedOptions(envPath);
    options.settings.insert(QStringLiteral("serverUrl"), QStringLiteral("ws://192.168.1.5:8082"));
    options.settings.insert(QStringLiteral("autoUploadImportedMedia"), true);
    options.processEnvironment.insert(QStringLiteral("MOUFFETTE_SERVER_URL"),
                                      QStringLiteral("ws://172.16.1.5:8083"));
    options.processEnvironment.insert(QStringLiteral("MOUFFETTE_AUTO_UPLOAD_IMPORTED_MEDIA"),
                                      QStringLiteral("false"));
    options.arguments << QStringLiteral("--server-url=wss://example.com:443");

    AppConfig config;
    QString error;
    QVERIFY2(config.load(options, &error), qPrintable(error));
    QCOMPARE(config.serverUrl(), QStringLiteral("wss://example.com:443"));
    QVERIFY(!config.autoUploadImportedMedia());
    QCOMPARE(config.provenance(AppConfig::Key::ServerUrl), QStringLiteral("cli:--server-url"));
    QCOMPARE(config.provenance(AppConfig::Key::AutoUploadImportedMedia),
             QStringLiteral("process:MOUFFETTE_AUTO_UPLOAD_IMPORTED_MEDIA"));
}

void AppConfigTest::commandLineEnvFileReplacesProcessSelection() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString processPath = writeEnvFile(directory, QStringLiteral("process.env"),
        "MOUFFETTE_SERVER_URL=ws://10.0.0.8:8080\n");
    const QString cliPath = writeEnvFile(directory, QStringLiteral("cli.env"),
        "MOUFFETTE_SERVER_URL=ws://192.168.50.8:8080\n");
    QVERIFY(!processPath.isEmpty());
    QVERIFY(!cliPath.isEmpty());

    AppConfig::LoadOptions options = isolatedOptions(QStringLiteral("does-not-exist.env"));
    options.processEnvironment.insert(QStringLiteral("MOUFFETTE_ENV_FILE"), processPath);
    options.arguments << QStringLiteral("--env-file") << cliPath;

    AppConfig config;
    QString error;
    QVERIFY2(config.load(options, &error), qPrintable(error));
    QCOMPARE(config.serverUrl(), QStringLiteral("ws://192.168.50.8:8080"));
    QCOMPARE(config.loadedEnvFilePath(), cliPath);
    QVERIFY(config.provenance(AppConfig::Key::ServerUrl).startsWith(QStringLiteral("cli:--env-file:")));
}

void AppConfigTest::parsesFalseBooleansAsFalse() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString envPath = writeEnvFile(directory, QStringLiteral("false.env"),
        "MOUFFETTE_CURSOR_DEBUG=false\n"
        "MOUFFETTE_ALLOW_MULTIPLE_INSTANCES=false\n"
        "MOUFFETTE_RUNTIME_DIAGNOSTICS=0\n"
        "MOUFFETTE_CANVAS_PROFILING=no\n");
    QVERIFY(!envPath.isEmpty());

    AppConfig config;
    AppConfig::LoadOptions options = isolatedOptions(envPath);
    QString error;
    QVERIFY2(config.load(options, &error), qPrintable(error));
    QVERIFY(!config.cursorDebug());
    QVERIFY(!config.allowMultipleInstances());
    QVERIFY(!config.runtimeDiagnostics());
    QVERIFY(!config.canvasProfiling());
}

void AppConfigTest::appliesProductionOverrideLastPerKey() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString basePath = writeEnvFile(directory, QStringLiteral("base.env"),
        "MOUFFETTE_ALLOW_MULTIPLE_INSTANCES=true\n"
        "MOUFFETTE_CURSOR_DEBUG=false\n");
    const QString productionPath = writeEnvFile(directory, QStringLiteral("production.env"),
        "MOUFFETTE_ALLOW_MULTIPLE_INSTANCES=false\n");
    QVERIFY(!basePath.isEmpty());
    QVERIFY(!productionPath.isEmpty());

    AppConfig::LoadOptions options = isolatedOptions(basePath);
    options.applyProductionOverride = true;
    options.productionEnvFilePath = productionPath;
    options.processEnvironment.insert(QStringLiteral("MOUFFETTE_ALLOW_MULTIPLE_INSTANCES"),
                                      QStringLiteral("true"));
    options.settings.insert(QStringLiteral("allowMultipleInstances"), true);
    options.processEnvironment.insert(QStringLiteral("MOUFFETTE_CURSOR_DEBUG"),
                                      QStringLiteral("true"));
    options.arguments << QStringLiteral("--allow-multiple-instances=true");

    AppConfig config;
    QString error;
    QVERIFY2(config.load(options, &error), qPrintable(error));
    QVERIFY(!config.allowMultipleInstances());
    QVERIFY(config.cursorDebug());
    QCOMPARE(config.provenance(AppConfig::Key::AllowMultipleInstances),
             QStringLiteral("production-override:%1").arg(productionPath));
}

void AppConfigTest::emptyProductionOverrideChangesNothing() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString basePath = writeEnvFile(directory, QStringLiteral("base.env"),
        "MOUFFETTE_ALLOW_MULTIPLE_INSTANCES=true\n");
    const QString productionPath = writeEnvFile(directory, QStringLiteral("production.env"),
        "# intentionally empty\n");

    AppConfig::LoadOptions options = isolatedOptions(basePath);
    options.applyProductionOverride = true;
    options.productionEnvFilePath = productionPath;

    AppConfig config;
    QString error;
    QVERIFY2(config.load(options, &error), qPrintable(error));
    QVERIFY(config.allowMultipleInstances());
    QVERIFY(config.provenance(AppConfig::Key::AllowMultipleInstances)
                .startsWith(QStringLiteral("embedded-env:")));
}

void AppConfigTest::acceptsPublicPlainWebSocketUrl() {
    AppConfig config;
    QString error;
    AppConfig::LoadOptions options = isolatedOptions(QString());
    options.processEnvironment.insert(QStringLiteral("MOUFFETTE_SERVER_URL"),
                                      QStringLiteral("ws://example.com:8080"));
    QVERIFY2(config.load(options, &error), qPrintable(error));
    QCOMPARE(config.serverUrl(), QStringLiteral("ws://example.com:8080"));
}

void AppConfigTest::acceptsBothWebSocketSchemesForRuntimeUrlEdits() {
    QString error;
    QUrl normalized;
    QVERIFY2(AppConfig::validateServerUrl(
        QStringLiteral("ws://example.com:8080"), &normalized, &error),
        qPrintable(error));
    QCOMPARE(normalized.toString(QUrl::FullyEncoded),
             QStringLiteral("ws://example.com:8080"));

    error.clear();
    QVERIFY2(AppConfig::validateServerUrl(
        QStringLiteral("ws://192.168.10.20:8080"), &normalized, &error),
        qPrintable(error));
    QCOMPARE(normalized.toString(QUrl::FullyEncoded),
             QStringLiteral("ws://192.168.10.20:8080"));

    error.clear();
    QVERIFY2(AppConfig::validateServerUrl(
        QStringLiteral("wss://example.com/socket"), &normalized, &error),
        qPrintable(error));

    error.clear();
    QVERIFY2(AppConfig::validateServerUrl(
        QStringLiteral("ws://receiver:8080"), &normalized, &error),
        qPrintable(error));

    error.clear();
    QVERIFY2(AppConfig::validateServerUrl(
        QStringLiteral("ws://receiver.local:8080"), &normalized, &error),
        qPrintable(error));

    error.clear();
    QVERIFY2(AppConfig::validateServerUrl(
        QStringLiteral("ws://preview.localhost:8080"), &normalized, &error),
        qPrintable(error));
}

void AppConfigTest::rejectsInvalidHiddenDeadlineOrdering() {
    AppConfig::LoadOptions options = isolatedOptions(QString());
    options.processEnvironment.insert(QStringLiteral("MOUFFETTE_REMOTE_SESSION_HIDDEN_TIMEOUT_MS"),
                                      QStringLiteral("300000"));
    options.processEnvironment.insert(QStringLiteral("MOUFFETTE_PROJECT_HIDDEN_RETENTION_MS"),
                                      QStringLiteral("300000"));

    AppConfig config;
    QString error;
    QVERIFY(!config.load(options, &error));
    QVERIFY(error.contains(QStringLiteral("must be greater")));
}

void AppConfigTest::validatesIncomingSessionOrphanTimeoutFromCli() {
    AppConfig::LoadOptions options = isolatedOptions(QString());
    options.arguments << QStringLiteral("--incoming-session-orphan-timeout-ms=4250");

    AppConfig config;
    QString error;
    QVERIFY2(config.load(options, &error), qPrintable(error));
    QCOMPARE(config.incomingSessionOrphanTimeoutMs(), qint64(4250));
    QCOMPARE(config.provenance(AppConfig::Key::IncomingSessionOrphanTimeoutMs),
             QStringLiteral("cli:--incoming-session-orphan-timeout-ms"));

    options.arguments = {QStringLiteral("tst_AppConfig"),
                         QStringLiteral("--incoming-session-orphan-timeout-ms=999")};
    error.clear();
    QVERIFY(!config.load(options, &error));
    QVERIFY(error.contains(QStringLiteral("MOUFFETTE_INCOMING_SESSION_ORPHAN_TIMEOUT_MS")));
}

void AppConfigTest::warnsAndIgnoresUnknownNamespacedEnvKey() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString envPath = writeEnvFile(directory, QStringLiteral("unknown.env"),
        "MOUFFETTE_SERVER_ULR=ws://127.0.0.1:8080\n");
    QVERIFY(!envPath.isEmpty());

    AppConfig config;
    AppConfig::LoadOptions options = isolatedOptions(envPath);
    QString error;
    QVERIFY2(config.load(options, &error), qPrintable(error));
    QCOMPARE(config.serverUrl(), QStringLiteral("ws://localhost:8080"));
}

QTEST_APPLESS_MAIN(AppConfigTest)

#include "tst_AppConfig.moc"
