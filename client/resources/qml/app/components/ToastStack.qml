import QtQuick
import QtQuick.Layouts
import Mouffette.App

Item {
    id: root
    required property var controller
    anchors.fill: parent
    z: 1000000
    enabled: false

    Column {
        id: stack
        objectName: "toastColumn"
        anchors.left: parent.left
        anchors.bottom: parent.bottom
        anchors.leftMargin: Theme.toastMarginLeft
        anchors.bottomMargin: Theme.toastMarginBottom
        spacing: Theme.toastSpacing

        Repeater {
            model: root.controller.toastModel
            delegate: Rectangle {
                id: toast
                required property int index
                required property int severityKind
                required property var peers
                required property string message
                required property bool dismissing
                property bool entered: false
                readonly property color tintColor:
                    toast.severityKind === 0 ? Theme.connectedBackground
                    : toast.severityKind === 1 ? Theme.errorBackground
                    : toast.severityKind === 2 ? Theme.warningBackground
                                               : Theme.brandBlueLight
                readonly property color accentColor:
                    toast.severityKind === 0 ? Theme.connectedText
                    : toast.severityKind === 1 ? Theme.errorText
                    : toast.severityKind === 2 ? Theme.warningText
                                               : Theme.brandBlue
                objectName: "toastBase_" + index
                width: Math.min(Math.max(1, root.width - Theme.toastMarginLeft * 2),
                                Math.min(420, Math.max(peers.length > 0 ? 280 : 1,
                                    Math.ceil(toastMetrics.advanceWidth + 28))))
                height: toastContent.implicitHeight + 20
                radius: Theme.toastRadius
                color: Theme.toastBackground
                opacity: entered && !dismissing ? 1 : 0

                Rectangle {
                    objectName: "toastTint_" + toast.index
                    anchors.fill: parent
                    radius: Theme.toastRadius
                    color: toast.tintColor
                    border.width: 1
                    border.color: toast.accentColor
                }

                TextMetrics {
                    id: toastMetrics
                    text: toast.message
                    font: toastText.font
                }

                ColumnLayout {
                    id: toastContent
                    anchors.left: parent.left
                    anchors.right: parent.right
                    anchors.verticalCenter: parent.verticalCenter
                    anchors.leftMargin: 14
                    anchors.rightMargin: 14
                    spacing: 7

                    NotificationPeers {
                        Layout.fillWidth: true
                        controller: root.controller
                        peers: toast.peers
                        textColor: toast.accentColor
                    }
                    Text {
                        id: toastText
                        objectName: "toastText_" + toast.index
                        Layout.fillWidth: true
                        text: toast.message
                        textFormat: Text.PlainText
                        color: toast.accentColor
                        font.pixelSize: Theme.toastTextSize
                        font.bold: true
                        wrapMode: Text.Wrap
                        verticalAlignment: Text.AlignVCenter
                    }
                }

                transform: Translate {
                    y: !toast.entered ? Theme.toastSlideDistance
                                      : (toast.dismissing
                                         ? Theme.toastSlideDistance / 2 : 0)
                    Behavior on y {
                        NumberAnimation {
                            duration: Theme.toastAnimationDuration
                            easing.type: Easing.OutQuad
                        }
                    }
                }
                Behavior on opacity {
                    NumberAnimation {
                        duration: Theme.toastAnimationDuration
                        easing.type: Easing.OutQuad
                    }
                }
                Behavior on y {
                    NumberAnimation {
                        duration: Theme.toastAnimationDuration
                        easing.type: Easing.OutQuad
                    }
                }
                Component.onCompleted: Qt.callLater(function() {
                    toast.entered = true
                })
            }
        }
    }
}
