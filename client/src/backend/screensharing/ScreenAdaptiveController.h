#pragma once

#include <QHash>
#include <QString>
#include <QtGlobal>
#include <algorithm>

// Application policy for the reliable video fallback. This is not a link-speed
// estimator: receipt RTT contains propagation, buffering and receiver scheduling.
// Each independently measured leg has its own baseline; a healthy satellite
// connection must not be mistaken for a growing queue.
class ScreenAdaptiveController final {
public:
    struct Limits {
        bool adaptive = true;
        int minimumBps = 128000;
        int initialBps = 1200000;
        int maximumBps = 12000000;
        int uploadBps = 1000000;
        int queueTargetMs = 150;
        int feedbackIntervalMs = 500;
        int recoveryHoldMs = 5000;
    };

    explicit ScreenAdaptiveController(Limits limits) : m_limits(limits) {
        m_budget = std::clamp(limits.initialBps, limits.minimumBps, limits.maximumBps);
    }

    void observe(const QString& leg, int rttMs, bool congested, bool uploadActive, qint64 now) {
        auto& sample = m_legs[leg];
        if (sample.at < 0 || now - sample.at > 30000) sample.baseline = -1;
        sample.at = now;
        if (rttMs >= 0) {
            if (sample.baseline < 0 || now - sample.baselineAt >= 30000 || rttMs <= sample.baseline) {
                sample.baseline = rttMs;
                sample.baselineAt = now;
            }
            congested |= rttMs - sample.baseline > m_limits.queueTargetMs;
        }
        sample.congested = congested;
        sample.upload = uploadActive;
        m_lastFeedback = now;
        if (congested) penalize(now);
        prune(now);
    }

    void penalize(qint64 now) {
        m_nextIncrease = now + m_limits.recoveryHoldMs;
        if (!m_limits.adaptive || (m_lastDecrease >= 0
            && now - m_lastDecrease < m_limits.feedbackIntervalMs)) return;
        m_budget = std::max(m_limits.minimumBps, int(m_budget * .65));
        m_lastDecrease = now;
    }

    int budget(qint64 now, bool localUpload) {
        prune(now);
        bool remoteUpload = false, recentCongestion = false;
        for (const auto& sample : m_legs) {
            if (now - sample.at > std::max(2000, m_limits.feedbackIntervalMs * 3)) continue;
            remoteUpload |= sample.upload;
            recentCongestion |= sample.congested;
        }
        if (m_nextIncrease < 0) m_nextIncrease = now + m_limits.recoveryHoldMs;
        if (m_limits.adaptive && !recentCongestion && !localUpload && !remoteUpload
            && m_lastFeedback >= 0 && now - m_lastFeedback < 2000
            && now >= m_nextIncrease) {
            m_budget = std::min(m_limits.maximumBps, m_budget + std::max(32000, m_budget / 5));
            m_nextIncrease = now + m_limits.recoveryHoldMs;
        }
        const int result = m_limits.adaptive ? m_budget : m_limits.maximumBps;
        return (localUpload || remoteUpload) ? std::min(result, m_limits.uploadBps) : result;
    }

    // A new transport has no valid latency samples. Retain a conservative
    // quality estimate instead of jumping back to full quality after an outage.
    void transportReset(qint64 now) {
        m_legs.clear();
        m_lastFeedback = -1;
        m_budget = std::min(m_budget, m_limits.initialBps);
        m_nextIncrease = now + m_limits.recoveryHoldMs;
    }

private:
    struct Sample { qint64 at = -1; qint64 baselineAt = -1; int baseline = -1; bool congested = false; bool upload = false; };
    void prune(qint64 now) {
        for (auto it = m_legs.begin(); it != m_legs.end();) {
            if (now - it->at > 30000) it = m_legs.erase(it); else ++it;
        }
    }
    Limits m_limits;
    QHash<QString, Sample> m_legs;
    int m_budget = 0;
    qint64 m_lastFeedback = -1;
    qint64 m_lastDecrease = -1;
    qint64 m_nextIncrease = -1;
};
