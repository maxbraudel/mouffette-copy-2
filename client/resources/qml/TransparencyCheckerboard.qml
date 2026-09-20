import QtQuick

// One viewport-sized quad at most, with no source texture or offscreen layer.
// The host cancels the media's scale so these dimensions are interface pixels.
ShaderEffect {
    objectName: "mediaTransparencyCheckerboard"

    required property rect viewportRect
    required property color colorA
    required property color colorB
    property real cellSize: 8

    width: Math.max(0, viewportRect.width)
    height: Math.max(0, viewportRect.height)
    visible: width > 0 && height > 0
    fragmentShader: "qrc:/shaders/transparency-checkerboard.frag.qsb"
}
