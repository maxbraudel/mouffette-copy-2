#import "MacVideoThumbnailer.h"
#ifdef Q_OS_MACOS
#import <Foundation/Foundation.h>
#import <AVFoundation/AVFoundation.h>
#import <CoreGraphics/CoreGraphics.h>
#include <QByteArray>

QImage MacVideoThumbnailer::firstFrame(const QString& localFilePath) {
    @autoreleasepool {
    QByteArray utf8 = localFilePath.toUtf8();
    NSString* nsPath = [NSString stringWithUTF8String:utf8.constData()];
    if (!nsPath) return QImage();
    NSURL* url = [NSURL fileURLWithPath:nsPath];
        if (!url) return QImage();
        AVURLAsset* asset = [AVURLAsset URLAssetWithURL:url options:nil];
        if (!asset) return QImage();
        AVAssetImageGenerator* gen = [[AVAssetImageGenerator alloc] initWithAsset:asset];
        gen.appliesPreferredTrackTransform = YES;
        gen.requestedTimeToleranceAfter = kCMTimeZero;
        gen.requestedTimeToleranceBefore = kCMTimeZero;
        CMTime time = CMTimeMake(0, 600); // time 0
        CMTime actual;
        NSError* error = nil;
        CGImageRef cgImg = [gen copyCGImageAtTime:time actualTime:&actual error:&error];
        if (!cgImg) {
            return QImage();
        }
        // Convert CGImageRef to QImage (ARGB32)
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
