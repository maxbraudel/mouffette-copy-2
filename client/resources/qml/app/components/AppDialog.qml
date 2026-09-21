import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Mouffette.App

Dialog {
    id: dialog

    property var controller: null
    property var peers: []
    property string message: ""
    property string detail: ""
    property string acceptText: "OK"
    property string rejectText: "Cancel"
    property bool destructive: false
    property bool showReject: true

    modal: true
    focus: true
    palette: Theme.controlPalette
    Overlay.modal: Rectangle { color: Theme.modalScrim }
    closePolicy: Popup.CloseOnEscape
    padding: 20

    background: Rectangle {
        color: Theme.elevatedBackground
        border.width: 1
        border.color: Theme.border
        radius: 10
    }

    contentItem: ColumnLayout {
        spacing: 14

        NotificationPeers {
            Layout.fillWidth: true
            controller: dialog.controller
            peers: dialog.peers
        }
        Text {
            Layout.fillWidth: true
            text: dialog.message
            color: Theme.text
            font.pixelSize: 16
            font.bold: true
            wrapMode: Text.Wrap
        }
        Text {
            Layout.fillWidth: true
            visible: text.length > 0
            text: dialog.detail
            color: Theme.mutedText
            font.pixelSize: 14
            wrapMode: Text.Wrap
        }
        RowLayout {
            Layout.fillWidth: true
            spacing: 8
            Item { Layout.fillWidth: true }
            AppButton {
                visible: dialog.showReject
                text: dialog.rejectText
                onClicked: dialog.reject()
            }
            AppButton {
                text: dialog.acceptText
                primary: !dialog.destructive
                destructive: dialog.destructive
                onClicked: dialog.accept()
            }
        }
    }
}
