#pragma once

#include "backend/config/AppConfig.h"
#include "backend/network/RetryPolicy.h"
#include "backend/network/RetryScheduler.h"

// Owns retry timing for a new operation after a definitive transient OPEN
// result. WebSocketClient owns retransmission of the *same* pending request.
// Neither controller uses a project, a Canvas, or a display status as identity.
class SessionRecoveryController {
public:
    struct Diagnostic {
        RetryAction action = RetryAction::Idle;
        qint64 nextAttemptAtMs = -1;
        int attempts = 0;
        QString reason;
    };
    explicit SessionRecoveryController(
        RetryScheduler::Clock clock = [] { return MouffetteClock::nowMs(); },
        RetryPolicy::Random random = [] { return QRandomGenerator::global()->generate64(); })
        : m_scheduler(nullptr, std::move(clock)), m_random(std::move(random)) {}

    std::function<void(const QString&)> ready;
    std::function<void()> changed;
    void retry(const QString& endpoint, const QString& reason) {
        if (!admit(endpoint) || waiting(endpoint)) return;
        const auto& config = AppConfig::instance();
        auto& state = m_states[endpoint];
        const int delay = RetryPolicy{config.sessionRetryBaseMs(), config.sessionRetryMaxMs(),
                                     config.reconnectJitterPercent()}.delay(state.attempts, m_random);
        state.attempts = RetryPolicy::increment(state.attempts);
        state.action = RetryAction::Scheduled;
        state.reason = reason;
        m_scheduler.schedule(endpoint, delay, [this, endpoint] {
            m_states[endpoint].action = RetryAction::Idle;
            if (changed) changed();
            if (ready) ready(endpoint);
        });
        if (changed) changed();
    }
    // Presence can expedite a waiting operation without clearing failure history.
    // Successful OPEN or explicit selection resets it.
    void expedite(const QString& endpoint) {
        m_scheduler.cancel(endpoint);
        if (m_states.contains(endpoint)) m_states[endpoint].action = RetryAction::Idle;
        if (changed) changed();
    }
    void running(const QString& endpoint) { setAction(endpoint, RetryAction::Running, QStringLiteral("session_request_in_flight")); }
    void pause(const QString& endpoint, const QString& reason) { setAction(endpoint, RetryAction::Suspended, reason); }
    void block(const QString& endpoint, const QString& reason) { setAction(endpoint, RetryAction::Blocked, reason); }
    Diagnostic diagnostic(const QString& endpoint) const {
        auto result = m_states.value(endpoint);
        result.nextAttemptAtMs = m_scheduler.dueAt(endpoint);
        return result;
    }
    bool waiting(const QString& endpoint) const { return m_scheduler.contains(endpoint); }
    qint64 nextAttemptAtMs(const QString& endpoint) const { return m_scheduler.dueAt(endpoint); }
    QString reason(const QString& endpoint) const { return m_states.value(endpoint).reason; }
    void reset(const QString& endpoint) {
        m_scheduler.cancel(endpoint);
        m_states.remove(endpoint);
        if (changed) changed();
    }
    void clear() { m_scheduler.cancelAll(); m_states.clear(); }
    void suspend() {
        m_scheduler.cancelAll();
        for (auto& state : m_states) {
            if (state.action != RetryAction::Blocked) state.action = RetryAction::Suspended;
        }
    }
    // Also used to deterministically process a wake-up with an injected clock.
    void processDue() { m_scheduler.processDue(); }
private:
    bool admit(const QString& endpoint) {
        if (endpoint.isEmpty()) return false;
        if (!m_states.contains(endpoint) && m_states.size() >= 4096) {
            qWarning("Session retry capacity exhausted");
            return false;
        }
        return true;
    }
    void setAction(const QString& endpoint, RetryAction action, const QString& reason) {
        if (!admit(endpoint)) return;
        if (action == RetryAction::Suspended && m_states.value(endpoint).action == RetryAction::Blocked) return;
        m_scheduler.cancel(endpoint);
        auto& state = m_states[endpoint];
        if (state.action == action && state.reason == reason) return;
        state.action = action;
        state.reason = reason;
        if (changed) changed();
    }
    RetryScheduler m_scheduler;
    RetryPolicy::Random m_random;
    QHash<QString, Diagnostic> m_states;
};
