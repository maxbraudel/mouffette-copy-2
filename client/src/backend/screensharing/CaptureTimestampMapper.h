#pragma once
#include <QtGlobal>
#include <cstdlib>

// Map relative native timestamps once, preserving capture spacing rather than
// stamping at the encoder. Host-clock timestamps already use the shared epoch.
class CaptureTimestampMapper {
public:
    static qint64 hostTimestamp(qint64 nativeUs, qint64 receivedUs) {
        // A known host-clock PTS must survive delayed delivery unchanged.
        // Anchoring it to the first callback hides latency and gives every
        // audio/display capture a different source epoch.
        return nativeUs >= 0 ? nativeUs : receivedUs;
    }
    qint64 map(qint64 nativeUs, qint64 receivedUs) {
        if (nativeUs < 0) return receivedUs;
        if (std::llabs(nativeUs - receivedUs) < 1000000) return nativeUs;
        if (!m_anchored || nativeUs < m_lastNative
            || std::llabs(nativeUs + m_offset - receivedUs) > 2000000) {
            m_offset = receivedUs - nativeUs;
            m_anchored = true;
        }
        m_lastNative = nativeUs;
        return nativeUs + m_offset;
    }
private:
    bool m_anchored = false;
    qint64 m_offset = 0, m_lastNative = 0;
};
