#pragma once
#include <QtGlobal>
#include <algorithm>
#include <cstdlib>

// Source media time and local scheduling time are different clock domains.
// Only this recent consumption anchor bridges them; network deadlines remain local.
class ScreenAudioClock {
public:
    void update(qint64 sourceUs, qint64 localUs) { m_sourceUs = sourceUs; m_localUs = localUs; }
    void reset() { m_sourceUs = -1; m_localUs = -1; }
    qint64 videoDelayUs(qint64 videoUs, qint64 nowUs) const {
        const qint64 age = nowUs - m_localUs;
        if (m_sourceUs < 0 || videoUs < 0 || age < 0 || age > 250000) return 0;
        const qint64 lead = videoUs - (m_sourceUs + age);
        if (std::llabs(lead) > 2000000) return 0;
        return std::clamp<qint64>(lead, 0, 40000);
    }
private:
    qint64 m_sourceUs = -1, m_localUs = -1;
};
