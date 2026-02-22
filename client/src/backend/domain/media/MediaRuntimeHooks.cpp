#include "backend/domain/media/MediaRuntimeHooks.h"

namespace {
MediaRuntimeHooks::UploadChangedNotifier g_uploadChangedNotifier;
MediaRuntimeHooks::FileErrorNotifier g_fileErrorNotifier;
MediaRuntimeHooks::ScreenSnapCallback g_screenSnapCallback;
MediaRuntimeHooks::ResizeSnapCallback g_resizeSnapCallback;
FileManager* g_fileManager = nullptr;
}

namespace MediaRuntimeHooks {
void setUploadChangedNotifier(UploadChangedNotifier cb) {
    g_uploadChangedNotifier = std::move(cb);
}

UploadChangedNotifier uploadChangedNotifier() {
    return g_uploadChangedNotifier;
}

void setFileErrorNotifier(FileErrorNotifier cb) {
    g_fileErrorNotifier = std::move(cb);
}

FileErrorNotifier fileErrorNotifier() {
    return g_fileErrorNotifier;
}

void setScreenSnapCallback(ScreenSnapCallback cb) {
    g_screenSnapCallback = std::move(cb);
}

ScreenSnapCallback screenSnapCallback() {
    return g_screenSnapCallback;
}

void setResizeSnapCallback(ResizeSnapCallback cb) {
    g_resizeSnapCallback = std::move(cb);
}

ResizeSnapCallback resizeSnapCallback() {
    return g_resizeSnapCallback;
}

void setFileManager(FileManager* manager) {
    g_fileManager = manager;
}

FileManager* fileManager() {
    return g_fileManager;
}
} // namespace MediaRuntimeHooks
