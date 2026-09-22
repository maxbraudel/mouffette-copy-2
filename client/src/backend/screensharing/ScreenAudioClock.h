#pragma once
#include <QtGlobal>
#include "backend/audiosharing/AudioOutputTiming.h"
#include <algorithm>
#include <cstdlib>

// Source media time and local scheduling time are different clock domains.
// The paired source/output anchor is sampled in the worker. IPC arrival must
// never become its local origin: GUI stalls would otherwise delay video again.
class ScreenAudioClock {
public:
    static constexpr qint64 MaximumVideoWaitUs = 150000;
    static constexpr qint64 MaximumClockAgeUs = 250000;
    static constexpr qint64 MaximumClockLeadUs = 100000;
    void setOutputQuantumUs(qint64 quantumUs) {
        m_outputQuantumUs = AudioOutputTiming::normalizeQuantumUs(quantumUs);
    }
    qint64 maximumVideoWaitUs() const {
        return AudioOutputTiming::maximumPlayoutDelayUs(m_outputQuantumUs);
    }
    qint64 maximumClockLeadUs() const {
        return std::max(MaximumClockLeadUs, 2 * m_outputQuantumUs);
    }
    void update(qint64 sourceUs, qint64 localUs) {
        if (sourceUs < 0 || localUs < 0 || localUs < m_localUs) return;
        m_sourceUs = sourceUs;
        m_localUs = localUs;
    }
    void reset() { m_sourceUs = -1; m_localUs = -1; }
    qint64 videoDelayUs(qint64 videoUs, qint64 nowUs) const {
        const qint64 age = nowUs - m_localUs;
        // A callback can report a sample already submitted for presentation in
        // the next device period. Extrapolate this paired anchor in either
        // direction; rejecting all future anchors disables sync on fast IPC.
        if (m_sourceUs < 0 || videoUs < 0 || age < -maximumClockLeadUs() || age > MaximumClockAgeUs) return 0;
        const qint64 lead = videoUs - (m_sourceUs + age);
        if (std::llabs(lead) > 2000000) return 0;
        return std::clamp<qint64>(lead, 0, maximumVideoWaitUs());
    }
private:
    qint64 m_sourceUs = -1, m_localUs = -1;
    qint64 m_outputQuantumUs = AudioOutputTiming::DefaultQuantumUs;
};
