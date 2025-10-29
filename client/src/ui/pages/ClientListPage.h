/**
 * @file ClientListPage.h
 * @brief Page displaying the list of available clients and ongoing scenes
 * 
 * This page is the main entry point of the application, showing:
 * - List of connected clients available for screen sharing
 * - List of ongoing remote scenes
 * 
 * Extracted from MainWindow.cpp as part of Phase 1 refactoring.
 */

#ifndef CLIENTLISTPAGE_H
#define CLIENTLISTPAGE_H

#include <QWidget>
#include <QListWidget>
#include <QLabel>
#include <QVBoxLayout>

class ClientInfo;
class SessionManager;

/**
 * @class ClientListPage
 * @brief Widget displaying available clients and ongoing scenes
 * 
 * Responsibilities:
 * - Display list of available clients
 * - Display list of ongoing scenes
 * - Handle user clicks on clients/scenes
 * - Manage placeholder messages when lists are empty
 */
class ClientListPage : public QWidget {
    Q_OBJECT

public:
    /**
     * @brief Constructor
     * @param sessionManager Pointer to the session manager for ongoing scenes
     * @param parent Parent widget
     */
    explicit ClientListPage(SessionManager* sessionManager, QWidget* parent = nullptr);
    
    /**
     * @brief Destructor
     */
    ~ClientListPage() override = default;

    /**
     * @brief Update the list of available clients
     * @param clients List of client information
     */
    void updateClientList(const QList<ClientInfo>& clients);

    /**
     * @brief Refresh the list of ongoing scenes from session manager
     */
    void refreshOngoingScenesList();

    /**
     * @brief Show placeholder when no clients are connected
     */
    void ensureClientListPlaceholder();

    /**
     * @brief Show placeholder when no ongoing scenes exist
     */
    void ensureOngoingScenesPlaceholder();

    /**
     * @brief Enable or disable the client list widget
     * @param enabled True to enable, false to disable
     */
    void setEnabled(bool enabled);

signals:
    /**
     * @brief Emitted when a client is clicked
     * @param clientInfo Information about the clicked client
     * @param clientIndex Index in the available clients list
     */
    void clientClicked(const ClientInfo& clientInfo, int clientIndex);

    /**
     * @brief Emitted when an ongoing scene is clicked
     * @param persistentClientId The persistent client ID of the session
     */
    void ongoingSceneClicked(const QString& persistentClientId);

private slots:
    /**
     * @brief Handle click on a client item
     * @param item The clicked list widget item
     */
    void onClientItemClicked(QListWidgetItem* item);

    /**
     * @brief Handle click on an ongoing scene item
     * @param item The clicked list widget item
     */
    void onOngoingSceneItemClicked(QListWidgetItem* item);

private:
    /**
     * @brief Setup the UI layout and widgets
     */
    void setupUI();

    /**
     * @brief Apply styling to list widgets
     * @param listWidget The list widget to style
     */
    void applyListWidgetStyle(QListWidget* listWidget);

    // Session manager (not owned)
    SessionManager* m_sessionManager;

    // UI Components
    QVBoxLayout* m_layout;
    QListWidget* m_clientListWidget;
    QLabel* m_ongoingScenesLabel;
    QListWidget* m_ongoingScenesList;

    // Data
    QList<ClientInfo> m_availableClients;
};

#endif // CLIENTLISTPAGE_H
