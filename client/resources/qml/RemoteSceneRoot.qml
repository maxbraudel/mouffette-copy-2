import QtQuick 2.15

Rectangle {
    id: root
    color: "transparent"
    clip: true

    property var mediaListModel: null
    signal spanReady(string mediaId, string spanId)

    Repeater {
        model: root.mediaListModel

        delegate: Item {
            id: spanDelegate
            property var media: modelData
            property bool readyReported: false

            x: media ? media.destX : 0
            y: media ? media.destY : 0
            width: media ? Math.max(0, media.destWidth) : 0
            height: media ? Math.max(0, media.destHeight) : 0
            z: media ? (media.z || 0) : 0
            visible: !!media && media.contentVisible !== false && media.renderVisible !== false
            opacity: media && media.renderOpacity !== undefined ? media.renderOpacity : 0.0
            clip: true

            readonly property real safeSourceWidth: media ? Math.max(0.000001, media.sourceWidth || 1.0) : 1.0
            readonly property real safeSourceHeight: media ? Math.max(0.000001, media.sourceHeight || 1.0) : 1.0
            readonly property real baseWidth: media ? Math.max(1, media.width || 1) : 1
            readonly property real baseHeight: media ? Math.max(1, media.height || 1) : 1
            readonly property real fullDisplayWidth: width / safeSourceWidth
            readonly property real fullDisplayHeight: height / safeSourceHeight
            readonly property real scaleX: fullDisplayWidth / baseWidth
            readonly property real scaleY: fullDisplayHeight / baseHeight

            Item {
                id: fullMedia
                x: -(spanDelegate.media ? (spanDelegate.media.sourceX || 0) : 0) * spanDelegate.fullDisplayWidth
                y: -(spanDelegate.media ? (spanDelegate.media.sourceY || 0) : 0) * spanDelegate.fullDisplayHeight
                width: spanDelegate.baseWidth
                height: spanDelegate.baseHeight
                transformOrigin: Item.TopLeft
                transform: Scale {
                    origin.x: 0
                    origin.y: 0
                    xScale: spanDelegate.scaleX
                    yScale: spanDelegate.scaleY
                }

                MediaVisual {
                    id: visual
                    anchors.fill: parent
                    media: spanDelegate.media
                    selected: false
                    textEditable: false
                }
            }

            function reportReady() {
                if (readyReported || !media || !visual.contentReady)
                    return
                readyReported = true
                root.spanReady(media.remoteMediaId || media.mediaId || "", media.spanId || "")
            }

            Connections {
                target: visual
                function onContentReadyChanged() { spanDelegate.reportReady() }
            }

            Component.onCompleted: Qt.callLater(reportReady)
        }
    }
}
