import QtQuick
import QtQuick.Controls
import Mouffette.App

TextField {
    id: control
    color: Theme.text
    selectionColor: Theme.brandBlue
    selectedTextColor: "white"
    placeholderTextColor: Theme.mutedText
    font.pixelSize: 14
    leftPadding: 10
    rightPadding: 10
    background: Rectangle {
        implicitHeight: 34
        radius: Theme.controlRadius
        color: Theme.interactionBackground
        border.width: 1
        border.color: control.activeFocus ? Theme.brandBlue : Theme.border
    }
}
