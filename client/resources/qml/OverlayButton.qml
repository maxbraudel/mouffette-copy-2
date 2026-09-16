import QtQuick
import QtQuick.Controls
import QtQuick.Effects
import Mouffette.App
// Reusable overlay icon button.
// Blocks pointer events from reaching the canvas DragHandler/PointHandler beneath it.
Item {
    id: root

    property string iconSource: ""
    property bool   isToggle:    false
    property bool   toggled:     false
    property string accessibleName: ""
    property string unavailableReason: ""
    // Use Item.enabled; shadowing it leaves native input eligibility divergent.
    // "solo" | "leading" | "middle" | "trailing"
    // Controls which corners are rounded within a segmented control.
    property string segmentRole: "solo"

    signal clicked()

    implicitWidth: 36
    implicitHeight: 36

    readonly property real _r: 6
    readonly property bool hovered: pressArea.containsMouse
    readonly property color currentBackgroundColor: _bgColor

    Accessible.role: Accessible.Button
    Accessible.name: accessibleName
    Accessible.description: root.enabled ? "" : unavailableReason

    // Which sides are flat (square corners)
    readonly property bool _flatLeft:  segmentRole === "trailing" || segmentRole === "middle"
    readonly property bool _flatRight: segmentRole === "leading"  || segmentRole === "middle"

    // Resolved background fill color
    readonly property color _bgColor: {
        if (!root.enabled)
            return Theme.overlayDisabledBackground
        if (pressArea.containsPress)
            return Theme.overlayPressed
        if (root.isToggle && root.toggled)
            return Theme.overlaySelected
        if (pressArea.containsMouse)
            return Theme.overlayHover
        return Theme.overlayBackground
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
            border.color: Theme.overlayBorder
            border.width: 1
        }
    }

    // Explicit border lines on flat sides — drawn outside the clip so they are
    // always visible. These are the seam dividers between segmented buttons.
    Rectangle {
        visible: root._flatLeft
        anchors { left: parent.left; top: parent.top; bottom: parent.bottom }
        width: 1
        color: Theme.overlayBorder
    }
    Rectangle {
        visible: root._flatRight
        anchors { right: parent.right; top: parent.top; bottom: parent.bottom }
        width: 1
        color: Theme.overlayBorder
    }

    // SVG icon uses 60% of the button size.
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
        layer.enabled: true
        layer.effect: MultiEffect {
            colorization: 1
            colorizationColor: root.isToggle && root.toggled
                               ? Theme.accent : Theme.overlayText
        }
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
