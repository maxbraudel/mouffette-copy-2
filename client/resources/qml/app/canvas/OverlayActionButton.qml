import QtQuick
import QtQuick.Controls
import QtQuick.Effects
import Mouffette.App
import "../components"

AbstractButton {
    id: control

    enum Tone { Normal, Uploading, Uploaded, Remote, Test }
    property int tone: OverlayActionButton.Normal
    // Busy acknowledgements retain their blue fill while clicks explain the wait.
    property bool busy: false
    property bool cancelOnHover: false
    readonly property bool showingCancel: cancelOnHover && enabled && hovered
    property bool monospace: false
    property real bottomRadius: 0
    property string unavailableReason: ""
    property url iconSource: ""
    readonly property bool hasIcon: iconSource.toString().length > 0
    readonly property real iconLabelWidth: hasIcon ? 16 + 6 : 0
    property alias textVariants: textMetrics.textVariants
    property alias monospaceTextVariants: monospaceMetrics.textVariants
    readonly property bool dimmed: !enabled && !busy
    readonly property color foregroundColor: dimmed ? Theme.overlayDisabledText
        : showingCancel ? Theme.errorText
        : tone === OverlayActionButton.Uploading ? Theme.brandBlue
        : tone === OverlayActionButton.Uploaded ? Theme.mediaUploaded
        : tone === OverlayActionButton.Remote || tone === OverlayActionButton.Test
          ? Theme.overlaySceneText
        : Theme.overlayText
    readonly property color backgroundColor: {
        if (dimmed) return Theme.overlayDisabledBackground
        if (showingCancel)
            return down ? Theme.destructivePressed : Theme.destructiveHover
        if (tone === OverlayActionButton.Uploading)
            return enabled && down ? Theme.primaryPressed
                 : enabled && hovered ? Theme.primaryHover : Theme.primaryBackground
        if (tone === OverlayActionButton.Uploaded)
            return down ? Theme.overlayUploadedPressed
                 : hovered ? Theme.overlayUploadedHover : Theme.connectedBackground
        if (tone === OverlayActionButton.Remote || tone === OverlayActionButton.Test)
            return down ? Theme.overlayScenePressed
                 : hovered ? Theme.overlaySceneHover : Theme.overlaySceneBackground
        return down ? Theme.overlayPressed
             : hovered ? Theme.overlayHover : "transparent"
    }

    implicitWidth: Math.max(textMetrics.maximumWidth, monospaceMetrics.maximumWidth)
        + iconLabelWidth + leftPadding + rightPadding
    implicitHeight: Theme.overlayButtonHeight
    padding: 0
    leftPadding: 20
    rightPadding: 20
    hoverEnabled: true
    Accessible.role: Accessible.Button
    Accessible.name: text
    Accessible.description: unavailableReason
    StateTextMetrics {
        id: textMetrics
        text: control.monospace ? "" : control.text
        font.family: control.font.family
        font.pixelSize: 14
        font.bold: true
    }
    StateTextMetrics {
        id: monospaceMetrics
        text: control.monospace ? control.text : ""
        font.family: Theme.monospaceFontFamily
        font.pixelSize: 14
        font.bold: true
    }
    contentItem: Item {
        id: buttonContent
        readonly property real labelWidth: Math.min(label.implicitWidth,
            Math.max(0, width - control.iconLabelWidth))
        Row {
            anchors.centerIn: parent
            height: parent.height
            spacing: 6
            Image {
                anchors.verticalCenter: parent.verticalCenter
                visible: control.hasIcon
                width: 16; height: 16
                source: control.iconSource
                sourceSize: Qt.size(width * 4, height * 4)
                fillMode: Image.PreserveAspectFit
                layer.enabled: control.hasIcon
                layer.effect: MultiEffect {
                    contrast: -1
                    brightness: 0.5
                    colorization: 1
                    colorizationColor: control.foregroundColor
                }
            }
            Text {
                id: label
                width: buttonContent.labelWidth
                height: parent.height
                text: control.text
                textFormat: Text.PlainText
                color: control.foregroundColor
                font: control.monospace ? monospaceMetrics.font : textMetrics.font
                horizontalAlignment: Text.AlignHCenter
                verticalAlignment: Text.AlignVCenter
                elide: Text.ElideRight
            }
        }
    }
    background: Rectangle {
        color: control.backgroundColor
        bottomLeftRadius: control.bottomRadius
        bottomRightRadius: control.bottomRadius
    }
}
