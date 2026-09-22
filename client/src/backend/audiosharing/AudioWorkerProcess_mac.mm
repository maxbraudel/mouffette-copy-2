#include "backend/audiosharing/AudioWorkerProcess.h"
#include <QCoreApplication>
#include <QDir>
#include <QEventLoop>
#include <QFileInfo>
#include <QPointer>
#include <QProcessEnvironment>
#include <atomic>
#import <AppKit/AppKit.h>

namespace {
constexpr auto Starting = QProcess::Starting;
constexpr auto Running = QProcess::Running;
constexpr auto NotRunning = QProcess::NotRunning;
constexpr auto FailedToStart = QProcess::FailedToStart;
constexpr auto CrashExit = QProcess::CrashExit;
constexpr auto NormalExit = QProcess::NormalExit;
}

struct AudioWorkerProcess::Launch {
    std::atomic_bool cancelled{false};
    NSRunningApplication* application = nil;
    ProcessState state = Starting;
    QString error;
    bool killed = false;
};

AudioWorkerProcess::AudioWorkerProcess(QObject* parent) : QObject(parent) {
    connect(&subprocess, &QProcess::started, this, &AudioWorkerProcess::started);
    connect(&subprocess, qOverload<int, QProcess::ExitStatus>(&QProcess::finished), this, &AudioWorkerProcess::finished);
    connect(&subprocess, &QProcess::errorOccurred, this, &AudioWorkerProcess::errorOccurred);
    connect(&subprocess, &QProcess::stateChanged, this, &AudioWorkerProcess::stateChanged);
    poll.setInterval(50);
    connect(&poll, &QTimer::timeout, this, &AudioWorkerProcess::checkTermination);
}

AudioWorkerProcess::~AudioWorkerProcess() {
    if (!launch) return; // QProcess owns the unbundled test-peer lifecycle.
    launch->cancelled.store(true);
    if (launch->application && !launch->application.terminated)
        [launch->application forceTerminate];
    // An asynchronous launch may complete after this object is destroyed.
    // Its retained cancellation state terminates that app in the completion
    // handler, without dereferencing this object or requiring a Qt event loop.
}

void AudioWorkerProcess::start(const QString& executable, const QStringList& arguments) {
    if (state() != NotRunning) return;
    QDir directory = QFileInfo(executable).dir();
    const bool macosDirectory = directory.dirName() == QLatin1String("MacOS");
    directory.cdUp();
    const bool contentsDirectory = directory.dirName() == QLatin1String("Contents");
    directory.cdUp();
    if (!macosDirectory || !contentsDirectory || !directory.dirName().endsWith(QLatin1String(".app"))) {
        launch.reset();
        subprocess.start(executable, arguments);
        return;
    }
    auto current = std::make_shared<Launch>();
    launch = current;
    emit stateChanged(Starting);
    const QPointer<AudioWorkerProcess> owner(this);
    auto* configuration = [NSWorkspaceOpenConfiguration configuration];
    configuration.createsNewApplicationInstance = YES;
    configuration.allowsRunningApplicationSubstitution = NO;
    configuration.activates = NO;
    configuration.hides = YES;
    configuration.addsToRecentItems = NO;
    configuration.promptsUserIfNeeded = NO;
    auto* nativeArguments = [NSMutableArray arrayWithCapacity:arguments.size()];
    for (const auto& argument : arguments) [nativeArguments addObject:argument.toNSString()];
    configuration.arguments = nativeArguments;
    auto* environment = [NSMutableDictionary dictionary];
    const auto inherited = QProcessEnvironment::systemEnvironment();
    for (const auto& key : inherited.keys()) environment[key.toNSString()] = inherited.value(key).toNSString();
    configuration.environment = environment;
    const auto bundle = [NSURL fileURLWithPath:directory.absolutePath().toNSString() isDirectory:YES];
    [[NSWorkspace sharedWorkspace] openApplicationAtURL:bundle configuration:configuration
        completionHandler:^(NSRunningApplication* application, NSError* error) {
            // NSWorkspace may call back on a background queue. Cancelled
            // launches must be reaped even after the owner or GUI loop is gone.
            if (current->cancelled.load()) { if (application) [application forceTerminate]; return; }
            const bool queued = QMetaObject::invokeMethod(QCoreApplication::instance(), [current, owner, application, error] {
                if (current->cancelled.load() || !owner || owner->launch != current) {
                    if (application) [application forceTerminate];
                    return;
                }
                if (error || !application || application.processIdentifier <= 0) {
                    current->state = NotRunning;
                    current->error = error ? QString::fromNSString(error.localizedDescription)
                        : QStringLiteral("LaunchServices did not return an audio helper process");
                    emit owner->stateChanged(NotRunning);
                    emit owner->errorOccurred(FailedToStart);
                    return;
                }
                current->application = application;
                current->state = Running;
                owner->poll.start();
                emit owner->stateChanged(Running);
                emit owner->started();
                owner->checkTermination();
            }, Qt::QueuedConnection);
            if (!queued && application) [application forceTerminate];
        }];
}

QProcess::ProcessState AudioWorkerProcess::state() const {
    return launch ? launch->state : subprocess.state();
}
qint64 AudioWorkerProcess::processId() const {
    return launch ? (launch->application && launch->state != NotRunning
        ? launch->application.processIdentifier : 0) : subprocess.processId();
}
QString AudioWorkerProcess::errorString() const {
    return launch ? launch->error : subprocess.errorString();
}

void AudioWorkerProcess::checkTermination() {
    if (!launch || launch->state == NotRunning || !launch->application.terminated) return;
    const bool killed = launch->killed;
    launch->state = NotRunning;
    poll.stop();
    emit stateChanged(NotRunning);
    // NSRunningApplication exposes termination, not an exit code. -1 means
    // unknown; never report a fabricated successful process exit.
    emit finished(-1, killed ? CrashExit : NormalExit);
}

void AudioWorkerProcess::kill() {
    if (!launch) { subprocess.kill(); return; }
    if (launch->state == NotRunning) return;
    launch->cancelled.store(true);
    launch->killed = true;
    if (launch->application) {
        [launch->application forceTerminate];
        checkTermination();
    } else {
        // The completion owns the eventual process and will kill it. Allow
        // the caller to finish shutdown without waiting on LaunchServices.
        launch->state = NotRunning;
        emit stateChanged(NotRunning);
        emit finished(-1, CrashExit);
    }
}

bool AudioWorkerProcess::waitForFinished(int msecs) {
    if (!launch) return subprocess.waitForFinished(msecs);
    checkTermination();
    if (state() == NotRunning) return true;
    if (msecs == 0) return false;
    QEventLoop loop;
    QTimer timeout;
    timeout.setSingleShot(true);
    connect(this, &AudioWorkerProcess::finished, &loop, &QEventLoop::quit);
    connect(this, &AudioWorkerProcess::errorOccurred, &loop, &QEventLoop::quit);
    connect(&timeout, &QTimer::timeout, &loop, &QEventLoop::quit);
    if (msecs >= 0) timeout.start(msecs);
    loop.exec(QEventLoop::ExcludeUserInputEvents);
    checkTermination();
    return state() == NotRunning;
}
