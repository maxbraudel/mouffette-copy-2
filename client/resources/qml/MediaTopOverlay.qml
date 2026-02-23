import QtQuick 2.15

// Top overlay panel attached above a selected media item.
// Mirrors the legacy ResizableMediaBase top panel:
//   Row 1: filename label
//   Row 2: visibility toggle | bring-forward | bring-backward | delete
// Positioned in item-local coordinates: y = -height - gap
Item {
    id: root

    property string mediaId: ""
    property string displayName: ""
    property bool contentVisible: true

    signal visibilityToggleRequested(string mediaId, bool visible)
    signal bringForwardRequested(string mediaId)
    signal bringBackwardRequested(string mediaId)
    signal deleteRequested(string mediaId)
    signal overlayHoveredChanged(bool hovered)

    readonly property real paddingH: 8
    readonly property real paddingV: 6
    readonly property real itemSpacing: 4
    readonly property real btnSize: 28

    // panelWidth/panelHeight exposed so CanvasRoot can read them for centering.
    readonly property real panelWidth: implicitWidth
    readonly property real panelHeight: implicitHeight

    implicitWidth:  btnRow.implicitWidth + paddingH * 2
    implicitHeight: (displayName.length > 0 ? btnSize + itemSpacing + 16 : btnSize) + paddingV * 2

    // Background pill
    Rectangle {
        anchors.fill: parent
        radius: 8
        color: "#CC1e2535"
        border.color: "#40FFFFFF"
        border.width: 1
    }

    // Block all pointer events from reaching canvas below
    MouseArea {
        anchors.fill: parent
        acceptedButtons: Qt.LeftButton | Qt.RightButton
        hoverEnabled: true
        onPressed: mouse.accepted = true
        onReleased: mouse.accepted = true
        onEntered: root.overlayHoveredChanged(true)
        onExited: root.overlayHoveredChanged(false)
    }

    // Filename label — stretches to container width, only shown when name is set
    Text {
        id: nameLabel
        visible: root.displayName.length > 0
        text: root.displayName
        color: "#FFFFFF"
        font.pixelSize: 11
        elide: Text.ElideRight
        maximumLineCount: 1
        x: root.paddingH
        y: root.paddingV
        width: root.width - root.paddingH * 2
    }

    // Buttons row — defines the container width
    Row {
        id: btnRow
        x: root.paddingH
        y: root.paddingV + (root.displayName.length > 0 ? 16 + root.itemSpacing : 0)
        spacing: root.itemSpacing

        OverlayButton {
            iconSource: root.contentVisible
                ? "qrc:/icons/icons/visibility-on.svg"
                : "qrc:/icons/icons/visibility-off.svg"
            isToggle: true
            toggled: root.contentVisible
            implicitWidth: root.btnSize
            implicitHeight: root.btnSize
            onClicked: root.visibilityToggleRequested(root.mediaId, !root.contentVisible)
        }
        OverlayButton {
            iconSource: "qrc:/icons/icons/arrow-up.svg"
            implicitWidth: root.btnSize
            implicitHeight: root.btnSize
            onClicked: root.bringForwardRequested(root.mediaId)
        }
        OverlayButton {
            iconSource: "qrc:/icons/icons/arrow-down.svg"
            implicitWidth: root.btnSize
            implicitHeight: root.btnSize
            onClicked: root.bringBackwardRequested(root.mediaId)
        }
        OverlayButton {
            iconSource: "qrc:/icons/icons/delete.svg"
            implicitWidth: root.btnSize
            implicitHeight: root.btnSize
            onClicked: root.deleteRequested(root.mediaId)
        }
    }
}
