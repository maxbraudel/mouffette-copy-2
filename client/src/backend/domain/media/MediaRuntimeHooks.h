#pragma once

#include <QPointF>
#include <QRectF>
#include <QSize>

#include <functional>

class FileManager;
class ResizableMediaBase;

struct ResizeSnapFeedback {
    qreal scale = 1.0;
    bool cornerSnapped = false;
    QPointF snappedMovingCornerScene;
};

namespace MediaRuntimeHooks {
using UploadChangedNotifier = std::function<void()>;
using FileErrorNotifier = std::function<void(ResizableMediaBase*)>;
using ScreenSnapCallback = std::function<QPointF(const QPointF&, const QRectF&, bool, ResizableMediaBase*)>;
using ResizeSnapCallback = std::function<ResizeSnapFeedback(qreal, const QPointF&, const QPointF&, const QSize&, bool, ResizableMediaBase*)>;
using MediaSettingsChangedNotifier = std::function<void(ResizableMediaBase*)>;
using MediaOpacityAnimationTickNotifier = std::function<void()>;

void setUploadChangedNotifier(UploadChangedNotifier cb);
UploadChangedNotifier uploadChangedNotifier();

void setFileErrorNotifier(FileErrorNotifier cb);
FileErrorNotifier fileErrorNotifier();

void setScreenSnapCallback(ScreenSnapCallback cb);
ScreenSnapCallback screenSnapCallback();

void setResizeSnapCallback(ResizeSnapCallback cb);
ResizeSnapCallback resizeSnapCallback();

void setFileManager(FileManager* manager);
FileManager* fileManager();

void setMediaSettingsChangedNotifier(MediaSettingsChangedNotifier cb);
MediaSettingsChangedNotifier mediaSettingsChangedNotifier();

void setMediaOpacityAnimationTickNotifier(MediaOpacityAnimationTickNotifier cb);
MediaOpacityAnimationTickNotifier mediaOpacityAnimationTickNotifier();
} // namespace MediaRuntimeHooks
