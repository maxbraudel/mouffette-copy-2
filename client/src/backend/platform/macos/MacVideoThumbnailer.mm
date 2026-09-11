#import "MacVideoThumbnailer.h"
#ifdef Q_OS_MACOS
#import <Foundation/Foundation.h>
#import <AVFoundation/AVFoundation.h>
#import <CoreGraphics/CoreGraphics.h>
#import <QuickLookThumbnailing/QuickLookThumbnailing.h>
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

        // Try QuickLook first for near-instant thumbnails
        if (@available(macOS 10.15, *)) {
            CGSize targetSize = CGSizeMake(640, 360);
            QLThumbnailGenerationRequest* request = [[[QLThumbnailGenerationRequest alloc] initWithFileAtURL:url size:targetSize scale:1.0 representationTypes:QLThumbnailGenerationRequestRepresentationTypeThumbnail] autorelease];
            if (request) {
                dispatch_semaphore_t sem = dispatch_semaphore_create(0);
                __block QImage quickLookImage;
                [[QLThumbnailGenerator sharedGenerator] generateBestRepresentationForRequest:request completionHandler:^(QLThumbnailRepresentation * _Nullable thumbnail, NSError * _Nullable error) {
                    if (thumbnail) {
                        CGImageRef cgThumb = [thumbnail CGImage];
                        if (cgThumb) {
                            const size_t w = CGImageGetWidth(cgThumb);
                            const size_t h = CGImageGetHeight(cgThumb);
                            QImage img((int)w, (int)h, QImage::Format_ARGB32_Premultiplied);
                            img.fill(Qt::transparent);
                            CGContextRef ctx = CGBitmapContextCreate(img.bits(), w, h, 8, img.bytesPerLine(), CGImageGetColorSpace(cgThumb), kCGImageAlphaPremultipliedFirst | kCGBitmapByteOrder32Host);
                            if (ctx) {
                                CGContextDrawImage(ctx, CGRectMake(0, 0, w, h), cgThumb);
                                CGContextRelease(ctx);
                            }
                            quickLookImage = img;
                        }
                    }
                    dispatch_semaphore_signal(sem);
                }];

                dispatch_time_t timeout = dispatch_time(DISPATCH_TIME_NOW, (int64_t)(0.15 * NSEC_PER_SEC));
                if (dispatch_semaphore_wait(sem, timeout) == 0 && !quickLookImage.isNull()) {
                    return quickLookImage;
                }
            }
        }

        // Fallback to AVFoundation extraction
        AVURLAsset* asset = [AVURLAsset URLAssetWithURL:url options:nil];
        if (!asset) return QImage();
        AVAssetImageGenerator* gen = [[AVAssetImageGenerator alloc] initWithAsset:asset];
        gen.appliesPreferredTrackTransform = YES;
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
            return QImage();
        }
        if (!cgImg) {
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
        return image;
    }
}
#endif
