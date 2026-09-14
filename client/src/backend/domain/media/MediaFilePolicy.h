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
inline constexpr quint64 MaximumPreparedImageBytes = 1024ULL * 1024ULL * 1024ULL;

// Classifies an existing local file from both its extension and its contents.
// Video support is deliberately strict: only an ISO-BMFF/MP4 file whose final
// extension is .mp4 and from which Qt's FFmpeg media pipeline actually decodes
// a valid video frame is accepted. A readable .mp4 that fails structural or
// decode validation is reported as UnsupportedVideo so callers can explain the
// video format restriction instead of treating it as an unrelated unknown file.
Kind classifyLocalFile(const QString& path);

// preparedImageBytes is the decoded-RGBA total already reserved by the scene
// being prepared. Supplying it lets both peers enforce the immutable 1 GiB
// preparation budget while validating each manifest entry.
ValidationResult validateLocalFile(const QString& path,
                                   quint64 preparedImageBytes = 0);

// Stable user-facing explanation for a validation failure. Callers should use
// this instead of inventing UI messages from the extension or media kind.
QString validationErrorDescription(const ValidationResult& validation);

// Validates an entire immutable scene revision in order and reserves decoded
// RGBA bytes cumulatively. The receiving renderer uses this function before
// constructing a render graph, so the 1 GiB budget cannot be bypassed by
// splitting images across several manifest entries. The sender relies on the
// upload validation receipt plus the immutable SHA-256 identity instead of
// decoding every asset again for each launch.
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
