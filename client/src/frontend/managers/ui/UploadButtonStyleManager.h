#ifndef UPLOADBUTTONSTYLEMANAGER_H
#define UPLOADBUTTONSTYLEMANAGER_H

#include <QObject>
#include <QPushButton>
#include <QFont>

class MainWindow;
class UploadManager;
class SessionManager;

/**
 * [PHASE 11] UploadButtonStyleManager
 * Manages all styling logic for upload buttons, including:
 * - Overlay button styles (canvas overlay)
 * - Regular button styles (sidebar)
 * - State-based styling (uploading, finalizing, idle, unload)
 * - Dynamic updates based on upload progress
 */
class UploadButtonStyleManager : public QObject {
    Q_OBJECT

public:
    explicit UploadButtonStyleManager(MainWindow* mainWindow, QObject* parent = nullptr);
    ~UploadButtonStyleManager() = default;

    // Main style application method
    void applyUploadButtonStyle(QPushButton* uploadButton);
    
    // Update button text during upload progress
    void updateUploadButtonProgress(QPushButton* uploadButton, int percent, int filesCompleted, int totalFiles);

private:
    MainWindow* m_mainWindow;
    
    // Style generation helpers
    QString generateOverlayIdleStyle() const;
    QString generateOverlayUploadingStyle() const;
    QString generateOverlayUnloadStyle() const;
    QString generateOverlayDisabledStyle() const;
    
    QString generateRegularGreyStyle() const;
    QString generateRegularBlueStyle() const;
    QString generateRegularGreenStyle() const;
    
    // State detection helpers
    bool isRemoteSceneLaunchedForButton(QPushButton* button) const;
    bool hasUnuploadedFilesForTarget(const QString& targetClientId) const;
    
    // Font helpers
    QFont getMonospaceFont() const;
    QFont getDefaultButtonFont() const;
};

#endif // UPLOADBUTTONSTYLEMANAGER_H
