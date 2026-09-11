import QtQuick 2.15

BaseMediaItem {
    id: root
    property string imageSource: ""
    contentReady: image.status === Image.Ready

    MediaSurface {
        anchors.fill: parent
        contentReady: image.status === Image.Ready
        fadeDuration: 80

        Image {
            id: image
            anchors.fill: parent
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
