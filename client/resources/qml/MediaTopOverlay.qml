import QtQuick
// Top overlay panel attached above a selected media item.
//   Row 1: show/hide | bring-forward | bring-backward | delete
//   Row 2: media name pill (full width, adaptive with ellipsis)
// Container width is driven by the buttons row.
Item {
    id: root
    objectName: "mediaTopOverlay"

    property string mediaId: ""
    property string displayName: ""
    property bool contentVisible: true
    property bool actionsAvailable: true

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
    implicitHeight: (displayName.length > 0 ? namePillHeight : 0)
                    + (actionsAvailable ? itemSpacing + btnSize : 0)

    // Row 2: media name in a styled pill, closest to the media.
    // Stretches to the full container width (= buttons row width).
    MediaNamePill {
        id: namePill
        visible: root.displayName.length > 0
        x: 0
        y: root.actionsAvailable ? root.btnSize + root.itemSpacing : 0
        width: root.width
        height: root.namePillHeight
        displayName: root.displayName

        MouseArea {
            anchors.fill: parent
            acceptedButtons: Qt.LeftButton | Qt.RightButton
            onPressed: mouse.accepted = true
            onReleased: mouse.accepted = true
        }

    }

    // Row 1: action buttons — defines the container width
    Row {
        id: btnRow
        visible: root.actionsAvailable
        enabled: root.actionsAvailable
        x: 0
        y: 0
        spacing: root.itemSpacing

        OverlayButton {
            iconSource: root.contentVisible
                ? "qrc:/icons/icons/visibility-on.svg"
                : "qrc:/icons/icons/visibility-off.svg"
            isToggle: true
            toggled: !root.contentVisible
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
