#include "RemoteFileTracker.h"
#include <QDebug>

RemoteFileTracker& RemoteFileTracker::instance() {
    static RemoteFileTracker instance;
    return instance;
}

void RemoteFileTracker::markFileUploadedToClient(const QString& fileId, const QString& clientId) {
    if (fileId.isEmpty() || clientId.isEmpty()) {
        return;
    }
    
    m_fileIdToClients[fileId].insert(clientId);
    qDebug() << "RemoteFileTracker: File" << fileId << "marked as uploaded to client" << clientId;
}

void RemoteFileTracker::unmarkFileUploadedToClient(const QString& fileId, const QString& clientId) {
    if (fileId.isEmpty() || clientId.isEmpty()) {
        return;
    }
    
    auto it = m_fileIdToClients.find(fileId);
    if (it != m_fileIdToClients.end()) {
        it.value().remove(clientId);
        if (it.value().isEmpty()) {
            m_fileIdToClients.erase(it);
        }
        qDebug() << "RemoteFileTracker: File" << fileId << "unmarked from client" << clientId;
    }
}

QList<QString> RemoteFileTracker::getClientsWithFile(const QString& fileId) const {
    return m_fileIdToClients.value(fileId).values();
}

bool RemoteFileTracker::isFileUploadedToClient(const QString& fileId, const QString& clientId) const {
    return m_fileIdToClients.value(fileId).contains(clientId);
}

bool RemoteFileTracker::isFileUploadedToAnyClient(const QString& fileId) const {
    return m_fileIdToClients.contains(fileId) && !m_fileIdToClients.value(fileId).isEmpty();
}

void RemoteFileTracker::unmarkAllFilesForClient(const QString& clientId) {
    if (clientId.isEmpty()) return;
    
    for (auto it = m_fileIdToClients.begin(); it != m_fileIdToClients.end(); ) {
        it.value().remove(clientId);
        if (it.value().isEmpty()) {
            it = m_fileIdToClients.erase(it);
        } else {
            ++it;
        }
    }
    qDebug() << "RemoteFileTracker: Unmarked all files for client" << clientId;
}

void RemoteFileTracker::associateFileWithIdea(const QString& fileId, const QString& ideaId) {
    if (fileId.isEmpty() || ideaId.isEmpty()) {
        return;
    }
    
    m_fileIdToIdeaIds[fileId].insert(ideaId);
    m_ideaIdToFileIds[ideaId].insert(fileId);
    qDebug() << "RemoteFileTracker: File" << fileId << "associated with idea" << ideaId;
}

void RemoteFileTracker::dissociateFileFromIdea(const QString& fileId, const QString& ideaId) {
    if (fileId.isEmpty() || ideaId.isEmpty()) {
        return;
    }
    
    auto fileIt = m_fileIdToIdeaIds.find(fileId);
    if (fileIt != m_fileIdToIdeaIds.end()) {
        fileIt.value().remove(ideaId);
        if (fileIt.value().isEmpty()) {
            m_fileIdToIdeaIds.erase(fileIt);
        }
    }
    
    auto ideaIt = m_ideaIdToFileIds.find(ideaId);
    if (ideaIt != m_ideaIdToFileIds.end()) {
        ideaIt.value().remove(fileId);
        if (ideaIt.value().isEmpty()) {
            m_ideaIdToFileIds.erase(ideaIt);
        }
    }
    
    qDebug() << "RemoteFileTracker: File" << fileId << "dissociated from idea" << ideaId;
}

QSet<QString> RemoteFileTracker::getFileIdsForIdea(const QString& ideaId) const {
    return m_ideaIdToFileIds.value(ideaId);
}

QSet<QString> RemoteFileTracker::getIdeaIdsForFile(const QString& fileId) const {
    return m_fileIdToIdeaIds.value(fileId);
}

void RemoteFileTracker::replaceIdeaFileSet(const QString& ideaId, const QSet<QString>& fileIds) {
    if (ideaId.isEmpty()) {
        return;
    }
    
    // Remove old associations
    QSet<QString> oldFiles = m_ideaIdToFileIds.value(ideaId);
    for (const QString& oldFileId : oldFiles) {
        auto it = m_fileIdToIdeaIds.find(oldFileId);
        if (it != m_fileIdToIdeaIds.end()) {
            it.value().remove(ideaId);
            if (it.value().isEmpty()) {
                m_fileIdToIdeaIds.erase(it);
            }
        }
    }
    
    // Set new associations
    m_ideaIdToFileIds[ideaId] = fileIds;
    for (const QString& fileId : fileIds) {
        m_fileIdToIdeaIds[fileId].insert(ideaId);
    }
    
    qDebug() << "RemoteFileTracker: Replaced file set for idea" << ideaId << "with" << fileIds.size() << "files";
}

void RemoteFileTracker::removeIdeaAssociations(const QString& ideaId) {
    if (ideaId.isEmpty()) return;
    
    const QSet<QString> files = m_ideaIdToFileIds.take(ideaId);
    for (const QString& fid : files) {
        QSet<QString>& ideas = m_fileIdToIdeaIds[fid];
        ideas.remove(ideaId);
        if (ideas.isEmpty()) {
            m_fileIdToIdeaIds.remove(fid);
        }
    }
    qDebug() << "RemoteFileTracker: Removed all associations for idea" << ideaId;
}

void RemoteFileTracker::removeAllTrackingForFile(const QString& fileId) {
    if (fileId.isEmpty()) return;
    
    // Remove from client tracking
    m_fileIdToClients.remove(fileId);
    
    // Remove from idea tracking
    QSet<QString> ideas = m_fileIdToIdeaIds.take(fileId);
    for (const QString& ideaId : ideas) {
        auto& files = m_ideaIdToFileIds[ideaId];
        files.remove(fileId);
        if (files.isEmpty()) {
            m_ideaIdToFileIds.remove(ideaId);
        }
    }
    
    qDebug() << "RemoteFileTracker: Removed all tracking for file" << fileId;
}

void RemoteFileTracker::setFileRemovalNotifier(FileRemovalNotifier callback) {
    m_fileRemovalNotifier = std::move(callback);
}

void RemoteFileTracker::checkAndNotifyIfUnused(const QString& fileId) {
    if (fileId.isEmpty()) {
        return;
    }
    
    // Check if file is still in use
    bool hasClients = isFileUploadedToAnyClient(fileId);
    bool hasIdeas = m_fileIdToIdeaIds.contains(fileId) && !m_fileIdToIdeaIds.value(fileId).isEmpty();
    
    if (!hasClients && !hasIdeas) {
        // File is unused, no need to notify
        return;
    }
    
    // File is in use somewhere, notify for potential removal
    if (m_fileRemovalNotifier) {
        QList<QString> clientIds = getClientsWithFile(fileId);
        QSet<QString> ideaSet = getIdeaIdsForFile(fileId);
        QList<QString> ideaIds = ideaSet.values();
        
        if (!clientIds.isEmpty() || !ideaIds.isEmpty()) {
            qDebug() << "RemoteFileTracker: Notifying removal for file" << fileId 
                     << "clients:" << clientIds << "ideas:" << ideaIds;
            m_fileRemovalNotifier(fileId, clientIds, ideaIds);
        }
    }
    
    // Clean up local tracking
    removeAllTrackingForFile(fileId);
}

void RemoteFileTracker::clear() {
    qDebug() << "RemoteFileTracker: Clearing all tracking data";
    m_fileIdToClients.clear();
    m_fileIdToIdeaIds.clear();
    m_ideaIdToFileIds.clear();
}
