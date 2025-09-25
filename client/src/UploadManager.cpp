#include "UploadManager.h"
#include "WebSocketClient.h"
#include "FileManager.h"
#include <QGraphicsScene>
#include <QGraphicsItem>
#include <QFileInfo>
#include <QJsonObject>
#include <QJsonArray>
#include <QStandardPaths>
#include <QDir>
#include <QCoreApplication>
#include <QEventLoop>
#include <QTimer>
#include <QDebug>

// Removed dependency on ResizableMediaBase / scene scanning.

UploadManager::UploadManager(QObject* parent) : QObject(parent) {}

void UploadManager::setWebSocketClient(WebSocketClient* client) { m_ws = client; }
void UploadManager::setTargetClientId(const QString& id) { m_targetClientId = id; }

void UploadManager::toggleUpload(const QVector<UploadFileInfo>& files) {
    if (!m_ws || !m_ws->isConnected() || m_targetClientId.isEmpty()) {
        qWarning() << "UploadManager: Not connected or no target set";
        return;
    }
    if (m_uploadActive) {
        // If active state but we are provided with additional files, start a new upload for them
        if (!files.isEmpty()) {
            startUpload(files);
            return;
        }
        // No new files: behave as unload toggle
        requestUnload();
        return;
    }
    if (m_uploadInProgress) { // cancel
        requestCancel();
        return;
    }
    if (files.isEmpty()) {
        qInfo() << "UploadManager: No files provided";
        return;
    }
    startUpload(files);
}

void UploadManager::requestUnload() {
    if (!m_ws || !m_ws->isConnected() || (m_uploadTargetClientId.isEmpty() && m_targetClientId.isEmpty())) return;
    if (!m_uploadActive) return; // nothing to unload
    m_ws->sendRemoveAllFiles(m_uploadTargetClientId.isEmpty() ? m_targetClientId : m_uploadTargetClientId);
    // Don't reset state here - wait for onAllFilesRemovedRemote() callback
    emit uiStateChanged();
}

void UploadManager::requestCancel() {
    if (!m_ws || !m_ws->isConnected() || (m_uploadTargetClientId.isEmpty() && m_targetClientId.isEmpty())) return;
    if (!m_uploadInProgress) return;
    m_cancelRequested = true;
    if (!m_currentUploadId.isEmpty()) {
        m_ws->sendUploadAbort(m_uploadTargetClientId.isEmpty() ? m_targetClientId : m_uploadTargetClientId, m_currentUploadId, "User cancelled");
    }
    // Also request removal of all files to clean remote state
    m_ws->sendRemoveAllFiles(m_uploadTargetClientId.isEmpty() ? m_targetClientId : m_uploadTargetClientId);
    // We'll reset final state upon all_files_removed callback
    emit uiStateChanged();
    // Start fallback timer (3s) in case remote never responds
    if (!m_cancelFallbackTimer) {
        m_cancelFallbackTimer = new QTimer(this);
        m_cancelFallbackTimer->setSingleShot(true);
        connect(m_cancelFallbackTimer, &QTimer::timeout, this, [this]() {
            if (m_cancelRequested) {
                resetToInitial();
                emit uiStateChanged();
            }
        });
    }
    m_cancelFallbackTimer->start(3000);
}

void UploadManager::startUpload(const QVector<UploadFileInfo>& files) {
    if (m_ws) {
        // Open the dedicated upload channel to avoid blocking control messages
        m_ws->ensureUploadChannel();
    }
    // Capture stable target id for the entire upload session
    m_uploadTargetClientId = m_targetClientId;
    m_currentUploadId = QUuid::createUuid().toString(QUuid::WithoutBraces);
    m_uploadInProgress = true;
    m_cancelRequested = false;
    m_finalizing = false;
    m_lastPercent = 0;
    m_filesCompleted = 0;
    m_totalFiles = files.size();
    m_totalBytes = 0;
    m_sentBytes = 0;
    emit uiStateChanged();

    // Build manifest with file deduplication info
    QJsonArray manifest;
    
    for (const auto& f : files) {
        QJsonObject obj;
        obj["fileId"] = f.fileId;
        obj["name"] = f.name;
        obj["extension"] = f.extension;
        obj["sizeBytes"] = static_cast<double>(f.size);
        
        // Include all mediaIds that use this fileId
        QList<QString> mediaIds = FileManager::instance().getMediaIdsForFile(f.fileId);
        QJsonArray mediaIdArray;
        for (const QString& mediaId : mediaIds) {
            mediaIdArray.append(mediaId);
        }
        obj["mediaIds"] = mediaIdArray;
        
        
        
        manifest.append(obj);
        // accumulate for weighted progress
        if (f.size > 0) m_totalBytes += f.size;
    }
    m_ws->sendUploadStart(m_uploadTargetClientId, manifest, m_currentUploadId);

    // Stream sequentially (same logic as previously in MainWindow)
    m_outgoingFiles = files;
    const int chunkSize = 128 * 1024;
    for (const auto& f : files) {
        QFile file(f.path);
        if (!file.open(QIODevice::ReadOnly)) continue;
        emit fileUploadStarted(f.fileId);
        qint64 sentForFile = 0;
        int chunkIndex = 0;
        while (!file.atEnd()) {
            if (m_cancelRequested) break;
            QByteArray chunk = file.read(chunkSize);
            m_ws->sendUploadChunk(m_uploadTargetClientId, m_currentUploadId, f.fileId, chunkIndex++, chunk.toBase64());
            sentForFile += chunk.size();
            m_sentBytes += chunk.size();
            if (f.size > 0) {
                int p = static_cast<int>(std::round(sentForFile * 100.0 / static_cast<double>(f.size)));
                p = std::clamp(p, 0, 100);
                emit fileUploadProgress(f.fileId, p);
            }
            // Emit weighted global progress based on bytes
            if (m_totalBytes > 0) {
                int globalPercent = static_cast<int>(std::round(m_sentBytes * 100.0 / static_cast<double>(m_totalBytes)));
                globalPercent = std::clamp(globalPercent, 0, 100);
                // filesCompleted is count of files fully sent by sender (not necessarily acknowledged yet)
                int filesCompletedLocal = m_filesCompleted + (file.atEnd() ? 1 : 0);
                emit uploadProgress(globalPercent, filesCompletedLocal, m_totalFiles);
            }
            // Yield briefly
            QCoreApplication::processEvents(QEventLoop::AllEvents, 2);
        }
        file.close();
        if (!m_cancelRequested) emit fileUploadFinished(f.fileId);
        if (m_cancelRequested) break;
        // After a file is fully sent, update local filesCompleted
        if (!m_cancelRequested) {
            m_filesCompleted = std::min(m_filesCompleted + 1, m_totalFiles);
            if (m_totalBytes > 0) {
                int globalPercent = static_cast<int>(std::round(m_sentBytes * 100.0 / static_cast<double>(m_totalBytes)));
                globalPercent = std::clamp(globalPercent, 0, 100);
                emit uploadProgress(globalPercent, m_filesCompleted, m_totalFiles);
            }
        }
    }
    if (m_cancelRequested) return; // completion suppressed
    m_ws->sendUploadComplete(m_uploadTargetClientId, m_currentUploadId);
    // We have sent all bytes; mark as finalizing until server acks upload_finished
    m_uploadInProgress = false;
    m_finalizing = true;
    m_finalizingTimer.restart();
    qDebug() << "UploadManager: Entering finalizing state (waiting for upload_finished)";
    emit uiStateChanged();
    // Start a safety timeout (e.g., 5 seconds). If the target doesn't ack by then,
    // we'll assume success to avoid long "Finalizing…" stalls. This mirrors observed behavior
    // where the upload channel may disconnect slightly after completion.
    if (!m_finalizeTimeoutTimer) {
        m_finalizeTimeoutTimer = new QTimer(this);
        m_finalizeTimeoutTimer->setSingleShot(true);
        connect(m_finalizeTimeoutTimer, &QTimer::timeout, this, [this]() {
            if (m_finalizing) {
                qWarning() << "UploadManager: Finalizing timeout hit; proceeding without upload_finished ack";
                // Consider the upload finished locally
                m_uploadActive = true;
                m_uploadInProgress = false;
                m_finalizing = false;
                emit uploadFinished();
                emit uiStateChanged();
                if (m_ws) m_ws->closeUploadChannel();
            }
        });
    }
    m_finalizeTimeoutTimer->start(5000);
}

// collectSceneFiles removed; files now gathered by caller (MainWindow)

void UploadManager::resetToInitial() {
    m_uploadActive = false;
    m_uploadInProgress = false;
    m_cancelRequested = false;
    m_finalizing = false;
    m_currentUploadId.clear();
    m_lastPercent = 0;
    m_filesCompleted = 0;
    m_totalFiles = 0;
    if (m_cancelFallbackTimer) m_cancelFallbackTimer->stop();
    m_uploadTargetClientId.clear();
    // Close upload channel if open
    if (m_ws) m_ws->closeUploadChannel();
}

// Slots forwarded from WebSocketClient (sender side)
void UploadManager::onUploadProgress(const QString& uploadId, int percent, int filesCompleted, int totalFiles) {
    if (uploadId != m_currentUploadId) return;
    if (m_cancelRequested) return;
    // While we are the sender and currently streaming, prefer local weighted progress to avoid desync.
    if (m_uploadInProgress) return;
    m_lastPercent = percent;
    m_filesCompleted = filesCompleted;
    m_totalFiles = totalFiles;
    emit uploadProgress(percent, filesCompleted, totalFiles);
}

void UploadManager::onUploadFinished(const QString& uploadId) {
    if (uploadId != m_currentUploadId) return;
    if (m_cancelRequested) return;
    
    // Mark all uploaded files and media as available on the target client
    for (const auto& f : m_outgoingFiles) {
        FileManager::instance().markFileUploadedToClient(f.fileId, m_uploadTargetClientId);
        // mark all media associated to this file id
        const QList<QString> mediaIds = FileManager::instance().getMediaIdsForFile(f.fileId);
        for (const QString& mediaId : mediaIds) {
            FileManager::instance().markMediaUploadedToClient(mediaId, m_uploadTargetClientId);
        }
    }
    
    const qint64 finalizeMs = m_finalizingTimer.isValid() ? m_finalizingTimer.elapsed() : -1;
    if (finalizeMs >= 0) {
        qDebug() << "UploadManager: Finalizing complete in" << finalizeMs << "ms";
    }
    if (m_finalizeTimeoutTimer) m_finalizeTimeoutTimer->stop();
    m_uploadActive = true; // switch to active state
    m_uploadInProgress = false;
    m_finalizing = false; // finalization complete
    emit uploadFinished();
    emit uiStateChanged();
    if (m_ws) m_ws->closeUploadChannel();
}

void UploadManager::onAllFilesRemovedRemote() {
    // Remote side confirmed unload; reset state
    // Clear all uploaded markers for this client so that all items are considered Not uploaded
    if (!m_uploadTargetClientId.isEmpty()) {
        FileManager::instance().unmarkAllForClient(m_uploadTargetClientId);
    } else if (!m_targetClientId.isEmpty()) {
        FileManager::instance().unmarkAllForClient(m_targetClientId);
    }
    
    // Now reset state
    resetToInitial();
    
    emit allFilesRemoved();
    emit uiStateChanged();
}

void UploadManager::onConnectionLost() {
    // If we were uploading or finalizing, treat it as an aborted session.
    const bool hadOngoing = m_uploadInProgress || m_finalizing;
    if (!hadOngoing) return;

    // Cancel local flags immediately
    m_cancelRequested = true;
    m_uploadInProgress = false;
    m_finalizing = false;

    // Do not mark anything as uploaded; roll back any optimistic UI
    // Unmark any files that were part of the outgoing batch but not yet confirmed by onUploadFinished
    if (!m_uploadTargetClientId.isEmpty()) {
        for (const auto& f : m_outgoingFiles) {
            // Only unmark if this file hasn't been confirmed uploaded (conservative: unmark all in batch)
            FileManager::instance().unmarkFileUploadedToClient(f.fileId, m_uploadTargetClientId);
            const QList<QString> mediaIds = FileManager::instance().getMediaIdsForFile(f.fileId);
            for (const QString& mediaId : mediaIds) {
                FileManager::instance().unmarkMediaUploadedToClient(mediaId, m_uploadTargetClientId);
            }
        }
    }

    // Notify UI to recompute button state and progress text
    emit uiStateChanged();

    // Leave m_uploadActive = false so next click starts a fresh upload.
    m_uploadActive = false;
    m_currentUploadId.clear();
    m_lastPercent = 0;
    m_filesCompleted = 0;
    // Keep m_outgoingFiles; they describe the interrupted batch (optional: clear if you prefer)
}

// Incoming side (target) - replicate subset of MainWindow logic for assembling files
void UploadManager::handleIncomingMessage(const QJsonObject& message) {
    const QString type = message.value("type").toString();
    if (type == "upload_start") {
        m_incoming = IncomingUploadSession();
        m_incoming.senderId = message.value("senderClientId").toString();
        m_incoming.uploadId = message.value("uploadId").toString();
        m_canceledIncoming.remove(m_incoming.uploadId);
        QString base = QStandardPaths::writableLocation(QStandardPaths::CacheLocation);
        if (base.isEmpty()) base = QDir::homePath() + "/.cache";
        QDir dir(base); dir.mkpath("Mouffette/Uploads");
        // Use sender client ID as folder name instead of upload ID
        QString cacheDir = base + "/Mouffette/Uploads/" + m_incoming.senderId;
        QDir().mkpath(cacheDir);
        m_incoming.cacheDirPath = cacheDir;
        
        qDebug() << "UploadManager: Creating upload folder:" << cacheDir;
        qDebug() << "UploadManager: Sender ID:" << m_incoming.senderId;
        QJsonArray files = message.value("files").toArray();
        m_incoming.totalFiles = files.size();
        for (const QJsonValue& v : files) {
            QJsonObject f = v.toObject();
            QString fileId = f.value("fileId").toString();
            QString name = f.value("name").toString();
            QString extension = f.value("extension").toString();
            QJsonArray mediaIdsArray = f.value("mediaIds").toArray();
            qint64 size = static_cast<qint64>(f.value("sizeBytes").toDouble());
            m_incoming.totalSize += qMax<qint64>(0, size);
            
            // Store all mediaIds for this fileId
            QStringList mediaIdsList;
            for (const QJsonValue& mediaIdVal : mediaIdsArray) {
                QString mediaId = mediaIdVal.toString();
                mediaIdsList.append(mediaId);
                m_incoming.fileIdToMediaId.insert(fileId, mediaId);
            }
            
            // Use fileId as filename with original extension
            QString filename = fileId;
            if (!extension.isEmpty()) {
                filename += "." + extension;
            }
            QString fullPath = cacheDir + "/" + filename;
            qDebug() << "UploadManager: Creating file:" << fullPath;
            qDebug() << "UploadManager: File ID:" << fileId;
            QFile* qf = new QFile(fullPath);
            if (!qf->open(QIODevice::WriteOnly)) { delete qf; continue; }
            m_incoming.openFiles.insert(fileId, qf);
            m_incoming.expectedSizes.insert(fileId, qMax<qint64>(0, size));
            m_incoming.receivedByFile.insert(fileId, 0);
        }
        if (m_ws && !m_incoming.senderId.isEmpty()) {
            m_ws->notifyUploadProgressToSender(m_incoming.senderId, m_incoming.uploadId, 0, 0, m_incoming.totalFiles);
        }
    } else if (type == "upload_chunk") {
        if (message.value("uploadId").toString() != m_incoming.uploadId) return;
        if (m_canceledIncoming.contains(m_incoming.uploadId)) return;
        QString fid = message.value("fileId").toString();
        QFile* qf = m_incoming.openFiles.value(fid, nullptr);
        if (!qf) return;
        QByteArray data = QByteArray::fromBase64(message.value("data").toString().toUtf8());
        qf->write(data);
        m_incoming.received += data.size();
        if (m_incoming.receivedByFile.contains(fid)) {
            qint64 soFar = m_incoming.receivedByFile.value(fid) + data.size();
            m_incoming.receivedByFile[fid] = soFar;
        }
        int filesCompleted = 0;
        for (auto it = m_incoming.expectedSizes.constBegin(); it != m_incoming.expectedSizes.constEnd(); ++it) {
            qint64 expected = it.value();
            qint64 got = m_incoming.receivedByFile.value(it.key(), 0);
            if (expected > 0 && got >= expected) filesCompleted++;
        }
        if (m_ws && !m_incoming.senderId.isEmpty() && m_incoming.totalSize > 0) {
            int percent = static_cast<int>(std::round(m_incoming.received * 100.0 / m_incoming.totalSize));
            m_ws->notifyUploadProgressToSender(m_incoming.senderId, m_incoming.uploadId, percent, filesCompleted, m_incoming.totalFiles);
        }
    } else if (type == "upload_complete") {
        if (message.value("uploadId").toString() != m_incoming.uploadId) return;
        for (auto it = m_incoming.openFiles.begin(); it != m_incoming.openFiles.end(); ++it) {
            if (it.value()) { it.value()->flush(); it.value()->close(); delete it.value(); }
        }
        m_incoming.openFiles.clear();
        if (m_ws && !m_incoming.senderId.isEmpty()) {
            m_ws->notifyUploadFinishedToSender(m_incoming.senderId, m_incoming.uploadId);
        }
    } else if (type == "upload_abort") {
        const QString abortedId = message.value("uploadId").toString();
        if (!abortedId.isEmpty()) m_canceledIncoming.insert(abortedId);
        for (auto it = m_incoming.openFiles.begin(); it != m_incoming.openFiles.end(); ++it) {
            if (it.value()) { it.value()->flush(); it.value()->close(); delete it.value(); }
        }
        m_incoming.openFiles.clear();
        if (!m_incoming.cacheDirPath.isEmpty()) { QDir dir(m_incoming.cacheDirPath); dir.removeRecursively(); }
        if (m_ws && !m_incoming.senderId.isEmpty()) {
            m_ws->notifyAllFilesRemovedToSender(m_incoming.senderId);
        }
        m_incoming = IncomingUploadSession();
    } else if (type == "remove_all_files") {
        if (!m_incoming.cacheDirPath.isEmpty()) { QDir dir(m_incoming.cacheDirPath); dir.removeRecursively(); }
        if (m_ws && !m_incoming.senderId.isEmpty()) {
            m_ws->notifyAllFilesRemovedToSender(m_incoming.senderId);
        }
        m_incoming = IncomingUploadSession();
    } else if (type == "connection_lost_cleanup") {
        // Optional: sender notified us to clean any partials; delete cache folder for that sender
        QString senderClientId = message.value("senderClientId").toString();
        if (!senderClientId.isEmpty()) {
            QString base = QStandardPaths::writableLocation(QStandardPaths::CacheLocation);
            if (base.isEmpty()) base = QDir::homePath() + "/.cache";
            QString dirPath = base + "/Mouffette/Uploads/" + senderClientId;
            QDir dir(dirPath);
            if (dir.exists()) {
                dir.removeRecursively();
            }
        }
    } else if (type == "remove_file") {
        QString senderClientId = message.value("senderClientId").toString();
        QString fileId = message.value("fileId").toString();
        
        if (!senderClientId.isEmpty() && !fileId.isEmpty()) {
            // Build directory path based on sender ID
            QString base = QStandardPaths::writableLocation(QStandardPaths::CacheLocation);
            if (base.isEmpty()) base = QDir::homePath() + "/.cache";
            QString dirPath = base + "/Mouffette/Uploads/" + senderClientId;
            
            QDir dir(dirPath);
            if (dir.exists()) {
                // Find all files that start with the fileId (to handle different extensions)
                QStringList nameFilters;
                nameFilters << fileId + "*";
                QFileInfoList files = dir.entryInfoList(nameFilters, QDir::Files);
                
                for (const QFileInfo& fileInfo : files) {
                    QString filePath = fileInfo.absoluteFilePath();
                    QFile file(filePath);
                    if (file.remove()) {
                        qDebug() << "UploadManager: Successfully removed file:" << filePath;
                    } else {
                        qWarning() << "UploadManager: Failed to remove file:" << filePath;
                    }
                }
                
                // Check if directory is now empty and remove it if so
                QFileInfoList remainingFiles = dir.entryInfoList(QDir::Files);
                if (remainingFiles.isEmpty()) {
                    if (dir.rmdir(dirPath)) {
                        qDebug() << "UploadManager: Removed empty directory:" << dirPath;
                    }
                }
                
                if (files.isEmpty()) {
                    qDebug() << "UploadManager: No files found matching fileId:" << fileId;
                }
            }
        }
    }
}
