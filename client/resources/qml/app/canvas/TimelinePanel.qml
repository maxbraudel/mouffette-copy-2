import QtQuick
import QtQuick.Controls
import QtQuick.Shapes
import QtQuick.Window
import Mouffette.App
import "../components"

FocusScope {
    id: root
    objectName: "sceneTimeline"
    required property var session
    property bool expanded: true
    readonly property real transportHeight: Theme.controlHeight + 16
    property real bottomCornerRadius: 0
    readonly property var timeline: session ? session.timeline : null
    readonly property bool editable: !!timeline && timeline.editable
    readonly property bool navigationEnabled: !!timeline && !timeline.remoteActive
    readonly property real maximumMs: timeline ? timeline.maxDurationMs : 180000
    property real viewDurationMs: timeline ? Math.min(maximumMs, timeline.initialViewDurationMs) : 15000
    readonly property real pixelsPerMs: Math.max(1, trackViewport.width - 24) / Math.max(1, viewDurationMs)
    readonly property real rulerHeight: timeline ? timeline.rulerHeightPx : 28
    readonly property real visibleStartX: trackViewport.contentX
    readonly property real visibleEndX: trackViewport.contentX + trackViewport.width
    readonly property real clipHeight: timeline ? timeline.clipTrackHeightPx : 48
    readonly property real keyHeight: 32
    readonly property int firstTrackIndex: timeline ? timeline.firstTrackIndex : 0
    readonly property int lastTrackIndex: firstTrackIndex + (timeline ? timeline.trackCount : 1) - 1
    readonly property var trackNames: {
        var names = []
        for (var i = 0; i < (timeline ? timeline.trackCount : 1); ++i)
            names.push("Track " + -(firstTrackIndex + i))
        return names
    }
    readonly property real slotMs: 1000 / (timeline ? timeline.slotsPerSecond : 30)
    readonly property int maximumFrame: Math.round(maximumMs / slotMs)
    readonly property int frameDigits: String(maximumFrame).length
    readonly property int minuteDigits: Math.max(2, String(Math.floor(Math.round(maximumMs) / 60000)).length)
    // Visual grouping only: authoring always retains the project slot size.
    readonly property int gridStride: Math.max(1, Math.ceil(8 / (slotMs * pixelsPerMs)))
    readonly property real gridStep: gridStride * slotMs
    readonly property real tickStep: Math.max(1, Math.ceil(80 / (gridStep * pixelsPerMs))) * gridStep
    property bool shiftHeld: false
    property bool controlHeld: false
    // Qt maps the physical macOS Control key to Meta, and Command to Control.
    readonly property int controlModifier: Qt.platform.os === "osx" ? Qt.MetaModifier : Qt.ControlModifier
    readonly property int controlKey: Qt.platform.os === "osx" ? Qt.Key_Meta : Qt.Key_Control
    function updateModifiers(modifiers) {
        shiftHeld = !!(modifiers & Qt.ShiftModifier)
        controlHeld = !!(modifiers & controlModifier)
    }
    property var activeDrag: null
    property var snapGuide: null
    readonly property real snapGuideMs: snapGuide ? snapGuide.targetTimeMs : -1
    readonly property string snapGuideLabel: snapGuide ? snapGuide.mediaName : ""
    readonly property bool textInputFocused: {
        var item = root.Window.window ? root.Window.window.activeFocusItem : null
        while (item) {
            if (item instanceof TextInput || item instanceof TextEdit) return true
            item = item.parent
        }
        return false
    }
    implicitHeight: timeline ? timeline.timelineHeightPx : 240

    // A grabbed MouseArea loses its cursor outside its bounds. Keep the edit
    // cursor over the window while dragging past the timeline's viewport.
    MouseArea {
        parent: root.Window.window ? root.Window.window.contentItem : root
        anchors.fill: parent
        z: 10000
        visible: root.visible && !!root.activeDrag && !!root.activeDrag.isClipDrag
        acceptedButtons: Qt.NoButton
        hoverEnabled: true
        cursorShape: root.activeDrag && root.activeDrag.editEdge !== 0
            ? Qt.SizeHorCursor : Qt.ClosedHandCursor
    }

    function formatTime(ms) {
        ms = Math.max(0, Math.round(ms))
        return String(Math.floor(ms / 60000)).padStart(minuteDigits, "0") + ":"
             + String(Math.floor(ms / 1000) % 60).padStart(2, "0") + "."
             + String(ms % 1000).padStart(3, "0")
    }
    function formatFrame(frame) {
        return "#" + String(Math.max(0, Math.round(frame))).padStart(frameDigits, "0")
    }
    function focusTrack() { root.forceActiveFocus() }
    function clampTime(ms) { return timeline ? timeline.gridTime(ms) : 0 }
    function snapResult(ms, excludeId, duration, includePlayhead) {
        if (!shiftHeld || !timeline) return { timeMs: clampTime(ms), snapped: false }
        return timeline.snapTime(ms, pixelsPerMs, excludeId || "", duration || 0, !!includePlayhead)
    }
    function showSnapGuide(result) {
        snapGuide = result && result.snapped ? result : null
    }
    function snapTime(ms, excludeId, duration, includePlayhead) {
        var result = snapResult(ms, excludeId, duration, includePlayhead)
        showSnapGuide(result)
        return result.timeMs
    }
    function endDrag() { activeDrag = null; showSnapGuide(null) }
    function clipOwnsResizePoint(clip, contentX) {
        var distance = Math.max(clip.x - contentX, contentX - clip.x - clip.width, 0)
        for (var i = 0; i < clipRepeater.count; ++i) {
            var other = clipRepeater.itemAt(i)
            if (!other || other === clip || other.shownTrack !== clip.shownTrack) continue
            var otherDistance = Math.max(other.x - contentX, contentX - other.x - other.width, 0)
            // Split overlapping hit areas at the gap midpoint; ties go right.
            if (otherDistance < distance - 0.0001
                    || (Math.abs(otherDistance - distance) < 0.0001 && other.x > clip.x)) return false
        }
        return true
    }
    function scrollTracks(delta) {
        clipViewport.contentY = Math.max(-clipViewport.topMargin,
            Math.min(clipViewport.contentHeight + clipViewport.bottomMargin - clipViewport.height,
            clipViewport.contentY + delta))
    }
    function autoScrollClip(drag, elapsedMs) {
        if (!navigationEnabled) return
        // Bound catch-up after a stalled frame so the clip never jumps ahead.
        var step = (timeline ? timeline.autoScrollSpeedPxPerSecond : 96)
            * Math.max(0, Math.min(50, elapsedMs)) / 1000
        var point = clipViewport.mapFromItem(root, drag.pointerPanelX, drag.pointerPanelY)
        var viewportPoint = trackViewport.mapFromItem(root, drag.pointerPanelX, drag.pointerPanelY)
        var dy = point.y < 22 ? -step : point.y > clipViewport.height - 22 ? step : 0
        var dx = viewportPoint.x < 22 ? -step : viewportPoint.x > trackViewport.width - 22 ? step : 0
        if (dy !== 0) scrollTracks(dy)
        if (dx !== 0) scrollTo(trackViewport.contentX + dx)
        if (dx !== 0 || dy !== 0) drag.refreshFromPointer()
    }
    function scrollTo(x) {
        trackViewport.contentX = Math.max(0, Math.min(trackViewport.contentWidth - trackViewport.width, x))
    }
    function zoomAround(factor, timeMs, anchorX) {
        if (!navigationEnabled) return
        viewDurationMs = Math.max(100, Math.min(maximumMs, viewDurationMs * factor))
        scrollTo(timeMs * pixelsPerMs + 12 - anchorX)
    }
    function zoom(factor) {
        var timeMs = timeline ? timeline.positionMs : 0
        var headX = timeMs * pixelsPerMs + 12 - trackViewport.contentX
        zoomAround(factor, timeMs,
            headX >= 0 && headX <= trackViewport.width ? headX : trackViewport.width / 2)
    }
    function handleWheel(wheel) {
        if (!navigationEnabled || activeDrag) { wheel.accepted = true; return }
        var precise = wheel.pixelDelta.x !== 0 || wheel.pixelDelta.y !== 0
        var dx = precise ? wheel.pixelDelta.x : wheel.angleDelta.x / 8
        var dy = precise ? wheel.pixelDelta.y : wheel.angleDelta.y / 8
        if ((wheel.modifiers & (Qt.ControlModifier | Qt.MetaModifier)) !== 0) {
            var delta = wheel.inverted ? -dy : dy
            var trackpad = precise || wheel.phase !== Qt.NoScrollPhase
                || (wheel.device && wheel.device.type === PointerDevice.TouchPad)
            if (delta !== 0)
                zoom(trackpad ? Math.exp(-delta * 0.003) : Math.pow(1.0015, -delta * 5))
        } else if (wheel.y >= root.rulerHeight + root.keyHeight + keyframeSeparator.height) {
            if ((wheel.modifiers & Qt.ShiftModifier) !== 0)
                scrollTo(trackViewport.contentX - (dx || dy) * (precise ? 1 : 3))
            else {
                if (dx !== 0) scrollTo(trackViewport.contentX - dx * (precise ? 1 : 3))
                if (dy !== 0) scrollTracks(-dy * (precise ? 1 : 3))
            }
        } else {
            scrollTo(trackViewport.contentX - (Math.abs(dx) > Math.abs(dy) ? dx : dy) * (precise ? 1 : 3))
        }
        wheel.accepted = true
    }
    function revealHead() {
        if (!timeline) return
        var head = timeline.positionMs * pixelsPerMs + 12
        if (head < trackViewport.contentX || head > trackViewport.contentX + trackViewport.width - 30)
            scrollTo(head - trackViewport.width * 0.2)
    }
    function followHead() {
        if (timeline && (timeline.playing || timeline.remoteActive)) revealHead()
    }
    function jumpToClipBoundary(direction) {
        if (!timeline) return
        var previousSlot = timeline.positionSlot
        timeline.seekClipBoundary(direction)
        if (timeline.positionSlot !== previousSlot) revealHead()
    }
    onShiftHeldChanged: if (activeDrag) activeDrag.refreshPreview()
    onControlHeldChanged: if (activeDrag && activeDrag.isClipDrag) activeDrag.refreshPreview()
    onExpandedChanged: {
        if (!expanded && activeDrag) {
            if (activeDrag.isClipDrag) activeDrag.cancelEdit()
            else { activeDrag.dragging = false; endDrag() }
        }
    }
    onFirstTrackIndexChanged: Qt.callLater(scrollTracks, 0)
    onLastTrackIndexChanged: Qt.callLater(scrollTracks, 0)
    onTimelineChanged: {
        viewDurationMs = timeline ? Math.min(maximumMs, timeline.initialViewDurationMs) : 15000
        trackViewport.contentX = 0
        clipViewport.contentY = (root.clipHeight - clipViewport.height) / 2
        endDrag(); shiftHeld = false; controlHeld = false
    }
    Keys.onPressed: event => {
        if (event.key === Qt.Key_Shift) { shiftHeld = true; event.accepted = true }
        else if (event.key === controlKey) { controlHeld = true; event.accepted = true }
    }
    Keys.onReleased: event => {
        if (event.key === Qt.Key_Shift) { shiftHeld = false; event.accepted = true }
        else if (event.key === controlKey) { controlHeld = false; event.accepted = true }
    }
    Connections {
        target: root.Window.window
        function onActiveChanged() {
            if (root.Window.window && !root.Window.window.active) {
                if (root.activeDrag && root.activeDrag.isClipDrag) root.activeDrag.cancelEdit()
                root.shiftHeld = false; root.controlHeld = false
            }
        }
    }
    Connections {
        target: root.timeline
        function onTransportChanged() { root.followHead() }
        function onRevealPlayhead() { root.revealHead() }
        function onRevealTrack(row) {
            if (root.activeDrag || !root.timeline || root.timeline.activeTrackIndex !== row) return
            var top = (root.firstTrackIndex + row) * root.clipHeight
            var bottom = top + root.clipHeight
            if (top < clipViewport.contentY) clipViewport.contentY = top
            else if (bottom > clipViewport.contentY + clipViewport.height)
                clipViewport.contentY = bottom - clipViewport.height
        }
    }
    Rectangle {
        anchors.fill: parent
        color: Theme.overlayBackground
        bottomLeftRadius: root.bottomCornerRadius
        bottomRightRadius: root.bottomCornerRadius
    }

    component TimelineButton: AppButton {
        focusPolicy: Qt.NoFocus
    }
    component TimelineEditButton: TimelineButton {
        iconOnly: editBar.compactButtons
    }
    function buttonRowTextWidth(row) {
        var width = 0
        var count = 0
        for (var i = 0; i < row.children.length; ++i) {
            var child = row.children[i]
            if (!child.visible) continue
            width += child instanceof AppButton ? child.textWidth : child.implicitWidth
            ++count
        }
        return width + Math.max(0, count - 1) * row.spacing
    }
    component KeyframeDiamond: Rectangle {
        width: root.timeline ? root.timeline.keyframeSizePx : 10
        height: width
        rotation: 45
        color: Theme.overlayText
        border.color: Theme.overlayBackground
    }
    TextMetrics {
        id: readoutMetrics
        font.family: Theme.monospaceFontFamily
        font.pixelSize: Theme.controlFontSize
        text: root.formatTime(root.maximumMs) + root.formatFrame(root.maximumFrame)
    }
    component TimelineReadout: Item {
        id: readout
        required property real timeMs
        required property int frame
        required property string description
        property alias timeObjectName: timeLabel.objectName
        property alias frameObjectName: frameLabel.objectName
        readonly property int gap: 6
        readonly property real labelSize: Math.min(Theme.controlFontSize,
            Theme.controlFontSize * Math.max(1, width - gap * 2 - 1) / Math.max(1, readoutMetrics.width))
        implicitWidth: readoutMetrics.width + gap * 2 + 1
        implicitHeight: Theme.controlHeight
        width: implicitWidth; height: implicitHeight
        Accessible.role: Accessible.StaticText
        Accessible.name: description + ": " + timeLabel.text + ", " + frameLabel.text

        Row {
            anchors.verticalCenter: parent.verticalCenter
            spacing: readout.gap
            Text {
                id: timeLabel
                height: readout.height
                text: root.formatTime(readout.timeMs)
                color: Theme.mutedText
                font.family: Theme.monospaceFontFamily
                font.pixelSize: readout.labelSize
                verticalAlignment: Text.AlignVCenter
            }
            Rectangle {
                anchors.verticalCenter: parent.verticalCenter
                width: 1; height: 16
                color: Theme.border
            }
            Text {
                id: frameLabel
                height: readout.height
                text: root.formatFrame(readout.frame)
                color: Theme.mutedText
                font.family: Theme.monospaceFontFamily
                font.pixelSize: readout.labelSize
                verticalAlignment: Text.AlignVCenter
            }
        }
    }
    Item {
        id: transportViewport
        objectName: "timelineTransportBar"
        anchors.left: parent.left; anchors.right: parent.right; anchors.top: parent.top
        anchors.margins: 8
        height: Theme.controlHeight
        readonly property real gap: 6
        readonly property bool compactButtons: width < playback.textWidth + currentReadout.implicitWidth * 2 + 24
        readonly property real sideWidth: Math.max(0, (width - playback.width) / 2 - 12)

        TimelineReadout {
            id: currentReadout
            objectName: "timelineCurrentReadout"
            anchors.left: parent.left
            width: Math.min(implicitWidth, transportViewport.sideWidth)
            timeObjectName: "timelineCurrentTime"
            frameObjectName: "timelineCurrentFrame"
            description: "Current position"
            timeMs: root.timeline ? root.timeline.positionMs : 0
            frame: root.timeline ? root.timeline.positionSlot : 0
        }
        TimelineReadout {
            objectName: "timelineMaximumReadout"
            anchors.right: parent.right
            width: Math.min(implicitWidth, transportViewport.sideWidth)
            timeObjectName: "timelineMaximumTime"
            frameObjectName: "timelineMaximumFrame"
            description: "Maximum duration"
            timeMs: root.maximumMs
            frame: root.maximumFrame
        }
        Row {
            id: playback
            objectName: "timelinePlaybackControls"
            anchors.horizontalCenter: parent.horizontalCenter
            height: parent.height
            spacing: transportViewport.gap
            readonly property real textWidth: startButton.textWidth + playButton.textWidth
                + endButton.textWidth + spacing * 2
            TimelineButton {
                id: startButton
                objectName: "timelineGoToStart"
                text: "Start"
                iconSource: "qrc:/icons/icons/timeline/start.svg"
                iconOnly: transportViewport.compactButtons
                Accessible.name: "Return to start"
                enabled: !!root.timeline && !root.timeline.remoteActive
                onClicked: { root.focusTrack(); root.timeline.goToStart() }
            }
            TimelineButton {
                id: playButton
                objectName: "timelinePlayPause"
                text: root.timeline && root.timeline.playing ? "Pause" : "Play"
                textVariants: ["Play", "Pause"]
                iconSource: root.timeline && root.timeline.playing
                    ? "qrc:/icons/icons/pause.svg" : "qrc:/icons/icons/play.svg"
                iconOnly: transportViewport.compactButtons
                primary: true
                checked: !!root.timeline && root.timeline.playing
                enabled: !!root.timeline && !root.timeline.remoteActive
                onClicked: { root.focusTrack(); root.timeline.togglePlayback() }
            }
            TimelineButton {
                id: endButton
                objectName: "timelineGoToEnd"
                text: "End"
                iconSource: "qrc:/icons/icons/timeline/end.svg"
                iconOnly: transportViewport.compactButtons
                Accessible.name: "Go to scene end"
                enabled: !!root.timeline && !root.timeline.remoteActive
                onClicked: { root.focusTrack(); root.timeline.goToEnd() }
            }
        }
    }
    Item {
        id: editorBody
        objectName: "timelineEditorBody"
        anchors.left: parent.left; anchors.right: parent.right
        y: root.transportHeight
        height: Math.max(0, root.height - y)
        visible: root.expanded
        clip: true
        Rectangle {
            id: transportSeparator
            anchors.left: parent.left; anchors.right: parent.right
            anchors.top: parent.top
            height: 1
            color: Theme.border
        }
        Item {
            id: editBar
            objectName: "timelineEditBar"
            // Measure labels independently of the current mode so resizing cannot oscillate.
            readonly property bool compactButtons: width < actionViewport.contentPadding * 2
                + root.buttonRowTextWidth(actions) + actions.spacing + root.buttonRowTextWidth(layoutActions)
            anchors.left: parent.left; anchors.right: parent.right
            anchors.top: transportSeparator.bottom
            anchors.topMargin: 8
            height: Theme.controlHeight
            Flickable {
                id: actionViewport
                objectName: "timelineEditActions"
                readonly property int contentPadding: 8
                anchors.fill: parent
                clip: true
                // Compact buttons share one scrollable row if their icons still do not fit.
                contentWidth: Math.max(width, contentPadding * 2 + actions.width + actions.spacing + layoutActions.width)
                contentHeight: height
                boundsBehavior: Flickable.StopAtBounds
                interactive: false
                Row {
                    id: actions
                    x: actionViewport.contentPadding
                    spacing: 6
                    TimelineEditButton {
                        objectName: "timelinePlaceKeyframe"
                        text: root.timeline && root.timeline.hasKeyframeAtPosition ? "Update keyframe" : "Place keyframe"
                        textVariants: ["Update keyframe", "Place keyframe"]
                        iconSource: root.timeline && root.timeline.hasKeyframeAtPosition
                            ? "qrc:/icons/icons/timeline/keyframe-update.svg" : "qrc:/icons/icons/timeline/keyframe-add.svg"
                        enabled: root.editable && root.timeline.canCapture
                        onClicked: { root.focusTrack(); root.timeline.placeKeyframe() }
                    }
                    TimelineEditButton {
                        objectName: "timelineSplitClip"
                        text: "Split clip"
                        iconSource: "qrc:/icons/icons/timeline/split.svg"
                        visible: !!root.timeline && root.timeline.primaryMediaId !== ""
                        enabled: root.editable && root.timeline.canSplit
                        onClicked: { root.focusTrack(); root.timeline.splitClip() }
                    }

                    TimelineEditButton {
                        objectName: "timelineCopy"
                        text: "Copy"
                        iconSource: "qrc:/icons/icons/timeline/copy.svg"
                        enabled: root.editable && root.timeline.hasActiveSelection
                        onClicked: { root.focusTrack(); root.timeline.copySelected() }
                    }
                    TimelineEditButton {
                        objectName: "timelinePaste"
                        text: "Paste"
                        iconSource: "qrc:/icons/icons/timeline/paste.svg"
                        enabled: root.editable && root.timeline.canPaste
                        onClicked: { root.focusTrack(); root.timeline.paste() }
                    }
                    TimelineEditButton {
                        objectName: "timelineDelete"
                        text: "Delete"
                        iconSource: "qrc:/icons/icons/delete.svg"
                        destructive: true
                        enabled: root.editable && root.timeline.hasActiveSelection
                        onClicked: { root.focusTrack(); root.timeline.deleteSelected() }
                    }
                    TimelineEditButton {
                        objectName: "timelinePlaceStop"
                        text: root.timeline && root.timeline.stopTimeMs >= 0 ? "Move Stop here" : "Place Stop"
                        textVariants: ["Move Stop here", "Place Stop"]
                        iconSource: "qrc:/icons/icons/timeline/stop-add.svg"
                        enabled: root.editable
                        onClicked: { root.focusTrack(); root.timeline.placeStop() }
                    }
                    TimelineEditButton {
                        objectName: "timelineRemoveStop"
                        text: "Remove Stop"
                        iconSource: "qrc:/icons/icons/timeline/stop-remove.svg"
                        destructive: true
                        visible: !!root.timeline && root.timeline.stopTimeMs >= 0
                        enabled: root.editable
                        onClicked: { root.focusTrack(); root.timeline.removeStop() }
                    }
                }
                Row {
                    id: layoutActions
                    objectName: "timelineLayoutControls"
                    anchors.right: parent.right
                    anchors.rightMargin: actionViewport.contentPadding
                    height: Theme.controlHeight
                    spacing: transportViewport.gap
                    TimelineEditButton {
                        id: zoomOutButton
                        objectName: "timelineZoomOut"
                        text: "Zoom out"
                        iconSource: "qrc:/icons/icons/timeline/zoom-out.svg"
                        enabled: root.navigationEnabled
                        onClicked: root.zoom(2)
                    }
                    TimelineEditButton {
                        id: zoomInButton
                        objectName: "timelineZoomIn"
                        text: "Zoom in"
                        iconSource: "qrc:/icons/icons/timeline/zoom-in.svg"
                        enabled: root.navigationEnabled
                        onClicked: root.zoom(0.5)
                    }
                    TimelineEditButton {
                        id: fitButton
                        objectName: "timelineFitDuration"
                        text: "Fit duration"
                        iconSource: "qrc:/icons/icons/timeline/fit.svg"
                        enabled: root.navigationEnabled
                        onClicked: { root.viewDurationMs = root.maximumMs; trackViewport.contentX = 0 }
                    }
                }
            }
            MouseArea {
                anchors.fill: actionViewport
                acceptedButtons: Qt.NoButton
                cursorShape: undefined
                scrollGestureEnabled: true
                onWheel: wheel => {
                    if (!root.navigationEnabled) { wheel.accepted = true; return }
                    var precise = wheel.pixelDelta.x !== 0 || wheel.pixelDelta.y !== 0
                    var dx = precise ? wheel.pixelDelta.x : wheel.angleDelta.x / 8
                    var dy = precise ? wheel.pixelDelta.y : wheel.angleDelta.y / 8
                    var delta = (Math.abs(dx) > Math.abs(dy) ? dx : dy) * (precise ? 1 : 3)
                    actionViewport.contentX = Math.max(0, Math.min(actionViewport.contentWidth - actionViewport.width,
                        actionViewport.contentX - delta))
                    wheel.accepted = true
                }
            }
        }
        Rectangle {
            id: tracksSeparator
            anchors.left: parent.left; anchors.right: parent.right
            anchors.top: editBar.bottom; anchors.topMargin: 8
            height: 1
            color: Theme.border
        }
        StateTextMetrics {
            id: trackNameMetrics
            font: keyTrackName.font
            text: keyTrackName.text
            textVariants: root.trackNames
        }
        component TrackName: Text {
            x: 12
            width: Math.max(0, parent.width - 25)
            font.pixelSize: 10
            color: Theme.overlayText
            textFormat: Text.PlainText
            verticalAlignment: Text.AlignVCenter
        }
        component TrackSeparators: Item {
            required property int trackIndex
            readonly property real trackTop: trackIndex * root.clipHeight - clipViewport.contentY
            anchors.fill: parent
            Rectangle {
                objectName: "timelineTrackTopSeparator"
                width: parent.width; height: 1
                visible: parent.trackTop > 0
                color: Theme.overlayBorder
            }
            Rectangle {
                objectName: "timelineTrackBottomSeparator"
                anchors.bottom: parent.bottom
                width: parent.width; height: 1
                visible: parent.trackIndex === root.lastTrackIndex
                    && parent.trackTop + root.clipHeight < clipViewport.height
                color: Theme.overlayBorder
            }
        }
        Rectangle {
            id: keyframeSeparator
            objectName: "timelineKeyframeSeparator"
            anchors.left: parent.left; anchors.right: parent.right
            anchors.top: tracksSeparator.bottom
            anchors.topMargin: root.rulerHeight + root.keyHeight
            height: 1
            z: 1
            color: Theme.overlayBorder
        }
        Item {
            id: trackHeaders
            objectName: "timelineTrackHeaders"
            anchors.left: parent.left
            anchors.top: tracksSeparator.bottom; anchors.bottom: parent.bottom
            width: trackNameMetrics.maximumWidth + 25
            clip: true
            Rectangle {
                anchors.right: parent.right
                width: 1; height: parent.height
                color: Theme.overlayBorder
            }
            Rectangle { y: root.rulerHeight; width: parent.width; height: 1; color: Theme.overlayBorder }
            TrackName {
                id: keyTrackName
                objectName: "timelineKeyframeHeader"
                y: root.rulerHeight
                height: root.keyHeight
                text: "Keyframes"
            }
            Item {
                id: clipHeaders
                objectName: "timelineClipHeaders"
                y: root.rulerHeight + root.keyHeight + keyframeSeparator.height
                width: parent.width; height: Math.max(0, parent.height - y)
                clip: true
                Repeater {
                    model: Math.ceil(clipHeaders.height / root.clipHeight) + 2
                    Item {
                        required property int index
                        readonly property int trackIndex: Math.floor(clipViewport.contentY / root.clipHeight) + index
                        objectName: "timelineClipTrackHeader"
                        y: trackIndex * root.clipHeight - clipViewport.contentY
                        width: clipHeaders.width; height: root.clipHeight
                        visible: trackIndex >= root.firstTrackIndex && trackIndex <= root.lastTrackIndex
                        TrackSeparators { trackIndex: parent.trackIndex }
                        TrackName {
                            objectName: "timelineClipTrackLabel"
                            height: parent.height
                            text: "Track " + -parent.trackIndex
                        }
                    }
                }
            }
            MouseArea {
                anchors.fill: parent
                acceptedButtons: Qt.NoButton
                cursorShape: undefined
                scrollGestureEnabled: true
                onWheel: wheel => root.handleWheel(wheel)
            }
        }
        Flickable {
            id: trackViewport
            objectName: "timelineTracks"
            anchors.left: trackHeaders.right; anchors.right: parent.right
            anchors.top: tracksSeparator.bottom; anchors.bottom: parent.bottom
            clip: true
            contentWidth: Math.max(width, root.maximumMs * root.pixelsPerMs + 24)
            contentHeight: height
            boundsBehavior: Flickable.StopAtBounds
            interactive: false
            ScrollBar.horizontal: ScrollBar {
                objectName: "timelineHorizontalScrollBar"
                enabled: root.navigationEnabled
                policy: ScrollBar.AlwaysOn
                z: 1
                background: null
            }
            Item {
                id: timelineContent
                width: trackViewport.contentWidth; height: trackViewport.height
                Rectangle { width: parent.width; height: root.rulerHeight; color: Theme.overlaySelected }
                Rectangle { y: root.rulerHeight; width: parent.width; height: 1; color: Theme.overlayBorder }
                MouseArea {
                    id: scrubber
                    objectName: "timelineScrubber"
                    property bool dragging: false
                    property real rawMs: 0
                    function refreshPreview() {
                        if (dragging) root.timeline.seek(root.snapTime(rawMs, "", 0))
                    }
                    function updatePosition(mouse) {
                        rawMs = (mouse.x - 12) / root.pixelsPerMs
                        root.updateModifiers(mouse.modifiers)
                        refreshPreview()
                    }
                    width: parent.width
                    height: root.rulerHeight
                    acceptedButtons: Qt.LeftButton
                    preventStealing: true
                    enabled: !!root.timeline && !root.timeline.remoteActive
                    onPressed: mouse => {
                        root.focusTrack()
                        dragging = true; root.activeDrag = scrubber
                        updatePosition(mouse)
                    }
                    onPositionChanged: mouse => { if (pressed && dragging) updatePosition(mouse) }
                    onReleased: mouse => {
                        if (dragging) updatePosition(mouse)
                        dragging = false; root.endDrag()
                    }
                    onCanceled: { dragging = false; root.endDrag() }
                }
                MouseArea {
                    y: clipViewport.y
                    width: parent.width
                    height: parent.height - y
                    acceptedButtons: Qt.LeftButton
                    preventStealing: true
                    enabled: !!root.timeline && !root.timeline.remoteActive
                    onPressed: {
                        root.focusTrack()
                        root.timeline.clearSelection()
                    }
                }
                Repeater {
                    model: Math.ceil(Math.max(0, trackViewport.width) / (root.gridStep * root.pixelsPerMs)) + 2
                    Rectangle {
                        required property int index
                        readonly property real timeMs: (Math.floor(Math.max(0, trackViewport.contentX - 12) / root.pixelsPerMs / root.gridStep) + index) * root.gridStep
                        objectName: "timelineGridLine"
                        x: 12 + timeMs * root.pixelsPerMs
                        y: root.rulerHeight
                        visible: timeMs <= root.maximumMs
                        width: 1; height: timelineContent.height - y
                        color: Theme.overlayBorder; opacity: 0.45
                    }
                }
                Repeater {
                    model: Math.ceil(Math.max(0, trackViewport.width) / (root.tickStep * root.pixelsPerMs)) + 2
                    Item {
                        required property int index
                        readonly property real timeMs: (Math.floor(Math.max(0, trackViewport.contentX - 12) / root.pixelsPerMs / root.tickStep) + index) * root.tickStep
                        x: 12 + timeMs * root.pixelsPerMs
                        visible: timeMs <= root.maximumMs
                        height: timelineContent.height
                        Rectangle { y: root.rulerHeight - 7; width: 1; height: 7; color: Theme.overlayDisabledText }
                        Rectangle { y: root.rulerHeight; width: 1; height: parent.height - root.rulerHeight; color: Theme.overlayBorder; opacity: 0.35 }
                        Text { x: 4; y: 3; text: root.formatTime(parent.timeMs); font.pixelSize: 10; color: Theme.overlayText }
                    }
                }
                Item {
                    id: keyTrack
                    objectName: "timelineKeyframeTrack"
                    y: root.rulerHeight
                    width: parent.width; height: root.keyHeight
                    MouseArea {
                        anchors.fill: parent
                        acceptedButtons: Qt.LeftButton
                        preventStealing: true
                        enabled: !!root.timeline && !root.timeline.remoteActive
                        onPressed: {
                            root.focusTrack()
                            root.timeline.clearKeyframeSelection()
                        }
                    }
                    Repeater {
                        model: root.timeline ? root.timeline.otherKeyframes : []
                        KeyframeDiamond {
                            required property var modelData
                            objectName: "otherMediaKeyframe"
                            x: 12 + modelData.timeMs * root.pixelsPerMs - width / 2
                            y: (keyTrack.height - height) / 2
                            opacity: root.timeline ? root.timeline.otherKeyframeOpacity : 0.3
                        }
                    }
                    Repeater {
                        model: root.timeline ? root.timeline.keyframes : []
                        Item {
                            id: keyItem
                            required property var modelData
                            objectName: "timelineKeyframe"
                            property bool dragging: false
                            property real previewMs: 0
                            property real rawMs: 0
                            readonly property real shownMs: dragging ? previewMs : modelData.timeMs
                            function refreshPreview() { previewMs = root.snapTime(rawMs, modelData.id, 0) }
                            x: 12 + shownMs * root.pixelsPerMs - width / 2
                            y: (keyTrack.height - height) / 2
                            z: dragging ? 2 : 1
                            width: 24; height: 24
                            KeyframeDiamond {
                                objectName: "timelineKeyframeDiamond"
                                anchors.centerIn: parent
                                color: root.timeline && root.timeline.selectedKeyframeId === keyItem.modelData.id ? Theme.accent : Theme.overlayText
                            }
                            MouseArea {
                                anchors.fill: parent
                                enabled: root.editable
                                cursorShape: Qt.SizeHorCursor
                                property real pressX: 0
                                property real initialMs: 0
                                onPressed: mouse => {
                                    root.focusTrack(); root.shiftHeld = !!(mouse.modifiers & Qt.ShiftModifier)
                                    root.timeline.selectKeyframe(keyItem.modelData.id)
                                    initialMs = keyItem.modelData.timeMs
                                    pressX = mapToItem(timelineContent, mouse.x, mouse.y).x
                                    keyItem.rawMs = initialMs; keyItem.previewMs = initialMs
                                    keyItem.dragging = true; root.activeDrag = keyItem
                                }
                                onPositionChanged: mouse => {
                                    if (!pressed) return
                                    root.shiftHeld = !!(mouse.modifiers & Qt.ShiftModifier)
                                    keyItem.rawMs = initialMs + (mapToItem(timelineContent, mouse.x, mouse.y).x - pressX) / root.pixelsPerMs
                                    keyItem.refreshPreview()
                                }
                                onReleased: {
                                    var id = keyItem.modelData.id; var ms = keyItem.previewMs
                                    keyItem.dragging = false; root.endDrag()
                                    if (ms !== initialMs) root.timeline.moveKeyframe(id, ms)
                                }
                                onCanceled: { keyItem.dragging = false; root.endDrag() }
                            }
                            ToolTip.visible: keyHover.hovered
                            ToolTip.text: root.formatTime(shownMs)
                            HoverHandler { id: keyHover }
                        }
                    }
                }
                Item {
                    id: clipViewport
                    objectName: "timelineClipViewport"
                    y: keyTrack.y + keyTrack.height + keyframeSeparator.height
                    width: parent.width; height: Math.max(0, parent.height - y)
                    // A track's Y coordinate never depends on the first occupied row.
                    // Only unused viewport space centers the tracks; overflowing rows have no padding.
                    readonly property real centerPadding: Math.max(0,
                        (height - (root.lastTrackIndex - root.firstTrackIndex + 1) * root.clipHeight) / 2)
                    readonly property real topMargin: -root.firstTrackIndex * root.clipHeight + centerPadding
                    readonly property real bottomMargin: centerPadding
                    readonly property real contentHeight: (root.lastTrackIndex + 1) * root.clipHeight
                    property real previousHeight: 0
                    clip: true
                    property real contentY: 0
                    readonly property alias contentItem: clipContent
                    onHeightChanged: {
                        contentY -= (height - previousHeight) / 2
                        previousHeight = height
                        Qt.callLater(root.scrollTracks, 0)
                    }
                    Component.onCompleted: contentY = (root.clipHeight - height) / 2
                    ScrollBar {
                        objectName: "timelineVerticalScrollBar"
                        enabled: root.navigationEnabled
                        orientation: Qt.Vertical
                        height: parent.height
                        readonly property real extent: clipViewport.topMargin + clipViewport.contentHeight + clipViewport.bottomMargin
                        size: Math.min(1, height / Math.max(1, extent))
                        position: (clipViewport.contentY + clipViewport.topMargin) / Math.max(1, extent)
                        onPositionChanged: if (root.navigationEnabled && pressed) clipViewport.contentY = position * extent - clipViewport.topMargin
                        active: hovered || pressed
                        x: root.visibleStartX + trackViewport.width - width
                        anchors.right: undefined
                        z: 10
                        policy: ScrollBar.AsNeeded
                        background: null
                    }
                    Item {
                        id: clipContent
                        y: -clipViewport.contentY
                        width: clipViewport.width
                        height: clipViewport.contentHeight
                        Repeater {
                            model: Math.ceil(clipViewport.height / root.clipHeight) + 2
                            Item {
                                id: clipTrack
                                required property int index
                                readonly property int trackIndex: Math.floor(clipViewport.contentY / root.clipHeight) + index
                                objectName: "timelineClipTrack"
                                y: trackIndex * root.clipHeight
                                width: clipViewport.width; height: root.clipHeight
                                visible: trackIndex >= root.firstTrackIndex && trackIndex <= root.lastTrackIndex
                                TrackSeparators { trackIndex: parent.trackIndex }
                                MouseArea {
                                    anchors.fill: parent
                                    enabled: root.editable
                                    onPressed: {
                                        root.focusTrack()
                                        root.timeline.setActiveTrackIndex(clipTrack.trackIndex - root.firstTrackIndex)
                                    }
                                }
                            }
                        }
                        Repeater {
                            id: clipRepeater
                            model: root.timeline ? root.timeline.clipModel : null
                            TimelineClip {
                                panel: root
                                timeContent: clipViewport.contentItem
                                trackHeight: root.clipHeight
                            }
                        }
                    }
                }
                Rectangle {
                    x: 12 + (root.timeline ? root.timeline.effectiveEndMs : root.maximumMs) * root.pixelsPerMs
                    y: root.rulerHeight
                    width: Math.max(0, timelineContent.width - x); height: timelineContent.height - y
                    color: "#44000000"
                }
                Rectangle {
                    id: playhead
                    objectName: "timelinePlayhead"
                    x: 12 + (root.timeline ? root.timeline.positionMs : 0) * root.pixelsPerMs
                    width: 1; height: timelineContent.height
                    enabled: !!root.timeline && !root.timeline.remoteActive
                    color: enabled ? Theme.accent : Theme.disabledText
                    Shape {
                        id: playheadCap
                        objectName: "timelinePlayheadCap"
                        x: (parent.width - width) / 2
                        anchors.top: parent.top
                        width: 12; height: 14
                        preferredRendererType: Shape.CurveRenderer
                        antialiasing: true
                        ShapePath {
                            strokeWidth: -1
                            fillColor: playhead.color
                            startX: 0; startY: 0
                            PathLine { x: playheadCap.width; y: 0 }
                            PathLine { x: playheadCap.width; y: playheadCap.height - 5 }
                            PathLine { x: playheadCap.width / 2; y: playheadCap.height }
                            PathLine { x: 0; y: playheadCap.height - 5 }
                            PathLine { x: 0; y: 0 }
                        }
                    }
                }
                Item {
                    id: stopMarker
                    objectName: "timelineStopMarker"
                    visible: !!root.timeline && root.timeline.stopTimeMs >= 0
                    property bool dragging: false
                    property real previewMs: 0
                    property real rawMs: 0
                    function refreshPreview() { previewMs = root.snapTime(rawMs, "", 0) }
                    x: 12 + (dragging ? previewMs : root.timeline ? root.timeline.stopTimeMs : 0) * root.pixelsPerMs - 8
                    width: 16; height: timelineContent.height
                    Rectangle { x: 7.5; width: 1; height: parent.height; color: "#ee7979" }
                    Rectangle { width: 14; height: Math.max(16, root.rulerHeight - 4); color: "#b74646"; radius: 2 }
                    Text { y: 2; anchors.horizontalCenter: parent.horizontalCenter; text: "S"; color: "white"; font.pixelSize: 11 }
                    MouseArea {
                        width: parent.width; height: root.rulerHeight
                        enabled: root.editable
                        cursorShape: Qt.SizeHorCursor
                        onPressed: mouse => {
                            root.focusTrack(); root.shiftHeld = !!(mouse.modifiers & Qt.ShiftModifier)
                            stopMarker.previewMs = root.timeline.stopTimeMs; stopMarker.rawMs = stopMarker.previewMs
                            stopMarker.dragging = true; root.activeDrag = stopMarker
                        }
                        onPositionChanged: mouse => {
                            if (!pressed) return
                            root.shiftHeld = !!(mouse.modifiers & Qt.ShiftModifier)
                            stopMarker.rawMs = (mapToItem(timelineContent, mouse.x, mouse.y).x - 12) / root.pixelsPerMs
                            stopMarker.refreshPreview()
                        }
                        onReleased: {
                            var ms = stopMarker.previewMs
                            stopMarker.dragging = false; root.endDrag(); root.timeline.setStopTime(ms)
                        }
                        onCanceled: { stopMarker.dragging = false; root.endDrag() }
                    }
                }
                Rectangle {
                    id: snapGuideLine
                    objectName: "timelineSnapGuide"
                    visible: root.snapGuideMs >= 0 && x >= root.visibleStartX && x < root.visibleEndX
                    x: 12 + root.snapGuideMs * root.pixelsPerMs
                    z: 20
                    width: 1; height: timelineContent.height
                    color: Theme.accent
                    Text {
                        objectName: "timelineSnapGuideLabel"
                        width: Math.min(implicitWidth, Math.max(0, trackViewport.width - 8))
                        x: Math.max(root.visibleStartX + 4,
                            Math.min(snapGuideLine.x + 4, root.visibleEndX - width - 4)) - snapGuideLine.x
                        y: root.rulerHeight + 2
                        text: root.snapGuideLabel
                        textFormat: Text.PlainText
                        elide: Text.ElideRight
                        color: Theme.accent; font.pixelSize: 11
                    }
                }
            }
        }
        Timer {
            interval: 16
            repeat: true
            running: !!root.activeDrag && !!root.activeDrag.isClipDrag
            property real previousTickMs: 0
            onRunningChanged: previousTickMs = Date.now()
            onTriggered: {
                var now = Date.now()
                root.autoScrollClip(root.activeDrag, now - previousTickMs)
                previousTickMs = now
            }
        }
        MouseArea {
            id: trackInput
            anchors.fill: trackViewport
            acceptedButtons: Qt.NoButton
            cursorShape: undefined
            hoverEnabled: true
            scrollGestureEnabled: true
            onWheel: wheel => root.handleWheel(wheel)
        }
    }
    Text {
        anchors.right: parent.right; anchors.bottom: parent.bottom; anchors.margins: 4
        text: root.timeline ? root.timeline.errorText : ""
        visible: root.expanded && text.length > 0
        color: "#ee7979"; font.pixelSize: 11
        width: Math.min(implicitWidth, root.width - 12); elide: Text.ElideRight
    }
    component TimelineShortcut: Shortcut {
        enabled: root.expanded && root.activeFocus && !root.textInputFocused && root.editable
        context: Qt.WindowShortcut
        autoRepeat: false
    }
    component TimelineZoomShortcut: Shortcut {
        enabled: root.expanded && root.navigationEnabled && !root.textInputFocused && (root.activeFocus || trackInput.containsMouse)
        context: Qt.WindowShortcut
        autoRepeat: true
    }
    TimelineZoomShortcut {
        sequences: ["+", "=", "Ctrl++", "Ctrl+=", "Meta++", "Meta+="]
        onActivated: root.zoom(0.5)
    }
    TimelineZoomShortcut {
        sequences: ["-", "Ctrl+-", "Meta+-"]
        onActivated: root.zoom(2)
    }
    TimelineShortcut { sequence: "Left"; autoRepeat: true; onActivated: root.timeline.stepSlots(-1) }
    TimelineShortcut { sequence: "Right"; autoRepeat: true; onActivated: root.timeline.stepSlots(1) }
    TimelineShortcut { sequence: "Shift+Left"; autoRepeat: true; onActivated: root.jumpToClipBoundary(-1) }
    TimelineShortcut { sequence: "Shift+Right"; autoRepeat: true; onActivated: root.jumpToClipBoundary(1) }
    TimelineShortcut {
        sequences: ["Delete", "Backspace", "Ctrl+Backspace"]
        onActivated: root.timeline.deleteSelected()
    }
    TimelineShortcut {
        sequences: Qt.platform.os === "osx" ? [StandardKey.Copy, "Meta+C"] : [StandardKey.Copy]
        onActivated: root.timeline.copySelected()
    }
    TimelineShortcut {
        sequences: Qt.platform.os === "osx" ? [StandardKey.Paste, "Meta+V"] : [StandardKey.Paste]
        onActivated: root.timeline.paste()
    }
}
