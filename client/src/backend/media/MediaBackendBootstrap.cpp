#include "backend/media/MediaBackendBootstrap.h"

#include <QCoreApplication>
#include <QMediaDevices>
extern "C" {
#include <libavcodec/avcodec.h>
}
#include <QPointer>
#include <QThread>
#include <QThreadPool>
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
        discovery.supportsVideo = avcodec_find_decoder(AV_CODEC_ID_H264) && avcodec_find_decoder(AV_CODEC_ID_AAC);
        return discovery;
    }).then(state, [](const Discovery& discovery) {
        if (!discovery.supportsVideo)
            return Result{false, QStringLiteral("The integrated FFmpeg playback engine is unavailable.")};
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
