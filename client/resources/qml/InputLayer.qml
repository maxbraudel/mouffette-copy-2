import QtQuick 2.15

Item {
    id: inputLayer

    property var interactionController: null
    property bool textToolActive: false
    default property alias layerChildren: layerRoot.data

    signal textCreateRequested(real viewX, real viewY)

    Item {
        id: layerRoot
        anchors.fill: parent
    }
}
