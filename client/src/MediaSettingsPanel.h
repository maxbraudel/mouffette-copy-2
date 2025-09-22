#pragma once

#include <QGraphicsProxyWidget>
#include <QPointer>
#include <QWidget>

class QVBoxLayout;
class QLabel;
class QCheckBox;
class QGraphicsView;
class MouseBlockingRoundedRectItem;

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

    // Update absolute position based on the provided view (left-docked with margin).
    // Call whenever zoom/transform/resize occurs.
    void updatePosition(QGraphicsView* view);

    // Optional: accessors to later read values (not used yet).
    QWidget* widget() const { return m_widget; }

protected:
    bool eventFilter(QObject* obj, QEvent* event) override;

private:
    void buildUi();
    void setBoxActive(QLabel* box, bool active);
    void clearActiveBox();
    bool isValidInputForBox(QLabel* box, QChar character);
    void updatePanelGeometry(); // Update proxy and background rect sizes

private:
    QGraphicsProxyWidget* m_proxy = nullptr;
    MouseBlockingRoundedRectItem* m_bgRect = nullptr;
    QWidget* m_widget = nullptr;
    QVBoxLayout* m_layout = nullptr;
    QLabel* m_title = nullptr;

    QCheckBox* m_autoPlayCheck = nullptr;
    QCheckBox* m_playDelayCheck = nullptr; // New: separate play delay checkbox

    QCheckBox* m_repeatCheck = nullptr;

    QCheckBox* m_fadeInCheck = nullptr;

    QCheckBox* m_fadeOutCheck = nullptr;

    // Value box widgets for click handling
    QLabel* m_autoPlayBox = nullptr;
    // New: display automatically after [x] seconds
    QCheckBox* m_displayAfterCheck = nullptr;
    QLabel* m_displayAfterBox = nullptr;
    QLabel* m_repeatBox = nullptr;
    QLabel* m_fadeInBox = nullptr;
    QLabel* m_fadeOutBox = nullptr;
    // New: set opacity to [x]%
    QCheckBox* m_opacityCheck = nullptr;
    QLabel* m_opacityBox = nullptr;
    QLabel* m_activeBox = nullptr; // currently active box (if any)
    bool m_clearOnFirstType = false; // if true, first keypress replaces previous content
    
    // Video-only option widgets (for show/hide based on media type)
    QWidget* m_autoPlayRow = nullptr;
    QWidget* m_repeatRow = nullptr;
};
