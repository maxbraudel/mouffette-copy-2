import QtQuick
import QtQuick.Controls
import Mouffette.App
import "../components"

AbstractButton {
    id: control

    enum Tone { Normal, Uploading, Uploaded, Remote, Test }
    property int tone: OverlayActionButton.Normal
    // Busy acknowledgements keep their blue fill even though input is locked.
    property bool busy: false
    property bool monospace: false
    property real bottomRadius: 0
    property string unavailableReason: ""
    property alias textVariants: textMetrics.textVariants
    property alias monospaceTextVariants: monospaceMetrics.textVariants
    readonly property bool dimmed: !enabled && !busy
    readonly property color foregroundColor: dimmed ? Theme.overlayDisabledText
        : tone === OverlayActionButton.Uploading ? Theme.brandBlue
        : tone === OverlayActionButton.Uploaded ? Theme.mediaUploaded
        : tone === OverlayActionButton.Remote || tone === OverlayActionButton.Test
          ? Theme.overlaySceneText
        : Theme.overlayText
    readonly property color backgroundColor: {
        if (dimmed) return Theme.overlayDisabledBackground
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

    implicitWidth: Math.max(textMetrics.maximumWidth, monospaceMetrics.maximumWidth) + leftPadding + rightPadding
    implicitHeight: Theme.overlayButtonHeight
    padding: 0
    leftPadding: 20
    rightPadding: 20
    hoverEnabled: true
    Accessible.role: Accessible.Button
    Accessible.name: text
    Accessible.description: control.enabled ? "" : unavailableReason
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
    contentItem: Text {
        id: label
        text: control.text
        textFormat: Text.PlainText
        color: control.foregroundColor
        font: control.monospace ? monospaceMetrics.font : textMetrics.font
        horizontalAlignment: Text.AlignHCenter
        verticalAlignment: Text.AlignVCenter
        elide: Text.ElideRight
    }
    background: Rectangle {
        color: control.backgroundColor
        bottomLeftRadius: control.bottomRadius
        bottomRightRadius: control.bottomRadius
    }
}
