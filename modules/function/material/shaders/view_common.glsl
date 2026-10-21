// =========================================================================
//  view_common.glsl — per-View GPU data struct (std430)
//
//  Must match C++ ViewGpuData in resources/SceneResources.hpp (320 B).
//  The C++ side is pinned by
//    static_assert(sizeof(ViewGpuData) == kViewDataStrideBytes)
//  where kViewDataStrideBytes lives in core/RenderTypes.hpp — that constant is
//  the per-view byte budget the neutral scene uploads, and this layout is what
//  fills it.
//
//  ⚠️ C++↔GLSL 之间**没有任何自动校验**。这个文件的意义是把原先散在 17 个
//  着色器里的 17 份手抄收敛成 1 份 —— 漂移面从 17 处缩到 1 处,但不等于消除。
//  改 C++ 的 ViewGpuData 时,这里必须同步改;反之亦然。
//
//  只声明**类型**,不声明资源。`uViews` 缓冲的声明留在各着色器本地,因为
//  set/binding 在两条流水线上不一样:
//    · .lglsl 走 lux_shader_emitter,按 LayoutContract 注入 → 不写 set/binding
//    · deferred_lighting.frag 不走 emitter → 自己硬写 set = 0, binding = 1
//  把这行也收进来会强行统一一件本来就不统一的事。
// =========================================================================
#ifndef VIEW_COMMON_GLSL
#define VIEW_COMMON_GLSL

struct ViewGpuData {
    mat4 view;      //  64B  col-major
    mat4 proj;      //  64B
    mat4 inv_view;  //  64B
    mat4 inv_proj;  //  64B
    vec4 cam_pos;   //  16B  xyz + w=1
    vec4 viewport;  //  16B  width, height, near_z, far_z
    ivec4 camera_page;
    vec4 camera_local_page_size; // xyz local, w coordinate page size
};                  // = 320B

vec3 luxViewRelative(vec3 scene_position, ViewGpuData view_data) {
    return scene_position - view_data.cam_pos.xyz;
}

#endif // VIEW_COMMON_GLSL
