import QtQuick 2.15

Item {
    id: mediaLayer
    property var mediaModel: []
    property Item contentItem: null
    property var sourceUrlResolver: function(path) { return path }

    signal mediaSelectRequested(string mediaId, bool additive)
    signal mediaMoveEnded(string mediaId, real sceneX, real sceneY)
    signal textCommitRequested(string mediaId, string text)

    Repeater {
        model: mediaLayer.mediaModel
        delegate: Item {
            id: mediaDelegate
            property var media: modelData
            width: media ? Math.max(1, media.width) : 1
            height: media ? Math.max(1, media.height) : 1
            x: media ? media.x : 0
            y: media ? media.y : 0
            scale: media ? (media.scale || 1.0) : 1.0
            z: media ? media.z : 0
            visible: !!media

            Loader {
                id: mediaContentLoader
                property var media: mediaDelegate.media
                sourceComponent: {
                    if (!mediaDelegate.media) return null
                    if (mediaDelegate.media.mediaType === "video") return videoDelegate
                    if (mediaDelegate.media.mediaType === "text") return textDelegate
                    return imageDelegate
                }
            }
        }
    }

    Component {
        id: imageDelegate
        ImageItem {
            mediaId: parent.media ? (parent.media.mediaId || "") : ""
            mediaX: 0
            mediaY: 0
            mediaWidth: parent.media ? parent.media.width : 0
            mediaHeight: parent.media ? parent.media.height : 0
            mediaScale: 1.0
            mediaZ: parent.media ? parent.media.z : 0
            selected: !!(parent.media && parent.media.selected)
            imageSource: mediaLayer.sourceUrlResolver(parent.media ? parent.media.sourcePath : "")
            onSelectRequested: function(mediaId, additive) {
                mediaLayer.mediaSelectRequested(mediaId, additive)
            }
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
            mediaZ: parent.media ? parent.media.z : 0
            selected: !!(parent.media && parent.media.selected)
            source: mediaLayer.sourceUrlResolver(parent.media ? parent.media.sourcePath : "")
            onSelectRequested: function(mediaId, additive) {
                mediaLayer.mediaSelectRequested(mediaId, additive)
            }
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
            mediaZ: parent.media ? parent.media.z : 0
            selected: !!(parent.media && parent.media.selected)
            textContent: parent.media ? (parent.media.textContent || "") : ""
            textEditable: !!(parent.media && parent.media.textEditable)
            onSelectRequested: function(mediaId, additive) {
                mediaLayer.mediaSelectRequested(mediaId, additive)
            }
            onTextCommitRequested: function(mediaId, text) {
                mediaLayer.textCommitRequested(mediaId, text)
            }
        }
    }
}
