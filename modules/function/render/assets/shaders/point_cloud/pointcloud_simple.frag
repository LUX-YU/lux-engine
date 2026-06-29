#version 450

// =============================================================================
// Simple point cloud fragment shader
//
// Outputs vertex color directly. No texture sampling, no material lookup.
// Used with pointcloud_simple.vert for GpuPointVertex-based rendering.
// =============================================================================

layout(location = 0) in vec4 vColor;
layout(location = 0) out vec4 outColor;

void main()
{
    // Optional: circular point shape (discard pixels outside circle radius)
    vec2 coord = gl_PointCoord - vec2(0.5);
    if (dot(coord, coord) > 0.25)
        discard;

    outColor = vColor;
}
