#pragma once

#include <lux/engine/ecs/render/components/3d/WaterSurfaceComponent.hpp>
#include <lux/engine/ecs/render/VisualTransition.hpp>
#include <lux/engine/ecs/components/ResolvedTransform3DComponent.hpp>
#include <lux/engine/ecs/render/components/TextureGpuCacheComponent.hpp>
#include <lux/engine/ecs/render/RenderSpatialTransform.hpp>
#include <lux/engine/ecs/render/RenderViewUtil.hpp>
#include <lux/engine/ecs/render/subsystems/PooledSlotSubsystem.hpp>

#include <lux/engine/function/render/client/genops/WaterOperation.ops.hpp>

#include <algorithm>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <optional>

namespace lux::ecs
{
    struct WaterSurfaceRenderPolicy final
    {
        using Component = WaterSurfaceComponent;
        static constexpr const char* feature = "Water";
        using Ops = lux::render::WaterOperationIds;
        using Desc = lux::render::WaterSurfaceDesc;
        using Handle = lux::render::RWaterSurfaceHandle;
        using Reply = lux::render::WaterSurfaceCreatedReply;
        using Require = ComponentList<ResolvedTransform3DComponent>;
        using Exclude = ComponentList<>;

        static std::optional<Desc> extract(
            lux::ecs::Entity entity,
            const Component& component,
            lux::ecs::Registry& registry,
            SceneRenderBinding& render)
        {
            const auto transform = makeRenderSpatialTransform(
                registry.get<ResolvedTransform3DComponent>(entity),
                render.sceneOriginTile3D());
            if (!transform)
            {
                render.requestSceneOriginRebase(
                    registry.get<ResolvedTransform3DComponent>(entity).
                        position);
                return std::nullopt;
            }

            Desc result{};
            result.transform = *transform;
            result.half_extent[0] = std::max(
                component.half_extent.x(), 0.01f);
            result.half_extent[1] = std::max(
                component.half_extent.y(), 0.01f);
            result.normal_scroll_a[0] = component.normal_scroll_a.x();
            result.normal_scroll_a[1] = component.normal_scroll_a.y();
            result.normal_scroll_b[0] = component.normal_scroll_b.x();
            result.normal_scroll_b[1] = component.normal_scroll_b.y();
            result.absorption_color[0] = std::clamp(
                component.absorption_color.x(), 0.0f, 1.0f);
            result.absorption_color[1] = std::clamp(
                component.absorption_color.y(), 0.0f, 1.0f);
            result.absorption_color[2] = std::clamp(
                component.absorption_color.z(), 0.0f, 1.0f);
            result.absorption_distance = std::max(
                component.absorption_distance, 0.001f);
            result.roughness = std::clamp(
                component.roughness, 0.0f, 1.0f);
            result.normal_strength = std::max(
                component.normal_strength, 0.0f);
            result.wave_scale = std::max(component.wave_scale, 0.0001f);
            if (const auto* texture =
                    registry.try_get<TextureGpuCacheComponent>(entity))
            {
                if (texture->source == component.normal_texture)
                    result.normal_texture = texture->handle;
            }

            const auto transition = visualTransitionOf(registry, entity);
            result.transition_milliseconds =
                transition.duration_milliseconds;
            result.transition_seed = transition.seed;
            return result;
        }

        static auto create(
            lux::render::RenderFrameSession& session,
            const Ops& ops,
            lux::render::RenderSceneId scene,
            const Desc& surface)
        {
            return lux::render::WaterProxy(session, ops).createSurface(
                {scene, surface});
        }

        static void update(
            lux::render::RenderFrameSession& session,
            const Ops& ops,
            lux::render::RenderSceneId scene,
            Handle handle,
            const Desc& surface)
        {
            lux::render::WaterProxy(session, ops).updateSurface(
                {scene, handle, surface});
        }

        static void destroy(
            lux::render::RenderFrameSession& session,
            const Ops& ops,
            lux::render::RenderSceneId scene,
            Handle handle)
        {
            lux::render::WaterProxy(session, ops).destroySurface(
                {scene, handle});
        }

        static Handle handle(const Reply& reply)
        {
            return reply.handle;
        }
    };

    using WaterSurfaceSubsystem =
        PooledSlotSubsystem<WaterSurfaceRenderPolicy>;
} // namespace lux::ecs
