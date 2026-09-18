#pragma once

#include "backend/config/AppConfig.h"

// Isolate configurable track-range scenarios from the user's environment.
struct TimelineTrackRangeConfig {
    AppConfig previous = AppConfig::instance();
    QString error;
    bool loaded = false;

    TimelineTrackRangeConfig(int above, int below)
    {
        AppConfig::LoadOptions options;
        options.defaultEnvFilePath.clear();
        options.processEnvironment.insert("MOUFFETTE_TIMELINE_MIN_TRACKS_ABOVE", QString::number(above));
        options.processEnvironment.insert("MOUFFETTE_TIMELINE_MIN_TRACKS_BELOW", QString::number(below));
        loaded = AppConfig::instance().load(options, &error);
    }
    ~TimelineTrackRangeConfig() { AppConfig::instance() = previous; }
};
