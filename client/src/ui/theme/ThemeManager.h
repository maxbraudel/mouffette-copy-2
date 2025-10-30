#ifndef THEMEMANAGER_H
#define THEMEMANAGER_H

#include <QObject>
#include <QPushButton>
#include <QLabel>
#include <QListWidget>

/**
 * @brief Centralized theme and style management system
 * 
 * ThemeManager is a singleton that manages all visual styling configuration
 * throughout the application. It provides:
 * - Centralized style configuration (margins, radii, fonts, etc.)
 * - Helper methods to apply consistent styles to UI elements
 * - Theme change notifications
 * 
 * This replaces the global variables and inline style functions previously
 * scattered throughout MainWindow.cpp.
 */
class ThemeManager : public QObject {
    Q_OBJECT

public:
    /**
     * @brief Style configuration structure
     * 
     * Contains all global style parameters that control the appearance
     * of UI elements throughout the application.
     */
    struct StyleConfig {
        // Window content margins
        int windowContentMarginTop = 20;
        int windowContentMarginRight = 20;
        int windowContentMarginBottom = 20;
        int windowContentMarginLeft = 20;
        
        // Window appearance
        int windowBorderRadiusPx = 10;
        
        // Content spacing
        int innerContentGap = 20;
        
        // Dynamic box configuration (buttons, status boxes)
        int dynamicBoxMinWidth = 80;
        int dynamicBoxHeight = 24;
        int dynamicBoxBorderRadius = 6;
        int dynamicBoxFontPx = 13;
        
        // Remote client info container
        int remoteClientContainerPadding = 6;
        
        // Title text
        int titleTextFontSize = 16;
        int titleTextHeight = 24;
    };

    /**
     * @brief Get the singleton instance
     */
    static ThemeManager* instance();
    
    /**
     * @brief Get the current style configuration
     */
    StyleConfig getStyleConfig() const { return m_config; }
    
    /**
     * @brief Set a new style configuration
     * @param config The new configuration to apply
     * 
     * Emits styleConfigChanged() signal after updating.
     */
    void setStyleConfig(const StyleConfig& config);
    
    // Style application helpers
    
    /**
     * @brief Apply pill button style (standard grey button)
     * @param button Button to style
     */
    void applyPillButton(QPushButton* button);
    
    /**
     * @brief Apply primary button style (blue brand button)
     * @param button Button to style
     */
    void applyPrimaryButton(QPushButton* button);
    
    /**
     * @brief Apply status box style with custom colors
     * @param label Label to style as status box
     * @param borderColor Border color CSS string
     * @param bgColor Background color CSS string
     * @param textColor Text color CSS string
     */
    void applyStatusBox(QLabel* label, const QString& borderColor, 
                        const QString& bgColor, const QString& textColor);
    
    /**
     * @brief Apply title text style (bold, sized text)
     * @param label Label to style as title
     */
    void applyTitleText(QLabel* label);
    
    /**
     * @brief Apply standard list widget style
     * @param listWidget List widget to style
     */
    void applyListWidgetStyle(QListWidget* listWidget);
    
    /**
     * @brief Get maximum width for upload button
     * @return Maximum width in pixels, or INT_MAX if unlimited
     */
    int getUploadButtonMaxWidth() const;
    
    // [Phase 17] Style configuration accessors
    int getWindowContentMarginTop() const { return m_config.windowContentMarginTop; }
    int getWindowContentMarginRight() const { return m_config.windowContentMarginRight; }
    int getWindowContentMarginBottom() const { return m_config.windowContentMarginBottom; }
    int getWindowContentMarginLeft() const { return m_config.windowContentMarginLeft; }
    int getWindowBorderRadiusPx() const { return m_config.windowBorderRadiusPx; }
    int getInnerContentGap() const { return m_config.innerContentGap; }
    int getDynamicBoxMinWidth() const { return m_config.dynamicBoxMinWidth; }
    int getDynamicBoxHeight() const { return m_config.dynamicBoxHeight; }
    int getDynamicBoxBorderRadius() const { return m_config.dynamicBoxBorderRadius; }
    int getDynamicBoxFontPx() const { return m_config.dynamicBoxFontPx; }
    int getRemoteClientContainerPadding() const { return m_config.remoteClientContainerPadding; }
    int getTitleTextFontSize() const { return m_config.titleTextFontSize; }
    int getTitleTextHeight() const { return m_config.titleTextHeight; }
    
    /**
     * @brief Update all widget stylesheets to reflect current theme
     * @param mainWindow Pointer to main window for accessing widgets
     * 
     * This method updates all dynamic stylesheets throughout the application
     * when the theme changes (e.g., light/dark mode switch).
     */
    void updateAllWidgetStyles(class MainWindow* mainWindow);

signals:
    /**
     * @brief Emitted when the theme changes (e.g., light/dark mode switch)
     */
    void themeChanged();
    
    /**
     * @brief Emitted when the style configuration changes
     */
    void styleConfigChanged();

private:
    ThemeManager(QObject* parent = nullptr);
    ~ThemeManager() override = default;
    
    // Singleton: prevent copying
    ThemeManager(const ThemeManager&) = delete;
    ThemeManager& operator=(const ThemeManager&) = delete;
    
    static ThemeManager* s_instance;
    StyleConfig m_config;
};

#endif // THEMEMANAGER_H
