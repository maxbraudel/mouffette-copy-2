#ifndef TOPBARMANAGER_H
#define TOPBARMANAGER_H

#include <QObject>
#include <QString>

class QWidget;
class QLabel;
class QHBoxLayout;
class ClippedContainer;

/**
 * @brief TopBarManager - Manages the top bar UI components
 * 
 * This class encapsulates the creation and management of the top bar,
 * including:
 * - Local client info container ("You" + network status)
 * - Remote client info container (managed by RemoteClientInfoManager)
 * - Layout coordination
 * - Network status updates with color-coded styling
 */
class TopBarManager : public QObject {
    Q_OBJECT
    
public:
    explicit TopBarManager(QObject* parent = nullptr);
    ~TopBarManager() override;
    
    /**
     * @brief Create the local client info container
     * 
     * Creates a container with "You" label and network status indicator.
     * Uses the same styling as remote client container for visual consistency.
     */
    void createLocalClientInfoContainer();
    
    /**
     * @brief Get the local client info container widget
     * @return Pointer to the container widget, or nullptr if not created
     */
    QWidget* getLocalClientInfoContainer() const;
    
    /**
     * @brief Get the local network status label
     * @return Pointer to the status label, or nullptr if not created
     */
    QLabel* getLocalNetworkStatusLabel() const;
    
    /**
     * @brief Update the local network status display
     * @param status Status string (CONNECTED, DISCONNECTED, CONNECTING, etc.)
     * 
     * Applies color-coded styling:
     * - Green for CONNECTED
     * - Yellow for CONNECTING/RECONNECTING/ERROR
     * - Red for DISCONNECTED
     */
    void setLocalNetworkStatus(const QString& status);
    
private:
    ClippedContainer* m_localClientInfoContainer = nullptr;
    QLabel* m_localClientTitleLabel = nullptr;  // "You" label
    QLabel* m_localNetworkStatusLabel = nullptr; // Network status
};

#endif // TOPBARMANAGER_H
