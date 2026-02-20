/**
 * @file CanvasViewPage.h
 * @brief Page displaying the canvas view for a selected remote client
 * 
 * This page shows:
 * - Remote client information (name, platform, connection status)
 * - Volume indicator
 * - Canvas container with spinner/canvas stack
 * - Upload button and back button
 * 
 * Extracted from MainWindow.cpp as part of Phase 1.2 refactoring.
 */

#ifndef CANVASVIEWPAGE_H
#define CANVASVIEWPAGE_H

#include <QWidget>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QStackedWidget>
#include <QGraphicsOpacityEffect>
#include <QPropertyAnimation>
#include <QFrame>

class ClientInfo;
class ScreenCanvas;
class SpinnerWidget;

/**
 * @class CanvasViewPage
 * @brief Widget displaying the canvas view for a remote client
 * 
 * Responsibilities:
 * - Display remote client information in top bar
 * - Show/hide volume indicator
 * - Manage canvas container with spinner/canvas states
 * - Handle remote connection status updates
 * - Coordinate upload button state
 */
class CanvasViewPage : public QWidget {
    Q_OBJECT

public:
    /**
     * @brief Constructor
     * @param parent Parent widget
     */
    explicit CanvasViewPage(QWidget* parent = nullptr);
    
    /**
     * @brief Destructor
     */
    ~CanvasViewPage() override = default;

    /**
     * @brief Get the main layout of the page
     */
    QVBoxLayout* getLayout() const { return m_layout; }

    /**
     * @brief Get the canvas container widget
     */
    QWidget* getCanvasContainer() const { return m_canvasContainer; }

    /**
     * @brief Get the canvas stack widget
     */
    QStackedWidget* getCanvasStack() const { return m_canvasStack; }

    /**
     * @brief Get the canvas host stack (for switching between canvases)
     */
    QStackedWidget* getCanvasHostStack() const { return m_canvasHostStack; }

    /**
     * @brief Get the loading spinner widget
     */
    SpinnerWidget* getLoadingSpinner() const { return m_loadingSpinner; }

    /**
     * @brief Get the remote client info container
     */
    QWidget* getRemoteClientInfoContainer() const { return m_remoteClientInfoContainer; }

    /**
     * @brief Get the volume indicator label
     */
    QLabel* getVolumeIndicator() const { return m_volumeIndicator; }

    /**
     * @brief Get the spinner opacity effect
     */
    QGraphicsOpacityEffect* getSpinnerOpacity() const { return m_spinnerOpacity; }

    /**
     * @brief Get the spinner fade animation
     */
    QPropertyAnimation* getSpinnerFade() const { return m_spinnerFade; }

    /**
     * @brief Get the canvas opacity effect
     */
    QGraphicsOpacityEffect* getCanvasOpacity() const { return m_canvasOpacity; }

    /**
     * @brief Get the canvas fade animation
     */
    QPropertyAnimation* getCanvasFade() const { return m_canvasFade; }

    /**
     * @brief Get the volume opacity effect
     */
    QGraphicsOpacityEffect* getVolumeOpacity() const { return m_volumeOpacity; }

    /**
     * @brief Get the volume fade animation
     */
    QPropertyAnimation* getVolumeFade() const { return m_volumeFade; }

    /**
     * @brief Update the display of the remote client name
     * @param client Client information to display
     */
    void updateClientNameDisplay(const ClientInfo& client);

    /**
     * @brief Update the volume indicator display
     * @param volumePercent Volume percentage (-1 if unavailable)
     */
    void updateVolumeIndicator(int volumePercent);

    /**
     * @brief Set the remote connection status
     * @param status Status text (e.g., "CONNECTED", "DISCONNECTED", "CONNECTING...")
     * @param propagateLoss Whether to propagate connection loss to canvas
     */
    void setRemoteConnectionStatus(const QString& status, bool propagateLoss = true);

    /**
     * @brief Set the active canvas
     * @param canvas The canvas to display
     */
    void setCanvas(ScreenCanvas* canvas);

    /**
     * @brief Get the current active canvas
     */
    ScreenCanvas* getCanvas() const { return m_screenCanvas; }

signals:
private:
    /**
     * @brief Setup the UI layout and widgets
     */
    void setupUI();

    /**
     * @brief Create the remote client info container
     */
    void createRemoteClientInfoContainer();

    /**
     * @brief Initialize remote client info in top bar
     */
    void initializeRemoteClientInfoInTopBar();

    /**
     * @brief Apply title text styling to a label
     */
    void applyTitleText(QLabel* label);

    // Main layout
    QVBoxLayout* m_layout;

    // Remote client info section
    QWidget* m_remoteClientInfoContainer;
    QLabel* m_clientNameLabel;
    QLabel* m_remoteConnectionStatusLabel;
    QLabel* m_volumeIndicator;
    QFrame* m_remoteInfoSep1; // Separator before status
    QFrame* m_remoteInfoSep2; // Separator before volume

    // Canvas container
    QWidget* m_canvasContainer;
    QStackedWidget* m_canvasStack;
    QStackedWidget* m_canvasHostStack;
    SpinnerWidget* m_loadingSpinner;
    ScreenCanvas* m_screenCanvas;

    // Animations
    QGraphicsOpacityEffect* m_spinnerOpacity;
    QPropertyAnimation* m_spinnerFade;
    QGraphicsOpacityEffect* m_canvasOpacity;
    QPropertyAnimation* m_canvasFade;
    QGraphicsOpacityEffect* m_volumeOpacity;
    QPropertyAnimation* m_volumeFade;

    // State
    // Animation durations
    int m_fadeDurationMs;
    int m_loaderFadeDurationMs;
};

#endif // CANVASVIEWPAGE_H
