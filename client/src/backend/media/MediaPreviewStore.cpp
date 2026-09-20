#include "backend/media/MediaPreviewStore.h"
#include "backend/media/ResidentVideoPlayer.h"
#include <QBuffer>
#include <QCryptographicHash>
#include <QDir>
#include <QFileInfo>
#include <QImageReader>
#include <QMutex>
#include <QSaveFile>
#include <QStandardPaths>
#include <map>
extern "C" {
#include <libavcodec/codec_par.h>
#include <libavutil/frame.h>
#include <libavutil/pixdesc.h>
}

namespace {
constexpr quint64 MiB = 1024ULL * 1024;
struct Entry { quint64 size; quint64 use; };
struct Cache {
    QMutex mutex;
    QString directory;
    QHash<QString, Entry> entries;
    std::map<quint64, QString> oldest;
    quint64 bytes = 0, sequence = 0;
    quint64 limit;
    const char* name;
    Cache(quint64 budget, const char* folder) : limit(budget), name(folder) {}
    void initialize() {
        if (!directory.isEmpty()) return;
        const QString root = QStandardPaths::writableLocation(QStandardPaths::CacheLocation);
        if (root.isEmpty()) return;
        directory = root + QStringLiteral("/media-previews-v1/") + QString::fromLatin1(name);
        QDir dir(directory);
        if (!dir.mkpath(QStringLiteral("."))) { directory.clear(); return; }
        const auto files = dir.entryInfoList({QStringLiteral("*.jpg")}, QDir::Files, QDir::Time | QDir::Reversed);
        for (const auto& file : files) {
            entries.insert(file.fileName(), {quint64(file.size()), ++sequence});
            oldest.emplace(sequence, file.fileName());
            bytes += quint64(file.size());
        }
        trim();
    }
    void trim() {
        // Bound both storage and filesystem metadata for very long sources.
        // Maintain the order incrementally: sorting all 12,000 entries for each
        // new frame makes long-source generation slow as soon as the cache fills.
        for (auto it = oldest.begin(); it != oldest.end();) {
            if (bytes <= limit && entries.size() <= 12000) break;
            const QString key = it->second;
            const QString path = directory + QLatin1Char('/') + key;
            if (!QFile::remove(path) && QFileInfo::exists(path)) { ++it; continue; }
            bytes -= entries.take(key).size;
            it = oldest.erase(it);
        }
    }
};
Cache& cache(bool proxy) {
    static Cache thumbs(64 * MiB, "thumbnails");
    static Cache proxies(512 * MiB, "scrub");
    return proxy ? proxies : thumbs;
}
QString identity(const ResidentMediaAsset& asset, int index, bool proxy) {
    if (index < 0 || index >= asset.frameIndex.size() || asset.sha256.isEmpty()) return {};
    const auto& time = asset.frameIndex[index];
    const QByteArray key = asset.sha256.toUtf8() + ':' + QByteArray::number(asset.videoTrack)
        + ':' + QByteArray::number(time.timestampUs) + ':' + QByteArray::number(time.durationUs)
        + ':' + QByteArray::number(asset.codedDisplaySize.width()) + ':' + QByteArray::number(asset.codedDisplaySize.height())
        + ':' + QByteArray::number(asset.rotation) + (proxy ? ":scrub960-v1" : ":thumb192x108-v1");
    return QString::fromLatin1(QCryptographicHash::hash(key, QCryptographicHash::Sha256).toHex()) + QStringLiteral(".jpg");
}
void discard(Cache& c, const QString& key) {
    QMutexLocker lock(&c.mutex);
    const QString path = c.directory + QLatin1Char('/') + key;
    if (!QFile::remove(path) && QFileInfo::exists(path)) return;
    if (auto it = c.entries.find(key); it != c.entries.end()) {
        c.bytes -= it->size; c.oldest.erase(it->use); c.entries.erase(it);
    }
}
QImage read(const ResidentMediaAsset& asset, int index, bool proxy) {
    const QString key = identity(asset, index, proxy);
    if (key.isEmpty()) return {};
    auto& c = cache(proxy);
    QByteArray encoded;
    {
        QMutexLocker lock(&c.mutex);
        c.initialize();
        if (c.directory.isEmpty()) return {};
        QFile file(c.directory + QLatin1Char('/') + key);
        if (!file.open(QIODevice::ReadOnly)) return {};
        if (file.size() <= 0 || file.size() > (proxy ? 4 * MiB : MiB)) return {};
        encoded = file.readAll();
        auto it = c.entries.find(key);
        if (it != c.entries.end()) {
            c.oldest.erase(it->use); it->use = ++c.sequence;
            c.oldest.emplace(it->use, key);
        }
    }
    QBuffer buffer(&encoded); buffer.open(QIODevice::ReadOnly);
    QImageReader reader(&buffer, "JPG");
    const QSize dimensions = reader.size();
    const int extent = proxy ? MediaPreviewStore::ProxyExtent : MediaThumbnails::Width;
    if (!dimensions.isValid() || dimensions.width() > extent || dimensions.height() > extent) {
        discard(c, key); return {};
    }
    const auto image = reader.read();
    if (image.isNull()) discard(c, key);
    return image;
}
bool write(const ResidentMediaAsset& asset, int index, const QImage& image, bool proxy) {
    const QString key = identity(asset, index, proxy);
    if (key.isEmpty() || image.isNull()) return false;
    QByteArray encoded;
    QBuffer buffer(&encoded); buffer.open(QIODevice::WriteOnly);
    if (!image.save(&buffer, "JPG", proxy ? 85 : 90)) return false;
    if (encoded.size() > (proxy ? 4 * MiB : MiB)) return false;
    auto& c = cache(proxy);
    QMutexLocker lock(&c.mutex);
    c.initialize();
    if (c.directory.isEmpty()) return false;
    QSaveFile file(c.directory + QLatin1Char('/') + key);
    if (!file.open(QIODevice::WriteOnly) || file.write(encoded) != encoded.size() || !file.commit()) return false;
    auto it = c.entries.find(key);
    if (it != c.entries.end()) { c.bytes -= it->size; c.oldest.erase(it->use); }
    c.entries.insert(key, {quint64(encoded.size()), ++c.sequence});
    c.oldest.emplace(c.sequence, key);
    c.bytes += quint64(encoded.size()); c.trim();
    return c.bytes <= c.limit && c.entries.size() <= 12000 && c.entries.contains(key);
}
}

bool MediaPreviewStore::supportsScrubProxy(const ResidentMediaAsset& asset) {
    if (!asset.video || asset.allIntra || !asset.videoPackets.codec || asset.frameIndex.isEmpty()) return false;
    const auto* format = av_pix_fmt_desc_get(AVPixelFormat(asset.videoPackets.codec->format));
    return format && !(format->flags & AV_PIX_FMT_FLAG_ALPHA) && format->comp[0].depth <= 8
        && asset.videoPackets.codec->color_trc != AVCOL_TRC_SMPTE2084
        && asset.videoPackets.codec->color_trc != AVCOL_TRC_ARIB_STD_B67;
}
QImage MediaPreviewStore::thumbnail(const ResidentMediaAsset& asset, int index) { return read(asset, index, false); }
void MediaPreviewStore::storeThumbnail(const ResidentMediaAsset& asset, int index, const QImage& image) {
    // JPEG must never discard an alpha channel that the timeline displays.
    const auto* pixel = asset.videoPackets.codec
        ? av_pix_fmt_desc_get(AVPixelFormat(asset.videoPackets.codec->format)) : nullptr;
    if (!image.hasAlphaChannel() || (pixel && !(pixel->flags & AV_PIX_FMT_FLAG_ALPHA))) write(asset, index, image, false);
}
bool MediaPreviewStore::hasScrubFrame(const ResidentMediaAsset& asset, int index) {
    auto& c = cache(true);
    const QString key = identity(asset, index, true);
    QMutexLocker lock(&c.mutex); c.initialize();
    if (key.isEmpty() || c.directory.isEmpty()) return false;
    QFile file(c.directory + QLatin1Char('/') + key);
    if (!file.open(QIODevice::ReadOnly) || file.size() < 4 || file.size() > 4 * MiB) return false;
    // Atomic writes normally guarantee a complete file; recover derivatives
    // left truncated/corrupt by external cleanup or earlier application builds.
    file.seek(file.size() - 2);
    if (file.read(2) != QByteArray::fromHex("ffd9")) return false;
    file.seek(0);
    QImageReader reader(&file, "JPG");
    const auto size = reader.size();
    return size.isValid() && size.width() <= ProxyExtent && size.height() <= ProxyExtent;
}
bool MediaPreviewStore::storeScrubFrame(const ResidentMediaAsset& asset, int index, const QImage& image) {
    return supportsScrubProxy(asset) && write(asset, index, image, true);
}
QVideoFrame MediaPreviewStore::scrubFrame(const ResidentMediaAsset& asset, int index) {
    if (!supportsScrubProxy(asset)) return {};
    QImage image = read(asset, index, true).convertToFormat(QImage::Format_RGBA8888);
    if (image.isNull()) return {};
    // Use the tracked native buffer path, so optional/visible preview frames
    // remain part of CPU allocation accounting and have immutable identities.
    AVFrame source{};
    source.format = AV_PIX_FMT_RGBA; source.width = image.width(); source.height = image.height();
    source.data[0] = image.bits(); source.linesize[0] = image.bytesPerLine();
    source.color_range = AVCOL_RANGE_JPEG; source.colorspace = AVCOL_SPC_RGB;
    source.color_primaries = AVCOL_PRI_BT709; source.color_trc = AVCOL_TRC_IEC61966_2_1;
    const auto time = asset.frameIndex[index];
    QString error;
    return mediaFrameFromAv(&source, image.size(), 0, time.timestampUs, time.timestampUs + time.durationUs, error);
}
