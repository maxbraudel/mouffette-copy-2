import QtQuick
import QtQuick.Controls
import QtQuick.Effects
import Mouffette.App

AbstractButton {
    id: control

    property bool primary: false
    property bool destructive: false
    property string unavailableReason: ""
    property alias textVariants: textMetrics.textVariants
    property url iconSource: ""
    property bool iconOnly: false
    readonly property real textWidth: Math.max(Theme.controlMinWidth, textMetrics.maximumWidth + 24)
    readonly property color foregroundColor: !enabled ? Theme.disabledText
        : destructive ? Theme.errorText : primary ? Theme.brandBlue : Theme.text

    implicitWidth: iconOnly ? 32 : textWidth
    implicitHeight: Theme.controlHeight
    hoverEnabled: true
    focusPolicy: Qt.TabFocus
    palette: Theme.controlPalette

    Accessible.role: Accessible.Button
    Accessible.name: text
    Accessible.description: control.enabled ? "" : unavailableReason
    ToolTip.visible: iconOnly && (hovered || visualFocus)
    ToolTip.text: text
    ToolTip.delay: 400

    StateTextMetrics {
        id: textMetrics
        text: control.text
        font: label.font
    }

    contentItem: Item {
        Text {
            id: label
            anchors.fill: parent
            visible: !control.iconOnly
            text: control.text
            color: control.foregroundColor
            font.pixelSize: Theme.controlFontSize
            font.bold: true
            horizontalAlignment: Text.AlignHCenter
            verticalAlignment: Text.AlignVCenter
            elide: Text.ElideRight
        }
        Image {
            anchors.centerIn: parent
            visible: control.iconOnly
            width: 16
            height: 16
            source: control.iconSource
            sourceSize: Qt.size(width * 4, height * 4)
            fillMode: Image.PreserveAspectFit
            layer.enabled: control.iconOnly
            layer.effect: MultiEffect {
                contrast: -1
                brightness: 0.5
                colorization: 1
                colorizationColor: control.foregroundColor
            }
        }
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
