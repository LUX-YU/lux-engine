#pragma once
// ============================================================================
//  LightOperation.hpp — LightFeature factory + feature-scoped light CRUD ops.
//
//  Scene lights (LightResources storage + create/update/destroy/batch) are a
//  FEATURE domain, not a core protocol op. The commands are dispatched by
//  dynamically-allocated TypeIds (registered via the feature's register_ops_fn
//  — the grid pattern) and sent by a feature-scoped LightProxy. The core
//  RenderProtocol.hpp no longer names light. NOTE: LightFeature owns the light
//  DATA; DeferredLighting/Forward/Shadow are its CONSUMERS (find<LightResources>).
//  (See .internal/render-architecture-decoupling-design-2026-06-19.md, contracts C2+C5.)
// ============================================================================

#include <lux/engine/meta/MetaAnnotations.hpp>
#include <lux/engine/function/render/client/protocol/RenderCommTypes.hpp> // TypeId, opcodes, CommandTraits
#include <lux/engine/function/render/client/protocol/FeatureOps.hpp>      // EOpKind / FeatureOpIds / reply_type_id_of_v
#include <lux/engine/function/render/client/resources/lighting/LightDescriptor.hpp>
#include <lux/engine/function/render/client/core/RenderSceneId.hpp>
#include <lux/engine/function/render/client/core/RenderEntityId.hpp>
#include <lux/engine/function/visibility.h>

#include <Eigen/Core>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <type_traits>
#include <variant>

namespace lux::render
{
    struct FeatureFactory;
    class RenderProgramSession;
    template <typename T> class RenderRequest;

    // =========================================================================
    //  Feature-scoped operation name constants (allocated dynamically at
    //  register_ops_fn time; the client reads the ids from the registration reply)
    // =========================================================================
    struct LUX_OP(
        lane = program,
        kind = stream,
        name = UpsertLight,
        method = upsertLight,
        bulk = LightBatch,
        bulk_method = upsertLights) UpsertLightPayload
    {
        RenderSceneId scene_id{};
        RenderEntityId entity{};
        std::uint32_t transition_milliseconds{0u};
        uint8_t light_type{0}; // 0=Dir, 1=Point, 2=Spot, 3=Area
        RenderLargePosition3D spatial_position{};
        float direction[3]{0.f, -1.f, 0.f};
        float color[3]{1.f, 1.f, 1.f};
        float intensity{1.f};
        float range{10.f};
        float attenuation_constant{1.f};
        float attenuation_linear{0.09f};
        float attenuation_quadratic{0.032f};
        float inner_cone_angle{0.5236f};
        float outer_cone_angle{0.7854f};
        uint32_t flags{0};
        uint32_t shadow_map_size{1024};
        float shadow_bias{0.005f};
        float shadow_normal_bias{0.01f};
        uint32_t cascade_count{4};
        float cascade_splits[kShadowCascadeSlots]{0.1f, 0.25f, 0.5f, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f};
        float area_size[2]{1.f, 1.f};
    };
    static_assert(std::is_trivially_copyable_v<UpsertLightPayload>);

    // Light identity is the owning scene plus its stable Render entity.
    struct LUX_OP(lane = program, kind = stream, name = RemoveLight, method = removeLight) RemoveLightPayload
    {
        RenderSceneId scene_id{};
        RenderEntityId entity{};
        std::uint32_t transition_milliseconds{0u};
    };
    static_assert(std::is_trivially_copyable_v<RemoveLightPayload>);

    struct LightStatsReply final
    {
        std::uint32_t directional_lights{0u};
        std::uint32_t point_lights{0u};
        std::uint32_t spot_lights{0u};
        std::uint32_t area_lights{0u};
        std::uint32_t transitioning_lights{0u};
    };
    static_assert(std::is_trivially_copyable_v<LightStatsReply>);

    struct LUX_OP(
        lane = control,
        kind = resource,
        name = LightStats,
        method = stats,
        reply = LightStatsReply,
        opcode = command) LightStatsPayload final
    {
        RenderSceneId scene_id{};
    };
    static_assert(std::is_trivially_copyable_v<LightStatsPayload>);

#ifndef __LUX_PARSE_TIME__
    static_assert(std::is_standard_layout_v<UpsertLightPayload>);
#endif

    // =========================================================================
    //  LightDescriptor ↔ flat payload conversion
    // =========================================================================

    inline UpsertLightPayload
    toLightPayload(RenderSceneId scene_id, RenderEntityId entity, const LightDescriptor& desc)
    {
        UpsertLightPayload p{};
        p.scene_id = scene_id;
        p.entity = entity;

        std::visit(
            [&](auto&& arg) {
                using T = std::decay_t<decltype(arg)>;
                if constexpr (std::is_same_v<T, DirectionalLightDesc>)
                {
                    p.light_type = 0;
                    Eigen::Map<Eigen::Vector3f>(p.direction) = arg.direction;
                    Eigen::Map<Eigen::Vector3f>(p.color) = arg.color;
                    p.intensity = arg.intensity;
                    p.flags = arg.flags;
                    p.shadow_map_size = arg.shadow_map_size;
                    p.shadow_bias = arg.shadow_bias;
                    p.shadow_normal_bias = arg.shadow_normal_bias;
                    p.cascade_count = arg.cascade_count;
                    std::copy(arg.cascade_splits.begin(), arg.cascade_splits.end(), p.cascade_splits);
                }
                else if constexpr (std::is_same_v<T, PointLightDesc>)
                {
                    p.light_type = 1;
                    p.spatial_position = arg.spatial_position;
                    Eigen::Map<Eigen::Vector3f>(p.color) = arg.color;
                    p.intensity = arg.intensity;
                    p.range = arg.range;
                    p.attenuation_constant = arg.attenuation_constant;
                    p.attenuation_linear = arg.attenuation_linear;
                    p.attenuation_quadratic = arg.attenuation_quadratic;
                    p.flags = arg.flags;
                    p.shadow_map_size = arg.shadow_map_size;
                    p.shadow_bias = arg.shadow_bias;
                    p.shadow_normal_bias = arg.shadow_normal_bias;
                }
                else if constexpr (std::is_same_v<T, SpotLightDesc>)
                {
                    p.light_type = 2;
                    p.spatial_position = arg.spatial_position;
                    Eigen::Map<Eigen::Vector3f>(p.direction) = arg.direction;
                    Eigen::Map<Eigen::Vector3f>(p.color) = arg.color;
                    p.intensity = arg.intensity;
                    p.range = arg.range;
                    p.attenuation_constant = arg.attenuation_constant;
                    p.attenuation_linear = arg.attenuation_linear;
                    p.attenuation_quadratic = arg.attenuation_quadratic;
                    p.inner_cone_angle = arg.inner_cone_angle;
                    p.outer_cone_angle = arg.outer_cone_angle;
                    p.flags = arg.flags;
                    p.shadow_map_size = arg.shadow_map_size;
                    p.shadow_bias = arg.shadow_bias;
                    p.shadow_normal_bias = arg.shadow_normal_bias;
                }
                else if constexpr (std::is_same_v<T, AreaLightDesc>)
                {
                    p.light_type = 3;
                    Eigen::Map<Eigen::Vector3f>(p.color) = arg.color;
                    p.intensity = arg.intensity;
                    p.area_size[0] = arg.size.x();
                    p.area_size[1] = arg.size.y();
                    p.flags = arg.flags;
                    p.shadow_map_size = arg.shadow_map_size;
                    p.shadow_bias = arg.shadow_bias;
                    p.shadow_normal_bias = arg.shadow_normal_bias;
                }
            },
            desc
        );

        return p;
    }

    inline LightDescriptor fromLightPayload(const UpsertLightPayload& p)
    {
        switch (p.light_type)
        {
        case 0: {
            DirectionalLightDesc d{};
            d.direction = Eigen::Map<const Eigen::Vector3f>(p.direction);
            d.color = Eigen::Map<const Eigen::Vector3f>(p.color);
            d.intensity = p.intensity;
            d.flags = p.flags;
            d.shadow_map_size = p.shadow_map_size;
            d.shadow_bias = p.shadow_bias;
            d.shadow_normal_bias = p.shadow_normal_bias;
            d.cascade_count = p.cascade_count;
            std::copy_n(p.cascade_splits, 8, d.cascade_splits.begin());
            return d;
        }
        case 1: {
            PointLightDesc d{};
            d.spatial_position = p.spatial_position;
            d.color = Eigen::Map<const Eigen::Vector3f>(p.color);
            d.intensity = p.intensity;
            d.range = p.range;
            d.attenuation_constant = p.attenuation_constant;
            d.attenuation_linear = p.attenuation_linear;
            d.attenuation_quadratic = p.attenuation_quadratic;
            d.flags = p.flags;
            d.shadow_map_size = p.shadow_map_size;
            d.shadow_bias = p.shadow_bias;
            d.shadow_normal_bias = p.shadow_normal_bias;
            return d;
        }
        case 2: {
            SpotLightDesc d{};
            d.spatial_position = p.spatial_position;
            d.direction = Eigen::Map<const Eigen::Vector3f>(p.direction);
            d.color = Eigen::Map<const Eigen::Vector3f>(p.color);
            d.intensity = p.intensity;
            d.range = p.range;
            d.attenuation_constant = p.attenuation_constant;
            d.attenuation_linear = p.attenuation_linear;
            d.attenuation_quadratic = p.attenuation_quadratic;
            d.inner_cone_angle = p.inner_cone_angle;
            d.outer_cone_angle = p.outer_cone_angle;
            d.flags = p.flags;
            d.shadow_map_size = p.shadow_map_size;
            d.shadow_bias = p.shadow_bias;
            d.shadow_normal_bias = p.shadow_normal_bias;
            return d;
        }
        case 3: {
            AreaLightDesc d{};
            d.color = Eigen::Map<const Eigen::Vector3f>(p.color);
            d.intensity = p.intensity;
            d.size = Eigen::Map<const Eigen::Vector2f>(p.area_size);
            d.flags = p.flags;
            d.shadow_map_size = p.shadow_map_size;
            d.shadow_bias = p.shadow_bias;
            d.shadow_normal_bias = p.shadow_normal_bias;
            return d;
        }
        default:
            return DirectionalLightDesc{};
        }
    }

    /// 无客户端创建参数 —— 空 tag 承载特性身份(数据型基础特性,
    /// SinglePerScene:第二实例只是同一注册表资源上的空壳,拒绝之)。
    struct LUX_TYPE_INFO(both) LUX_COMM_CONFIG(
        prefix = Light,
        id = lux.render.light.v1,
        display = Light,
        feature = LightFeature,
        feature_header = lux / engine / render / renderer / features / light / LightFeature.hpp,
        multiplicity = single) LightCommTag
    {
    };
    static_assert(std::is_trivially_copyable_v<LightCommTag>);

    class LightProxy; // 生成于 comm/genops/LightOperation.ops.hpp

    // ── 便捷面(§7.5):LightDescriptor 变体 → flat 载荷的转换留手写,
    //    定义在 LightOperationHandlers.cpp;proxy 按值传以接临时对象。──
    LUX_FUNCTION_PUBLIC void lightUpsert(
        LightProxy proxy,
        RenderSceneId scene_id,
        RenderEntityId entity,
        const LightDescriptor& desc,
        std::uint32_t transition_milliseconds = 0u
    );
} // namespace lux::render
