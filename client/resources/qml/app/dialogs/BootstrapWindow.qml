import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Mouffette.App
import "../components"

Window {
    id: root

    required property var controller

    width: 460
    minimumWidth: 460
    height: content.implicitHeight + 48
    visible: !controller.ready
    title: "Mouffette"
    modality: Qt.ApplicationModal
    color: Theme.windowBackground

    ColumnLayout {
        id: content
        anchors.fill: parent
        anchors.margins: 24
        spacing: 14

        Text {
            Layout.fillWidth: true
            text: root.controller.bootstrapTitle
            color: Theme.text
            font.pixelSize: 18
            font.bold: true
        }
        Text {
            Layout.fillWidth: true
            text: root.controller.bootstrapDetail
            color: Theme.mutedText
            wrapMode: Text.Wrap
        }
        ProgressBar {
            palette: Theme.controlPalette
            Layout.fillWidth: true
            visible: !root.controller.bootstrapDecisionRequired
            indeterminate: true
        }
        RowLayout {
            Layout.fillWidth: true
            visible: root.controller.bootstrapDecisionRequired
            Item { Layout.fillWidth: true }
            AppButton {
                text: "Quit"
                onClicked: root.controller.quitBootstrap()
            }
            AppButton {
                text: root.controller.bootstrapPrimaryText
                primary: true
                onClicked: root.controller.acceptBootstrapDecision()
            }
        }
    }

    onClosing: function(close) {
        if (!root.controller.ready) {
            close.accepted = false
            root.controller.quitBootstrap()
        }
    }
}
