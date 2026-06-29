#version 450

// =============================================================================
// Point Splatting fragment shader
//
// Replaces the hard-circle clip of pointcloud_simple.frag with a Gaussian
// soft falloff.  When combined with alpha blending in the graphics pipeline,
// neighbouring splats merge smoothly — filling point-cloud gaps and giving
// the appearance of a continuous surface.
//
// Blend state expected by PCFeatureSplatting:
//   srcColorBlend = SRC_ALPHA
//   dstColorBlend = ONE_MINUS_SRC_ALPHA
//   depthWrite    = FALSE      (splats are semi-transparent, don't occlude peers)
//   depthTest     = TRUE       (still respect opaque geometry depth)
// =============================================================================

layout(location = 0) in vec4 vColor;
layout(location = 0) out vec4 outColor;

void main()
{
    // Map gl_PointCoord from [0,1]^2 to [-0.5, 0.5]^2.
    vec2 coord    = gl_PointCoord - vec2(0.5);
    float dist_sq = dot(coord, coord);

    // Gaussian falloff: e^(-dist_sq / (2 * sigma^2))
    // sigma = 0.25 → exponent coefficient = 1/(2*0.0625) = 8
    // At the point edge (dist_sq = 0.25, i.e. radius = 0.5) alpha ≈ 0.135.
    float alpha = exp(-dist_sq * 8.0);

    // Discard nearly-invisible fragments to avoid depth-buffer pollution.
    if (alpha < 0.01)
        discard;

    outColor = vec4(vColor.rgb, vColor.a * alpha);
}
