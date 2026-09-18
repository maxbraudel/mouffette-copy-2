#pragma once

#include <QRandomGenerator>
#include <algorithm>
#include <functional>
#include <limits>

// Policy has no configuration singleton or clock. A maximum is a hard ceiling,
// including jitter. The random source is injectable for boundary tests.
struct RetryPolicy {
    enum class Growth { Exponential, Linear, Fixed };
    int baseMs = 1000;
    int maximumMs = 5000;
    int jitterPercent = 20;
    Growth growth = Growth::Exponential;
    bool randomInitial = false;
    using Random = std::function<quint64()>;

    int delay(int attempt, Random random = [] { return QRandomGenerator::global()->generate64(); }) const {
        attempt = std::clamp(attempt, 0, 30);
        const qint64 maximum = std::max(1, maximumMs);
        const qint64 base = std::clamp<qint64>(baseMs, 1, maximum);
        qint64 nominal = base;
        if (growth == Growth::Exponential) nominal = std::min(maximum, base << attempt);
        if (growth == Growth::Linear) nominal = std::min(maximum, base * std::max(1, attempt));
        qint64 minimum = 1, upper = maximum;
        if (randomInitial && attempt == 0) { minimum = 0; upper = base; }
        else {
            const qint64 spread = nominal * std::clamp(jitterPercent, 0, 50) / 100;
            minimum = std::max<qint64>(1, nominal - spread);
            upper = std::min(maximum, nominal + spread);
        }
        return static_cast<int>(minimum + random() % static_cast<quint64>(upper - minimum + 1));
    }
    static int increment(int attempt) { return std::clamp(attempt, 0, 29) + 1; }
};

// Tracks uninterrupted readiness, never authentication age or elapsed wall time.
class StableConnectionWindow {
public:
    bool transition(bool healthy, qint64 now, qint64 requiredMs,
                    qint64 maximumObservationGapMs = std::numeric_limits<qint64>::max()) {
        if (m_lastObservation >= 0 && (now < m_lastObservation
            || now - m_lastObservation >= maximumObservationGapMs)) reset();
        const bool satisfied = m_since >= 0 && now >= m_since
            && now - m_since >= requiredMs && !m_reported;
        if (satisfied) m_reported = true;
        if (!healthy) reset();
        else if (m_since < 0) { m_since = now; m_reported = false; }
        if (healthy) m_lastObservation = now;
        return satisfied;
    }
    void reset() { m_since = -1; m_lastObservation = -1; m_reported = false; }
private:
    qint64 m_since = -1;
    qint64 m_lastObservation = -1;
    bool m_reported = false;
};
