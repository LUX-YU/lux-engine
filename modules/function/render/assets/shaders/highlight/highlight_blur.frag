#version 450
// =========================================================================
//  highlight_blur.frag — separable Gaussian blur of the R8 highlight mask.
//
//  Run TWICE: horizontally (mask -> blurH) then vertically (blurH -> blurV),
//  driven by the push-constant direction. The blurred mask is the raw material
//  for the outer halo: highlight_composite.frag subtracts the SHARP mask from
//  this blurred field so the halo lands only OUTSIDE the silhouette.
//
//  9-tap (centre + 4 symmetric pairs) normalised Gaussian; the per-tap texel
//  step is textureSize-derived (resolution-independent) and scaled by `radius`.
//  Reuses the fullscreen tonemap.vert triangle (vUV at location 0).
// =========================================================================
layout(set = 1, binding = 0) uniform sampler2D uTex;

layout(location = 0) in  vec2 vUV;
layout(location = 0) out float outVal;

// offset 0/4 = shared scene/view index (pushed by the framework); the blur
// params start at offset 8 (matches the TonemapFeature push-constant convention).
layout(push_constant) uniform PC {
    uint  scene_index;   // 0
    uint  view_index;    // 4
    float dir_x;         // 8  — blur axis (1,0) for H, (0,1) for V
    float dir_y;         // 12
    float radius;        // 16 — per-tap step multiplier (in texels)
} pc;

// Normalised Gaussian weights (sigma ~= 2): centre + 4 pairs sum to 1.0.
const float W[5] = float[](0.204164, 0.180174, 0.123832, 0.066282, 0.027631);

void main()
{
    vec2 texel = 1.0 / vec2(textureSize(uTex, 0));
    vec2 step  = vec2(pc.dir_x, pc.dir_y) * texel * pc.radius;

    float sum = texture(uTex, vUV).r * W[0];
    for (int i = 1; i < 5; ++i)
    {
        float fi = float(i);
        sum += texture(uTex, vUV + step * fi).r * W[i];
        sum += texture(uTex, vUV - step * fi).r * W[i];
    }
    outVal = sum;
}
