import QtQuick 2.15

Item {
    id: root

    property real mediaX: 0
    property real mediaY: 0
    property real mediaWidth: 0
    property real mediaHeight: 0
    property real mediaZ: 0
    property bool selected: false
    property string mediaId: ""
    property string imageSource: ""

    signal selectRequested(string mediaId, bool additive)

    x: mediaX
    y: mediaY
    width: mediaWidth
    height: mediaHeight
    z: mediaZ

    Rectangle {
        anchors.fill: parent
        color: "#141a24"
        visible: image.status !== Image.Ready
    }

    Image {
        id: image
        anchors.fill: parent
        source: root.imageSource
        fillMode: Image.PreserveAspectFit
        smooth: true
        asynchronous: true
        mipmap: true
    }

    Rectangle {
        anchors.fill: parent
        color: "transparent"
        border.width: root.selected ? 2 : 0
        border.color: "#4A90E2"
    }

    MouseArea {
        anchors.fill: parent
        scrollGestureEnabled: false
        onPressed: function(mouse) {
            root.selectRequested(root.mediaId, (mouse.modifiers & Qt.ShiftModifier) !== 0)
            mouse.accepted = true
        }
    }
}
