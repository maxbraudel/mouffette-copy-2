import QtQuick 2.15
import QtQuick.Window 2.15

Rectangle {
    id: root
    color: "#10131a"
    border.width: 1
    border.color: "#2A2E33"
    radius: 5
    clip: true

    signal mediaSelectRequested(string mediaId, bool additive)
    signal mediaMoveStarted(string mediaId, real sceneX, real sceneY, bool snap)
    signal mediaMoveUpdated(string mediaId, real sceneX, real sceneY, bool snap)
    signal mediaMoveEnded(string mediaId, real sceneX, real sceneY, bool snap)
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
    // Three-phase seek protocol — maps directly onto C++ drag-lock handlers:
    //   Begin  : C++ sets m_draggingProgress=true before any seek is issued
    //   Update : C++ seeks to the live scrub position (fires frequently)
    //   End    : C++ issues final seek then clears m_draggingProgress
    signal overlaySeekBeginRequested(string mediaId, real ratio)
    signal overlaySeekUpdateRequested(string mediaId, real ratio)
    signal overlaySeekEndRequested(string mediaId, real ratio)
    signal overlayFitToTextToggleRequested(string mediaId)
    signal overlayHorizontalAlignRequested(string mediaId, string alignment)
    signal overlayVerticalAlignRequested(string mediaId, string alignment)

    property int screenCount: 0
    property bool remoteActive: false
    property var screensModel: []
    property var uiZonesModel: []
    property var mediaModel: []
    // Stable C++ QAbstractListModel — the Repeater binds here so delegates are
    // NEVER destroyed on move/resize commits.  Only dataChanged fires per row.
    // The legacy mediaModel JS-array above is kept for all utility consumers
    // (hit-testing, selection chrome, input layer, etc.).
    property var mediaListModel: null
    property var selectionChromeModel: []
    property var snapGuidesModel: []
    // Live drag tracking — updated every frame during a move drag, purely in QML
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
    property bool textToolActive: false
    property bool remoteCursorVisible: false
    property real remoteCursorX: 0
    property real remoteCursorY: 0
    property int remoteCursorDiameter: 30
    property color remoteCursorFill: "#FFFFFFFF"
    property color remoteCursorBorder: "#E6000000"
    property real remoteCursorBorderWidth: 2
    property real viewScale: 1.0
    property real panX: 0.0
    property real panY: 0.0
    property real minScale: 0.2
    property real maxScale: 10.0
    property real wheelZoomBase: 1.0015
    property real wheelZoomSensitivity: 5.0
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
    // True while any text media item is in text-edit mode. Used to disable canvas
    // pan so parent DragHandlers don't interfere with TextEdit cursor placement.
    property bool anyMediaEditing: false
    // Video state dictionary: keys are mediaId strings, values are state maps.
    // Published every 50 ms by QuickCanvasController for ALL video items
    // (not just the selected one), so overlays remain live after deselection.
    property var videoStateModel: ({})

    focus: true
    Keys.onReleased: function(event) {
        if (event.key === Qt.Key_Shift) {
            // Clear guides only when no active snapped interaction is alive.
            // During fast Shift-release + mouse-release races, clearing unconditionally
            // can hide guides while snap state is still authoritative for the current gesture.
            if (!root.liveSnapDragActive && !selectionChrome.interacting) {
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
        if (!mediaId || mediaId.length === 0)
            return
        root.mediaSelectRequested(mediaId, !!additive)
    }

    function canStartMediaMove(media, contentItem, dragActive, mediaId) {
        if (!inputLayer || !inputLayer.useInputCoordinator) {
            if (!media)
                return false
            if (contentItem && contentItem.editing === true)
                return false
            if (root.textToolActive)
                return false
            if (selectionChrome.interacting)
                return false
            var hoveredId = selectionChrome.hoveredMediaId || ""
            if (hoveredId !== "")
                return false
            return dragActive || inputLayer.inputCoordinator.canStart("move", mediaId)
        }
        return !!inputLayer
            && !!inputLayer.inputCoordinator
            && inputLayer.inputCoordinator.canStartMove(media, contentItem, dragActive, mediaId)
    }

    function canStartCanvasPan(panActive) {
        if (!inputLayer || !inputLayer.useInputCoordinator) {
            return panActive || (interactionMode === "idle"
                && !root.textToolActive
                && !selectionChrome.interacting
                && !selectionChrome.handlePriorityActive
                && root.liveDragMediaId === "")
        }
        return !!inputLayer
            && !!inputLayer.inputCoordinator
            && inputLayer.inputCoordinator.canEnablePan(panActive)
    }

    function canStartTextToolTap() {
        if (!inputLayer || !inputLayer.useInputCoordinator) {
            return interactionMode === "idle"
                && root.textToolActive
                && !selectionChrome.interacting
                && !selectionChrome.handlePriorityActive
                && root.liveDragMediaId === ""
        }
        return !!inputLayer
            && !!inputLayer.inputCoordinator
            && inputLayer.inputCoordinator.canStartTextToolTap()
    }

    function clampScale(value) {
        return Math.max(minScale, Math.min(maxScale, value))
    }

    function applyZoomAt(anchorX, anchorY, factor) {
        if (!isFiniteNumber(factor) || factor <= 0.0)
            return

        var oldScale = viewScale
        var nextScale = clampScale(oldScale * factor)
        if (!isFiniteNumber(nextScale) || Math.abs(nextScale - oldScale) < 0.000001)
            return

        var worldX = (anchorX - panX) / oldScale
        var worldY = (anchorY - panY) / oldScale

        viewScale = nextScale
        panX = anchorX - worldX * nextScale
        panY = anchorY - worldY * nextScale
    }

    function fitToScreens(marginPx) {
        if (!screensModel || screensModel.length === 0)
            return false

        var minX = Number.POSITIVE_INFINITY
        var minY = Number.POSITIVE_INFINITY
        var maxX = Number.NEGATIVE_INFINITY
        var maxY = Number.NEGATIVE_INFINITY

        for (var i = 0; i < screensModel.length; ++i) {
            var s = screensModel[i]
            if (!s)
                continue
            minX = Math.min(minX, s.x)
            minY = Math.min(minY, s.y)
            maxX = Math.max(maxX, s.x + s.width)
            maxY = Math.max(maxY, s.y + s.height)
        }

        if (!isFiniteNumber(minX) || !isFiniteNumber(minY) || !isFiniteNumber(maxX) || !isFiniteNumber(maxY))
            return false

        var boundsW = Math.max(1.0, maxX - minX)
        var boundsH = Math.max(1.0, maxY - minY)
        var margin = Math.max(0.0, marginPx || 0.0)
        var availW = Math.max(1.0, viewport.width - margin * 2.0)
        var availH = Math.max(1.0, viewport.height - margin * 2.0)

        var sx = availW / boundsW
        var sy = availH / boundsH
        var fitScale = Math.min(sx, sy)
        if (!isFiniteNumber(fitScale) || fitScale <= 0.0)
            return false

        var cx = (minX + maxX) * 0.5
        var cy = (minY + maxY) * 0.5

        viewScale = fitScale
        panX = viewport.width * 0.5 - cx * fitScale
        panY = viewport.height * 0.5 - cy * fitScale
        return true
    }

    function recenterView(marginPx) {
        if (fitToScreens(marginPx === undefined ? 53 : marginPx))
            return
        panX = 0.0
        panY = 0.0
        viewScale = 1.0
    }

    function fitToBounds(boundsX, boundsY, boundsW, boundsH, marginPx) {
        if (!isFiniteNumber(boundsW) || !isFiniteNumber(boundsH) || boundsW <= 0.0 || boundsH <= 0.0) {
            recenterView()
            return
        }

        var margin = Math.max(0.0, marginPx || 0.0)
        var availW = Math.max(1.0, viewport.width - margin * 2.0)
        var availH = Math.max(1.0, viewport.height - margin * 2.0)
        var sx = availW / boundsW
        var sy = availH / boundsH
        var targetScale = clampScale(Math.min(sx, sy))

        if (!isFiniteNumber(targetScale) || targetScale <= 0.0)
            targetScale = 1.0

        viewScale = targetScale

        var contentW = boundsW * targetScale
        var contentH = boundsH * targetScale
        var left = margin + (availW - contentW) * 0.5
        var top = margin + (availH - contentH) * 0.5

        panX = left - boundsX * targetScale
        panY = top - boundsY * targetScale
    }

    function isZoomModifier(modifiers) {
        if (Qt.platform.os === "osx") {
            return (modifiers & Qt.MetaModifier) !== 0
        }
        return (modifiers & Qt.ControlModifier) !== 0
    }

    function wheelDeltaY(wheel) {
        if (wheel.pixelDelta && wheel.pixelDelta.y !== 0)
            return wheel.pixelDelta.y
        if (wheel.angleDelta)
            return wheel.angleDelta.y / 8.0
        return 0.0
    }

    function wheelDeltaX(wheel) {
        if (wheel.pixelDelta && wheel.pixelDelta.x !== 0)
            return wheel.pixelDelta.x
        if (wheel.angleDelta)
            return wheel.angleDelta.x / 8.0
        return 0.0
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
        if (!inputLayer || !inputLayer.useInputCoordinator || !inputLayer.inputCoordinator)
            return

        var coordinator = inputLayer.inputCoordinator
        var mode = coordinator.mode
        var owner = coordinator.ownerId || ""

        if (mode === "idle")
            return

        if (mode === "move") {
            if (activeMediaDragCount <= 0
                    || liveDragMediaId === ""
                    || owner === ""
                    || liveDragMediaId !== owner
                    || !mediaModelContainsId(owner)) {
                coordinator.forceReset(reason + ":stale-move")
            }
            return
        }

        if (mode === "resize") {
            if (!selectionChrome || !selectionChrome.interacting || owner === "" || !mediaModelContainsId(owner)) {
                coordinator.forceReset(reason + ":stale-resize")
            }
            return
        }

        if (mode === "pan") {
            if (!panDrag || !panDrag.active) {
                coordinator.forceReset(reason + ":stale-pan")
            }
            return
        }

        if (mode === "text") {
            coordinator.forceReset(reason + ":stale-text")
        }
    }

    onMediaModelChanged: {
        reconcileInputCoordinatorState("media-model-changed")
    }

    onSelectionChromeModelChanged: {
        reconcileInputCoordinatorState("selection-model-changed")
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
                anchors.fill: parent
                mediaModel: root.mediaModel
                contentItem: viewport.contentRootItem
                sourceUrlResolver: root.mediaSourceUrl
                onMediaSelectRequested: function(mediaId, additive) {
                    root.mediaSelectRequested(mediaId, additive)
                }
                onMediaMoveEnded: function(mediaId, sceneX, sceneY) {
                    root.mediaMoveEnded(mediaId, sceneX, sceneY)
                }
                onTextCommitRequested: function(mediaId, text) {
                    root.textCommitRequested(mediaId, text)
                }

                // Each media item is a native QML Item with its own local x/y/scale.
                // A DragHandler lives here (inside contentRoot, which has scale:viewScale)
                // so DragHandler translation is already in scene coordinates — no C++ per frame.
                Repeater {
                    id: mediaRepeater
                    // Use the stable C++ QAbstractListModel so that move/resize
                    // commits issue dataChanged per row rather than a full
                    // model replacement that would destroy every delegate.
                    model: root.mediaListModel
                    delegate: Item {
                    id: mediaDelegate
                    property var media: modelData

                    // Local position/scale — tracks model when idle, free during drag
                    property real localX: media ? media.x : 0.0
                    property real localY: media ? media.y : 0.0
                    property real localScale: media ? (media.scale || 1.0) : 1.0
                    property bool localDragging: false
                    property bool overlayHovered: false
                    // Actual rendered scale — switches to live scale during resize/alt-resize
                    // so overlay counter-scale stays correct every frame.
                    readonly property real effectiveScale: usesLiveAltResize ? root.liveAltResizeScale
                                                         : (usesLiveResize ? root.liveResizeScale : localScale)
                    readonly property string currentMediaId: media ? (media.mediaId || "") : ""
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
                        interval: 300
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
                        if (!mediaDrag)
                            return
                        if (!mediaDrag.active && mediaDrag.activeMoveMediaId === "" && !mediaDelegate.localDragging)
                            return

                        var orphanMediaId = mediaDrag.activeMoveMediaId
                        if (!orphanMediaId || orphanMediaId.length === 0)
                            orphanMediaId = mediaDelegate.currentMediaId

                        if (mediaDrag.countedAsActive) {
                            root.activeMediaDragCount = Math.max(0, root.activeMediaDragCount - 1)
                            mediaDrag.countedAsActive = false
                        }

                        mediaDelegate.localDragging = false
                        root.liveDragMediaId = ""
                        root.liveDragViewOffsetX = 0.0
                        root.liveDragViewOffsetY = 0.0

                        if (inputLayer && inputLayer.inputCoordinator) {
                            if (inputLayer.useInputCoordinator) {
                                inputLayer.inputCoordinator.forceReset("media-delegate-destroyed")
                            } else {
                                inputLayer.inputCoordinator.endMode("move", orphanMediaId)
                            }
                        }
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
                    width:  usesLiveAltResize ? Math.max(1, root.liveAltResizeWidth)
                                              : (media ? Math.max(1, media.width) : 1)
                    height: usesLiveAltResize ? Math.max(1, root.liveAltResizeHeight)
                                              : (media ? Math.max(1, media.height) : 1)
                    x: usesLiveAltResize ? root.liveAltResizeX
                                         : (usesLiveResize ? root.liveResizeX : effectiveLocalX)
                    y: usesLiveAltResize ? root.liveAltResizeY
                                         : (usesLiveResize ? root.liveResizeY : effectiveLocalY)
                    scale: usesLiveAltResize ? root.liveAltResizeScale
                                             : (usesLiveResize ? root.liveResizeScale : localScale)
                    transformOrigin: Item.TopLeft
                    z: media ? media.z : 0
                    visible: !!media && (media.contentVisible !== false)
                    opacity: media ? ((media.contentOpacity !== undefined ? media.contentOpacity : 1.0)
                                    * (media.animatedDisplayOpacity !== undefined ? media.animatedDisplayOpacity : 1.0))
                                   : 1.0

                    // DragHandler inside contentRoot: translation is in scene coords (contentRoot
                    // has scale:viewScale, so 1 unit here = 1 scene unit, not viewport pixel).

                    // Double-click handler: lives at the mediaDelegate PointerHandler level
                    // because parent PointerHandlers receive events BEFORE child MouseAreas
                    // in Qt Quick 6's event delivery order. tapCount===2 is the reliable
                    // cross-platform double-click detection path for PointerHandlers.
                    TapHandler {
                        id: mediaDoubleClick
                        target: null
                        acceptedDevices: PointerDevice.Mouse | PointerDevice.TouchPad
                        acceptedButtons: Qt.LeftButton
                        // ApprovesTakeOverByAnything: TapHandler will not steal grabs from
                        // DragHandler/PointHandler, but will immediately yield its grab
                        // to the DragHandler once the drag threshold is exceeded. This
                        // prevents double-click detection from blocking media drag.
                        grabPermissions: PointerHandler.ApprovesTakeOverByAnything
                        enabled: !!mediaDelegate.media
                                 && !mediaDelegate.overlayHovered
                                 && !(mediaContentLoader.item && mediaContentLoader.item.editing === true)
                                 && selectionChrome.hoveredHandleId === ""

                        onTapped: function(eventPoint) {
                            if (tapCount !== 2)
                                return
                            var item = mediaContentLoader.item
                            if (!item || typeof item.fireDoubleClick !== "function")
                                return
                            var additive = (eventPoint.modifiers & Qt.ShiftModifier) !== 0
                            item.fireDoubleClick(additive)
                        }
                    }

                    PointHandler {
                        id: mediaPressSelect
                        target: null
                        acceptedDevices: PointerDevice.Mouse | PointerDevice.TouchPad
                        acceptedButtons: Qt.LeftButton
                        grabPermissions: PointerHandler.TakeOverForbidden
                        enabled: !!mediaDelegate.media
                                 && !mediaDelegate.overlayHovered
                                 && !(mediaContentLoader.item && mediaContentLoader.item.editing === true)
                                 && selectionChrome.hoveredHandleId === ""

                        function selectNow(modifiers) {
                            var mediaId = mediaDelegate.currentMediaId
                            if (!mediaId || mediaId.length === 0)
                                return

                            var additive = (modifiers & Qt.ShiftModifier) !== 0

                            if (inputLayer && inputLayer.useInputCoordinator) {
                                inputLayer.inputCoordinator.noteMediaPrimaryPress(mediaId, additive)
                            } else {
                                root.requestMediaSelection(mediaId, additive)
                            }
                        }

                        onActiveChanged: {
                            if (!active)
                                return

                            var modifiers = Qt.application.keyboardModifiers
                            if (mediaPressSelect.point && mediaPressSelect.point.modifiers !== undefined) {
                                modifiers = mediaPressSelect.point.modifiers
                            }
                            selectNow(modifiers)
                        }
                    }

                    DragHandler {
                        id: mediaDrag
                        target: null
                        acceptedDevices: PointerDevice.Mouse | PointerDevice.TouchPad
                        acceptedButtons: Qt.LeftButton
                        enabled: !mediaDelegate.overlayHovered
                                 && root.canStartMediaMove(mediaDelegate.media,
                                                        mediaContentLoader.item,
                                                        mediaDrag.active,
                                                        mediaDelegate.currentMediaId)
                        dragThreshold: 4

                        property real startX: 0.0
                        property real startY: 0.0
                        property real dragOriginSceneX: 0.0
                        property real dragOriginSceneY: 0.0
                        property real dragOriginViewX: 0.0
                        property real dragOriginViewY: 0.0
                        property string activeMoveMediaId: ""
                        property bool countedAsActive: false

                        onActiveChanged: {
                            if (active) {
                                activeMoveMediaId = mediaDelegate.currentMediaId
                                // Acquire the coordinator lock first. Multiple DragHandlers
                                // can fire simultaneously (overlapping items); only the first
                                // caller to tryBeginMove wins. Do NOT touch selection before
                                // this check — if we lose the lock the wrong item would be
                                // selected.
                                var moveGranted = inputLayer.useInputCoordinator
                                    ? inputLayer.inputCoordinator.tryBeginMove(activeMoveMediaId)
                                    : inputLayer.inputCoordinator.beginMode("move", activeMoveMediaId)
                                if (!moveGranted) {
                                    activeMoveMediaId = ""
                                    mediaDelegate.localDragging = false
                                    root.liveDragMediaId = ""
                                    root.liveDragViewOffsetX = 0.0
                                    root.liveDragViewOffsetY = 0.0
                                    countedAsActive = false
                                    return
                                }
                                if (!countedAsActive) {
                                    root.activeMediaDragCount += 1
                                    countedAsActive = true
                                }
                                // Select the item being dragged (handles cases where the press
                                // selection signal hasn't been processed by C++ yet).
                                if (inputLayer && inputLayer.useInputCoordinator) {
                                    inputLayer.inputCoordinator.noteMediaPrimaryPress(activeMoveMediaId, false)
                                } else if (activeMoveMediaId) {
                                    root.mediaSelectRequested(activeMoveMediaId, false)
                                }
                                startX = mediaDelegate.localX
                                startY = mediaDelegate.localY
                                var startPoint = mediaDrag.centroid.scenePosition
                                var startScene = viewport.contentRootItem.mapFromItem(null, startPoint.x, startPoint.y)
                                dragOriginSceneX = startScene.x
                                dragOriginSceneY = startScene.y
                                dragOriginViewX = startPoint.x
                                dragOriginViewY = startPoint.y
                                mediaDelegate.localDragging = true
                                root.liveDragMediaId = activeMoveMediaId
                                var snapAtStart = (mediaDrag.centroid.modifiers & Qt.ShiftModifier) !== 0
                                root.mediaMoveStarted(activeMoveMediaId,
                                                      mediaDelegate.localX,
                                                      mediaDelegate.localY,
                                                      snapAtStart)
                            } else {
                                var finalMediaId = activeMoveMediaId
                                if (countedAsActive) {
                                    root.activeMediaDragCount = Math.max(0, root.activeMediaDragCount - 1)
                                    countedAsActive = false
                                }
                                mediaDelegate.localDragging = false
                                dragOriginSceneX = 0.0
                                dragOriginSceneY = 0.0
                                dragOriginViewX = 0.0
                                dragOriginViewY = 0.0
                                // liveDragViewOffsetX/Y are cleared now (no longer needed visually).
                                root.liveDragViewOffsetX = 0.0
                                root.liveDragViewOffsetY = 0.0
                                if (inputLayer.useInputCoordinator) {
                                    inputLayer.inputCoordinator.endMove(finalMediaId)
                                } else {
                                    inputLayer.inputCoordinator.endMode("move", finalMediaId)
                                }
                                // Clear liveDragMediaId BEFORE mediaMoveEnded so that when
                                // onMediaChanged fires synchronously inside C++ handleMediaMoveEnded
                                // (triggered by commitMediaTransform → commitMediaGeometry), the
                                // snap freeze check in onMediaChanged sees liveDragMediaId=="" and
                                // reliably clears the freeze via the fallback branch.
                                // This eliminates the race where the fallback was skipped because
                                // liveDragMediaId was still set at the time onMediaChanged evaluated.
                                root.liveDragMediaId = ""
                                activeMoveMediaId = ""

                                if (finalMediaId !== "") {
                                    var snapAtEnd = (mediaDrag.centroid.modifiers & Qt.ShiftModifier) !== 0
                                    // Send effectiveLocalX/Y (the snapped position the user sees)
                                    // instead of raw localX/Y (raw cursor position). This ensures
                                    // C++ commits the position that matches the visual.
                                    root.mediaMoveEnded(finalMediaId,
                                                        mediaDelegate.effectiveLocalX,
                                                        mediaDelegate.effectiveLocalY,
                                                        snapAtEnd)
                                }
                                // Arm the safety-net timer in case onMediaChanged never fires
                                // (e.g. commitMediaGeometry found no matching entry in mediaModel).
                                if (typeof root !== "undefined" && root.liveSnapDragActive
                                        && root.liveSnapDragMediaId === finalMediaId) {
                                    snapFreezeCleanupTimer.restart()
                                }
                            }
                        }

                        onTranslationChanged: {
                            if (!active || !mediaDelegate.localDragging) return
                            // Use explicit scene-position mapping to avoid any coordinate
                            // ambiguity from contentRoot's scale transform.
                            var cur = mediaDrag.centroid.scenePosition
                            var curScene = viewport.contentRootItem.mapFromItem(null, cur.x, cur.y)
                            mediaDelegate.localX = startX + (curScene.x - dragOriginSceneX)
                            mediaDelegate.localY = startY + (curScene.y - dragOriginSceneY)
                            if (activeMoveMediaId !== "") {
                                var snapNow = (mediaDrag.centroid.modifiers & Qt.ShiftModifier) !== 0
                                root.mediaMoveUpdated(activeMoveMediaId,
                                                      mediaDelegate.localX,
                                                      mediaDelegate.localY,
                                                      snapNow)
                            }
                            // After mediaMoveUpdated returns, C++ has synchronously pushed
                            // liveSnapDragX/Y to the snapped position (or cleared the freeze
                            // if snap is no longer active). Keep localX/Y in sync with the
                            // snapped position so that whenever usesSnapDrag flips to false
                            // for any reason, effectiveLocalX falls back to localX which
                            // already holds the correct snapped value — not the raw cursor.
                            if (mediaDelegate.usesSnapDrag) {
                                mediaDelegate.localX = root.liveSnapDragX
                                mediaDelegate.localY = root.liveSnapDragY
                            }
                            // Chrome viewport offset = scene delta * viewScale
                            root.liveDragViewOffsetX = cur.x - dragOriginViewX
                            root.liveDragViewOffsetY = cur.y - dragOriginViewY
                        }

                        onCanceled: {
                            if (countedAsActive) {
                                root.activeMediaDragCount = Math.max(0, root.activeMediaDragCount - 1)
                                countedAsActive = false
                            }
                            mediaDelegate.localDragging = false
                            dragOriginSceneX = 0.0
                            dragOriginSceneY = 0.0
                            dragOriginViewX = 0.0
                            dragOriginViewY = 0.0
                            root.liveDragMediaId = ""
                            root.liveDragViewOffsetX = 0.0
                            root.liveDragViewOffsetY = 0.0
                            // No commit path on cancel — onMediaChanged may never fire.
                            // Arm the safety-net timer so the freeze is force-cleared after
                            // a short window if nothing else clears it first.
                            if (root.liveSnapDragActive
                                    && root.liveSnapDragMediaId === activeMoveMediaId) {
                                snapFreezeCleanupTimer.restart()
                            }
                            if (inputLayer.useInputCoordinator) {
                                inputLayer.inputCoordinator.endMove(activeMoveMediaId)
                            } else {
                                inputLayer.inputCoordinator.endMode("move", activeMoveMediaId)
                            }
                            activeMoveMediaId = ""
                        }
                    }

                    Loader {
                        id: mediaContentLoader
                        property var media: mediaDelegate.media
                        property bool selected: mediaDelegate.isSelected
                        readonly property real liveWidth:  mediaDelegate.usesLiveAltResize
                                                          ? root.liveAltResizeWidth
                                                          : (media ? media.width : 0)
                        readonly property real liveHeight: mediaDelegate.usesLiveAltResize
                                                          ? root.liveAltResizeHeight
                                                          : (media ? media.height : 0)
                        sourceComponent: {
                            if (!mediaDelegate.media) return null
                            if (mediaDelegate.media.mediaType === "video") return videoDelegate
                            if (mediaDelegate.media.mediaType === "text") return textDelegate
                            return imageDelegate
                        }
                    }

                    Binding {
                        target: mediaContentLoader.item
                        property: "mediaWidth"
                        value: mediaContentLoader.liveWidth
                        when: mediaContentLoader.item !== null && mediaDelegate.usesLiveAltResize
                    }

                    Binding {
                        target: mediaContentLoader.item
                        property: "mediaHeight"
                        value: mediaContentLoader.liveHeight
                        when: mediaContentLoader.item !== null && mediaDelegate.usesLiveAltResize
                    }

                    // Connections lives inside mediaDelegate so both `mediaDelegate` and
                    // `root` ids are in scope — Components defined outside the Repeater
                    // cannot access the delegate's id directly.
                    Connections {
                        target: mediaContentLoader.item
                        ignoreUnknownSignals: true
                        function onSelectRequested(mediaId, additive) {
                            if (inputLayer.useInputCoordinator) {
                                inputLayer.inputCoordinator.noteMediaPrimaryPress(mediaId, additive)
                            } else {
                                root.requestMediaSelection(mediaId, additive)
                            }
                        }
                        function onPrimaryPressed(mediaId, additive) {
                            if (inputLayer.useInputCoordinator) {
                                inputLayer.inputCoordinator.noteMediaPrimaryPress(mediaId, additive)
                            } else {
                                root.requestMediaSelection(mediaId, additive)
                            }
                        }
                        function onTextCommitRequested(mediaId, text) {
                            root.textCommitRequested(mediaId, text)
                        }
                        function onTextLiveUpdateRequested(mediaId, text) {
                            root.textLiveUpdateRequested(mediaId, text)
                        }
                        // Track whether any text item is currently in edit mode so
                        // canvas pan handlers can be disabled while editing.
                        function onEditingChanged() {
                            root.anyMediaEditing = !!(mediaContentLoader.item
                                                     && mediaContentLoader.item.editing === true)
                        }
                    }

                    }
                }

                // Components receive mediaX:0/mediaY:0 and mediaScale:1 — the parent
                // mediaDelegate Item handles all position/scale/z transforms.
                Component {
                    id: imageDelegate
                    ImageItem {
                        mediaId: parent.media ? (parent.media.mediaId || "") : ""
                        mediaX: 0
                        mediaY: 0
                        mediaWidth: parent.media ? parent.media.width : 0
                        mediaHeight: parent.media ? parent.media.height : 0
                        mediaScale: 1.0
                        mediaZ: parent.media.z
                        selected: !!parent.selected
                        imageSource: parent.media
                                   ? (parent.media.sourceUrl || root.mediaSourceUrl(parent.media.sourcePath || ""))
                                   : ""
                    }
                }

                Component {
                    id: videoDelegate
                    VideoItem {
                        mediaId: parent.media ? (parent.media.mediaId || "") : ""
                        mediaX: 0
                        mediaY: 0
                        mediaWidth: parent.media ? parent.media.width : 0
                        mediaHeight: parent.media ? parent.media.height : 0
                        mediaScale: 1.0
                        mediaZ: parent.media.z
                        selected: !!parent.selected
                        cppMediaPlayer: parent.media ? (parent.media.videoPlayerPtr || null) : null
                        cppVideoSink: parent.media ? (parent.media.videoSinkPtr || null) : null
                        videoPlaybackErrorCode: parent.media ? (parent.media.videoPlaybackErrorCode || 0) : 0
                        videoPlaybackErrorString: parent.media ? (parent.media.videoPlaybackErrorString || "") : ""
                        videoHasRenderedFrame: !!(parent.media && parent.media.videoHasRenderedFrame)
                        videoHasPosterFrame: !!(parent.media && parent.media.videoHasPosterFrame)
                        videoFirstFramePrimed: !!(parent.media && parent.media.videoFirstFramePrimed)
                    }
                }

                Component {
                    id: textDelegate
                    TextItem {
                        mediaId: parent.media ? (parent.media.mediaId || "") : ""
                        mediaX: 0
                        mediaY: 0
                        mediaWidth: parent.media ? parent.media.width : 0
                        mediaHeight: parent.media ? parent.media.height : 0
                        mediaScale: 1.0
                        mediaZ: parent.media.z
                        selected: !!parent.selected
                        textContent: parent.media ? (parent.media.textContent || "") : ""
                        horizontalAlignment: parent.media ? (parent.media.textHorizontalAlignment || "center") : "center"
                        verticalAlignment: parent.media ? (parent.media.textVerticalAlignment || "center") : "center"
                        fitToTextEnabled: !!(parent.media && parent.media.fitToTextEnabled)
                        fontFamily: parent.media ? (parent.media.textFontFamily || "Arial") : "Arial"
                        fontPixelSize: Math.max(1, parent.media ? (parent.media.textFontPixelSize || 22) : 22)
                        fontWeight: parent.media ? (parent.media.textFontWeight || 400) : 400
                        fontItalic: !!(parent.media && parent.media.textItalic)
                        fontUnderline: !!(parent.media && parent.media.textUnderline)
                        fontUppercase: !!(parent.media && parent.media.textUppercase)
                        textColor: parent.media ? (parent.media.textColor || "#FFFFFFFF") : "#FFFFFFFF"
                        outlineWidthPercent: parent.media ? (parent.media.textOutlineWidthPercent || 0.0) : 0.0
                        outlineColor: parent.media ? (parent.media.textOutlineColor || "#FF000000") : "#FF000000"
                        highlightEnabled: !!(parent.media && parent.media.textHighlightEnabled)
                        highlightColor: parent.media ? (parent.media.textHighlightColor || "#00000000") : "#00000000"
                        textEditable: !!(parent.media && parent.media.textEditable)
                    }
                }
            }

            RemoteCursor {
                cursorVisible: root.remoteCursorVisible
                cursorX: root.remoteCursorX
                cursorY: root.remoteCursorY
                diameter: root.remoteCursorDiameter
                fillColor: root.remoteCursorFill
                borderColor: root.remoteCursorBorder
                borderWidth: root.remoteCursorBorderWidth
            }

        SelectionLayer {
            id: selectionLayer
            parent: viewport
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
                anchors.fill: parent
                contentItem: viewport.contentRootItem
                viewportItem: viewport
                guidesModel: root.snapGuidesModel
                z: 89000
            }

            SelectionChrome {
                id: selectionChrome
                anchors.fill: parent
                contentItem: viewport.contentRootItem
                viewportItem: viewport
                interactionController: root
                inputCoordinator: inputLayer ? inputLayer.inputCoordinator : null
                useInputCoordinator: inputLayer ? inputLayer.useInputCoordinator : true
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

        InputLayer {
            id: inputLayer
            parent: viewport
            anchors.fill: parent
            interactionController: root
            textToolActive: root.textToolActive
            mediaModel: root.mediaModel
            contentItem: viewport.contentRootItem
            selectionHandlePriorityActive: selectionChrome.interacting
            selectionHandleHoveredMediaId: selectionChrome.hoveredMediaId
            liveDragMediaId: root.liveDragMediaId
            onTextCreateRequested: function(viewX, viewY) {
                root.textCreateRequested(viewX, viewY)
            }

            DragHandler {
                id: panDrag
                // Background pan must never preempt media press-selection.
                // Also disabled while any text item is in edit mode so the passive
                // grab from this DragHandler doesn't block TextEdit cursor placement.
                enabled: !root.anyMediaEditing && root.canStartCanvasPan(panDrag.active)
                target: null
                acceptedDevices: PointerDevice.Mouse
                acceptedButtons: Qt.LeftButton
                dragThreshold: 16
                grabPermissions: PointerHandler.TakeOverForbidden
                property real startPanX: 0.0
                property real startPanY: 0.0
                property bool panSessionActive: false

                onActiveChanged: {
                    if (active) {
                        var panGranted = false
                        if (inputLayer.useInputCoordinator) {
                            var pressPoint = panDrag.centroid.scenePressPosition
                            panGranted = inputLayer.inputCoordinator.tryBeginPanAt(pressPoint.x, pressPoint.y)
                        } else {
                            panGranted = inputLayer.inputCoordinator.beginMode("pan", "canvas")
                        }
                        if (!panGranted) {
                            panSessionActive = false
                            return
                        }
                        panSessionActive = true
                        startPanX = root.panX
                        startPanY = root.panY
                    } else {
                        panSessionActive = false
                        if (inputLayer.useInputCoordinator) {
                            inputLayer.inputCoordinator.endPan()
                        } else {
                            inputLayer.inputCoordinator.endMode("pan", "canvas")
                        }
                    }
                }

                onTranslationChanged: {
                    if (!panSessionActive)
                        return
                    root.panX = startPanX + translation.x
                    root.panY = startPanY + translation.y
                }

                onCanceled: {
                    panSessionActive = false
                    if (inputLayer.useInputCoordinator) {
                        inputLayer.inputCoordinator.endPan()
                    } else {
                        inputLayer.inputCoordinator.endMode("pan", "canvas")
                    }
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
                property real startPanX: 0.0
                property real startPanY: 0.0
                property bool panSessionActive: false

                onActiveChanged: {
                    if (active) {
                        var panGranted = false
                        if (inputLayer.useInputCoordinator) {
                            panGranted = inputLayer.inputCoordinator.beginMode("pan", "canvas")
                        } else {
                            panGranted = inputLayer.inputCoordinator.beginMode("pan", "canvas")
                        }
                        if (!panGranted) {
                            panSessionActive = false
                            return
                        }
                        panSessionActive = true
                        startPanX = root.panX
                        startPanY = root.panY
                    } else {
                        panSessionActive = false
                        if (inputLayer.useInputCoordinator) {
                            inputLayer.inputCoordinator.endPan()
                        } else {
                            inputLayer.inputCoordinator.endMode("pan", "canvas")
                        }
                    }
                }

                onTranslationChanged: {
                    if (!panSessionActive)
                        return
                    root.panX = startPanX + translation.x
                    root.panY = startPanY + translation.y
                }

                onCanceled: {
                    panSessionActive = false
                    if (inputLayer.useInputCoordinator) {
                        inputLayer.inputCoordinator.endPan()
                    } else {
                        inputLayer.inputCoordinator.endMode("pan", "canvas")
                    }
                }
            }

            PinchHandler {
                id: pinchZoom
                target: null
                enabled: interactionMode === "idle"
                property real lastScale: 1.0

                onActiveChanged: {
                    if (active) {
                        lastScale = 1.0
                    }
                }

                onScaleChanged: {
                    if (!active)
                        return
                    var factor = scale / lastScale
                    if (isFiniteNumber(factor) && factor > 0.0)
                        root.applyZoomAt(centroid.position.x, centroid.position.y, factor)
                    lastScale = scale
                }
            }

            TapHandler {
                id: textToolTap
                target: null
                // Text-create tap is only active in text tool mode.
                enabled: root.canStartTextToolTap()
                acceptedButtons: Qt.LeftButton

                onTapped: function(eventPoint) {
                    if (inputLayer.useInputCoordinator) {
                        inputLayer.inputCoordinator.tryBeginTextCreateAt(eventPoint.position.x,
                                                                          eventPoint.position.y)
                    } else {
                        root.textCreateRequested(eventPoint.position.x, eventPoint.position.y)
                    }
                }
            }

            MouseArea {
                anchors.fill: parent
                acceptedButtons: Qt.NoButton
                hoverEnabled: false
                scrollGestureEnabled: true
                preventStealing: false

                onWheel: function(event) {
                    if (!root.canProcessCameraWheel()) {
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
                interval: 120
                repeat: true
                running: inputLayer.useInputCoordinator
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
    Item {
        id: overlayLayer
        anchors.fill: parent
        z: 99000

        Repeater {
            // One overlay pair per selected item — selectionChromeModel
            // is the authoritative source for selected-item geometry.
            model: root.selectionChromeModel
            delegate: Item {
                id: overlayDelegate

                // selectionChromeModel entry — has baked width/height
                readonly property var chromeEntry: modelData
                readonly property string mid: chromeEntry ? (chromeEntry.mediaId || "") : ""

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
                readonly property bool isDragging: root.liveDragMediaId === mid
                // isSnapDragging intentionally does NOT require isDragging.
                // liveDragMediaId is cleared before commitMediaTransform updates the model,
                // so gating on isDragging causes a flicker. The snap freeze clears via
                // onMediaChanged once the committed position arrives in the model.
                readonly property bool isSnapDragging: root.liveSnapDragActive
                                                       && root.liveSnapDragMediaId === mid

                // Scene position — switch to live resize/alt-resize coords when active.
                // chromeEntry.x/y are already in QML scene units (scenePos * sceneUnitScale).
                readonly property real sceneX: isLiveAltResizing ? root.liveAltResizeX
                                             : (isLiveResizing   ? root.liveResizeX
                                                                  : (chromeEntry ? (chromeEntry.x || 0) : 0))
                readonly property real sceneY: isLiveAltResizing ? root.liveAltResizeY
                                             : (isLiveResizing   ? root.liveResizeY
                                                                  : (chromeEntry ? (chromeEntry.y || 0) : 0))

                // Rendered screen size — chromeEntry.width/height = base * itemScale * sceneUnitScale,
                // so multiply by viewScale only to get screen pixels.
                // During live alt-resize use the live width/height directly (already in scene units).
                // During live uniform resize, recompute from mediaEntry base size * liveResizeScale.
                readonly property real screenW: {
                    if (isLiveAltResizing)
                        return root.liveAltResizeWidth * root.liveAltResizeScale * root.viewScale
                    if (isLiveResizing && mediaEntry)
                        return (mediaEntry.width || 0) * root.liveResizeScale * root.viewScale
                    return (chromeEntry ? (chromeEntry.width || 0) : 0) * root.viewScale
                }
                readonly property real screenH: {
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
                    mediaId: overlayDelegate.mid
                    visible: overlayDelegate.mediaEntry
                             && overlayDelegate.mediaEntry.mediaType === "video"

                    isPlaying: overlayDelegate.videoState ? !!overlayDelegate.videoState.isPlaying : false
                    isMuted:   overlayDelegate.videoState ? !!overlayDelegate.videoState.isMuted   : false
                    isLooping: overlayDelegate.videoState ? !!overlayDelegate.videoState.isLooping : false
                    progress:  overlayDelegate.videoState ? (overlayDelegate.videoState.progress || 0.0) : 0.0
                    volume:    overlayDelegate.videoState ? (overlayDelegate.videoState.volume   || 1.0) : 1.0

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
