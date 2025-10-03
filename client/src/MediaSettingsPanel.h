#pragma once

#include <QGraphicsProxyWidget>
#include <QPointer>
#include <QWidget>

class QVBoxLayout;
class QLabel;
class QCheckBox;
class QGraphicsView;
class MouseBlockingRoundedRectItem;
class QScrollArea;
class QScrollBar;
class QTimer;

// Floating settings panel shown when a media's settings toggle is enabled.
// Implemented as a QWidget embedded into the scene via QGraphicsProxyWidget.
class MediaSettingsPanel : public QObject {
    Q_OBJECT
public:
    explicit MediaSettingsPanel(QObject* parent = nullptr);
    ~MediaSettingsPanel() override;

    // Adds the panel to the given scene (once) and prepares for display.
    void ensureInScene(QGraphicsScene* scene);

    // Shows or hides the panel; when showing, position near the left edge of the view.
    void setVisible(bool visible);
    bool isVisible() const;
    
    // Configure which options are available based on media type
    void setMediaType(bool isVideo);
    void setMediaItem(class ResizableMediaBase* item) { m_mediaItem = item; }
    void applyOpacityFromUi(); // make publicly callable
    // Access fade durations (seconds). Returns 0 if disabled or invalid. Infinity symbol => treat as 0 (instant) for now.
    double fadeInSeconds() const;
    double fadeOutSeconds() const;

    // Update absolute position based on the provided view (left-docked with margin).
    // Call whenever zoom/transform/resize occurs.
    void updatePosition(QGraphicsView* view);

    // Optional: accessors to later read values (not used yet).
    QWidget* widget() const { return m_widget; }

protected:
    bool eventFilter(QObject* obj, QEvent* event) override;

private slots:
    void onDisplayAutomaticallyToggled(bool checked);
    void onPlayAutomaticallyToggled(bool checked);
    void onOpacityToggled(bool checked);

private:
    void buildUi();
    void setBoxActive(QLabel* box, bool active);
    void clearActiveBox();
    bool isValidInputForBox(QLabel* box, QChar character);
    void updateScrollbarGeometry();

private:
    QGraphicsProxyWidget* m_proxy = nullptr;
    MouseBlockingRoundedRectItem* m_bgRect = nullptr;
    QWidget* m_widget = nullptr;
    // Root layout (wraps scroll area)
    QVBoxLayout* m_rootLayout = nullptr;
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
