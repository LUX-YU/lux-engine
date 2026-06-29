#version 450
// =============================================================================
// particle.frag — GPU particle fragment shader
//
// Renders a soft circle billboard with radial alpha falloff.
// The pass uses additive blending (Src ONE, Dst ONE) for a glowing fire look.
// =============================================================================

layout(location = 0) in vec4  vColor;
layout(location = 1) in vec2  vBillboardUV;    // -1..1 in billboard space
layout(location = 2) in float vLifetimeFrac;   // 1 = just born, 0 = about to die

layout(location = 0) out vec4 outColor;

void main()
{
    // Radial distance from billboard centre (0 = centre, 1 = edge).
    float dist = length(vBillboardUV);

    // Discard outside the circle to keep clean edges.
    if (dist > 1.0) discard;

    // Smooth circular falloff: bright centre, transparent edge.
    float radialAlpha = 1.0 - smoothstep(0.0, 1.0, dist);

    // Fade out near death (last 20% of lifetime).
    float lifetimeAlpha = smoothstep(0.0, 0.2, vLifetimeFrac);

    float alpha = radialAlpha * lifetimeAlpha * vColor.a;

    // For additive blending the RGB is pre-multiplied with alpha here.
    outColor = vec4(vColor.rgb * alpha, alpha);
}
