#version 450
/// @file shadow_evsm_caster.frag
/// @brief EVSM (Exponential Variance Shadow Maps) caster — writes the four
///        warped depth moments into an RGBA16F atlas tile.
///
/// Sibling of shadow_depth.frag: same vertex shader (mesh_shadow_vp.vert)
/// produces post-projection gl_FragCoord.z; this frag rewrites that depth
/// through Lauritzen's dual exponential warp and squares the result to
/// build the first two moments of each warp. The compute blur pass that
/// follows (shadow_evsm_blur_h/v.comp) Gaussian-filters these moments
/// across the tile — pre-filtered moments are what makes EVSM bias-free
/// at sampling time. See .internal/plan/evsm-shadow-implementation-guide.md
/// §2.4 for the math.
///
/// Push-constant exponents (kEVSMPosExponent / kEVSMNegExponent) tune the
/// warp aggressiveness. Frostbite-style defaults: pos=40, neg=5.

// FP16 atlas (RGBA16F) max value is ~65504. exp²(c·z) must stay safely below
// it, so c·z ≤ ½·ln(65504)/ln(e) ≈ 5.5 at depth=1. We use 5.0 for ~30% safety
// margin against fp16 rounding at the high end. RGBA32F atlas can push these
// to Frostbite's 40/5; both constants must match `shadow_evsm.glsl::warpDepth`.
const float kEVSMPosExponent = 5.0;
const float kEVSMNegExponent = 5.0;

layout(location = 0) in vec2 vUV;        // unused but kept to share vertex shader
// Depth-linearization params forwarded from mesh_shadow_vp.vert (flat — constant
// per slice). Perspective (point/spot) slices must warp a LINEAR depth or the
// fp16 moments lose precision at distance; ortho (directional) slices pass
// depth_is_perspective = 0 and use NDC z directly.
layout(location = 1) flat in float vShadowNear;
layout(location = 2) flat in float vShadowFar;
layout(location = 3) flat in uint  vDepthPersp;
layout(location = 0) out vec4 out_moments;

void main()
{
    // gl_FragCoord.z is the post-projection NDC depth in [0,1].
    float depth = gl_FragCoord.z;

    // Perspective slices: recover view-space distance from the hyperbolic NDC z
    // and renormalize to a LINEAR [0,1] metric. This keeps the warped moments
    // well-distributed across the depth range so the variance survives fp16.
    // MUST match the sampler's linearization in shadow_evsm.glsl exactly.
    if (vDepthPersp != 0u)
    {
        float vz = (vShadowFar * vShadowNear)
                 / (vShadowFar - depth * (vShadowFar - vShadowNear));
        depth = (vz - vShadowNear) / max(vShadowFar - vShadowNear, 1e-6);
    }
    depth = clamp(depth, 0.0, 1.0);

    // Dual exponential warp: positive expands the high end, negative the low.
    // Their product is bounded so that the four moments fit RGBA16F without
    // overflow for sensible exponent ranges (kPos <= 42, kNeg <= 8 on Frostbite).
    float pos =  exp( kEVSMPosExponent * depth);
    float neg = -exp(-kEVSMNegExponent * depth);

    out_moments = vec4(pos, pos * pos, neg, neg * neg);
}
