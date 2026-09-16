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
    void configuresMediaRamReserve();
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

void AppConfigTest::loadsEmbeddedDefaults() {
    AppConfig config;
    AppConfig::LoadOptions options;
    options.arguments = {QStringLiteral("tst_AppConfig")};
    options.processEnvironment = QProcessEnvironment();

    QString error;
    QVERIFY2(config.load(options, &error), qPrintable(error));
    QCOMPARE(config.serverUrl(), QStringLiteral("ws://localhost:8080"));
    QCOMPARE(config.mediaRamReservePercent(), 0);
    QCOMPARE(config.mediaRamReserveMinMiB(), 548);
    QCOMPARE(config.remoteSessionHiddenTimeoutMs(), qint64(30000));
    QCOMPARE(config.projectHiddenRetentionMs(), qint64(60000));
    QCOMPARE(config.incomingSessionOrphanTimeoutMs(), qint64(3000));
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

void AppConfigTest::compiledDefaultDisablesMultipleInstances() {
    AppConfig config;
    AppConfig::LoadOptions options = isolatedOptions(QString());

    QString error;
    QVERIFY2(config.load(options, &error), qPrintable(error));
    QVERIFY(!config.allowMultipleInstances());
    QCOMPARE(config.mediaRamReservePercent(), 0);
    QCOMPARE(config.mediaRamReserveMinMiB(), 548);
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
