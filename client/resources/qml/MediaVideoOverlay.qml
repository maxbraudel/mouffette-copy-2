import QtQuick
import QtQuick.Layouts
import Mouffette.App as AppStyle
// Timeline controls video transport. This overlay edits its intrinsic audio state.
Item {
    id: root

    // --- Public API ---
    property string mediaId: ""
    property bool isMuted: false
    property real volume: 1.0      // 0..1

    // --- Signals ---
    signal muteToggleRequested(string mediaId)
    signal volumeChangeRequested(string mediaId, real value)

    signal overlayHoveredChanged(bool hovered)

    // --- Layout constants ---
    readonly property real itemSpacing:   4
    readonly property real btnSize:       36
    readonly property real sliderMinWidth: 120
    readonly property real chromeRadius: 6
    readonly property color chromeBackground: AppStyle.Theme.overlayBackground
    readonly property color chromeBorder: AppStyle.Theme.overlayBorder

    // panelWidth/panelHeight exposed so CanvasRoot can read them for centering.
    readonly property real panelWidth:  implicitWidth
    readonly property real panelHeight: implicitHeight

    // Width = controls row natural width (no outer padding).
    // Audio controls occupy one row.
    implicitWidth:  controlsRow.implicitWidth
    implicitHeight: btnSize

    // ---- Controls row ----
    // Buttons are fixed square; volume slider expands to fill remaining space.
    RowLayout {
        id: controlsRow
        x: 0
        y: 0
        width: root.width
        spacing: root.itemSpacing

        OverlayButton {
            objectName: "videoMuteButton"
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
                objectName: "videoVolumeSlider"
                progress: root.volume
                anchors.fill: parent
                anchors.leftMargin: 8
                anchors.rightMargin: 8
                anchors.topMargin: 0
                anchors.bottomMargin: 0
                onDragStarted: function(r) { root.volumeChangeRequested(root.mediaId, r) }
                onSeeked:    function(r) { root.volumeChangeRequested(root.mediaId, r) }
                onDragEnded: function(r) { root.volumeChangeRequested(root.mediaId, r) }
            }
        }
    }

}
