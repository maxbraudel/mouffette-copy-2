import QtQuick
import QtQuick.Layouts
import Mouffette.App
import "../components"

RowLayout {
    id: root
    required property var session
    spacing: 8

    AppButton {
        objectName: "canvasSettingsButton"
        text: "⚙"
        checked: root.session && root.session.settingsVisible
        enabled: root.session && root.session.actionsEnabled
        onClicked: root.session.settingsVisible = !root.session.settingsVisible
    }
    RowLayout {
        spacing: 0
        AppButton {
            text: "↖"
            checked: !root.session || root.session.activeTool === "selection"
            onClicked: root.session.setActiveTool("selection")
        }
        AppButton {
            text: "T"
            checked: root.session && root.session.activeTool === "text"
            onClicked: root.session.setActiveTool("text")
        }
    }
}
