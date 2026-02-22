import QtQuick 2.15

Item {
    id: inputLayer

    property var interactionController: null
    property bool textToolActive: false

    signal textCreateRequested(real viewX, real viewY)

    TapHandler {
        id: textToolTap
        target: null
        enabled: !!inputLayer.interactionController
                 && inputLayer.interactionController.isInteractionIdle()

        acceptedButtons: Qt.LeftButton
        onTapped: function(eventPoint) {
            if (!inputLayer.textToolActive)
                return
            inputLayer.textCreateRequested(eventPoint.position.x, eventPoint.position.y)
        }
    }
}
