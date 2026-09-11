#pragma once

#include <QString>

namespace MediaFilePolicy {

enum class Kind {
    Image,
    Mp4Video,
    UnsupportedVideo,
    Unsupported
};

// Classifies an existing local file from both its extension and its contents.
// Video support is deliberately strict: only an ISO-BMFF/MP4 file whose final
// extension is .mp4 is accepted. A readable .mp4 that fails structural
// validation is reported as UnsupportedVideo so callers can explain the video
// format restriction instead of treating it as an unrelated unknown file.
Kind classifyLocalFile(const QString& path);

bool isAcceptedLocalFile(const QString& path);
bool isMp4Video(const QString& path);
bool isKnownVideoExtension(const QString& extension);

} // namespace MediaFilePolicy
