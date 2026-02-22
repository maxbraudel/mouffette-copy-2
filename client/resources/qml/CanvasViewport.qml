import QtQuick 2.15

Item {
    id: viewport
    clip: true

    property real panX: 0.0
    property real panY: 0.0
    property real viewScale: 1.0
    default property alias viewportChildren: contentRoot.data
    readonly property alias contentRootItem: contentRoot

    Item {
        id: contentRoot
        width: viewport.width
        height: viewport.height
        x: viewport.panX
        y: viewport.panY
        scale: viewport.viewScale
        transformOrigin: Item.TopLeft
    }
}
