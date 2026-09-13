import QtQuick
// The one visual delegate shared by the interactive authoring canvas and the
// passive remote renderer. Geometry, interaction and screen clipping remain
// responsibilities of the respective parent roots.
Item {
    id: root

    property var media: null
    property bool selected: false
    property bool textEditable: false
    property QtObject editingSession: null
    property bool handoffCovered: false
    property var handoffFrameSource: null

    readonly property var visualItem: visualLoader.item
    // Compatibility alias for the former Loader-based local delegate.
    readonly property var item: visualItem
    readonly property bool editing: !!(visualItem && visualItem.editing === true)
    readonly property bool contentReady: !!(visualItem && visualItem.contentReady === true)
    readonly property bool handoffContentReady: !!visualItem
                                                && visualItem.handoffContentReady === true

    signal primaryPressed(string mediaId, bool additive)
    signal textCommitRequested(string mediaId, string text)
    signal textLiveUpdateRequested(string mediaId, string text)

    function fireDoubleClick(additive) {
        if (visualItem && typeof visualItem.fireDoubleClick === "function")
            visualItem.fireDoubleClick(!!additive)
    }

    Loader {
        id: visualLoader
        anchors.fill: parent
        sourceComponent: {
            if (!root.media) return null
            if (root.media.mediaType === "video") return videoDelegate
            if (root.media.mediaType === "text") return textDelegate
            return imageDelegate
        }
    }

    Connections {
        target: root.visualItem
        ignoreUnknownSignals: true
        function onPrimaryPressed(mediaId, additive) {
            root.primaryPressed(mediaId, additive)
        }
        function onTextCommitRequested(mediaId, text) {
            root.textCommitRequested(mediaId, text)
        }
        function onTextLiveUpdateRequested(mediaId, text) {
            root.textLiveUpdateRequested(mediaId, text)
        }
    }

    Component {
        id: imageDelegate
        ImageItem {
            mediaId: root.media ? (root.media.mediaId || "") : ""
            mediaX: 0
            mediaY: 0
            mediaWidth: root.width
            mediaHeight: root.height
            mediaScale: 1.0
            mediaZ: 0
            selected: root.selected
            handoffCovered: root.handoffCovered
            handoffFrameSource: root.handoffFrameSource
            imageSource: root.media
                         ? (root.media.sourceUrl || root.media.sourcePath || "")
                         : ""
        }
    }

    Component {
        id: videoDelegate
        VideoItem {
            mediaId: root.media ? (root.media.mediaId || "") : ""
            mediaX: 0
            mediaY: 0
            mediaWidth: root.width
            mediaHeight: root.height
            mediaScale: 1.0
            mediaZ: 0
            selected: root.selected
            handoffCovered: root.handoffCovered
            handoffFrameSource: root.handoffFrameSource
            cppMediaPlayer: root.media ? (root.media.videoPlayerPtr || null) : null
            cppVideoSink: root.media ? (root.media.videoSinkPtr || null) : null
            remoteFrameSource: root.media ? (root.media.remoteFrameSource || null) : null
            videoPlaybackErrorCode: root.media ? (root.media.videoPlaybackErrorCode || 0) : 0
            videoPlaybackErrorString: root.media ? (root.media.videoPlaybackErrorString || "") : ""
            videoHasRenderedFrame: !!(root.media && root.media.videoHasRenderedFrame)
            videoHasPosterFrame: !!(root.media && root.media.videoHasPosterFrame)
            videoFirstFramePrimed: !!(root.media && root.media.videoFirstFramePrimed)
        }
    }

    Component {
        id: textDelegate
        TextItem {
            mediaId: root.media ? (root.media.mediaId || "") : ""
            mediaX: 0
            mediaY: 0
            mediaWidth: root.width
            mediaHeight: root.height
            mediaScale: 1.0
            mediaZ: 0
            selected: root.selected
            editingSession: root.editingSession
            textContent: root.media ? (root.media.textContent || "") : ""
            horizontalAlignment: root.media ? (root.media.textHorizontalAlignment || "center") : "center"
            verticalAlignment: root.media ? (root.media.textVerticalAlignment || "center") : "center"
            fitToTextEnabled: !!(root.media && root.media.fitToTextEnabled)
            fontFamily: root.media ? (root.media.textFontFamily || "Impact") : "Impact"
            fontPixelSize: Math.max(1, root.media ? (root.media.textFontPixelSize || 22) : 22)
            fontWeight: root.media ? (root.media.textFontWeight || 400) : 400
            fontItalic: !!(root.media && root.media.textItalic)
            fontUnderline: !!(root.media && root.media.textUnderline)
            fontUppercase: !!(root.media && root.media.textUppercase)
            textColor: root.media ? (root.media.textColor || "#FFFFFFFF") : "#FFFFFFFF"
            outlineWidthPercent: root.media ? (root.media.textOutlineWidthPercent || 0.0) : 0.0
            outlineWidthPx: root.media && root.media.textOutlineWidthPx !== undefined
                            ? root.media.textOutlineWidthPx : -1.0
            outlineColor: root.media ? (root.media.textOutlineColor || "#FF000000") : "#FF000000"
            highlightEnabled: !!(root.media && root.media.textHighlightEnabled)
            highlightColor: root.media ? (root.media.textHighlightColor || "#00000000") : "#00000000"
            textEditable: root.textEditable
        }
    }
}
