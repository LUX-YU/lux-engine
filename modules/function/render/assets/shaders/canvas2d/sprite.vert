#version 450

// =============================================================================
// Canvas2D sprite vertex shader (R2-02).
//
// CPU expands each SpriteDraw into 6 CanvasVertex (2 triangles) in WORLD space
// (the sprite's WorldTransform2D 4x4 is applied on the CPU to a unit quad). This
// shader only applies the scene camera (set 0, binding 1 — the SAME per-view
// ViewGpuData path as 3D; the 2D camera's ortho view/proj is uploaded there by the
// camera bridge, R2-04). Painter order comes from the CPU draw order (no depth).
//
// CanvasVertex (24 bytes):
//   vec2  position       (8 bytes, location 0)  world XY (z = 0)
//   vec2  uv             (8 bytes, location 1)  atlas UV
//   uint  rgba           (4 bytes, location 2)  PREMULTIPLIED RGBA8, R[7:0]..A[31:24]
//   uint  texture_bindless (4 bytes, location 3) bindless set-2 index, or 0xFFFFFFFF = tint-only
// =============================================================================

layout(location = 0) in vec2 inPosition;
layout(location = 1) in vec2 inUV;
layout(location = 2) in uint inRGBA;
layout(location = 3) in uint inTexBindless;

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

layout(location = 0) out vec2 vUV;
layout(location = 1) out vec4 vColor;
layout(location = 2) flat out uint vTexBindless;

void main()
{
    gl_Position = uViews.views[uPC.view_index].proj
                * uViews.views[uPC.view_index].view
                * vec4(inPosition, 0.0, 1.0);

    vUV          = inUV;
    vTexBindless = inTexBindless;

    float r = float((inRGBA >>  0u) & 0xFFu) / 255.0;
    float g = float((inRGBA >>  8u) & 0xFFu) / 255.0;
    float b = float((inRGBA >> 16u) & 0xFFu) / 255.0;
    float a = float((inRGBA >> 24u) & 0xFFu) / 255.0;
    vColor = vec4(r, g, b, a);   // already premultiplied (pipeline blends ONE / 1-SRC_ALPHA)
}
