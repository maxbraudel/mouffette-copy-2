import QtQuick 2.15

BaseMediaItem {
    id: root
    property string imageSource: ""

    Rectangle {
        anchors.fill: parent
        color: "#141a24"
        visible: image.status !== Image.Ready
    }

    Image {
        id: image
        anchors.fill: parent
        source: root.imageSource
        fillMode: Image.PreserveAspectFit
        smooth: true
        asynchronous: true
        // mipmap is beneficial for downscaling only; at high zoom (upscaling) it wastes
        // GPU memory on a full mip chain and can cause allocation failures → black render.
        mipmap: false
    }
}
