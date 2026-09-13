#include "frontend/rendering/navigation/ScreenNavigationManager.h"

#include "backend/domain/models/ClientInfo.h"
#include "shared/rendering/ICanvasHost.h"

ScreenNavigationManager::ScreenNavigationManager(QObject* parent)
    : QObject(parent)
{
}

void ScreenNavigationManager::setActiveCanvas(ICanvasHost* canvas)
{
    m_activeCanvas = canvas;
}

void ScreenNavigationManager::setPresentation(bool loading, bool canvasVisible)
{
    if (m_loading == loading && m_canvasVisible == canvasVisible) return;
    m_loading = loading;
    m_canvasVisible = canvasVisible;
    emit presentationChanged();
}

void ScreenNavigationManager::showScreenView(const ClientInfo& client,
                                             bool hasCachedContent)
{
    m_onScreenView = true;
    m_currentClientId = client.endpointId().isEmpty()
        ? client.getId() : client.endpointId();
    if (hasCachedContent || !client.isOnline()) {
        if (m_activeCanvas) m_activeCanvas->showContentAfterReconnect();
        setPresentation(false, true);
    } else {
        if (m_activeCanvas) m_activeCanvas->hideContentPreservingState();
        setPresentation(true, false);
    }
    emit screenViewEntered(m_currentClientId);
}

void ScreenNavigationManager::refreshActiveClientPreservingCanvas(
    const ClientInfo& client)
{
    if (!m_onScreenView) {
        showScreenView(client);
        return;
    }
    m_currentClientId = client.endpointId().isEmpty()
        ? client.getId() : client.endpointId();
    emit screenViewEntered(m_currentClientId);
}

void ScreenNavigationManager::showClientList()
{
    if (!m_onScreenView && m_currentClientId.isEmpty()) return;
    m_onScreenView = false;
    m_currentClientId.clear();
    setPresentation(false, false);
    emit clientListEntered();
}

void ScreenNavigationManager::revealCanvas()
{
    if (!m_onScreenView) return;
    if (m_activeCanvas) m_activeCanvas->showContentAfterReconnect();
    setPresentation(false, true);
}

void ScreenNavigationManager::enterLoadingStateImmediate()
{
    if (!m_onScreenView) return;
    const bool preserveContent = m_canvasVisible;
    if (!preserveContent && m_activeCanvas) {
        m_activeCanvas->hideContentPreservingState();
    }
    setPresentation(true, preserveContent);
}
