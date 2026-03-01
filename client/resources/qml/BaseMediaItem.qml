import QtQuick 2.15

Item {
    id: root

    function debugInputLog() {
        console.warn("[QuickCanvas][InputDebug][BaseMediaItem]",
                     "mediaId=", root.mediaId,
                     "pointerEnabled=", root.pointerEnabled,
                     "selected=", root.selected,
                     "x=", root.x,
                     "y=", root.y,
                     "w=", root.width,
                     "h=", root.height)
    }

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

    // Single-press handling for selection/primary press.
    // Double-click is handled at delegate level (CanvasRoot mediaDoubleClick)
    // and forwarded via fireDoubleClick() so it can coexist with parent
    // PointerHandlers used for selection/drag arbitration.
    MouseArea {
        anchors.fill: parent
        enabled: root.pointerEnabled
        acceptedButtons: Qt.LeftButton
        scrollGestureEnabled: false

        onPressed: function(mouse) {
            var additive = (mouse.modifiers & Qt.ShiftModifier) !== 0
            root.debugInputLog()
            console.warn("[QuickCanvas][InputDebug][BaseMediaItem] onPressed",
                         "mouse=", mouse.x, mouse.y,
                         "button=", mouse.button,
                         "modifiers=", mouse.modifiers,
                         "additive=", additive)
            root.primaryPressed(root.mediaId, additive)
            mouse.accepted = true
        }

        onPositionChanged: function(mouse) {
            if (!pressed)
                return
            console.warn("[QuickCanvas][InputDebug][BaseMediaItem] onPositionChanged",
                         "mouse=", mouse.x, mouse.y,
                         "mediaId=", root.mediaId,
                         "buttons=", mouse.buttons)
        }

        onReleased: function(mouse) {
            console.warn("[QuickCanvas][InputDebug][BaseMediaItem] onReleased",
                         "mouse=", mouse.x, mouse.y,
                         "mediaId=", root.mediaId)
        }

        onCanceled: {
            console.warn("[QuickCanvas][InputDebug][BaseMediaItem] onCanceled",
                         "mediaId=", root.mediaId)
        }
    }
}