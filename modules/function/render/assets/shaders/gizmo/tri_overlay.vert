#version 450

// =============================================================================
// Triangle overlay gizmo vertex shader
//
// Vertex format: GizmoVertex (= GpuPointVertex, 16 bytes)
//   - vec3 position   (12 bytes, location 0)
//   - uint packed_attr (4 bytes,  location 1)  RGBA packed: R[7:0] G[15:8] B[23:16] A[31:24]
// =============================================================================

layout(location = 0) in vec3 inPosition;
layout(location = 1) in uint inPackedAttr;

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

layout(push_constant) uniform PC {
    uint scene_index;
    uint view_index;
} uPC;

layout(location = 0) out vec4 vColor;

void main()
{
    gl_Position = uViews.views[uPC.view_index].proj
                * uViews.views[uPC.view_index].view
                * vec4(inPosition, 1.0);

    float r = float((inPackedAttr >>  0u) & 0xFFu) / 255.0;
    float g = float((inPackedAttr >>  8u) & 0xFFu) / 255.0;
    float b = float((inPackedAttr >> 16u) & 0xFFu) / 255.0;
    float a = float((inPackedAttr >> 24u) & 0xFFu) / 255.0;

    vColor = vec4(r, g, b, a);
}
