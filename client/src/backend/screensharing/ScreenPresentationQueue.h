#pragma once
#include "ScreenAudioClock.h"
#include <deque>
#include <iterator>
#include <optional>
#include <utility>

// Decoding and presentation have separate lifetimes. Retain a small ordered
// window so a new frame never inherits the deadline of an older image. Costs
// describe retained decoded surfaces, not compressed network packet sizes.
template<class Frame>
class ScreenPresentationQueue {
public:
    static constexpr size_t MaximumFrames = 12;
    static constexpr qint64 MaximumBytes = 64 * 1024 * 1024;
    bool empty() const { return m_frames.empty(); }
    size_t size() const { return m_frames.size(); }
    qint64 bytes() const { return m_bytes; }
    void clear() { m_frames.clear(); m_bytes = 0; }
    void push(Frame frame, qint64 sourceUs, qint64 nowUs, qint64 bytes,
              qint64 budget = MaximumBytes) {
        // A source epoch change is fenced by the owner. Repeated timestamps can
        // replace only the same image, without moving its maximum-wait origin.
        if (!empty() && sourceUs < m_frames.back().sourceUs) return;
        if (!empty() && sourceUs == m_frames.back().sourceUs) {
            nowUs = m_frames.back().queuedUs;
            m_bytes -= m_frames.back().bytes;
            m_frames.pop_back();
        }
        bytes = std::max<qint64>(0, bytes);
        m_frames.push_back({std::move(frame), sourceUs, nowUs, bytes});
        m_bytes += bytes;
        // Keep the earliest pending image: continually evicting it in favour
        // of newer future images would freeze a busy screen forever when the
        // budget holds less than one jitter window. Retain the latest image
        // too when possible, thinning the intervening future images instead.
        // A single surface can exceed its share of the memory budget.
        while (size() > 1 && (size() > MaximumFrames || m_bytes > budget)) {
            const auto discard = size() > 2 ? std::prev(m_frames.end(), 2) : std::prev(m_frames.end());
            m_bytes -= discard->bytes;
            m_frames.erase(discard);
        }
    }
    std::optional<Frame> takeReady(const ScreenAudioClock& clock, qint64 nowUs,
                                   bool immediate = false) {
        std::optional<Frame> result;
        while (!empty()) {
            const auto& next = m_frames.front();
            if (!immediate && nowUs - next.queuedUs < ScreenAudioClock::MaximumVideoWaitUs
                && clock.videoDelayUs(next.sourceUs, nowUs) > 0) break;
            result = std::move(m_frames.front().frame);
            pop();
        }
        // If several images are already due after a stall, show the newest due
        // image once; preserve any future images at their own source time.
        return result;
    }
private:
    struct Entry { Frame frame; qint64 sourceUs, queuedUs, bytes; };
    std::deque<Entry> m_frames;
    qint64 m_bytes = 0;
    void pop() { m_bytes -= m_frames.front().bytes; m_frames.pop_front(); }
};
