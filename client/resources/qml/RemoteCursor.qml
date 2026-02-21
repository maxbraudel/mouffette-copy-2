import QtQuick 2.15

Rectangle {
    id: root

    property bool cursorVisible: false
    property real cursorX: 0
    property real cursorY: 0
    property int diameter: 30
    property color fillColor: "#FFFFFFFF"
    property color borderColor: "#E6000000"
    property real borderWidth: 2

    visible: cursorVisible
    x: cursorX - diameter / 2
    y: cursorY - diameter / 2
    width: diameter
    height: diameter
    radius: diameter / 2
    z: 11500

    color: fillColor
    border.color: borderColor
    border.width: borderWidth
}
