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

    // Fixed panel dimensions — must NOT depend on media item size.
    // 4 buttons × 26px + 3 gaps × 4px = 116px content; + 2×8 padding = 132px.
    // Label row adds height but not width (clamped to 160px, elided).
    readonly property real panelWidth: 148
    readonly property real panelHeight: displayName.length > 0 ? 72 : 46

    readonly property real paddingH: 8
    readonly property real paddingV: 6
    readonly property real itemSpacing: 4

    width: panelWidth
    height: panelHeight

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

    Column {
        id: contentColumn
        x: root.paddingH
        y: root.paddingV
        width: root.panelWidth - root.paddingH * 2
        spacing: root.itemSpacing

        // Row 1: filename label
        Text {
            id: nameLabel
            text: root.displayName
            color: "#FFFFFF"
            font.pixelSize: 11
            font.family: "system-ui, -apple-system, sans-serif"
            elide: Text.ElideRight
            maximumLineCount: 1
            width: Math.min(implicitWidth, 160)
            visible: root.displayName.length > 0
        }

        // Row 2: action buttons
        Row {
            spacing: root.itemSpacing

            OverlayButton {
                id: visibilityBtn
                iconSource: root.contentVisible
                    ? "qrc:/icons/icons/visibility-on.svg"
                    : "qrc:/icons/icons/visibility-off.svg"
                isToggle: true
                toggled: root.contentVisible
                implicitWidth: 26
                implicitHeight: 26
                onClicked: {
                    root.visibilityToggleRequested(root.mediaId, !root.contentVisible)
                }
            }

            OverlayButton {
                iconSource: "qrc:/icons/icons/arrow-up.svg"
                implicitWidth: 26
                implicitHeight: 26
                onClicked: root.bringForwardRequested(root.mediaId)
            }

            OverlayButton {
                iconSource: "qrc:/icons/icons/arrow-down.svg"
                implicitWidth: 26
                implicitHeight: 26
                onClicked: root.bringBackwardRequested(root.mediaId)
            }

            OverlayButton {
                iconSource: "qrc:/icons/icons/delete.svg"
                implicitWidth: 26
                implicitHeight: 26
                onClicked: root.deleteRequested(root.mediaId)
            }
        }
    }
}
