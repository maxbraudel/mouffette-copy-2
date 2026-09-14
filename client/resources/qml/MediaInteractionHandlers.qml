import QtQuick

// Delegate-local handlers are deliberately limited to selection and text
// activation. CanvasRoot owns movement through one viewport-level DragHandler,
// so renderer subtrees can never change or cancel the native move grab.
Item {
    id: interaction
    anchors.fill: parent

    property var rootController: null
    property var coordinatorRef: null
    property var delegateItem: null
    property var mediaContentItem: null

    readonly property var activeCoordinator: coordinatorRef
    readonly property bool mediaPressSelectEnabledState: !!rootController
        && !!delegateItem
        && !!delegateItem.media
        && !delegateItem.overlayHovered
        && !(mediaContentItem && mediaContentItem.editing === true)
        && !!activeCoordinator

    containmentMask: QtObject {
        function contains(p: point): bool {
            if (p.x < 0 || p.y < 0
                    || p.x >= interaction.width || p.y >= interaction.height)
                return false
            if (!interaction.activeCoordinator || !interaction.delegateItem)
                return false
            var scenePoint = interaction.mapToItem(null, p.x, p.y)
            return interaction.activeCoordinator.canObserveMediaAtScenePoint(
                interaction.delegateItem.currentMediaId,
                scenePoint.x, scenePoint.y)
        }
    }

    TapHandler {
        id: mediaDoubleClick
        target: null
        acceptedDevices: PointerDevice.Mouse | PointerDevice.TouchPad
        acceptedButtons: Qt.LeftButton
        grabPermissions: PointerHandler.ApprovesTakeOverByAnything
        enabled: interaction.mediaPressSelectEnabledState

        onTapped: function(eventPoint) {
            if (tapCount !== 2 || !activeCoordinator || !delegateItem)
                return
            var mediaId = delegateItem.currentMediaId
            var sceneX = eventPoint.scenePosition.x
            var sceneY = eventPoint.scenePosition.y
            if (!activeCoordinator.canActivateMediaAtScenePoint(
                    mediaId, sceneX, sceneY))
                return
            var item = mediaContentItem
            if (!item || typeof item.fireDoubleClick !== "function")
                return
            var additive = (eventPoint.modifiers & Qt.ShiftModifier) !== 0
            if (rootController
                    && typeof rootController.requestMediaSelection === "function")
                rootController.requestMediaSelection(mediaId, additive)
            Qt.callLater(function() {
                if (!delegateItem || delegateItem.currentMediaId !== mediaId)
                    return
                var currentItem = mediaContentItem
                if (currentItem
                        && typeof currentItem.fireDoubleClick === "function")
                    currentItem.fireDoubleClick(additive, sceneX, sceneY)
            })
        }
    }

    PointHandler {
        id: mediaPressSelect
        target: null
        acceptedDevices: PointerDevice.Mouse | PointerDevice.TouchPad
        acceptedButtons: Qt.LeftButton
        grabPermissions: PointerHandler.ApprovesTakeOverByAnything
        enabled: interaction.mediaPressSelectEnabledState

        onActiveChanged: {
            if (!active || !activeCoordinator || !delegateItem || !rootController)
                return
            var mediaId = delegateItem.currentMediaId
            if (!activeCoordinator.claimMediaPress(mediaId))
                return
            var modifiers = Qt.application.keyboardModifiers
            if (mediaPressSelect.point
                    && mediaPressSelect.point.modifiers !== undefined)
                modifiers = mediaPressSelect.point.modifiers
            activeCoordinator.noteMediaPrimaryPress(
                mediaId, (modifiers & Qt.ShiftModifier) !== 0)
        }
    }
}
