import QtQuick
import Mouffette.Canvas

BaseMediaItem {
    id: root
    property var residentFrameSource: null
    property bool residencyReady: false
    property bool requireInitialSkeleton: true
    contentReady: root.residencyReady && image.hasFrame
    initialFramePresented: !requireInitialSkeleton || surface.firstFramePresented

    MediaSurface {
        id: surface
        anchors.fill: parent
        requireInitialSkeleton: root.requireInitialSkeleton
        contentReady: root.residencyReady && image.hasFrame
        RemoteVideoFrameItem {
            id: image
            anchors.fill: parent
            frameSource: root.residentFrameSource
        }
    }
}
