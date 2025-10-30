#include "frontend/ui/layout/ResponsiveLayoutManager.h"
#include "MainWindow.h"
#include "frontend/ui/pages/ClientListPage.h"
#include "frontend/ui/pages/CanvasViewPage.h"  // Phase 1.2: Need complete type for conversion
#include <QLayoutItem>

ResponsiveLayoutManager::ResponsiveLayoutManager(MainWindow* parent)
    : QObject(parent), m_mainWindow(parent)
{
}

ResponsiveLayoutManager::~ResponsiveLayoutManager()
{
    // Cleanup is handled by Qt's parent-child system
}

void ResponsiveLayoutManager::initialize()
{
    // Initial state setup - called after UI is created
    m_clientInfoInTopBar = true;
    m_buttonsVisible = true;
}

void ResponsiveLayoutManager::setClientInfoThreshold(int width)
{
    m_clientInfoThreshold = width;
}

void ResponsiveLayoutManager::setButtonVisibilityThreshold(int width)
{
    m_buttonVisibilityThreshold = width;
}

void ResponsiveLayoutManager::updateResponsiveLayout()
{
    if (!m_mainWindow) return;
    
    const int windowWidth = m_mainWindow->width();
    
    // Handle client info container positioning (only if container exists)
    QWidget* remoteClientInfoContainer = getRemoteClientInfoContainer();
    if (remoteClientInfoContainer) {
        const bool shouldBeInTopBar = windowWidth >= m_clientInfoThreshold;
        
        // Only move if current position differs from desired position
        if (shouldBeInTopBar != m_clientInfoInTopBar) {
            if (shouldBeInTopBar) {
                moveClientInfoToTopBar();
            } else {
                moveClientInfoBelowTopBar();
            }
        }
    }
    
    // Always update button visibility based on window width and current page
    updateResponsiveButtonVisibility();
}

void ResponsiveLayoutManager::updateResponsiveButtonVisibility()
{
    if (!m_mainWindow) return;
    
    const int windowWidth = m_mainWindow->width();
    const bool shouldButtonsBeVisible = windowWidth >= m_buttonVisibilityThreshold;
    
    // Only apply responsive behavior when we're on the screen view page (remote client canvas)
    if (!isOnScreenView()) {
        // On client list page, always show buttons regardless of window size
        if (!m_buttonsVisible) {
            updateButtonVisibility(true);
            m_buttonsVisible = true;
        }
        return;
    }
    
    // Only change visibility if it differs from desired state
    if (shouldButtonsBeVisible == m_buttonsVisible) {
        return;
    }
    
    // Update button visibility based on window width (only on screen view page)
    updateButtonVisibility(shouldButtonsBeVisible);
    m_buttonsVisible = shouldButtonsBeVisible;
}

void ResponsiveLayoutManager::moveClientInfoToTopBar()
{
    QWidget* remoteClientInfoContainer = getRemoteClientInfoContainer();
    QHBoxLayout* connectionLayout = getConnectionLayout();
    QPushButton* backButton = getBackButton();
    
    if (!remoteClientInfoContainer || !connectionLayout || !backButton) return;
    
    // Remove container from current parent
    remoteClientInfoContainer->setParent(nullptr);
    
    // Move to top bar (m_connectionLayout)
    int backButtonIndex = -1;
    for (int i = 0; i < connectionLayout->count(); ++i) {
        QLayoutItem* item = connectionLayout->itemAt(i);
        if (item && item->widget() == backButton) {
            backButtonIndex = i;
            break;
        }
    }
    
    if (backButtonIndex >= 0) {
        connectionLayout->insertWidget(backButtonIndex + 1, remoteClientInfoContainer);
    }
    m_clientInfoInTopBar = true;
    
    // Remove responsive container and its spacing if they exist
    if (m_responsiveClientInfoContainer) {
        m_responsiveClientInfoContainer->deleteLater();
        m_responsiveClientInfoContainer = nullptr;
    }
    if (m_responsiveClientInfoSpacing) {
        QVBoxLayout* mainLayout = getMainLayout();
        if (mainLayout) {
            mainLayout->removeItem(m_responsiveClientInfoSpacing);
            delete m_responsiveClientInfoSpacing;
            m_responsiveClientInfoSpacing = nullptr;
        }
    }
}

void ResponsiveLayoutManager::moveClientInfoBelowTopBar()
{
    QWidget* remoteClientInfoContainer = getRemoteClientInfoContainer();
    QVBoxLayout* mainLayout = getMainLayout();
    
    if (!remoteClientInfoContainer || !mainLayout) return;
    
    // Remove container from current parent
    remoteClientInfoContainer->setParent(nullptr);
    
    // Move below top bar
    if (!m_responsiveClientInfoContainer) {
        // Create container positioned below top section
        m_responsiveClientInfoContainer = new QWidget();
        QHBoxLayout* responsiveLayout = new QHBoxLayout(m_responsiveClientInfoContainer);
        responsiveLayout->setContentsMargins(0, 0, 0, 0);
        responsiveLayout->setSpacing(0);
        
        // Add to main layout after top section but before bottom section
        // Find the position after top section (position 2: topSection is at 1, spacing at 2)
        mainLayout->insertWidget(2, m_responsiveClientInfoContainer);
        m_responsiveClientInfoSpacing = new QSpacerItem(0, getInnerContentGap(), QSizePolicy::Minimum, QSizePolicy::Fixed);
        mainLayout->insertSpacerItem(3, m_responsiveClientInfoSpacing);
    }
    
    // Move container to responsive layout (anchored left)
    QHBoxLayout* responsiveLayout = qobject_cast<QHBoxLayout*>(m_responsiveClientInfoContainer->layout());
    if (responsiveLayout) {
        responsiveLayout->addWidget(remoteClientInfoContainer);
        responsiveLayout->addStretch(); // Push container to the left
    }
    
    m_clientInfoInTopBar = false;
}

void ResponsiveLayoutManager::updateButtonVisibility(bool visible)
{
    QWidget* localClientInfoContainer = getLocalClientInfoContainer();
    QPushButton* connectToggleButton = getConnectToggleButton();
    QPushButton* settingsButton = getSettingsButton();
    
    if (localClientInfoContainer) localClientInfoContainer->setVisible(visible);
    if (connectToggleButton) connectToggleButton->setVisible(visible);
    if (settingsButton) settingsButton->setVisible(visible);
}

bool ResponsiveLayoutManager::isOnScreenView() const
{
    QStackedWidget* stackedWidget = getStackedWidget();
    QWidget* screenViewWidget = getScreenViewWidget();
    
    return (stackedWidget && screenViewWidget && stackedWidget->currentWidget() == screenViewWidget);
}

// Widget accessor methods (through MainWindow)
QWidget* ResponsiveLayoutManager::getRemoteClientInfoContainer() const
{
    return m_mainWindow ? m_mainWindow->getRemoteClientInfoContainer() : nullptr;
}

QHBoxLayout* ResponsiveLayoutManager::getConnectionLayout() const
{
    return m_mainWindow ? m_mainWindow->getConnectionLayout() : nullptr;
}

QVBoxLayout* ResponsiveLayoutManager::getMainLayout() const
{
    return m_mainWindow ? m_mainWindow->getMainLayout() : nullptr;
}

QStackedWidget* ResponsiveLayoutManager::getStackedWidget() const
{
    return m_mainWindow ? m_mainWindow->getStackedWidget() : nullptr;
}

QWidget* ResponsiveLayoutManager::getScreenViewWidget() const
{
    // Phase 1.2: getScreenViewWidget() now returns CanvasViewPage
    return m_mainWindow ? m_mainWindow->getCanvasViewPage() : nullptr;
}

QWidget* ResponsiveLayoutManager::getClientListPage() const
{
    return m_mainWindow ? m_mainWindow->getClientListPage() : nullptr;
}

QPushButton* ResponsiveLayoutManager::getBackButton() const
{
    return m_mainWindow ? m_mainWindow->getBackButton() : nullptr;
}

QLabel* ResponsiveLayoutManager::getConnectionStatusLabel() const
{
    return m_mainWindow ? m_mainWindow->getConnectionStatusLabel() : nullptr;
}

QWidget* ResponsiveLayoutManager::getLocalClientInfoContainer() const
{
    return m_mainWindow ? m_mainWindow->getLocalClientInfoContainer() : nullptr;
}

QPushButton* ResponsiveLayoutManager::getConnectToggleButton() const
{
    return m_mainWindow ? m_mainWindow->getConnectToggleButton() : nullptr;
}

QPushButton* ResponsiveLayoutManager::getSettingsButton() const
{
    return m_mainWindow ? m_mainWindow->getSettingsButton() : nullptr;
}

int ResponsiveLayoutManager::getInnerContentGap() const
{
    return m_mainWindow->getInnerContentGap();
}