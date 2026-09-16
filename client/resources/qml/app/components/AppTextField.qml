import QtQuick
import QtQuick.Controls
import Mouffette.App

TextField {
    id: control
    palette: Theme.controlPalette
    color: enabled ? Theme.text : Theme.disabledText
    selectionColor: Theme.selectionBackground
    selectedTextColor: Theme.selectionText
    placeholderTextColor: Theme.mutedText
    font.pixelSize: 14
    leftPadding: 10
    rightPadding: 10
    background: Rectangle {
        implicitHeight: 34
        radius: Theme.controlRadius
        color: Theme.fieldBackground
        border.width: 1
        border.color: control.activeFocus ? Theme.focusBorder
                      : control.enabled ? Theme.fieldBorder : Theme.controlDisabledBorder
    }
}
