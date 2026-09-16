import QtQuick
import QtQuick.Controls
import Mouffette.App

AbstractButton {
    id: control

    property bool primary: false
    property bool destructive: false
    property string unavailableReason: ""
    property alias textVariants: textMetrics.textVariants

    implicitWidth: Math.max(Theme.controlMinWidth, textMetrics.maximumWidth + 24)
    implicitHeight: Theme.controlHeight
    hoverEnabled: true
    focusPolicy: Qt.TabFocus
    palette: Theme.controlPalette

    Accessible.role: Accessible.Button
    Accessible.name: text
    Accessible.description: control.enabled ? "" : unavailableReason

    StateTextMetrics {
        id: textMetrics
        text: control.text
        font: label.font
    }

    contentItem: Text {
        id: label
        text: control.text
        color: {
            if (!control.enabled)
                return Theme.disabledText
            if (control.destructive)
                return Theme.errorText
            return control.primary ? Theme.brandBlue : Theme.text
        }
        font.pixelSize: Theme.controlFontSize
        font.bold: true
        horizontalAlignment: Text.AlignHCenter
        verticalAlignment: Text.AlignVCenter
        elide: Text.ElideRight
    }

    background: Rectangle {
        radius: Theme.controlRadius
        color: {
            if (!control.enabled)
                return Theme.buttonDisabled
            if (control.destructive)
                return control.down ? Theme.destructivePressed
                     : control.hovered ? Theme.destructiveHover : Theme.destructiveBackground
            if (control.down)
                return control.primary ? Theme.primaryPressed : Theme.buttonPressed
            if (control.hovered)
                return control.primary ? Theme.primaryHover : Theme.buttonHover
            if (control.checked)
                return control.primary ? Theme.primaryPressed : Theme.buttonPressed
            return control.primary ? Theme.primaryBackground : Theme.buttonBackground
        }
        border.width: 1
        border.color: control.visualFocus ? Theme.focusBorder : Theme.border
    }

}
