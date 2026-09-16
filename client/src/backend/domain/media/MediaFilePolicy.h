#pragma once

#include <QImage>
#include <QList>
#include <QSize>
#include <QString>

namespace MediaFilePolicy {

enum class Kind {
    Image,
    Mp4Video,
    UnsupportedVideo,
    Unsupported
};

struct ValidationResult {
    Kind kind = Kind::Unsupported;
    QSize imageSize;
    QSize videoSize;
    QImage videoFirstFrame;
    quint64 decodedRgbaBytes = 0;
    QString errorCode;

    bool accepted() const {
        return kind == Kind::Image || kind == Kind::Mp4Video;
    }
};

// A scene-preparation entry couples the untrusted declared media kind with the
// concrete local file that will actually be decoded.  assetId is used only to
// identify a rejected entry to the caller; it is never trusted as a path.
struct PreparationAsset {
    QString assetId;
    QString path;
    Kind expectedKind = Kind::Unsupported;
};

struct PreparationValidationResult {
    bool accepted = false;
    quint64 totalDecodedRgbaBytes = 0;
    QString failedAssetId;
    QString errorCode;
};

inline constexpr quint64 MaximumImagePixels = 64ULL * 1000ULL * 1000ULL;

// Structural/header validation. Does not decode pixels or create a player;
// checks installed video decoder capabilities for the resident decoder.
ValidationResult validateLocalFileMetadata(const QString& path);

// Import-only structural/header validation with the same format/codec allowlist.
// Defers installed video decoder discovery so obtaining shell geometry never
// initializes the Qt multimedia backend. Residency still validates capabilities
// and complete decoding before any content becomes ready.
ValidationResult validateLocalFileGeometry(const QString& path);

// Classifies an existing local file from both its extension and its contents.
// Video support is deliberately strict: only an ISO-BMFF/MP4 file whose final
// extension is .mp4 and from which Qt's FFmpeg media pipeline actually decodes
// a valid video frame is accepted. A readable .mp4 that fails structural or
// decode validation is reported as UnsupportedVideo so callers can explain the
// video format restriction instead of treating it as an unrelated unknown file.
Kind classifyLocalFile(const QString& path);

// The legacy size argument is retained for source compatibility. Actual RAM
// admission belongs to MediaResidencyManager, including videos and scratch.
ValidationResult validateLocalFile(const QString& path,
                                   quint64 preparedImageBytes = 0);

// Stable user-facing explanation for a validation failure. Callers should use
// this instead of inventing UI messages from the extension or media kind.
QString validationErrorDescription(const ValidationResult& validation);

// Validates asset formats and computes decoded image bytes with checked
// arithmetic. Dynamic residency admission is a separate, shared authority.
PreparationValidationResult validatePreparationAssets(
    const QList<PreparationAsset>& assets,
    quint64 preparedImageBytes = 0);

bool isAcceptedLocalFile(const QString& path);
bool isMp4Video(const QString& path);
bool isKnownVideoExtension(const QString& extension);

// Recomputes the complete file digest without changing repository mappings.
// Scene preparation uses this immediately before publishing its immutable
// manifest so an in-place source modification cannot retain the old asset ID.
bool matchesSha256(const QString& path, const QString& expectedSha256);

} // namespace MediaFilePolicy
