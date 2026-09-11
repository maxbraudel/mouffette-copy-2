import QtQuick 2.15

// One editing owner per interactive canvas. Selection remains owned by the
// backend; this object consumes its projection and never changes selection.
QtObject {
    id: session
    property Item _activeEditor: null
    readonly property Item activeEditor: _activeEditor
    property var selectionModel: []

    function isSelected(mediaId) {
        for (var i = 0; i < selectionModel.length; ++i) {
            if (selectionModel[i] && selectionModel[i].mediaId === mediaId)
                return true
        }
        return false
    }

    function begin(editor) {
        if (!editor || !editor.textEditable || !isSelected(editor.mediaId))
            return false
        if (activeEditor === editor)
            return true
        finish()
        // A synchronous commit listener can change selection or open a newer
        // editor. Never overwrite that decision after returning from finish().
        if (activeEditor || !editor || !editor.textEditable || !isSelected(editor.mediaId))
            return false
        _activeEditor = editor
        return true
    }

    function finish(expectedEditor) {
        var editor = activeEditor
        if (!editor || (expectedEditor && expectedEditor !== editor))
            return false
        var mediaId = editor.mediaId
        var text = editor.currentText()
        // Snapshot first, release ownership before emitting: backend publication
        // can re-enter this object. A late commit cannot revive an old editor.
        _activeEditor = null
        editor.textCommitRequested(mediaId, text)
        return true
    }

    function abandon(editor) {
        // A destroyed/replaced visual has no document left to commit. In
        // particular it must never clear a newer editor's ownership.
        if (activeEditor === editor)
            _activeEditor = null
    }

    onSelectionModelChanged: {
        if (activeEditor && !isSelected(activeEditor.mediaId))
            finish()
    }
}
