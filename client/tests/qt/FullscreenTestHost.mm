#import <Cocoa/Cocoa.h>
#include <cstdio>
#include <cstring>

// A separate application owns the fullscreen Space. A second window in the
// tested process cannot exercise AppKit's cross-application Space rules.
@interface FullscreenTestDelegate : NSObject <NSWindowDelegate, NSApplicationDelegate>
@property(retain) NSWindow* window;
@property NSUInteger entryAttempts;
@end
@implementation FullscreenTestDelegate
- (void)applicationDidFinishLaunching:(NSNotification*)notification
{
    [self enterFullscreen];
}
- (void)enterFullscreen
{
    ++self.entryAttempts;
    [self.window makeKeyAndOrderFront:nil];
    [NSApp activateIgnoringOtherApps:YES];
    // Activation and removal of a preceding Space animate asynchronously.
    // Allow that transition to settle before requesting a new fullscreen Space.
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 600 * NSEC_PER_MSEC),
                   dispatch_get_main_queue(), ^{ [self.window toggleFullScreen:nil]; });
}
- (void)windowDidEnterFullScreen:(NSNotification*)notification
{
    std::printf("READY %ld\n", (long)self.window.windowNumber);
    std::fflush(stdout);
}
- (void)windowDidFailToEnterFullScreen:(NSWindow*)window
{
    if (self.entryAttempts < 3) {
        [self enterFullscreen];
        return;
    }
    std::puts("FAILED to enter fullscreen");
    std::fflush(stdout);
}
- (void)windowDidExitFullScreen:(NSNotification*)notification
{
    [NSApp terminate:nil];
}
@end

int main()
{
    @autoreleasepool {
        NSApplication* app = [NSApplication sharedApplication];
        [app setActivationPolicy:NSApplicationActivationPolicyRegular];
        NSWindow* window = [[NSWindow alloc] initWithContentRect:NSMakeRect(100, 100, 700, 450)
            styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable | NSWindowStyleMaskResizable
            backing:NSBackingStoreBuffered defer:NO];
        FullscreenTestDelegate* delegate = [[FullscreenTestDelegate alloc] init];
        delegate.window = window;
        window.delegate = delegate;
        app.delegate = delegate;
        window.title = @"Mouffette fullscreen test fixture";
        window.collectionBehavior = NSWindowCollectionBehaviorFullScreenPrimary;
        dispatch_async(dispatch_get_global_queue(QOS_CLASS_DEFAULT, 0), ^{
            char command[32];
            while (std::fgets(command, sizeof(command), stdin)) {
                if (std::strncmp(command, "QUIT", 4) == 0) {
                    dispatch_async(dispatch_get_main_queue(), ^{
                        if (window.styleMask & NSWindowStyleMaskFullScreen) [window toggleFullScreen:nil];
                        else [app terminate:nil];
                    });
                    break;
                }
            }
        });
        [app run];
    }
}
