import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Mouffette.App
import "../components"

Item {
    id: root
    required property var controller

    ColumnLayout {
        anchors.fill: parent
        spacing: 12

        RowLayout {
            Layout.fillWidth: true
            Text {
                text: "Latest notifications are shown first."
                color: Theme.mutedText
            }
            Item { Layout.fillWidth: true }
            AppButton {
                text: "Clear History"
                enabled: root.controller.historyModel.count > 0
                onClicked: root.controller.requestClearHistory()
            }
        }

        ListView {
            id: historyList
            objectName: "historyList"
            Layout.fillWidth: true
            Layout.fillHeight: true
            clip: true
            spacing: 8
            model: root.controller.historyModel
            boundsBehavior: Flickable.StopAtBounds
            ScrollBar.vertical: ScrollBar { policy: ScrollBar.AsNeeded }

            delegate: Rectangle {
                id: card
                required property int index
                required property int severityKind
                required property string severityLabel
                required property string category
                required property string message
                required property string timestampText
                width: historyList.width - 6
                height: content.implicitHeight + 21
                radius: 8
                color: Theme.interactionBackground
                border.width: 1
                border.color: Theme.border

                ColumnLayout {
                    id: content
                    anchors.fill: parent
                    anchors.margins: 11
                    spacing: 7

                    RowLayout {
                        Layout.fillWidth: true
                        spacing: 8
                        Rectangle {
                            Layout.preferredWidth: severityText.implicitWidth + 14
                            Layout.preferredHeight: 20
                            radius: 5
                            color: card.severityKind === 0 ? Theme.connectedBackground
                                   : card.severityKind === 1 ? Theme.errorBackground
                                   : card.severityKind === 2 ? Theme.warningBackground
                                                             : Theme.brandBlueLight
                            Text {
                                id: severityText
                                anchors.centerIn: parent
                                text: card.severityLabel || "INFO"
                                color: card.severityKind === 0 ? Theme.connectedText
                                       : card.severityKind === 1 ? Theme.errorText
                                       : card.severityKind === 2 ? Theme.warningText
                                                                 : Theme.brandBlue
                                font.pixelSize: 11
                                font.bold: true
                            }
                        }
                        Text {
                            text: card.category || "General"
                            color: Theme.text
                            font.bold: true
                        }
                        Item { Layout.fillWidth: true }
                        Text {
                            text: card.timestampText
                            color: Theme.mutedText
                        }
                    }
                    TextEdit {
                        Layout.fillWidth: true
                        text: card.message
                        color: Theme.text
                        readOnly: true
                        selectByMouse: true
                        wrapMode: TextEdit.Wrap
                    }
                }
            }

            Text {
                anchors.centerIn: parent
                visible: historyList.count === 0
                text: "No notifications yet.\nNew notifications will appear here."
                color: Theme.mutedText
                font.pixelSize: 16
                font.italic: true
                horizontalAlignment: Text.AlignHCenter
            }
        }
    }
}
