#pragma once
#include <QLoggingCategory>
#include <QtGlobal>
#include <algorithm>
#include <limits>

inline const QLoggingCategory& audioCaptureTimingLog() {
    static const QLoggingCategory category("mouffette.audio.capture.timing", QtWarningMsg);
    return category;
}

// Opt-in timing only: no samples, sound levels, device names, or media content.
// QT_LOGGING_RULES=mouffette.audio.capture.timing.debug=true enables one line/s.
class AudioCaptureTiming {
public:
    void observe(const char* backend, qint64 arrivalUs, qint64 nativeTimestampUs,
                 qint64 mappedTimestampUs, int frames) {
        if (!audioCaptureTimingLog().isDebugEnabled()) return;
        if (lastArrivalUs) maximumArrivalGapUs = std::max(maximumArrivalGapUs, arrivalUs - lastArrivalUs);
        lastArrivalUs = arrivalUs;
        const auto age = arrivalUs - mappedTimestampUs;
        minimumAgeUs = std::min(minimumAgeUs, age); maximumAgeUs = std::max(maximumAgeUs, age);
        totalAgeUs += age; totalFrames += frames; ++buffers;
        if (arrivalUs < nextReportUs) return;
        qCDebug(audioCaptureTimingLog).nospace()
            << "backend=" << backend << " buffers=" << buffers << " frames=" << totalFrames
            << " native_age_us=" << (arrivalUs - nativeTimestampUs)
            << " clock_offset_us=" << (mappedTimestampUs - nativeTimestampUs)
            << " mapped_age_us_avg=" << (totalAgeUs / buffers)
            << " mapped_age_us_min=" << minimumAgeUs << " mapped_age_us_max=" << maximumAgeUs
            << " arrival_gap_us_max=" << maximumArrivalGapUs;
        nextReportUs = arrivalUs + 1000000;
        minimumAgeUs = std::numeric_limits<qint64>::max(); maximumAgeUs = std::numeric_limits<qint64>::min();
        totalAgeUs = 0; totalFrames = 0; buffers = 0; maximumArrivalGapUs = 0;
    }
private:
    qint64 nextReportUs = 0, lastArrivalUs = 0, maximumArrivalGapUs = 0;
    qint64 minimumAgeUs = std::numeric_limits<qint64>::max(), maximumAgeUs = std::numeric_limits<qint64>::min();
    qint64 totalAgeUs = 0, totalFrames = 0, buffers = 0;
};
