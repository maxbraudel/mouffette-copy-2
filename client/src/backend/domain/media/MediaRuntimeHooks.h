#pragma once

#include <QPointF>
#include <QRectF>
#include <QSize>
#include <QObject>

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

// Session-owned callbacks.  Each canvas creates one context and injects it
// into its media items; QObject lifetime makes every callback disappear with
// that canvas instead of being overwritten by another open session.
class Context final : public QObject {
public:
    explicit Context(QObject* parent = nullptr) : QObject(parent) {}

    ScreenSnapCallback screenSnapCallback;
    ResizeSnapCallback resizeSnapCallback;
    MediaSettingsChangedNotifier mediaSettingsChangedNotifier;
    MediaOpacityAnimationTickNotifier mediaOpacityAnimationTickNotifier;
};

void setUploadChangedNotifier(UploadChangedNotifier cb);
UploadChangedNotifier uploadChangedNotifier();

void setFileErrorNotifier(FileErrorNotifier cb);
FileErrorNotifier fileErrorNotifier();

void setFileManager(FileManager* manager);
FileManager* fileManager();
} // namespace MediaRuntimeHooks
