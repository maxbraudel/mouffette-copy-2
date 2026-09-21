import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Dialogs
import Mouffette.App
import "../components"

Dialog {
    id: dialog

    required property var controller
    property string validationError: ""

    parent: Overlay.overlay
    anchors.centerIn: parent
    width: Math.min(520, parent ? parent.width - 40 : 520)
    height: Math.min(implicitHeight, parent ? Math.max(200, parent.height - 40) : implicitHeight)
    modal: true
    focus: true
    palette: Theme.controlPalette
    Overlay.modal: Rectangle { color: Theme.modalScrim }
    title: "Settings"
    padding: 20
    closePolicy: Popup.CloseOnEscape

    onOpened: {
        controller.beginProfileEdit()
        username.text = controller.settingsUsername
        serverUrl.text = controller.settingsServerUrl
        autoUpload.checked = controller.settingsAutoUpload
        appAlwaysOnTop.checked = controller.settingsAppAlwaysOnTop
        screenSharing.checked = controller.settingsScreenSharingEnabled
        validationError = ""
    }
    onClosed: controller.cancelProfileEdit()

    FileDialog {
        id: photoPicker
        title: "Choose profile picture"
        fileMode: FileDialog.OpenFile
        nameFilters: ["Images (*.png *.jpg *.jpeg *.webp)"]
        onAccepted: {
            dialog.validationError = dialog.controller.importProfilePicture(selectedFile)
        }
    }

    background: Rectangle {
        color: Theme.elevatedBackground
        border.width: 1
        border.color: Theme.border
        radius: 10
    }

    contentItem: ColumnLayout {
        spacing: 12
        enabled: !dialog.controller.clearingStorage

        ScrollView {
            id: scroll
            objectName: "settingsScrollView"
            Layout.fillWidth: true
            Layout.fillHeight: true
            Layout.minimumHeight: 0
            implicitHeight: form.implicitHeight
            contentWidth: availableWidth
            clip: true
            ScrollBar.horizontal.policy: ScrollBar.AlwaysOff

            ColumnLayout {
                id: form
                width: scroll.availableWidth
                spacing: 12

                RowLayout {
                    Layout.fillWidth: true
                    spacing: 12
                    ProfilePicture {
                        id: preview
                        objectName: "settingsProfilePicture"
                        Layout.preferredWidth: 72
                        Layout.preferredHeight: 72
                        Layout.alignment: Qt.AlignTop
                        source: dialog.controller.settingsProfilePictureDraftSource
                    }
                    ColumnLayout {
                        Layout.fillWidth: true
                        spacing: 8
                        Text {
                            text: "Username (optional)"
                            color: Theme.text
                        }
                        AppTextField {
                            id: username
                            objectName: "settingsUsername"
                            Layout.fillWidth: true
                            placeholderText: dialog.controller.settingsHostname
                            // QML counts UTF-16 units; validation counts Unicode
                            // code points so all 64 characters may be emoji.
                            maximumLength: 128
                        }
                        RowLayout {
                            spacing: 8
                            AppButton {
                                objectName: "settingsChooseProfilePicture"
                                text: "Choose photo"
                                onClicked: photoPicker.open()
                            }
                            AppButton {
                                objectName: "settingsRemoveProfilePicture"
                                text: "Remove"
                                enabled: dialog.controller.settingsProfilePictureDraftSource.length > 0
                                    && dialog.controller.settingsProfilePictureDraftSource !== preview.defaultSource.toString()
                                onClicked: {
                                    dialog.controller.removeProfilePicture()
                                    dialog.validationError = ""
                                }
                            }
                        }
                        Text {
                            Layout.fillWidth: true
                            text: "Optional photo · PNG, JPG or WebP\nCropped to 250 × 250"
                            color: Theme.mutedText
                            font.pixelSize: 12
                            wrapMode: Text.Wrap
                        }
                    }
                }

                Text {
                    Layout.fillWidth: true
                    text: "Leave empty to use this computer's hostname."
                    color: Theme.mutedText
                    font.pixelSize: 12
                    wrapMode: Text.Wrap
                }
                Text {
                    text: "Server URL"
                    color: Theme.text
                }
                AppTextField {
                    id: serverUrl
                    objectName: "settingsServerUrl"
                    Layout.fillWidth: true
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
                AppCheckBox {
                    id: screenSharing
                    objectName: "settingsScreenSharingEnabled"
                    text: "Share my screens with connected clients"
                }
                Text {
                    Layout.fillWidth: true
                    text: "Connected clients can see the live content of all your screens. Changes apply when you save."
                    color: Theme.mutedText
                    font.pixelSize: 12
                    wrapMode: Text.Wrap
                }
                Text {
                    objectName: "settingsScreenRecordingHelp"
                    Layout.fillWidth: true
                    visible: Qt.platform.os === "osx"
                    text: "This checkbox does not grant macOS Screen Recording permission. Open System Settings → Privacy & Security → Screen & System Audio Recording and allow the app named in the permission request. For development launches, this may be Terminal or Visual Studio Code. Then quit and reopen all Mouffette instances."
                    color: Theme.mutedText
                    font.pixelSize: 12
                    wrapMode: Text.Wrap
                }
                Text {
                    objectName: "settingsScreenSharingStatus"
                    Layout.fillWidth: true
                    visible: text.length > 0
                    text: dialog.controller.settingsScreenSharingStatus || ""
                    textFormat: Text.PlainText
                    color: Theme.mutedText
                    font.pixelSize: 12
                    wrapMode: Text.Wrap
                }
            }
        }
        Text {
            Layout.fillWidth: true
            visible: dialog.validationError.length > 0
            text: dialog.validationError
            textFormat: Text.PlainText
            color: Theme.errorText
            wrapMode: Text.Wrap
        }
        RowLayout {
            Layout.fillWidth: true
            AppButton {
                objectName: "clearStorageAndCloseButton"
                text: "Clear storage and close"
                ToolTip.visible: hovered
                ToolTip.text: "Clear this instance's settings, projects, history and cache. Its network identity and other instances are kept."
                destructive: true
                enabled: dialog.controller.ready && !dialog.controller.clearingStorage
                onClicked: dialog.controller.clearStorageAndClose()
            }
            Item { Layout.fillWidth: true }
            AppButton {
                objectName: "settingsCancel"
                text: "Cancel"
                onClicked: dialog.reject()
            }
            AppButton {
                objectName: "settingsSave"
                text: "Save"
                primary: true
                onClicked: {
                    var error = dialog.controller.saveSettings(serverUrl.text.trim(), autoUpload.checked,
                                                               appAlwaysOnTop.checked, username.text,
                                                               screenSharing.checked)
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
