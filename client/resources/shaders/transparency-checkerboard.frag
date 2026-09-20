#version 440

layout(location = 0) in vec2 qt_TexCoord0;
layout(location = 0) out vec4 fragColor;

layout(std140, binding = 0) uniform buf {
    mat4 qt_Matrix;
    float qt_Opacity;
    vec4 viewportRect;
    vec4 colorA;
    vec4 colorB;
    float cellSize;
};

void main()
{
    // Coordinates stay bounded by the viewport, including for enormous media.
    vec2 position = viewportRect.xy + qt_TexCoord0 * viewportRect.zw;
    vec2 cell = floor(position / max(1.0, cellSize));
    fragColor = mix(colorA, colorB, mod(cell.x + cell.y, 2.0)) * qt_Opacity;
}
