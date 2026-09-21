// Include the native implementation in this standalone test target to exercise
// its actual session cleanup without adding production test hooks or starting
// ScreenCaptureKit. The target must not also compile MacScreenCapture.mm.
#include "backend/screensharing/MacScreenCapture.mm"

#include <QElapsedTimer>
#include <QtTest>

using StopCompletion = void (^)(NSError*);

// stopSession only sends this one message to its stream. An NSObject with the
// same selector lets the test control when the asynchronous completion arrives.
@interface DeferredCaptureStream : NSObject {
@public
    int stopCalls;
    StopCompletion completion;
    StopCompletion updateCompletion;
    int updateCalls;
    int configuredWidth;
    int configuredFps;
}
- (void)stopCaptureWithCompletionHandler:(StopCompletion)handler;
- (void)completeStop;
- (void)updateConfiguration:(SCStreamConfiguration*)config completionHandler:(StopCompletion)handler;
- (void)completeUpdate;
@end

@implementation DeferredCaptureStream
- (void)stopCaptureWithCompletionHandler:(StopCompletion)handler {
    ++stopCalls;
    completion = [handler copy];
}
- (void)completeStop {
    StopCompletion pending = completion;
    completion = nil;
    if (pending) pending(nil);
}
- (void)updateConfiguration:(SCStreamConfiguration*)config completionHandler:(StopCompletion)handler {
    ++updateCalls;
    configuredWidth = int(config.width);
    configuredFps = int(config.minimumFrameInterval.timescale / config.minimumFrameInterval.value);
    updateCompletion = [handler copy];
}
- (void)completeUpdate {
    StopCompletion pending = updateCompletion;
    updateCompletion = nil;
    if (pending) pending(nil);
}
@end

namespace {
bool drainMainQueueUntil(const std::function<bool()>& finished) {
    QElapsedTimer timeout;
    timeout.start();
    while (!finished() && timeout.elapsed() < 2000) {
        // The offscreen Qt event dispatcher need not service libdispatch's
        // main queue; its native CFRunLoop does, without creating an NSApp.
        CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.001, false);
    }
    return finished();
}

std::shared_ptr<NativeState> session(DeferredCaptureStream* stream) {
    auto state = std::make_shared<NativeState>();
    state->stream = (SCStream*)stream;
    state->delegate = (id<SCStreamOutput, SCStreamDelegate>)[[NSObject alloc] init];
    state->outputQueue = dispatch_queue_create("Mouffette.Test.ScreenCaptureLifecycle", DISPATCH_QUEUE_SERIAL);
    return state;
}
}

class MacScreenCaptureLifecycleTest final : public QObject {
    Q_OBJECT
private slots:
    void configurationChangesCoalesceAndKeepLatestProfile() {
        @autoreleasepool {
            DeferredCaptureStream* stream = [[DeferredCaptureStream alloc] init];
            auto state = session(stream);
            state->started = true;
            state->appliedSize = QSize(1920, 1080);
            state->appliedFps = 30;
            state->desiredSize = QSize(1280, 720);
            state->desiredFps = 15;
            updateConfiguration(state);
            QCOMPARE(stream->updateCalls, 1);
            QCOMPARE(stream->configuredWidth, 1280);
            QCOMPARE(stream->configuredFps, 15);
            state->desiredSize = QSize(640, 360);
            state->desiredFps = 10;
            updateConfiguration(state);
            QCOMPARE(stream->updateCalls, 1);
            [stream completeUpdate];
            QVERIFY(drainMainQueueUntil([stream] { return stream->updateCalls == 2; }));
            QCOMPARE(stream->configuredWidth, 640);
            QCOMPARE(stream->configuredFps, 10);
            [stream completeUpdate];
            QVERIFY(drainMainQueueUntil([&state] { return !state->updating; }));
            QCOMPARE(state->appliedSize, QSize(640, 360));
            QCOMPARE(state->appliedFps, 10);
            updateConfiguration(state);
            QCOMPARE(stream->updateCalls, 2);
        }
    }

    void stoppedSessionCannotRestartFromConfigurationCompletion() {
        @autoreleasepool {
            DeferredCaptureStream* stream = [[DeferredCaptureStream alloc] init];
            auto state = session(stream);
            state->started = true;
            state->desiredSize = QSize(1280, 720);
            state->desiredFps = 15;
            updateConfiguration(state);
            const std::weak_ptr<NativeState> observed = state;
            state->desiredSize = QSize(640, 360);
            state->closed.store(true);
            state.reset();
            QVERIFY(!observed.expired());
            [stream completeUpdate];
            QVERIFY(drainMainQueueUntil([&observed] { return observed.expired(); }));
            QCOMPARE(stream->updateCalls, 1);
        }
    }

    void deferredStopRetainsSessionThroughBothCallbacks() {
        @autoreleasepool {
            DeferredCaptureStream* stream = [[DeferredCaptureStream alloc] init];
            auto state = session(stream);
            state->started = true;
            state->closed.store(true);
            const std::weak_ptr<NativeState> observed = state;
            __weak DeferredCaptureStream* observedStream = stream;
            __weak NSObject* observedDelegate = (NSObject*)state->delegate;

            stopSession(state);
            stopSession(state);
            QCOMPARE(stream->stopCalls, 1);
            QVERIFY(stream->completion != nil);

            state.reset();
            const bool retainedByNativeCompletion = !observed.expired();
            // Fail before calling a dangling block on the old implementation:
            // an Objective-C block capturing a shared_ptr reference does not
            // retain its pointee when the caller relinquishes ownership.
            if (!retainedByNativeCompletion) stream->completion = nil;
            QVERIFY2(retainedByNativeCompletion,
                     "The deferred native stop callback must own its session");

            [stream completeStop];
            QVERIFY(stream->completion == nil);
            QVERIFY2(!observed.expired(),
                     "The queued main-thread cleanup must retain the session after the native callback returns");
            stream = nil;

            QVERIFY2(drainMainQueueUntil([&observed] { return observed.expired(); }),
                     "Native stop completion must eventually release its session");
            QVERIFY(observedStream == nil);
            QVERIFY(observedDelegate == nil);
        }
    }

    void stopDuringStartupWaitsForStartupCompletion() {
        @autoreleasepool {
            DeferredCaptureStream* stream = [[DeferredCaptureStream alloc] init];
            auto state = session(stream);
            state->starting = true;
            state->closed.store(true);

            stopSession(state);
            QCOMPARE(stream->stopCalls, 0);
            QVERIFY(state->stream != nil);
            QVERIFY(!state->stopping);

            // Mirror the successful start completion for a session revoked
            // during startup. It must issue exactly one deferred native stop.
            state->starting = false;
            state->started = true;
            stopSession(state);
            stopSession(state);
            QCOMPARE(stream->stopCalls, 1);
            QVERIFY(state->stopping);
            [stream completeStop];

            QVERIFY(drainMainQueueUntil([&state] { return state->stream == nil; }));
            QVERIFY(state->delegate == nil);
            QVERIFY(state->outputQueue == nil);
            stopSession(state);
            QCOMPARE(stream->stopCalls, 1);
        }
    }

    void sessionThatNeverStartedReleasesResourcesWithoutNativeStop() {
        @autoreleasepool {
            DeferredCaptureStream* stream = [[DeferredCaptureStream alloc] init];
            auto state = session(stream);
            __weak NSObject* observedDelegate = (NSObject*)state->delegate;

            stopSession(state);

            QCOMPARE(stream->stopCalls, 0);
            QVERIFY(stream->completion == nil);
            QVERIFY(state->stream == nil);
            QVERIFY(state->delegate == nil);
            QVERIFY(state->outputQueue == nil);
            QVERIFY(observedDelegate == nil);
            stopSession(state);
            QCOMPARE(stream->stopCalls, 0);
        }
    }
};

QTEST_GUILESS_MAIN(MacScreenCaptureLifecycleTest)
#include "tst_MacScreenCaptureLifecycle.moc"
