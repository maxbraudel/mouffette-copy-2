#pragma once

#include "backend/media/ResidentMediaAsset.h"

// Disposable, versioned editing derivatives. All I/O is worker-only. Original
// assets and project identities never depend on the existence of this cache.
class MediaPreviewStore final {
public:
    static bool supportsScrubProxy(const ResidentMediaAsset& asset);
    static QImage thumbnail(const ResidentMediaAsset& asset, int index);
    static void storeThumbnail(const ResidentMediaAsset& asset, int index, const QImage& image);
    static QVideoFrame scrubFrame(const ResidentMediaAsset& asset, int index);
    static bool hasScrubFrame(const ResidentMediaAsset& asset, int index);
    static bool storeScrubFrame(const ResidentMediaAsset& asset, int index, const QImage& image);
    static constexpr int ProxyExtent = 960;
};
