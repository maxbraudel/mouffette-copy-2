import QtQuick 2.15
import Mouffette.Canvas 1.0

BaseMediaItem {
    id: root
    property string imageSource: ""
    property bool handoffCovered: false
    property var handoffFrameSource: null
    readonly property bool handoffContentReady: image.status === Image.Ready
    contentReady: image.status === Image.Ready

    MediaSurface {
        anchors.fill: parent
        contentReady: handoffPoster.hasFrame || image.status === Image.Ready
        revealImmediately: root.handoffCovered
        fadeDuration: 80

        // The exact drag pixels become part of the final item during handoff.
        // This closes the scene-graph gap between Image.Ready and the first
        // texture presentation; the full-resolution Image simply covers it.
        RemoteVideoFrameItem {
            id: handoffPoster
            anchors.fill: parent
            z: 0
            frameSource: root.handoffFrameSource
        }

        Image {
            id: image
            anchors.fill: parent
            z: 1
            source: root.imageSource
            fillMode: Image.Stretch
            smooth: true
            asynchronous: true
            // mipmap is beneficial for downscaling only; at high zoom (upscaling) it wastes
            // GPU memory on a full mip chain and can cause allocation failures → black render.
            mipmap: false

            onStatusChanged: {
                if (status === Image.Error) {
                    console.warn("[QuickCanvas][ImageItem] load failed",
                                 "mediaId=", root.mediaId,
                                 "source=", root.imageSource)
                }
            }
        }
    }
}
