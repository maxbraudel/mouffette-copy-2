#ifndef SYSTEMTRAYMANAGER_H
#define SYSTEMTRAYMANAGER_H

#include <QObject>
#include <QSystemTrayIcon>

class QIcon;

/**
 * @brief SystemTrayManager - Manages system tray icon and interactions
 * 
 * This class encapsulates system tray functionality including:
 * - Tray icon setup with fallback
 * - Click handling (show/hide window)
 * - Tooltip management
 */
class SystemTrayManager : public QObject {
    Q_OBJECT
    
public:
    explicit SystemTrayManager(QObject* parent = nullptr);
    ~SystemTrayManager() override;
    
    /**
     * @brief Setup and show the system tray icon
     * 
     * Creates tray icon with Mouffette icon (with fallback),
     * sets tooltip, and shows the icon.
     */
    void setup();
    
    /**
     * @brief Get the system tray icon
     * @return Pointer to the tray icon, or nullptr if not created
     */
    QSystemTrayIcon* getTrayIcon() const { return m_trayIcon; }
    
    /**
     * @brief Show the tray icon
     */
    void show();
    
    /**
     * @brief Hide the tray icon
     */
    void hide();
    
    /**
     * @brief Set the tray icon tooltip
     * @param tooltip Tooltip text to display
     */
    void setToolTip(const QString& tooltip);
    
signals:
    /**
     * @brief Emitted when tray icon is activated (clicked)
     * @param reason The activation reason (Trigger, DoubleClick, Context, etc.)
     */
    void activated(QSystemTrayIcon::ActivationReason reason);
    
private:
    QSystemTrayIcon* m_trayIcon = nullptr;
};

#endif // SYSTEMTRAYMANAGER_H
