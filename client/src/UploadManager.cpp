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

void UploadManager::forceResetForClient(const QString& clientId) {
    if (!clientId.isEmpty()) {
        const bool matchesUploadTarget = (!m_uploadTargetClientId.isEmpty() && m_uploadTargetClientId == clientId);
        const bool matchesCurrentTarget = (!m_targetClientId.isEmpty() && m_targetClientId == clientId);
        if (!matchesUploadTarget && !matchesCurrentTarget && !m_uploadActive && !m_uploadInProgress && !m_finalizing) {
            return;
        }
    }

    resetToInitial();
    emit uiStateChanged();
}

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

void UploadManager::requestRemoval(const QString& clientId) {
    if (!m_ws || !m_ws->isConnected() || clientId.isEmpty()) return;
    // Ensure subsequent all_files_removed callbacks attribute to the correct target
    m_uploadTargetClientId = clientId;
    m_lastRemovalClientId = clientId;
    m_ws->sendRemoveAllFiles(clientId);
}

void UploadManager::requestUnload() {
    const QString clientId = m_uploadTargetClientId.isEmpty() ? m_targetClientId : m_uploadTargetClientId;
    if (!m_uploadActive || clientId.isEmpty()) return;
    requestRemoval(clientId);
    // Don't reset state here - wait for onAllFilesRemovedRemote() callback
    emit uiStateChanged();
}

void UploadManager::requestCancel() {
    const QString clientId = m_uploadTargetClientId.isEmpty() ? m_targetClientId : m_uploadTargetClientId;
    if (!m_ws || !m_ws->isConnected() || clientId.isEmpty()) return;
    if (!m_uploadInProgress) return;
    m_cancelRequested = true;
    if (!m_currentUploadId.isEmpty()) {
        m_ws->sendUploadAbort(clientId, m_currentUploadId, "User cancelled");
    }
    // Also request removal of all files to clean remote state
    requestRemoval(clientId);
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
    m_remoteProgressReceived = false;
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
            // Emit weighted global progress based on bytes, but do not exceed 99%
            if (!m_remoteProgressReceived && m_totalBytes > 0) {
                int globalPercent = static_cast<int>(std::round(m_sentBytes * 100.0 / static_cast<double>(m_totalBytes)));
                globalPercent = std::clamp(globalPercent, 0, 99); // keep <100 until remote confirms
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
        if (!m_cancelRequested && !m_remoteProgressReceived) {
            m_filesCompleted = std::min(m_filesCompleted + 1, m_totalFiles);
            if (m_totalBytes > 0) {
                int globalPercent = static_cast<int>(std::round(m_sentBytes * 100.0 / static_cast<double>(m_totalBytes)));
                globalPercent = std::clamp(globalPercent, 0, 99);
                emit uploadProgress(globalPercent, m_filesCompleted, m_totalFiles);
            }
        }
    }
    if (m_cancelRequested) return; // completion suppressed
    m_ws->sendUploadComplete(m_uploadTargetClientId, m_currentUploadId);
    // We have sent all bytes; remain in uploading state until remote finishes
    // Enter finalizing only when we stop sending and await remote ack
    m_uploadInProgress = true;
    m_finalizing = false;
    emit uiStateChanged();
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
    m_sentBytes = 0;
    m_totalBytes = 0;
    m_remoteProgressReceived = false;
    m_outgoingFiles.clear();
    if (m_cancelFallbackTimer) m_cancelFallbackTimer->stop();
    m_uploadTargetClientId.clear();
    m_activeSessionIdentity.clear();
    // Close upload channel if open
    if (m_ws) m_ws->closeUploadChannel();
}

// Slots forwarded from WebSocketClient (sender side)
void UploadManager::onUploadProgress(const QString& uploadId, int percent, int filesCompleted, int totalFiles) {
    if (uploadId != m_currentUploadId) return;
    if (m_cancelRequested) return;
    // Always accept target-side progress; it's authoritative
    m_remoteProgressReceived = true;
    m_lastPercent = percent;
    m_filesCompleted = filesCompleted;
    m_totalFiles = totalFiles;
    emit uploadProgress(percent, filesCompleted, totalFiles);
}

void UploadManager::onUploadCompletedFileIds(const QString& uploadId, const QStringList& fileIds) {
    if (uploadId != m_currentUploadId) return;
    if (m_cancelRequested) return;
    if (fileIds.isEmpty()) return;
    emit uploadCompletedFileIds(fileIds);
}

void UploadManager::onUploadFinished(const QString& uploadId) {
    if (uploadId != m_currentUploadId) return;
    if (m_cancelRequested) return;
    // Switch to finalizing for a brief moment to align UI state, then finish
    m_uploadInProgress = false;
    m_finalizing = true;
    emit uiStateChanged();
    
    // Mark all uploaded files and media as available on the target client
    for (const auto& f : m_outgoingFiles) {
        FileManager::instance().markFileUploadedToClient(f.fileId, m_uploadTargetClientId);
        // mark all media associated to this file id
        const QList<QString> mediaIds = FileManager::instance().getMediaIdsForFile(f.fileId);
        for (const QString& mediaId : mediaIds) {
            FileManager::instance().markMediaUploadedToClient(mediaId, m_uploadTargetClientId);
        }
    }
    
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
    const QString removedClientId = !m_uploadTargetClientId.isEmpty() ? m_uploadTargetClientId : m_targetClientId;
    if (!removedClientId.isEmpty()) {
        FileManager::instance().unmarkAllForClient(removedClientId);
    }

    // Now reset state
    resetToInitial();

    m_lastRemovalClientId = removedClientId;

    emit allFilesRemoved();
    emit uiStateChanged();
}

void UploadManager::onConnectionLost() {
    // If we were uploading or finalizing, treat it as an aborted session.
    const bool hadOngoing = m_uploadInProgress || m_finalizing;

    if (hadOngoing) {
        // Cancel local flags immediately
        m_cancelRequested = true;
        m_uploadInProgress = false;
        m_finalizing = false;

        // Do not mark anything as uploaded; roll back any optimistic UI
        // Unmark any files that were part of the outgoing batch but not yet confirmed by onUploadFinished
        if (!m_uploadTargetClientId.isEmpty()) {
            for (const auto& f : m_outgoingFiles) {
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
        m_totalFiles = 0;
        m_sentBytes = 0;
        m_totalBytes = 0;
        m_remoteProgressReceived = false;
        m_outgoingFiles.clear();
    }

    cleanupIncomingCacheForConnectionLoss();
}

void UploadManager::cleanupIncomingCacheForConnectionLoss() {
    // Reset any active incoming session and associated bookkeeping, then remove cached files on disk.
    if (!m_incoming.senderId.isEmpty()) {
        qDebug() << "UploadManager: Clearing incoming upload cache for sender" << m_incoming.senderId << "after connection loss";
    }
    m_incoming = IncomingUploadSession();
    m_expectedChunkIndex.clear();
    m_canceledIncoming.clear();

    QString base = QStandardPaths::writableLocation(QStandardPaths::CacheLocation);
    if (base.isEmpty()) base = QDir::homePath() + "/.cache";
    const QString uploadsRoot = base + "/Mouffette/Uploads";
    QDir uploadsDir(uploadsRoot);
    if (!uploadsDir.exists()) {
        return;
    }

    if (!uploadsDir.removeRecursively()) {
        qWarning() << "UploadManager: Failed to remove uploads cache folder after connection loss:" << uploadsRoot;
    } else {
        qDebug() << "UploadManager: Cleared uploads cache folder after connection loss:" << uploadsRoot;
    }

    FileManager::instance().removeReceivedFileMappingsUnderPathPrefix(uploadsRoot + "/");
}

// Incoming side (target) - replicate subset of MainWindow logic for assembling files
void UploadManager::handleIncomingMessage(const QJsonObject& message) {
    const QString type = message.value("type").toString();
    if (type == "upload_start") {
        m_incoming = IncomingUploadSession();
        // Reset per-session chunk ordering state
        m_expectedChunkIndex.clear();
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
            // Register mapping so remote scene resolution can find this fileId immediately (even before complete)
            FileManager::instance().registerReceivedFilePath(fileId, fullPath);
            // Initialize expected chunk index for this file to 0
            m_expectedChunkIndex.insert(m_incoming.uploadId + ":" + fileId, 0);
        }
        if (m_ws && !m_incoming.senderId.isEmpty()) {
            m_ws->notifyUploadProgressToSender(m_incoming.senderId, m_incoming.uploadId, 0, 0, m_incoming.totalFiles, QStringList());
        }
    } else if (type == "upload_chunk") {
        if (message.value("uploadId").toString() != m_incoming.uploadId) return;
        if (m_canceledIncoming.contains(m_incoming.uploadId)) return;
        QString fid = message.value("fileId").toString();
        QFile* qf = m_incoming.openFiles.value(fid, nullptr);
        if (!qf) return;
        
        // CRITICAL FIX: Handle chunk ordering properly per upload session
        int chunkIndex = message.value("chunkIndex").toInt();
        QByteArray data = QByteArray::fromBase64(message.value("data").toString().toUtf8());

        const QString key = m_incoming.uploadId + ":" + fid;
        if (!m_expectedChunkIndex.contains(key)) {
            m_expectedChunkIndex.insert(key, 0);
        }
        const int expected = m_expectedChunkIndex.value(key);
        if (chunkIndex != expected) {
            qWarning() << "UploadManager: Out-of-order chunk for" << fid
                       << "(upload" << m_incoming.uploadId << ") - expected" << expected
                       << "got" << chunkIndex << "- dropping to prevent corruption";
            return;
        }
        m_expectedChunkIndex[key] = expected + 1;

        // Ensure we are at end for append (defensive in case of any reuse)
        qf->seek(qf->size());
        qint64 written = qf->write(data);
        if (written != data.size()) {
            qWarning() << "UploadManager: Partial write - expected" << data.size() << "wrote" << written;
        }
        qf->flush(); // Ensure data is written immediately
        
        m_incoming.received += data.size();
        if (m_incoming.receivedByFile.contains(fid)) {
            qint64 soFar = m_incoming.receivedByFile.value(fid) + data.size();
            m_incoming.receivedByFile[fid] = soFar;
        }
        int filesCompleted = 0;
        QStringList completedIds;
        for (auto it = m_incoming.expectedSizes.constBegin(); it != m_incoming.expectedSizes.constEnd(); ++it) {
            qint64 expected = it.value();
            qint64 got = m_incoming.receivedByFile.value(it.key(), 0);
            if (expected > 0 && got >= expected) { filesCompleted++; completedIds.append(it.key()); }
        }
        if (m_ws && !m_incoming.senderId.isEmpty() && m_incoming.totalSize > 0) {
            int percent = static_cast<int>(std::round(m_incoming.received * 100.0 / m_incoming.totalSize));
            // Build per-file progress array (only for files in progress to reduce payload)
            QJsonArray perFileArr;
            // Include the file that just received data
            if (m_incoming.expectedSizes.contains(fid)) {
                const qint64 expected = m_incoming.expectedSizes.value(fid);
                const qint64 got = m_incoming.receivedByFile.value(fid, 0);
                int pf = 0;
                if (expected > 0) pf = static_cast<int>(std::round(got * 100.0 / expected));
                QJsonObject o; o["fileId"] = fid; o["percent"] = pf; perFileArr.append(o);
            }
            m_ws->notifyUploadProgressToSender(m_incoming.senderId, m_incoming.uploadId, percent, filesCompleted, m_incoming.totalFiles, completedIds, perFileArr);
        }
    } else if (type == "upload_complete") {
        if (message.value("uploadId").toString() != m_incoming.uploadId) return;
        
        // Clean up chunk tracking before closing files (for this upload)
        const QString prefix = m_incoming.uploadId + ":";
        auto itKey = m_expectedChunkIndex.begin();
        while (itKey != m_expectedChunkIndex.end()) {
            if (itKey.key().startsWith(prefix)) itKey = m_expectedChunkIndex.erase(itKey); else ++itKey;
        }
        
        for (auto it = m_incoming.openFiles.begin(); it != m_incoming.openFiles.end(); ++it) {
            if (it.value()) { it.value()->flush(); it.value()->close(); delete it.value(); }
        }
        m_incoming.openFiles.clear();
        // Send a final 100% progress update to the sender to ensure UI reaches 100 only when target is fully done
        if (m_ws && !m_incoming.senderId.isEmpty()) {
            const int finalPercent = 100;
            const int filesCompleted = m_incoming.totalFiles;
            // Include all fileIds as completed and per-file 100
            QStringList allIds = m_incoming.expectedSizes.keys();
            QJsonArray perFileArr;
            for (const QString& fidAll : allIds) { QJsonObject o; o["fileId"] = fidAll; o["percent"] = 100; perFileArr.append(o); }
            m_ws->notifyUploadProgressToSender(m_incoming.senderId, m_incoming.uploadId, finalPercent, filesCompleted, m_incoming.totalFiles, allIds, perFileArr);
        }
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
        
        // Clean up chunk tracking for aborted upload
        const QString prefix2 = abortedId + ":";
        auto itKey2 = m_expectedChunkIndex.begin();
        while (itKey2 != m_expectedChunkIndex.end()) {
            if (itKey2.key().startsWith(prefix2)) itKey2 = m_expectedChunkIndex.erase(itKey2); else ++itKey2;
        }
        
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
        // Clear all expected indices; treat as a hard reset
        m_expectedChunkIndex.clear();
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

                // Clear local mapping for this fileId so future re-uploads can re-register a fresh path
                FileManager::instance().removeReceivedFileMapping(fileId);
                
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
