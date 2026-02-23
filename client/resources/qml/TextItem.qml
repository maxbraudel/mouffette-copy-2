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
        root.selectRequested(mediaId, additive)
        root.preEditText = root.textContent || ""
        root.editing = true
        textEditor.text = root.preEditText
        textEditor.forceActiveFocus()
    }

    Rectangle {
        id: textBackground
        anchors.fill: parent
        color: root.highlightEnabled ? root.highlightColor : "transparent"
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

        Keys.onReturnPressed: function(event) {
            root.textCommitRequested(root.mediaId, textEditor.text)
            root.editing = false
            event.accepted = true
        }

        Keys.onEnterPressed: function(event) {
            root.textCommitRequested(root.mediaId, textEditor.text)
            root.editing = false
            event.accepted = true
        }

        Keys.onEscapePressed: function(event) {
            textEditor.text = root.preEditText
            root.editing = false
            event.accepted = true
        }

        onActiveFocusChanged: {
            if (!activeFocus && root.editing) {
                root.textCommitRequested(root.mediaId, textEditor.text)
                root.editing = false
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
