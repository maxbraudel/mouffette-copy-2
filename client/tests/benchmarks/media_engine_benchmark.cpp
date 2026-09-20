// Manual, reproducible benchmark: media_engine_benchmark [--legacy] [--copies N] files...
// JSON distinguishes resident storage from measured heap/process footprint.
#include "backend/media/MediaDecoder.h"
#include "backend/media/ResidentVideoPlayer.h"
#include "backend/media/DecodeScheduler.h"
#include "backend/media/PlaybackAudio.h"
#include <QGuiApplication>
#include <QAudioOutput>
#include <QBuffer>
#include <QFile>
#include <QVideoSink>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QElapsedTimer>
#include <QThread>
#include <QTimer>
#include <QEventLoop>
#include <algorithm>
#include <vector>
#include <ctime>
#ifdef Q_OS_MACOS
#include <mach/mach.h>
#include <malloc/malloc.h>
#endif
#ifdef Q_OS_WIN
#include <Windows.h>
#include <Psapi.h>
#endif
static quint64 footprint() {
#ifdef Q_OS_MACOS
    task_vm_info_data_t data{}; mach_msg_type_number_t count = TASK_VM_INFO_COUNT;
    task_info(mach_task_self(), TASK_VM_INFO, reinterpret_cast<task_info_t>(&data), &count);
    return data.phys_footprint;
#elif defined(Q_OS_WIN)
    PROCESS_MEMORY_COUNTERS_EX data{};
    GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&data), sizeof(data));
    return data.PrivateUsage;
#else
    return 0;
#endif
}
static quint64 heap() {
#ifdef Q_OS_MACOS
    malloc_statistics_t data{}; malloc_zone_statistics(nullptr, &data); return data.size_in_use;
#else
    return 0;
#endif
}
static bool await(const std::function<bool()>& condition, int timeoutMs = 10000) {
    QElapsedTimer clock; clock.start();
    while (!condition() && clock.elapsed() < timeoutMs) { QCoreApplication::processEvents(); QThread::msleep(1); }
    return condition();
}
static void spin(int milliseconds) { QEventLoop loop; QTimer::singleShot(milliseconds, &loop, &QEventLoop::quit); loop.exec(); }
struct Cursor {
    QBuffer source;
    QAudioOutput audio;
    QVideoSink sink;
    std::unique_ptr<QMediaPlayer> legacy;
    std::unique_ptr<ResidentVideoPlayer> shared;
    quint64 presentations = 0;
    Cursor(bool old) {
        audio.setMuted(true);
        QObject::connect(&sink, &QVideoSink::videoFrameChanged, &sink, [this](const auto& frame) { if (frame.isValid()) ++presentations; });
        if (old) { legacy = std::make_unique<QMediaPlayer>(); legacy->setVideoSink(&sink); legacy->setAudioOutput(&audio); }
        else { shared = std::make_unique<ResidentVideoPlayer>(); shared->setVideoSink(&sink); shared->setAudioOutput(&audio); }
    }
    void set(std::shared_ptr<const ResidentMediaAsset> asset, const QByteArray& original, qint64 position) {
        if (shared) { shared->setAsset(std::move(asset)); shared->prepare(position); }
        else { source.setData(original); source.open(QIODevice::ReadOnly); legacy->setSourceDevice(&source, QUrl("resident:///video.mp4")); }
    }
    void seek(qint64 position) { if (shared) shared->prepare(position); else { legacy->setPosition(position); legacy->pause(); } }
    void play() { if (shared) shared->play(); else legacy->play(); }
    void pause() { if (shared) shared->pause(); else legacy->pause(); }
    void scrub(bool enabled) { if (shared) shared->setScrubbing(enabled); }
    bool ready(qint64 position) {
        if (shared) return shared->preparedAt(position);
        auto f = sink.videoFrame(); return f.isValid() && f.startTime()/1000 <= position && f.endTime() > position*1000;
    }
};
int main(int argc, char** argv) {
    QGuiApplication app(argc, argv);
    auto args = app.arguments(); args.removeFirst();
    bool legacy = args.removeAll("--legacy") > 0;
    QString dumpPath;
    int dumpFlag = args.indexOf("--dump-h264");
    if (dumpFlag >= 0 && dumpFlag+1 < args.size()) { dumpPath = args[dumpFlag+1]; args.removeAt(dumpFlag+1); args.removeAt(dumpFlag); }
    int copies = 1;
    int flag = args.indexOf("--copies");
    if (flag >= 0 && flag+1 < args.size()) { copies = args[flag+1].toInt(); args.removeAt(flag+1); args.removeAt(flag); }
    if (args.isEmpty() || copies < 1 || copies > 32) return 1;
    const quint64 beforeHeap = heap(), beforeFootprint = footprint();
    QElapsedTimer loading; loading.start();
    quint64 stored = 0, peakConversion = 0;
    std::vector<std::shared_ptr<const ResidentMediaAsset>> assets;
    std::vector<QByteArray> originals;
    for (const auto& path : args) {
        if (legacy) { QFile file(path); if (!file.open(QIODevice::ReadOnly)) return 2; originals.push_back(file.readAll()); stored += originals.back().capacity(); assets.push_back({}); }
        else {
            QString error;
            auto asset = MediaDecoder::decode(path, {}, &error);
            if (!asset) { qWarning() << error; return 3; }
            if (!dumpPath.isEmpty()) {
                if (!asset->allIntra) return 11;
                QFile dump(dumpPath); if (!dump.open(QIODevice::WriteOnly)) return 12;
                for (const auto& packet : asset->videoPackets.packets)
                    if (dump.write(asset->videoPackets.bytes.constData()+packet.offset, packet.size) != packet.size) return 13;
                dumpPath.clear();
            }
            stored += asset->memoryBreakdown().videoBytes;
            peakConversion = std::max(peakConversion, asset->conversionPeakBytes);
            assets.push_back(std::move(asset)); originals.emplace_back();
        }
    }
    const auto loadMs = loading.elapsed();
    const quint64 loadedHeap = heap();
    std::vector<std::unique_ptr<Cursor>> cursors;
    for (size_t i = 0; i < assets.size(); ++i) for (int copy = 0; copy < copies; ++copy) {
        auto cursor = std::make_unique<Cursor>(legacy); cursor->set(assets[i], originals[i], 500);
        cursors.push_back(std::move(cursor));
    }
    if (legacy) {
        if (!await([&] { return std::all_of(cursors.begin(), cursors.end(), [](const auto& c) { return c->legacy->mediaStatus() == QMediaPlayer::LoadedMedia; }); })) return 4;
        for (auto& cursor : cursors) cursor->seek(500);
    }
    if (!await([&] { return std::all_of(cursors.begin(), cursors.end(), [](const auto& c) { return c->ready(500); }); })) return 5;
    spin(100);
    const quint64 preparedHeap = heap(), preparedFootprint = footprint();
    const int audioDevices = PlaybackAudio::deviceCount();
    const quint64 preparedAudio = PlaybackAudio::bufferBytes(), preparedVideo = mediaFrameCpuBytes();
    std::vector<double> starts, audioVisualStarts, seeks;
    for (int trial = 0; trial < 25; ++trial) {
        for (auto& cursor : cursors) cursor->seek(500);
        if (!await([&] { return std::all_of(cursors.begin(), cursors.end(), [](const auto& c) { return c->ready(500); }); })) return 6;
        std::vector<quint64> counts;
        for (auto& cursor : cursors) counts.push_back(cursor->presentations);
        QElapsedTimer clock; clock.start();
        for (auto& cursor : cursors) cursor->play();
        if (!await([&] { for (size_t i=0; i<cursors.size(); ++i) if (cursors[i]->presentations == counts[i]) return false; return true; })) return 7;
        starts.push_back(clock.nsecsElapsed()/1e6);
        if (!legacy) {
            if (!await([&] { return std::all_of(cursors.begin(), cursors.end(), [](const auto& c) { return c->shared->audioPresentedSincePlay(); }); })) return 10;
            audioVisualStarts.push_back(clock.nsecsElapsed()/1e6);
        }
        for (auto& cursor : cursors) cursor->pause();
    }
    for (auto& cursor : cursors) { cursor->seek(500); cursor->presentations = 0; }
    if (!await([&] { return std::all_of(cursors.begin(), cursors.end(), [](const auto& c) { return c->ready(500); }); })) return 8;
    const auto playCpu = std::clock(); QElapsedTimer playClock; playClock.start();
    for (auto& cursor : cursors) cursor->play();
    spin(3000);
    const double playSeconds = playClock.nsecsElapsed()/1e9;
    QJsonArray fps;
    for (auto& cursor : cursors) { cursor->pause(); fps.append(cursor->presentations/playSeconds); }
    const double cpuPercent = 100.0 * (std::clock()-playCpu)/CLOCKS_PER_SEC/playSeconds;
    for (auto& cursor : cursors) cursor->scrub(true);
    for (int i=0; i<30; ++i) {
        qint64 position = 100 + (i*173) % 2500;
        QElapsedTimer clock; clock.start();
        for (auto& cursor : cursors) cursor->seek(position);
        if (!await([&] { return std::all_of(cursors.begin(), cursors.end(), [=](const auto& c) { return c->ready(position); }); })) return 9;
        seeks.push_back(clock.nsecsElapsed()/1e6);
    }
    for (auto& cursor : cursors) cursor->scrub(false);
    const quint64 runningHeap = heap(), runningFootprint = footprint();
    cursors.clear(); assets.clear(); originals.clear();
    DecodeScheduler::instance().evictOptionalCaches(); spin(1800);
    std::sort(starts.begin(), starts.end()); std::sort(seeks.begin(), seeks.end());
    QJsonObject result{{"backend", legacy ? "Qt per-occurrence baseline" : "shared indexed FFmpeg"},
        {"qtVersion", QString::fromLatin1(qVersion())}, {"audioMixerDevices", audioDevices},
        {"preparedTrackedAudioBytes", double(preparedAudio)}, {"preparedTrackedVideoBytes", double(preparedVideo)},
        {"files", int(args.size())}, {"occurrences", int(args.size())*copies}, {"storedBytes", double(stored)},
        {"loadMs", double(loadMs)}, {"conversionPeakBudgetBytes", double(peakConversion)},
        {"initialHeapBytes", double(beforeHeap)}, {"loadedHeapBytes", double(loadedHeap)},
        {"preparedHeapBytes", double(preparedHeap)}, {"preparedDecodeIncrementBytes", double(preparedHeap-loadedHeap)},
        {"initialFootprintBytes", double(beforeFootprint)}, {"preparedFootprintBytes", double(preparedFootprint)},
        {"runningHeapBytes", double(runningHeap)}, {"runningFootprintBytes", double(runningFootprint)},
        {"releasedHeapBytes", double(heap())}, {"releasedFootprintBytes", double(footprint())},
        {"deliveredFpsPerCursor", fps}, {"playCpuPercent", cpuPercent},
        {"playToNextFrameP95Ms", starts[size_t(starts.size()*.95)]}, {"playToNextFrameMaxMs", starts.back()},
        {"scrubP50Ms", seeks[seeks.size()/2]}, {"scrubP95Ms", seeks[size_t(seeks.size()*.95)]},
        {"trackedVideoBytesAfterRelease", double(mediaFrameCpuBytes())}};
    if (!audioVisualStarts.empty() && audioDevices > 0) {
        std::sort(audioVisualStarts.begin(), audioVisualStarts.end());
        result.insert("playToVideoSinkAndAudioCallbackP95Ms", audioVisualStarts[size_t(audioVisualStarts.size()*.95)]);
    }
    printf("%s\n", QJsonDocument(result).toJson(QJsonDocument::Compact).constData());
}
