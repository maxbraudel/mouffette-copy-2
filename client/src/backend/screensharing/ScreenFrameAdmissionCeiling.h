#pragma once

#include "ScreenStreamCodec.h"
#include <cmath>

// Per-layer ceiling learned from actual encoded access-unit sizes. This policy
// does not estimate link capacity or keep packets: it makes an IDR small enough
// to fit the transport's existing byte/time admission bound.
struct ScreenFrameAdmissionCeiling {
    // True only when applying the resulting ceiling can reduce this profile.
    // Invalid/within-budget samples do nothing. Repeated failures at the floor
    // retain the hold without asking the caller to reopen the encoder forever.
    bool limit(const ScreenStreamProfile& current, qint64 bytes, qint64 maximumBytes, qint64 now) {
        if (bytes <= 0 || maximumBytes <= 0 || bytes <= maximumBytes || now < 0) return false;
        m_lastRejection = std::max(m_lastRejection, now);
        if (m_lastReduction >= 0 && now - m_lastReduction < ReductionIntervalMs) return false;
        const auto value = current.normalized();
        const double ratio = double(maximumBytes) / double(bytes);
        const int edge = std::max(160, int(value.maximumEdge * std::sqrt(ratio) * .85) & ~1);
        const int bitrate = std::max(32000, int(value.bitrateBps * ratio * .85));
        const int fps = std::max(1, value.framesPerSecond * 4 / 5);
        const int nextEdge = std::min(m_maximumEdge, edge);
        const int nextBitrate = std::min(m_bitrateBps, bitrate);
        const int nextFps = std::min(m_framesPerSecond, fps);
        const bool changed = nextEdge != m_maximumEdge || nextBitrate != m_bitrateBps
            || nextFps != m_framesPerSecond;
        m_limited = true;
        m_maximumEdge = nextEdge;
        m_bitrateBps = nextBitrate;
        m_framesPerSecond = nextFps;
        if (changed) {
            m_lastReduction = now;
            m_lastRecovery = now;
        }
        return changed && (nextEdge < value.maximumEdge || nextBitrate < value.bitrateBps
            || nextFps < value.framesPerSecond);
    }

    ScreenStreamProfile apply(ScreenStreamProfile desired, qint64 now) {
        desired = desired.normalized();
        if (!m_limited) return desired;
        // Recover at most one step per call/second, even after a long pause.
        // Retaining unused headroom as a ceiling also prevents a later viewport
        // expansion from jumping straight back to a previously rejected IDR.
        if (now >= m_lastRejection && now - m_lastRejection >= RecoveryHoldMs
            && now >= m_lastRecovery && now - m_lastRecovery >= RecoveryIntervalMs) {
            if (desired.maximumEdge > m_maximumEdge)
                m_maximumEdge = std::min(desired.maximumEdge,
                    (m_maximumEdge + std::max(16, m_maximumEdge / 10)) & ~1);
            if (desired.bitrateBps > m_bitrateBps)
                m_bitrateBps = std::min(desired.bitrateBps,
                    m_bitrateBps + std::max(32000, m_bitrateBps / 10));
            if (desired.framesPerSecond > m_framesPerSecond)
                m_framesPerSecond = std::min(desired.framesPerSecond,
                    m_framesPerSecond + std::max(1, m_framesPerSecond / 10));
            m_lastRecovery = now;
        }
        desired.maximumEdge = std::min(desired.maximumEdge, m_maximumEdge);
        desired.bitrateBps = std::min(desired.bitrateBps, m_bitrateBps);
        desired.framesPerSecond = std::min(desired.framesPerSecond, m_framesPerSecond);
        return desired;
    }

private:
    static constexpr qint64 ReductionIntervalMs = 500;
    static constexpr qint64 RecoveryHoldMs = 5000;
    static constexpr qint64 RecoveryIntervalMs = 1000;
    int m_maximumEdge = 3840;
    int m_bitrateBps = 100000000;
    int m_framesPerSecond = 60;
    bool m_limited = false;
    qint64 m_lastRejection = -1;
    qint64 m_lastReduction = -1;
    qint64 m_lastRecovery = -1;
};
