import QtQuick 2.15

Item {
    id: root

    property real mediaX: 0
    property real mediaY: 0
    property real mediaWidth: 0
    property real mediaHeight: 0
    property real mediaScale: 1.0
    property real mediaZ: 0
    property bool selected: false
    property string mediaId: ""
    property string imageSource: ""

    signal selectRequested(string mediaId, bool additive)

    x: mediaX
    y: mediaY
    width: mediaWidth
    height: mediaHeight
    scale: mediaScale
    transformOrigin: Item.TopLeft
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
        // mipmap is beneficial for downscaling only; at high zoom (upscaling) it wastes
        // GPU memory on a full mip chain and can cause allocation failures → black render.
        mipmap: false
    }

    MouseArea {
        anchors.fill: parent
        scrollGestureEnabled: false
        onPressed: function(mouse) {
            var additive = (mouse.modifiers & Qt.ShiftModifier) !== 0
            var shouldSelect = additive || !root.selected
            if (shouldSelect)
                root.selectRequested(root.mediaId, additive)
            mouse.accepted = shouldSelect
        }
    }
}
