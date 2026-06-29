#version 450
// =========================================================================
//  highlight_composite.frag — composites the soft OUTER halo over SceneColor.
//
//  Samples the BLURRED mask (uBlur, the H+V Gaussian of the silhouette) and the
//  SHARP mask (uMask). The halo is the blurred field with the interior cut out:
//      outer = clamp(blurred - inside, 0, 1)
//  so the halo appears only OUTSIDE the silhouette and the object's own surface
//  stays untouched (UE-style highlight outline). Alpha-blended onto SceneColor.
//
//  Reuses the fullscreen tonemap.vert triangle (vUV at location 0). Pipeline has
//  alpha blending ENABLED (SRC_ALPHA / ONE_MINUS_SRC_ALPHA).
// =========================================================================
layout(set = 1, binding = 0) uniform sampler2D uBlur;   // blurred mask (H+V)
layout(set = 1, binding = 1) uniform sampler2D uMask;   // sharp mask (silhouette)

layout(location = 0) in  vec2 vUV;
layout(location = 0) out vec4 outColor;

// offset 0/4 = shared scene/view index; halo params start at offset 8.
layout(push_constant) uniform PC {
    uint  scene_index;   // 0
    uint  view_index;    // 4
    float color_r;       // 8
    float color_g;       // 12
    float color_b;       // 16
    float intensity;     // 20 — scales the halo alpha (clamped to 1)
} pc;

void main()
{
    float blurred = texture(uBlur, vUV).r;
    float inside  = texture(uMask, vUV).r;

    float outer = clamp(blurred - inside, 0.0, 1.0);   // halo OUTSIDE the silhouette only
    float a     = clamp(outer * pc.intensity, 0.0, 1.0);
    if (a <= 0.0)
        discard;

    outColor = vec4(pc.color_r, pc.color_g, pc.color_b, a);
}
