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

    property bool pointerEnabled: true
    property bool doubleClickEnabled: false

    signal selectRequested(string mediaId, bool additive)
    signal primaryPressed(string mediaId, bool additive)
    signal primaryDoubleClicked(string mediaId, bool additive)

    // Called by the parent delegate's TapHandler (which sits at a higher level
    // in the scene graph and receives events before child MouseAreas).
    function fireDoubleClick(additive) {
        if (root.doubleClickEnabled)
            root.primaryDoubleClicked(root.mediaId, !!additive)
    }

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

    // Single-press + double-click: both handled in the same MouseArea.
    // onDoubleClicked maps to WM_LBUTTONDBLCLK on Windows (the most reliable
    // path) and to QEvent::MouseButtonDblClick on all platforms — far more
    // robust than TapHandler when onPressed accepts the event exclusively.
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

        onDoubleClicked: function(mouse) {
            if (!root.doubleClickEnabled)
                return
            var additive = (mouse.modifiers & Qt.ShiftModifier) !== 0
            root.primaryDoubleClicked(root.mediaId, additive)
            mouse.accepted = true
        }
    }
}