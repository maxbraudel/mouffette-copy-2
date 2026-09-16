#include "backend/media/MediaBackendBootstrap.h"

#include <QAudioOutput>
#include <QCoreApplication>
#include <QMediaDevices>
#include <QMediaFormat>
#include <QMediaPlayer>
#include <QPointer>
#include <QThread>
#include <QThreadPool>
#include <QVideoSink>
#include <QtConcurrent/QtConcurrentRun>

namespace {
class BackendTasks;
QPointer<BackendTasks> tasks;
void waitForBackendTasks();

class BackendTasks final : public QObject {
public:
    explicit BackendTasks(QCoreApplication* parent) : QObject(parent)
    {
        pool.setMaxThreadCount(1);
        // No persistent polling or idle worker is needed after preparation.
        pool.setExpiryTimeout(0);
        qAddPostRoutine(waitForBackendTasks);
        connect(parent, &QCoreApplication::aboutToQuit, this,
                [this] { pool.waitForDone(); });
    }
    ~BackendTasks() override
    {
        pool.waitForDone();
        qRemovePostRoutine(waitForBackendTasks);
    }

    QThreadPool pool;
    bool initializationStarted = false;
    QFuture<MediaBackendBootstrap::Result> initialization;
};

void waitForBackendTasks()
{
    // QCoreApplication clears qApp before deleting QObject children. Join while
    // Qt's device discovery can still access it, also without exec()/aboutToQuit.
    if (tasks) tasks->pool.waitForDone();
}

BackendTasks* backendTasks()
{
    Q_ASSERT(QCoreApplication::instance() && QThread::isMainThread());
    if (!tasks) tasks = new BackendTasks(QCoreApplication::instance());
    return tasks;
}

struct Discovery {
    QAudioDevice audioDevice;
    bool supportsVideo = false;
};
}

QFuture<MediaBackendBootstrap::Result> MediaBackendBootstrap::initialize()
{
    auto* state = backendTasks();
    if (state->initializationStarted
        && (!state->initialization.isFinished()
            || (!state->initialization.isCanceled() && state->initialization.result().ready)))
        return state->initialization;
    state->initializationStarted = true;
    state->initialization = QtConcurrent::run(&state->pool, [] {
        Discovery discovery;
        discovery.audioDevice = QMediaDevices::defaultAudioOutput();
        QMediaFormat format(QMediaFormat::MPEG4);
        discovery.supportsVideo = !format.supportedVideoCodecs(QMediaFormat::Decode).isEmpty();
        return discovery;
    }).then(state, [](const Discovery& discovery) {
        // Platform QObjects are constructed and destroyed on the GUI thread,
        // after plugin loading, hardware probing and audio enumeration finish.
        // No source, decoder queues, audio stream or rendered surface is opened.
        QVideoSink sink;
        QAudioOutput audio(discovery.audioDevice);
        QMediaPlayer player;
        player.setVideoSink(&sink);
        player.setAudioOutput(&audio);
        if (!discovery.supportsVideo || !player.isAvailable())
            return Result{false, QStringLiteral("The audio/video playback engine is unavailable.")};
        return Result{true, {}};
    });
    return state->initialization;
}

QFuture<QAudioDevice> MediaBackendBootstrap::defaultAudioOutput()
{
    // Query again for each occurrence: a device may have changed since startup.
    // This also supports isolated documents/tests without an application shell.
    return QtConcurrent::run(&backendTasks()->pool,
                             [] { return QMediaDevices::defaultAudioOutput(); });
}
