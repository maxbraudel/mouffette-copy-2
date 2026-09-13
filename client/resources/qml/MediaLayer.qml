import QtQuick
Item {
    id: mediaLayer
    default property alias layerChildren: layerRoot.data

    Item {
        id: layerRoot
        anchors.fill: parent
    }
}
