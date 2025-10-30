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
        
        NSArray<AVAssetTrack*>* videoTracks = [asset tracksWithMediaType:AVMediaTypeVideo];
        if (videoTracks.count == 0) return QSize();
        
        AVAssetTrack* videoTrack = videoTracks[0];
        CGSize naturalSize = [videoTrack naturalSize];
        
        // Apply transform to get display size (handles rotation)
        CGAffineTransform transform = [videoTrack preferredTransform];
        CGSize displaySize = CGSizeApplyAffineTransform(naturalSize, transform);
        
        return QSize(std::abs(displaySize.width), std::abs(displaySize.height));
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
        CMTime actual;
        NSError* error = nil;
        CGImageRef cgImg = [gen copyCGImageAtTime:time actualTime:&actual error:&error];
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
