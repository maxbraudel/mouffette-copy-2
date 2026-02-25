import QtQuick 2.15

// Reusable overlay icon button matching the legacy OverlayButtonElement style.
// Blocks pointer events from reaching the canvas DragHandler/PointHandler beneath it.
Item {
    id: root

    property string iconSource: ""
    property bool   isToggle:    false
    property bool   toggled:     false
    property bool   enabled:     true
    // "solo" | "leading" | "middle" | "trailing"
    // Controls which corners are rounded, matching legacy SegmentRole behavior.
    property string segmentRole: "solo"

    signal clicked()

    implicitWidth: 36
    implicitHeight: 36

    readonly property real _r: 6

    // Which sides are flat (square corners)
    readonly property bool _flatLeft:  segmentRole === "trailing" || segmentRole === "middle"
    readonly property bool _flatRight: segmentRole === "leading"  || segmentRole === "middle"

    // Resolved background fill color
    readonly property color _bgColor: {
        if (!root.enabled)
            return "#61323232"
        if ((root.isToggle && root.toggled) || pressArea.containsPress)
            return "#F2345780"
        return "#F2323232"
    }

    // Clipping container — clips away the rounded corners that should be flat.
    // The bg rect is oversized/shifted so those corners extend outside the clip.
    Item {
        id: bgClip
        anchors.fill: parent
        clip: true

        Rectangle {
            id: bg
            // Shift/extend so rounded corners on flat sides are pushed outside clip.
            x:      root._flatLeft  ? -root._r : 0
            y:      0
            width:  root.width
                    + (root._flatLeft  ? root._r : 0)
                    + (root._flatRight ? root._r : 0)
            height: root.height
            radius: root._r
            color:  root._bgColor
            border.color: "#FF646464"
            border.width: 1
            Behavior on color { ColorAnimation { duration: 80 } }
        }
    }

    // Explicit border lines on flat sides — drawn outside the clip so they are
    // always visible. These are the seam dividers between segmented buttons.
    Rectangle {
        visible: root._flatLeft
        anchors { left: parent.left; top: parent.top; bottom: parent.bottom }
        width: 1
        color: "#FF646464"
    }
    Rectangle {
        visible: root._flatRight
        anchors { right: parent.right; top: parent.top; bottom: parent.bottom }
        width: 1
        color: "#FF646464"
    }

    // SVG icon — 60% of button size matches legacy OverlayButtonElement (buttonSize * 0.6).
    // sourceSize at 4× for sharp rendering on HiDPI displays.
    Image {
        id: icon
        anchors.centerIn: parent
        width: Math.round(root.width * 0.6)
        height: Math.round(root.height * 0.6)
        source: root.iconSource
        sourceSize: Qt.size(width * 4, height * 4)
        fillMode: Image.PreserveAspectFit
        smooth: true
        mipmap: true
        opacity: root.enabled ? 1.0 : 0.35
    }

    // Input capture — blocks drag/pan handlers beneath
    MouseArea {
        id: pressArea
        anchors.fill: parent
        hoverEnabled: true
        enabled: root.enabled
        acceptedButtons: Qt.LeftButton
        onClicked: {
            root.clicked()
        }
        // Consume the event so it never reaches the canvas DragHandler
        onPressed: function(mouse) { mouse.accepted = true }
        onReleased: function(mouse) { mouse.accepted = true }
    }
}
