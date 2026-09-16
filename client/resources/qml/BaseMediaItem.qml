import QtQuick
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
    // Passive renderers use this to report when the visual can be revealed.
    property bool contentReady: true
    property bool initialFramePresented: true

    property bool pointerEnabled: true
    signal primaryPressed(string mediaId, bool additive)

    x: mediaX
    y: mediaY
    width: mediaWidth
    height: mediaHeight
    scale: mediaScale
    transformOrigin: Item.TopLeft
    z: mediaZ

    default property alias content: contentHost.data

    Item {
        id: contentHost
        anchors.fill: parent
    }

    // Single-press handling for selection/primary press.
    // Double-click is handled by the viewport's passive TapHandler so it can
    // coexist with the viewport handlers used for selection/drag arbitration.
    MouseArea {
        anchors.fill: parent
        enabled: root.pointerEnabled
        acceptedButtons: Qt.LeftButton
        scrollGestureEnabled: false

        onPressed: function(mouse) {
            var additive = (mouse.modifiers & Qt.ShiftModifier) !== 0
            root.primaryPressed(root.mediaId, additive)
            mouse.accepted = true
        }
    }
}
