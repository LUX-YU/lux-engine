#version 450
// =========================================================================
//  tonemap.frag — HDR → SDR tone mapping
//
//  Reads the HDR color texture and applies tone mapping + gamma correction.
//
//  Descriptor sets:
//    Set 0: Scene (unused in this shader, but pipeline expects it)
//    Set 1: HDR color input (binding 0: sampler2D)
//
//  Push constants (20 bytes total):
//    [0, 8)   — scene_index + view_index (shared, set by framework)
//    [8, 20)  — exposure, gamma, operator_id (per-feature)
// =========================================================================

layout(location = 0) in vec2 vUV;
layout(location = 0) out vec4 outColor;

layout(set = 1, binding = 0) uniform sampler2D uHDRColor;

layout(push_constant) uniform PC {
    uint  scene_index;
    uint  view_index;
    float exposure;
    float gamma;
    uint  operator_id;  // 0=Reinhard, 1=ACES, 2=Uncharted2, 3=None
} uPC;

// ========== Tone map operators ==========

// Reinhard
vec3 toneMapReinhard(vec3 hdr) {
    return hdr / (hdr + vec3(1.0));
}

// ACES Filmic (Krzysztof Narkowicz fit)
vec3 toneMapACES(vec3 x) {
    const float a = 2.51;
    const float b = 0.03;
    const float c = 2.43;
    const float d = 0.59;
    const float e = 0.14;
    return clamp((x * (a * x + b)) / (x * (c * x + d) + e), 0.0, 1.0);
}

// Uncharted 2 (John Hable)
vec3 uncharted2Helper(vec3 x) {
    const float A = 0.15, B = 0.50, C = 0.10, D = 0.20, E = 0.02, F = 0.30;
    return ((x * (A * x + C * B) + D * E) / (x * (A * x + B) + D * F)) - E / F;
}

vec3 toneMapUncharted2(vec3 hdr) {
    const float W = 11.2;
    vec3 curr = uncharted2Helper(hdr);
    vec3 white_scale = vec3(1.0) / uncharted2Helper(vec3(W));
    return curr * white_scale;
}

// ========== Main ==========

void main()
{
    vec3 hdr = texture(uHDRColor, vUV).rgb;

    // Apply exposure
    vec3 exposed = hdr * uPC.exposure;

    // Apply tone mapping operator
    vec3 mapped;
    if (uPC.operator_id == 0u)
        mapped = toneMapReinhard(exposed);
    else if (uPC.operator_id == 1u)
        mapped = toneMapACES(exposed);
    else if (uPC.operator_id == 2u)
        mapped = toneMapUncharted2(exposed);
    else
        mapped = exposed;

    // Output is written to an sRGB framebuffer (VK_FORMAT_B8G8R8A8_SRGB),
    // so hardware applies the sRGB transfer function automatically.
    // No manual pow(1/gamma) needed — output linear values directly.
    outColor = vec4(mapped, 1.0);
}
