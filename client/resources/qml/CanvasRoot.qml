import QtQuick
import QtQuick.Window
import Mouffette.App as AppStyle
import Mouffette.Canvas
Rectangle {
    id: root
    color: AppStyle.Theme.canvasBackground
    border.width: 1
    border.color: AppStyle.Theme.border
    radius: 5
    clip: true

    signal mediaSelectRequested(string mediaId, bool additive)
    signal mediaMoveStarted(string mediaId, real sceneX, real sceneY, bool snap)
    signal mediaMoveUpdated(string mediaId, real sceneX, real sceneY, bool snap)
    signal mediaMoveEnded(string mediaId, real sceneX, real sceneY, bool snap)
    signal clearSelectionRequested()
    signal mediaResizeRequested(string mediaId, string handleId, real sceneX, real sceneY, bool snap, bool altPressed)
    signal mediaResizeEnded(string mediaId)
    signal textCommitRequested(string mediaId, string text)
    signal textLiveUpdateRequested(string mediaId, string text)
    signal textCreateRequested(real viewX, real viewY)

    // Overlay action signals (forwarded to C++)
    signal overlayVisibilityToggleRequested(string mediaId, bool visible)
    signal overlayBringForwardRequested(string mediaId)
    signal overlayBringBackwardRequested(string mediaId)
    signal overlayDeleteRequested(string mediaId)
    signal overlayPlayPauseRequested(string mediaId)
    signal overlayStopRequested(string mediaId)
    signal overlayRepeatToggleRequested(string mediaId)
    signal overlayMuteToggleRequested(string mediaId)
    signal overlayVolumeChangeRequested(string mediaId, real value)
    // Three-phase seek protocol. C++ coalesces frequent updates and releases
    // the scrub session only when the final native frame is acknowledged.
    signal overlaySeekBeginRequested(string mediaId, real ratio)
    signal overlaySeekUpdateRequested(string mediaId, real ratio)
    signal overlaySeekEndRequested(string mediaId, real ratio)
    signal overlayFitToTextToggleRequested(string mediaId)
    signal overlayHorizontalAlignRequested(string mediaId, string alignment)
    signal overlayVerticalAlignRequested(string mediaId, string alignment)

    readonly property int screenCount: screensModel.length
    readonly property var canvasController: sessionViewModel
                                            ? sessionViewModel.canvasController : null
    readonly property bool editingEnabled: canvasController ? canvasController.editingEnabled : true
    readonly property var selectionChrome: selectionLayerLoader.item
                                           ? selectionLayerLoader.item.chromeItem : null
    readonly property var liveTransforms: canvasController ? canvasController.liveTransforms : ({})
    readonly property var hostingWindow: root.Window.window
    property bool remoteActive: canvasController
                                ? canvasController.remoteActive : false
    property var screensModel: canvasController
                               ? canvasController.screensModel : []
    property var uiZonesModel: canvasController
                               ? canvasController.uiZonesModel : []
    property var mediaModel: canvasController
                             ? canvasController.mediaSnapshot : []
    // Stable C++ QAbstractListModel — the Repeater binds here so delegates are
    // Not recreated on move/resize commits: only dataChanged fires per row.
    // The mediaModel JS-array above supplies metadata to chrome and overlays;
    // pointer picking uses the actual live delegates, not that snapshot.
    property var mediaListModel: canvasController
                                 ? canvasController.mediaModel : null
    // Presentation view model supplied by CanvasPage. It owns validation and
    // document commands; the QML surface only supplies local pointer geometry.
    property var sessionViewModel: null
    property var selectionChromeModel: canvasController
                                       ? canvasController.selectionChromeModel : []
    property var snapGuidesModel: []
    // Raw pointer tracking; the controller projects the whole selection's final geometry.
    property string liveDragMediaId: ""
    property real liveDragViewOffsetX: 0.0
    property real liveDragViewOffsetY: 0.0
    // Snap-corrected live drag position — set by C++ per tick when Shift+drag snap is active.
    // liveSnapDragActive is DERIVED from liveSnapDragMediaId so both are always in sync:
    // setting liveSnapDragMediaId = "" is the one and only way to clear the freeze.
    property string liveSnapDragMediaId: ""
    readonly property bool liveSnapDragActive: liveSnapDragMediaId !== ""
    property real   liveSnapDragX:       0.0
    property real   liveSnapDragY:       0.0
    property bool textToolActive: canvasController
                                  ? canvasController.textToolActive : false
    property bool remoteCursorVisible: canvasController
                                       ? canvasController.remoteCursorVisible : false
    property real remoteCursorX: canvasController
                                 ? canvasController.remoteCursorX : 0
    property real remoteCursorY: canvasController
                                 ? canvasController.remoteCursorY : 0
    property int remoteCursorDiameter: 30
    property color remoteCursorFill: AppStyle.Theme.remoteCursorFill
    property color remoteCursorBorder: AppStyle.Theme.remoteCursorBorder
    property real remoteCursorBorderWidth: 2
    property real viewScale: 1.0
    property real panX: 0.0
    property real panY: 0.0
    property real wheelZoomBase: 1.0015
    property real wheelZoomSensitivity: 5.0
    property real trackpadZoomSensitivity: 0.003
    // Transient live-resize overlay state (avoids full model mutation per pointer tick)
    property bool liveResizeActive: false
    property string liveResizeMediaId: ""
    property real liveResizeX: 0.0
    property real liveResizeY: 0.0
    property real liveResizeScale: 1.0
    // Transient live alt-resize state (base-size changes, not scale)
    property bool liveAltResizeActive: false
    property string liveAltResizeMediaId: ""
    property real liveAltResizeX: 0.0
    property real liveAltResizeY: 0.0
    property real liveAltResizeWidth: 0.0
    property real liveAltResizeHeight: 0.0
    property real liveAltResizeScale: 1.0
    readonly property string interactionMode: (inputLayer && inputLayer.inputCoordinator)
                                       ? inputLayer.inputCoordinator.mode
                                       : "idle"
    readonly property string interactionOwnerId: (inputLayer && inputLayer.inputCoordinator)
                                          ? inputLayer.inputCoordinator.ownerId
                                          : ""
    property int activeMediaDragCount: 0
    readonly property bool mediaMoveHandlerActive: globalMediaDrag.active
    readonly property string activeMoveMediaId: globalMediaDrag.activeMoveMediaId
    property string pendingInputReconcileReason: ""
    // True while any text media item is in text-edit mode. Used to disable canvas
    // pan so parent DragHandlers don't interfere with TextEdit cursor placement.
    readonly property bool anyMediaEditing: textEditSession.activeEditor !== null
    // Reference to the TextItem currently in edit mode, or null. Used to commit
    // and exit editing when the user presses outside the item.
    readonly property Item currentEditingMediaItem: textEditSession.activeEditor
    property int textEditingRequestSerial: 0
    TextEditSession {
        id: textEditSession
        selectionModel: root.selectionChromeModel
    }
    // Video state dictionary: keys are mediaId strings, values are state maps.
    // Published every 50 ms by QuickCanvasController for ALL video items
    // (not just the selected one), so overlays remain live after deselection.
    property var videoStateModel: canvasController
                                  ? canvasController.videoStateModel : ({})
    focus: true

    property Item shortcutScope: root
    readonly property bool shortcutScopeFocused: {
        var item = hostingWindow ? hostingWindow.activeFocusItem : null
        while (item) {
            if (item === shortcutScope)
                return true
            item = item.parent
        }
        return false
    }
    readonly property bool textInputFocused: {
        var item = hostingWindow ? hostingWindow.activeFocusItem : null
        while (item) {
            if (item instanceof TextInput || item instanceof TextEdit)
                return true
            item = item.parent
        }
        return false
    }
    readonly property bool mediaShortcutsEnabled: visible && enabled && editingEnabled
                                                  && !!canvasController && !anyMediaEditing
                                                  && shortcutScopeFocused && !textInputFocused
                                                  && interactionMode === "idle"
    readonly property string selectedVideoId: selectionChromeModel.length === 1
        && videoStateModel[selectionChromeModel[0].mediaId] !== undefined
        ? selectionChromeModel[0].mediaId : ""

    component CanvasShortcut: Shortcut {
        property bool applicable: true
        enabled: root.mediaShortcutsEnabled && applicable
        context: Qt.WindowShortcut
        autoRepeat: false
    }
    CanvasShortcut {
        sequences: Qt.platform.os === "osx" ? [StandardKey.Copy, "Meta+C"] : [StandardKey.Copy]
        applicable: root.selectionChromeModel.length > 0
        onActivated: root.canvasController.copySelectedMedia()
    }
    CanvasShortcut {
        sequences: Qt.platform.os === "osx" ? [StandardKey.Paste, "Meta+V"] : [StandardKey.Paste]
        onActivated: root.canvasController.pasteMedia()
    }
    CanvasShortcut {
        // Qt's Ctrl modifier maps to the physical Command key on macOS.
        sequences: Qt.platform.os === "osx" ? ["Ctrl+Backspace"] : ["Delete", "Backspace"]
        applicable: root.selectionChromeModel.length > 0
        onActivated: root.canvasController.deleteSelectedMedia()
    }
    CanvasShortcut {
        sequence: "Space"
        applicable: root.selectedVideoId !== ""
        onActivated: root.canvasController.handleOverlayPlayPause(root.selectedVideoId)
    }
    CanvasShortcut {
        sequence: "M"
        applicable: root.selectedVideoId !== ""
        onActivated: root.canvasController.handleOverlayMuteToggle(root.selectedVideoId)
    }
    CanvasShortcut {
        sequence: "S"
        applicable: root.selectedVideoId !== ""
        onActivated: root.canvasController.handleVideoStartToggle(root.selectedVideoId)
    }
    CanvasShortcut {
        sequence: "E"
        applicable: root.selectedVideoId !== ""
        onActivated: root.canvasController.handleVideoEndToggle(root.selectedVideoId)
    }

    function synchronizeTransientState() {
        if (!canvasController)
            return
        viewScale = canvasController.viewScale
        panX = canvasController.panX
        panY = canvasController.panY
        snapGuidesModel = canvasController.snapGuidesModel
        liveSnapDragMediaId = canvasController.liveSnapDragMediaId
        liveSnapDragX = canvasController.liveSnapDragX
        liveSnapDragY = canvasController.liveSnapDragY
        liveResizeActive = canvasController.liveResizeActive
        liveResizeMediaId = canvasController.liveResizeMediaId
        liveResizeX = canvasController.liveResizeX
        liveResizeY = canvasController.liveResizeY
        liveResizeScale = canvasController.liveResizeScale
        liveAltResizeActive = canvasController.liveAltResizeActive
        liveAltResizeMediaId = canvasController.liveAltResizeMediaId
        liveAltResizeX = canvasController.liveAltResizeX
        liveAltResizeY = canvasController.liveAltResizeY
        liveAltResizeWidth = canvasController.liveAltResizeWidth
        liveAltResizeHeight = canvasController.liveAltResizeHeight
        liveAltResizeScale = canvasController.liveAltResizeScale
    }

    function discardPointerEdits() {
        ++root.textEditingRequestSerial
        abandonPointerInteractions("editing-canceled")
        for (var i = 0; i < mediaRepeater.count; ++i) {
            var item = mediaRepeater.itemAt(i)
            if (item && item.media) {
                item.localDragging = false
                item.localX = item.media.x
                item.localY = item.media.y
                item.localScale = item.media.scale
            }
        }
    }

    onEditingEnabledChanged: {
        if (!editingEnabled) {
            discardPointerEdits()
            textEditSession.finish()
        }
    }

    function registerCanvasSurface() {
        if (!canvasController)
            return
        if (hostingWindow)
            canvasController.registerWindow(hostingWindow)
        canvasController.setViewportSize(width, height)
        synchronizeTransientState()
    }

    onCanvasControllerChanged: {
        ++root.textEditingRequestSerial
        registerCanvasSurface()
    }
    onHostingWindowChanged: registerCanvasSurface()
    onWidthChanged: if (canvasController) canvasController.setViewportSize(width, height)
    onHeightChanged: if (canvasController) canvasController.setViewportSize(width, height)
    Component.onCompleted: registerCanvasSurface()

    function requestTextEditing(mediaId, selectAll, additive, sceneX, sceneY) {
        if (!root.editingEnabled || !mediaId)
            return
        // Creation has already selected the new media in the document. A
        // double-click explicitly replaces selection unless Shift is held.
        if (!selectAll)
            root.mediaSelectRequested(mediaId, !!additive)
        var controller = root.canvasController
        var requestSerial = ++root.textEditingRequestSerial
        // Finish insertion/selection bindings and the native release before
        // entering the editor. Resolve its current visual by ID, never capture
        // a renderer which publication or a workspace switch could destroy.
        Qt.callLater(function() {
            if (root.textEditingRequestSerial !== requestSerial
                    || root.canvasController !== controller || !root.editingEnabled
                    || !textEditSession.isSelected(mediaId))
                return
            var delegate = root.mediaDelegateById(mediaId)
            if (delegate && delegate.visible && delegate.enabled)
                delegate.beginTextEditing(!!selectAll, sceneX, sceneY)
        })
    }

    Connections {
        target: root.canvasController
        function onPresentationChanged() { root.synchronizeTransientState() }
        function onPendingEditsCanceled() { root.discardPointerEdits() }
        function onTextEditingRequested(mediaId) {
            root.requestTextEditing(mediaId, true, false)
        }
    }

    function abandonPointerInteractions(reason) {
        if (canvasController)
            canvasController.finishSelectionScaleGesture()
        if (!inputLayer || !inputLayer.inputCoordinator)
            return

        // Qt normally cancels native grabs at these lifecycle boundaries. The
        // logical finalizers below are the fallback when that callback is lost
        // or arrives after the document has already republished its model.
        var coordinator = inputLayer.inputCoordinator
        if (coordinator.mode === "move" || liveDragMediaId !== "")
            finishActiveMoveInteraction(reason || "lifecycle")
        if (selectionChrome
                && (selectionChrome.interacting
                    || selectionChrome.activeResizeMediaId !== ""))
            selectionChrome.finishResizeSession(true)
        if (coordinator.mode === "resize")
            coordinator.forceReset((reason || "lifecycle") + ":stale-resize")
        else if (coordinator.primaryGestureActive)
            coordinator.endPrimaryGesture()
    }

    // A window/app transition can consume the mouse release before Qt sends it
    // back to the canvas. Close the active transaction locally at that
    // lifecycle boundary; a later native ungrab is harmless because both
    // finalizers are idempotent.
    Connections {
        target: root.hostingWindow
        ignoreUnknownSignals: true

        function onActiveChanged() {
            if (root.hostingWindow && !root.hostingWindow.active)
                root.abandonPointerInteractions("window-deactivated")
        }

        function onVisibilityChanged() {
            if (!root.hostingWindow)
                return
            var visibility = root.hostingWindow.visibility
            if (visibility === Window.Hidden || visibility === Window.Minimized)
                root.abandonPointerInteractions("window-hidden")
        }
    }

    Connections {
        target: Qt.application
        ignoreUnknownSignals: true

        function onStateChanged() {
            if (Qt.application.state !== Qt.ApplicationActive)
                root.abandonPointerInteractions("application-suspended")
        }
    }

    onMediaSelectRequested: (mediaId, additive) => canvasController?.handleMediaSelectRequested(mediaId, additive)
    onClearSelectionRequested: () => canvasController?.handleClearSelectionRequested()
    onMediaMoveStarted: (mediaId, x, y, snap) => canvasController?.handleMediaMoveStarted(mediaId, x, y, snap)
    onMediaMoveUpdated: (mediaId, x, y, snap) => canvasController?.handleMediaMoveUpdated(mediaId, x, y, snap)
    onMediaMoveEnded: (mediaId, x, y, snap) => canvasController?.handleMediaMoveEnded(mediaId, x, y, snap)
    onMediaResizeRequested: (mediaId, handleId, x, y, snap, altPressed) => canvasController?.handleMediaResizeRequested(mediaId, handleId, x, y, snap, altPressed)
    onMediaResizeEnded: mediaId => canvasController?.handleMediaResizeEnded(mediaId)
    onTextCommitRequested: (mediaId, text) => canvasController?.handleTextCommitRequested(mediaId, text)
    onTextLiveUpdateRequested: (mediaId, text) => canvasController?.handleTextLiveUpdateRequested(mediaId, text)
    onTextCreateRequested: (x, y) => canvasController?.handleTextCreateRequested(x, y)
    onOverlayVisibilityToggleRequested: (mediaId, visible) => canvasController?.handleOverlayVisibilityToggle(mediaId, visible)
    onOverlayBringForwardRequested: mediaId => canvasController?.handleOverlayBringForward(mediaId)
    onOverlayBringBackwardRequested: mediaId => canvasController?.handleOverlayBringBackward(mediaId)
    onOverlayDeleteRequested: mediaId => canvasController?.handleOverlayDelete(mediaId)
    onOverlayPlayPauseRequested: mediaId => canvasController?.handleOverlayPlayPause(mediaId)
    onOverlayStopRequested: mediaId => canvasController?.handleOverlayStop(mediaId)
    onOverlayRepeatToggleRequested: mediaId => canvasController?.handleOverlayRepeatToggle(mediaId)
    onOverlayMuteToggleRequested: mediaId => canvasController?.handleOverlayMuteToggle(mediaId)
    onOverlayVolumeChangeRequested: (mediaId, value) => canvasController?.handleOverlayVolumeChange(mediaId, value)
    onOverlaySeekBeginRequested: (mediaId, ratio) => canvasController?.handleOverlaySeekBegin(mediaId, ratio)
    onOverlaySeekUpdateRequested: (mediaId, ratio) => canvasController?.handleOverlaySeekUpdate(mediaId, ratio)
    onOverlaySeekEndRequested: (mediaId, ratio) => canvasController?.handleOverlaySeekEnd(mediaId, ratio)
    onOverlayFitToTextToggleRequested: mediaId => canvasController?.handleOverlayFitToTextToggle(mediaId)
    onOverlayHorizontalAlignRequested: (mediaId, alignment) => canvasController?.handleOverlayHorizontalAlign(mediaId, alignment)
    onOverlayVerticalAlignRequested: (mediaId, alignment) => canvasController?.handleOverlayVerticalAlign(mediaId, alignment)

    DropArea {
        anchors.fill: parent
        z: 200000

        onEntered: function(drag) {
            if (root.sessionViewModel
                    && root.sessionViewModel.beginFileDrag(drag.urls,
                                                           drag.x, drag.y)) {
                drag.accept(Qt.CopyAction)
            } else {
                drag.accepted = false
            }
        }
        onPositionChanged: function(drag) {
            if (root.sessionViewModel
                    && root.sessionViewModel.updateFileDrag(drag.x, drag.y)) {
                drag.accept(Qt.CopyAction)
            } else {
                drag.accepted = false
            }
        }
        onDropped: function(drop) {
            if (root.sessionViewModel
                    && root.sessionViewModel.commitFileDrop(drop.x, drop.y)) {
                drop.acceptProposedAction()
            } else {
                drop.accepted = false
            }
        }
        onExited: if (root.sessionViewModel) root.sessionViewModel.cancelFileDrag()
    }

    Keys.onReleased: function(event) {
        if (event.key === Qt.Key_Alt && root.canvasController)
            root.canvasController.finishSelectionScaleGesture()
        if (event.key === Qt.Key_Shift) {
            // Clear guides only when no active snapped interaction is alive.
            // During fast Shift-release + mouse-release races, clearing unconditionally
            // can hide guides while snap state is still authoritative for the current gesture.
            if (!root.liveSnapDragActive && (!selectionChrome || !selectionChrome.interacting)) {
                root.snapGuidesModel = []
            }
            // Do NOT touch liveSnapDragMediaId here. liveSnapDragActive is derived
            // (readonly) and the freeze lifecycle is owned exclusively by:
            //   • C++ handleMediaMoveUpdated — clears freeze when snap disengages during drag
            //   • QML onMediaChanged          — clears freeze after committed position arrives
            //   • snapFreezeCleanupTimer      — safety-net for stuck freeze after drag end
            // Clearing early would expose stale localX/Y before onMediaChanged syncs it.
        }
    }

    function requestMediaSelection(mediaId, additive) {
        if (!editingEnabled || !mediaId || mediaId.length === 0)
            return
        // Pressing an existing selection begins a group gesture. Explicit
        // selection commands (inspector/list) retain their replace semantics.
        if (textEditSession.isSelected(mediaId))
            return
        root.mediaSelectRequested(mediaId, !!additive)
    }

    // One picker for every input consumer. The actual delegates include live
    // drag/resize transforms and visibility; a previously published DTO does not.
    function mediaIdAtPoint(viewX, viewY) {
        if (!viewport.contains(Qt.point(viewX, viewY)))
            return ""
        var hitId = ""
        var hitZ = -Infinity
        for (var i = 0; i < mediaRepeater.count; ++i) {
            var candidate = mediaRepeater.itemAt(i)
            if (!candidate || !candidate.visible || !candidate.enabled || candidate.opacity <= 0)
                continue
            var point = candidate.mapFromItem(viewport, viewX, viewY)
            if (point.x < 0 || point.y < 0 || point.x >= candidate.width || point.y >= candidate.height)
                continue
            if (candidate.z >= hitZ) {
                hitId = candidate.currentMediaId
                hitZ = candidate.z
            }
        }
        return hitId
    }

    function mediaDelegateById(mediaId) {
        if (!mediaId)
            return null
        for (var i = 0; i < mediaRepeater.count; ++i) {
            var candidate = mediaRepeater.itemAt(i)
            if (candidate && candidate.currentMediaId === mediaId)
                return candidate
        }
        return null
    }

    function mediaPlaybackControlsReady(mediaId, entry) {
        if (!entry || entry.residencyReady !== true)
            return false
        if (entry.canvasMedia !== true)
            return true
        var delegate = mediaDelegateById(mediaId)
        return !!delegate && delegate.initialFramePresented && delegate.contentReady
    }

    function finishActiveMoveInteraction(reason) {
        return globalMediaDrag.finishMoveSession(reason || "recovery")
    }

    function canStartCanvasPan(panActive) {
        return !!inputLayer
            && !!inputLayer.inputCoordinator
            && inputLayer.inputCoordinator.canEnablePan(panActive)
    }

    function canStartTextToolTap() {
        return editingEnabled && !!inputLayer
            && !!inputLayer.inputCoordinator
            && inputLayer.inputCoordinator.canStartTextToolTap()
    }

    // The controller owns camera math and publishes one complete transform.
    // The unhosted surface also supports local pan for interaction fixtures.
    function panBy(dx, dy) {
        if (canvasController) {
            canvasController.panBy(dx, dy)
        } else {
            panX += dx
            panY += dy
        }
    }

    function applyZoomAt(anchorX, anchorY, factor) {
        if (canvasController)
            canvasController.zoomAt(anchorX, anchorY, factor)
    }

    function fitToScreens(marginPx) {
        return canvasController
            ? canvasController.fitToScreens(marginPx === undefined ? 53 : marginPx) : false
    }

    function recenterView(marginPx) {
        if (canvasController)
            canvasController.recenterView(marginPx === undefined ? 53 : marginPx)
    }

    function fitToBounds(boundsX, boundsY, boundsW, boundsH, marginPx) {
        if (canvasController)
            canvasController.fitToBounds(boundsX, boundsY, boundsW, boundsH,
                                         marginPx === undefined ? 53 : marginPx)
    }

    function isZoomModifier(modifiers) {
        // Qt maps the physical macOS Control key to MetaModifier (Command is
        // ControlModifier). This gesture deliberately uses Control, not Command.
        if (Qt.platform.os === "osx") {
            return (modifiers & Qt.MetaModifier) !== 0
        }
        return (modifiers & Qt.ControlModifier) !== 0
    }

    function wheelDeltaY(wheel) {
        if (wheel.pixelDelta && (wheel.pixelDelta.x !== 0 || wheel.pixelDelta.y !== 0))
            return wheel.pixelDelta.y
        if (wheel.angleDelta)
            return wheel.angleDelta.y / 8.0
        return 0.0
    }

    function wheelDeltaX(wheel) {
        if (wheel.pixelDelta && (wheel.pixelDelta.x !== 0 || wheel.pixelDelta.y !== 0))
            return wheel.pixelDelta.x
        if (wheel.angleDelta)
            return wheel.angleDelta.x / 8.0
        return 0.0
    }

    function isTrackpadWheel(wheel) {
        if (!wheel)
            return false

        // QML WheelEvent exposes the device and phase, not QWheelEvent.source.
        // Begin/end packets can have zero pixel deltas and still be trackpad events.
        return (wheel.device && wheel.device.type === PointerDevice.TouchPad)
            || wheel.phase !== Qt.NoScrollPhase
            || (!!wheel.pixelDelta
                && (wheel.pixelDelta.x !== 0 || wheel.pixelDelta.y !== 0))
    }

    function isFiniteNumber(v) {
        return v !== Infinity && v !== -Infinity && !isNaN(v)
    }

    function canProcessCameraWheel() {
        if (interactionMode === "idle")
            return true
        return interactionMode === "pan"
            && (interactionOwnerId === "" || interactionOwnerId === "canvas")
    }

    function mediaModelContainsId(mediaId) {
        if (!mediaId || !mediaModel)
            return false
        for (var i = 0; i < mediaModel.length; ++i) {
            var entry = mediaModel[i]
            if (entry && entry.mediaId === mediaId)
                return true
        }
        return false
    }

    function reconcileInputCoordinatorState(reason) {
        if (!inputLayer || !inputLayer.inputCoordinator)
            return

        var coordinator = inputLayer.inputCoordinator
        var mode = coordinator.mode
        var owner = coordinator.ownerId || ""

        // Repair the physical press lease independently from the logical mode.
        // A native cancel can leave the store behind even when no move/resize
        // session remains.
        if (coordinator.primaryGestureActive && !primaryGestureRouter.active)
            coordinator.endPrimaryGesture()

        // `interacting` alone is not proof that Qt still owns the pointer. A
        // resize whose exclusive grab vanished must be committed/closed once,
        // otherwise every media DragHandler stays disabled indefinitely.
        if (selectionChrome
                && (selectionChrome.interacting
                    || selectionChrome.activeResizeMediaId !== "")
                && !selectionChrome.resizeHandlerActive) {
            selectionChrome.finishResizeSession(true)
            mode = coordinator.mode
            owner = coordinator.ownerId || ""
        }

        if (mode === "idle") {
            if (activeMediaDragCount > 0 || liveDragMediaId !== "")
                finishActiveMoveInteraction(reason)
            return
        }

        if (mode === "move") {
            var moveDelegate = mediaDelegateById(owner)
            var moveHealthy = activeMediaDragCount > 0
                    && liveDragMediaId !== ""
                    && owner !== ""
                    && liveDragMediaId === owner
                    && mediaModelContainsId(owner)
                    && !!moveDelegate
                    && globalMediaDrag.active
                    && globalMediaDrag.activeMoveMediaId === owner
            if (!moveHealthy) {
                finishActiveMoveInteraction(reason)
            }
            return
        }

        if (mode === "resize") {
            var resizeHealthy = !!selectionChrome
                && selectionChrome.interacting
                && selectionChrome.resizeHandlerActive
                && owner !== ""
                && selectionChrome.activeResizeMediaId === owner
                && mediaModelContainsId(owner)
            if (!resizeHealthy) {
                // The global handle survives deletion of its media delegate.
                // Close its session (and backend resize) before releasing the
                // pointer, instead of leaving hidden chrome state behind.
                if (selectionChrome
                        && (selectionChrome.interacting
                            || selectionChrome.activeResizeMediaId !== ""))
                    selectionChrome.finishResizeSession(true)
                if (coordinator.mode === "resize")
                    coordinator.forceReset(reason + ":stale-resize")
            }
            return
        }

        if (mode === "pan") {
            if ((!panDrag || !panDrag.active) && (!middlePanDrag || !middlePanDrag.active)) {
                coordinator.forceReset(reason + ":stale-pan")
            }
            return
        }

        // Text creation is a synchronous transaction with a finally block.
        // Its callback legitimately publishes media/selection before returning;
        // observing that publication must not reset the transaction mid-call.
    }

    function scheduleInputCoordinatorReconcile(reason) {
        // Model/selection notifications can arrive synchronously from inside a
        // PointerHandler's onActiveChanged callback. Readonly QML bindings such
        // as moveHandlerActive are updated only after that callback unwinds;
        // reconciling inline would mistake a newly-started native grab for a
        // stale one and cancel every media move before its first update.
        pendingInputReconcileReason = reason || "model-publication"
        inputReconcileTimer.restart()
    }

    Timer {
        id: inputReconcileTimer
        interval: 0
        repeat: false
        onTriggered: {
            var reason = root.pendingInputReconcileReason
            root.pendingInputReconcileReason = ""
            root.reconcileInputCoordinatorState(reason)
        }
    }

    onMediaModelChanged: {
        scheduleInputCoordinatorReconcile("media-model-changed")
    }

    onSelectionChromeModelChanged: {
        scheduleInputCoordinatorReconcile("selection-model-changed")
    }

    function mediaSourceUrl(path) {
        if (!path || path.length === 0)
            return ""
        if (path.indexOf("file:") === 0 || path.indexOf("qrc:") === 0 || path.indexOf("http:") === 0 || path.indexOf("https:") === 0)
            return path

        var normalized = path.replace(/\\/g, "/")
        if (/^[A-Za-z]:\//.test(normalized)) {
            return "file:///" + encodeURI(normalized)
        }
        if (normalized.indexOf("//") === 0) {
            return "file:" + encodeURI(normalized)
        }
        if (normalized.indexOf("/") === 0) {
            return "file://" + encodeURI(normalized)
        }
        return "file:///" + encodeURI(normalized)
    }

    function applyLiveResizeGeometry(mediaId, sceneX, sceneY, scale) {
        if (!mediaId)
            return false

        liveResizeMediaId = mediaId
        liveResizeX = sceneX
        liveResizeY = sceneY
        liveResizeScale = scale
        liveResizeActive = true
        return true
    }

    function applyLiveAltResizeGeometry(mediaId, sceneX, sceneY, width, height, scale) {
        if (!mediaId)
            return false

        liveAltResizeMediaId = mediaId
        liveAltResizeX = sceneX
        liveAltResizeY = sceneY
        liveAltResizeWidth = width
        liveAltResizeHeight = height
        liveAltResizeScale = scale
        liveAltResizeActive = true
        return true
    }

    function commitMediaGeometry(mediaId, sceneX, sceneY, scale) {
        if (!mediaId || !mediaModel || mediaModel.length === 0)
            return false

        for (var index = 0; index < mediaModel.length; ++index) {
            var entry = mediaModel[index]
            if (!entry || entry.mediaId !== mediaId)
                continue

            var updated = ({})
            for (var key in entry)
                updated[key] = entry[key]

            updated.x = sceneX
            updated.y = sceneY
            updated.scale = scale

            var next = mediaModel.slice(0)
            next[index] = updated
            mediaModel = next
            return true
        }

        return false
    }

    function beginLiveResize(mediaId) {
        if (!mediaId)
            return false

        // Alt may be released without ending the pointer gesture. The backend
        // commits that pending non-uniform geometry before entering this path;
        // remove its visual override so uniform updates become authoritative.
        if (liveAltResizeMediaId === mediaId) {
            liveAltResizeActive = false
            liveAltResizeMediaId = ""
            liveAltResizeX = 0.0
            liveAltResizeY = 0.0
            liveAltResizeWidth = 0.0
            liveAltResizeHeight = 0.0
            liveAltResizeScale = 1.0
        }

        // Initialize geometry first so turning the live flag on never renders
        // a transient frame at (0,0) with scale=1.
        if (mediaModel && mediaModel.length > 0) {
            for (var i = 0; i < mediaModel.length; ++i) {
                var entry = mediaModel[i]
                if (!entry || entry.mediaId !== mediaId)
                    continue
                liveResizeX = entry.x
                liveResizeY = entry.y
                liveResizeScale = entry.scale || 1.0
                break
            }
        }
        liveResizeMediaId = mediaId
        liveResizeActive = true
        return true
    }

    function endLiveResize(mediaId, sceneX, sceneY, scale) {
        if (!mediaId)
            return false

        var committed = commitMediaGeometry(mediaId, sceneX, sceneY, scale)
        liveResizeActive = false
        liveResizeMediaId = ""
        liveResizeX = 0.0
        liveResizeY = 0.0
        liveResizeScale = 1.0
        if (liveAltResizeMediaId === mediaId) {
            liveAltResizeActive = false
            liveAltResizeMediaId = ""
            liveAltResizeX = 0.0
            liveAltResizeY = 0.0
            liveAltResizeWidth = 0.0
            liveAltResizeHeight = 0.0
            liveAltResizeScale = 1.0
        }
        return committed
    }

    // Viewport-space background: stays centered during navigation, while the
    // scene and all its media render above it regardless of their own z values.
    Text {
        objectName: "emptyScreenHint"
        anchors.centerIn: parent
        width: Math.min(parent.width - 48, 640)
        visible: !!root.sessionViewModel && root.sessionViewModel.loading === false
                 && root.screenCount === 0
        text: "No screens available"
        color: AppStyle.Theme.mutedText
        font.pixelSize: Math.max(28, AppStyle.Theme.titleFontSize * 1.5)
        font.bold: true
        horizontalAlignment: Text.AlignHCenter
        wrapMode: Text.WordWrap
    }

    CanvasViewport {
        id: viewport
        anchors.fill: parent
        panX: root.panX
        panY: root.panY
        viewScale: root.viewScale

            Repeater {
                model: root.screensModel
                delegate: ScreenItem {
                    screenX: modelData.x
                    screenY: modelData.y
                    screenWidth: modelData.width
                    screenHeight: modelData.height
                    primary: modelData.primary
                    // Label: zoom-invariant rendering, matches old widget canvas
                    viewScale:   root.viewScale
                    screenIndex: modelData.displayIndex || (index + 1)
                    screenId:    modelData.screenId !== undefined ? modelData.screenId : -1
                    pixelWidth:  modelData.pixelWidth  || 0
                    pixelHeight: modelData.pixelHeight || 0
                    uiZonesModel: root.uiZonesModel
                }
            }

            MediaLayer {
                id: mediaLayer
                // Interactive ancestors cover the viewport, not a viewport-sized
                // rectangle in scene coordinates. Qt can otherwise prune visible
                // children from pointer delivery after zoom/pan (effective clipping).
                parent: viewport
                anchors.fill: parent
                z: 1

                // Each media item is a native QML Item with its own local x/y/scale.
                // The viewport-level globalMediaDrag owns movement so image,
                // video and text delegates all follow the same input path.
                Repeater {
                    id: mediaRepeater
                    // Use the stable C++ QAbstractListModel so that move/resize
                    // commits issue dataChanged per row rather than a full
                    // model replacement that would destroy every delegate.
                    model: root.mediaListModel
                    delegate: Item {
                    id: mediaDelegate
                    property var media: modelData
                    readonly property bool contentReady: mediaContentLoader.contentReady
                    readonly property bool initialFramePresented: mediaContentLoader.initialFramePresented

                    // Local position/scale — tracks model when idle, free during drag
                    property real localX: media ? media.x : 0.0
                    property real localY: media ? media.y : 0.0
                    property real localScale: media ? (media.scale || 1.0) : 1.0
                    property bool localDragging: false
                    function beginTextEditing(selectAll, sceneX, sceneY) {
                        var editor = mediaContentLoader.visualItem
                        return !!editor && editor.textEditable
                            && editor.beginEditing(selectAll !== false, sceneX, sceneY)
                    }
                    // Actual rendered scale — switches to live scale during resize/alt-resize
                    // so overlay counter-scale stays correct every frame.
                    readonly property var liveTransform: root.liveTransforms[currentMediaId] || null
                    readonly property real effectiveScale: liveTransform ? liveTransform.scale
                        : (usesLiveAltResize ? root.liveAltResizeScale
                           : (usesLiveResize ? root.liveResizeScale : localScale))
                    readonly property string currentMediaId: media ? (media.mediaId || "") : ""
                    readonly property bool moveHandlerActive: root.mediaMoveHandlerActive
                                                              && root.activeMoveMediaId === currentMediaId
                    readonly property string activeMoveMediaId: moveHandlerActive
                                                                 ? root.activeMoveMediaId : ""

                    function scheduleSnapFreezeCleanup() {
                        snapFreezeCleanupTimer.restart()
                    }

                    // Derived from selectionChromeModel — NOT from media.selected.
                    // This avoids mediaModel churn (and delegate destruction) on selection changes.
                    readonly property bool isSelected: {
                        var mid = currentMediaId
                        if (!mid || !root.selectionChromeModel) return false
                        for (var i = 0; i < root.selectionChromeModel.length; ++i) {
                            var entry = root.selectionChromeModel[i]
                            if (entry && entry.mediaId === mid) return true
                        }
                        return false
                    }

                    onMediaChanged: {
                        if (!localDragging && media) {
                            // Always sync local position to the committed model value.
                            // Safe because localDragging=false means no active drag is
                            // writing to localX/Y, so overwriting never causes a jump.
                            localX = media.x
                            localY = media.y
                            localScale = media.scale || 1.0

                            // Release the snap freeze whenever this item's drag is over.
                            // liveDragMediaId is cleared BEFORE mediaMoveEnded is signalled,
                            // so by the time onMediaChanged fires the drag is always ended
                            // and the condition below is always satisfied post-drag.
                            // During an active drag the freeze must stay so effectiveLocalX/Y
                            // keeps showing the snapped position — hence the guard.
                            // The safety-net timer covers the rare edge case where this
                            // handler never fires at all (delegate destroyed, etc.).
                            if (root.liveSnapDragMediaId === currentMediaId
                                    && root.liveDragMediaId !== currentMediaId) {
                                root.liveSnapDragMediaId = ""
                                snapFreezeCleanupTimer.stop()
                            }
                        }
                    }

                    // Safety-net: if onMediaChanged never fires after drag end (edge
                    // case: delegate destroyed, model update suppressed, etc.) this timer
                    // force-clears the snap freeze so content is never permanently stuck.
                    Timer {
                        id: snapFreezeCleanupTimer
                        interval: UiTiming.snapFreezeCleanupDelayMs
                        repeat: false
                        onTriggered: {
                            if (root.liveSnapDragMediaId === mediaDelegate.currentMediaId) {
                                // Sync localX/Y to the snapped position BEFORE clearing the
                                // freeze so that effectiveLocalX falls back to the correct
                                // snapped value, not the stale raw cursor position.
                                if (!mediaDelegate.localDragging) {
                                    mediaDelegate.localX = root.liveSnapDragX
                                    mediaDelegate.localY = root.liveSnapDragY
                                }
                                root.liveSnapDragMediaId = ""
                            }
                        }
                    }

                    Component.onDestruction: {
                        if (root.activeMoveMediaId === currentMediaId)
                            root.finishActiveMoveInteraction("delegate-destroyed")
                    }

                    readonly property bool usesLiveResize: root.liveResizeActive
                                                         && root.liveResizeMediaId === currentMediaId
                                                         && !localDragging
                    readonly property bool usesLiveAltResize: root.liveAltResizeActive
                                                             && root.liveAltResizeMediaId === currentMediaId
                                                             && !localDragging
                    // usesSnapDrag has NO localDragging guard by design.
                    // liveDragMediaId is cleared before mediaMoveEnded is called, so the
                    // snap freeze must also survive across the localDragging=false transition
                    // until onMediaChanged fires and clears liveSnapDragActive. Without this,
                    // effectiveLocalX/Y would fall back to the raw unsnapped localX/Y for
                    // one frame before the committed model position arrives.
                    readonly property bool usesSnapDrag: root.liveSnapDragActive
                                                        && root.liveSnapDragMediaId === currentMediaId
                    // When C++ has computed a snapped position, use it; otherwise use raw QML drag coords
                    readonly property real effectiveLocalX: usesSnapDrag ? root.liveSnapDragX : localX
                    readonly property real effectiveLocalY: usesSnapDrag ? root.liveSnapDragY : localY
                    width: liveTransform ? liveTransform.width : usesLiveAltResize ? Math.max(1, root.liveAltResizeWidth)
                                              : (media ? Math.max(1, media.width) : 1)
                    height: liveTransform ? liveTransform.height : usesLiveAltResize ? Math.max(1, root.liveAltResizeHeight)
                                              : (media ? Math.max(1, media.height) : 1)
                    readonly property real sceneX: liveTransform ? liveTransform.x : usesLiveAltResize ? root.liveAltResizeX
                                         : (usesLiveResize ? root.liveResizeX : effectiveLocalX)
                    readonly property real sceneY: liveTransform ? liveTransform.y : usesLiveAltResize ? root.liveAltResizeY
                                         : (usesLiveResize ? root.liveResizeY : effectiveLocalY)
                    x: root.panX + sceneX * root.viewScale
                    y: root.panY + sceneY * root.viewScale
                    scale: effectiveScale * root.viewScale
                    transformOrigin: Item.TopLeft
                    z: media ? media.z : 0
                    visible: !!media && media.contentVisible
                    opacity: media ? media.contentOpacity * media.animatedDisplayOpacity : 1.0
                    // Opacity alone does not disable Qt input. Match the picker
                    // for the whole subtree, including TextEdit and MouseArea,
                    // so invisible content cannot swallow another item's press.
                    enabled: opacity > 0

                    MediaVisual {
                        id: mediaContentLoader
                        media: mediaDelegate.media
                        selected: mediaDelegate.isSelected
                        anchors.fill: parent
                        freeResizePreview: !!mediaDelegate.liveTransform && !!mediaDelegate.liveTransform.altResize
                        uniformScalePreview: !!mediaDelegate.liveTransform
                            && mediaDelegate.liveTransform.altResize === false
                        textEditable: root.editingEnabled
                        editingSession: textEditSession
                    }

                    // Connections lives inside mediaDelegate so both `mediaDelegate` and
                    // `root` ids are in scope — Components defined outside the Repeater
                    // cannot access the delegate's id directly.
                    Connections {
                        target: mediaContentLoader.visualItem
                        ignoreUnknownSignals: true
                        function onPrimaryPressed(mediaId, additive) {
                            if (!mediaId)
                                return
                            inputLayer.inputCoordinator.noteMediaPrimaryPress(mediaId, !!additive)
                        }
                        function onTextCommitRequested(mediaId, text) {
                            root.textCommitRequested(mediaId, text)
                        }
                        function onTextLiveUpdateRequested(mediaId, text) {
                            root.textLiveUpdateRequested(mediaId, text)
                        }
                    }

                    }
                }

            }

            Loader {
                parent: viewport
                x: root.panX
                y: root.panY
                scale: root.viewScale
                transformOrigin: Item.TopLeft
                active: root.editingEnabled
                z: 2
                sourceComponent: RemoteCursor {
                    objectName: "canvasRemoteCursor"
                    cursorVisible: root.remoteCursorVisible
                    cursorX: root.remoteCursorX
                    cursorY: root.remoteCursorY
                    diameter: root.remoteCursorDiameter
                    fillColor: root.remoteCursorFill
                    borderColor: root.remoteCursorBorder
                    borderWidth: root.remoteCursorBorderWidth
                }
            }

        Loader {
            id: selectionLayerLoader
            parent: viewport
            anchors.fill: parent
            z: 3
            active: root.editingEnabled
            sourceComponent: SelectionLayer {
                id: selectionLayer
                readonly property alias chromeItem: selectionChromeItem
                anchors.fill: parent
                contentItem: viewport.contentRootItem
                viewportItem: viewport
                interactionController: root
                inputCoordinator: inputLayer ? inputLayer.inputCoordinator : null
                mediaModel: root.mediaModel
                selectionModel: root.selectionChromeModel
                snapGuidesModel: root.snapGuidesModel
                draggedMediaId: root.liveDragMediaId
                dragOffsetViewX: root.liveDragViewOffsetX
                dragOffsetViewY: root.liveDragViewOffsetY
                onMediaResizeRequested: function(mediaId, handleId, sceneX, sceneY, snap, altPressed) {
                    root.mediaResizeRequested(mediaId, handleId, sceneX, sceneY, snap, altPressed)
                }
                onMediaResizeEnded: function(mediaId) {
                    root.mediaResizeEnded(mediaId)
                }

                SnapGuides {
                    id: snapGuides
                    objectName: "canvasSnapGuides"
                    anchors.fill: parent
                    contentItem: viewport.contentRootItem
                    viewportItem: viewport
                    guidesModel: root.snapGuidesModel
                    z: 89000
                }

                SelectionChrome {
                    id: selectionChromeItem
                    objectName: "canvasSelectionChrome"
                    anchors.fill: parent
                    contentItem: viewport.contentRootItem
                    viewportItem: viewport
                    interactionController: root
                    inputCoordinator: inputLayer ? inputLayer.inputCoordinator : null
                    mediaModel: root.mediaModel
                    selectionModel: root.selectionChromeModel
                    // Live drag offset: chrome visually follows the moving item without model repush
                    draggedMediaId: root.liveDragMediaId
                    dragOffsetViewX: root.liveDragViewOffsetX
                    dragOffsetViewY: root.liveDragViewOffsetY
                    z: 90000
                    onResizeRequested: function(mediaId, handleId, sceneX, sceneY, snap, altPressed) {
                        root.mediaResizeRequested(mediaId, handleId, sceneX, sceneY, snap, altPressed)
                    }
                    onResizeEnded: function(mediaId) {
                        root.mediaResizeEnded(mediaId)
                    }
                }
            }
        }

        InputLayer {
            id: inputLayer
            parent: viewport
            anchors.fill: parent
            z: 4
            interactionController: root
            textToolActive: root.textToolActive
            selectionHandlePriorityActive: !!selectionChrome && selectionChrome.interacting
            liveDragMediaId: root.liveDragMediaId
            onTextCreateRequested: function(viewX, viewY) {
                root.textCreateRequested(viewX, viewY)
            }

            // A single native handler owns every media move. Keeping the grab
            // above renderer delegates makes movement independent from Image,
            // VideoOutput, poster-frame and TextEdit subtree lifecycles.
            Item {
                id: mediaMoveInputSurface
                anchors.fill: parent

                containmentMask: QtObject {
                    function contains(p: point): bool {
                        if (!inputLayer.inputCoordinator || root.textToolActive
                                || (selectionChrome && selectionChrome.interacting))
                            return false
                        if (selectionChrome && selectionChrome.hitTestHandle(p.x, p.y))
                            return false
                        var mediaId = root.mediaIdAtPoint(p.x, p.y)
                        if (!mediaId)
                            return false
                        var editor = textEditSession.activeEditor
                        return !editor || editor.mediaId !== mediaId
                    }
                }
            }

            // Use the exact same viewport-space body as selection and movement.
            // Text renderers (and their scaling/clipping/native input subtrees)
            // must not determine which part of a media accepts a double-click.
            TapHandler {
                id: mediaDoubleTap
                parent: mediaMoveInputSurface
                target: null
                acceptedDevices: PointerDevice.Mouse | PointerDevice.TouchPad
                acceptedButtons: Qt.LeftButton
                grabPermissions: PointerHandler.ApprovesTakeOverByAnything
                enabled: root.editingEnabled && !root.textToolActive
                dragThreshold: globalMediaDrag.dragThreshold

                property string firstTappedMediaId: ""
                property var firstTapController: null

                function mediaAtEventPoint(eventPoint) {
                    var p = viewport.mapFromItem(null, eventPoint.scenePosition.x,
                                                eventPoint.scenePosition.y)
                    return root.mediaIdAtPoint(p.x, p.y)
                }

                onTapped: function(eventPoint) {
                    if (mediaDoubleTap.tapCount === 1) {
                        mediaDoubleTap.firstTappedMediaId = mediaDoubleTap.mediaAtEventPoint(eventPoint)
                        mediaDoubleTap.firstTapController = root.canvasController
                    }
                }

                onDoubleTapped: function(eventPoint) {
                    var mediaId = mediaDoubleTap.mediaAtEventPoint(eventPoint)
                    if (!mediaId || mediaId !== mediaDoubleTap.firstTappedMediaId
                            || root.canvasController !== mediaDoubleTap.firstTapController)
                        return
                    var sceneX = eventPoint.scenePosition.x
                    var sceneY = eventPoint.scenePosition.y
                    if (!inputLayer.inputCoordinator.canActivateMediaAtScenePoint(
                            mediaId, sceneX, sceneY))
                        return
                    var delegate = root.mediaDelegateById(mediaId)
                    if (!delegate || !delegate.media || delegate.media.mediaType !== "text")
                        return
                    root.requestTextEditing(mediaId, false,
                        (mediaDoubleTap.point.modifiers & Qt.ShiftModifier) !== 0,
                        sceneX, sceneY)
                }

                onCanceled: mediaDoubleTap.firstTappedMediaId = ""
                onEnabledChanged: if (!enabled) mediaDoubleTap.firstTappedMediaId = ""
            }

            DragHandler {
                id: globalMediaDrag
                parent: mediaMoveInputSurface
                target: null
                acceptedDevices: PointerDevice.Mouse | PointerDevice.TouchPad
                acceptedButtons: Qt.LeftButton
                grabPermissions: PointerHandler.CanTakeOverFromAnything
                enabled: root.editingEnabled
                dragThreshold: 4

                property real pressMediaX: 0.0
                property real pressMediaY: 0.0
                property real pressPointerContentX: 0.0
                property real pressPointerContentY: 0.0
                property real pressPointerViewX: 0.0
                property real pressPointerViewY: 0.0
                property real lastLocalX: 0.0
                property real lastLocalY: 0.0
                property bool lastSnapRequested: false
                property string activeMoveMediaId: ""

                function resetMoveState() {
                    pressMediaX = 0.0
                    pressMediaY = 0.0
                    pressPointerContentX = 0.0
                    pressPointerContentY = 0.0
                    pressPointerViewX = 0.0
                    pressPointerViewY = 0.0
                    lastLocalX = 0.0
                    lastLocalY = 0.0
                    lastSnapRequested = false
                }

                function modelPosition(mediaId) {
                    for (var i = 0; i < root.mediaModel.length; ++i) {
                        var entry = root.mediaModel[i]
                        if (entry && entry.mediaId === mediaId)
                            return Qt.point(entry.x || 0.0, entry.y || 0.0)
                    }
                    return Qt.point(lastLocalX, lastLocalY)
                }

                function finishMoveSession(reason) {
                    var coordinator = inputLayer.inputCoordinator
                    var mediaId = activeMoveMediaId
                    if (!mediaId && coordinator && coordinator.mode === "move")
                        mediaId = coordinator.ownerId || ""
                    if (!mediaId)
                        mediaId = root.liveDragMediaId

                    var hadMoveState = mediaId !== ""
                        || root.activeMediaDragCount > 0
                        || root.liveDragMediaId !== ""
                        || (coordinator && coordinator.mode === "move")
                    if (!hadMoveState) {
                        resetMoveState()
                        return false
                    }

                    var delegateItem = root.mediaDelegateById(mediaId)
                    var fallback = modelPosition(mediaId)
                    var finalX = delegateItem ? delegateItem.effectiveLocalX : fallback.x
                    var finalY = delegateItem ? delegateItem.effectiveLocalY : fallback.y
                    var snapAtEnd = lastSnapRequested
                    var needsSnapCleanup = root.liveSnapDragActive
                        && root.liveSnapDragMediaId === mediaId

                    // Clear every QML latch before publishing the synchronous
                    // backend commit. Model notifications may re-enter the
                    // watchdog from inside mediaMoveEnded().
                    activeMoveMediaId = ""
                    if (delegateItem)
                        delegateItem.localDragging = false
                    root.activeMediaDragCount = 0
                    root.liveDragMediaId = ""
                    root.liveDragViewOffsetX = 0.0
                    root.liveDragViewOffsetY = 0.0

                    if (coordinator && coordinator.mode === "move") {
                        if (coordinator.ownerId === "" || coordinator.ownerId === mediaId)
                            coordinator.endMove(mediaId)
                        else
                            coordinator.forceReset((reason || "recovery") + ":stale-move")
                    }

                    if (mediaId)
                        root.mediaMoveEnded(mediaId, finalX, finalY, snapAtEnd)
                    if (needsSnapCleanup && delegateItem)
                        delegateItem.scheduleSnapFreezeCleanup()
                    resetMoveState()
                    return true
                }

                onActiveChanged: {
                    if (!active) {
                        finishMoveSession("native-release")
                        return
                    }

                    var coordinator = inputLayer.inputCoordinator
                    if (!coordinator || (selectionChrome && selectionChrome.interacting)) {
                        activeMoveMediaId = ""
                        return
                    }
                    var mediaId = coordinator.primaryOwnerMediaId || ""
                    var delegateItem = root.mediaDelegateById(mediaId)
                    if (!delegateItem || !coordinator.tryBeginMove(mediaId)) {
                        activeMoveMediaId = ""
                        return
                    }

                    var pressPoint = globalMediaDrag.centroid.scenePressPosition
                    var pressContent = viewport.contentRootItem.mapFromItem(
                        null, pressPoint.x, pressPoint.y)
                    activeMoveMediaId = mediaId
                    pressPointerContentX = pressContent.x
                    pressPointerContentY = pressContent.y
                    pressPointerViewX = pressPoint.x
                    pressPointerViewY = pressPoint.y
                    pressMediaX = delegateItem.localX
                    pressMediaY = delegateItem.localY
                    lastLocalX = delegateItem.localX
                    lastLocalY = delegateItem.localY
                    lastSnapRequested = (globalMediaDrag.centroid.modifiers
                                         & Qt.ShiftModifier) !== 0

                    delegateItem.localDragging = true
                    root.activeMediaDragCount = 1
                    root.liveDragMediaId = mediaId
                    root.mediaMoveStarted(mediaId, delegateItem.localX,
                                          delegateItem.localY,
                                          lastSnapRequested)
                }

                onTranslationChanged: {
                    if (!root.editingEnabled || !active || !activeMoveMediaId)
                        return
                    var delegateItem = root.mediaDelegateById(activeMoveMediaId)
                    if (!delegateItem) {
                        finishMoveSession("delegate-missing")
                        return
                    }

                    var currentPoint = globalMediaDrag.centroid.scenePosition
                    var currentContent = viewport.contentRootItem.mapFromItem(
                        null, currentPoint.x, currentPoint.y)
                    delegateItem.localX = pressMediaX
                        + currentContent.x - pressPointerContentX
                    delegateItem.localY = pressMediaY
                        + currentContent.y - pressPointerContentY
                    lastSnapRequested = (globalMediaDrag.centroid.modifiers
                                         & Qt.ShiftModifier) !== 0
                    root.mediaMoveUpdated(activeMoveMediaId,
                                          delegateItem.localX,
                                          delegateItem.localY,
                                          lastSnapRequested)
                    if (delegateItem.usesSnapDrag) {
                        delegateItem.localX = root.liveSnapDragX
                        delegateItem.localY = root.liveSnapDragY
                    }
                    lastLocalX = delegateItem.effectiveLocalX
                    lastLocalY = delegateItem.effectiveLocalY
                    root.liveDragViewOffsetX = currentPoint.x - pressPointerViewX
                    root.liveDragViewOffsetY = currentPoint.y - pressPointerViewY
                }

                onCanceled: finishMoveSession("native-cancel")
            }

            PointHandler {
                id: primaryGestureRouter
                target: null
                acceptedDevices: PointerDevice.Mouse | PointerDevice.TouchPad
                acceptedButtons: Qt.LeftButton
                grabPermissions: PointerHandler.ApprovesTakeOverByAnything
                // Always observe the entire native press/release lifecycle,
                // including the press which enters or leaves text editing.
                enabled: true

                onActiveChanged: {
                    if (!inputLayer || !inputLayer.inputCoordinator)
                        return

                    if (active) {
                        ++root.textEditingRequestSerial
                        var viewPoint = point ? point.position : centroid.position
                        var handle = selectionChrome ? selectionChrome.hitTestHandle(viewPoint.x, viewPoint.y) : null
                        var mediaId = root.mediaIdAtPoint(viewPoint.x, viewPoint.y)
                        var editor = textEditSession.activeEditor
                        if (editor && (editor.mediaId !== mediaId || handle)) {
                            textEditSession.finish(editor)
                        }
                        if (!editor || editor.mediaId !== mediaId || handle)
                            root.forceActiveFocus(Qt.MouseFocusReason)
                        var ownerKind = inputLayer.inputCoordinator.beginPrimaryGesture(
                            viewPoint.x,
                            viewPoint.y,
                            handle ? handle.handleId : "",
                            handle ? handle.mediaId : ""
                        )
                        // Dispatch selection from the same atomic hit-test that
                        // assigned ownership. Renderer primary-press callbacks
                        // may also call this method; the per-gesture guard
                        // deduplicates them regardless of delivery order.
                        // Keep clicks inside the active TextEdit untouched.
                        if (ownerKind === "media" && !textEditSession.activeEditor) {
                            var modifiers = Qt.application.keyboardModifiers
                            if (point && point.modifiers !== undefined)
                                modifiers = point.modifiers
                            inputLayer.inputCoordinator.noteMediaPrimaryPress(
                                mediaId, (modifiers & Qt.ShiftModifier) !== 0)
                        }
                        // The same exact press decision drives deselection. A
                        // second TapHandler using hover state can disagree at
                        // handle edges and clear selection during resize.
                        if (ownerKind === "canvas" && !root.textToolActive
                                && root.selectionChromeModel.length > 0)
                            root.clearSelectionRequested()
                    } else {
                        inputLayer.inputCoordinator.endPrimaryGesture()
                    }
                }

                onCanceled: {
                    if (!inputLayer || !inputLayer.inputCoordinator)
                        return
                    inputLayer.inputCoordinator.endPrimaryGesture()
                }
            }

            DragHandler {
                id: panDrag
                // Background pan must never preempt media press-selection.
                // Also disabled while any text item is in edit mode so the passive
                // grab from this DragHandler doesn't block TextEdit cursor placement.
                enabled: !root.anyMediaEditing
                         && root.canStartCanvasPan(panDrag.active)
                         && inputLayer.inputCoordinator.ownerAllowsCanvasPan(panDrag.active)
                target: null
                acceptedDevices: PointerDevice.Mouse
                acceptedButtons: Qt.LeftButton
                dragThreshold: 16
                // Yield to media/resize drags whenever they need the gesture.
                grabPermissions: PointerHandler.ApprovesTakeOverByAnything
                property real lastTranslationX: 0.0
                property real lastTranslationY: 0.0
                property bool panSessionActive: false

                onActiveChanged: {
                    if (active) {
                        var pressPoint = panDrag.centroid.pressPosition
                        var panGranted = inputLayer.inputCoordinator.tryBeginPanAt(pressPoint.x, pressPoint.y)
                        if (!panGranted) {
                            panSessionActive = false
                            return
                        }
                        panSessionActive = true
                        lastTranslationX = 0.0
                        lastTranslationY = 0.0
                    } else {
                        if (panSessionActive)
                            inputLayer.inputCoordinator.endPan()
                        panSessionActive = false
                    }
                }

                onTranslationChanged: {
                    if (!panSessionActive)
                        return
                    root.panBy(translation.x - lastTranslationX,
                               translation.y - lastTranslationY)
                    lastTranslationX = translation.x
                    lastTranslationY = translation.y
                }

                onCanceled: {
                    if (panSessionActive)
                        inputLayer.inputCoordinator.endPan()
                    panSessionActive = false
                }
            }

            DragHandler {
                id: middlePanDrag
                // Middle mouse always pans the camera (including over media).
                enabled: !root.anyMediaEditing && root.canStartCanvasPan(middlePanDrag.active)
                target: null
                acceptedDevices: PointerDevice.Mouse
                acceptedButtons: Qt.MiddleButton
                dragThreshold: 0
                grabPermissions: PointerHandler.TakeOverForbidden
                cursorShape: active ? Qt.ClosedHandCursor : Qt.ArrowCursor
                property real lastTranslationX: 0.0
                property real lastTranslationY: 0.0
                property bool panSessionActive: false

                onActiveChanged: {
                    if (active) {
                        var panGranted = inputLayer.inputCoordinator.beginMode("pan", "canvas")
                        if (!panGranted) {
                            panSessionActive = false
                            return
                        }
                        panSessionActive = true
                        lastTranslationX = 0.0
                        lastTranslationY = 0.0
                    } else {
                        if (panSessionActive)
                            inputLayer.inputCoordinator.endPan()
                        panSessionActive = false
                    }
                }

                onTranslationChanged: {
                    if (!panSessionActive)
                        return
                    root.panBy(translation.x - lastTranslationX,
                               translation.y - lastTranslationY)
                    lastTranslationX = translation.x
                    lastTranslationY = translation.y
                }

                onCanceled: {
                    if (panSessionActive)
                        inputLayer.inputCoordinator.endPan()
                    panSessionActive = false
                }
            }

            PinchHandler {
                id: pinchZoom
                target: null
                enabled: interactionMode === "idle"

                onScaleChanged: function(delta) {
                    if (!active)
                        return
                    // delta is the multiplier for this movement. The deprecated
                    // scale property persists across gestures and must not be
                    // compared with a baseline reset to 1 at each new pinch.
                    root.applyZoomAt(centroid.position.x, centroid.position.y, delta)
                }
            }

            TapHandler {
                id: textToolTap
                target: null
                // Text-create tap is only active in text tool mode.
                enabled: root.canStartTextToolTap()
                acceptedButtons: Qt.LeftButton

                onTapped: function(eventPoint) {
                    inputLayer.inputCoordinator.tryBeginTextCreateAt(eventPoint.position.x,
                                                                      eventPoint.position.y)
                }
            }

            MouseArea {
                anchors.fill: parent
                acceptedButtons: Qt.NoButton
                hoverEnabled: false
                scrollGestureEnabled: true
                preventStealing: false

                onWheel: function(event) {
                    if ((event.modifiers & Qt.AltModifier) !== 0) {
                        // Alt/Option scroll belongs to the selection, including
                        // over empty canvas. Never pan/zoom the camera here.
                        if (event.phase === Qt.ScrollBegin && root.canvasController)
                            root.canvasController.finishSelectionScaleGesture()
                        if (!pinchZoom.active && root.interactionMode === "idle"
                                && !inputLayer.inputCoordinator.primaryGestureActive
                                && root.editingEnabled && root.canvasController) {
                            var delta = root.wheelDeltaY(event)
                            if (event.inverted) delta = -delta
                            if (delta !== 0.0) {
                                var factor = root.isTrackpadWheel(event)
                                    ? Math.exp(delta * root.trackpadZoomSensitivity)
                                    : Math.pow(root.wheelZoomBase, delta * root.wheelZoomSensitivity)
                                root.canvasController.updateSelectionScaleGesture(
                                    factor, event.phase !== Qt.NoScrollPhase)
                            }
                        }
                        // ScrollEnd normally carries no delta. Commit even if
                        // the modifier/interaction state changed during input.
                        if (event.phase === Qt.ScrollEnd && root.canvasController)
                            root.canvasController.finishSelectionScaleGesture()
                        event.accepted = true
                        return
                    }

                    if (root.canvasController)
                        root.canvasController.finishSelectionScaleGesture()
                    if (pinchZoom.active || !root.canProcessCameraWheel()) {
                        event.accepted = true
                        return
                    }

                    if (root.isTrackpadWheel(event)) {
                        var panDx = root.wheelDeltaX(event)
                        var panDy = root.wheelDeltaY(event)
                        if (root.isZoomModifier(event.modifiers)) {
                            // Physical swipe up zooms in, regardless of the
                            // macOS natural-scrolling setting. Keep ordinary
                            // panning in the system's configured direction.
                            var zoomDelta = event.inverted ? -panDy : panDy
                            if (zoomDelta !== 0.0)
                                root.applyZoomAt(event.x, event.y,
                                    Math.exp(zoomDelta * root.trackpadZoomSensitivity))
                        } else if (panDx !== 0.0 || panDy !== 0.0) {
                            root.panBy(panDx, panDy)
                        }
                        event.accepted = true
                        return
                    }

                    var dy = root.wheelDeltaY(event)
                    if (dy !== 0.0) {
                        var factor = Math.pow(root.wheelZoomBase, dy * root.wheelZoomSensitivity)
                        root.applyZoomAt(event.x, event.y, factor)
                    }

                    event.accepted = true
                }
            }

            Timer {
                interval: UiTiming.inputWatchdogIntervalMs
                repeat: true
                running: true
                onTriggered: {
                    root.reconcileInputCoordinatorState("watchdog")
                }
            }
        }
    }

    // ---- Unclipped overlay layer ----
    // Driven by selectionChromeModel whose x/y/width/height already have
    // item scale baked in (width = base * itemScale * sceneUnitScale).
    // Extra metadata (displayName, mediaType, etc.) is looked up from mediaModel.
    Loader {
        anchors.fill: parent
        z: 99000
        active: root.editingEnabled
        sourceComponent: Item {
            id: overlayLayer
            objectName: "canvasMediaOverlays"
            anchors.fill: parent

            Repeater {
                // One overlay pair per selected item — selectionChromeModel
                // is the authoritative source for selected-item geometry.
                model: root.selectionChromeModel
                delegate: Item {
                    id: overlayDelegate

                    // selectionChromeModel entry — has baked width/height
                    readonly property var chromeEntry: modelData
                    readonly property string mid: chromeEntry ? (chromeEntry.mediaId || "") : ""
                    readonly property var liveTransform: root.liveTransforms[mid] || null

                    // Look up the matching mediaModel entry for metadata
                    readonly property var mediaEntry: {
                        var m = mid
                        if (!m) return null
                        for (var i = 0; i < root.mediaModel.length; ++i) {
                            if (root.mediaModel[i].mediaId === m) return root.mediaModel[i]
                        }
                        return null
                    }

                    // Is a live (uniform) resize active for this item?
                    readonly property bool isLiveResizing: root.liveResizeActive && root.liveResizeMediaId === mid
                    // Is a live alt-resize (base-size change) active for this item?
                    readonly property bool isLiveAltResizing: root.liveAltResizeActive && root.liveAltResizeMediaId === mid

                    // Is this item being dragged?
                    readonly property bool isDragging: !liveTransform && root.liveDragMediaId === mid
                    // isSnapDragging intentionally does NOT require isDragging.
                    // liveDragMediaId is cleared before commitMediaTransform updates the model,
                    // so gating on isDragging causes a flicker. The snap freeze clears via
                    // onMediaChanged once the committed position arrives in the model.
                    readonly property bool isSnapDragging: !liveTransform && root.liveSnapDragActive
                                                           && root.liveSnapDragMediaId === mid

                    // Scene position — switch to live resize/alt-resize coords when active.
                    // chromeEntry.x/y are already in QML scene units (scenePos * sceneUnitScale).
                    readonly property real sceneX: liveTransform ? liveTransform.x : isLiveAltResizing ? root.liveAltResizeX
                                                 : (isLiveResizing   ? root.liveResizeX
                                                                      : (chromeEntry ? (chromeEntry.x || 0) : 0))
                    readonly property real sceneY: liveTransform ? liveTransform.y : isLiveAltResizing ? root.liveAltResizeY
                                                 : (isLiveResizing   ? root.liveResizeY
                                                                      : (chromeEntry ? (chromeEntry.y || 0) : 0))

                    // Rendered screen size — chromeEntry.width/height = base * itemScale * sceneUnitScale,
                    // so multiply by viewScale only to get screen pixels.
                    // During live alt-resize use the live width/height directly (already in scene units).
                    // During live uniform resize, recompute from mediaEntry base size * liveResizeScale.
                    readonly property real screenW: {
                        if (liveTransform) return liveTransform.width * liveTransform.scale * root.viewScale
                        if (isLiveAltResizing)
                            return root.liveAltResizeWidth * root.liveAltResizeScale * root.viewScale
                        if (isLiveResizing && mediaEntry)
                            return (mediaEntry.width || 0) * root.liveResizeScale * root.viewScale
                        return (chromeEntry ? (chromeEntry.width || 0) : 0) * root.viewScale
                    }
                    readonly property real screenH: {
                        if (liveTransform) return liveTransform.height * liveTransform.scale * root.viewScale
                        if (isLiveAltResizing)
                            return root.liveAltResizeHeight * root.liveAltResizeScale * root.viewScale
                        if (isLiveResizing && mediaEntry)
                            return (mediaEntry.height || 0) * root.liveResizeScale * root.viewScale
                        return (chromeEntry ? (chromeEntry.height || 0) : 0) * root.viewScale
                    }

                    // Effective drag offset in screen pixels.
                    // When snap is active, derive from snapped scene position so overlay
                    // tracks the item precisely; otherwise use raw viewport delta.
                    readonly property real effectiveDragOffsetX: isSnapDragging
                        ? (root.liveSnapDragX - sceneX) * root.viewScale
                        : root.liveDragViewOffsetX
                    readonly property real effectiveDragOffsetY: isSnapDragging
                        ? (root.liveSnapDragY - sceneY) * root.viewScale
                        : root.liveDragViewOffsetY

                    // Screen-space top-left (add live drag offset if dragging or snap-frozen).
                    // isSnapDragging outlives isDragging by design (snap freeze is cleared by
                    // onMediaChanged after drag ends), so we apply the offset for both states.
                    readonly property real screenLeft: sceneX * root.viewScale + root.panX
                                                       + ((isDragging || isSnapDragging) ? effectiveDragOffsetX : 0)
                    readonly property real screenTop:  sceneY * root.viewScale + root.panY
                                                       + ((isDragging || isSnapDragging) ? effectiveDragOffsetY : 0)

                    // Derived screen anchors
                    readonly property real screenCentreX: screenLeft + screenW * 0.5
                    readonly property real screenBottom:  screenTop  + screenH

                    // Video state for this item — looked up by mediaId in the state dictionary.
                    // Evaluates to null when no state has been pushed for this id.
                    readonly property var videoState: root.videoStateModel[mid] || null

                    // Top overlay: filename + utility buttons
                    MediaTopOverlay {
                        id: topOverlay
                        mediaId: overlayDelegate.mid
                        displayName: overlayDelegate.mediaEntry ? (overlayDelegate.mediaEntry.displayName || "") : ""
                        actionsAvailable: !!overlayDelegate.mediaEntry
                        contentVisible: overlayDelegate.mediaEntry ? (overlayDelegate.mediaEntry.contentVisible !== false) : true
                        visible: true

                        x: overlayDelegate.screenCentreX - panelWidth  * 0.5
                        y: overlayDelegate.screenTop      - panelHeight - 8

                        onVisibilityToggleRequested: function(m, v) { root.overlayVisibilityToggleRequested(m, v) }
                        onBringForwardRequested:     function(m)    { root.overlayBringForwardRequested(m) }
                        onBringBackwardRequested:    function(m)    { root.overlayBringBackwardRequested(m) }
                        onDeleteRequested:           function(m)    { root.overlayDeleteRequested(m) }
                        onOverlayHoveredChanged:     function(h)    { /* input handled by overlay's own MouseArea */ }
                    }

                    // Bottom overlay: video transport controls (video only)
                    MediaVideoOverlay {
                        id: bottomOverlay
                        objectName: "mediaVideoOverlay"
                        mediaId: overlayDelegate.mid
                        visible: root.mediaPlaybackControlsReady(overlayDelegate.mid, overlayDelegate.mediaEntry)
                                 && overlayDelegate.mediaEntry.mediaType === "video"
                        enabled: visible

                        isPlaying: overlayDelegate.videoState ? !!overlayDelegate.videoState.isPlaying : false
                        isMuted:   overlayDelegate.videoState ? !!overlayDelegate.videoState.isMuted   : false
                        isLooping: overlayDelegate.videoState ? !!overlayDelegate.videoState.isLooping : false
                        progress:  overlayDelegate.videoState ? (overlayDelegate.videoState.progress || 0.0) : 0.0
                        startProgress: overlayDelegate.videoState ? overlayDelegate.videoState.startProgress : -1.0
                        endProgress: overlayDelegate.videoState ? overlayDelegate.videoState.endProgress : -1.0
                        volume: overlayDelegate.videoState && overlayDelegate.videoState.volume !== undefined
                                ? overlayDelegate.videoState.volume : 1.0

                        x: overlayDelegate.screenCentreX - panelWidth * 0.5
                        y: overlayDelegate.screenBottom   + 8

                        onPlayPauseRequested:    function(m)    { root.overlayPlayPauseRequested(m) }
                        onStopRequested:         function(m)    { root.overlayStopRequested(m) }
                        onRepeatToggleRequested: function(m)    { root.overlayRepeatToggleRequested(m) }
                        onMuteToggleRequested:   function(m)    { root.overlayMuteToggleRequested(m) }
                        onVolumeChangeRequested:  function(m, v) { root.overlayVolumeChangeRequested(m, v) }
                        onSeekBeginRequested:     function(m, r) { root.overlaySeekBeginRequested(m, r) }
                        onSeekUpdateRequested:    function(m, r) { root.overlaySeekUpdateRequested(m, r) }
                        onSeekEndRequested:       function(m, r) { root.overlaySeekEndRequested(m, r) }
                        onOverlayHoveredChanged:  function(h)    { /* input handled by overlay's own MouseArea */ }
                    }

                    // Bottom overlay: text alignment controls (text only)
                    MediaTextOverlay {
                        id: textOverlay
                        objectName: "mediaTextOverlay"
                        mediaId: overlayDelegate.mid
                        visible: overlayDelegate.mediaEntry
                                 && overlayDelegate.mediaEntry.mediaType === "text"

                        fitToTextEnabled: {
                            var entry = overlayDelegate.mediaEntry
                            if (!entry)
                                return true
                            if (entry.fitToTextEnabled === undefined || entry.fitToTextEnabled === null)
                                return true
                            return !!entry.fitToTextEnabled
                        }
                        horizontalAlignment: overlayDelegate.mediaEntry ? (overlayDelegate.mediaEntry.textHorizontalAlignment || "center") : "center"
                        verticalAlignment:   overlayDelegate.mediaEntry ? (overlayDelegate.mediaEntry.textVerticalAlignment   || "center") : "center"

                        x: overlayDelegate.screenCentreX - panelWidth * 0.5
                        y: overlayDelegate.screenBottom   + 8

                        onFitToTextToggleRequested:  function(m)    { root.overlayFitToTextToggleRequested(m) }
                        onHorizontalAlignRequested:  function(m, a) { root.overlayHorizontalAlignRequested(m, a) }
                        onVerticalAlignRequested:    function(m, a) { root.overlayVerticalAlignRequested(m, a) }
                        onOverlayHoveredChanged:     function(h)    { /* input handled by overlay's own MouseArea */ }
                    }
                }
            }
        }
    }
}
