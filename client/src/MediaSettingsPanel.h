#pragma once

#include <QGraphicsProxyWidget>
#include <QPointer>
#include <QWidget>

class QVBoxLayout;
class QLabel;
class QCheckBox;
class QLineEdit;
class QGraphicsView;

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

    // Update absolute position based on the provided view (left-docked with margin).
    // Call whenever zoom/transform/resize occurs.
    void updatePosition(QGraphicsView* view);

    // Optional: accessors to later read values (not used yet).
    QWidget* widget() const { return m_widget; }

private:
    void buildUi();

private:
    QGraphicsProxyWidget* m_proxy = nullptr;
    QWidget* m_widget = nullptr;
    QVBoxLayout* m_layout = nullptr;
    QLabel* m_title = nullptr;

    QCheckBox* m_autoPlayCheck = nullptr;
    QLineEdit* m_autoPlaySeconds = nullptr;

    QCheckBox* m_repeatCheck = nullptr;
    QLineEdit* m_repeatTimes = nullptr;

    QCheckBox* m_fadeInCheck = nullptr;
    QLineEdit* m_fadeInSeconds = nullptr;

    QCheckBox* m_fadeOutCheck = nullptr;
    QLineEdit* m_fadeOutSeconds = nullptr;
};
