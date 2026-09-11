#import "MacVideoThumbnailer.h"
#ifdef Q_OS_MACOS
#import <Foundation/Foundation.h>
#import <AVFoundation/AVFoundation.h>
#import <CoreGraphics/CoreGraphics.h>
#include <QByteArray>

QSize MacVideoThumbnailer::videoDimensions(const QString& localFilePath) {
    @autoreleasepool {
        QByteArray utf8 = localFilePath.toUtf8();
        NSString* nsPath = [NSString stringWithUTF8String:utf8.constData()];
        if (!nsPath) return QSize();
        NSURL* url = [NSURL fileURLWithPath:nsPath];
        if (!url) return QSize();

        AVURLAsset* asset = [AVURLAsset URLAssetWithURL:url options:nil];
        if (!asset) return QSize();
        
        __block NSArray<AVAssetTrack*>* videoTracks = nil;
        dispatch_semaphore_t tracksReady = dispatch_semaphore_create(0);
        if (@available(macOS 12.0, *)) {
            [asset loadTracksWithMediaType:AVMediaTypeVideo completionHandler:^(NSArray<AVAssetTrack*>* tracks, NSError*) {
                videoTracks = [tracks retain];
                dispatch_semaphore_signal(tracksReady);
            }];
            const dispatch_time_t timeout = dispatch_time(DISPATCH_TIME_NOW, 3 * NSEC_PER_SEC);
            if (dispatch_semaphore_wait(tracksReady, timeout) != 0) return QSize();
        } else {
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
            videoTracks = [[asset tracksWithMediaType:AVMediaTypeVideo] retain];
#pragma clang diagnostic pop
        }
        if (videoTracks.count == 0) {
            [videoTracks release];
            return QSize();
        }
        
        AVAssetTrack* videoTrack = videoTracks[0];
        CGSize naturalSize = [videoTrack naturalSize];
        
        // Apply transform to get display size (handles rotation)
        CGAffineTransform transform = [videoTrack preferredTransform];
        CGSize displaySize = CGSizeApplyAffineTransform(naturalSize, transform);
        
        const QSize dimensions(std::abs(displaySize.width), std::abs(displaySize.height));
        [videoTracks release];
        return dimensions;
    }
}

QImage MacVideoThumbnailer::firstFrame(const QString& localFilePath) {
    @autoreleasepool {
        QByteArray utf8 = localFilePath.toUtf8();
        NSString* nsPath = [NSString stringWithUTF8String:utf8.constData()];
        if (!nsPath) return QImage();
        NSURL* url = [NSURL fileURLWithPath:nsPath];
        if (!url) return QImage();

        // Decode the exact frame at t=0. System thumbnail APIs deliberately
        // choose representative frames and therefore cannot satisfy the drag
        // preview's first-frame contract.
        AVURLAsset* asset = [AVURLAsset URLAssetWithURL:url options:nil];
        if (!asset) return QImage();
        AVAssetImageGenerator* gen = [[AVAssetImageGenerator alloc] initWithAsset:asset];
        gen.appliesPreferredTrackTransform = YES;
        gen.maximumSize = CGSizeMake(2048, 2048);
        gen.requestedTimeToleranceAfter = kCMTimeZero;
        gen.requestedTimeToleranceBefore = kCMTimeZero;
        CMTime time = CMTimeMake(0, 600);
        __block CGImageRef cgImg = nullptr;
        dispatch_semaphore_t imageReady = dispatch_semaphore_create(0);
        NSArray<NSValue*>* times = @[[NSValue valueWithCMTime:time]];
        [gen generateCGImagesAsynchronouslyForTimes:times
                                  completionHandler:^(CMTime, CGImageRef image, CMTime,
                                                      AVAssetImageGeneratorResult result, NSError*) {
            if (result == AVAssetImageGeneratorSucceeded && image) {
                cgImg = CGImageRetain(image);
            }
            dispatch_semaphore_signal(imageReady);
        }];
        const dispatch_time_t timeout = dispatch_time(DISPATCH_TIME_NOW, 5 * NSEC_PER_SEC);
        if (dispatch_semaphore_wait(imageReady, timeout) != 0) {
            [gen cancelAllCGImageGeneration];
            [gen release];
            return QImage();
        }
        if (!cgImg) {
            [gen release];
            return QImage();
        }

        const size_t width = CGImageGetWidth(cgImg);
        const size_t height = CGImageGetHeight(cgImg);
        QImage image((int)width, (int)height, QImage::Format_ARGB32_Premultiplied);
        image.fill(Qt::transparent);
        CGContextRef ctx = CGBitmapContextCreate(image.bits(), width, height, 8, image.bytesPerLine(), CGImageGetColorSpace(cgImg), kCGImageAlphaPremultipliedFirst | kCGBitmapByteOrder32Host);
        if (ctx) {
            CGContextDrawImage(ctx, CGRectMake(0, 0, width, height), cgImg);
            CGContextRelease(ctx);
        }
        CGImageRelease(cgImg);
        [gen release];
        return image;
    }
}
#endif
