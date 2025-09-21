#ifndef FILEWATCHER_H
#define FILEWATCHER_H

#include <QObject>
#include <QFileSystemWatcher>
#include <QHash>
#include <QSet>
#include <QString>
#include <QTimer>

class ResizableMediaBase;

// FileWatcher monitors source files of media items and automatically removes
// media items from the canvas when their source files are deleted or become inaccessible.
class FileWatcher : public QObject {
    Q_OBJECT
    
public:
    explicit FileWatcher(QObject* parent = nullptr);
    ~FileWatcher() override;
    
    // Add a media item to watch - will monitor its source file path
    void watchMediaItem(ResizableMediaBase* mediaItem);
    
    // Remove a media item from watching (called when item is deleted)
    void unwatchMediaItem(ResizableMediaBase* mediaItem);
    
    // Clear all watched items
    void clearAll();
    
    // Check all currently watched files and emit signals for missing ones
    void checkAllFiles();

signals:
    // Emitted when a watched file is deleted or becomes inaccessible
    // Contains the list of media items that should be removed
    void filesDeleted(const QList<ResizableMediaBase*>& mediaItems);

private slots:
    void onFileDeleted(const QString& filePath);
    void onDirectoryChanged(const QString& dirPath);
    void performDelayedCheck();

private:
    void addFileToWatch(const QString& filePath, ResizableMediaBase* mediaItem);
    void removeFileFromWatch(const QString& filePath, ResizableMediaBase* mediaItem);
    bool isFileAccessible(const QString& filePath) const;
    
    QFileSystemWatcher* m_watcher;
    
    // Map from file path to set of media items using that file
    QHash<QString, QSet<ResizableMediaBase*>> m_fileToMedia;
    
    // Map from media item to its watched file path (for reverse lookup)
    QHash<ResizableMediaBase*, QString> m_mediaToFile;
    
    // Timer to batch file deletion checks (avoid too many rapid signals)
    QTimer* m_delayedCheckTimer;
    
    // Set of files that need to be checked
    QSet<QString> m_filesToCheck;
};

#endif // FILEWATCHER_H