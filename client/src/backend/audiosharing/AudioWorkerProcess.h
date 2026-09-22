#pragma once

#include <QProcess>

#ifdef Q_OS_MACOS
#include <QTimer>
#include <memory>

// Bundled audio workers need a LaunchServices application identity, not only
// a different executable: a QProcess child inherits its parent's responsible
// application and ScreenCaptureKit also excludes that application's scenes.
// Standalone test peers deliberately retain ordinary QProcess behavior.
class AudioWorkerProcess final : public QObject {
    Q_OBJECT
public:
    using ProcessState = QProcess::ProcessState;
    explicit AudioWorkerProcess(QObject* parent = nullptr);
    ~AudioWorkerProcess() override;
    void setProcessChannelMode(QProcess::ProcessChannelMode mode) { subprocess.setProcessChannelMode(mode); }
    void start(const QString& executable, const QStringList& arguments);
    ProcessState state() const;
    qint64 processId() const;
    QString errorString() const;
    void kill();
    bool waitForFinished(int msecs = 30000);

signals:
    void started();
    void finished(int exitCode, QProcess::ExitStatus status);
    void errorOccurred(QProcess::ProcessError error);
    void stateChanged(QProcess::ProcessState state);

private:
    QProcess subprocess;
    struct Launch;
    std::shared_ptr<Launch> launch;
    QTimer poll;
    void checkTermination();
};
#else
using AudioWorkerProcess = QProcess;
#endif
