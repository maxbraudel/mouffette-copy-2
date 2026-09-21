#pragma once

#include "ScreenStreamCodec.h"
#include <QHash>

// Source-side allocation only. Subscriber counts and subscriber bandwidth do
// not enter this calculation: each produced layer crosses the uplink once.
struct ScreenPublicationProfiles {
    struct Limits {
        int maximumEdge = 3840;
        int maximumFps = 30;
        int idleIntervalMs = 1000;
        int keyFrameIntervalMs = 4000;
        QString softwarePreset = QStringLiteral("veryfast");
        bool lowEnabled = true;
        int lowMaximumEdge = 960;
        int lowMaximumFps = 20;
        int lowMaximumBitrateBps = 750000;
        int lowMinimumTotalBitrateBps = 600000;
    };

    static ScreenStreamProfile profile(int bitrate, int edge, const Limits& limits) {
        ScreenStreamProfile result;
        result.bitrateBps = std::max(32000, bitrate);
        result.maximumEdge = 320;
        result.framesPerSecond = 5;
        struct Level { int bitrate; int edge; int fps; };
        static const Level levels[] = {{220000,480,8}, {400000,640,12}, {700000,960,15},
            {1100000,1280,24}, {2200000,1920,30}, {4500000,2560,30},
            {8000000,3840,30}, {16000000,3840,60}};
        for (const auto& level : levels) if (bitrate >= level.bitrate) {
            result.maximumEdge = level.edge; result.framesPerSecond = level.fps;
        }
        result.maximumEdge = std::min({result.maximumEdge, edge, limits.maximumEdge});
        result.framesPerSecond = std::min(result.framesPerSecond, limits.maximumFps);
        result.idleIntervalMs = limits.idleIntervalMs;
        result.keyFrameIntervalMs = limits.keyFrameIntervalMs;
        result.softwarePreset = limits.softwarePreset;
        return result.normalized();
    }

    static QHash<QString, ScreenStreamProfile> select(int budget, int edge, bool lowRequested,
                                                     bool lowSuppressed, bool lowAlreadyActive,
                                                     const Limits& limits) {
        const QString main = QStringLiteral("main"), low = QStringLiteral("low");
        // A 20% hold band avoids creating/destroying a second encoder around
        // the uplink threshold. CPU/encoder failure suppression is immediate.
        const int threshold = lowAlreadyActive ? limits.lowMinimumTotalBitrateBps * 4 / 5
                                              : limits.lowMinimumTotalBitrateBps;
        const bool dual = limits.lowEnabled && lowRequested && !lowSuppressed
            && budget >= std::max(64000, threshold);
        if (!dual) return {{main, profile(budget, edge, limits)}};
        const int lowBudget = std::min(limits.lowMaximumBitrateBps, std::max(32000, budget / 4));
        auto mainProfile = profile(budget - lowBudget, edge, limits);
        auto lowProfile = profile(lowBudget, std::min(edge, limits.lowMaximumEdge), limits);
        lowProfile.framesPerSecond = std::min(lowProfile.framesPerSecond, limits.lowMaximumFps);
        // At low source quality an extra encoder buys no useful size/cadence
        // distinction. Use the same main layer for every consumer instead.
        if (mainProfile.maximumEdge <= lowProfile.maximumEdge
            && mainProfile.framesPerSecond <= lowProfile.framesPerSecond)
            return {{main, profile(budget, edge, limits)}};
        return {{main, mainProfile}, {low, lowProfile}};
    }
};
