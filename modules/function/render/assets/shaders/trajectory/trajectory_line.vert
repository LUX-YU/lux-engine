#version 450

// =============================================================================
// Trajectory Line vertex shader
//
// Vertex format: GpuTrajectoryVertex (24 bytes)
//   - vec3 position     (12 bytes, location 0)
//   - uint packed_color  (4 bytes,  location 1) RGBA packed: R[7:0] G[15:8] B[23:16] A[31:24]
//   - float time         (4 bytes,  location 2) Normalized arc-length [0,1]
//   - float width        (4 bytes,  location 3) Per-vertex width (unused in line mode)
//
// Uses the same ViewGpuData layout as pointcloud_simple.vert
// =============================================================================

// ========== Vertex Input (GpuTrajectoryVertex, 24 bytes) ==========
layout(location = 0) in vec3  inPosition;
layout(location = 1) in uint  inPackedColor;
layout(location = 2) in float inTime;
layout(location = 3) in float inWidth;

// ========== SET 0: View (std430) ==========
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
    uint scene_index;   ///< SceneGlobalBuffer slot
    uint view_index;    ///< ViewBuffer slot
} uPC;

// ========== Output to fragment shader ==========
layout(location = 0) out vec4  vColor;
layout(location = 1) out float vTime;

void main()
{
    mat4 viewMat = uViews.views[uPC.view_index].view;
    mat4 projMat = uViews.views[uPC.view_index].proj;

    gl_Position = projMat * viewMat * vec4(inPosition, 1.0);

    // Unpack RGBA (matches GpuTrajectoryVertex::packColor())
    float r = float((inPackedColor >>  0u) & 0xFFu) / 255.0;
    float g = float((inPackedColor >>  8u) & 0xFFu) / 255.0;
    float b = float((inPackedColor >> 16u) & 0xFFu) / 255.0;
    float a = float((inPackedColor >> 24u) & 0xFFu) / 255.0;

    vColor = vec4(r, g, b, a);
    vTime  = inTime;
}
