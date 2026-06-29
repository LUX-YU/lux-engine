#pragma once
/**
 * @file LightRenderTraits.hpp
 * @brief EcsRenderTraits for Directional / Point / Spot lights — POOL. Each entity
 *        owns a pooled render light (create/update/destroy by handle via LightProxy,
 *        dirty-diffed on the desc). Directional places its direction directly; Point/
 *        Spot pull world-space position from WorldTransformComponent (Require). The
 *        three differ only in which desc fields they fill. Replaces the 3 light
 */

#include <lux/engine/render/comm/client/RenderSession.hpp>               // RenderSession
#include <lux/engine/render/core/RenderResourceHandle.hpp>              // RLightHandle
#include <lux/engine/render/core/RenderSceneId.hpp>                     // RenderSceneId
#include <lux/engine/render/core/LightDescriptor.hpp>                   // *LightDesc / LightDescriptor variant
#include <lux/engine/render/renderer/features/light/LightOperation.hpp> // LightProxy / LightOperationIds / LightCreatedReply

#include "lux/engine/gameplay/3d/world/components/DirectionalLightComponent.hpp"
#include "lux/engine/gameplay/3d/world/components/PointLightComponent.hpp"
#include "lux/engine/gameplay/3d/world/components/SpotLightComponent.hpp"
#include "lux/engine/gameplay/3d/world/components/WorldTransformComponent.hpp"
#include "lux/engine/gameplay/render_bridge/EcsRenderTraits.hpp"

namespace lux::gameplay
{
    // Component types live in the lux::gameplay::d3 kit.
    using lux::gameplay::d3::DirectionalLightComponent;
    using lux::gameplay::d3::PointLightComponent;
    using lux::gameplay::d3::SpotLightComponent;
    using lux::gameplay::d3::WorldTransformComponent;

    namespace light_detail
    {
        // Shared POOL plumbing for every light kind (only the desc differs).
        template <class Desc>
        inline auto create(lux::render::RenderSession& s, const lux::render::LightOperationIds& o,
                           lux::render::RenderSceneId sc, const Desc& d)
        {
            return lux::render::LightProxy(s, o).createLight(sc, lux::render::LightDescriptor{d});
        }
        template <class Desc>
        inline void update(lux::render::RenderSession& s, const lux::render::LightOperationIds& o,
                           lux::render::RenderSceneId sc, lux::render::RLightHandle h, const Desc& d)
        {
            lux::render::LightProxy(s, o).updateLight(sc, h, lux::render::LightDescriptor{d});
        }
        inline void destroy(lux::render::RenderSession& s, const lux::render::LightOperationIds& o,
                            lux::render::RenderSceneId sc, lux::render::RLightHandle h)
        {
            lux::render::LightProxy(s, o).destroyLight(sc, h);
        }
    }

    template <>
    struct EcsRenderTraits<DirectionalLightComponent>
    {
        static constexpr ERenderableKind kind    = ERenderableKind::POOL;
        static constexpr const char*     feature = "Light";
        using Ops     = lux::render::LightOperationIds;
        using Desc    = lux::render::DirectionalLightDesc;
        using Handle  = lux::render::RLightHandle;
        using Reply   = lux::render::LightCreatedReply;
        using Require = ComponentList<>;
        using Exclude = ComponentList<>;

        static Desc extract(lux::meta::entity_id, const DirectionalLightComponent& c, lux::meta::EntityRegistry&)
        {
            Desc d{};
            d.direction       = c.direction;     // unit world-space; NOT from a transform
            d.color           = c.color;
            d.intensity       = c.intensity;
            d.flags           = c.flags;
            d.shadow_map_size = c.shadow_map_size;
            d.shadow_bias     = c.shadow_bias;
            d.cascade_count   = c.cascade_count;
            d.cascade_splits  = c.cascade_splits;
            return d;
        }
        static auto   create (lux::render::RenderSession& s, const Ops& o, lux::render::RenderSceneId sc, const Desc& d) { return light_detail::create(s, o, sc, d); }
        static void   update (lux::render::RenderSession& s, const Ops& o, lux::render::RenderSceneId sc, Handle h, const Desc& d) { light_detail::update(s, o, sc, h, d); }
        static void   destroy(lux::render::RenderSession& s, const Ops& o, lux::render::RenderSceneId sc, Handle h) { light_detail::destroy(s, o, sc, h); }
        static Handle handle (const Reply& r) { return r.handle; }
    };

    template <>
    struct EcsRenderTraits<PointLightComponent>
    {
        static constexpr ERenderableKind kind    = ERenderableKind::POOL;
        static constexpr const char*     feature = "Light";
        using Ops     = lux::render::LightOperationIds;
        using Desc    = lux::render::PointLightDesc;
        using Handle  = lux::render::RLightHandle;
        using Reply   = lux::render::LightCreatedReply;
        using Require = ComponentList<WorldTransformComponent>;   // position
        using Exclude = ComponentList<>;

        static Desc extract(lux::meta::entity_id e, const PointLightComponent& c, lux::meta::EntityRegistry& reg)
        {
            Desc d{};
            d.position              = reg.get<WorldTransformComponent>(e).world.block<3, 1>(0, 3);
            d.color                 = c.color;
            d.intensity             = c.intensity;
            d.range                 = c.range;
            d.attenuation_constant  = c.attenuation_constant;
            d.attenuation_linear    = c.attenuation_linear;
            d.attenuation_quadratic = c.attenuation_quadratic;
            d.flags                 = c.flags;
            d.shadow_map_size       = c.shadow_map_size;
            d.shadow_bias           = c.shadow_bias;
            d.shadow_normal_bias    = c.shadow_normal_bias;
            return d;
        }
        static auto   create (lux::render::RenderSession& s, const Ops& o, lux::render::RenderSceneId sc, const Desc& d) { return light_detail::create(s, o, sc, d); }
        static void   update (lux::render::RenderSession& s, const Ops& o, lux::render::RenderSceneId sc, Handle h, const Desc& d) { light_detail::update(s, o, sc, h, d); }
        static void   destroy(lux::render::RenderSession& s, const Ops& o, lux::render::RenderSceneId sc, Handle h) { light_detail::destroy(s, o, sc, h); }
        static Handle handle (const Reply& r) { return r.handle; }
    };

    template <>
    struct EcsRenderTraits<SpotLightComponent>
    {
        static constexpr ERenderableKind kind    = ERenderableKind::POOL;
        static constexpr const char*     feature = "Light";
        using Ops     = lux::render::LightOperationIds;
        using Desc    = lux::render::SpotLightDesc;
        using Handle  = lux::render::RLightHandle;
        using Reply   = lux::render::LightCreatedReply;
        using Require = ComponentList<WorldTransformComponent>;   // position
        using Exclude = ComponentList<>;

        static Desc extract(lux::meta::entity_id e, const SpotLightComponent& c, lux::meta::EntityRegistry& reg)
        {
            Desc d{};
            d.position              = reg.get<WorldTransformComponent>(e).world.block<3, 1>(0, 3);
            d.direction             = c.direction;
            d.color                 = c.color;
            d.intensity             = c.intensity;
            d.range                 = c.range;
            d.attenuation_constant  = c.attenuation_constant;
            d.attenuation_linear    = c.attenuation_linear;
            d.attenuation_quadratic = c.attenuation_quadratic;
            d.inner_cone_angle      = c.inner_cone_angle;
            d.outer_cone_angle      = c.outer_cone_angle;
            d.flags                 = c.flags;
            d.shadow_map_size       = c.shadow_map_size;
            d.shadow_bias           = c.shadow_bias;
            d.shadow_normal_bias    = c.shadow_normal_bias;
            return d;
        }
        static auto   create (lux::render::RenderSession& s, const Ops& o, lux::render::RenderSceneId sc, const Desc& d) { return light_detail::create(s, o, sc, d); }
        static void   update (lux::render::RenderSession& s, const Ops& o, lux::render::RenderSceneId sc, Handle h, const Desc& d) { light_detail::update(s, o, sc, h, d); }
        static void   destroy(lux::render::RenderSession& s, const Ops& o, lux::render::RenderSceneId sc, Handle h) { light_detail::destroy(s, o, sc, h); }
        static Handle handle (const Reply& r) { return r.handle; }
    };

} // namespace lux::gameplay
