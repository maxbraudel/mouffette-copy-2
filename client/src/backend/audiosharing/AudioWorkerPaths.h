#pragma once

#include <QDir>
#include <QFileInfo>
#include <QString>

// ScreenCaptureKit excludes audio by application identity. The macOS helper
// must belong to its own bundle, so the main application's received scenes
// remain capturable while previews and monitoring stay excluded.
inline QString audioWorkerExecutablePath(const QString& applicationExecutable) {
#ifdef Q_OS_MACOS
    return QDir::cleanPath(QFileInfo(applicationExecutable).absolutePath()
        + QStringLiteral("/../Helpers/MouffetteAudioWorker.app/Contents/MacOS/MouffetteAudioWorker"));
#else
    return applicationExecutable;
#endif
}
