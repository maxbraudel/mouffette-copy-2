#pragma once

#include <QAudioDevice>
#include <QFuture>
#include <QString>

// GUI-thread entry points. Preparation is shared for the application's lifetime;
// callers observe completion without blocking the bootstrap window's event loop.
namespace MediaBackendBootstrap {
struct Result {
    bool ready = false;
    QString error;
};
QFuture<Result> initialize();
QFuture<QAudioDevice> defaultAudioOutput();
}
