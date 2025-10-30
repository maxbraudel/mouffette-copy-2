#include "managers/app/MenuBarManager.h"
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
    
    // Create Help menu
    m_helpMenu = menuBar->addMenu("Help");
    
    // Create About action
    m_aboutAction = new QAction("About", this);
    connect(m_aboutAction, &QAction::triggered, this, &MenuBarManager::aboutRequested);
    m_helpMenu->addAction(m_aboutAction);

    qDebug() << "MenuBarManager: Menu bar setup complete";
}
