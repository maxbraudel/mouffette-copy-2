#pragma once

#include "backend/media/ResidentMediaAsset.h"
#include <functional>

class MediaDecoder final
{
public:
    struct Geometry {
        bool video = false;
        QSize displaySize;
        qint64 durationUs = 0;
        QString error;
        bool accepted() const { return error.isEmpty() && displaySize.isValid(); }
    };
    struct Probe {
        bool video = false;
        QSize displaySize;
        quint64 estimatedBytes = 0;
        quint64 scratchBytes = 64ULL * 1024 * 1024;
        quint64 playbackBudgetBytes = 0;
        qint64 durationUs = 0;
        QString error;
        bool accepted() const { return error.isEmpty() && displaySize.isValid(); }
    };
    struct DecodeCallbacks {
        std::function<bool()> cancelled;
        // The TOTAL allocation owned by this job, including decoder scratch.
        // Called before growth; false aborts without publishing partial data.
        std::function<bool(quint64)> reserve;
        // Retained allocations only, excluding projected growth and scratch.
        std::function<void(quint64)> allocated;
        std::function<void(const ResidentMediaMemory&)> allocatedBreakdown;
        std::function<void(double)> progress;
    };

    // Worker-thread APIs: never construct QMediaPlayer or enter an event loop.
    // Import geometry does not establish residency or replace full validation.
    static Geometry inspectGeometry(const QString& path,
                                    const std::function<bool()>& cancelled = {});
    static Probe probe(const QString& path);
    static std::shared_ptr<ResidentMediaAsset> decode(
        const QString& path, const DecodeCallbacks& callbacks = {},
        QString* error = nullptr);
};
