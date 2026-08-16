#pragma once
/**
 * @file LightSubsystems.hpp
 * @brief DirectionalLight / PointLight / SpotLightSubsystem —— 三种光源的渲染子系统,
 *        「池化槽位」形状(PooledSlotSubsystem<各自的 RenderPolicy>). Each entity
 *        owns a pooled render light (create/update/destroy by handle via LightProxy,
 *        dirty-diffed on the desc). Directional places its direction directly; Point/
 *        Spot pull world-space position from ResolvedTransform3DComponent (Require). The
 *        three differ only in which desc fields they fill. Replaces the 3 light
 */

#include <lux/engine/function/render/client/RenderFrameSession.hpp>               // RenderFrameSession
#include <lux/engine/function/render/client/core/RenderResourceHandle.hpp>              // RLightHandle
#include <lux/engine/function/render/client/core/RenderSceneId.hpp>                     // RenderSceneId
#include <lux/engine/function/render/client/resources/lighting/LightDescriptor.hpp>                   // *LightDesc / LightDescriptor variant
#include <lux/engine/function/render/client/genops/LightOperation.ops.hpp> // LightProxy / LightOperationIds / LightCreatedReply

#include "lux/engine/ecs/render/components/3d/DirectionalLightComponent.hpp"
#include "lux/engine/ecs/render/components/3d/PointLightComponent.hpp"
#include "lux/engine/ecs/render/components/3d/SpotLightComponent.hpp"
#include "lux/engine/ecs/components/ResolvedTransform3DComponent.hpp"
#include <lux/engine/ecs/render/RenderSpatialTransform.hpp>
#include "lux/engine/ecs/render/RenderViewUtil.hpp"                 // ComponentList
#include "lux/engine/ecs/render/subsystems/PooledSlotSubsystem.hpp"

#include <optional>

namespace lux::ecs
{
    // Component types live in the lux::ecs kit.

    namespace light_detail
    {
        inline std::optional<lux::render::RenderLargePosition3D>
        spatialPosition(
            const ResolvedTransform3DComponent& transform,
            SceneRenderBinding& render) noexcept
        {
            const auto position = makeRenderLargePosition(
                transform.position,
                render.sceneOriginTile3D()
            );
            if (!position)
            {
                render.requestSceneOriginRebase(transform.position);
                return std::nullopt;
            }
            return position;
        }

        // Shared POOL plumbing for every light kind (only the desc differs).
        template <class Desc>
        inline auto create(
            lux::render::RenderFrameSession& s,
            const lux::render::LightOperationIds& o,
            lux::render::RenderSceneId sc,
            const Desc& d,
            std::uint32_t transition_milliseconds = 0u)
        {
            return lux::render::lightCreate(
                lux::render::LightProxy(s, o),
                sc,
                lux::render::LightDescriptor{d},
                transition_milliseconds);
        }
        template <class Desc>
        inline void update(lux::render::RenderFrameSession& s, const lux::render::LightOperationIds& o,
                           lux::render::RenderSceneId sc, lux::render::RLightHandle h, const Desc& d)
        {
            lux::render::lightUpdate(lux::render::LightProxy(s, o), sc, h, lux::render::LightDescriptor{d});
        }
        inline void destroy(lux::render::RenderFrameSession& s, const lux::render::LightOperationIds& o,
                            lux::render::RenderSceneId sc, lux::render::RLightHandle h)
        {
            lux::render::LightProxy(s, o).destroyLight({.scene_id = sc, .handle = h});
        }
        inline void retire(
            lux::render::RenderFrameSession& s,
            const lux::render::LightOperationIds& o,
            lux::render::RenderSceneId sc,
            lux::render::RLightHandle h,
            std::uint32_t transition_milliseconds)
        {
            lux::render::LightProxy(s, o).destroyLight({
                .scene_id = sc,
                .handle = h,
                .transition_milliseconds = transition_milliseconds});
        }
    }

    /// PooledSlotSubsystem 的平行光策略(原 EcsRenderTraits<DirectionalLightComponent> 特化)。
    struct DirectionalLightRenderPolicy final
    {
        using Component = DirectionalLightComponent;
        static constexpr const char* feature = "Light";
        using Ops     = lux::render::LightOperationIds;
        using Desc    = lux::render::DirectionalLightDesc;
        using Handle  = lux::render::RLightHandle;
        using Reply   = lux::render::LightCreatedReply;
        using Require = ComponentList<>;
        using Exclude = ComponentList<>;
        static constexpr bool supports_visual_transition = false;

        static std::optional<Desc> extract(
            lux::meta::entity_id,
            const DirectionalLightComponent& c,
            lux::meta::EntityRegistry&,
            SceneRenderBinding&)
        {
            Desc d{};
            d.direction       = c.direction;     // unit world-space; NOT from a transform
            d.color           = c.color;
            d.intensity       = c.intensity;
            d.flags           = c.cast_shadow ? lux::render::LIGHT_FLAG_CAST_SHADOW : 0u;
            d.shadow_map_size = c.shadow_map_size;
            d.shadow_bias     = c.shadow_bias;
            d.cascade_count   = c.cascade_count;
            d.cascade_splits  = c.cascade_splits;
            return d;
        }
        static auto   create (lux::render::RenderFrameSession& s, const Ops& o, lux::render::RenderSceneId sc, const Desc& d) { return light_detail::create(s, o, sc, d); }
        static void   update (lux::render::RenderFrameSession& s, const Ops& o, lux::render::RenderSceneId sc, Handle h, const Desc& d) { light_detail::update(s, o, sc, h, d); }
        static void   destroy(lux::render::RenderFrameSession& s, const Ops& o, lux::render::RenderSceneId sc, Handle h) { light_detail::destroy(s, o, sc, h); }
        static Handle handle (const Reply& r) { return r.handle; }
    };

    /// PooledSlotSubsystem 的点光策略(原 EcsRenderTraits<PointLightComponent> 特化)。
    struct PointLightRenderPolicy final
    {
        using Component = PointLightComponent;
        static constexpr const char* feature = "Light";
        using Ops     = lux::render::LightOperationIds;
        using Desc    = lux::render::PointLightDesc;
        using Handle  = lux::render::RLightHandle;
        using Reply   = lux::render::LightCreatedReply;
        using Require = ComponentList<ResolvedTransform3DComponent>;   // position
        using Exclude = ComponentList<>;
        static constexpr bool supports_visual_transition = true;

        static std::optional<Desc> extract(
            lux::meta::entity_id e,
            const PointLightComponent& c,
            lux::meta::EntityRegistry& reg,
            SceneRenderBinding& render)
        {
            Desc d{};
            const auto position = light_detail::spatialPosition(
                reg.get<ResolvedTransform3DComponent>(e),
                render
            );
            if (!position)
                return std::nullopt;
            d.spatial_position = *position;
            d.color                 = c.color;
            d.intensity             = c.intensity;
            d.range                 = c.range;
            d.attenuation_constant  = c.attenuation_constant;
            d.attenuation_linear    = c.attenuation_linear;
            d.attenuation_quadratic = c.attenuation_quadratic;
            d.flags                 = c.cast_shadow ? lux::render::LIGHT_FLAG_CAST_SHADOW : 0u;
            d.shadow_map_size       = c.shadow_map_size;
            d.shadow_bias           = c.shadow_bias;
            d.shadow_normal_bias    = c.shadow_normal_bias;
            return d;
        }
        static auto create(
            lux::render::RenderFrameSession& s,
            const Ops& o,
            lux::render::RenderSceneId sc,
            const Desc& d,
            std::uint32_t transition_milliseconds)
        {
            return light_detail::create(
                s, o, sc, d, transition_milliseconds);
        }
        static void   update (lux::render::RenderFrameSession& s, const Ops& o, lux::render::RenderSceneId sc, Handle h, const Desc& d) { light_detail::update(s, o, sc, h, d); }
        static void   destroy(lux::render::RenderFrameSession& s, const Ops& o, lux::render::RenderSceneId sc, Handle h) { light_detail::destroy(s, o, sc, h); }
        static void retire(
            lux::render::RenderFrameSession& s,
            const Ops& o,
            lux::render::RenderSceneId sc,
            Handle h,
            std::uint32_t transition_milliseconds)
        {
            light_detail::retire(
                s, o, sc, h, transition_milliseconds);
        }
        static Handle handle (const Reply& r) { return r.handle; }
    };

    /// PooledSlotSubsystem 的聚光策略(原 EcsRenderTraits<SpotLightComponent> 特化)。
    struct SpotLightRenderPolicy final
    {
        using Component = SpotLightComponent;
        static constexpr const char* feature = "Light";
        using Ops     = lux::render::LightOperationIds;
        using Desc    = lux::render::SpotLightDesc;
        using Handle  = lux::render::RLightHandle;
        using Reply   = lux::render::LightCreatedReply;
        using Require = ComponentList<ResolvedTransform3DComponent>;   // position
        using Exclude = ComponentList<>;
        static constexpr bool supports_visual_transition = true;

        static std::optional<Desc> extract(
            lux::meta::entity_id e,
            const SpotLightComponent& c,
            lux::meta::EntityRegistry& reg,
            SceneRenderBinding& render)
        {
            Desc d{};
            const auto position = light_detail::spatialPosition(
                reg.get<ResolvedTransform3DComponent>(e),
                render
            );
            if (!position)
                return std::nullopt;
            d.spatial_position = *position;
            d.direction             = c.direction;
            d.color                 = c.color;
            d.intensity             = c.intensity;
            d.range                 = c.range;
            d.attenuation_constant  = c.attenuation_constant;
            d.attenuation_linear    = c.attenuation_linear;
            d.attenuation_quadratic = c.attenuation_quadratic;
            d.inner_cone_angle      = c.inner_cone_angle;
            d.outer_cone_angle      = c.outer_cone_angle;
            d.flags                 = c.cast_shadow ? lux::render::LIGHT_FLAG_CAST_SHADOW : 0u;
            d.shadow_map_size       = c.shadow_map_size;
            d.shadow_bias           = c.shadow_bias;
            d.shadow_normal_bias    = c.shadow_normal_bias;
            return d;
        }
        static auto create(
            lux::render::RenderFrameSession& s,
            const Ops& o,
            lux::render::RenderSceneId sc,
            const Desc& d,
            std::uint32_t transition_milliseconds)
        {
            return light_detail::create(
                s, o, sc, d, transition_milliseconds);
        }
        static void   update (lux::render::RenderFrameSession& s, const Ops& o, lux::render::RenderSceneId sc, Handle h, const Desc& d) { light_detail::update(s, o, sc, h, d); }
        static void   destroy(lux::render::RenderFrameSession& s, const Ops& o, lux::render::RenderSceneId sc, Handle h) { light_detail::destroy(s, o, sc, h); }
        static void retire(
            lux::render::RenderFrameSession& s,
            const Ops& o,
            lux::render::RenderSceneId sc,
            Handle h,
            std::uint32_t transition_milliseconds)
        {
            light_detail::retire(
                s, o, sc, h, transition_milliseconds);
        }
        static Handle handle (const Reply& r) { return r.handle; }
    };

    /// 三种光源的渲染子系统:「池化槽位」形状(每实体一个池化对象,句柄 CRUD + 脏比较)。
    using DirectionalLightSubsystem = PooledSlotSubsystem<DirectionalLightRenderPolicy>;
    using PointLightSubsystem       = PooledSlotSubsystem<PointLightRenderPolicy>;
    using SpotLightSubsystem        = PooledSlotSubsystem<SpotLightRenderPolicy>;

} // namespace lux::ecs
