#version 450

// =============================================================================
// Simple point cloud vertex shader
//
// Vertex format: GpuPointVertex (16 bytes)
//   - vec3 position  (12 bytes, location 0)
//   - uint packed_attr (4 bytes, location 1)  RGBA packed: R[7:0] G[15:8] B[23:16] I[31:24]
//
// Uses the same ViewGpuData layout as fr.vert (set 0, binding 1, ViewBuffer SSBO)
// so the existing scene descriptor set can be reused without a separate layout.
// =============================================================================

// ========== Vertex Input (GpuPointVertex, 16 bytes) ==========
layout(location = 0) in vec3 inPosition;    // World-space point position
layout(location = 1) in uint inPackedAttr;  // Packed RGBA color + intensity

// ========== SET 0: View (std140) ==========
struct ViewGpuData {
    mat4 view;
    mat4 proj;
    mat4 inv_view;
    mat4 inv_proj;
    vec4 cam_pos;
    vec4 viewport;
};
layout(set = 0, binding = 1, std430) readonly buffer ViewBuffer {
    ViewGpuData views[];
} uViews;

// ========== Push Constant ==========
layout(push_constant) uniform PC {
    uint  scene_index;    ///< SceneGlobalBuffer slot
    uint  view_index;      ///< ViewBuffer slot
    float point_size;   // Point size in pixels
} uPC;

// ========== Output to fragment shader ==========
layout(location = 0) out vec4 vColor;

void main()
{
    // Transform world-space position to clip space
    gl_Position = uViews.views[uPC.view_index].proj * uViews.views[uPC.view_index].view * vec4(inPosition, 1.0);

    // Unpack RGBA from packed attribute (matches GpuPointVertex::pack())
    float r = float((inPackedAttr >>  0u) & 0xFFu) / 255.0;
    float g = float((inPackedAttr >>  8u) & 0xFFu) / 255.0;
    float b = float((inPackedAttr >> 16u) & 0xFFu) / 255.0;

    vColor = vec4(r, g, b, 1.0);

    // Set point size from push constant
    gl_PointSize = uPC.point_size;
}
