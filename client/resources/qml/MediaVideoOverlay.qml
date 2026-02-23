import QtQuick 2.15
import QtQuick.Layouts 1.15

// Bottom overlay panel attached below a selected video media item.
// Mirrors the legacy ResizableVideoItem controls panel:
//   Row 1: play/pause | stop | repeat | mute | volume slider
//   Row 2: progress bar (full width)
// Positioned in item-local coordinates: y = parent.height + gap
Item {
    id: root

    property string mediaId: ""
    property bool isPlaying: false
    property bool isMuted: false
    property bool isLooping: false
    property real progress: 0.0    // 0..1
    property real volume: 1.0      // 0..1

    signal playPauseRequested(string mediaId)
    signal stopRequested(string mediaId)
    signal repeatToggleRequested(string mediaId)
    signal muteToggleRequested(string mediaId)
    signal volumeChangeRequested(string mediaId, real value)
    signal seekRequested(string mediaId, real ratio)
    signal overlayHoveredChanged(bool hovered)

    readonly property real paddingH: 8
    readonly property real paddingV: 6
    readonly property real itemSpacing: 4
    readonly property real minWidth: 200

    readonly property real panelWidth: Math.max(minWidth, contentColumn.implicitWidth + paddingH * 2)
    readonly property real panelHeight: contentColumn.implicitHeight + paddingV * 2

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
        anchors.right: parent.right
        anchors.rightMargin: root.paddingH
        spacing: root.itemSpacing

        // Row 1: transport controls + volume
        Row {
            spacing: root.itemSpacing

            // Play / Pause
            OverlayButton {
                iconSource: root.isPlaying
                    ? "qrc:/icons/icons/pause.svg"
                    : "qrc:/icons/icons/play.svg"
                implicitWidth: 26
                implicitHeight: 26
                onClicked: root.playPauseRequested(root.mediaId)
            }

            // Stop
            OverlayButton {
                iconSource: "qrc:/icons/icons/stop.svg"
                implicitWidth: 26
                implicitHeight: 26
                onClicked: root.stopRequested(root.mediaId)
            }

            // Repeat toggle
            OverlayButton {
                iconSource: "qrc:/icons/icons/loop.svg"
                isToggle: true
                toggled: root.isLooping
                implicitWidth: 26
                implicitHeight: 26
                onClicked: root.repeatToggleRequested(root.mediaId)
            }

            // Mute toggle
            OverlayButton {
                iconSource: root.isMuted
                    ? "qrc:/icons/icons/volume-off.svg"
                    : "qrc:/icons/icons/volume-on.svg"
                isToggle: true
                toggled: root.isMuted
                implicitWidth: 26
                implicitHeight: 26
                onClicked: root.muteToggleRequested(root.mediaId)
            }

            // Volume slider
            OverlaySlider {
                id: volumeSlider
                value: root.isMuted ? 0.0 : root.volume
                implicitWidth: 60
                implicitHeight: 26
                onSeeked: function(r) {
                    root.volumeChangeRequested(root.mediaId, r)
                }
            }
        }

        // Row 2: progress bar (stretches full available width)
        OverlaySlider {
            id: progressSlider
            value: root.progress
            width: contentColumn.width
            implicitHeight: 16
            onDragStarted: function(r) {
                root.seekRequested(root.mediaId, r)
            }
            onSeeked: function(r) {
                root.seekRequested(root.mediaId, r)
            }
        }
    }
}
