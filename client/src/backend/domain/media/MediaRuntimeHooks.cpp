#include "backend/domain/media/MediaRuntimeHooks.h"

namespace {
MediaRuntimeHooks::UploadChangedNotifier g_uploadChangedNotifier;
MediaRuntimeHooks::FileErrorNotifier g_fileErrorNotifier;
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

void setFileManager(FileManager* manager) {
    g_fileManager = manager;
}

FileManager* fileManager() {
    return g_fileManager;
}

} // namespace MediaRuntimeHooks
