#include "files/FileWatcher.h"
#include "domain/media/MediaItems.h"
#include <QFileInfo>
#include <QDir>
#include <QDebug>

FileWatcher::FileWatcher(QObject* parent) 
    : QObject(parent)
    , m_watcher(new QFileSystemWatcher(this))
    , m_delayedCheckTimer(new QTimer(this))
{
    // Connect file system watcher signals
    connect(m_watcher, &QFileSystemWatcher::fileChanged, this, &FileWatcher::onFileDeleted);
    connect(m_watcher, &QFileSystemWatcher::directoryChanged, this, &FileWatcher::onDirectoryChanged);
    
    // Setup delayed check timer (batch multiple file checks)
    m_delayedCheckTimer->setSingleShot(true);
    m_delayedCheckTimer->setInterval(500); // 500ms delay to batch checks
    connect(m_delayedCheckTimer, &QTimer::timeout, this, &FileWatcher::performDelayedCheck);
}

FileWatcher::~FileWatcher() {
    clearAll();
}

void FileWatcher::watchMediaItem(ResizableMediaBase* mediaItem) {
    if (!mediaItem) return;
    
    const QString filePath = mediaItem->sourcePath();
    if (filePath.isEmpty()) return;
    
    // Check if file is accessible before adding to watch
    if (!isFileAccessible(filePath)) {
        qDebug() << "FileWatcher: File is not accessible, will not watch:" << filePath;
        return;
    }
    
    // If this media item was already watching a different file, stop watching the old one
    if (m_mediaToFile.contains(mediaItem)) {
        const QString oldPath = m_mediaToFile.value(mediaItem);
        if (oldPath != filePath) {
            removeFileFromWatch(oldPath, mediaItem);
        }
    }
    
    addFileToWatch(filePath, mediaItem);
    m_mediaToFile[mediaItem] = filePath;
    
    qDebug() << "FileWatcher: Now watching file" << filePath << "for media item" << mediaItem->mediaId();
}

void FileWatcher::unwatchMediaItem(ResizableMediaBase* mediaItem) {
    if (!mediaItem) return;
    
    // Safety check: verify mediaId is valid before proceeding
    QString mediaId = mediaItem->mediaId();
    if (mediaId.isEmpty() || mediaId.contains('\0')) {
        qDebug() << "FileWatcher: Trying to unwatch media item with invalid mediaId:" << mediaId;
        // Still try to clean up if we can find the item in our maps
        for (auto it = m_mediaToFile.begin(); it != m_mediaToFile.end(); ++it) {
            if (it.key() == mediaItem) {
                const QString filePath = it.value();
                removeFileFromWatch(filePath, mediaItem);
                m_mediaToFile.erase(it);
                qDebug() << "FileWatcher: Cleaned up corrupted media item mapping";
                return;
            }
        }
        return;
    }
    
    if (m_mediaToFile.contains(mediaItem)) {
        const QString filePath = m_mediaToFile.value(mediaItem);
        removeFileFromWatch(filePath, mediaItem);
        m_mediaToFile.remove(mediaItem);
        
        qDebug() << "FileWatcher: Stopped watching media item" << mediaId;
    }
}

void FileWatcher::clearAll() {
    m_watcher->removePaths(m_watcher->files());
    m_watcher->removePaths(m_watcher->directories());
    m_fileToMedia.clear();
    m_mediaToFile.clear();
    m_filesToCheck.clear();
    m_delayedCheckTimer->stop();
}

void FileWatcher::checkAllFiles() {
    QList<ResizableMediaBase*> itemsToRemove;
    
    for (auto it = m_mediaToFile.begin(); it != m_mediaToFile.end(); ++it) {
        ResizableMediaBase* mediaItem = it.key();
        const QString& filePath = it.value();
        
        if (!isFileAccessible(filePath)) {
            itemsToRemove.append(mediaItem);
            qDebug() << "FileWatcher: File no longer accessible:" << filePath;
        }
    }
    
    if (!itemsToRemove.isEmpty()) {
        emit filesDeleted(itemsToRemove);
    }
}

void FileWatcher::onFileDeleted(const QString& filePath) {
    qDebug() << "FileWatcher: File changed/deleted signal received for:" << filePath;
    
    // Add to files to check (will be processed with delay)
    m_filesToCheck.insert(filePath);
    
    // Start or restart the delayed check timer
    m_delayedCheckTimer->start();
}

void FileWatcher::onDirectoryChanged(const QString& dirPath) {
    qDebug() << "FileWatcher: Directory changed:" << dirPath;
    
    // When a directory changes, check all files in that directory that we're watching
    for (auto it = m_fileToMedia.begin(); it != m_fileToMedia.end(); ++it) {
        const QString& filePath = it.key();
        QFileInfo fileInfo(filePath);
        
        if (fileInfo.absolutePath() == dirPath) {
            m_filesToCheck.insert(filePath);
        }
    }
    
    // Start or restart the delayed check timer
    if (!m_filesToCheck.isEmpty()) {
        m_delayedCheckTimer->start();
    }
}

void FileWatcher::performDelayedCheck() {
    if (m_filesToCheck.isEmpty()) return;
    
    QList<ResizableMediaBase*> itemsToRemove;
    
    // Check each file that was flagged for checking
    for (const QString& filePath : m_filesToCheck) {
        if (!isFileAccessible(filePath)) {
            // File is no longer accessible, mark all associated media items for removal
            if (m_fileToMedia.contains(filePath)) {
                const auto& mediaItems = m_fileToMedia.value(filePath);
                for (ResizableMediaBase* mediaItem : mediaItems) {
                    // Safety check: ensure mediaItem pointer is valid and not already queued
                    if (mediaItem && !itemsToRemove.contains(mediaItem)) {
                        // Additional safety: check if mediaId is valid (non-empty and not corrupted)
                        QString mediaId = mediaItem->mediaId();
                        if (!mediaId.isEmpty() && !mediaId.contains('\0')) {
                            itemsToRemove.append(mediaItem);
                        } else {
                            qDebug() << "FileWatcher: Skipping media item with invalid mediaId:" << mediaId;
                        }
                    }
                }
            }
        }
    }
    
    // Clear the check list
    m_filesToCheck.clear();
    
    // Emit signal if we found items to remove
    if (!itemsToRemove.isEmpty()) {
        qDebug() << "FileWatcher: Emitting filesDeleted signal for" << itemsToRemove.size() << "media items";
        emit filesDeleted(itemsToRemove);
    }
}

void FileWatcher::addFileToWatch(const QString& filePath, ResizableMediaBase* mediaItem) {
    // Add media item to the file's set
    m_fileToMedia[filePath].insert(mediaItem);
    
    // Add file to watcher if not already watched
    if (!m_watcher->files().contains(filePath)) {
        if (m_watcher->addPath(filePath)) {
            qDebug() << "FileWatcher: Added file to watcher:" << filePath;
        } else {
            qDebug() << "FileWatcher: Failed to add file to watcher:" << filePath;
        }
    }
    
    // Also watch the parent directory for file deletion detection
    QFileInfo fileInfo(filePath);
    const QString dirPath = fileInfo.absolutePath();
    if (!m_watcher->directories().contains(dirPath)) {
        if (m_watcher->addPath(dirPath)) {
            qDebug() << "FileWatcher: Added directory to watcher:" << dirPath;
        } else {
            qDebug() << "FileWatcher: Failed to add directory to watcher:" << dirPath;
        }
    }
}

void FileWatcher::removeFileFromWatch(const QString& filePath, ResizableMediaBase* mediaItem) {
    if (!m_fileToMedia.contains(filePath)) return;
    
    // Remove media item from the file's set
    m_fileToMedia[filePath].remove(mediaItem);
    
    // If no more media items are using this file, remove it from watcher
    if (m_fileToMedia[filePath].isEmpty()) {
        m_fileToMedia.remove(filePath);
        m_watcher->removePath(filePath);
        qDebug() << "FileWatcher: Removed file from watcher:" << filePath;
        
        // Check if we can remove the directory too (if no other files in it are watched)
        QFileInfo fileInfo(filePath);
        const QString dirPath = fileInfo.absolutePath();
        bool stillWatchingDir = false;
        
        for (auto it = m_fileToMedia.begin(); it != m_fileToMedia.end(); ++it) {
            QFileInfo otherFileInfo(it.key());
            if (otherFileInfo.absolutePath() == dirPath) {
                stillWatchingDir = true;
                break;
            }
        }
        
        if (!stillWatchingDir) {
            m_watcher->removePath(dirPath);
            qDebug() << "FileWatcher: Removed directory from watcher:" << dirPath;
        }
    }
}

bool FileWatcher::isFileAccessible(const QString& filePath) const {
    if (filePath.isEmpty()) return false;
    
    QFileInfo fileInfo(filePath);
    return fileInfo.exists() && fileInfo.isFile() && fileInfo.isReadable();
}