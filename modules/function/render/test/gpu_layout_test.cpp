// ============================================================================
//  gpu_layout_test.cpp — PERMANENT (cpu tier): the C++ mirror structs are laid
//  out the way the shaders' copies of them are.
//
//  Every struct below exists twice — once here in C++, once in a GLSL file — and
//  until now nothing checked that the two agreed. What that cost, in the two
//  cases found by hand:
//
//    · AreaLightGPU used a 16-byte-aligned vec2 where std430 wants 8, so every
//      field from `size` on sat 8+ bytes away from where the shader reads it.
//      Both sides were divisible by 16, so the `sizeof % 16 == 0` assertion the
//      struct carried passed on both.
//    · One of five hand-copied ShadowSliceGPU declarations had lost its last
//      three fields. std430 rounded the truncated struct back up to the same
//      128-byte array stride, so indexing still worked and nothing failed.
//
//  Each field list below is the SHADER's block, field for field, and every
//  offset is checked against the C++ member. A field reorder, an alignment
//  change, or a wrong GLSL type is a compile error naming the member.
//
//  ⚠️ These lists are still written by hand, so they can drift from the .glsl
//  in the same way the .glsl drifted from C++. They are the shape the comm-ops
//  generator should emit from LUX_GPU_FIELD(glsl=...) annotations — pinning that
//  shape first is why they are hand-written now (same order L2-0 used).
//  Until then: change a GLSL struct, change the matching list here.
//
//  No Vulkan, no device — this is a compile-time check with a trivial runtime.
// ============================================================================

#include <lux/engine/function/render/client/core/GpuLayout.hpp>
#include <lux/engine/function/render/client/core/RenderSpatialTypes.hpp>

#include <lux/engine/render/resources/SceneResources.hpp>              // ViewGpuData
#include <lux/engine/render/resources/lighting/LightResources.hpp>     // *LightGPU
#include <lux/engine/function/render/client/resources/lighting/ShadowMapTypes.hpp>     // ShadowSliceGPU
#include <lux/engine/render/resources/mesh/InstanceResources.hpp>      // Instance*
#include <lux/engine/render/resources/mesh/MeshSectionTable.hpp>       // MeshSectionRecord
#include <lux/engine/render/resources/material/MaterialGpuTypes.hpp>   // *FamilyGPU
#include <lux/engine/render/renderer/features/deferred/DeferredLightingFeature.hpp>  // ClusterParamsGPU

#include <cassert>
#include <cmath>
#include <cstdio>
#include <limits>

namespace lux::render
{
    // ── ViewGpuData  ←→  assets/shaders/view_common.glsl ──────────────────
    #define LUX_L_VIEW(X, PAD)      \
        X(Mat4, view,     1)        \
        X(Mat4, proj,     1)        \
        X(Mat4, inv_view, 1)        \
        X(Mat4, inv_proj, 1)        \
        X(Vec4, cam_pos,  1)        \
        X(Vec4, viewport, 1)        \
        X(IVec4, camera_page, 1)    \
        X(Vec4, camera_local_page_size, 1)
    LUX_GPU_VERIFY(ViewGpuData, Std430, LUX_L_VIEW)

    // ── ClusterParamsGPU  ←→  assets/shaders/cluster_common.lglslh ────────
    //     (LUX_CLUSTER_PARAMS_FIELDS,std140 UBO,三个 cluster_*.comp 共用)
    //
    // 这一条补得晚,因为它此前只有一句 `sizeof == 176` 的总长断言 —— 而总长
    // 恰恰是**抓不住字段重排的那一维**:把 grid 和 limits 对调,两边都还是 176,
    // 断言照过,而 GPU 读到的 cluster_count 变成了 enable_clustered。
    // 本文件开头记的 AreaLightGPU 事故就是这个形状。
    #define LUX_L_CLUSTER_PARAMS(X, PAD)    \
        X(Mat4,  view,     1)               \
        X(Mat4,  proj,     1)               \
        X(Vec4,  viewport, 1)               \
        X(UVec4, grid,     1)               \
        X(UVec4, limits,   1)               \
        X(IVec4, camera_page, 1)            \
        X(Vec4, camera_local_page_size, 1)
    LUX_GPU_VERIFY(ClusterParamsGPU, Std140, LUX_L_CLUSTER_PARAMS)

    // ── ShadowSliceGPU  ←→  assets/shaders/shadow_common.lglslh ───────────
    #define LUX_L_SHADOW_SLICE(X, PAD)      \
        X(Mat4,  light_vp,             1)   \
        X(Float, bias,                 1)   \
        X(Float, slope_bias,           1)   \
        X(Float, normal_bias,          1)   \
        X(Float, texel_size,           1)   \
        X(Vec2,  atlas_uv_scale,       1)   \
        X(Vec2,  atlas_uv_bias,        1)   \
        X(Vec2,  atlas_inner_uv_min,   1)   \
        X(Vec2,  atlas_inner_uv_max,   1)   \
        X(Uint,  atlas_layer,          1)   \
        X(Float, shadow_near,          1)   \
        X(Float, shadow_far,           1)   \
        X(Uint,  depth_is_perspective, 1)   \
        X(IVec4, origin_page,                1) \
        X(Vec4,  origin_local_page_size,     1)
    LUX_GPU_VERIFY(ShadowSliceGPU, Std430, LUX_L_SHADOW_SLICE)

    // ── ShadowConfigGPU  ←→  assets/shaders/shadow_evsm.glsl ShadowConfigUBO ──
    //     (std140 UBO,LIGHT_SET binding 6)
    //
    // 同 ClusterParamsGPU:此前只有 `sizeof % 16 == 0`,而这一条**在任何字段数
    // 是 4 的倍数的重排下都成立** —— 11 个 4 字节标量凑成 48 字节,把
    // spot_light_count 和 point_light_offset 对调,断言照过,阴影切片索引错位。
    #define LUX_L_SHADOW_CONFIG(X, PAD)             \
        X(Uint,  total_slices,            1)        \
        X(Uint,  dir_light_offset,        1)        \
        X(Uint,  dir_cascade_count,       1)        \
        X(Uint,  spot_light_offset,       1)        \
        X(Uint,  spot_light_count,        1)        \
        X(Uint,  point_light_offset,      1)        \
        X(Uint,  point_light_count,       1)        \
        X(Float, dir_split_is_normalized, 1)        \
        X(Float, dir_split_near,          1)        \
        X(Float, dir_split_far,           1)        \
        X(Uint,  dir_caster_slot,         1)
    LUX_GPU_VERIFY(ShadowConfigGPU, Std140, LUX_L_SHADOW_CONFIG)

    // ── The four light structs  ←→  assets/shaders/light_types.glsl ───────
    //  `aligned16vec3 color` is ONE C++ member covering the shader's
    //  `vec3 color; uint _pad0;` pair — hence the PAD entries.
    #define LUX_L_DIR_LIGHT(X, PAD)         \
        X(Vec3,  color,              1)     \
        PAD(Uint, color_w,           1)     \
        X(Float, intensity,          1)     \
        X(Vec3,  direction,          1)     \
        PAD(Uint, direction_w,       1)     \
        X(Uint,  flags,              1)     \
        X(Uint,  shadow_map_size,    1)     \
        X(Float, shadow_bias,        1)     \
        X(Float, shadow_normal_bias, 1)     \
        X(Uint,  cascade_count,      1)     \
        X(Float, cascade_splits,     8)     \
        X(Uint,  _pad0,              3)
    LUX_GPU_VERIFY(DirectionalLightGPU, Std430, LUX_L_DIR_LIGHT)

    #define LUX_L_POINT_LIGHT(X, PAD)           \
        X(Vec3,  color,                  1)     \
        PAD(Uint, color_w,               1)     \
        X(Float, intensity,              1)     \
        X(Uint,  _intensity_pad,         3)     \
        X(IVec4, position_page,          1)     \
        X(Vec3,  position_local,         1)     \
        PAD(Uint, position_w,            1)     \
        X(Float, range,                  1)     \
        X(Float, attenuation_constant,   1)     \
        X(Float, attenuation_linear,     1)     \
        X(Float, attenuation_quadratic,  1)     \
        X(Uint,  flags,                  1)     \
        X(Uint,  shadow_map_size,        1)     \
        X(Float, shadow_bias,            1)     \
        X(Float, shadow_normal_bias,     1)     \
        X(Uint,  _pad0,                  3)
    LUX_GPU_VERIFY(PointLightGPU, Std430, LUX_L_POINT_LIGHT)

    #define LUX_L_SPOT_LIGHT(X, PAD)            \
        X(Vec3,  color,                  1)     \
        PAD(Uint, color_w,               1)     \
        X(Float, intensity,              1)     \
        X(Uint,  _intensity_pad,         3)     \
        X(IVec4, position_page,          1)     \
        X(Vec3,  position_local,         1)     \
        PAD(Uint, position_w,            1)     \
        X(Vec3,  direction,              1)     \
        PAD(Uint, direction_w,           1)     \
        X(Float, range,                  1)     \
        X(Float, attenuation_constant,   1)     \
        X(Float, attenuation_linear,     1)     \
        X(Float, attenuation_quadratic,  1)     \
        X(Float, inner_cone_angle,       1)     \
        X(Float, outer_cone_angle,       1)     \
        X(Uint,  flags,                  1)     \
        X(Uint,  shadow_map_size,        1)     \
        X(Float, shadow_bias,            1)     \
        X(Float, shadow_normal_bias,     1)     \
        X(Uint,  _pad0,                  2)
    LUX_GPU_VERIFY(SpotLightGPU, Std430, LUX_L_SPOT_LIGHT)

    //  The one that was actually wrong: `size` is a vec2 (align 8), and the C++
    //  side used to give it align 16, moving this and every later field.
    #define LUX_L_AREA_LIGHT(X, PAD)            \
        X(Vec3,  color,                  1)     \
        PAD(Uint, color_w,               1)     \
        X(Float, intensity,              1)     \
        X(Vec2,  size,                   1)     \
        X(Uint,  flags,                  1)     \
        X(Uint,  shadow_map_size,        1)     \
        X(Float, shadow_bias,            1)     \
        X(Float, shadow_normal_bias,     1)     \
        X(Uint,  _pad0,                  1)
    LUX_GPU_VERIFY(AreaLightGPU, Std430, LUX_L_AREA_LIGHT)

    // ── Instance structs  ←→  assets/shaders/instance.glsl ────────────────
    //  Two spellings differ from the shader's without changing the layout, and
    //  both are checked as the C++ side spells them:
    //    · `vec4 bsphere` there is `float bsphere[4]` here. Same 16 bytes, but
    //      NOT the same alignment (vec4 wants 16, float[4] wants 4) — it agrees
    //      only because the field is first. Moving it would need a real vec4.
    //    · `uint _pad0; uint _pad1;` there is `uint _pad[2]` here. In std430 an
    //      array of scalars has stride 4, so the two spellings coincide.
    #define LUX_L_INST_CULL(X, PAD)         \
        X(Vec4, bsphere,     1)             \
        X(IVec4, bsphere_page, 1)           \
        X(Uint, bucket_id,   1)             \
        X(Uint, lod_count,   1)             \
        X(Uint, lod_mdc,     4)             \
        X(Uint, _pad,        2)
    LUX_GPU_VERIFY(InstanceCullMeta, Std430, LUX_L_INST_CULL)

    #define LUX_L_SECTION(X, PAD)           \
        X(Uint, first_index,  1)            \
        X(Uint, index_count,  1)            \
        X(Int,  base_vertex,  1)            \
        X(Uint, vertex_count, 1)
    LUX_GPU_VERIFY(MeshSectionRecord, Std430, LUX_L_SECTION)

    // ── Material families  ←→  assets/shaders/material_types.glsl ─────────
    #define LUX_L_TEXREF(X, PAD)            \
        X(Uint, representation_index, 1)    \
        X(Uint, resource_index,       1)    \
        X(Uint, aux,                  1)    \
        X(Uint, flags,                1)
    LUX_GPU_VERIFY(TextureRefGPU, Std430, LUX_L_TEXREF)
}

int main()
{
    Eigen::Matrix4f perspective = Eigen::Matrix4f::Zero();
    constexpr float perspective_near = 0.25f;
    constexpr float perspective_far = 8192.0f;
    perspective(0, 0) = 1.0f;
    perspective(1, 1) = 1.0f;
    perspective(2, 2) = -perspective_far /
        (perspective_far - perspective_near);
    perspective(2, 3) = -(perspective_far * perspective_near) /
        (perspective_far - perspective_near);
    perspective(3, 2) = -1.0f;
    float recovered_near = 0.0f;
    float recovered_far = 0.0f;
    assert(lux::render::projectionDepthRange(
        perspective, recovered_near, recovered_far));
    assert(std::abs(recovered_near - perspective_near) < 1.0e-4f);
    assert(std::abs(recovered_far - perspective_far) < 4.0f);

    Eigen::Matrix4f orthographic = Eigen::Matrix4f::Identity();
    constexpr float orthographic_near = 2.0f;
    constexpr float orthographic_far = 502.0f;
    orthographic(2, 2) = -1.0f /
        (orthographic_far - orthographic_near);
    orthographic(2, 3) = -orthographic_near /
        (orthographic_far - orthographic_near);
    assert(lux::render::projectionDepthRange(
        orthographic, recovered_near, recovered_far));
    assert(std::abs(recovered_near - orthographic_near) < 1.0e-4f);
    assert(std::abs(recovered_far - orthographic_far) < 1.0e-3f);

    std::int32_t page_delta_2d[2]{12, -4};
    const std::int64_t origin_delta[3]{10, -2, 7};
    assert(lux::render::canRebaseRenderPageDelta2D(
        page_delta_2d,
        origin_delta));
    lux::render::rebaseRenderPageDelta2D(
        page_delta_2d,
        origin_delta);
    assert(page_delta_2d[0] == 2 && page_delta_2d[1] == -2);
    const std::int64_t rejected_delta[3]{
        std::numeric_limits<std::int64_t>::max(), 0, 0};
    assert(!lux::render::canRebaseRenderPageDelta2D(
        page_delta_2d,
        rejected_delta));

    // Everything above is a static_assert; reaching main means they all held.
    std::puts("gpu_layout_test PASSED (all checks are compile-time)");
    return 0;
}
