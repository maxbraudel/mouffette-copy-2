import QtQuick 2.15
import QtQuick.Layouts 1.15

// Bottom overlay panel attached below a selected video media item.
// Mirrors the legacy ResizableVideoItem controls panel:
//   Row 1: play/pause | stop | repeat | mute | volume slider
//   Row 2: progress bar (full width)
// Positioned in item-local coordinates: y = parent.height + gap
//
// Seek protocol (three-phase to match C++ drag-lock pattern):
//   seekBeginRequested  — pointer pressed on progress bar (C++ sets m_draggingProgress=true)
//   seekUpdateRequested — pointer moved during drag (live preview, fires frequently)
//   seekEndRequested    — pointer released (C++ clears m_draggingProgress, commits position)
Item {
    id: root

    // --- Public API ---
    property string mediaId: ""
    property bool isPlaying: false
    property bool isMuted: false
    property bool isLooping: false
    property real progress: 0.0    // 0..1, authoritative playback position from C++
    property real volume: 1.0      // 0..1

    // --- Signals ---
    signal playPauseRequested(string mediaId)
    signal stopRequested(string mediaId)
    signal repeatToggleRequested(string mediaId)
    signal muteToggleRequested(string mediaId)
    signal volumeChangeRequested(string mediaId, real value)

    // Three-phase seek protocol — each maps to a dedicated C++ handler so that
    // the drag-in-progress flag (m_draggingProgress) is set/cleared correctly and
    // C++ never pushes conflicting position updates while the user is scrubbing.
    signal seekBeginRequested(string mediaId, real ratio)   // drag start (lock)
    signal seekUpdateRequested(string mediaId, real ratio)  // drag move  (live preview)
    signal seekEndRequested(string mediaId, real ratio)     // drag end   (commit + unlock)

    signal overlayHoveredChanged(bool hovered)

    // --- Layout constants ---
    readonly property real itemSpacing:   4
    readonly property real btnSize:       36
    readonly property real sliderMinWidth: 120
    readonly property real progressHeight: btnSize
    readonly property real chromeRadius: 6
    readonly property color chromeBackground: "#F2323232"
    readonly property color chromeBorder: "#FF646464"

    // panelWidth/panelHeight exposed so CanvasRoot can read them for centering.
    readonly property real panelWidth:  implicitWidth
    readonly property real panelHeight: implicitHeight

    // Width = controls row natural width (no outer padding).
    // Height = buttons row + gap + progress bar.
    implicitWidth:  controlsRow.implicitWidth
    implicitHeight: btnSize + itemSpacing + progressHeight

    // ---- Controls row ----
    // Buttons are fixed square; volume slider expands to fill remaining space.
    RowLayout {
        id: controlsRow
        x: 0
        y: 0
        width: root.width
        spacing: root.itemSpacing

        OverlayButton {
            iconSource: root.isPlaying ? "qrc:/icons/icons/pause.svg" : "qrc:/icons/icons/play.svg"
            Layout.preferredWidth: root.btnSize
            Layout.preferredHeight: root.btnSize
            onClicked: root.playPauseRequested(root.mediaId)
        }
        OverlayButton {
            iconSource: "qrc:/icons/icons/stop.svg"
            Layout.preferredWidth: root.btnSize
            Layout.preferredHeight: root.btnSize
            onClicked: root.stopRequested(root.mediaId)
        }
        OverlayButton {
            iconSource: "qrc:/icons/icons/loop.svg"
            isToggle: true
            toggled: root.isLooping
            Layout.preferredWidth: root.btnSize
            Layout.preferredHeight: root.btnSize
            onClicked: root.repeatToggleRequested(root.mediaId)
        }
        OverlayButton {
            iconSource: root.isMuted ? "qrc:/icons/icons/volume-off.svg" : "qrc:/icons/icons/volume-on.svg"
            isToggle: true
            toggled: root.isMuted
            Layout.preferredWidth: root.btnSize
            Layout.preferredHeight: root.btnSize
            onClicked: root.muteToggleRequested(root.mediaId)
        }
        // Volume slider: minimum width, expands to fill remaining space.
        // 'progress' (not 'value') is the binding-safe input property — see OverlaySlider.
        // Live volume update on every drag frame gives immediate audio feedback.
        Rectangle {
            Layout.fillWidth: true
            Layout.minimumWidth: root.sliderMinWidth
            Layout.preferredHeight: root.btnSize
            radius: root.chromeRadius
            color: root.chromeBackground
            border.color: root.chromeBorder
            border.width: 1

            OverlaySlider {
                id: volumeSlider
                progress: root.isMuted ? 0.0 : root.volume
                anchors.fill: parent
                anchors.leftMargin: 8
                anchors.rightMargin: 8
                anchors.topMargin: 0
                anchors.bottomMargin: 0
                onSeeked:    function(r) { root.volumeChangeRequested(root.mediaId, r) }
                onDragEnded: function(r) { root.volumeChangeRequested(root.mediaId, r) }
            }
        }
    }

    // ---- Progress bar ----
    // Full container width. Uses the three-phase seek protocol so C++ can set
    // the drag-lock flag before any seek call reaches the player.
    Rectangle {
        x: 0
        y: root.btnSize + root.itemSpacing
        width: root.width
        height: root.progressHeight
        radius: root.chromeRadius
        color: root.chromeBackground
        border.color: root.chromeBorder
        border.width: 1

        OverlaySlider {
            id: progressSlider
            progress: root.progress
            anchors.fill: parent
            anchors.leftMargin: 8
            anchors.rightMargin: 8
            anchors.topMargin: 0
            anchors.bottomMargin: 0
            onDragStarted: function(r) { root.seekBeginRequested(root.mediaId, r) }
            onSeeked:      function(r) { root.seekUpdateRequested(root.mediaId, r) }
            onDragEnded:   function(r) { root.seekEndRequested(root.mediaId, r) }
        }
    }
}
