#pragma once

#include <QObject>

class QWidget;
class QToolButton;

class CanvasGlobalOverlayHost : public QObject {
    Q_OBJECT
public:
    enum class ToolChoice {
        Selection,
        Text
    };

    explicit CanvasGlobalOverlayHost(QObject* parent = nullptr);

    void attachViewport(QWidget* viewport);
    void ensureSettingsToggleButton();
    void ensureToolSelector();
    void updateGeometry(int margin, int spacing, int buttonSize, int iconSize);

    void setCurrentTool(ToolChoice tool);
    ToolChoice currentTool() const { return m_currentTool; }

    bool isSettingsChecked() const;
    void setSettingsChecked(bool checked, bool silent = false);

signals:
    void settingsToggled(bool checked);
    void toolSelected(CanvasGlobalOverlayHost::ToolChoice tool);

private:
    QWidget* m_viewport = nullptr;
    QToolButton* m_settingsToggleButton = nullptr;
    QWidget* m_toolSelectorContainer = nullptr;
    QToolButton* m_selectionToolButton = nullptr;
    QToolButton* m_textToolButton = nullptr;
    ToolChoice m_currentTool = ToolChoice::Selection;
};
