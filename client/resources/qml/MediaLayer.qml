import QtQuick 2.15

Item {
    id: mediaLayer
    default property alias layerChildren: layerRoot.data

    Item {
        id: layerRoot
        anchors.fill: parent
    }
}
