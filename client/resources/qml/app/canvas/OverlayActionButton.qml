import QtQuick
import QtQuick.Controls
import Mouffette.App

AbstractButton {
    id: control

    enum Tone { Normal, Uploading, Uploaded, Remote, Test }
    property int tone: OverlayActionButton.Normal
    // Busy acknowledgements keep their blue fill even though input is locked.
    property bool busy: false
    property bool monospace: false
    property real bottomRadius: 0
    property string unavailableReason: ""
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

    implicitWidth: label.implicitWidth + 40
    implicitHeight: Theme.overlayButtonHeight
    padding: 0
    leftPadding: 20
    rightPadding: 20
    hoverEnabled: true
    Accessible.role: Accessible.Button
    Accessible.name: text
    Accessible.description: control.enabled ? "" : unavailableReason
    contentItem: Text {
        id: label
        text: control.text
        textFormat: Text.PlainText
        color: control.foregroundColor
        font.family: control.monospace
                     ? (Qt.platform.os === "osx" ? "Menlo" : "Courier New")
                     : control.font.family
        font.pixelSize: 14
        font.bold: true
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
