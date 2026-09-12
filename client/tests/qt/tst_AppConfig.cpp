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
    void appliesDocumentedPrecedence();
    void commandLineEnvFileReplacesProcessSelection();
    void parsesFalseBooleansAsFalse();
    void rejectsPublicPlainWebSocketUrlAtomically();
    void appliesTransportSecurityPolicyToRuntimeUrlEdits();
    void rejectsInvalidHiddenDeadlineOrdering();
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
    QCOMPARE(config.remoteSessionHiddenTimeoutMs(), qint64(60000));
    QCOMPARE(config.projectHiddenRetentionMs(), qint64(300000));
    QCOMPARE(config.uploadConcurrency(), 2);
    QVERIFY(config.useQuickCanvasRenderer());
    QVERIFY(!config.cursorDebug());
    QCOMPARE(config.loadedEnvFilePath(), QStringLiteral(":/config/client.env"));
    QVERIFY(config.provenance(AppConfig::Key::ServerUrl).startsWith(QStringLiteral("embedded-env:")));
}

void AppConfigTest::appliesDocumentedPrecedence() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString envPath = writeEnvFile(directory, QStringLiteral("client.env"),
        "MOUFFETTE_SERVER_URL=ws://10.0.0.5:8081\n"
        "MOUFFETTE_AUTO_UPLOAD_IMPORTED_MEDIA=false\n"
        "MOUFFETTE_USE_QUICK_CANVAS_RENDERER=false\n");
    QVERIFY(!envPath.isEmpty());

    AppConfig::LoadOptions options = isolatedOptions(envPath);
    options.settings.insert(QStringLiteral("serverUrl"), QStringLiteral("ws://192.168.1.5:8082"));
    options.settings.insert(QStringLiteral("autoUploadImportedMedia"), true);
    options.settings.insert(QStringLiteral("useQuickCanvasRenderer"), true);
    options.processEnvironment.insert(QStringLiteral("MOUFFETTE_SERVER_URL"),
                                      QStringLiteral("ws://172.16.1.5:8083"));
    options.processEnvironment.insert(QStringLiteral("MOUFFETTE_AUTO_UPLOAD_IMPORTED_MEDIA"),
                                      QStringLiteral("false"));
    options.processEnvironment.insert(QStringLiteral("MOUFFETTE_USE_QUICK_CANVAS_RENDERER"),
                                      QStringLiteral("false"));
    options.arguments << QStringLiteral("--server-url=wss://example.com:443")
                      << QStringLiteral("--use-quick-canvas-renderer=true");

    AppConfig config;
    QString error;
    QVERIFY2(config.load(options, &error), qPrintable(error));
    QCOMPARE(config.serverUrl(), QStringLiteral("wss://example.com:443"));
    QVERIFY(config.useQuickCanvasRenderer());
    QVERIFY(!config.autoUploadImportedMedia());
    QCOMPARE(config.provenance(AppConfig::Key::ServerUrl), QStringLiteral("cli:--server-url"));
    QCOMPARE(config.provenance(AppConfig::Key::UseQuickCanvasRenderer),
             QStringLiteral("cli:--use-quick-canvas-renderer"));
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
        "MOUFFETTE_RUNTIME_DIAGNOSTICS=0\n"
        "MOUFFETTE_MIGRATION_TELEMETRY=off\n"
        "MOUFFETTE_CANVAS_PROFILING=no\n");
    QVERIFY(!envPath.isEmpty());

    AppConfig config;
    AppConfig::LoadOptions options = isolatedOptions(envPath);
    QString error;
    QVERIFY2(config.load(options, &error), qPrintable(error));
    QVERIFY(!config.cursorDebug());
    QVERIFY(!config.runtimeDiagnostics());
    QVERIFY(!config.migrationTelemetry());
    QVERIFY(!config.canvasProfiling());
}

void AppConfigTest::rejectsPublicPlainWebSocketUrlAtomically() {
    AppConfig config;
    AppConfig::LoadOptions defaults = isolatedOptions(QString());
    QString error;
    QVERIFY2(config.load(defaults, &error), qPrintable(error));
    const QString previousUrl = config.serverUrl();

    AppConfig::LoadOptions invalid = isolatedOptions(QString());
    invalid.processEnvironment.insert(QStringLiteral("MOUFFETTE_SERVER_URL"),
                                      QStringLiteral("ws://example.com:8080"));
    QVERIFY(!config.load(invalid, &error));
    QVERIFY(error.contains(QStringLiteral("wss://")));
    QCOMPARE(config.serverUrl(), previousUrl);
}

void AppConfigTest::appliesTransportSecurityPolicyToRuntimeUrlEdits() {
    QString error;
    QUrl normalized;
    QVERIFY(!AppConfig::validateServerUrl(
        QStringLiteral("ws://example.com:8080"), &normalized, &error));
    QVERIFY(error.contains(QStringLiteral("wss://")));

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
    QVERIFY(!AppConfig::validateServerUrl(
        QStringLiteral("ws://receiver:8080"), &normalized, &error));
    QVERIFY(error.contains(QStringLiteral("wss://")));

    error.clear();
    QVERIFY(!AppConfig::validateServerUrl(
        QStringLiteral("ws://receiver.local:8080"), &normalized, &error));
    QVERIFY(error.contains(QStringLiteral("wss://")));

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
