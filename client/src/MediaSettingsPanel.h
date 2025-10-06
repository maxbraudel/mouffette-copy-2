#pragma once

#include <QWidget>
#include <QIcon>

class QVBoxLayout;
class QLabel;
class QCheckBox;
class QScrollArea;
class QScrollBar;
class QTimer;

// Floating settings panel shown when a media's settings toggle is enabled.
// Implemented as a QWidget parented to the viewport (like info overlay).
class MediaSettingsPanel : public QObject {
    Q_OBJECT
public:
    explicit MediaSettingsPanel(QWidget* parentWidget);
    ~MediaSettingsPanel() override = default;

    // Shows or hides the panel widget
    void setVisible(bool visible);
    bool isVisible() const;
    
    // Configure which options are available based on media type
    void setMediaType(bool isVideo);
    void setMediaItem(class ResizableMediaBase* item) { m_mediaItem = item; }
    void applyOpacityFromUi(); // make publicly callable
    // Access fade durations (seconds). Returns 0 if disabled or invalid. Infinity symbol => treat as 0 (instant) for now.
    double fadeInSeconds() const;
    double fadeOutSeconds() const;

    // Scene automation accessors
    bool displayAutomaticallyEnabled() const;
    int displayDelayMillis() const; // 0 if disabled or invalid
    bool playAutomaticallyEnabled() const; // video only, safe if not video
    int playDelayMillis() const; // 0 if disabled or invalid

    // Update absolute position (left-docked with margin).
    // Call whenever viewport resize occurs.
    void updatePosition();

    // Expand or collapse the settings content (show/hide options)
    void setExpanded(bool expanded);
    bool isExpanded() const { return m_isExpanded; }

    // Optional: accessors to later read values (not used yet).
    QWidget* widget() const { return m_widget; }

protected:
    bool eventFilter(QObject* obj, QEvent* event) override;

private slots:
    void onDisplayAutomaticallyToggled(bool checked);
    void onPlayAutomaticallyToggled(bool checked);
    void onOpacityToggled(bool checked);

private:
    void buildUi(QWidget* parentWidget);
    void setBoxActive(QLabel* box, bool active);
    void clearActiveBox();
    bool isValidInputForBox(QLabel* box, QChar character);
    void updateScrollbarGeometry();
    void refreshToggleMetrics();
    void updatePanelChrome();

private:
    QWidget* m_widget = nullptr; // parented to viewport
    // Root layout (wraps header and scroll area)
    QVBoxLayout* m_rootLayout = nullptr;
    // Header with toggle button
    QWidget* m_headerWidget = nullptr;
    QVBoxLayout* m_headerLayout = nullptr;
    class QPushButton* m_toggleButton = nullptr;
    bool m_isExpanded = false;
    int m_collapsedButtonSizePx = 36;
    int m_toggleIconSizePx = 20;
    const int m_expandedWidthPx = 221;
    QIcon m_settingsIcon;
    // Scrollable content
    QScrollArea* m_scrollArea = nullptr;
    QWidget* m_innerContent = nullptr;
    QVBoxLayout* m_contentLayout = nullptr;
    QLabel* m_title = nullptr;
    QLabel* m_elementPropertiesTitle = nullptr;

    QCheckBox* m_autoPlayCheck = nullptr;
    QCheckBox* m_playDelayCheck = nullptr; // New: separate play delay checkbox

    QCheckBox* m_repeatCheck = nullptr;
    
    // Display delay checkbox (separate from display automatically)
    QCheckBox* m_displayDelayCheck = nullptr;

    QCheckBox* m_fadeInCheck = nullptr;

    QCheckBox* m_fadeOutCheck = nullptr;

    // Value box widgets for click handling
    QLabel* m_autoPlayBox = nullptr;
    QLabel* m_autoPlaySecondsLabel = nullptr; // "seconds" text after the play delay input box
    // New: display automatically after [x] seconds
    QCheckBox* m_displayAfterCheck = nullptr;
    QLabel* m_displayAfterBox = nullptr;
    QLabel* m_displayAfterSecondsLabel = nullptr; // "seconds" text after the input box
    QLabel* m_repeatBox = nullptr;
    QLabel* m_fadeInBox = nullptr;
    QLabel* m_fadeOutBox = nullptr;
    // New: set opacity to [x]%
    QCheckBox* m_opacityCheck = nullptr;
    QLabel* m_opacityBox = nullptr;
    QLabel* m_activeBox = nullptr; // currently active box (if any)
    bool m_clearOnFirstType = false; // if true, first keypress replaces previous content
    // Overlay scrollbar to mirror media list behavior
    QScrollBar* m_overlayVScroll = nullptr;
    QTimer* m_scrollbarHideTimer = nullptr;
    
    // Video-only option widgets (for show/hide based on media type)
    QWidget* m_autoPlayRow = nullptr;
    QWidget* m_playDelayRow = nullptr;
    QWidget* m_repeatRow = nullptr;
    class ResizableMediaBase* m_mediaItem = nullptr; // not owning
};
