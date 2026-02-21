import QtQuick 2.15
import QtQuick.Controls 2.15
import QtMultimedia

Rectangle {
    id: root
    color: "transparent"
    focus: true

    property real panStartX: 0
    property real panStartY: 0
    property real pinchStartZoom: 1.0

    function selectedMediaDelegate() {
        if (!quickCanvasModel || !quickCanvasModel.selectedMediaId || quickCanvasModel.selectedMediaId.length === 0) {
            return null
        }
        for (var i = 0; i < mediaRepeater.count; ++i) {
            var item = mediaRepeater.itemAt(i)
            if (item && item.mediaIdentifier === quickCanvasModel.selectedMediaId) {
                return item
            }
        }
        return null
    }

    property string pendingMoveMediaId: ""
    property real pendingMoveX: 0
    property real pendingMoveY: 0

    function sceneStrokeWidth(pixelWidth) {
        var zoom = quickCanvasModel ? quickCanvasModel.zoom : 1.0
        if (zoom <= 0) {
            zoom = 1.0
        }
        return pixelWidth / zoom
    }

    Timer {
        id: batchedMoveTimer
        interval: 16
        repeat: false
        onTriggered: {
            if (!quickCanvasModel || !quickCanvasModel.requestMediaMove || root.pendingMoveMediaId.length === 0) {
                return
            }
            quickCanvasModel.requestMediaMove(root.pendingMoveMediaId, root.pendingMoveX, root.pendingMoveY)
        }
    }

    Rectangle {
        anchors.fill: parent
        color: "#111315"
        border.width: 1
        border.color: "#2A2E33"
        radius: 5
    }

    MouseArea {
        id: sceneTextCreateArea
        anchors.fill: parent
        enabled: quickCanvasModel ? quickCanvasModel.textToolActive : false
        acceptedButtons: enabled ? Qt.LeftButton : Qt.NoButton
        hoverEnabled: false
        scrollGestureEnabled: false
        z: -100

        onClicked: function(mouse) {
            if (!quickCanvasModel || !quickCanvasModel.textToolActive || !quickCanvasModel.requestTextMediaCreate) {
                return
            }

            var zoom = quickCanvasModel.zoom
            if (zoom <= 0) {
                zoom = 1.0
            }

            var sceneX = (mouse.x - quickCanvasModel.pan.x) / zoom
            var sceneY = (mouse.y - quickCanvasModel.pan.y) / zoom
            quickCanvasModel.requestTextMediaCreate(sceneX, sceneY)
            mouse.accepted = true
        }
    }

    Item {
        id: sceneLayer
        anchors.fill: parent
        x: quickCanvasModel ? quickCanvasModel.pan.x : 0
        y: quickCanvasModel ? quickCanvasModel.pan.y : 0
        scale: quickCanvasModel ? quickCanvasModel.zoom : 1.0

        Repeater {
            model: quickCanvasModel ? quickCanvasModel.screensModel : null
            delegate: Rectangle {
                x: model.screenX
                y: model.screenY
                width: Math.max(1, model.screenWidth)
                height: Math.max(1, model.screenHeight)
                radius: 5
                color: "#1E2A37"
                border.width: root.sceneStrokeWidth(model.screenPrimary ? 2 : 1)
                border.color: model.screenPrimary ? "#4AA6FF" : "#3A4048"

                Text {
                    anchors.horizontalCenter: parent.horizontalCenter
                    anchors.top: parent.top
                    anchors.topMargin: -22
                    text: model.screenLabel + " (" + model.screenWidth + "x" + model.screenHeight + ")"
                    color: "#EEF3F8"
                    font.pixelSize: 12
                    font.bold: true
                }
            }
        }

        Repeater {
            model: (quickCanvasModel && quickCanvasModel.showDebugGrid) ? 50 : 0
            delegate: Rectangle {
                x: index * 80
                y: 0
                width: 1
                height: root.height * 2
                color: "#1E2328"
                opacity: 0.65
            }
        }

        Repeater {
            model: quickCanvasModel ? quickCanvasModel.guidesModel : null
            delegate: Item {
                x: model.x1
                y: model.y1
                width: Math.max(1, model.length)
                height: 2
                rotation: -model.angle
                transformOrigin: Item.Left

                Rectangle {
                    anchors.fill: parent
                    color: "#48C2FF"
                    opacity: 0.9
                }
            }
        }

        Repeater {
            id: mediaRepeater
            model: quickCanvasModel ? quickCanvasModel.mediaModel : null
            delegate: Rectangle {
                id: mediaRoot
                property bool mediaSelected: model.selected
                property string mediaIdentifier: model.mediaId
                property string mediaTypeName: model.mediaType
                property string mediaUploadState: model.uploadState || "not_uploaded"
                property string textHorizontalAlignment: model.textHorizontalAlignment || "center"
                property string textVerticalAlignment: model.textVerticalAlignment || "center"
                property bool fitToTextEnabled: model.fitToTextEnabled || false
                property string textFontFamily: model.textFontFamily || "Arial"
                property int textFontPixelSize: model.textFontPixelSize > 0 ? model.textFontPixelSize : 14
                property int textFontWeight: model.textFontWeight || Font.Normal
                property bool textItalic: model.textItalic || false
                property bool textUnderline: model.textUnderline || false
                property bool textUppercase: model.textUppercase || false
                property string textColor: model.textColor || "#FFE8EEF5"
                property real textOutlineWidthPercent: model.textOutlineWidthPercent || 0
                property string textOutlineColor: model.textOutlineColor || "#FF000000"
                property bool textHighlightEnabled: model.textHighlightEnabled || false
                property string textHighlightColor: model.textHighlightColor || "#80FFFF00"
                property alias mediaVideoItem: videoItem
                x: model.x
                y: model.y
                width: Math.max(1, model.width)
                height: Math.max(1, model.height)
                radius: 6
                z: model.z
                color: {
                    if (model.mediaType === "text") return "#2A3542"
                    if (model.mediaType === "video") return "#3A2B2B"
                    return "#25364A"
                }
                border.width: 0
                opacity: model.uploadState === "uploaded" ? 0.95 : (model.uploadState === "uploading" ? 0.85 : 0.75)

                function commitMove() {
                    if (quickCanvasModel && quickCanvasModel.requestMediaMove) {
                        quickCanvasModel.requestMediaMove(model.mediaId, mediaRoot.x, mediaRoot.y)
                    }
                }

                function commitResize() {
                    if (quickCanvasModel && quickCanvasModel.requestMediaResize) {
                        quickCanvasModel.requestMediaResize(model.mediaId, mediaRoot.width, mediaRoot.height)
                    }
                }

                function videoIsPlaying() {
                    return model.mediaType === "video" && videoItem.playbackState === MediaPlayer.PlayingState
                }

                function toggleVideoPlayback() {
                    if (model.mediaType !== "video") {
                        return
                    }
                    if (videoItem.playbackState === MediaPlayer.PlayingState) {
                        videoItem.pause()
                    } else {
                        videoItem.play()
                    }
                }

                function toggleVideoMuted() {
                    if (model.mediaType !== "video") {
                        return
                    }
                    videoItem.muted = !videoItem.muted
                }

                function toggleVideoLoop() {
                    if (model.mediaType !== "video") {
                        return
                    }
                    videoItem.loops = (videoItem.loops === 1) ? MediaPlayer.Infinite : 1
                }

                Image {
                    visible: model.mediaType === "image" && model.sourcePath && model.sourcePath.length > 0
                    anchors.fill: parent
                    anchors.margins: 1
                    fillMode: Image.PreserveAspectFit
                    source: visible ? ("file://" + model.sourcePath) : ""
                    cache: true
                    asynchronous: true
                    smooth: true
                }

                Rectangle {
                    visible: model.mediaType === "video" && (!model.sourcePath || model.sourcePath.length === 0)
                    anchors.fill: parent
                    anchors.margins: 1
                    radius: 5
                    color: "#1A1D22"
                    opacity: 0.75

                    Text {
                        anchors.centerIn: parent
                        text: "Video"
                        color: "#D5DCE4"
                        font.pixelSize: 12
                    }
                }

                Video {
                    id: videoItem
                    visible: model.mediaType === "video" && model.sourcePath && model.sourcePath.length > 0
                    anchors.fill: parent
                    anchors.margins: 1
                    source: visible ? ("file://" + model.sourcePath) : ""
                    autoPlay: true
                    muted: true
                    loops: MediaPlayer.Infinite
                    fillMode: VideoOutput.PreserveAspectFit
                }

                Rectangle {
                    visible: model.mediaType === "text"
                    anchors.fill: parent
                    anchors.margins: 1
                    radius: 5
                    color: "#1F2832"
                    opacity: 0.95
                }

                Rectangle {
                    visible: model.mediaType === "text" && mediaRoot.textHighlightEnabled
                    anchors.fill: parent
                    anchors.margins: 6
                    radius: 4
                    color: mediaRoot.textHighlightColor
                    opacity: 0.35
                }

                TextEdit {
                    id: textEditor
                    visible: model.mediaType === "text"
                    anchors.fill: parent
                    anchors.margins: 8
                    text: model.textContent || ""
                    color: mediaRoot.textColor
                    font.family: mediaRoot.textFontFamily
                    font.pixelSize: mediaRoot.textFontPixelSize
                    font.weight: mediaRoot.textFontWeight
                    font.italic: mediaRoot.textItalic
                    font.underline: mediaRoot.textUnderline
                    font.capitalization: mediaRoot.textUppercase ? Font.AllUppercase : Font.MixedCase
                    wrapMode: TextEdit.Wrap
                    selectByMouse: true
                    readOnly: !(model.textEditable && model.selected)
                    onActiveFocusChanged: {
                        if (!activeFocus && quickCanvasModel && quickCanvasModel.requestTextUpdate) {
                            quickCanvasModel.requestTextUpdate(model.mediaId, text)
                        }
                    }
                }

                MouseArea {
                    anchors.fill: parent
                    enabled: !(model.mediaType === "text" && textEditor.activeFocus)
                    onPressed: function(mouse) {
                        if (quickCanvasModel && quickCanvasModel.requestMediaSelect) {
                            quickCanvasModel.requestMediaSelect(model.mediaId, (mouse.modifiers & Qt.ShiftModifier) !== 0)
                        }
                    }
                    onDoubleClicked: {
                        if (quickCanvasModel && quickCanvasModel.requestMediaBringToFront) {
                            quickCanvasModel.requestMediaBringToFront(model.mediaId)
                        }
                    }
                }

                DragHandler {
                    id: mediaDragHandler
                    target: mediaRoot
                    enabled: !(model.mediaType === "text" && textEditor.activeFocus)
                    xAxis.enabled: true
                    yAxis.enabled: true
                    onActiveChanged: {
                        if (!active) {
                            mediaRoot.commitMove()
                        }
                    }
                }

                Rectangle {
                    id: frontButton
                    width: 18
                    height: 18
                    radius: 3
                    anchors.top: parent.top
                    anchors.right: parent.right
                    anchors.topMargin: 4
                    anchors.rightMargin: 4
                    color: "#2E3946"
                    opacity: model.selected ? 0.95 : 0.0

                    Text {
                        anchors.centerIn: parent
                        text: "↑"
                        color: "#E9EEF5"
                        font.pixelSize: 11
                    }

                    MouseArea {
                        anchors.fill: parent
                        onClicked: {
                            if (quickCanvasModel && quickCanvasModel.requestMediaBringToFront) {
                                quickCanvasModel.requestMediaBringToFront(model.mediaId)
                            }
                        }
                    }
                }

                Rectangle {
                    id: backButton
                    width: 18
                    height: 18
                    radius: 3
                    anchors.top: parent.top
                    anchors.right: frontButton.left
                    anchors.topMargin: 4
                    anchors.rightMargin: 4
                    color: "#2E3946"
                    opacity: model.selected ? 0.95 : 0.0

                    Text {
                        anchors.centerIn: parent
                        text: "↓"
                        color: "#E9EEF5"
                        font.pixelSize: 11
                    }

                    MouseArea {
                        anchors.fill: parent
                        onClicked: {
                            if (quickCanvasModel && quickCanvasModel.requestMediaSendToBack) {
                                quickCanvasModel.requestMediaSendToBack(model.mediaId)
                            }
                        }
                    }
                }

                Rectangle {
                    id: resizeHandle
                    visible: model.selected
                    width: 14
                    height: 14
                    radius: 3
                    color: "#FFFFFF"
                    border.width: root.sceneStrokeWidth(1)
                    border.color: "#111111"
                    anchors.right: parent.right
                    anchors.bottom: parent.bottom
                    anchors.rightMargin: -7
                    anchors.bottomMargin: -7

                    MouseArea {
                        id: resizeArea
                        anchors.fill: parent
                        property real startX: 0
                        property real startY: 0
                        property real startWidth: 0
                        property real startHeight: 0
                        cursorShape: Qt.SizeFDiagCursor

                        onPressed: function(mouse) {
                            startX = mouse.x
                            startY = mouse.y
                            startWidth = mediaRoot.width
                            startHeight = mediaRoot.height
                        }

                        onPositionChanged: function(mouse) {
                            var delta = Math.max(mouse.x - startX, mouse.y - startY)
                            var nextWidth = Math.max(40, startWidth + delta)
                            var aspect = startWidth > 0 ? (startHeight / startWidth) : 1.0
                            mediaRoot.width = nextWidth
                            mediaRoot.height = Math.max(24, nextWidth * aspect)
                        }

                        onReleased: {
                            mediaRoot.commitResize()
                            mediaRoot.commitMove()
                        }
                    }
                }

                Text {
                    anchors.left: parent.left
                    anchors.leftMargin: 8
                    anchors.top: parent.top
                    anchors.topMargin: 6
                    text: model.displayName
                    color: "#E6EDF5"
                    font.pixelSize: 11
                    elide: Text.ElideRight
                    width: Math.max(30, parent.width - 16)
                }
            }
        }

        Repeater {
            model: (quickCanvasModel && quickCanvasModel.showDebugGrid) ? 30 : 0
            delegate: Rectangle {
                x: 0
                y: index * 80
                width: root.width * 2
                height: 1
                color: "#1E2328"
                opacity: 0.65
            }
        }

        Rectangle {
            x: 220
            y: 130
            width: 620
            height: 260
            radius: 10
            color: "#1B2026"
            border.width: root.sceneStrokeWidth(1)
            border.color: "#3A4048"

            Text {
                anchors.centerIn: parent
                text: "Qt Quick Canvas Prototype"
                color: "#F4F7FA"
                font.pixelSize: 38
                renderType: Text.NativeRendering
            }
        }

        Rectangle {
            visible: quickCanvasModel ? quickCanvasModel.selectionVisible : false
            x: quickCanvasModel ? quickCanvasModel.selectionRect.x : 0
            y: quickCanvasModel ? quickCanvasModel.selectionRect.y : 0
            width: quickCanvasModel ? Math.max(1, quickCanvasModel.selectionRect.width) : 1
            height: quickCanvasModel ? Math.max(1, quickCanvasModel.selectionRect.height) : 1
            color: "transparent"
            border.width: root.sceneStrokeWidth(2)
            border.color: "#FFFFFF"
            opacity: 0.9
        }

        Rectangle {
            visible: quickCanvasModel ? quickCanvasModel.remoteCursorVisible : false
            x: quickCanvasModel ? (quickCanvasModel.remoteCursor.x - width / 2) : 0
            y: quickCanvasModel ? (quickCanvasModel.remoteCursor.y - height / 2) : 0
            width: 26
            height: 26
            radius: width / 2
            color: "#FFFFFF"
            border.width: root.sceneStrokeWidth(2)
            border.color: "#111111"
            z: 99999
        }
    }

    // Cosmetic media border overlay — rendered in viewport/screen space so the
    // border thickness is completely independent of camera zoom, matching the
    // Widget Canvas pen.setCosmetic(true) behaviour in ScreenCanvas.cpp.
    Repeater {
        id: mediaBorderOverlay
        model: quickCanvasModel ? quickCanvasModel.mediaModel : null
        delegate: Item {
            id: borderDelegate
            // Capture zoom and pan as explicit binding deps so the position
            // re-evaluates whenever the camera moves or zooms.
            property real _zoom: quickCanvasModel ? quickCanvasModel.zoom : 1.0
            property var  _pan:  quickCanvasModel ? quickCanvasModel.pan  : Qt.point(0, 0)
            readonly property point _topLeft: {
                var _dz = _zoom; var _dp = _pan   // force dep tracking
                return sceneLayer.mapToItem(root, model.x, model.y)
            }
            // Snap to integer pixels — keeps the 1 px border crisp at all zoom levels
            x: Math.round(_topLeft.x)
            y: Math.round(_topLeft.y)
            width:  Math.round(Math.max(1, model.width)  * _zoom)
            height: Math.round(Math.max(1, model.height) * _zoom)
            z: 49999
            enabled: false  // never intercept mouse/touch events

            Rectangle {
                anchors.fill: parent
                color: "transparent"
                // border.width is in unscaled viewport pixels — always exactly 1 or 2 px
                border.width: model.selected ? 2 : 1
                border.color: model.selected ? "#FFFFFF" : "#6B7785"
                radius: Math.round(6 * borderDelegate._zoom)
                antialiasing: false
            }
        }
    }

    Rectangle {
        id: quickOverlayPanel
        visible: false
        anchors.top: parent.top
        anchors.left: parent.left
        anchors.topMargin: 10
        anchors.leftMargin: 10
        width: 280
        height: Math.min(540, Math.max(220, parent.height - 40))
        radius: 8
        color: "#0C0F12"
        border.width: 1
        border.color: "#2B3138"
        opacity: 0.92

        function selectedMediaId() {
            var selected = root.selectedMediaDelegate()
            return selected ? selected.mediaIdentifier : ""
        }

        function selectedMediaType() {
            var selected = root.selectedMediaDelegate()
            return selected ? selected.mediaTypeName : ""
        }

        function selectedTextHorizontal() {
            var selected = root.selectedMediaDelegate()
            return selected ? selected.textHorizontalAlignment : "center"
        }

        function selectedTextVertical() {
            var selected = root.selectedMediaDelegate()
            return selected ? selected.textVerticalAlignment : "center"
        }

        function selectedFitToText() {
            var selected = root.selectedMediaDelegate()
            return selected ? selected.fitToTextEnabled : false
        }

        function selectedTextWeight() {
            var selected = root.selectedMediaDelegate()
            return selected ? selected.textFontWeight : Font.Normal
        }

        function selectedTextItalic() {
            var selected = root.selectedMediaDelegate()
            return selected ? selected.textItalic : false
        }

        function selectedTextUnderline() {
            var selected = root.selectedMediaDelegate()
            return selected ? selected.textUnderline : false
        }

        function selectedTextUppercase() {
            var selected = root.selectedMediaDelegate()
            return selected ? selected.textUppercase : false
        }

        function selectedTextPixelSize() {
            var selected = root.selectedMediaDelegate()
            return selected ? selected.textFontPixelSize : 14
        }

        function selectedTextFamily() {
            var selected = root.selectedMediaDelegate()
            return selected ? selected.textFontFamily : "Arial"
        }

        function selectedTextColor() {
            var selected = root.selectedMediaDelegate()
            return selected ? selected.textColor : "#FFE8EEF5"
        }

        function selectedOutlineWidthPercent() {
            var selected = root.selectedMediaDelegate()
            return selected ? selected.textOutlineWidthPercent : 0
        }

        function selectedOutlineColor() {
            var selected = root.selectedMediaDelegate()
            return selected ? selected.textOutlineColor : "#FF000000"
        }

        function selectedHighlightEnabled() {
            var selected = root.selectedMediaDelegate()
            return selected ? selected.textHighlightEnabled : false
        }

        function selectedHighlightColor() {
            var selected = root.selectedMediaDelegate()
            return selected ? selected.textHighlightColor : "#80FFFF00"
        }

        function applyTextLayout(horizontal, vertical, fitToTextEnabled) {
            if (!quickCanvasModel || !quickCanvasModel.requestTextLayoutUpdate) {
                return
            }
            var id = selectedMediaId()
            if (!id || id.length === 0) {
                return
            }
            quickCanvasModel.requestTextLayoutUpdate(id, horizontal, vertical, fitToTextEnabled)
        }

        function applyTextStyle(weight, italic, underline, uppercase, pixelSize, family, color, outlineWidth, outlineColor, highlightEnabled, highlightColor) {
            if (!quickCanvasModel || !quickCanvasModel.requestTextStyleUpdate) {
                return
            }
            var id = selectedMediaId()
            if (!id || id.length === 0) {
                return
            }
            quickCanvasModel.requestTextStyleUpdate(id, weight, italic, underline, uppercase, pixelSize, family, color, outlineWidth, outlineColor, highlightEnabled, highlightColor)
        }

        Column {
            anchors.fill: parent
            anchors.margins: 10
            spacing: 8

            Text {
                text: "Media"
                color: "#E4EAF2"
                font.pixelSize: 13
                font.bold: true
            }

            ListView {
                id: mediaListView
                width: parent.width
                height: (quickOverlayPanel.selectedMediaType() === "text")
                    ? (quickOverlayPanel.height - 370)
                        : (quickOverlayPanel.height - 86)
                clip: true
                spacing: 4
                model: quickCanvasModel ? quickCanvasModel.mediaModel : null

                delegate: Rectangle {
                    width: mediaListView.width
                    height: 30
                    radius: 4
                    color: model.selected ? "#2A3645" : "#13181E"
                    border.width: 1
                    border.color: model.selected ? "#6AAEFF" : "#2B333D"

                    Text {
                        anchors.verticalCenter: parent.verticalCenter
                        anchors.left: parent.left
                        anchors.leftMargin: 8
                        text: model.displayName + "  [" + model.mediaType + "]"
                        color: "#DFE6EE"
                        font.pixelSize: 11
                        elide: Text.ElideRight
                        width: parent.width - 16
                    }

                    MouseArea {
                        anchors.fill: parent
                        onClicked: {
                            if (quickCanvasModel && quickCanvasModel.requestMediaSelect) {
                                quickCanvasModel.requestMediaSelect(model.mediaId, false)
                            }
                        }
                    }
                }
            }

            Row {
                spacing: 6

                Button {
                    text: "Front"
                    enabled: quickOverlayPanel.selectedMediaId().length > 0
                    onClicked: {
                        if (quickCanvasModel && quickCanvasModel.requestMediaBringToFront) {
                            quickCanvasModel.requestMediaBringToFront(quickOverlayPanel.selectedMediaId())
                        }
                    }
                }

                Button {
                    text: "Back"
                    enabled: quickOverlayPanel.selectedMediaId().length > 0
                    onClicked: {
                        if (quickCanvasModel && quickCanvasModel.requestMediaSendToBack) {
                            quickCanvasModel.requestMediaSendToBack(quickOverlayPanel.selectedMediaId())
                        }
                    }
                }

                Button {
                    text: "Delete"
                    enabled: quickOverlayPanel.selectedMediaId().length > 0
                    onClicked: {
                        if (quickCanvasModel && quickCanvasModel.requestMediaDelete) {
                            quickCanvasModel.requestMediaDelete(quickOverlayPanel.selectedMediaId())
                        }
                    }
                }
            }

            Column {
                visible: quickOverlayPanel.selectedMediaType() === "text"
                spacing: 4

                Text {
                    text: "Text Layout"
                    color: "#CAD4DF"
                    font.pixelSize: 11
                    font.bold: true
                }

                Row {
                    spacing: 4

                    Button {
                        text: "L"
                        onClicked: quickOverlayPanel.applyTextLayout("left", quickOverlayPanel.selectedTextVertical(), quickOverlayPanel.selectedFitToText())
                    }
                    Button {
                        text: "C"
                        onClicked: quickOverlayPanel.applyTextLayout("center", quickOverlayPanel.selectedTextVertical(), quickOverlayPanel.selectedFitToText())
                    }
                    Button {
                        text: "R"
                        onClicked: quickOverlayPanel.applyTextLayout("right", quickOverlayPanel.selectedTextVertical(), quickOverlayPanel.selectedFitToText())
                    }
                }

                Row {
                    spacing: 4

                    Button {
                        text: "Top"
                        onClicked: quickOverlayPanel.applyTextLayout(quickOverlayPanel.selectedTextHorizontal(), "top", quickOverlayPanel.selectedFitToText())
                    }
                    Button {
                        text: "Mid"
                        onClicked: quickOverlayPanel.applyTextLayout(quickOverlayPanel.selectedTextHorizontal(), "center", quickOverlayPanel.selectedFitToText())
                    }
                    Button {
                        text: "Bot"
                        onClicked: quickOverlayPanel.applyTextLayout(quickOverlayPanel.selectedTextHorizontal(), "bottom", quickOverlayPanel.selectedFitToText())
                    }
                    Button {
                        text: quickOverlayPanel.selectedFitToText() ? "Fit On" : "Fit Off"
                        onClicked: quickOverlayPanel.applyTextLayout(
                            quickOverlayPanel.selectedTextHorizontal(),
                            quickOverlayPanel.selectedTextVertical(),
                            !quickOverlayPanel.selectedFitToText())
                    }
                }

                Text {
                    text: "Text Style"
                    color: "#CAD4DF"
                    font.pixelSize: 11
                    font.bold: true
                }

                Row {
                    spacing: 4

                    Button {
                        text: "B"
                        onClicked: quickOverlayPanel.applyTextStyle(
                            quickOverlayPanel.selectedTextWeight() >= 700 ? 400 : 700,
                            quickOverlayPanel.selectedTextItalic(),
                            quickOverlayPanel.selectedTextUnderline(),
                            quickOverlayPanel.selectedTextUppercase(),
                            quickOverlayPanel.selectedTextPixelSize(),
                            quickOverlayPanel.selectedTextFamily(),
                            quickOverlayPanel.selectedTextColor(),
                            quickOverlayPanel.selectedOutlineWidthPercent(),
                            quickOverlayPanel.selectedOutlineColor(),
                            quickOverlayPanel.selectedHighlightEnabled(),
                            quickOverlayPanel.selectedHighlightColor())
                    }

                    Button {
                        text: "I"
                        onClicked: quickOverlayPanel.applyTextStyle(
                            quickOverlayPanel.selectedTextWeight(),
                            !quickOverlayPanel.selectedTextItalic(),
                            quickOverlayPanel.selectedTextUnderline(),
                            quickOverlayPanel.selectedTextUppercase(),
                            quickOverlayPanel.selectedTextPixelSize(),
                            quickOverlayPanel.selectedTextFamily(),
                            quickOverlayPanel.selectedTextColor(),
                            quickOverlayPanel.selectedOutlineWidthPercent(),
                            quickOverlayPanel.selectedOutlineColor(),
                            quickOverlayPanel.selectedHighlightEnabled(),
                            quickOverlayPanel.selectedHighlightColor())
                    }

                    Button {
                        text: "U"
                        onClicked: quickOverlayPanel.applyTextStyle(
                            quickOverlayPanel.selectedTextWeight(),
                            quickOverlayPanel.selectedTextItalic(),
                            !quickOverlayPanel.selectedTextUnderline(),
                            quickOverlayPanel.selectedTextUppercase(),
                            quickOverlayPanel.selectedTextPixelSize(),
                            quickOverlayPanel.selectedTextFamily(),
                            quickOverlayPanel.selectedTextColor(),
                            quickOverlayPanel.selectedOutlineWidthPercent(),
                            quickOverlayPanel.selectedOutlineColor(),
                            quickOverlayPanel.selectedHighlightEnabled(),
                            quickOverlayPanel.selectedHighlightColor())
                    }

                    Button {
                        text: "Aa"
                        onClicked: quickOverlayPanel.applyTextStyle(
                            quickOverlayPanel.selectedTextWeight(),
                            quickOverlayPanel.selectedTextItalic(),
                            quickOverlayPanel.selectedTextUnderline(),
                            !quickOverlayPanel.selectedTextUppercase(),
                            quickOverlayPanel.selectedTextPixelSize(),
                            quickOverlayPanel.selectedTextFamily(),
                            quickOverlayPanel.selectedTextColor(),
                            quickOverlayPanel.selectedOutlineWidthPercent(),
                            quickOverlayPanel.selectedOutlineColor(),
                            quickOverlayPanel.selectedHighlightEnabled(),
                            quickOverlayPanel.selectedHighlightColor())
                    }
                }

                Row {
                    spacing: 4

                    Button {
                        text: "Size-"
                        onClicked: quickOverlayPanel.applyTextStyle(
                            quickOverlayPanel.selectedTextWeight(),
                            quickOverlayPanel.selectedTextItalic(),
                            quickOverlayPanel.selectedTextUnderline(),
                            quickOverlayPanel.selectedTextUppercase(),
                            Math.max(8, quickOverlayPanel.selectedTextPixelSize() - 2),
                            quickOverlayPanel.selectedTextFamily(),
                            quickOverlayPanel.selectedTextColor(),
                            quickOverlayPanel.selectedOutlineWidthPercent(),
                            quickOverlayPanel.selectedOutlineColor(),
                            quickOverlayPanel.selectedHighlightEnabled(),
                            quickOverlayPanel.selectedHighlightColor())
                    }
                    Button {
                        text: "Size+"
                        onClicked: quickOverlayPanel.applyTextStyle(
                            quickOverlayPanel.selectedTextWeight(),
                            quickOverlayPanel.selectedTextItalic(),
                            quickOverlayPanel.selectedTextUnderline(),
                            quickOverlayPanel.selectedTextUppercase(),
                            Math.min(160, quickOverlayPanel.selectedTextPixelSize() + 2),
                            quickOverlayPanel.selectedTextFamily(),
                            quickOverlayPanel.selectedTextColor(),
                            quickOverlayPanel.selectedOutlineWidthPercent(),
                            quickOverlayPanel.selectedOutlineColor(),
                            quickOverlayPanel.selectedHighlightEnabled(),
                            quickOverlayPanel.selectedHighlightColor())
                    }
                    Button {
                        text: quickOverlayPanel.selectedHighlightEnabled() ? "HL On" : "HL Off"
                        onClicked: quickOverlayPanel.applyTextStyle(
                            quickOverlayPanel.selectedTextWeight(),
                            quickOverlayPanel.selectedTextItalic(),
                            quickOverlayPanel.selectedTextUnderline(),
                            quickOverlayPanel.selectedTextUppercase(),
                            quickOverlayPanel.selectedTextPixelSize(),
                            quickOverlayPanel.selectedTextFamily(),
                            quickOverlayPanel.selectedTextColor(),
                            quickOverlayPanel.selectedOutlineWidthPercent(),
                            quickOverlayPanel.selectedOutlineColor(),
                            !quickOverlayPanel.selectedHighlightEnabled(),
                            quickOverlayPanel.selectedHighlightColor())
                    }
                }

                Row {
                    spacing: 4

                    TextField {
                        id: textColorField
                        width: 78
                        text: quickOverlayPanel.selectedTextColor()
                        placeholderText: "#FFFFFFFF"
                        onAccepted: quickOverlayPanel.applyTextStyle(
                            quickOverlayPanel.selectedTextWeight(),
                            quickOverlayPanel.selectedTextItalic(),
                            quickOverlayPanel.selectedTextUnderline(),
                            quickOverlayPanel.selectedTextUppercase(),
                            quickOverlayPanel.selectedTextPixelSize(),
                            quickOverlayPanel.selectedTextFamily(),
                            text,
                            quickOverlayPanel.selectedOutlineWidthPercent(),
                            quickOverlayPanel.selectedOutlineColor(),
                            quickOverlayPanel.selectedHighlightEnabled(),
                            quickOverlayPanel.selectedHighlightColor())
                    }

                    TextField {
                        id: outlineColorField
                        width: 78
                        text: quickOverlayPanel.selectedOutlineColor()
                        placeholderText: "#FF000000"
                        onAccepted: quickOverlayPanel.applyTextStyle(
                            quickOverlayPanel.selectedTextWeight(),
                            quickOverlayPanel.selectedTextItalic(),
                            quickOverlayPanel.selectedTextUnderline(),
                            quickOverlayPanel.selectedTextUppercase(),
                            quickOverlayPanel.selectedTextPixelSize(),
                            quickOverlayPanel.selectedTextFamily(),
                            quickOverlayPanel.selectedTextColor(),
                            quickOverlayPanel.selectedOutlineWidthPercent(),
                            text,
                            quickOverlayPanel.selectedHighlightEnabled(),
                            quickOverlayPanel.selectedHighlightColor())
                    }

                    TextField {
                        id: highlightColorField
                        width: 78
                        text: quickOverlayPanel.selectedHighlightColor()
                        placeholderText: "#80FFFF00"
                        onAccepted: quickOverlayPanel.applyTextStyle(
                            quickOverlayPanel.selectedTextWeight(),
                            quickOverlayPanel.selectedTextItalic(),
                            quickOverlayPanel.selectedTextUnderline(),
                            quickOverlayPanel.selectedTextUppercase(),
                            quickOverlayPanel.selectedTextPixelSize(),
                            quickOverlayPanel.selectedTextFamily(),
                            quickOverlayPanel.selectedTextColor(),
                            quickOverlayPanel.selectedOutlineWidthPercent(),
                            quickOverlayPanel.selectedOutlineColor(),
                            quickOverlayPanel.selectedHighlightEnabled(),
                            text)
                    }
                }

                Row {
                    spacing: 4

                    Button {
                        text: "Outline-"
                        onClicked: quickOverlayPanel.applyTextStyle(
                            quickOverlayPanel.selectedTextWeight(),
                            quickOverlayPanel.selectedTextItalic(),
                            quickOverlayPanel.selectedTextUnderline(),
                            quickOverlayPanel.selectedTextUppercase(),
                            quickOverlayPanel.selectedTextPixelSize(),
                            quickOverlayPanel.selectedTextFamily(),
                            quickOverlayPanel.selectedTextColor(),
                            Math.max(0, quickOverlayPanel.selectedOutlineWidthPercent() - 1),
                            quickOverlayPanel.selectedOutlineColor(),
                            quickOverlayPanel.selectedHighlightEnabled(),
                            quickOverlayPanel.selectedHighlightColor())
                    }
                    Button {
                        text: "Outline+"
                        onClicked: quickOverlayPanel.applyTextStyle(
                            quickOverlayPanel.selectedTextWeight(),
                            quickOverlayPanel.selectedTextItalic(),
                            quickOverlayPanel.selectedTextUnderline(),
                            quickOverlayPanel.selectedTextUppercase(),
                            quickOverlayPanel.selectedTextPixelSize(),
                            quickOverlayPanel.selectedTextFamily(),
                            quickOverlayPanel.selectedTextColor(),
                            Math.min(100, quickOverlayPanel.selectedOutlineWidthPercent() + 1),
                            quickOverlayPanel.selectedOutlineColor(),
                            quickOverlayPanel.selectedHighlightEnabled(),
                            quickOverlayPanel.selectedHighlightColor())
                    }

                    TextField {
                        id: familyField
                        width: 90
                        text: quickOverlayPanel.selectedTextFamily()
                        placeholderText: "Font"
                        onAccepted: quickOverlayPanel.applyTextStyle(
                            quickOverlayPanel.selectedTextWeight(),
                            quickOverlayPanel.selectedTextItalic(),
                            quickOverlayPanel.selectedTextUnderline(),
                            quickOverlayPanel.selectedTextUppercase(),
                            quickOverlayPanel.selectedTextPixelSize(),
                            text,
                            quickOverlayPanel.selectedTextColor(),
                            quickOverlayPanel.selectedOutlineWidthPercent(),
                            quickOverlayPanel.selectedOutlineColor(),
                            quickOverlayPanel.selectedHighlightEnabled(),
                            quickOverlayPanel.selectedHighlightColor())
                    }
                }
            }

            Column {
                visible: quickOverlayPanel.selectedMediaType() === "video"
                spacing: 4

                Text {
                    text: "Video"
                    color: "#CAD4DF"
                    font.pixelSize: 11
                    font.bold: true
                }

                Row {
                    spacing: 4

                    Button {
                        text: {
                            var selected = root.selectedMediaDelegate()
                            return selected && selected.videoIsPlaying() ? "Pause" : "Play"
                        }
                        onClicked: {
                            var selected = root.selectedMediaDelegate()
                            if (selected) {
                                selected.toggleVideoPlayback()
                            }
                        }
                    }

                    Button {
                        text: {
                            var selected = root.selectedMediaDelegate()
                            if (!selected) return "Mute"
                            return selected.mediaVideoItem.muted ? "Unmute" : "Mute"
                        }
                        onClicked: {
                            var selected = root.selectedMediaDelegate()
                            if (selected) {
                                selected.toggleVideoMuted()
                            }
                        }
                    }

                    Button {
                        text: {
                            var selected = root.selectedMediaDelegate()
                            if (!selected) return "Loop"
                            return selected.mediaVideoItem.loops === MediaPlayer.Infinite ? "Loop∞" : "Loop1"
                        }
                        onClicked: {
                            var selected = root.selectedMediaDelegate()
                            if (selected) {
                                selected.toggleVideoLoop()
                            }
                        }
                    }
                }
            }

            Column {
                spacing: 3

                Text {
                    text: "Selection Info"
                    color: "#CAD4DF"
                    font.pixelSize: 11
                    font.bold: true
                }

                Text {
                    color: "#AEB9C6"
                    font.pixelSize: 10
                    text: {
                        var selected = root.selectedMediaDelegate()
                        if (!selected) {
                            return "No media selected"
                        }
                        return "id=" + selected.mediaIdentifier + " type=" + selected.mediaTypeName + " upload=" + selected.mediaUploadState
                    }
                }

                Text {
                    color: "#AEB9C6"
                    font.pixelSize: 10
                    text: {
                        var selected = root.selectedMediaDelegate()
                        if (!selected) {
                            return ""
                        }
                        return "x=" + selected.x.toFixed(1)
                            + " y=" + selected.y.toFixed(1)
                            + " w=" + selected.width.toFixed(1)
                            + " h=" + selected.height.toFixed(1)
                            + " z=" + selected.z.toFixed(1)
                    }
                }
            }
        }
    }

    Rectangle {
        id: mediaListOverlay
        visible: false
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        anchors.rightMargin: 10
        anchors.bottomMargin: 46
        width: 320
        height: Math.min(300, Math.max(160, parent.height * 0.35))
        radius: 8
        color: "#0C0F12"
        border.width: 1
        border.color: "#2B3138"
        opacity: 0.92

        Column {
            anchors.fill: parent
            anchors.margins: 10
            spacing: 8

            Text {
                text: "Media List"
                color: "#E4EAF2"
                font.pixelSize: 13
                font.bold: true
            }

            ListView {
                id: mediaListOverlayView
                width: parent.width
                height: parent.height - 28
                clip: true
                spacing: 4
                model: quickCanvasModel ? quickCanvasModel.mediaModel : null

                delegate: Rectangle {
                    width: mediaListOverlayView.width
                    height: 30
                    radius: 4
                    color: model.selected ? "#2A3645" : "#13181E"
                    border.width: 1
                    border.color: model.selected ? "#6AAEFF" : "#2B333D"

                    Text {
                        anchors.verticalCenter: parent.verticalCenter
                        anchors.left: parent.left
                        anchors.leftMargin: 8
                        text: model.displayName + "  [" + model.mediaType + "]"
                        color: "#DFE6EE"
                        font.pixelSize: 11
                        elide: Text.ElideRight
                        width: parent.width - 16
                    }

                    MouseArea {
                        anchors.fill: parent
                        onClicked: {
                            if (quickCanvasModel && quickCanvasModel.requestMediaSelect) {
                                quickCanvasModel.requestMediaSelect(model.mediaId, false)
                            }
                        }
                    }
                }
            }
        }
    }

    Rectangle {
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        height: 36
        color: "#000000"
        opacity: 0.45

        Row {
            anchors.verticalCenter: parent.verticalCenter
            anchors.left: parent.left
            anchors.leftMargin: 12
            spacing: 16

            Text {
                text: quickCanvasModel ? quickCanvasModel.statusText : "Quick Canvas"
                color: "#DFE6EE"
                font.pixelSize: 12
            }

            Text {
                text: quickCanvasModel ? ("zoom=" + quickCanvasModel.zoom.toFixed(2)) : "zoom=1.00"
                color: "#AEB9C6"
                font.pixelSize: 12
            }

            Text {
                text: quickCanvasModel ? ("fps=" + quickCanvasModel.renderFps.toFixed(1) + " frame=" + quickCanvasModel.renderFrameMs.toFixed(1) + "ms") : "fps=0.0 frame=0.0ms"
                color: "#AEB9C6"
                font.pixelSize: 12
            }
        }
    }

    MouseArea {
        id: viewportPanArea
        anchors.fill: parent
        acceptedButtons: Qt.MiddleButton | Qt.RightButton
        hoverEnabled: false
        scrollGestureEnabled: true
        preventStealing: false
        z: 100000

        property real dragStartMouseX: 0
        property real dragStartMouseY: 0
        property real dragStartPanX: 0
        property real dragStartPanY: 0

        onPressed: function(mouse) {
            if (!quickCanvasModel) {
                return
            }

            if (mouse.button === Qt.MiddleButton || mouse.button === Qt.RightButton) {
                dragStartMouseX = mouse.x
                dragStartMouseY = mouse.y
                dragStartPanX = quickCanvasModel.pan.x
                dragStartPanY = quickCanvasModel.pan.y
                mouse.accepted = true
            }
        }

        onPositionChanged: function(mouse) {
            if (!quickCanvasModel) {
                return
            }

            var dragging = (mouse.buttons & Qt.MiddleButton) || (mouse.buttons & Qt.RightButton)
            if (!dragging) {
                return
            }

            var dx = mouse.x - dragStartMouseX
            var dy = mouse.y - dragStartMouseY
            quickCanvasModel.pan = Qt.point(dragStartPanX + dx,
                                            dragStartPanY + dy)
            mouse.accepted = true
        }

        onWheel: function(wheel) {
            if (!quickCanvasModel) {
                return
            }

            var zoomGesture = (wheel.modifiers & Qt.ControlModifier) || (wheel.modifiers & Qt.MetaModifier)
            if (zoomGesture) {
                var factor = Math.pow(1.0015, wheel.angleDelta.y)
                quickCanvasModel.zoom = quickCanvasModel.zoom * factor
                wheel.accepted = true
                return
            }

            var dx = 0
            var dy = 0
            if (wheel.pixelDelta && (wheel.pixelDelta.x !== 0 || wheel.pixelDelta.y !== 0)) {
                dx = wheel.pixelDelta.x
                dy = wheel.pixelDelta.y
            } else {
                dx = wheel.angleDelta.x / 8.0
                dy = wheel.angleDelta.y / 8.0
            }

            quickCanvasModel.pan = Qt.point(quickCanvasModel.pan.x + dx,
                                            quickCanvasModel.pan.y + dy)
            wheel.accepted = true
        }
    }

    Keys.onPressed: function(event) {
        if (!quickCanvasModel) return

        var selected = root.selectedMediaDelegate()
        var step = (event.modifiers & Qt.ShiftModifier) ? 20 : 5

        if (event.key === Qt.Key_Plus || event.key === Qt.Key_Equal) {
            quickCanvasModel.zoomIn()
            event.accepted = true
            return
        }
        if (event.key === Qt.Key_Minus || event.key === Qt.Key_Underscore) {
            quickCanvasModel.zoomOut()
            event.accepted = true
            return
        }
        if (event.key === Qt.Key_0) {
            quickCanvasModel.resetView()
            event.accepted = true
            return
        }

        if (!selected) {
            return
        }

        if (event.key === Qt.Key_Delete || event.key === Qt.Key_Backspace) {
            if (quickCanvasModel.requestMediaDelete) {
                quickCanvasModel.requestMediaDelete(selected.mediaIdentifier)
            }
            event.accepted = true
            return
        }
        if (event.key === Qt.Key_BracketRight) {
            if (quickCanvasModel.requestMediaBringToFront) {
                quickCanvasModel.requestMediaBringToFront(selected.mediaIdentifier)
            }
            event.accepted = true
            return
        }
        if (event.key === Qt.Key_BracketLeft) {
            if (quickCanvasModel.requestMediaSendToBack) {
                quickCanvasModel.requestMediaSendToBack(selected.mediaIdentifier)
            }
            event.accepted = true
            return
        }

        var moved = false
        var nextX = selected.x
        var nextY = selected.y
        if (event.key === Qt.Key_Left) {
            nextX -= step
            moved = true
        } else if (event.key === Qt.Key_Right) {
            nextX += step
            moved = true
        } else if (event.key === Qt.Key_Up) {
            nextY -= step
            moved = true
        } else if (event.key === Qt.Key_Down) {
            nextY += step
            moved = true
        }

        if (moved) {
            selected.x = nextX
            selected.y = nextY
            root.pendingMoveMediaId = selected.mediaIdentifier
            root.pendingMoveX = nextX
            root.pendingMoveY = nextY
            batchedMoveTimer.restart()
            event.accepted = true
        }
    }

    PinchHandler {
        id: pinchZoom
        target: null
        onActiveChanged: {
            if (active && quickCanvasModel) {
                root.pinchStartZoom = quickCanvasModel.zoom
            }
        }
        onActiveScaleChanged: {
            if (!quickCanvasModel || !active) return
            quickCanvasModel.zoom = root.pinchStartZoom * activeScale
        }
    }
}
