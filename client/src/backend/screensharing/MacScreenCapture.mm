#include "backend/screensharing/MacScreenCapture.h"
#include "backend/screensharing/ScreenCaptureVideoBuffer.h"
#include "backend/screensharing/ScreenStreamCodec.h"
#include "backend/platform/WindowCaptureExclusion.h"

#include <QGuiApplication>
#include <QScreen>
#include <QSet>
#include <QWindow>
#include <QtGui/qscreen_platform.h>
#include <algorithm>
#include <atomic>
#include <utility>

#import <AppKit/AppKit.h>
#import <CoreMedia/CoreMedia.h>
#import <CoreVideo/CoreVideo.h>
#import <ScreenCaptureKit/ScreenCaptureKit.h>

// AVFoundation (transitively imported by ScreenCaptureKit) and FFmpeg both
// declare AVMediaType; isolate FFmpeg's otherwise unused enum in this TU.
#define AVMediaType FFmpegAVMediaType
extern "C" {
#include <libavutil/buffer.h>
#include <libavutil/frame.h>
#include <libavutil/pixfmt.h>
}
#undef AVMediaType

namespace {
class SurfaceBuffer final : public ScreenCaptureVideoBuffer {
public:
    explicit SurfaceBuffer(CVPixelBufferRef surface) : m_surface(CVPixelBufferRetain(surface)) {}
    ~SurfaceBuffer() override { unmap(); CVPixelBufferRelease(m_surface); }
    QVideoFrameFormat format() const override {
        QVideoFrameFormat result(QSize(int(CVPixelBufferGetWidth(m_surface)), int(CVPixelBufferGetHeight(m_surface))),
            QVideoFrameFormat::Format_NV12);
        result.setColorSpace(QVideoFrameFormat::ColorSpace_BT709);
        result.setColorRange(QVideoFrameFormat::ColorRange_Video);
        result.setColorTransfer(QVideoFrameFormat::ColorTransfer_BT709);
        return result;
    }
    MapData map(QVideoFrame::MapMode mode) override {
        if (mode != QVideoFrame::ReadOnly || CVPixelBufferLockBaseAddress(m_surface, kCVPixelBufferLock_ReadOnly) != kCVReturnSuccess)
            return {};
        m_mapped = true;
        MapData result;
        result.planeCount = 2;
        for (int plane = 0; plane < 2; ++plane) {
            result.data[plane] = static_cast<uchar*>(CVPixelBufferGetBaseAddressOfPlane(m_surface, plane));
            result.bytesPerLine[plane] = int(CVPixelBufferGetBytesPerRowOfPlane(m_surface, plane));
            result.dataSize[plane] = result.bytesPerLine[plane] * int(CVPixelBufferGetHeightOfPlane(m_surface, plane));
        }
        return result;
    }
    void unmap() override {
        if (m_mapped) { CVPixelBufferUnlockBaseAddress(m_surface, kCVPixelBufferLock_ReadOnly); m_mapped = false; }
    }
    AVFrame* nativeEncoderFrame() const override {
        auto* frame = av_frame_alloc();
        if (!frame) return nullptr;
        frame->format = AV_PIX_FMT_VIDEOTOOLBOX;
        frame->width = int(CVPixelBufferGetWidth(m_surface));
        frame->height = int(CVPixelBufferGetHeight(m_surface));
        frame->color_range = AVCOL_RANGE_MPEG;
        frame->colorspace = AVCOL_SPC_BT709;
        frame->color_primaries = AVCOL_PRI_BT709;
        frame->color_trc = AVCOL_TRC_BT709;
        auto* retained = CVPixelBufferRetain(m_surface);
        frame->buf[0] = av_buffer_create(reinterpret_cast<uint8_t*>(retained), 0,
            [](void*, uint8_t* data) { CVPixelBufferRelease(reinterpret_cast<CVPixelBufferRef>(data)); }, nullptr, 0);
        if (!frame->buf[0]) { CVPixelBufferRelease(retained); av_frame_free(&frame); return nullptr; }
        frame->data[3] = reinterpret_cast<uint8_t*>(m_surface);
        return frame;
    }
private:
    CVPixelBufferRef m_surface;
    bool m_mapped = false;
};

struct NativeState {
    std::atomic_bool closed{false};
    std::atomic_bool errorDelivered{false};
    MacScreenCapture::FrameCallback frame;
    MacScreenCapture::ErrorCallback error;
    // These strong Objective-C references are accessed only on the main queue.
    SCStream* stream = nil;
    id<SCStreamOutput, SCStreamDelegate> delegate = nil;
    dispatch_queue_t outputQueue = nil;
    bool starting = false;
    bool started = false;
    bool stopping = false;
    bool updating = false;
    CGDirectDisplayID displayID = 0;
    quint64 filterRevision = 1;
    quint64 appliedFilterRevision = 0;
    bool updatingFilter = false;
    std::atomic_bool filterReady{false};
    QSize desiredSize;
    QSize appliedSize;
    int desiredFps = 30;
    int appliedFps = 0;
    // The native output queue only reads this atomic, never the main-queue
    // configuration. Late surfaces from the old size cannot trigger CPU scale.
    std::atomic<quint64> desiredSurfaceSize{0};
};

quint64 surfaceSizeKey(QSize size) {
    return (quint64(quint32(size.width())) << 32) | quint32(size.height());
}

QSize boundedCaptureSize(QSize source, int maximumEdge) {
    if (source.width() > maximumEdge || source.height() > maximumEdge)
        source.scale(maximumEdge, maximumEdge, Qt::KeepAspectRatio);
    return QSize(std::max(2, source.width() & ~1), std::max(2, source.height() & ~1));
}

SCStreamConfiguration* configuration(QSize output, int fps) {
    SCStreamConfiguration* config = [[SCStreamConfiguration alloc] init];
    config.width = size_t(output.width()); config.height = size_t(output.height());
    config.minimumFrameInterval = CMTimeMake(1, fps);
    config.queueDepth = 3;
    config.pixelFormat = kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange;
    config.colorMatrix = kCVImageBufferYCbCrMatrix_ITU_R_709_2;
    config.colorSpaceName = kCGColorSpaceITUR_709;
    config.showsCursor = NO;
    config.scalesToFit = YES;
    if (@available(macOS 13.0, *)) config.capturesAudio = NO;
    if (@available(macOS 14.0, *)) config.preservesAspectRatio = YES;
    if (@available(macOS 15.0, *)) {
        config.captureDynamicRange = SCCaptureDynamicRangeSDR;
        config.captureMicrophone = NO;
    }
    return config;
}

void reportError(const std::shared_ptr<NativeState>& state, QString message,
                 ScreenCaptureError code = ScreenCaptureError::CaptureFailed) {
    if (state->closed.load() || state->errorDelivered.exchange(true)) return;
    state->error(code, message);
}

void reportNativeError(const std::shared_ptr<NativeState>& state, NSError* error, const char* stage) {
    const auto domain = error ? QString::fromNSString(error.domain) : QString();
    const auto nativeCode = error ? qint64(error.code) : 0;
    const auto code = MacScreenCapture::nativeErrorCode(domain, nativeCode);
    if (code == ScreenCaptureError::PermissionDenied) {
        reportError(state, QStringLiteral("macOS denied screen recording. Open System Settings > Privacy & Security > Screen & System Audio Recording and allow the application that launched Mouffette (for example Visual Studio Code or Terminal), or launch Mouffette.app directly and allow Mouffette. Then fully quit and relaunch that application and reconnect."), code);
        return;
    }
    const auto description = error ? QString::fromNSString(error.localizedDescription) : QStringLiteral("No native error details were provided");
    reportError(state, QStringLiteral("%1: %2 (domain=%3, code=%4)")
        .arg(QString::fromLatin1(stage), description, domain.isEmpty() ? QStringLiteral("none") : domain).arg(nativeCode), code);
}

QSet<quint32> sceneWindowIds() {
    QSet<quint32> ids;
    if (QGuiApplication::platformName() != QLatin1String("cocoa")) return ids;
    for (QWindow* window : WindowCaptureExclusion::instance().sceneWindows()) {
        // Qt's Cocoa WId is an NSView, not a WindowServer window number. Never
        // create a native surface while enumerating hidden/prepared scenes.
        if (!window->handle()) continue;
        NSView* view = (__bridge NSView*)reinterpret_cast<void*>(window->winId());
        NSWindow* native = [view window];
        if (native && native.windowNumber > 0) ids.insert(quint32(native.windowNumber));
    }
    return ids;
}

NSArray<SCWindow*>* sceneExceptionWindows(NSArray<SCWindow*>* windows,
                                         const QSet<quint32>& sceneIds, pid_t ownPid) {
    NSMutableArray<SCWindow*>* scenes = [NSMutableArray array];
    for (SCWindow* window in windows) {
        if (window.owningApplication.processID == ownPid && sceneIds.contains(window.windowID))
            [scenes addObject:window];
    }
    return scenes;
}

SCContentFilter* contentFilter(SCShareableContent* content, CGDirectDisplayID displayID,
                              const QSet<quint32>& sceneIds) {
    SCDisplay* selected = nil;
    for (SCDisplay* display in content.displays)
        if (display.displayID == displayID) { selected = display; break; }
    SCRunningApplication* ownApplication = nil;
    const auto ownPid = NSProcessInfo.processInfo.processIdentifier;
    for (SCRunningApplication* application in content.applications)
        if (application.processID == ownPid) { ownApplication = application; break; }
    // Never fall back to an unfiltered display if discovery is incomplete.
    if (!selected || !ownApplication) return nil;
    NSArray<SCWindow*>* scenes = sceneExceptionWindows(content.windows, sceneIds, ownPid);
    // Excluding the process automatically excludes newly created controls.
    // Explicit exceptions keep received scenes visible. Audio has its own
    // capture: SCK's audio application filters must not reuse this video policy.
    return [[SCContentFilter alloc] initWithDisplay:selected
        excludingApplications:@[ownApplication] exceptingWindows:scenes];
}

void refreshContentFilter(const std::shared_ptr<NativeState> state);

void applyContentFilter(const std::shared_ptr<NativeState> state,
                        SCContentFilter* filter, quint64 revision) {
    [state->stream updateContentFilter:filter completionHandler:^(NSError* error) {
        dispatch_async(dispatch_get_main_queue(), ^{
            state->updatingFilter = false;
            if (state->closed.load()) return;
            if (error) {
                reportNativeError(state, error, "ScreenCaptureKit window exclusion update failed");
                return;
            }
            state->appliedFilterRevision = revision;
            state->filterReady.store(revision == state->filterRevision);
            refreshContentFilter(state);
        });
    }];
}

void refreshContentFilter(const std::shared_ptr<NativeState> state) {
    if (state->closed.load() || !state->stream || !state->started || state->stopping
        || state->updatingFilter || state->filterRevision == state->appliedFilterRevision) return;
    state->updatingFilter = true;
    const auto revision = state->filterRevision;
    [SCShareableContent getShareableContentExcludingDesktopWindows:NO onScreenWindowsOnly:NO
        completionHandler:^(SCShareableContent* content, NSError* nativeError) {
            dispatch_async(dispatch_get_main_queue(), ^{
                if (state->closed.load()) { state->updatingFilter = false; return; }
                if (nativeError || !content) {
                    state->updatingFilter = false;
                    reportNativeError(state, nativeError, "ScreenCaptureKit window exclusion discovery failed");
                    return;
                }
                if (revision != state->filterRevision) {
                    state->updatingFilter = false;
                    refreshContentFilter(state);
                    return;
                }
                SCContentFilter* filter = contentFilter(content, state->displayID, sceneWindowIds());
                if (!filter) {
                    state->updatingFilter = false;
                    reportError(state, QStringLiteral("Cannot resolve the display and Mouffette windows for safe screen sharing"));
                    return;
                }
                applyContentFilter(state, filter, revision);
            });
        }];
}

void invalidateContentFilter(const std::shared_ptr<NativeState> state) {
    if (state->closed.load()) return;
    ++state->filterRevision;
    state->filterReady.store(false);
    // Retire a retained frame when a scene becomes a control or its handle is
    // destroyed. Fresh frames resume only after the latest filter is applied.
    if (state->frame) state->frame({});
    dispatch_async(dispatch_get_main_queue(), ^{ refreshContentFilter(state); });
}

// One native update at a time. A newer desired profile replaces pending work;
// its callback owns the session and cannot restart a stopped stream.
void updateConfiguration(const std::shared_ptr<NativeState> state) {
    if (state->closed.load() || !state->stream || !state->started || state->updating || state->stopping
        || (state->desiredSize == state->appliedSize && state->desiredFps == state->appliedFps)) return;
    state->updating = true;
    const auto output = state->desiredSize;
    const int fps = state->desiredFps;
    [state->stream updateConfiguration:configuration(output, fps) completionHandler:^(NSError* error) {
        dispatch_async(dispatch_get_main_queue(), ^{
            state->updating = false;
            if (state->closed.load()) return;
            if (error) {
                reportNativeError(state, error, "ScreenCaptureKit configuration update failed");
                return;
            }
            state->appliedSize = output;
            state->appliedFps = fps;
            updateConfiguration(state);
        });
    }];
}

// Main queue only. A revoked session that is still starting is stopped by its
// start completion; calling stop prematurely can otherwise leave it running.
// Pass ownership by value: Objective-C blocks preserve C++ reference captures,
// so a reference parameter would outlive the caller's shared_ptr without
// retaining NativeState through both asynchronous completion blocks.
void stopSession(const std::shared_ptr<NativeState> state) {
    if (!state->stream || state->starting || state->stopping) return;
    if (!state->started) { state->stream = nil; state->delegate = nil; state->outputQueue = nil; return; }
    state->stopping = true;
    [state->stream stopCaptureWithCompletionHandler:^(NSError*) {
        dispatch_async(dispatch_get_main_queue(), ^{
            state->stream = nil; state->delegate = nil; state->outputQueue = nil;
        });
    }];
}
}

@interface MouffetteScreenCaptureOutput : NSObject <SCStreamOutput, SCStreamDelegate> {
@public
    std::weak_ptr<NativeState> state;
}
@end

@implementation MouffetteScreenCaptureOutput
- (void)stream:(SCStream*)stream didOutputSampleBuffer:(CMSampleBufferRef)sample ofType:(SCStreamOutputType)type {
    const auto current = state.lock();
    if (!current || current->closed.load() || !current->filterReady.load()
        || type != SCStreamOutputTypeScreen || !CMSampleBufferIsValid(sample)) return;
    @autoreleasepool {
        const auto array = CMSampleBufferGetSampleAttachmentsArray(sample, false);
        if (!array || CFArrayGetCount(array) == 0) return;
        NSDictionary* attachments = (__bridge NSDictionary*)CFArrayGetValueAtIndex(array, 0);
        NSNumber* status = attachments[SCStreamFrameInfoStatus];
        if (!status) return;
        const auto action = MacScreenCapture::sampleAction(int(status.integerValue));
        if (action == MacScreenCapture::SampleAction::Suspend) {
            // Blank/suspended samples are reversible stream states, not a
            // capture failure. Clear the latest surface, then await completion.
            if (!current->closed.load()) current->frame({});
            return;
        }
        if (action == MacScreenCapture::SampleAction::Stop) {
            reportError(current, QStringLiteral("The system stopped screen capture")); return;
        }
        // Started/idle samples can contain metadata only, without a surface.
        if (action != MacScreenCapture::SampleAction::Deliver) return;
        auto surface = CMSampleBufferGetImageBuffer(sample);
        if (!surface || CVPixelBufferGetPixelFormatType(surface) != kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange
            || CVPixelBufferGetPlaneCount(surface) != 2) {
            reportError(current, QStringLiteral("ScreenCaptureKit returned an unsupported screen format"));
            return;
        }
        const QSize surfaceSize(int(CVPixelBufferGetWidth(surface)), int(CVPixelBufferGetHeight(surface)));
        if (surfaceSizeKey(surfaceSize) != current->desiredSurfaceSize.load()) return;
        QVideoFrame frame = MacScreenCapture::frameFromPixelBuffer(surface);
        auto pts = CMSampleBufferGetPresentationTimeStamp(sample);
        // Audio is captured by an independent SCStream in the helper. Convert
        // both stream clocks into CoreMedia's host epoch before transport.
        if (stream.synchronizationClock && CMTIME_IS_NUMERIC(pts))
            pts = CMSyncConvertTime(pts, stream.synchronizationClock, CMClockGetHostTimeClock());
        if (CMTIME_IS_NUMERIC(pts)) frame.setStartTime(CMTimeConvertScale(pts, 1000000, kCMTimeRoundingMethod_Default).value);
        if (!current->closed.load() && current->filterReady.load()) current->frame(frame);
    }
}
- (void)stream:(SCStream*)stream didStopWithError:(NSError*)error {
    Q_UNUSED(stream);
    if (const auto current = state.lock()) reportNativeError(current, error, "ScreenCaptureKit stream stopped");
}
@end

struct MacScreenCapture::Private {
    std::shared_ptr<NativeState> state;
    ScreenStreamProfile profile;
    QSize sourceSize;
    QMetaObject::Connection filterConnection;
};
MacScreenCapture::MacScreenCapture() : d(std::make_unique<Private>()) {}
MacScreenCapture::~MacScreenCapture() { stop(); }
bool MacScreenCapture::isActive() const { return d->state && !d->state->closed.load(); }

void MacScreenCapture::setProfile(const ScreenStreamProfile& profile) {
    d->profile = profile.normalized();
    const auto state = d->state;
    if (!state) return;
    const auto output = boundedCaptureSize(d->sourceSize, d->profile.maximumEdge);
    const int fps = d->profile.framesPerSecond;
    state->desiredSurfaceSize.store(surfaceSizeKey(output));
    dispatch_async(dispatch_get_main_queue(), ^{
        if (state->closed.load()) return;
        state->desiredSize = output;
        state->desiredFps = fps;
        updateConfiguration(state);
    });
}

ScreenCaptureError MacScreenCapture::nativeErrorCode(const QString& domain, qint64 code) {
    return domain == QString::fromNSString(SCStreamErrorDomain) && code == SCStreamErrorUserDeclined
        ? ScreenCaptureError::PermissionDenied : ScreenCaptureError::CaptureFailed;
}

MacScreenCapture::SampleAction MacScreenCapture::sampleAction(int nativeStatus) {
    switch (nativeStatus) {
    case SCFrameStatusComplete: return SampleAction::Deliver;
    case SCFrameStatusBlank: case SCFrameStatusSuspended: return SampleAction::Suspend;
    case SCFrameStatusStopped: return SampleAction::Stop;
    default: return SampleAction::Ignore;
    }
}

QVideoFrame MacScreenCapture::frameFromPixelBuffer(CVPixelBufferRef buffer) {
    if (!buffer || CVPixelBufferGetPixelFormatType(buffer) != kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange
        || CVPixelBufferGetPlaneCount(buffer) != 2) return {};
    return QVideoFrame(std::make_unique<SurfaceBuffer>(buffer));
}

bool MacScreenCapture::start(QScreen* screen, FrameCallback frame, ErrorCallback error) {
    stop();
    if (!screen || QGuiApplication::platformName() != QLatin1String("cocoa")) {
        error(ScreenCaptureError::CaptureFailed, QStringLiteral("Native macOS screen capture requires a connected Cocoa screen")); return false;
    }
    const auto* native = screen->nativeInterface<QNativeInterface::QCocoaScreen>();
    NSNumber* number = native ? [[native->nativeScreen() deviceDescription] objectForKey:@"NSScreenNumber"] : nil;
    if (!number) { error(ScreenCaptureError::CaptureFailed, QStringLiteral("Cannot resolve the selected macOS display")); return false; }
    const CGDirectDisplayID displayID = number.unsignedIntValue;
    d->sourceSize = QSize(qRound(screen->geometry().width() * screen->devicePixelRatio()),
                          qRound(screen->geometry().height() * screen->devicePixelRatio()));
    const auto state = std::make_shared<NativeState>();
    state->displayID = displayID;
    state->frame = std::move(frame); state->error = std::move(error);
    state->desiredSize = boundedCaptureSize(d->sourceSize, d->profile.maximumEdge);
    state->desiredFps = d->profile.framesPerSecond;
    state->desiredSurfaceSize.store(surfaceSizeKey(state->desiredSize));
    d->state = state;
    auto& exclusions = WindowCaptureExclusion::instance();
    d->filterConnection = QObject::connect(&exclusions, &WindowCaptureExclusion::sceneWindowsChanged,
        &exclusions, [weak = std::weak_ptr<NativeState>(state)] {
            if (const auto active = weak.lock()) invalidateContentFilter(active);
        });
    const auto initialFilterRevision = state->filterRevision;
    // ScreenCaptureKit performs the OS screen-recording authorization. Never
    // enumerate content until local sharing is enabled and a viewer subscribes.
    [SCShareableContent getShareableContentExcludingDesktopWindows:NO onScreenWindowsOnly:NO
        completionHandler:^(SCShareableContent* content, NSError* nativeError) {
            dispatch_async(dispatch_get_main_queue(), ^{
                if (state->closed.load()) return;
                if (nativeError || !content) {
                    reportNativeError(state, nativeError, "ScreenCaptureKit content discovery failed"); return;
                }
                SCContentFilter* filter = contentFilter(content, displayID, sceneWindowIds());
                if (!filter) {
                    reportError(state, QStringLiteral("Cannot resolve the display and Mouffette windows for safe screen sharing"));
                    return;
                }
                state->appliedFilterRevision = initialFilterRevision;
                state->appliedSize = state->desiredSize;
                state->appliedFps = state->desiredFps;
                SCStreamConfiguration* config = configuration(state->appliedSize, state->appliedFps);
                auto* delegate = [[MouffetteScreenCaptureOutput alloc] init];
                delegate->state = state;
                state->delegate = delegate;
                state->outputQueue = dispatch_queue_create("Mouffette.ScreenCaptureKit", DISPATCH_QUEUE_SERIAL);
                state->stream = [[SCStream alloc] initWithFilter:filter configuration:config delegate:delegate];
                NSError* addError = nil;
                if (![state->stream addStreamOutput:delegate type:SCStreamOutputTypeScreen
                    sampleHandlerQueue:state->outputQueue error:&addError]) {
                    reportNativeError(state, addError, "ScreenCaptureKit output attachment failed"); return;
                }
                state->starting = true;
                [state->stream startCaptureWithCompletionHandler:^(NSError* startError) {
                    dispatch_async(dispatch_get_main_queue(), ^{
                        state->starting = false;
                        state->started = !startError;
                        if (startError) reportNativeError(state, startError, "ScreenCaptureKit startup failed");
                        if (state->closed.load()) stopSession(state);
                        else if (state->started) {
                            state->filterReady.store(state->appliedFilterRevision == state->filterRevision);
                            refreshContentFilter(state);
                            updateConfiguration(state);
                        }
                    });
                }];
            });
        }];
    return true;
}

void MacScreenCapture::stop() {
    QObject::disconnect(d->filterConnection);
    d->filterConnection = {};
    const auto state = std::exchange(d->state, {});
    if (!state) return;
    state->closed.store(true);
    dispatch_async(dispatch_get_main_queue(), ^{
        // Keep stream, delegate and queue alive through completion. Every late
        // discovery/start/frame/error callback is fenced by this session state.
        stopSession(state);
    });
}
