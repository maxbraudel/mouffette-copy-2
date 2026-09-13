import QtQuick
import Mouffette.App

Item {
    id: root
    required property var controller
    anchors.fill: parent
    z: 1000000
    enabled: false

    Column {
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        anchors.rightMargin: 20
        anchors.bottomMargin: 20
        spacing: 10

        Repeater {
            model: root.controller.toastModel
            delegate: Rectangle {
                id: toast
                required property int index
                required property int severityKind
                required property string message
                width: Math.min(420, Math.max(260, toastText.implicitWidth + 32))
                height: toastText.implicitHeight + 24
                radius: 8
                color: toast.severityKind === 0 ? Theme.connectedBackground
                       : toast.severityKind === 1 ? Theme.errorBackground
                       : toast.severityKind === 2 ? Theme.warningBackground
                                                 : Theme.brandBlueLight
                border.width: 1
                border.color: toast.severityKind === 0 ? Theme.connectedText
                              : toast.severityKind === 1 ? Theme.errorText
                              : toast.severityKind === 2 ? Theme.warningText
                                                        : Theme.brandBlue

                Text {
                    id: toastText
                    anchors.fill: parent
                    anchors.margins: 12
                    text: toast.message
                    color: Theme.text
                    wrapMode: Text.Wrap
                    verticalAlignment: Text.AlignVCenter
                }

                Behavior on opacity { NumberAnimation { duration: 180 } }
                Behavior on y { NumberAnimation { duration: 180; easing.type: Easing.OutCubic } }
            }
        }
    }
}
