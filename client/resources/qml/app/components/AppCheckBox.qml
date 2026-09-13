import QtQuick
import QtQuick.Controls
import Mouffette.App

CheckBox {
    id: control

    spacing: 7
    indicator: Rectangle {
        implicitWidth: 16
        implicitHeight: 16
        x: control.leftPadding
        y: (control.height - height) / 2
        radius: 3
        color: control.checked ? Theme.brandBlue : "transparent"
        border.width: 1
        border.color: control.checked ? Theme.brandBlue : Theme.border

        Text {
            anchors.centerIn: parent
            visible: control.checked
            text: "✓"
            color: "white"
            font.pixelSize: 12
            font.bold: true
        }
    }
    contentItem: Text {
        leftPadding: control.indicator.width + control.spacing
        text: control.text
        color: control.enabled ? Theme.text : Theme.disabledText
        font.pixelSize: 14
        verticalAlignment: Text.AlignVCenter
    }
}
