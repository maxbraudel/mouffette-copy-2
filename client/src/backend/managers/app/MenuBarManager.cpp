#include "backend/managers/app/MenuBarManager.h"
#include <QMainWindow>
#include <QMenuBar>
#include <QApplication>
#include <QKeySequence>
#include <QDebug>

MenuBarManager::MenuBarManager(QMainWindow* mainWindow, QObject* parent)
    : QObject(parent)
    , m_mainWindow(mainWindow)
{
}

void MenuBarManager::setup()
{
    if (!m_mainWindow) {
        qWarning() << "MenuBarManager::setup() - No main window provided";
        return;
    }

    QMenuBar* menuBar = m_mainWindow->menuBar();
    if (!menuBar) {
        qWarning() << "MenuBarManager::setup() - Failed to get menu bar";
        return;
    }

    // Create File menu
    m_fileMenu = menuBar->addMenu("File");
    
    // Create Quit action with standard keyboard shortcut
    m_exitAction = new QAction("Quit Mouffette", this);
    m_exitAction->setShortcut(QKeySequence::Quit);
    connect(m_exitAction, &QAction::triggered, this, &MenuBarManager::quitRequested);
    m_fileMenu->addAction(m_exitAction);
    
    // Create View menu
    m_viewMenu = menuBar->addMenu("View");
    
    // Create Global Text Rasterization toggle action
    m_textRasterizationAction = new QAction("Global Text Rasterization", this);
    m_textRasterizationAction->setCheckable(true);
    m_textRasterizationAction->setChecked(true); // Enabled by default
    m_textRasterizationAction->setToolTip("Render all text elements in a single composited layer for better performance");
    m_viewMenu->addAction(m_textRasterizationAction);
    
    // Create Rasterization Factor submenu for debugging
    m_rasterFactorMenu = m_viewMenu->addMenu("Rasterization Factor (Debug)");
    m_rasterFactorGroup = new QActionGroup(this);
    
    // Add preset factor values
    const QList<QPair<QString, qreal>> presets = {
        {"0.25x (Very Low)", 0.25},
        {"0.5x (Low)", 0.5},
        {"1.0x (Normal)", 1.0},
        {"2.0x (High)", 2.0}
    };
    
    for (const auto& preset : presets) {
        QAction* action = new QAction(preset.first, this);
        action->setCheckable(true);
        action->setData(preset.second);
        action->setActionGroup(m_rasterFactorGroup);
        m_rasterFactorMenu->addAction(action);
        
        // Set 1.0x as default
        if (qFuzzyCompare(preset.second, 1.0)) {
            action->setChecked(true);
        }
    }
    
    // Create Help menu
    m_helpMenu = menuBar->addMenu("Help");
    
    // Create About action
    m_aboutAction = new QAction("About", this);
    connect(m_aboutAction, &QAction::triggered, this, &MenuBarManager::aboutRequested);
    m_helpMenu->addAction(m_aboutAction);

    qDebug() << "MenuBarManager: Menu bar setup complete";
}
