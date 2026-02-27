import QtQuick 2.15

BaseMediaItem {
    id: root
    property string textContent: ""
    property string horizontalAlignment: "center"
    property string verticalAlignment: "center"
    property bool fitToTextEnabled: false
    property string fontFamily: "Arial"
    property int fontPixelSize: 22
    property int fontWeight: 400
    property bool fontItalic: false
    property bool fontUnderline: false
    property bool fontUppercase: false
    property color textColor: "#FFFFFFFF"
    property real outlineWidthPercent: 0.0
    property color outlineColor: "#FF000000"
    property bool highlightEnabled: false
    property color highlightColor: "#00000000"
    property bool textEditable: false
    property bool editing: false
    property string preEditText: ""
    signal textCommitRequested(string mediaId, string text)

    pointerEnabled: !root.editing
    doubleClickEnabled: true

    onPrimaryDoubleClicked: function(mediaId, additive) {
        if (!root.textEditable)
            return
        root.selectRequested(mediaId, additive)
        root.preEditText = root.textContent || ""
        root.editing = true
        textEditor.text = root.preEditText
        textEditor.forceActiveFocus()
    }

    Rectangle {
        id: textBackground
        visible: root.highlightEnabled && (root.textContent || "").length > 0 && !root.editing
        color: root.highlightColor
        radius: 2

        readonly property real contentMargin: 4
        readonly property real highlightPadding: 2
        readonly property real contentWidth: Math.max(0, root.width - contentMargin * 2)
        readonly property real contentHeight: Math.max(0, root.height - contentMargin * 2)
        readonly property real paintedW: Math.max(0, Math.min(contentWidth, textNode.paintedWidth))
        readonly property real paintedH: Math.max(0, Math.min(contentHeight, textNode.paintedHeight))

        width: paintedW + highlightPadding * 2
        height: paintedH + highlightPadding * 2

        x: {
            if (root.horizontalAlignment === "left") {
                return contentMargin - highlightPadding
            }
            if (root.horizontalAlignment === "right") {
                return contentMargin + (contentWidth - paintedW) - highlightPadding
            }
            return contentMargin + (contentWidth - paintedW) * 0.5 - highlightPadding
        }

        y: {
            if (root.verticalAlignment === "top") {
                return contentMargin - highlightPadding
            }
            if (root.verticalAlignment === "bottom") {
                return contentMargin + (contentHeight - paintedH) - highlightPadding
            }
            return contentMargin + (contentHeight - paintedH) * 0.5 - highlightPadding
        }
    }

    Text {
        id: textNode
        visible: !root.editing
        anchors.fill: parent
        anchors.margins: 4
        text: root.fontUppercase ? (root.textContent || "").toUpperCase() : root.textContent
        color: root.textColor
        wrapMode: root.fitToTextEnabled ? Text.NoWrap : Text.WordWrap
        horizontalAlignment: root.horizontalAlignment === "left"
            ? Text.AlignLeft
            : (root.horizontalAlignment === "right" ? Text.AlignRight : Text.AlignHCenter)
        verticalAlignment: root.verticalAlignment === "top"
            ? Text.AlignTop
            : (root.verticalAlignment === "bottom" ? Text.AlignBottom : Text.AlignVCenter)
        font.family: root.fontFamily
        font.pixelSize: Math.max(1, root.fontPixelSize)
        font.weight: root.fontWeight
        font.italic: root.fontItalic
        font.underline: root.fontUnderline
        style: root.outlineWidthPercent > 0 ? Text.Outline : Text.Normal
        styleColor: root.outlineColor
        renderType: Text.QtRendering
    }

    Item {
        id: editViewport
        visible: root.editing
        anchors.fill: parent
        anchors.margins: 4
        clip: true

        TextEdit {
            id: textEditor
            // anchors.fill gives TextEdit the full viewport hit area.
            // With only width+y, height = contentHeight only, so click-to-place-cursor
            // fails in empty space and PointerHandlers can steal grabs.
            anchors.fill: parent
            // Vertical alignment via topPadding. contentHeight = raw text height
            // (excludes padding), so there is no binding loop.
            topPadding: {
                var extra = Math.max(0, height - contentHeight)
                if (root.verticalAlignment === "top")    return 0
                if (root.verticalAlignment === "bottom") return extra
                return Math.floor(extra * 0.5)
            }
            text: root.preEditText
            color: root.textColor
            wrapMode: root.fitToTextEnabled ? TextEdit.NoWrap : TextEdit.Wrap
            focus: root.editing
            activeFocusOnPress: true
            cursorVisible: root.editing
            // selectByMouse is unreliable when TextEdit sits inside a scaled viewport
            // because Qt6 passes raw screen coordinates to the internal selection handler
            // instead of item-local coordinates. We use an explicit MouseArea below.
            selectByMouse: false
            horizontalAlignment: root.horizontalAlignment === "left"
                ? Text.AlignLeft
                : (root.horizontalAlignment === "right" ? Text.AlignRight : Text.AlignHCenter)
            font.family: root.fontFamily
            font.pixelSize: Math.max(1, root.fontPixelSize)
            font.weight: root.fontWeight
            font.italic: root.fontItalic
            font.underline: root.fontUnderline

            Keys.onPressed: function(event) {
                var isEnter = (event.key === Qt.Key_Return || event.key === Qt.Key_Enter)
                var hasCommitModifier = (event.modifiers & Qt.ControlModifier) || (event.modifiers & Qt.MetaModifier)
                if (isEnter && hasCommitModifier) {
                    root.textCommitRequested(root.mediaId, textEditor.text)
                    root.editing = false
                    event.accepted = true
                }
            }
        }

        // Explicit mouse handler for cursor placement and drag selection.
        // Required because TextEdit.selectByMouse is unreliable when the item lives
        // inside a scaled/transformed viewport (Qt6 coordinate mapping issue).
        MouseArea {
            id: textEditMouseArea
            anchors.fill: parent
            acceptedButtons: Qt.LeftButton
            // Track the anchor position when a drag-select begins.
            property int pressCharPos: 0

            onPressed: function(mouse) {
                textEditor.forceActiveFocus()
                var pos = textEditor.positionAt(mouse.x, mouse.y)
                pressCharPos = pos
                textEditor.cursorPosition = pos
                mouse.accepted = true
            }

            onPositionChanged: function(mouse) {
                if (pressed) {
                    var pos = textEditor.positionAt(mouse.x, mouse.y)
                    textEditor.select(pressCharPos, pos)
                }
            }
        }
    }

}
