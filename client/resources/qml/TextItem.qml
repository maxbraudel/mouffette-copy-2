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

    TextEdit {
        id: textEditor
        visible: root.editing
        anchors.fill: parent
        anchors.margins: 4
        text: root.preEditText
        color: root.textColor
        wrapMode: root.fitToTextEnabled ? TextEdit.NoWrap : TextEdit.Wrap
        horizontalAlignment: root.horizontalAlignment === "left"
            ? Text.AlignLeft
            : (root.horizontalAlignment === "right" ? Text.AlignRight : Text.AlignHCenter)
        font.family: root.fontFamily
        font.pixelSize: Math.max(1, root.fontPixelSize)
        font.weight: root.fontWeight
        font.italic: root.fontItalic
        font.underline: root.fontUnderline
        selectByMouse: true

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

    Rectangle {
        anchors.fill: parent
        color: "transparent"
        border.width: root.editing ? 1 : 0
        border.color: "#66FFFFFF"
        visible: root.editing
    }
}
