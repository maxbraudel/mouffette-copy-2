import QtQuick 2.15

// Horizontal slider matching the legacy OverlaySliderElement style.
// Track + fill + draggable thumb. Value range [0, 1].
// Blocks pointer events from reaching canvas handlers beneath it.
//
// Binding contract:
//   'progress' is the authoritative source of truth supplied by the parent.
//   This component NEVER writes to 'progress', so no declarative parent binding
//   is ever destroyed (Qt6 QML property binding destruction is irreversible).
//
//   '_visualValue' is the internal display position. A Binding element keeps it
//   in sync with 'progress' during idle. While the user drags, '_dragging' is
//   set to true, which suspends that Binding and lets mouse handlers control
//   '_visualValue' freely. When the drag ends '_dragging' returns to false and
//   the Binding (with RestoreBinding) re-evaluates from 'progress' immediately.
//
// Signals:
//   dragStarted(ratio) — pointer pressed; begin scrub, C++ should lock drag state
//   seeked(ratio)      — pointer moved during drag (frequent, use for live preview)
//   dragEnded(ratio)   — pointer released; commit final position, unlock drag state
Item {
    id: root

    // --- Public API ---

    // Authoritative displayed position [0..1].
    // Set this via a declarative binding from the parent. Never write to it here.
    property real progress: 0.0

    // Whether this slider accepts pointer input.
    property bool enabled: true

    // --- Signals ---
    signal dragStarted(real ratio)
    signal seeked(real ratio)
    signal dragEnded(real ratio)

    // --- Internal state ---

    // True while the user holds the mouse button down.
    property bool _dragging: false

    // Visual display position. Tracks 'progress' at rest; overrides during drag.
    // All visuals (fill, thumb) are driven by this property — never by 'progress' directly.
    property real _visualValue: 0.0

    // Keep _visualValue synchronised with the incoming 'progress' value whenever
    // the user is NOT actively dragging. restoreMode: RestoreBinding ensures the
    // binding engine re-evaluates the expression (not just restores a cached literal)
    // after each drag ends — critical for correct post-seek progress display.
    Binding {
        target: root
        property: "_visualValue"
        value: root.progress
        when: !root._dragging
        restoreMode: Binding.RestoreBinding
    }

    implicitWidth: 120
    implicitHeight: 16

    // Track background — gOverlayBorderColor = QColor(100,100,100,255)
    Rectangle {
        id: track
        anchors.verticalCenter: parent.verticalCenter
        anchors.left: parent.left
        anchors.right: parent.right
        height: 4
        radius: 2
        color: "#FF646464"
    }

    // Fill — gOverlayActiveBackgroundColor = QColor(52,87,128,240)
    Rectangle {
        id: fill
        anchors.verticalCenter: track.verticalCenter
        anchors.left: track.left
        width: Math.max(0, Math.min(track.width, root._visualValue * track.width))
        height: track.height
        radius: track.radius
        color: "#F2345780"
    }

    // Thumb
    Rectangle {
        id: thumb
        width: 12
        height: 12
        radius: 6
        color: "#FFFFFF"
        anchors.verticalCenter: track.verticalCenter
        x: Math.max(0, Math.min(track.width - width, root._visualValue * track.width - width * 0.5))
    }

    // Input capture
    MouseArea {
        id: sliderArea
        anchors.fill: parent
        enabled: root.enabled
        acceptedButtons: Qt.LeftButton
        hoverEnabled: true

        function ratioFromX(mx) {
            return Math.max(0.0, Math.min(1.0, mx / track.width))
        }

        onPressed: function(mouse) {
            mouse.accepted = true
            var r = ratioFromX(mouse.x)
            root._dragging = true
            root._visualValue = r
            root.dragStarted(r)
            root.seeked(r)
        }

        onPositionChanged: function(mouse) {
            if (!pressed) return
            var r = ratioFromX(mouse.x)
            root._visualValue = r
            root.seeked(r)
        }

        onReleased: function(mouse) {
            mouse.accepted = true
            var finalRatio = root._visualValue
            root.dragEnded(finalRatio)
            root._dragging = false
            // Binding { when: !_dragging } reactivates here and re-syncs
            // _visualValue with the next incoming 'progress' value.
        }
    }
}
