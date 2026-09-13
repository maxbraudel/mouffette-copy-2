import QtQuick
import "../.." as CanvasControls

Row {
    id: root
    required property var session
    spacing: 8

    CanvasControls.OverlayButton {
        objectName: "canvasSettingsButton"
        iconSource: "qrc:/icons/icons/settings.svg"
        accessibleName: "Settings"
        isToggle: true
        toggled: root.session && root.session.settingsVisible
        enabled: root.session && root.session.actionsEnabled
        onClicked: root.session.settingsVisible = !root.session.settingsVisible
    }
    Row {
        spacing: 0
        CanvasControls.OverlayButton {
            objectName: "canvasSelectionToolButton"
            iconSource: "qrc:/icons/icons/tools/selection-tool.svg"
            accessibleName: "Selection tool"
            isToggle: true
            toggled: !root.session || root.session.activeTool === "selection"
            segmentRole: "leading"
            enabled: root.session && root.session.actionsEnabled
            onClicked: root.session.setActiveTool("selection")
        }
        CanvasControls.OverlayButton {
            objectName: "canvasTextToolButton"
            iconSource: "qrc:/icons/icons/tools/text-tool.svg"
            accessibleName: "Text tool"
            isToggle: true
            toggled: root.session && root.session.activeTool === "text"
            segmentRole: "trailing"
            enabled: root.session && root.session.actionsEnabled
            onClicked: root.session.setActiveTool("text")
        }
    }
}
