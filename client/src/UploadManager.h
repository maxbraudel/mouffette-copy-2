#ifndef UPLOADMANAGER_H
#define UPLOADMANAGER_H

#include <QObject>
#include <QJsonArray>
#include <QPointer>
#include <QHash>
#include <QFile>
#include <QSet>
#include <QVector>
#include <QTimer>
#include <QUuid>
#include <functional>

class WebSocketClient;
class QGraphicsScene; // no longer used directly (kept for forward compatibility)
class QGraphicsItem;  // no longer used

struct UploadFileInfo {
    QString fileId;
    QString path;
    QString name;
    qint64 size = 0;
};

struct IncomingUploadSession {
    QString senderId;
    QString uploadId;
    QString cacheDirPath;
    QHash<QString, QFile*> openFiles;          // fileId -> QFile*
    QHash<QString, qint64> expectedSizes;      // fileId -> total bytes
    QHash<QString, qint64> receivedByFile;     // fileId -> received bytes
    qint64 totalSize = 0;
    qint64 received = 0;
    int totalFiles = 0;
};

// Dedicated component that encapsulates upload/unload logic previously in MainWindow.
// Responsibilities:
//  - Build manifest from scene media items
//  - Stream chunks (sequential for now) and report progress
//  - Handle cancel/abort, unload, and incoming upload assembly
//  - Expose high level signals UI can bind to
//  - Keep WebSocket protocol usage isolated
class UploadManager : public QObject {
    Q_OBJECT
public:
    explicit UploadManager(QObject* parent = nullptr);
    void setWebSocketClient(WebSocketClient* client);
    void setTargetClientId(const QString& id);
    QString targetClientId() const { return m_targetClientId; }

    // Outbound (sender side)
    bool hasActiveUpload() const { return m_uploadActive; }
    bool isUploading() const { return m_uploadInProgress; }
    bool isCancelling() const { return m_cancelRequested; }
    QString currentUploadId() const { return m_currentUploadId; }

    // Toggle behavior (call from UI):
    //  - if active (already uploaded): unload
    //  - else if uploading: cancel
    //  - else start new upload with provided files
    void toggleUpload(const QVector<UploadFileInfo>& files);
    void requestUnload();
    void requestCancel();

    // Incoming (target side) handling entry point
    void handleIncomingMessage(const QJsonObject& message);

signals:
    void uiStateChanged(); // generic signal to refresh button text/state
    void uploadProgress(int percent, int filesCompleted, int totalFiles); // forwarded from server
    void uploadFinished();
    void unloaded();

public slots:
    // Forwarded from WebSocket layer
    void onUploadProgress(const QString& uploadId, int percent, int filesCompleted, int totalFiles);
    void onUploadFinished(const QString& uploadId);
    void onUnloadedRemote();

private:
    void startUpload(const QVector<UploadFileInfo>& files);
    void resetToInitial();

    QPointer<WebSocketClient> m_ws;
    QString m_targetClientId;

    // Sender side state
    bool m_uploadActive = false;      // true after remote finished (acts as toggle to unload)
    bool m_uploadInProgress = false;  // true while streaming chunks
    bool m_cancelRequested = false;   // user pressed cancel mid-stream
    QString m_currentUploadId;        // uuid
    int m_lastPercent = 0;
    int m_filesCompleted = 0;
    int m_totalFiles = 0;

    // Incoming session (target side)
    IncomingUploadSession m_incoming;
    QSet<QString> m_canceledIncoming; // uploadIds canceled by sender
};

#endif // UPLOADMANAGER_H
