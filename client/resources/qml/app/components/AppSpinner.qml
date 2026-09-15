import QtQuick
import Mouffette.App
import Mouffette.Canvas

Item {
    id: root

    property color color: Theme.brandBlue
    property bool running: visible
    property int lineCount: 12

    implicitWidth: 48
    implicitHeight: 48

    Repeater {
        model: root.lineCount
        delegate: Rectangle {
            required property int index
            x: root.width / 2 - width / 2
            y: 2
            width: Math.max(2, root.width / 9)
            height: Math.max(6, root.height / 4)
            radius: width / 2
            color: root.color
            opacity: 0.18 + 0.82 * (index + 1) / root.lineCount
            transform: Rotation {
                origin.x: width / 2
                origin.y: root.height / 2 - y
                angle: index * 360 / root.lineCount
            }
        }
    }

    RotationAnimator on rotation {
        from: 0
        to: 360
        duration: UiTiming.spinnerRotationDurationMs
        loops: Animation.Infinite
        running: root.running
    }
}
