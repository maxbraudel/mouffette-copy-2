#ifndef RESPONSIVELAYOUTMANAGER_H
#define RESPONSIVELAYOUTMANAGER_H

#include <QObject>
#include <QWidget>
#include <QHBoxLayout>
#include <QVBoxLayout>
#include <QSpacerItem>
#include <QStackedWidget>
#include <QPushButton>
#include <QLabel>

class MainWindow;

class ResponsiveLayoutManager : public QObject
{
    Q_OBJECT

public:
    explicit ResponsiveLayoutManager(MainWindow* parent = nullptr);
    ~ResponsiveLayoutManager();

    // Main responsive update methods
    void updateResponsiveLayout();
    void updateResponsiveButtonVisibility();

    // Configuration methods
    void setClientInfoThreshold(int width);
    void setButtonVisibilityThreshold(int width);

    // Initialization methods
    void initialize();

private:
    MainWindow* m_mainWindow;
    
    // Thresholds for responsive behavior
    int m_clientInfoThreshold = 600;     // Width threshold for client info positioning
    int m_buttonVisibilityThreshold = 1100; // Width threshold for button visibility
    
    // State tracking
    bool m_clientInfoInTopBar = true;
    bool m_buttonsVisible = true;
    
    // Responsive layout widgets
    QWidget* m_responsiveClientInfoContainer = nullptr;
    QSpacerItem* m_responsiveClientInfoSpacing = nullptr;
    
    // Helper methods
    void moveClientInfoToTopBar();
    void moveClientInfoBelowTopBar();
    void updateButtonVisibility(bool visible);
    bool isOnScreenView() const;
    
    // Widget accessors (through MainWindow)
    QWidget* getRemoteClientInfoContainer() const;
    QHBoxLayout* getConnectionLayout() const;
    QVBoxLayout* getMainLayout() const;
    QStackedWidget* getStackedWidget() const;
    QWidget* getScreenViewWidget() const;
    QWidget* getClientListPage() const;
    QPushButton* getBackButton() const;
    QLabel* getConnectionStatusLabel() const;
    QWidget* getLocalClientInfoContainer() const;
    QPushButton* getConnectToggleButton() const;
    QPushButton* getSettingsButton() const;
    int getInnerContentGap() const;
};

#endif // RESPONSIVELAYOUTMANAGER_H