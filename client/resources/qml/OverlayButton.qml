import QtQuick 2.15

// Reusable overlay icon button matching the legacy OverlayButtonElement style.
// Blocks pointer events from reaching the canvas DragHandler/PointHandler beneath it.
Item {
    id: root

    property string iconSource: ""
    property bool isToggle: false
    property bool toggled: false
    property bool enabled: true

    signal clicked()

    implicitWidth: 36
    implicitHeight: 36

    // Background pill
    Rectangle {
        id: bg
        anchors.fill: parent
        radius: 6
        // Colors match AppColors: gOverlayBackgroundColor = QColor(50,50,50,240)
        //                          gOverlayActiveBackgroundColor = QColor(52,87,128,240)
        //                          gOverlayBorderColor = QColor(100,100,100,255)
        color: {
            if (!root.enabled)
                return "#61323232"  // base at 35% alpha
            if ((root.isToggle && root.toggled) || pressArea.containsPress)
                return "#F2345780"  // gOverlayActiveBackgroundColor
            return "#F2323232"      // gOverlayBackgroundColor (hover = same as idle)
        }
        border.color: "#FF646464"  // gOverlayBorderColor
        border.width: 1

        Behavior on color { ColorAnimation { duration: 80 } }
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
        onPressed: mouse.accepted = true
        onReleased: mouse.accepted = true
    }
}
