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
}
- (void)stopCaptureWithCompletionHandler:(StopCompletion)handler;
- (void)completeStop;
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
