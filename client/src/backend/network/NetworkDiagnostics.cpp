#include "NetworkDiagnostics.h"
#include "backend/config/AppConfig.h"
#include "backend/runtime/RuntimeProfile.h"
#include "backend/runtime/SuspendInclusiveClock.h"
#include "backend/network/ProtocolConstants.h"
#include "AppBuildConfig.h"
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QSet>
#include <QHash>
#include <QRegularExpression>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>
#include <algorithm>

namespace {
QJsonObject safeFields(const QJsonObject& input) {
    static const QSet<QString> strings{
        "remoteSessionId", "sessionId", "uploadId", "transferId", "sceneRunId", "endpointId",
        "ownerEndpointId", "targetEndpointId", "serverBootId", "runtimeId", "requestId",
        "channel", "reason", "code", "errorCode", "before", "after", "phase", "type",
        "errorClass", "closeReason", "result", "appVersion", "qtVersion"
    };
    static const QSet<QString> numbers{
        "generation", "connectionGeneration", "stateRevision", "sequence", "expectedGeneration",
        "receivedGeneration", "expectedRevision", "receivedRevision", "protocolVersion", "policyVersion",
        "budgetBps", "screens", "viewers", "width", "height", "fps", "rttMs", "heartbeatAgeMs", "proofAgeMs", "controlContactAgeMs", "loopLagMs", "queueBytes", "dataQueueBytes",
        "sentBytes", "confirmedBytes", "offset", "size", "windowBytes", "delayMs", "remainingMs",
        "closeCode", "socketError", "count", "dropped", "temporary", "success", "critical",
        "heartbeatIntervalMs", "transportSuspectAfterMs", "transportTimeoutMs", "sessionRecoveryTimeoutMs",
        "retentionMs", "cacheMaxMiB", "verbose", "fileBytes", "queueCapacity", "elapsedMs", "reusedBytes",
        "leaseTimeoutMs", "scenePrepareTimeoutMs", "sceneActivationLeadMs", "sceneMaxClockSkewMs",
        "sceneStartedAckTimeoutMs", "sceneStopTimeoutMs", "sceneMaxStartSkewMs", "uploadIdleTimeoutMs",
        "uploadTargetAckTimeoutMs", "removalAckTimeoutMs", "uploadConcurrency", "controlRequestRetryMs",
        "uploadChannelRetryBaseMs", "uploadChannelRetryMaxMs", "uploadChannelAttemptTimeoutMs",
        "reconnectFastStepMs", "reconnectFastMaxMs", "reconnectBaseMs", "reconnectMaxMs",
        "reconnectJitterPercent", "sessionDeadlinePollIntervalMs", "leaseHealthCheckIntervalMs"
    };
    static const QRegularExpression token(QStringLiteral("^[A-Za-z0-9_:.+ -]{1,128}$"));
    static const QSet<QString> codes {"reason", "code", "errorCode", "closeReason", "errorClass", "result", "type"};
    static const QRegularExpression codePattern(QStringLiteral("^[a-z][a-z0-9_]{0,95}$"));
    QJsonObject output;
    for (auto it = input.begin(); it != input.end(); ++it) {
        if (numbers.contains(it.key()) && (it->isDouble() || it->isBool())) output.insert(it.key(), *it);
        else if (strings.contains(it.key()) && it->isString()
                 && token.match(it->toString()).hasMatch()
                 && (!codes.contains(it.key()) || codePattern.match(it->toString()).hasMatch())) output.insert(it.key(), *it);
    }
    return output;
}
QByteArray line(const QString& event, QJsonObject fields) {
    fields.insert("event", event);
    fields.insert("utc", QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs));
    fields.insert("monotonicMs", static_cast<double>(MouffetteClock::nowMs()));
    return QJsonDocument(fields).toJson(QJsonDocument::Compact) + '\n';
}
}

struct NetworkDiagnostics::State {
    QString directory;
    Options options;
    std::mutex mutex;
    std::condition_variable wake;
    std::deque<QByteArray> queue;
    std::atomic<quint64> dropped{0};
    bool stopping = false;
    struct Rate { qint64 start = 0; int emitted = 0; quint64 suppressed = 0; };
    QHash<QString, Rate> rates;
    qint64 globalStart = 0;
    int globalEmitted = 0;
    std::thread worker;

    void run() {
        QFile file;
        qint64 retryAt = 0;
        qint64 summaryAt = MouffetteClock::nowMs() + 5000;
        quint64 previousDrops = 0;
        auto write = [&](const QByteArray& bytes) {
            const qint64 now = MouffetteClock::nowMs();
            if (bytes.size() > options.fileBytes || now < retryAt) { ++dropped; return; }
            const QString current = QDir(directory).filePath("network.0.jsonl");
            if (!file.isOpen()) {
                if (!QDir().mkpath(directory)) { retryAt = now + 5000; ++dropped; return; }
                file.setFileName(current);
                if (!file.open(QIODevice::WriteOnly | QIODevice::Append)) { retryAt = now + 5000; ++dropped; return; }
            }
            if (file.size() + bytes.size() > options.fileBytes) {
                file.close();
                const QString oldest = QDir(directory).filePath("network.2.jsonl");
                const QString previous = QDir(directory).filePath("network.1.jsonl");
                // Fail closed on rotation errors instead of growing a fourth file.
                if ((QFile::exists(oldest) && !QFile::remove(oldest))
                    || (QFile::exists(previous) && !QFile::rename(previous, oldest))
                    || !QFile::rename(current, previous)
                    || !file.open(QIODevice::WriteOnly | QIODevice::Append)) {
                    retryAt = now + 5000; ++dropped; return;
                }
            }
            if (file.write(bytes) != bytes.size() || !file.flush()) {
                file.close(); retryAt = now + 5000; ++dropped;
            }
        };
        for (;;) {
            std::deque<QByteArray> batch;
            std::deque<QByteArray> summaries;
            bool done = false;
            {
                std::unique_lock<std::mutex> lock(mutex);
                wake.wait_for(lock, std::chrono::milliseconds(250), [&] { return stopping || !queue.empty(); });
                batch.swap(queue);
                done = stopping;
                const qint64 now = MouffetteClock::nowMs();
                if (now >= summaryAt || done) {
                    for (auto it = rates.begin(); it != rates.end(); ++it) {
                        if (it->suppressed) summaries.push_back(line("event_summary", {{"type", it.key()}, {"count", double(it->suppressed)}}));
                    }
                    rates.clear();
                    const quint64 drops = dropped.load();
                    if (drops > previousDrops) summaries.push_back(line("logger_dropped", {{"count", double(drops - previousDrops)}}));
                    previousDrops = drops;
                    summaryAt = now + 5000;
                }
            }
            for (const auto& bytes : batch) write(bytes);
            for (const auto& bytes : summaries) write(bytes);
            if (done) break;
        }
    }
};

NetworkDiagnostics::NetworkDiagnostics(const QString& directory, Options options)
    : m_state(std::make_shared<State>()) {
    m_state->directory = directory;
    m_state->options = options;
    m_state->options.queueCapacity = std::clamp(options.queueCapacity, 1, 1024);
    m_state->options.fileBytes = std::clamp<qint64>(options.fileBytes, 512, 5 * 1024 * 1024);
    if (options.enabled) {
        auto state = m_state;
        state->worker = std::thread([state] { state->run(); });
        enqueue("diagnostics_started", {{"appVersion", MOUFFETTE_VERSION_STRING}, {"qtVersion", QT_VERSION_STR},
            {"protocolVersion", MouffetteProtocol::Version}, {"verbose", options.verbose},
            {"fileBytes", double(m_state->options.fileBytes)}, {"queueCapacity", m_state->options.queueCapacity}}, true);
    }
}
NetworkDiagnostics::~NetworkDiagnostics() {
    if (!m_state->worker.joinable()) return;
    { std::lock_guard<std::mutex> lock(m_state->mutex); m_state->stopping = true; }
    m_state->wake.notify_one();
    // Production service has process lifetime. A deliberately blocked disk must
    // never hold up application shutdown; detached worker owns its remaining state.
    m_state->worker.detach();
}
void NetworkDiagnostics::enqueue(const QString& event, const QJsonObject& fields, bool critical) {
    if (!m_state->options.enabled) return;
    static const QRegularExpression validEvent(QStringLiteral("^[a-z][a-z0-9_]{0,63}$"));
    if (!validEvent.match(event).hasMatch()) return;
    std::unique_lock<std::mutex> lock(m_state->mutex, std::try_to_lock);
    if (!lock.owns_lock()) { ++m_state->dropped; return; }
    const qint64 now = MouffetteClock::nowMs();
    if (now - m_state->globalStart >= 1000) { m_state->globalStart = now; m_state->globalEmitted = 0; }
    if (m_state->globalEmitted >= 64 || m_state->queue.size() >= size_t(m_state->options.queueCapacity)) {
        ++m_state->dropped; return;
    }
    if (!m_state->rates.contains(event) && m_state->rates.size() >= 128) { ++m_state->dropped; return; }
    auto& rate = m_state->rates[event];
    if (now - rate.start >= 1000) { rate.start = now; rate.emitted = 0; }
    if (!critical && rate.emitted >= (m_state->options.verbose ? 4 : 1)) { ++rate.suppressed; return; }
    QJsonObject safe = safeFields(fields);
    safe.insert("critical", critical);
    QByteArray bytes = line(event, safe);
    if (bytes.size() > 4096) { ++m_state->dropped; return; }
    ++rate.emitted;
    ++m_state->globalEmitted;
    m_state->queue.push_back(std::move(bytes));
    lock.unlock();
    m_state->wake.notify_one();
}
quint64 NetworkDiagnostics::droppedCount() const { return m_state->dropped.load(); }
void NetworkDiagnostics::record(const QString& event, const QJsonObject& fields, bool critical) {
    static NetworkDiagnostics* service = [] {
        const auto& config = AppConfig::instance();
        Options options;
        options.enabled = config.networkDiagnostics();
        options.verbose = config.networkDiagnosticsVerbose();
        auto* logger = new NetworkDiagnostics(QDir(RuntimeProfile::appDataLocation()).filePath("diagnostics"), options);
        logger->enqueue("effective_configuration", {{"retentionMs", config.remoteMediaRetentionMs()},
            {"cacheMaxMiB", config.remoteMediaCacheMaxMiB()}, {"verbose", config.networkDiagnosticsVerbose()},
            {"uploadConcurrency", config.uploadConcurrency()}, {"uploadIdleTimeoutMs", config.uploadIdleTimeoutMs()},
            {"controlRequestRetryMs", config.controlRequestRetryMs()},
            {"uploadChannelRetryBaseMs", config.uploadChannelRetryBaseMs()},
            {"uploadChannelRetryMaxMs", config.uploadChannelRetryMaxMs()},
            {"uploadChannelAttemptTimeoutMs", config.uploadChannelAttemptTimeoutMs()},
            {"reconnectFastStepMs", config.reconnectFastStepMs()}, {"reconnectFastMaxMs", config.reconnectFastMaxMs()},
            {"reconnectBaseMs", config.reconnectBaseMs()}, {"reconnectMaxMs", config.reconnectMaxMs()},
            {"reconnectJitterPercent", config.reconnectJitterPercent()},
            {"leaseHealthCheckIntervalMs", config.leaseHealthCheckIntervalMs()},
            {"sessionDeadlinePollIntervalMs", config.sessionDeadlinePollIntervalMs()}}, true);
        return logger;
    }();
    service->enqueue(event, fields, critical);
}
