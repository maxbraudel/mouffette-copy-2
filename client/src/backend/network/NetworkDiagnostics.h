#pragma once

#include <QJsonObject>
#include <QString>
#include <memory>

// No file I/O or unbounded Qt queued invocation occurs on a producer thread.
// Only allowlisted scalar metadata enters the log; never pass payloads/paths.
class NetworkDiagnostics final {
public:
    struct Options {
        bool enabled = true;
        bool verbose = false;
        int queueCapacity = 1024;
        qint64 fileBytes = 5 * 1024 * 1024;
    };
    explicit NetworkDiagnostics(const QString& directory, Options options);
    ~NetworkDiagnostics();
    NetworkDiagnostics(const NetworkDiagnostics&) = delete;
    NetworkDiagnostics& operator=(const NetworkDiagnostics&) = delete;
    void enqueue(const QString& event, const QJsonObject& fields = {}, bool critical = false);
    quint64 droppedCount() const;
    static void record(const QString& event, const QJsonObject& fields = {}, bool critical = false);
private:
    struct State;
    std::shared_ptr<State> m_state;
};
