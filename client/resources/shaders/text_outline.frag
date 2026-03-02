#version 440

layout(location = 0) in vec2 qt_TexCoord0;
layout(location = 0) out vec4 fragColor;

layout(std140, binding = 0) uniform buf {
    mat4 qt_Matrix;
    float qt_Opacity;
    float outlineR;
    float outlineG;
    float outlineB;
    float outlineA;
    float fillR;
    float fillG;
    float fillB;
    float fillA;
    float texelX;
    float texelY;
    float outlineRadius;
};

layout(binding = 1) uniform sampler2D source;

void main() {
    float glyphAlpha = texture(source, qt_TexCoord0).a;

    if (outlineRadius < 0.5 || texelX < 0.00001 || texelY < 0.00001) {
        fragColor = vec4(fillR, fillG, fillB, fillA * glyphAlpha) * qt_Opacity;
        return;
    }

    float radius = clamp(outlineRadius, 1.0, 24.0);
    float searchRadius = radius + 1.5;
    float searchRadius2 = searchRadius * searchRadius;

    float nearestDist = 1e9;
    const int MAX_RADIUS = 24;
    for (int oy = -MAX_RADIUS; oy <= MAX_RADIUS; ++oy) {
        for (int ox = -MAX_RADIUS; ox <= MAX_RADIUS; ++ox) {
            float d2 = float(ox * ox + oy * oy);
            if (d2 > searchRadius2)
                continue;

            vec2 offset = vec2(float(ox) * texelX,
                               float(oy) * texelY);
            float a = texture(source, qt_TexCoord0 + offset).a;
            if (a <= 0.001)
                continue;

            // Sub-pixel refine: stronger alpha means the true edge is likely
            // slightly closer than the texel center.
            float d = sqrt(d2) - a * 0.5;
            nearestDist = min(nearestDist, d);
        }
    }

    // Convert nearest distance to smooth outline coverage.
    // coverage=1 well inside border band, 0 outside; smooth around the outer edge.
    float edgeSoftness = 0.9;
    float outlineCoverage = 0.0;
    if (nearestDist < 1e8) {
        outlineCoverage = clamp((radius + edgeSoftness - nearestDist) / (2.0 * edgeSoftness), 0.0, 1.0);
    }

    float outlineMask = outlineCoverage * (1.0 - glyphAlpha);

    vec4 outlinePixel = vec4(outlineR, outlineG, outlineB, outlineA * outlineMask);
    vec4 glyphPixel = vec4(fillR, fillG, fillB, fillA * glyphAlpha);

    float outA = glyphPixel.a + outlinePixel.a * (1.0 - glyphPixel.a);
    vec3 outRGB = outA > 0.0
        ? (glyphPixel.rgb * glyphPixel.a + outlinePixel.rgb * outlinePixel.a * (1.0 - glyphPixel.a)) / outA
        : vec3(0.0);

    fragColor = vec4(outRGB, outA) * qt_Opacity;
}
