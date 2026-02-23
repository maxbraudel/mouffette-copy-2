import QtQuick 2.15
import QtQuick.Layouts 1.15

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

    // Measured size — parent positions us using this
    readonly property real panelWidth: Math.max(minWidth, contentColumn.width + paddingH * 2)
    readonly property real panelHeight: contentColumn.height + paddingV * 2

    readonly property real paddingH: 8
    readonly property real paddingV: 6
    readonly property real gap: 8
    readonly property real minWidth: 120
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
        anchors.top: parent.top
        anchors.topMargin: root.paddingV
        anchors.left: parent.left
        anchors.leftMargin: root.paddingH
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
