import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Mouffette.App
import "../components"

Dialog {
    id: dialog

    required property var controller
    property string validationError: ""

    parent: Overlay.overlay
    anchors.centerIn: parent
    width: Math.min(520, parent ? parent.width - 40 : 520)
    modal: true
    focus: true
    palette: Theme.controlPalette
    Overlay.modal: Rectangle { color: Theme.modalScrim }
    title: "Settings"
    padding: 20
    closePolicy: Popup.CloseOnEscape

    onOpened: {
        serverUrl.text = controller.settingsServerUrl
        autoUpload.checked = controller.settingsAutoUpload
        appAlwaysOnTop.checked = controller.settingsAppAlwaysOnTop
        validationError = ""
    }

    background: Rectangle {
        color: Theme.elevatedBackground
        border.width: 1
        border.color: Theme.border
        radius: 10
    }

    contentItem: ColumnLayout {
        spacing: 12
        enabled: !controller.clearingStorage
        Text {
            text: "Server URL"
            color: Theme.text
        }
        AppTextField {
            id: serverUrl
            objectName: "settingsServerUrl"
            Layout.fillWidth: true
        }
        Text {
            Layout.fillWidth: true
            visible: dialog.validationError.length > 0
            text: dialog.validationError
            color: Theme.errorText
            wrapMode: Text.Wrap
        }
        AppCheckBox {
            id: autoUpload
            text: "Upload imported media automatically"
        }
        AppCheckBox {
            id: appAlwaysOnTop
            objectName: "settingsAppAlwaysOnTop"
            text: "App always on top"
        }
        RowLayout {
            Layout.fillWidth: true
            AppButton {
                objectName: "clearStorageAndCloseButton"
                text: "Clear storage and close"
                destructive: true
                enabled: controller.ready && !controller.clearingStorage
                onClicked: controller.clearStorageAndClose()
            }
            Item { Layout.fillWidth: true }
            AppButton {
                text: "Cancel"
                onClicked: dialog.reject()
            }
            AppButton {
                text: "Save"
                primary: true
                onClicked: {
                    var error = controller.saveSettings(serverUrl.text.trim(), autoUpload.checked, appAlwaysOnTop.checked)
                    if (error && error.length > 0) {
                        dialog.validationError = error
                        return
                    }
                    dialog.accept()
                }
            }
        }
    }
}
