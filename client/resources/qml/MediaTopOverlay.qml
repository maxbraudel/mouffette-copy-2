import QtQuick 2.15

// Top overlay panel attached above a selected media item.
//   Row 1: media name pill (full width, adaptive with ellipsis)
//   Row 2: show/hide | bring-forward | bring-backward | delete
// Container width is driven by the buttons row.
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

    readonly property real itemSpacing: 4
    readonly property real btnSize: 36
    readonly property real namePillHeight: btnSize

    // panelWidth/panelHeight exposed so CanvasRoot can read them for centering.
    readonly property real panelWidth: implicitWidth
    readonly property real panelHeight: implicitHeight

    // Width = buttons row natural width (no outer padding).
    // Height = name pill (if present) + gap + buttons row.
    implicitWidth:  btnRow.implicitWidth
    implicitHeight: (displayName.length > 0 ? namePillHeight + itemSpacing : 0) + btnSize

    // Row 1: media name in a styled pill.
    // Stretches to the full container width (= buttons row width).
    Rectangle {
        id: namePill
        visible: root.displayName.length > 0
        x: 0
        y: 0
        width: root.width
        height: root.namePillHeight
        radius: 6
        color: "#CC1e2535"
        border.color: "#40FFFFFF"
        border.width: 1

        MouseArea {
            anchors.fill: parent
            acceptedButtons: Qt.LeftButton | Qt.RightButton
            onPressed: mouse.accepted = true
            onReleased: mouse.accepted = true
        }

        Text {
            anchors.verticalCenter: parent.verticalCenter
            anchors.left: parent.left
            anchors.right: parent.right
            text: root.displayName
            color: "#FFFFFF"
            font.pixelSize: 16
            elide: Text.ElideRight
            maximumLineCount: 1
            horizontalAlignment: Text.AlignHCenter
        }
    }

    // Row 2: action buttons — defines the container width
    Row {
        id: btnRow
        x: 0
        y: (root.displayName.length > 0 ? root.namePillHeight + root.itemSpacing : 0)
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
