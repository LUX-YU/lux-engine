#version 450
/// @file shadow_depth.frag
/// @brief Shadow pass fragment shader — writes depth only.
///
/// For opaque objects this is a no-op (depth is written by rasterization).
/// For alpha-cutout objects, the UV input can be used to sample albedo
/// and discard fragments below the cutoff (future extension).

layout(location = 0) in vec2 vUV;

void main()
{
    // Depth is written automatically by the rasterizer.
    // Alpha-cutout support can be added here if needed:
    //   float alpha = texture(uAlbedo, vUV).a;
    //   if (alpha < 0.5) discard;
}
