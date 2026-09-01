#pragma once

#include <lux/engine/meta/MetaAnnotations.hpp>
#include <lux/engine/function/render/client/protocol/RenderCommTypes.hpp>
#include <lux/engine/function/render/client/core/FeatureHandle.hpp>
#include <lux/engine/function/render/client/core/RenderResourceHandle.hpp>
#include <lux/engine/function/render/client/core/RenderSceneId.hpp>
#include <lux/engine/function/render/client/core/RenderSpatialTypes.hpp>
#include <lux/engine/function/render/client/core/ResourceHandle.hpp>

#include <cstdint>
#include <type_traits>

namespace lux::render
{
    struct WaterSurfaceTag
    {
    };
    using RWaterSurfaceHandle = RenderResourceHandle<WaterSurfaceTag>;

    struct WaterSurfaceDesc final
    {
        RenderSpatialTransform3D transform{};
        float half_extent[2]{50.0f, 50.0f};
        float normal_scroll_a[2]{0.015f, 0.006f};
        float normal_scroll_b[2]{-0.008f, 0.012f};
        float absorption_color[3]{0.03f, 0.16f, 0.19f};
        float absorption_distance{8.0f};
        float roughness{0.12f};
        float normal_strength{0.35f};
        float wave_scale{0.08f};
        RTextureHandle normal_texture{};
        std::uint32_t transition_seed{1u};
        std::uint32_t transition_milliseconds{350u};
    };
    static_assert(std::is_trivially_copyable_v<WaterSurfaceDesc>);

    struct WaterSurfaceCreatedReply final
    {
        RWaterSurfaceHandle handle{};
        std::uint32_t status{0u};
    };
    static_assert(std::is_trivially_copyable_v<WaterSurfaceCreatedReply>);

    struct LUX_OP(
        lane = program,
        kind = resource,
        name = WaterSurfaceCreate,
        method = createSurface,
        reply = WaterSurfaceCreatedReply) WaterSurfaceCreatePayload final
    {
        RenderSceneId scene_id{};
        WaterSurfaceDesc surface{};
    };
    static_assert(std::is_trivially_copyable_v<WaterSurfaceCreatePayload>);

    struct LUX_OP(
        lane = program,
        kind = stream,
        name = WaterSurfaceUpdate,
        method = updateSurface,
        opcode = resource,
        bulk = WaterSurfaceBatch,
        bulk_method = updateSurfaces) WaterSurfaceUpdatePayload final
    {
        RenderSceneId scene_id{};
        RWaterSurfaceHandle handle{};
        WaterSurfaceDesc surface{};
    };
    static_assert(std::is_trivially_copyable_v<WaterSurfaceUpdatePayload>);

    struct LUX_OP(lane = program, kind = stream, name = WaterSurfaceDestroy, method = destroySurface, opcode = resource)
        WaterSurfaceDestroyPayload final
    {
        RenderSceneId scene_id{};
        RWaterSurfaceHandle handle{};
    };
    static_assert(std::is_trivially_copyable_v<WaterSurfaceDestroyPayload>);

    struct WaterStatsReply final
    {
        std::uint32_t resident_surfaces{0u};
        std::uint32_t visible_patches{0u};
        std::uint32_t transitioning_surfaces{0u};
        std::uint32_t transparent_hard_cuts{0u};
        std::uint64_t cpu_resident_bytes{0u};
        std::uint64_t gpu_capacity_bytes{0u};
    };
    static_assert(std::is_trivially_copyable_v<WaterStatsReply>);

    struct LUX_OP(
        lane = control,
        kind = resource,
        name = WaterStats,
        method = stats,
        reply = WaterStatsReply,
        opcode = command) WaterStatsPayload final
    {
        RenderSceneId scene_id{};
    };
    static_assert(std::is_trivially_copyable_v<WaterStatsPayload>);

    struct LUX_TYPE_INFO(both) LUX_COMM_CONFIG(
        prefix = Water,
        id = lux.render.water.v1,
        display = Water,
        requires = "lux.render.fog.v1?,lux.render.linear_depth.v1,lux.render.view_camera.v1",
        feature = WaterFeature,
        feature_header = lux / engine / render / renderer / features / water / WaterFeature.hpp,
        multiplicity = single) WaterCommConfig final
    {
        ShaderHandle vertex_shader LUX_TYPE_MEMBER(skip_static = true) LUX_NO_MEMBER(){};
        ShaderHandle fragment_shader LUX_TYPE_MEMBER(skip_static = true) LUX_NO_MEMBER(){};
        std::uint32_t maximum_surfaces{256u};
    };
    static_assert(std::is_trivially_copyable_v<WaterCommConfig>);
} // namespace lux::render
