// Manual benchmark for the CURRENT original-source editing path. Reports sink
// delivery, not physical display scanout. Uses an isolated disposable disk cache.
#include "backend/media/MediaDecoder.h"
#include "backend/media/IndexedMediaDecoder.h"
#include "backend/media/ResidentVideoPlayer.h"
#include "backend/media/DecodeScheduler.h"
#include "backend/media/EditingProxyCache.h"
#include "backend/media/MediaPreviewStore.h"
#include <QGuiApplication>
#include <QVideoSink>
#include <QAudioOutput>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QTimer>
#include <QThread>
#include <QJsonDocument>
#include <QJsonObject>
#include <QStandardPaths>
#include <QDir>
#include <QUuid>
#include <algorithm>

static bool await(const std::function<bool()>& done, int timeout = 15000) {
    QElapsedTimer clock; clock.start();
    while (!done() && clock.elapsed() < timeout) { QCoreApplication::processEvents(); QThread::msleep(1); }
    return done();
}
static void spin(int ms) { QEventLoop loop; QTimer::singleShot(ms, &loop, &QEventLoop::quit); loop.exec(); }
static double p95(QVector<double> values) {
    if (values.isEmpty()) return -1;
    std::sort(values.begin(), values.end()); return values[std::min(values.size()-1, qsizetype(values.size()*.95))];
}
int main(int argc, char** argv) {
    QGuiApplication app(argc, argv);
    if (app.arguments().size() != 2) return 1;
    QCoreApplication::setApplicationName(QStringLiteral("MouffetteInteractionBenchmark-") + QUuid::createUuid().toString(QUuid::Id128));
    const QString cacheDirectory = QStandardPaths::writableLocation(QStandardPaths::CacheLocation);
    auto& proxies = EditingProxyCache::instance(); proxies.setEnabled(false);
    QElapsedTimer importing; importing.start();
    qint64 previewMs = -1;
    MediaDecoder::DecodeCallbacks callbacks; callbacks.retainOriginalVideo = true;
    callbacks.preview = [&](auto) { if (previewMs < 0) previewMs = importing.elapsed(); };
    QString error;
    auto asset = MediaDecoder::decode(app.arguments()[1], callbacks, &error);
    if (!asset) { qWarning() << error; return 2; }
    const qint64 validatedMs = importing.elapsed();
    QJsonObject result{{"qtVersion", QString::fromLatin1(qVersion())}, {"representation", "validated original"},
        {"firstPreviewMs", double(previewMs)}, {"validationMs", double(validatedMs)},
        {"frames", asset->frameIndex.size()}, {"durationMs", asset->durationUs/1000.0}};
    QAudioOutput audio; audio.setMuted(true);
    QVideoSink sink;
    ResidentVideoPlayer player;
    player.setAudioOutput(&audio); player.setVideoSink(&sink); player.setAsset(asset); player.prepare(0);
    if (!await([&] { return player.preparedAt(0); })) return 3;
    const qint64 maximum = std::max<qint64>(1, player.duration() - 100);
    auto seeks = [&](const QString& phase) {
        QVector<double> times;
        player.setScrubbing(true);
        for (int i = 0; i < 24; ++i) {
            const qint64 target = (qint64((i * 17) % 24) * maximum / 24) + 1;
            QElapsedTimer clock; clock.start(); player.setPosition(target);
            if (!await([&] { return player.preparedAt(target); })) return false;
            times.append(clock.nsecsElapsed()/1e6);
        }
        result.insert(phase + "SeekP95Ms", p95(times));
        QElapsedTimer release; release.start(); player.setScrubbing(false);
        if (!await([&] { return player.preparedAt(player.position()); })) return false;
        result.insert(phase + "SeekReleaseExactMs", release.nsecsElapsed()/1e6);
        return true;
    };
    auto drag = [&](const QString& phase) {
        DecodeScheduler::instance().evictOptionalCaches();
        QObject thumbnailOwner;
        for (int n = 1; n < 48; ++n)
            DecodeScheduler::instance().requestThumbnail(&thumbnailOwner, asset, asset->durationUs*n/48, [](auto) {});
        QVector<double> deliveredAge;
        qint64 latestRequestMs = 0;
        int delivered = 0;
        QElapsedTimer clock; clock.start();
        auto connection = QObject::connect(&sink, &QVideoSink::videoFrameChanged, &sink, [&](const auto& frame) {
            if (frame.isValid()) { ++delivered; deliveredAge.append(std::abs(frame.startTime()/1000.0 - latestRequestMs)); }
        });
        player.setScrubbing(true);
        for (int i = 0; i < 180; ++i) {
            // Traverse both directions and stop inside a GOP, not on the
            // already prepared poster at zero (which hides exact seek cost).
            const double fraction = i < 90 ? .1 + .8*i/89.0 : .9 - .7*(i-90)/89.0;
            latestRequestMs = qRound64(maximum * fraction);
            player.setPosition(latestRequestMs); spin(16);
        }
        const double seconds = clock.nsecsElapsed()/1e9;
        QObject::disconnect(connection);
        QElapsedTimer release; release.start(); player.setScrubbing(false);
        const bool exact = await([&] { return player.preparedAt(latestRequestMs); });
        DecodeScheduler::instance().cancel(&thumbnailOwner, 0);
        result.insert(phase + "DragDeliveredFrames", delivered);
        result.insert(phase + "DragDeliveredFps", delivered/seconds);
        result.insert(phase + "DragTimelineErrorP95Ms", p95(deliveredAge));
        result.insert(phase + "ReleaseExactMs", release.nsecsElapsed()/1e6);
        return exact;
    };
    if (!seeks("cold") || !drag("cold")) return 4;
    QElapsedTimer generating; generating.start(); proxies.setEnabled(true);
    if (!await([&] { return proxies.pendingAssets() == 0; }, 180000)) return 5;
    result.insert("backgroundProxyMs", double(generating.elapsed()));
    int cachedFrames = 0;
    for (int i = 0; i < asset->frameIndex.size(); ++i)
        if (MediaPreviewStore::hasScrubFrame(*asset, i)) ++cachedFrames;
    result.insert("proxyFramesAvailable", cachedFrames);
    // Drop decoded frames so the second run measures independent proxy access.
    DecodeScheduler::instance().evictOptionalCaches();
    if (!seeks("warm") || !drag("warm")) return 6;
    player.clearAsset(); asset.reset();
    DecodeScheduler::instance().evictOptionalCaches(); spin(1800);
    result.insert("trackedFrameBytesAfterRelease", double(mediaFrameCpuBytes()));
    printf("%s\n", QJsonDocument(result).toJson(QJsonDocument::Indented).constData());
    QDir(cacheDirectory).removeRecursively();
    return 0;
}
