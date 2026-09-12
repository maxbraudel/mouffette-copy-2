#include "backend/config/AppConfig.h"

#include <QFile>
#include <QDebug>
#include <QHostAddress>
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

constexpr std::array<SettingSpec, 12> kSpecs{{
    {Key::ServerUrl, "MOUFFETTE_SERVER_URL", "server-url", "serverUrl", "ws://localhost:8080", false},
    {Key::RemoteSessionHiddenTimeoutMs, "MOUFFETTE_REMOTE_SESSION_HIDDEN_TIMEOUT_MS", "remote-session-hidden-timeout-ms", nullptr, "60000", false},
    {Key::ProjectHiddenRetentionMs, "MOUFFETTE_PROJECT_HIDDEN_RETENTION_MS", "project-hidden-retention-ms", nullptr, "300000", false},
    {Key::UploadConcurrency, "MOUFFETTE_UPLOAD_CONCURRENCY", "upload-concurrency", nullptr, "2", false},
    {Key::AutoUploadImportedMedia, "MOUFFETTE_AUTO_UPLOAD_IMPORTED_MEDIA", "auto-upload-imported-media", "autoUploadImportedMedia", "false", true},
    {Key::UseQuickCanvasRenderer, "MOUFFETTE_USE_QUICK_CANVAS_RENDERER", "use-quick-canvas-renderer", "useQuickCanvasRenderer", "true", true},
    {Key::QtMediaBackend, "QT_MEDIA_BACKEND", "media-backend", nullptr, "ffmpeg", false},
    {Key::InstanceSuffix, "MOUFFETTE_INSTANCE_SUFFIX", "instance-suffix", nullptr, "", false},
    {Key::CursorDebug, "MOUFFETTE_CURSOR_DEBUG", "cursor-debug", nullptr, "false", true},
    {Key::RuntimeDiagnostics, "MOUFFETTE_RUNTIME_DIAGNOSTICS", "runtime-diagnostics", nullptr, "false", true},
    {Key::MigrationTelemetry, "MOUFFETTE_MIGRATION_TELEMETRY", "migration-telemetry", nullptr, "false", true},
    {Key::CanvasProfiling, "MOUFFETTE_CANVAS_PROFILING", "canvas-profiling", nullptr, "false", true},
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

bool isPrivateWebSocketHost(const QString& inputHost) {
    const QString host = inputHost.trimmed().toLower();
    QHostAddress address;
    if (address.setAddress(host)) {
        if (address.isLoopback() || address.isLinkLocal()) {
            return true;
        }

        bool isIpv4 = false;
        const quint32 ipv4 = address.toIPv4Address(&isIpv4);
        if (isIpv4) {
            return (ipv4 & 0xff000000U) == 0x0a000000U
                || (ipv4 & 0xfff00000U) == 0xac100000U
                || (ipv4 & 0xffff0000U) == 0xc0a80000U;
        }

        const Q_IPV6ADDR ipv6 = address.toIPv6Address();
        return (ipv6[0] & 0xfeU) == 0xfcU; // RFC 4193 unique-local fc00::/7
    }

    // Hostnames are deliberately not guessed to be private. A single-label or
    // mDNS-looking name can resolve to a public address (and can change between
    // validation and connection), so plain WebSocket is limited to literal
    // private/link-local/loopback addresses and the DNS names reserved for the
    // local loopback namespace.
    return host == QStringLiteral("localhost")
        || host.endsWith(QStringLiteral(".localhost"));
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
    if (scheme == QStringLiteral("ws") && !isPrivateWebSocketHost(url.host())) {
        return setError(error, QStringLiteral("MOUFFETTE_SERVER_URL from %1 must use wss:// for a public host")
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
    m_projectHiddenRetentionMs = 300000;
    m_uploadConcurrency = 2;
    m_autoUploadImportedMedia = false;
    m_useQuickCanvasRenderer = true;
    m_qtMediaBackend = QStringLiteral("ffmpeg");
    m_instanceSuffix.clear();
    m_cursorDebug = false;
    m_runtimeDiagnostics = false;
    m_migrationTelemetry = false;
    m_canvasProfiling = false;
}

bool AppConfig::initialize(const QStringList& arguments, QString* errorMessage) {
    LoadOptions options;
    options.arguments = arguments;
    options.processEnvironment = QProcessEnvironment::systemEnvironment();

    QSettings settings(QStringLiteral("Mouffette"), QStringLiteral("Client"));
    for (const SettingSpec& spec : kSpecs) {
        if (!spec.settingsName) {
            continue;
        }
        const QString settingsName = QLatin1String(spec.settingsName);
        if (settings.contains(settingsName)) {
            options.settings.insert(settingsName, settings.value(settingsName));
        }
    }
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

    qint64 uploadConcurrency = 0;
    if (!parseInteger(rawValues.at(Key::UploadConcurrency), keyName(Key::UploadConcurrency),
                      1, 2, uploadConcurrency, errorMessage)) {
        return false;
    }
    candidate.m_uploadConcurrency = static_cast<int>(uploadConcurrency);

    if (!parseBoolean(rawValues.at(Key::AutoUploadImportedMedia),
                      keyName(Key::AutoUploadImportedMedia),
                      candidate.m_autoUploadImportedMedia, errorMessage)
        || !parseBoolean(rawValues.at(Key::UseQuickCanvasRenderer),
                         keyName(Key::UseQuickCanvasRenderer),
                         candidate.m_useQuickCanvasRenderer, errorMessage)
        || !parseBoolean(rawValues.at(Key::CursorDebug), keyName(Key::CursorDebug),
                         candidate.m_cursorDebug, errorMessage)
        || !parseBoolean(rawValues.at(Key::RuntimeDiagnostics), keyName(Key::RuntimeDiagnostics),
                         candidate.m_runtimeDiagnostics, errorMessage)
        || !parseBoolean(rawValues.at(Key::MigrationTelemetry), keyName(Key::MigrationTelemetry),
                         candidate.m_migrationTelemetry, errorMessage)
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

    candidate.m_instanceSuffix = rawValues.at(Key::InstanceSuffix).value.trimmed();
    static const QRegularExpression suffixPattern(QStringLiteral("^[A-Za-z0-9_.-]{0,64}$"));
    if (!suffixPattern.match(candidate.m_instanceSuffix).hasMatch()) {
        return setError(errorMessage, QStringLiteral("MOUFFETTE_INSTANCE_SUFFIX from %1 is invalid")
                                          .arg(rawValues.at(Key::InstanceSuffix).source));
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
