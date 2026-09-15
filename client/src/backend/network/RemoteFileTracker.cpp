#include "backend/network/RemoteFileTracker.h"
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

void RemoteFileTracker::associateFileWithProject(const QString& fileId, const QString& projectId) {
    if (fileId.isEmpty() || projectId.isEmpty()) {
        qWarning() << "RemoteFileTracker: associateFileWithProject called with empty parameter - fileId:" << fileId << "projectId:" << projectId;
        return;
    }
    
    m_fileIdToProjectIds[fileId].insert(projectId);
    m_projectIdToFileIds[projectId].insert(fileId);
    qDebug() << "RemoteFileTracker: File" << fileId << "associated with project" << projectId;
}

void RemoteFileTracker::dissociateFileFromProject(const QString& fileId, const QString& projectId) {
    if (fileId.isEmpty() || projectId.isEmpty()) {
        qWarning() << "RemoteFileTracker: dissociateFileFromProject called with empty parameter - fileId:" << fileId << "projectId:" << projectId;
        return;
    }
    
    auto fileIt = m_fileIdToProjectIds.find(fileId);
    if (fileIt != m_fileIdToProjectIds.end()) {
        fileIt.value().remove(projectId);
        if (fileIt.value().isEmpty()) {
            m_fileIdToProjectIds.erase(fileIt);
        }
    }
    
    auto projectIt = m_projectIdToFileIds.find(projectId);
    if (projectIt != m_projectIdToFileIds.end()) {
        projectIt.value().remove(fileId);
        if (projectIt.value().isEmpty()) {
            m_projectIdToFileIds.erase(projectIt);
        }
    }
    
    qDebug() << "RemoteFileTracker: File" << fileId << "dissociated from project" << projectId;
}

QSet<QString> RemoteFileTracker::getFileIdsForProject(const QString& projectId) const {
    return m_projectIdToFileIds.value(projectId);
}

QSet<QString> RemoteFileTracker::getProjectIdsForFile(const QString& fileId) const {
    return m_fileIdToProjectIds.value(fileId);
}

void RemoteFileTracker::replaceProjectFileSet(const QString& projectId, const QSet<QString>& fileIds) {
    if (projectId.isEmpty()) {
        qWarning() << "RemoteFileTracker: replaceProjectFileSet called with empty projectId";
        return;
    }
    
    // Remove old associations
    QSet<QString> oldFiles = m_projectIdToFileIds.value(projectId);
    for (const QString& oldFileId : oldFiles) {
        auto it = m_fileIdToProjectIds.find(oldFileId);
        if (it != m_fileIdToProjectIds.end()) {
            it.value().remove(projectId);
            if (it.value().isEmpty()) {
                m_fileIdToProjectIds.erase(it);
            }
        }
    }
    
    // Set new associations
    m_projectIdToFileIds[projectId] = fileIds;
    for (const QString& fileId : fileIds) {
        m_fileIdToProjectIds[fileId].insert(projectId);
    }
    
    qDebug() << "RemoteFileTracker: Replaced file set for project" << projectId << "with" << fileIds.size() << "files";
}

void RemoteFileTracker::removeProjectAssociations(const QString& projectId) {
    if (projectId.isEmpty()) {
        qWarning() << "RemoteFileTracker: removeProjectAssociations called with empty projectId";
        return;
    }
    
    const QSet<QString> files = m_projectIdToFileIds.take(projectId);
    for (const QString& fid : files) {
        QSet<QString>& projects = m_fileIdToProjectIds[fid];
        projects.remove(projectId);
        if (projects.isEmpty()) {
            m_fileIdToProjectIds.remove(fid);
        }
    }
    qDebug() << "RemoteFileTracker: Removed all associations for project" << projectId;
}

void RemoteFileTracker::removeAllTrackingForFile(const QString& fileId) {
    if (fileId.isEmpty()) return;
    
    // Remove from client tracking
    m_fileIdToClients.remove(fileId);
    
    // Remove from project tracking
    QSet<QString> projects = m_fileIdToProjectIds.take(fileId);
    for (const QString& projectId : projects) {
        auto& files = m_projectIdToFileIds[projectId];
        files.remove(fileId);
        if (files.isEmpty()) {
            m_projectIdToFileIds.remove(projectId);
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
    
    // Check whether the file is still referenced.
    bool hasClients = isFileUploadedToAnyClient(fileId);
    bool hasProjects = m_fileIdToProjectIds.contains(fileId) && !m_fileIdToProjectIds.value(fileId).isEmpty();
    
    if (!hasClients && !hasProjects) {
        // File is unused, no need to notify
        return;
    }
    
    // A reference remains; notify the cleanup policy.
    if (m_fileRemovalNotifier) {
        QList<QString> clientIds = getClientsWithFile(fileId);
        QSet<QString> projectSet = getProjectIdsForFile(fileId);
        QList<QString> projectIds = projectSet.values();
        
        if (!clientIds.isEmpty() || !projectIds.isEmpty()) {
            qDebug() << "RemoteFileTracker: Notifying removal for file" << fileId 
                     << "clients:" << clientIds << "projects:" << projectIds;
            m_fileRemovalNotifier(fileId, clientIds, projectIds);
        }
    }
    
    // Clean up local tracking
    removeAllTrackingForFile(fileId);
}

void RemoteFileTracker::clear() {
    qDebug() << "RemoteFileTracker: Clearing all tracking data";
    m_fileIdToClients.clear();
    m_fileIdToProjectIds.clear();
    m_projectIdToFileIds.clear();
}
