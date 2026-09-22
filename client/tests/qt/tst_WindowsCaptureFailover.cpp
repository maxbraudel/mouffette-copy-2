// Exercise the real DXGI error wiring with a fake WGC backend. No desktop
// capture is started and no OS permission is requested by these tests.
#include "backend/screensharing/ScreenCaptureSource.cpp"
#include <QGuiApplication>
#include <QSignalSpy>
#include <QtTest>

namespace LocalScreenTopology {
QList<Screen> screens(bool, bool* success) {
    if (success) *success = true;
    return {};
}
}
namespace {
int fallbackStarts = 0;
WindowsScreenCapture::FrameCallback deliver;
WindowsScreenCapture::ErrorCallback fail;
}
struct WindowsScreenCapture::Private { bool active = false; };
WindowsScreenCapture::WindowsScreenCapture() : d(std::make_unique<Private>()) {}
WindowsScreenCapture::~WindowsScreenCapture() { stop(); }
bool WindowsScreenCapture::start(QScreen*, FrameCallback frame, ErrorCallback error) {
    ++fallbackStarts;
    deliver = std::move(frame); fail = std::move(error);
    d->active = true;
    return true;
}
void WindowsScreenCapture::stop() { d->active = false; }
bool WindowsScreenCapture::isActive() const { return d->active; }
void WindowsScreenCapture::setProfile(const ScreenStreamProfile&) {}

class WindowsCaptureFailoverTest final : public QObject {
    Q_OBJECT
    void prepare(ScreenCaptureSource& source) {
        source.d->screen = QGuiApplication::primaryScreen();
        source.d->mailbox = std::make_shared<CaptureMailbox>();
        updateLayerProfiles(*source.d->mailbox, source.d->profiles);
    }
    void dxgiError(ScreenCaptureSource& source) {
        emit source.d->capture.errorOccurred(QScreenCapture::CaptureFailed,
            QStringLiteral("Failed to duplicate IDXGIOutput1 COM error 0x8000ffff"));
    }
private slots:
    void init() { fallbackStarts = 0; deliver = {}; fail = {}; }
    void duplicateErrorsSwitchOnceAndPreservePublication() {
        ScreenCaptureSource source;
        prepare(source);
        QSignalSpy errors(&source, &ScreenCaptureSource::errorOccurred);
        const auto mailbox = source.d->mailbox;
        const auto previousEpoch = mailbox->layers.value("main").epoch;
        mailbox->latest = QVideoFrame(QVideoFrameFormat(QSize(64, 48), QVideoFrameFormat::Format_BGRA8888));
        dxgiError(source); dxgiError(source);
        QTRY_COMPARE(fallbackStarts, 1);
        QCOMPARE(source.d->mailbox, mailbox);
        QVERIFY(source.isActive());
        QVERIFY(!mailbox->latest.isValid());
        QVERIFY(mailbox->layers.value("main").epoch > previousEpoch);
        QVERIFY(mailbox->layers.value("main").forceKeyFrame);
        QVERIFY(errors.isEmpty());
        auto frame = QVideoFrame(QVideoFrameFormat(QSize(64, 48), QVideoFrameFormat::Format_BGRA8888));
        const auto timestamp = MediaCaptureClock::nowUs() - 10000;
        frame.setStartTime(timestamp);
        deliver(frame);
        QCOMPARE(mailbox->latest.size(), QSize(64, 48));
        QCOMPARE(mailbox->capturedAtUs, timestamp);
        dxgiError(source);
        QCoreApplication::processEvents();
        QCOMPARE(fallbackStarts, 1);
        QVERIFY(source.isActive());
    }
    void stopFencesQueuedDxgiFailure() {
        ScreenCaptureSource source;
        prepare(source);
        dxgiError(source);
        source.stop();
        QCoreApplication::processEvents();
        QCOMPARE(fallbackStarts, 0);
        QVERIFY(!source.isActive());
    }
    void replacementFencesOldNativeCallbacks() {
        ScreenCaptureSource source;
        prepare(source);
        dxgiError(source);
        QTRY_COMPARE(fallbackStarts, 1);
        const auto oldFailure = fail;
        const auto oldDelivery = deliver;
        // Queue a failure before stopping, then deliver another after stop.
        oldFailure(ScreenCaptureError::CaptureFailed, QStringLiteral("old backend"));
        source.stop();
        prepare(source);
        const auto replacement = source.d->mailbox;
        QSignalSpy errors(&source, &ScreenCaptureSource::errorOccurred);
        oldFailure(ScreenCaptureError::CaptureFailed, QStringLiteral("late backend"));
        oldDelivery(QVideoFrame(QVideoFrameFormat(QSize(64, 48), QVideoFrameFormat::Format_BGRA8888)));
        QCoreApplication::processEvents();
        QCOMPARE(source.d->mailbox, replacement);
        QVERIFY(!replacement->latest.isValid());
        QVERIFY(errors.isEmpty());
    }
    void bothFailuresReportOriginalAndFallbackDiagnostics() {
        ScreenCaptureSource source;
        prepare(source);
        QSignalSpy errors(&source, &ScreenCaptureSource::errorOccurred);
        dxgiError(source);
        QTRY_COMPARE(fallbackStarts, 1);
        fail(ScreenCaptureError::CaptureFailed, QStringLiteral("WGC: access denied"));
        QTRY_COMPARE(errors.size(), 1);
        QVERIFY(!source.isActive());
        const auto message = errors.first().at(1).toString();
        QVERIFY(message.contains("0x8000ffff"));
        QVERIFY(message.contains("WGC: access denied"));
    }
};
QTEST_MAIN(WindowsCaptureFailoverTest)
#include "tst_WindowsCaptureFailover.moc"
