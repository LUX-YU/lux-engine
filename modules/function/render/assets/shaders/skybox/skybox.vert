#version 450

// =============================================================================
// Skybox vertex shader — fullscreen triangle, outputs world-space view direction.
//
// Uses ViewBuffer inv_view/inv_proj matrices to reconstruct the view ray for each fragment.
// Emits gl_Position.z = 1.0 (far plane) so that the skybox sits behind all
// geometry when depth_compare = LESS_OR_EQUAL, depth_write = OFF.
// =============================================================================

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

// ====== Shared Push Constants (scene/view indices, offset 0) ======
layout(push_constant) uniform SharedPC {
    uint scene_index;
    uint view_index;
} uPC;

layout(location = 0) out vec3 vWorldDir;

void main()
{
    // Fullscreen triangle: 3 vertices cover the entire screen
    vec2 V[3] = vec2[](vec2(-1.0, -1.0), vec2(3.0, -1.0), vec2(-1.0, 3.0));
    vec2 ndc = V[gl_VertexIndex];

    // Output at far plane (z = 1.0, w = 1.0 → NDC depth = 1.0)
    gl_Position = vec4(ndc, 1.0, 1.0);

    // Reconstruct world-space direction from NDC
    vec4 viewPos = uViews.views[uPC.view_index].inv_proj * vec4(ndc, 1.0, 1.0);
    viewPos.xyz /= viewPos.w;
    vWorldDir = mat3(uViews.views[uPC.view_index].inv_view) * viewPos.xyz;
}
