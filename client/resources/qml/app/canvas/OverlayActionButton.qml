import QtQuick
import QtQuick.Controls
import Mouffette.App

AbstractButton {
    id: control

    enum Tone { Normal, Uploading, Uploaded, Remote, Test }
    property int tone: OverlayActionButton.Normal
    property string unavailableReason: ""

    implicitHeight: Theme.overlayButtonHeight
    hoverEnabled: true
    Accessible.role: Accessible.Button
    Accessible.name: text
    Accessible.description: control.enabled ? "" : unavailableReason
    ToolTip.visible: hovered && !enabled && unavailableReason.length > 0
    ToolTip.text: unavailableReason
    contentItem: Text {
        text: control.text
        color: control.tone === OverlayActionButton.Uploaded ? Theme.mediaUploaded
             : control.tone === OverlayActionButton.Uploading ? Theme.brandBlue
             : control.tone === OverlayActionButton.Remote || control.tone === OverlayActionButton.Test
               ? "#ff96ff" : Theme.overlayText
        font.pixelSize: 14
        font.bold: true
        horizontalAlignment: Text.AlignHCenter
        verticalAlignment: Text.AlignVCenter
        elide: Text.ElideRight
    }
    background: Rectangle {
        color: !control.enabled ? Qt.rgba(Theme.overlayBackground.r,
                                         Theme.overlayBackground.g,
                                         Theme.overlayBackground.b, 0.35)
             : control.down ? Qt.rgba(1, 1, 1, 0.10)
             : control.hovered ? Qt.rgba(1, 1, 1, 0.05)
             : control.tone === OverlayActionButton.Uploading ? Theme.primaryBackground
             : control.tone === OverlayActionButton.Uploaded ? Theme.connectedBackground
             : "transparent"
    }
}
