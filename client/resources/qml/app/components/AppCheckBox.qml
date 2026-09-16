import QtQuick
import QtQuick.Controls.Basic
import Mouffette.App

CheckBox {
    id: control

    property color textColor: control.enabled ? Theme.text : Theme.disabledText
    property color checkedColor: control.enabled ? Theme.accent : Theme.buttonDisabled
    property color uncheckedColor: "transparent"
    property color uncheckedBorderColor: control.enabled ? Theme.controlBorder : Theme.controlDisabledBorder
    property color checkmarkColor: control.enabled ? Theme.onAccent : Theme.disabledText

    palette: Theme.controlPalette

    spacing: 7
    indicator: Rectangle {
        implicitWidth: 16
        implicitHeight: 16
        x: control.leftPadding
        y: (control.height - height) / 2
        radius: 3
        color: control.checked ? control.checkedColor : control.uncheckedColor
        border.width: 1
        border.color: control.visualFocus ? Theme.focusBorder
                      : control.checked && control.enabled ? control.checkedColor : control.uncheckedBorderColor

        Text {
            anchors.centerIn: parent
            visible: control.checked
            text: "✓"
            color: control.checkmarkColor
            font.pixelSize: 12
            font.bold: true
        }
    }
    contentItem: Text {
        leftPadding: control.indicator.width + control.spacing
        text: control.text
        color: control.textColor
        font.pixelSize: 14
        verticalAlignment: Text.AlignVCenter
    }
}
