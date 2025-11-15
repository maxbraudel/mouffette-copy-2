#ifndef MENUBARMANAGER_H
#define MENUBARMANAGER_H

#include <QObject>
#include <QMenu>
#include <QAction>
#include <QActionGroup>

class QMenuBar;
class QMainWindow;

/**
 * @brief MenuBarManager handles the creation and management of the application's menu bar
 * 
 * Responsibilities:
 * - Create and configure File menu with Quit action
 * - Create and configure Help menu with About action
 * - Manage menu bar lifecycle
 * 
 * This manager centralizes all menu bar setup logic that was previously in MainWindow::setupMenuBar()
 */
class MenuBarManager : public QObject
{
    Q_OBJECT

public:
    explicit MenuBarManager(QMainWindow* mainWindow, QObject* parent = nullptr);
    ~MenuBarManager() override = default;

    /**
     * @brief Setup the complete menu bar with all menus and actions
     * Creates File and Help menus with their respective actions
     */
    void setup();

    /**
     * @brief Get the File menu
     * @return Pointer to the File menu
     */
    QMenu* getFileMenu() const { return m_fileMenu; }

    /**
     * @brief Get the Help menu
     * @return Pointer to the Help menu
     */
    QMenu* getHelpMenu() const { return m_helpMenu; }
    
    /**
     * @brief Get the View menu
     * @return Pointer to the View menu
     */
    QMenu* getViewMenu() const { return m_viewMenu; }

    /**
     * @brief Get the Exit action
     * @return Pointer to the Exit action
     */
    QAction* getExitAction() const { return m_exitAction; }

    /**
     * @brief Get the About action
     * @return Pointer to the About action
     */
    QAction* getAboutAction() const { return m_aboutAction; }
    
    /**
     * @brief Get the Global Text Rasterization toggle action
     * @return Pointer to the text rasterization action
     */
    QAction* getTextRasterizationAction() const { return m_textRasterizationAction; }
    
    /**
     * @brief Get the rasterization factor action group
     * @return Pointer to the action group containing factor presets
     */
    QActionGroup* getRasterFactorActionGroup() const { return m_rasterFactorGroup; }

signals:
    /**
     * @brief Signal emitted when the user triggers the Quit action
     */
    void quitRequested();

    /**
     * @brief Signal emitted when the user triggers the About action
     */
    void aboutRequested();

private:
    QMainWindow* m_mainWindow = nullptr;
    QMenu* m_fileMenu = nullptr;
    QMenu* m_viewMenu = nullptr;
    QMenu* m_rasterFactorMenu = nullptr;
    QMenu* m_helpMenu = nullptr;
    QAction* m_exitAction = nullptr;
    QAction* m_aboutAction = nullptr;
    QAction* m_textRasterizationAction = nullptr;
    QActionGroup* m_rasterFactorGroup = nullptr;
};

#endif // MENUBARMANAGER_H
