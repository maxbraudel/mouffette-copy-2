#pragma once

#include <QWidget>

class QVBoxLayout;
class QLabel;
class QCheckBox;
class QScrollArea;
class QScrollBar;
class QTimer;
class QPushButton;
class QSpacerItem;

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
    void updateTextSectionVisibility(bool isTextMedia);
    void setMediaItem(class ResizableMediaBase* item);
    void applyOpacityFromUi(); // make publicly callable
    void applyVolumeFromUi();
    void refreshVolumeDisplay(); // Update volume display from media item state (for real-time slider sync)
    // Access fade durations (seconds). Returns 0 if disabled or invalid. Infinity symbol => treat as 0 (instant) for now.
    double fadeInSeconds() const;
    double fadeOutSeconds() const;

    // Scene automation accessors
    bool displayAutomaticallyEnabled() const;
    int displayDelayMillis() const; // 0 if disabled or invalid
    bool playAutomaticallyEnabled() const; // video only, safe if not video
    int playDelayMillis() const; // 0 if disabled or invalid
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
    void onMuteDelayToggled(bool checked);
    void onPauseDelayToggled(bool checked);
    void onVolumeToggled(bool checked);
    void onTextColorToggled(bool checked);
    void onTextColorBoxClicked();
    void onTextBorderWidthToggled(bool checked);
    void onTextBorderColorToggled(bool checked);
    void onTextBorderColorBoxClicked();
    void onTextHighlightToggled(bool checked);
    void onTextHighlightColorBoxClicked();
    void onTextFontWeightToggled(bool checked);
    void onTextUnderlineToggled(bool checked);
    void onTextItalicToggled(bool checked);
    void onTextUppercaseToggled(bool checked);
    void onSceneTabClicked();
    void onElementTabClicked();

private:
    void buildUi(QWidget* parentWidget);
    void setBoxActive(QLabel* box, bool active);
    void clearActiveBox();
    bool isValidInputForBox(QLabel* box, QChar character);
    bool boxSupportsDecimal(QLabel* box) const;
    void updateScrollbarGeometry();
    void pullSettingsFromMedia();
    void pushSettingsToMedia();
    void updateActiveTabUi();
    void updateSectionHeaderVisibility();
    void refreshTextColorBoxStyle(bool activeHighlight = false);
    void refreshTextBorderColorBoxStyle(bool activeHighlight = false);
    void refreshTextHighlightBoxStyle(bool activeHighlight = false);

private:
    QWidget* m_widget = nullptr; // parented to viewport
    QVBoxLayout* m_rootLayout = nullptr;
    const int m_panelWidthPx = 221;
    // Scrollable content
    QScrollArea* m_scrollArea = nullptr;
    QWidget* m_scrollContainer = nullptr;
    QWidget* m_innerContent = nullptr;
    QVBoxLayout* m_contentLayout = nullptr;
    int m_anchorLeftMargin = 16;
    int m_anchorTopMargin = 16;
    int m_anchorBottomMargin = 16;
    
    // Tab system
    QPushButton* m_sceneTabButton = nullptr;
    QPushButton* m_elementTabButton = nullptr;
    QWidget* m_sceneOptionsContainer = nullptr;
    QWidget* m_elementPropertiesContainer = nullptr;
    QVBoxLayout* m_sceneOptionsLayout = nullptr;
    QVBoxLayout* m_elementPropertiesLayout = nullptr;
    QWidget* m_tabSwitcherContainer = nullptr;
    QWidget* m_tabSwitcherSeparator = nullptr;
    enum class ActiveTab { Scene, Element };
    ActiveTab m_activeTab = ActiveTab::Scene;

    // Section headers and their preceding spacers (for dynamic visibility)
    QLabel* m_sceneImageHeader = nullptr;
    QLabel* m_sceneAudioHeader = nullptr;
    QLabel* m_sceneVideoHeader = nullptr;
    QSpacerItem* m_sceneImageHeaderGap = nullptr;
    QSpacerItem* m_sceneAudioHeaderGap = nullptr;
    QSpacerItem* m_sceneVideoHeaderGap = nullptr;
    QSpacerItem* m_sceneAudioSpacer = nullptr;
    QSpacerItem* m_sceneVideoSpacer = nullptr;
    QLabel* m_elementImageHeader = nullptr;
    QLabel* m_elementAudioHeader = nullptr;
    QSpacerItem* m_elementImageHeaderGap = nullptr;
    QSpacerItem* m_elementAudioHeaderGap = nullptr;
    QSpacerItem* m_elementAudioSpacer = nullptr;

    QCheckBox* m_autoPlayCheck = nullptr;
    QCheckBox* m_playDelayCheck = nullptr;
    QCheckBox* m_pauseDelayCheck = nullptr;
    QCheckBox* m_repeatCheck = nullptr;
    QCheckBox* m_displayDelayCheck = nullptr;
    QCheckBox* m_unmuteDelayCheck = nullptr;

    QCheckBox* m_fadeInCheck = nullptr;

    QCheckBox* m_fadeOutCheck = nullptr;
    QCheckBox* m_audioFadeInCheck = nullptr;
    QCheckBox* m_audioFadeOutCheck = nullptr;

    QCheckBox* m_hideDelayCheck = nullptr;
    QCheckBox* m_hideWhenVideoEndsCheck = nullptr;
    QCheckBox* m_muteDelayCheck = nullptr;
    QCheckBox* m_muteWhenVideoEndsCheck = nullptr;

    // Value box widgets for click handling
    QLabel* m_autoPlayBox = nullptr;
    QLabel* m_autoPlaySecondsLabel = nullptr;
    QCheckBox* m_displayAfterCheck = nullptr;
    QLabel* m_displayAfterBox = nullptr;
    QLabel* m_displayAfterSecondsLabel = nullptr;
    QLabel* m_repeatBox = nullptr;
    QLabel* m_fadeInBox = nullptr;
    QLabel* m_fadeOutBox = nullptr;
    QLabel* m_audioFadeInBox = nullptr;
    QLabel* m_audioFadeOutBox = nullptr;
    QLabel* m_hideDelayBox = nullptr;
    QLabel* m_muteDelayBox = nullptr;
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
    QWidget* m_audioFadeInRow = nullptr;
    QWidget* m_audioFadeOutRow = nullptr;
    QWidget* m_hideDelayRow = nullptr;
    QWidget* m_hideWhenEndsRow = nullptr;
    QWidget* m_muteDelayRow = nullptr;
    QWidget* m_muteWhenEndsRow = nullptr;
    QWidget* m_volumeRow = nullptr;
    QWidget* m_unmuteRow = nullptr;
    QWidget* m_unmuteDelayRow = nullptr;

    QLabel* m_hideDelaySecondsLabel = nullptr;
    QLabel* m_muteDelaySecondsLabel = nullptr;
    QLabel* m_pauseDelayBox = nullptr;
    QLabel* m_pauseDelaySecondsLabel = nullptr;
    QLabel* m_audioFadeInSecondsLabel = nullptr;
    QLabel* m_audioFadeOutSecondsLabel = nullptr;
    
    // Text-only options (for TextMediaItem)
    QLabel* m_elementTextHeader = nullptr;
    QSpacerItem* m_elementTextHeaderGap = nullptr;
    QSpacerItem* m_elementTextSpacer = nullptr;
    QCheckBox* m_textColorCheck = nullptr;
    QLabel* m_textColorBox = nullptr;
    QWidget* m_textColorRow = nullptr;
    QCheckBox* m_textHighlightCheck = nullptr;
    QLabel* m_textHighlightBox = nullptr;
    QWidget* m_textHighlightRow = nullptr;
    QCheckBox* m_textBorderWidthCheck = nullptr;
    QLabel* m_textBorderWidthBox = nullptr;
    QLabel* m_textBorderWidthUnitsLabel = nullptr;
    QWidget* m_textBorderWidthRow = nullptr;
    QCheckBox* m_textBorderColorCheck = nullptr;
    QLabel* m_textBorderColorBox = nullptr;
    QWidget* m_textBorderColorRow = nullptr;
    QCheckBox* m_textFontWeightCheck = nullptr;
    QLabel* m_textFontWeightBox = nullptr;
    QWidget* m_textFontWeightRow = nullptr;
    QCheckBox* m_textUnderlineCheck = nullptr;
    QWidget* m_textUnderlineRow = nullptr;
    QCheckBox* m_textItalicCheck = nullptr;
    QWidget* m_textItalicRow = nullptr;
    QCheckBox* m_textUppercaseCheck = nullptr;
    QWidget* m_textUppercaseRow = nullptr;
    
    class ResizableMediaBase* m_mediaItem = nullptr; // not owning
};
