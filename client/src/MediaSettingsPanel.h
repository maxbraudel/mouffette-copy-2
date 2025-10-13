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
    void setMediaItem(class ResizableMediaBase* item);
    void applyOpacityFromUi(); // make publicly callable
    void applyVolumeFromUi();
    // Access fade durations (seconds). Returns 0 if disabled or invalid. Infinity symbol => treat as 0 (instant) for now.
    double fadeInSeconds() const;
    double fadeOutSeconds() const;

    // Scene automation accessors
    bool displayAutomaticallyEnabled() const;
    int displayDelayMillis() const; // 0 if disabled or invalid
    bool playAutomaticallyEnabled() const; // video only, safe if not video
    int playDelayMillis() const; // 0 if disabled or invalid
    void updateAvailableHeight(int maxHeightPx);
    void updatePosition();
    void setAnchorMargins(int leftMarginPx, int topMarginPx, int bottomMarginPx);

    // Optional: accessors to later read values (not used yet).
    QWidget* widget() const { return m_widget; }

protected:
    bool eventFilter(QObject* obj, QEvent* event) override;

private slots:
    void onDisplayAutomaticallyToggled(bool checked);
    void onUnmuteAutomaticallyToggled(bool checked);
    void onPlayAutomaticallyToggled(bool checked);
    void onOpacityToggled(bool checked);
    void onHideDelayToggled(bool checked);
    void onPauseDelayToggled(bool checked);
    void onVolumeToggled(bool checked);

private:
    void buildUi(QWidget* parentWidget);
    void setBoxActive(QLabel* box, bool active);
    void clearActiveBox();
    bool isValidInputForBox(QLabel* box, QChar character);
    bool boxSupportsDecimal(QLabel* box) const;
    void updateScrollbarGeometry();
    void pullSettingsFromMedia();
    void pushSettingsToMedia();

private:
    QWidget* m_widget = nullptr; // parented to viewport
    QVBoxLayout* m_rootLayout = nullptr;
    const int m_panelWidthPx = 221;
    // Scrollable content
    QScrollArea* m_scrollArea = nullptr;
    QWidget* m_scrollContainer = nullptr;
    QWidget* m_innerContent = nullptr;
    QVBoxLayout* m_contentLayout = nullptr;
    QLabel* m_title = nullptr;
    QLabel* m_elementPropertiesTitle = nullptr;
    int m_anchorLeftMargin = 16;
    int m_anchorTopMargin = 16;
    int m_anchorBottomMargin = 16;

    QCheckBox* m_autoPlayCheck = nullptr;
    QCheckBox* m_playDelayCheck = nullptr; // New: separate play delay checkbox
    QCheckBox* m_pauseDelayCheck = nullptr; // New: pause delay checkbox

    QCheckBox* m_repeatCheck = nullptr;
    
    // Display delay checkbox (separate from display automatically)
    QCheckBox* m_displayDelayCheck = nullptr;
    QCheckBox* m_unmuteDelayCheck = nullptr;

    QCheckBox* m_fadeInCheck = nullptr;

    QCheckBox* m_fadeOutCheck = nullptr;

    QCheckBox* m_hideDelayCheck = nullptr;
    QCheckBox* m_hideWhenVideoEndsCheck = nullptr;

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
    QLabel* m_hideDelayBox = nullptr;
    // New: set opacity to [x]%
    QCheckBox* m_opacityCheck = nullptr;
    QLabel* m_opacityBox = nullptr;
    QCheckBox* m_volumeCheck = nullptr;
    QLabel* m_volumeBox = nullptr;
    QCheckBox* m_unmuteCheck = nullptr;
    QLabel* m_unmuteDelayBox = nullptr;
    QLabel* m_unmuteDelaySecondsLabel = nullptr;
    QLabel* m_activeBox = nullptr; // currently active box (if any)
    bool m_clearOnFirstType = false; // if true, first keypress replaces previous content
    bool m_pendingDecimalInsertion = false; // awaiting first fractional digit after a decimal point
    // Overlay scrollbar to mirror media list behavior
    QScrollBar* m_overlayVScroll = nullptr;
    QTimer* m_scrollbarHideTimer = nullptr;
    bool m_updatingFromMedia = false;
    
    // Video-only option widgets (for show/hide based on media type)
    QWidget* m_autoPlayRow = nullptr;
    QWidget* m_playDelayRow = nullptr;
    QWidget* m_pauseDelayRow = nullptr;
    QWidget* m_repeatRow = nullptr;
    QWidget* m_hideDelayRow = nullptr;
    QWidget* m_hideWhenEndsRow = nullptr;
    QWidget* m_volumeRow = nullptr;
    QWidget* m_unmuteRow = nullptr;
    QWidget* m_unmuteDelayRow = nullptr;

    QLabel* m_hideDelaySecondsLabel = nullptr;
    QLabel* m_pauseDelayBox = nullptr;
    QLabel* m_pauseDelaySecondsLabel = nullptr;
    class ResizableMediaBase* m_mediaItem = nullptr; // not owning
};
