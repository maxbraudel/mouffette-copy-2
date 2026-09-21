// Manual retained-original seek benchmark. Measures delivered frames and the
// preparation barrier used when the timeline resumes after a pointer release.
#include "backend/media/MediaDecoder.h"
#include "backend/media/IndexedMediaDecoder.h"
#include "backend/media/ResidentVideoPlayer.h"
#include "backend/media/DecodeScheduler.h"
#include "backend/media/EditingProxyCache.h"
#include "backend/media/MediaPreviewStore.h"
#include <QGuiApplication>
#include <QAudioOutput>
#include <QAudioDevice>
#include <QMediaDevices>
#include <QVideoSink>
#include <QElapsedTimer>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QStandardPaths>
#include <QDir>
#include <QFileInfo>
#include <QThread>
#include <QUuid>
#include <cstdio>

namespace {
bool await(const std::function<bool()>& done, int timeout = 10000)
{
    QElapsedTimer clock; clock.start();
    while (!done() && clock.elapsed() < timeout) {
        QCoreApplication::processEvents();
        QThread::msleep(1);
    }
    return done();
}
double milliseconds(const QElapsedTimer& clock) { return clock.nsecsElapsed() / 1e6; }
void spin(int ms) { QElapsedTimer clock; clock.start(); await([&] { return clock.elapsed() >= ms; }, ms + 1000); }
}

int main(int argc, char** argv)
{
    QGuiApplication app(argc, argv);
    if (app.arguments().size() != 2) return 1;
    app.setApplicationName(QStringLiteral("MouffetteSeekBenchmark-") + QUuid::createUuid().toString(QUuid::Id128));
    const auto cacheDirectory = QStandardPaths::writableLocation(QStandardPaths::CacheLocation);
    auto& proxies = EditingProxyCache::instance();
    proxies.setEnabled(false);
    MediaDecoder::DecodeCallbacks callbacks;
    callbacks.retainOriginalVideo = true;
    QElapsedTimer clock; clock.start();
    QString error;
    auto asset = MediaDecoder::decode(app.arguments()[1], callbacks, &error);
    if (!asset) { qWarning() << error; return 2; }
    QJsonObject result{{"file", QFileInfo(app.arguments()[1]).fileName()},
        {"qtVersion", QString::fromLatin1(qVersion())}, {"representation", "retained original"},
        {"validationMs", milliseconds(clock)}, {"durationMs", asset->durationUs / 1000.0},
        {"frames", asset->frameIndex.size()}, {"audioDeviceAvailable", !QMediaDevices::defaultAudioOutput().isNull()}};
    QList<qint64> targets;
    for (const double fraction : {.1, .2, .5, .8, .5, .2, .1, .8})
        targets.append(qint64(asset->durationUs / 1000.0 * fraction));

    QJsonArray raw;
    for (const qint64 target : targets) {
        const int index = IndexedMediaDecoder::frameAt(*asset, target * 1000);
        QJsonObject sample{{"targetMs", double(target)}, {"frameIndex", index}};
        {
            IndexedMediaDecoder decoder;
            clock.restart();
            if (!decoder.videoFrame(*asset, index, error).isValid()) { qWarning() << error; return 3; }
            sample.insert("freshDecoderTargetMs", milliseconds(clock));
            clock.restart();
            if (!decoder.videoFrame(*asset, index + 1, error).isValid()) return 3;
            sample.insert("nextFrameMs", milliseconds(clock));
        }
        {
            IndexedMediaDecoder decoder;
            clock.restart();
            if (!decoder.videoFrame(*asset, 0, error).isValid()) return 3;
            sample.insert("openAndFirstFrameMs", milliseconds(clock));
            clock.restart();
            if (!decoder.videoFrame(*asset, index, error).isValid()) return 3;
            sample.insert("openDecoderTargetMs", milliseconds(clock));
        }
        raw.append(sample);
    }
    result.insert("decoder", raw);
    qInfo().noquote() << "Raw decoder measurements:" << QJsonDocument(raw).toJson(QJsonDocument::Compact);

    QAudioOutput output; output.setMuted(true);
    QVideoSink sink;
    ResidentVideoPlayer player;
    player.setAudioOutput(&output); player.setVideoSink(&sink); player.setAsset(asset);
    if (!await([&] { return player.preparedAt(0); })) return 4;
    auto measure = [&](const QString& phase) {
        QJsonArray samples;
        player.play();
        if (!await([&] { return player.isPlaying() && player.position() > 50; })) return false;
        for (const qint64 target : targets) {
            DecodeScheduler::instance().evictOptionalCaches();
            QElapsedTimer seekClock, releaseClock;
            double targetImageMs = -1, nativeAfterReleaseMs = -1, advancedAfterReleaseMs = -1;
            const int index = IndexedMediaDecoder::frameAt(*asset, target * 1000);
            const qint64 targetEnd = asset->frameIndex[index].timestampUs + asset->frameIndex[index].durationUs;
            QObject observer;
            QObject::connect(&sink, &QVideoSink::videoFrameChanged, &observer, [&](const QVideoFrame& frame) {
                if (!frame.isValid()) return;
                if (frame.startTime() <= target * 1000 && frame.endTime() > target * 1000) {
                    if (targetImageMs < 0) targetImageMs = milliseconds(seekClock);
                    if (releaseClock.isValid() && nativeAfterReleaseMs < 0
                        && frame.size() == asset->firstFrame.frame.size())
                        nativeAfterReleaseMs = milliseconds(releaseClock);
                }
                if (releaseClock.isValid() && player.isPlaying() && frame.startTime() >= targetEnd
                    && advancedAfterReleaseMs < 0) advancedAfterReleaseMs = milliseconds(releaseClock);
            });
            seekClock.start();
            player.setScrubbing(true); player.pause(); player.setPosition(target);
            spin(20); // Separate press and release, as a short timeline click does.
            releaseClock.start();
            player.setScrubbing(false); player.prepare(target);
            if (!await([&] { return player.preparedAt(target); })) { qWarning() << player.errorString(); return false; }
            const double preparedMs = milliseconds(releaseClock);
            if (nativeAfterReleaseMs < 0 && player.preparedFrame(target).isValid()) nativeAfterReleaseMs = 0;
            player.play();
            if (!await([&] { return player.isPlaying(); })) return false;
            const double resumedMs = milliseconds(releaseClock);
            if (!await([&] { return advancedAfterReleaseMs >= 0; })) return false;
            samples.append(QJsonObject{{"targetMs", double(target)}, {"targetImageFromPressMs", targetImageMs},
                {"nativeTargetFromReleaseMs", nativeAfterReleaseMs}, {"preparedFromReleaseMs", preparedMs},
                {"playingFromReleaseMs", resumedMs}, {"nextPresentedFrameFromReleaseMs", advancedAfterReleaseMs}});
            spin(60);
        }
        player.pause();
        result.insert(phase, samples);
        return true;
    };
    if (!measure(QStringLiteral("coldEditingCache"))) return 5;
    clock.restart(); proxies.setEnabled(true);
    if (!await([&] { return proxies.pendingAssets() == 0; }, 90000)) return 6;
    result.insert("editingCacheGenerationMs", milliseconds(clock));
    int available = 0;
    for (int index = 0; index < asset->frameIndex.size(); ++index)
        if (MediaPreviewStore::hasScrubFrame(*asset, index)) ++available;
    result.insert("editingFramesAvailable", available);
    if (!measure(QStringLiteral("warmEditingCache"))) return 7;
    player.clearAsset(); asset.reset();
    DecodeScheduler::instance().evictOptionalCaches();
    std::printf("%s\n", QJsonDocument(result).toJson(QJsonDocument::Indented).constData());
    QDir(cacheDirectory).removeRecursively();
    return 0;
}
