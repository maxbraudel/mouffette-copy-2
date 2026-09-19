import QtQuick
import QtQuick.Controls
import Mouffette.App

Rectangle {
    id: clipItem
    required property var modelData
    required property var panel
    required property Item timeContent
    required property real trackHeight
    property bool interactive: true
    objectName: interactive ? "timelineClip" : "otherMediaClip"
    readonly property bool isClipDrag: true
    property bool dragging: false
    property int initialTrack: 0
    property int previewTrack: 0
    property int requestedTrack: 0
    property int lastValidTrack: 0
    property real lastValidStart: 0
    property real lastValidEnd: 0
    property real pressContentX: 0
    property real pressContentY: 0
    property real pressPanelX: 0
    property real pressPanelY: 0
    property real pointerPanelX: 0
    property real pointerPanelY: 0
    readonly property int shownTrack: dragging ? previewTrack : modelData.displayTrackIndex
    property int editEdge: 0
    property real previewStart: 0
    property real previewEnd: 0
    property real rawMs: 0
    property real initialStart: 0
    property real initialEnd: 0
    readonly property real shownStart: dragging ? previewStart : modelData.startMs
    readonly property real shownEnd: dragging ? previewEnd : modelData.startMs + modelData.durationMs
    readonly property bool selected: interactive && modelData.selected
    function refreshPreview() {
        if (editEdge === 0) {
            previewStart = Math.max(0, Math.min(panel.maximumMs - (initialEnd - initialStart),
                panel.snapTime(rawMs, modelData.id, initialEnd - initialStart)))
            previewEnd = previewStart + (initialEnd - initialStart)
        } else if (editEdge < 0) {
            previewStart = Math.max(0,
                Math.min(initialEnd - panel.slotMs, panel.snapTime(rawMs, modelData.id, 0)))
            previewEnd = initialEnd
        } else {
            previewStart = initialStart
            previewEnd = Math.max(initialStart + panel.slotMs, Math.min(panel.maximumMs,
                panel.snapTime(rawMs, modelData.id, 0)))
        }
        var wantedStart = previewStart
        var wantedEnd = previewEnd
        var result = panel.timeline.previewClipEdit(modelData.id, wantedStart, wantedEnd, requestedTrack,
            editEdge, lastValidStart, lastValidEnd, lastValidTrack, panel.controlHeld)
        if (result.startMs === undefined) { cancelEdit(); return }
        previewStart = result.startMs; previewEnd = result.endMs; previewTrack = result.row
        if (result.free) {
            lastValidStart = previewStart; lastValidEnd = previewEnd; lastValidTrack = previewTrack
        }
        if (previewStart !== wantedStart || previewEnd !== wantedEnd || previewTrack !== requestedTrack) {
            panel.snapGuideMs = -1; panel.snapGuideLabel = ""
        }
    }
    function refreshFromPointer() {
        var point = timeContent.mapFromItem(panel, pointerPanelX, pointerPanelY)
        rawMs = (editEdge > 0 ? initialEnd : initialStart) + (point.x - pressContentX) / panel.pixelsPerMs
        requestedTrack = editEdge === 0 ? Math.max(0, Math.min(panel.timeline.trackCount - 1,
            initialTrack + Math.round((point.y - pressContentY) / trackHeight))) : initialTrack
        refreshPreview()
    }
    function beginEdit(edge, mouse, area) {
        if (mouse.button !== Qt.LeftButton && !(mouse.modifiers & panel.controlModifier)) {
            mouse.accepted = false; return
        }
        panel.focusTrack(); panel.updateModifiers(mouse.modifiers)
        panel.timeline.selectClip(modelData.id)
        initialTrack = modelData.displayTrackIndex; previewTrack = initialTrack
        requestedTrack = initialTrack; lastValidTrack = initialTrack
        initialStart = modelData.startMs; initialEnd = modelData.startMs + modelData.durationMs
        previewStart = initialStart; previewEnd = initialEnd
        lastValidStart = initialStart; lastValidEnd = initialEnd
        editEdge = edge; rawMs = edge > 0 ? initialEnd : initialStart
        var point = area.mapToItem(timeContent, mouse.x, mouse.y)
        pressContentX = point.x; pressContentY = point.y
        var panelPoint = area.mapToItem(panel, mouse.x, mouse.y)
        pressPanelX = panelPoint.x; pressPanelY = panelPoint.y
        pointerPanelX = panelPoint.x; pointerPanelY = panelPoint.y
    }
    function updateEdit(mouse, area) {
        panel.updateModifiers(mouse.modifiers)
        var point = area.mapToItem(panel, mouse.x, mouse.y)
        pointerPanelX = point.x; pointerPanelY = point.y
        if (!dragging) {
            // Measure intent in fixed panel coordinates, independently of scrolling.
            var dx = Math.abs(pointerPanelX - pressPanelX)
            var dy = editEdge === 0 ? Math.abs(pointerPanelY - pressPanelY) : 0
            if (Math.max(dx, dy) < Qt.styleHints.startDragDistance) return
            dragging = true; panel.activeDrag = clipItem
        }
        refreshFromPointer()
    }
    function cancelEdit() { dragging = false; panel.endDrag() }
    function finishEdit(mouse, area) {
        if (!dragging) return
        panel.updateModifiers(mouse.modifiers)
        var point = area.mapToItem(panel, mouse.x, mouse.y)
        pointerPanelX = point.x; pointerPanelY = point.y
        refreshFromPointer()
        var overwrite = panel.controlHeld
        var id = modelData.id; var start = panel.clampTime(previewStart); var end = panel.clampTime(previewEnd); var edge = editEdge; var track = previewTrack
        dragging = false; panel.endDrag()
        if (start === initialStart && end === initialEnd && track === initialTrack) return
        if (edge === 0) panel.timeline.moveClip(id, start, track, overwrite)
        else panel.timeline.trimClip(id, start, end, overwrite)
    }
    x: 12 + shownStart * panel.pixelsPerMs
    y: (shownTrack + panel.firstTrackIndex) * trackHeight
    z: dragging ? 3 : selected ? 2 : 1
    width: Math.max(2, (shownEnd - shownStart) * panel.pixelsPerMs)
    height: Math.max(0, trackHeight)
    radius: 3
    color: selected
        ? Theme.controlSelectionBackground : interactive ? Theme.overlayHover : Theme.overlayPressed
    border.width: 1
    border.color: interactive ? Theme.overlayText : Theme.overlayBorder
    // Resize hit areas straddle the border; the viewport provides clipping.
    clip: false
    readonly property real shownSourceIn: modelData.sourceInMs
        + (dragging && editEdge < 0 ? shownStart - initialStart : 0)
    readonly property real shownDuration: shownEnd - shownStart
    readonly property real leadingHoldMs: modelData.isVideo
        ? Math.min(shownDuration, Math.max(0, -shownSourceIn)) : 0
    readonly property real trailingHoldMs: modelData.isVideo
        ? Math.min(shownDuration, Math.max(0, shownSourceIn + shownDuration - modelData.actualSourceDurationMs)) : 0
    HoldRegion {
        objectName: "timelineClipLeadingHold"
        width: clipItem.leadingHoldMs * panel.pixelsPerMs
    }
    HoldRegion {
        objectName: "timelineClipTrailingHold"
        x: clipItem.width - width
        width: clipItem.trailingHoldMs * panel.pixelsPerMs
    }
    ToolTip.visible: clipHover.hovered
    ToolTip.text: modelData.mediaName
        + (leadingHoldMs > 0 ? "\nFirst frame held silently for " + leadingHoldMs.toFixed(3) + " ms" : "")
        + (trailingHoldMs > 0 ? "\nLast frame held silently for " + trailingHoldMs.toFixed(3) + " ms" : "")
    HoverHandler { id: clipHover }
    Item {
        id: clipLabel
        objectName: "timelineClipLabel"
        // Center the label within the intersection of this clip and the viewport.
        readonly property real leftBound: Math.max(8, panel.visibleStartX - clipItem.x + 8)
        readonly property real rightBound: Math.min(clipItem.width - 8, panel.visibleEndX - clipItem.x - 8)
        implicitWidth: clipTitle.implicitWidth
        width: Math.min(implicitWidth, Math.max(0, rightBound - leftBound))
        height: parent.height
        x: leftBound + Math.max(0, rightBound - leftBound - width) / 2
        visible: clipItem.interactive && width > 0
        Text {
            id: clipTitle
            objectName: "timelineClipTitle"
            width: parent.width
            height: parent.height
            text: clipItem.modelData.mediaName
            textFormat: Text.PlainText
            font.pixelSize: 10; color: Theme.overlayText
            verticalAlignment: Text.AlignVCenter; elide: Text.ElideRight
        }
    }
    component HoldRegion: Rectangle {
        height: clipItem.height
        visible: width > 0
        color: Theme.overlayText
        opacity: 0.12
    }
    MouseArea {
        id: clipMove
        anchors.fill: parent
        enabled: clipItem.interactive && panel.editable
        cursorShape: clipItem.dragging ? Qt.ClosedHandCursor : Qt.OpenHandCursor
        acceptedButtons: Qt.LeftButton | (Qt.platform.os === "osx" ? Qt.RightButton : Qt.NoButton)
        onPressed: mouse => clipItem.beginEdit(0, mouse, clipMove)
        onPositionChanged: mouse => { if (pressed) clipItem.updateEdit(mouse, clipMove) }
        onReleased: mouse => clipItem.finishEdit(mouse, clipMove)
        onCanceled: clipItem.cancelEdit()
    }
    Repeater {
        model: [-1, 1]
        MouseArea {
            id: trimHandle
            required property int modelData
            objectName: modelData < 0 ? "timelineClipTrimStart" : "timelineClipTrimEnd"
            x: (modelData < 0 ? 0 : clipItem.width) - width / 2
            width: Math.min(8, clipItem.width / 3); height: clipItem.height
            visible: clipItem.interactive
            enabled: clipItem.interactive && panel.editable
            cursorShape: Qt.SizeHorCursor
            containmentMask: QtObject {
                function contains(p: point): bool {
                    if (p.x < 0 || p.x >= trimHandle.width
                            || p.y < 0 || p.y >= trimHandle.height) return false
                    var clipX = trimHandle.x + p.x
                    // Inside a clip, its own edge wins over a neighbour's overhang.
                    if (clipX >= 0 && clipX < clipItem.width) return true
                    return panel.clipOwnsResizePoint(clipItem, clipItem.x + clipX)
                }
            }
            acceptedButtons: Qt.LeftButton | (Qt.platform.os === "osx" ? Qt.RightButton : Qt.NoButton)
            onPressed: mouse => clipItem.beginEdit(modelData, mouse, trimHandle)
            onPositionChanged: mouse => { if (pressed) clipItem.updateEdit(mouse, trimHandle) }
            onReleased: mouse => clipItem.finishEdit(mouse, trimHandle)
            onCanceled: clipItem.cancelEdit()
        }
    }
}
