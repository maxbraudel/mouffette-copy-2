#include "backend/audiosharing/SystemAudioCapture.h"
#include "backend/audiosharing/MediaCaptureClock.h"
#include <QTimer>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <utility>
#import <ScreenCaptureKit/ScreenCaptureKit.h>
#import <CoreAudio/CoreAudio.h>
#import <CoreMedia/CoreMedia.h>
#import <CoreGraphics/CoreGraphics.h>
#import <AppKit/AppKit.h>

namespace {
struct NativeAudio {
    std::atomic_bool closed{false};
    std::atomic<float> left{1}, right{1};
    SystemAudioCapture::Pcm pcm;
    SystemAudioCapture::State state;
    SCStream* stream = nil;
    id<SCStreamOutput, SCStreamDelegate> delegate = nil;
    dispatch_queue_t queue = nil;
    bool starting = false, started = false, stopping = false;
    qint64 timestampOffset = 0;
    bool mappedTime = false;
    CGDirectDisplayID display = 0;
};
template<typename T> bool property(AudioObjectID device, AudioObjectPropertySelector selector,
                                   AudioObjectPropertyElement element, T& value,
                                   AudioObjectPropertyScope scope = kAudioObjectPropertyScopeOutput) {
    AudioObjectPropertyAddress address{selector, scope, element};
    UInt32 size = sizeof(value);
    return AudioObjectGetPropertyData(device, &address, 0, nullptr, &size, &value) == noErr && size == sizeof(value);
}
float channelGain(AudioDeviceID device, UInt32 channel) {
    UInt32 muted = 0;
    if (property(device, kAudioDevicePropertyMute, channel, muted) && muted) return 0;
    for (const auto element : {channel, UInt32(kAudioObjectPropertyElementMain)}) {
        Float32 db = 0;
        if (property(device, kAudioDevicePropertyVolumeDecibels, element, db) && !std::isnan(db) && db <= 0)
            return std::pow(10.0f, db / 20.0f); // -infinity is a silent channel.
        Float32 scalar = 1;
        if (property(device, kAudioDevicePropertyVolumeScalar, element, scalar)) {
            if (scalar <= 0) return 0;
            // A CoreAudio slider value is not an amplitude multiplier. Ask the
            // device for its actual taper before applying attenuation once.
            db = scalar;
            if (property(device, kAudioDevicePropertyVolumeScalarToDecibels, element, db)
                && !std::isnan(db) && db <= 0) return std::pow(10.0f, db / 20.0f);
        }
    }
    // Outputs with no readable hardware attenuation (e.g. HDMI) use unity.
    return 1;
}
void updateGain(const std::shared_ptr<NativeAudio>& state) {
    AudioDeviceID device = kAudioObjectUnknown;
    if (!property(kAudioObjectSystemObject, kAudioHardwarePropertyDefaultOutputDevice,
                  kAudioObjectPropertyElementMain, device, kAudioObjectPropertyScopeGlobal)
        || device == kAudioObjectUnknown) return; // A transient route error must not restore full volume.
    UInt32 mute = 0;
    if (property(device, kAudioDevicePropertyMute, kAudioObjectPropertyElementMain, mute) && mute) {
        state->left = 0; state->right = 0; return;
    }
    UInt32 stereo[2]{1, 2};
    property(device, kAudioDevicePropertyPreferredChannelsForStereo, kAudioObjectPropertyElementMain, stereo);
    state->left = channelGain(device, stereo[0]); state->right = channelGain(device, stereo[1]);
}
void stopNative(std::shared_ptr<NativeAudio> state) {
    if (!state->stream || state->starting || state->stopping) return;
    if (!state->started) { state->stream = nil; state->delegate = nil; state->queue = nil; return; }
    state->stopping = true;
    [state->stream stopCaptureWithCompletionHandler:^(NSError*) {
        dispatch_async(dispatch_get_main_queue(), ^{
            state->stream = nil; state->delegate = nil; state->queue = nil;
        });
    }];
}
QString describe(NSError* error) {
    if (error.code == SCStreamErrorUserDeclined)
        return QStringLiteral("System audio recording was denied. Allow Mouffette in System Settings > Privacy & Security > Screen & System Audio Recording, then restart Mouffette.");
    return error ? QString::fromNSString(error.localizedDescription) : QStringLiteral("System audio capture is unavailable");
}
}
@interface MouffetteAudioCaptureOutput : NSObject <SCStreamOutput, SCStreamDelegate> {
@public std::weak_ptr<NativeAudio> state;
}
@end
@implementation MouffetteAudioCaptureOutput
- (void)stream:(SCStream*)stream didOutputSampleBuffer:(CMSampleBufferRef)sample ofType:(SCStreamOutputType)type {
    if (type != SCStreamOutputTypeAudio || !CMSampleBufferIsValid(sample)) return;
    const auto current = state.lock();
    if (!current || current->closed.load()) return;
    @autoreleasepool {
        const auto format = CMSampleBufferGetFormatDescription(sample);
        const auto* audio = format ? CMAudioFormatDescriptionGetStreamBasicDescription(format) : nullptr;
        if (!audio || audio->mFormatID != kAudioFormatLinearPCM || !(audio->mFormatFlags & kAudioFormatFlagIsFloat)
            || audio->mBitsPerChannel != 32 || audio->mSampleRate != 48000
            || audio->mChannelsPerFrame < 1 || audio->mChannelsPerFrame > 2) return;
        const auto frames = CMSampleBufferGetNumSamples(sample);
        if (frames < 1 || frames > 4800) return;
        size_t needed = 0;
        if (CMSampleBufferGetAudioBufferListWithRetainedBlockBuffer(sample, &needed, nullptr, 0,
                kCFAllocatorDefault, kCFAllocatorDefault, kCMSampleBufferFlag_AudioBufferList_Assure16ByteAlignment,
                nullptr) != noErr || needed > 65536) return;
        QByteArray storage(qsizetype(needed), Qt::Uninitialized);
        auto* buffers = reinterpret_cast<AudioBufferList*>(storage.data());
        CMBlockBufferRef block = nullptr;
        if (CMSampleBufferGetAudioBufferListWithRetainedBlockBuffer(sample, nullptr, buffers, needed,
                kCFAllocatorDefault, kCFAllocatorDefault, kCMSampleBufferFlag_AudioBufferList_Assure16ByteAlignment,
                &block) != noErr) return;
        const bool planar = audio->mFormatFlags & kAudioFormatFlagIsNonInterleaved;
        const int channels = int(audio->mChannelsPerFrame);
        const bool valid = buffers->mNumberBuffers >= (planar ? channels : 1)
            && buffers->mBuffers[0].mData && buffers->mBuffers[0].mDataByteSize >= frames * sizeof(float) * (planar ? 1 : channels)
            && (!planar || channels == 1 || (buffers->mBuffers[1].mData && buffers->mBuffers[1].mDataByteSize >= frames * sizeof(float)));
        if (valid) {
            QByteArray pcm(frames * 2 * sizeof(float), Qt::Uninitialized);
            auto* target = reinterpret_cast<float*>(pcm.data());
            const auto* first = static_cast<const float*>(buffers->mBuffers[0].mData);
            const auto* second = planar && channels == 2 ? static_cast<const float*>(buffers->mBuffers[1].mData) : first;
            const float left = current->left.load(), right = current->right.load();
            for (int frame = 0; frame < frames; ++frame) {
                target[frame * 2] = first[frame * (planar ? 1 : channels)] * left;
                target[frame * 2 + 1] = (planar ? second[frame] : first[frame * channels + (channels == 2 ? 1 : 0)]) * right;
            }
            auto pts = CMSampleBufferGetPresentationTimeStamp(sample);
            if (stream.synchronizationClock && CMTIME_IS_NUMERIC(pts))
                pts = CMSyncConvertTime(pts, stream.synchronizationClock, CMClockGetHostTimeClock());
            const auto now = MediaCaptureClock::nowUs();
            qint64 native = CMTIME_IS_NUMERIC(pts) ? CMTimeConvertScale(pts, 1000000, kCMTimeRoundingMethod_Default).value : now;
            if (!current->mappedTime) {
                current->mappedTime = true;
                current->timestampOffset = std::abs(now - native) < 5000000 ? 0 : now - native;
            }
            if (!current->closed.load()) current->pcm(std::move(pcm), native + current->timestampOffset);
        }
        if (block) CFRelease(block);
    }
}
- (void)stream:(SCStream*)stream didStopWithError:(NSError*)error {
    Q_UNUSED(stream);
    if (const auto current = state.lock(); current && !current->closed.load()) current->state(false, describe(error));
}
@end

namespace {
class MacAudioCapture final : public SystemAudioCapture {
public:
    std::shared_ptr<NativeAudio> state;
    QTimer volume;
    MacAudioCapture() {
        volume.setInterval(50);
        QObject::connect(&volume, &QTimer::timeout, &volume, [this] {
            if (!state || state->closed.load()) return;
            updateGain(state);
            // Audio ownership never follows a viewport or a video subscriber.
            // A vanished reference display is rebound within this one session.
            if (state->started && state->display && !CGDisplayIsActive(state->display)) updateFilter(state);
        });
    }
    ~MacAudioCapture() override { stop(); }
    static void updateFilter(std::shared_ptr<NativeAudio> current) {
        current->display = 0;
        [SCShareableContent getShareableContentExcludingDesktopWindows:NO onScreenWindowsOnly:NO
            completionHandler:^(SCShareableContent* content, NSError* error) {
                dispatch_async(dispatch_get_main_queue(), ^{
                    if (current->closed.load() || !current->stream) return;
                    SCDisplay* display = content.displays.firstObject;
                    if (error || !display) { current->state(false, describe(error)); return; }
                    current->display = display.displayID;
                    auto* filter = [[SCContentFilter alloc] initWithDisplay:display excludingWindows:@[]];
                    [current->stream updateContentFilter:filter completionHandler:^(NSError* failure) {
                        if (failure && !current->closed.load()) current->state(false, describe(failure));
                    }];
                });
            }];
    }
    void start(Pcm pcm, State status) override {
        stop();
        if (@available(macOS 13.0, *)) {
            auto current = std::make_shared<NativeAudio>(); current->pcm = std::move(pcm); current->state = std::move(status);
            state = current; updateGain(current); volume.start();
            [SCShareableContent getShareableContentExcludingDesktopWindows:NO onScreenWindowsOnly:NO
                completionHandler:^(SCShareableContent* content, NSError* error) {
                    dispatch_async(dispatch_get_main_queue(), ^{
                        if (current->closed.load()) return;
                        SCDisplay* display = nil;
                        for (SCDisplay* candidate in content.displays) if (candidate.displayID == CGMainDisplayID()) { display = candidate; break; }
                        if (!display) display = content.displays.firstObject;
                        if (error || !display) { current->state(false, describe(error)); return; }
                        current->display = display.displayID;
                        auto* filter = [[SCContentFilter alloc] initWithDisplay:display excludingWindows:@[]];
                        auto* config = [[SCStreamConfiguration alloc] init];
                        config.width = 2; config.height = 2; config.minimumFrameInterval = CMTimeMake(1, 1);
                        config.queueDepth = 3; config.showsCursor = NO;
                        config.capturesAudio = YES; config.excludesCurrentProcessAudio = YES;
                        config.sampleRate = 48000; config.channelCount = 2;
                        if (@available(macOS 15.0, *)) config.captureMicrophone = NO;
                        auto* output = [[MouffetteAudioCaptureOutput alloc] init]; output->state = current;
                        current->delegate = output;
                        current->queue = dispatch_queue_create("Mouffette.SystemAudio", DISPATCH_QUEUE_SERIAL);
                        current->stream = [[SCStream alloc] initWithFilter:filter configuration:config delegate:output];
                        NSError* attachError = nil;
                        // SCK expects a screen output on some OS releases. Its
                        // tiny 1 fps surfaces are discarded above, never encoded.
                        if (![current->stream addStreamOutput:output type:SCStreamOutputTypeScreen sampleHandlerQueue:current->queue error:&attachError]
                            || ![current->stream addStreamOutput:output type:SCStreamOutputTypeAudio sampleHandlerQueue:current->queue error:&attachError]) {
                            current->state(false, describe(attachError)); return;
                        }
                        current->starting = true;
                        [current->stream startCaptureWithCompletionHandler:^(NSError* failure) {
                            dispatch_async(dispatch_get_main_queue(), ^{
                                current->starting = false; current->started = !failure;
                                if (current->closed.load()) stopNative(current);
                                else current->state(!failure, failure ? describe(failure) : QString());
                            });
                        }];
                    });
                }];
        } else status(false, QStringLiteral("System audio sharing requires macOS 13 or later"));
    }
    void stop() override {
        volume.stop(); auto current = std::exchange(state, {});
        if (!current) return;
        current->closed.store(true);
        dispatch_async(dispatch_get_main_queue(), ^{ stopNative(current); });
    }
};
}
std::unique_ptr<SystemAudioCapture> createSystemAudioCapture() { return std::make_unique<MacAudioCapture>(); }
void initializeAudioWorkerPlatform() { [NSApp setActivationPolicy:NSApplicationActivationPolicyProhibited]; }
