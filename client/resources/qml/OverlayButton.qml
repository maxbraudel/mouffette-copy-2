import QtQuick
import QtQuick.Controls
import QtQuick.Effects
import Mouffette.App as AppStyle
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
            return AppStyle.Theme.overlayDisabledBackground
        if (pressArea.containsPress)
            return AppStyle.Theme.overlayPressed
        if (root.isToggle && root.toggled)
            return AppStyle.Theme.overlaySelected
        if (pressArea.containsMouse)
            return AppStyle.Theme.overlayHover
        return AppStyle.Theme.overlayBackground
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
            border.color: AppStyle.Theme.overlayBorder
            border.width: 1
        }
    }

    // Each joint belongs to the segment on its right. Painting both flat
    // edges would put two adjacent one-pixel lines at every shared boundary.
    Rectangle {
        visible: root._flatLeft
        anchors { left: parent.left; top: parent.top; bottom: parent.bottom }
        width: 1
        color: AppStyle.Theme.overlayBorder
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
        layer.enabled: true
        layer.effect: MultiEffect {
            // Normalize the monochrome SVG to its alpha mask before tinting;
            // embedded source RGB must not darken the semantic icon color.
            contrast: -1
            brightness: 0.5
            colorization: 1
            colorizationColor: !root.enabled ? AppStyle.Theme.overlayDisabledText
                             : root.isToggle && root.toggled
                               ? AppStyle.Theme.accent : AppStyle.Theme.overlayText
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
