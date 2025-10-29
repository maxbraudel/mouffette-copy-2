#ifndef UPLOADEVENTHANDLER_H
#define UPLOADEVENTHANDLER_H

#include <QObject>

class MainWindow;

/**
 * @brief Handler for upload-related events and logic
 * 
 * Responsibilities:
 * - Handle upload button clicks
 * - Manage upload file collection from canvas
 * - Coordinate upload progress tracking
 * - Handle upload state transitions
 * - Manage upload completion and removal
 */
class UploadEventHandler : public QObject
{
    Q_OBJECT

public:
    explicit UploadEventHandler(MainWindow* mainWindow, QObject* parent = nullptr);

public slots:
    /**
     * @brief Handles the upload button click event
     * 
     * Collects media files from canvas, manages upload state,
     * and coordinates with UploadManager for file transfer
     */
    void onUploadButtonClicked();

private:
    MainWindow* m_mainWindow;
};

#endif // UPLOADEVENTHANDLER_H
