#ifndef LUX_TRANSITION_DITHER_GLSL
#define LUX_TRANSITION_DITHER_GLSL

// Stable object seed plus pixel coordinate gives every representation the
// same coverage decision in Depth/GBuffer/Shadow/Picking. The integer hash is
// deliberately frame-independent; only coverage changes with scene time.
float luxTransitionThreshold(uvec2 pixel, uint seed) {
    uint value = pixel.x * 0x8da6b343u;
    value ^= pixel.y * 0xd8163841u;
    value ^= seed * 0xcb1ab31fu;
    value ^= value >> 16u;
    value *= 0x7feb352du;
    value ^= value >> 15u;
    value *= 0x846ca68bu;
    value ^= value >> 16u;
    return float(value & 0x00ffffffu) / float(0x01000000u);
}

void luxApplyTransitionCoverage(float coverage, uint seed) {
    if (coverage <= 0.0 ||
        (coverage < 1.0 && luxTransitionThreshold(
            uvec2(gl_FragCoord.xy), seed) >= coverage))
    {
        discard;
    }
}

// Parent/child cross-fades use the same stable seed. A fading-in child keeps
// threshold < coverage; its fading-out parent keeps the complementary set
// threshold >= 1-coverage. Their union is complete and their intersection is
// empty whenever the paired coverages sum to one.
void luxApplyDirectedTransitionCoverage(
    float coverage,
    uint seed,
    bool fade_out)
{
    float threshold = luxTransitionThreshold(uvec2(gl_FragCoord.xy), seed);
    if (coverage <= 0.0 ||
        (coverage < 1.0 && (fade_out
            ? threshold < 1.0 - coverage
            : threshold >= coverage)))
    {
        discard;
    }
}

#endif
