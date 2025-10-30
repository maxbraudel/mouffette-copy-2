#pragma once

#include <QObject>
#include <QWidget>
#include <QLabel>
#include <QHBoxLayout>
#include <QFrame>
#include "backend/domain/models/ClientInfo.h"

// Forward declarations
class ClippedContainer;

/**
 * @brief Manages the remote client info container in the top bar
 * 
 * This manager handles:
 * - Creation and styling of the remote client info container
 * - Dynamic layout management (adding/removing status and volume indicators)
 * - Updating client name display
 * - Updating volume indicator
 * 
 * Phase 5: Extracted from MainWindow to reduce complexity
 */
class RemoteClientInfoManager : public QObject {
    Q_OBJECT

public:
    explicit RemoteClientInfoManager(QObject* parent = nullptr);
    ~RemoteClientInfoManager() override = default;

    /**
     * @brief Get the remote client info container widget
     * @return The container widget (may be null if not created)
     */
    QWidget* getContainer() const;

    /**
     * @brief Get the client name label
     * @return The client name label (may be null if not created)
     */
    QLabel* getClientNameLabel() const { return m_clientNameLabel; }

    /**
     * @brief Get the remote connection status label
     * @return The status label (may be null if not created)
     */
    QLabel* getRemoteConnectionStatusLabel() const { return m_remoteConnectionStatusLabel; }

    /**
     * @brief Get the volume indicator label
     * @return The volume indicator label (may be null if not created)
     */
    QLabel* getVolumeIndicator() const { return m_volumeIndicator; }

    /**
     * @brief Create the remote client info container if it doesn't exist
     * 
     * Creates the container with hostname, connection status, and volume indicator
     * using dynamic box styling with proper clipping for rounded corners.
     */
    void createContainer();

    /**
     * @brief Update the client name display
     * @param client Client information to display
     */
    void updateClientNameDisplay(const ClientInfo& client);

    /**
     * @brief Update the volume indicator with current volume
     * @param volumePercent Volume percentage (-1 for unknown/no volume)
     */
    void updateVolumeIndicator(int volumePercent);

    /**
     * @brief Add the remote status label to the layout
     * 
     * Inserts the status label (and its separator) after the hostname
     */
    void addRemoteStatusToLayout();

    /**
     * @brief Remove the remote status label from the layout
     * 
     * Removes the status label and its separator from the layout
     */
    void removeRemoteStatusFromLayout();

    /**
     * @brief Add the volume indicator to the layout
     * 
     * Inserts the volume indicator (and its separator) after the status
     */
    void addVolumeIndicatorToLayout();

    /**
     * @brief Remove the volume indicator from the layout
     * 
     * Removes the volume indicator and its separator from the layout
     */
    void removeVolumeIndicatorFromLayout();

private:
    /**
     * @brief Create the labels if they don't exist
     * 
     * Called internally by createContainer() to ensure all labels are initialized
     */
    void ensureLabelsExist();

    // UI Components
    ClippedContainer* m_remoteClientInfoContainer = nullptr;
    QLabel* m_clientNameLabel = nullptr;
    QLabel* m_remoteConnectionStatusLabel = nullptr;
    QLabel* m_volumeIndicator = nullptr;
    QFrame* m_remoteInfoSep1 = nullptr;  // Separator after hostname
    QFrame* m_remoteInfoSep2 = nullptr;  // Separator after status
};
