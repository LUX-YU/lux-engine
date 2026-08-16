// ============================================================================
//  Built-in SpawnRecipes (spawn actions expressed as data, not imperative
//  code): the Create menu's built-in entries, seeded into the SpawnRegistry
//  at editor init.
//  Lives beside the registry it feeds — the recipes are REGISTRY DATA, not
//  editor-shell behavior; packs/plugins append theirs through the same add().
//
//  Spawns mutate the live World directly, exactly like the Inspector's Add
//  Component (panel paints run between World ticks); the next tick's Transform
//  system resolves the hierarchy and the render bridges pick up any
//  renderable.
// ============================================================================

#include <lux/engine/editor/app/SpawnRegistry.hpp>

#include <lux/engine/ecs/World.hpp>
#include <lux/engine/ecs/components/NameComponent.hpp>
#include <lux/engine/ecs/components/Transform2DComponent.hpp>
#include <lux/engine/ecs/components/Transform3DComponent.hpp>
#include <lux/engine/ecs/components/ResolvedTransform3DComponent.hpp>
#include <lux/engine/ecs/render/components/2d/Image2DComponent.hpp>
#include <lux/engine/ecs/render/components/2d/Camera2DComponent.hpp>
#include <lux/engine/ecs/render/components/3d/Camera3DComponent.hpp>
#include <lux/engine/ecs/render/components/3d/DirectionalLightComponent.hpp>
#include <lux/engine/ecs/render/components/3d/PointLightComponent.hpp>

namespace lux::editor
{
    void registerBuiltinSpawnRecipes(SpawnRegistry& registry)
    {

        const auto place2d = [](lux::meta::EntityRegistryBase& reg,
                                entt::entity entity,
                                const SpawnContext& ctx)
        {
            auto& transform = reg.emplace<lux::ecs::Transform2DComponent>(
                entity);
            if (ctx.pos2d)
                transform.position = *ctx.pos2d;
        };
        const auto place3d = [](lux::meta::EntityRegistryBase& reg,
                                entt::entity entity,
                                const SpawnContext& ctx)
        {
            auto& transform = reg.emplace<lux::ecs::Transform3DComponent>(
                entity);
            if (ctx.pos3d)
                transform.position = *ctx.pos3d;
        };
        // Empty is genuinely dimension-neutral. A Transform is added only by
        // a dimension-specific recipe or an explicit Add Component action.
        registry.add({ .id = "empty", .label = "Empty", .category = "",
            .spawn = [](const SpawnContext& ctx)
            {
                auto& reg = ctx.world.registry();
                const auto e = reg.create();
                reg.emplace<lux::ecs::NameComponent>(e, lux::ecs::NameComponent{"Entity"});
                return e;
            }});

        // 2D ▸ Image — flat white 1×1 quad until a texture is assigned.
        registry.add({ .id = "image2d", .label = "Image", .category = "2D",
            .required_contribution = "org.lux.builtin.presentation2d",
            .spawn = [place2d](const SpawnContext& ctx)
            {
                auto& reg = ctx.world.registry();
                const auto e = reg.create();
                reg.emplace<lux::ecs::NameComponent>(e, lux::ecs::NameComponent{"Image"});
                place2d(reg, e, ctx);
                reg.emplace<lux::ecs::Image2DComponent>(e);
                return e;
            }});

        // NO auto-tagging (§4.4 ruling): spawning a camera does NOT make it
        // the active one. The user activates a camera explicitly (Hierarchy >
        // Set as Active Camera); a scene without one plays BLACK — that is
        // the correct minimal-prototype behavior, not a gap the editor
        // papers over by deciding for the user.

        // 2D ▸ Camera — a SCENE camera (unbound; inert in-editor by design —
        // the editor camera keeps the viewport; scene cameras matter at runtime).
        registry.add({ .id = "camera2d", .label = "Camera", .category = "2D",
            .required_contribution = "org.lux.builtin.presentation2d",
            .spawn = [place2d](const SpawnContext& ctx)
            {
                auto& reg = ctx.world.registry();
                const auto e = reg.create();
                reg.emplace<lux::ecs::NameComponent>(e, lux::ecs::NameComponent{"Camera"});
                place2d(reg, e, ctx);
                reg.emplace<lux::ecs::Camera2DComponent>(e);
                return e;
            }});

        // 3D ▸ Camera — likewise a scene camera, unbound in-editor.
        registry.add({ .id = "camera3d", .label = "Camera", .category = "3D",
            .required_contribution = "org.lux.builtin.presentation3d",
            .spawn = [place3d](const SpawnContext& ctx)
            {
                auto& reg = ctx.world.registry();
                const auto e = reg.create();
                reg.emplace<lux::ecs::NameComponent>(e, lux::ecs::NameComponent{"Camera"});
                place3d(reg, e, ctx);
                reg.emplace<lux::ecs::ResolvedTransform3DComponent>(e);
                reg.emplace<lux::ecs::Camera3DComponent>(e);
                return e;
            }});

        // 3D ▸ lights.
        registry.add({ .id = "dirlight3d", .label = "Directional Light",
            .category = "3D",
            .required_contribution = "org.lux.builtin.presentation3d",
            .spawn = [place3d](const SpawnContext& ctx)
            {
                auto& reg = ctx.world.registry();
                const auto e = reg.create();
                reg.emplace<lux::ecs::NameComponent>(e, lux::ecs::NameComponent{"Directional Light"});
                place3d(reg, e, ctx);
                reg.emplace<lux::ecs::ResolvedTransform3DComponent>(e);
                reg.emplace<lux::ecs::DirectionalLightComponent>(e);
                return e;
            }});
        registry.add({ .id = "pointlight3d", .label = "Point Light",
            .category = "3D",
            .required_contribution = "org.lux.builtin.presentation3d",
            .spawn = [place3d](const SpawnContext& ctx)
            {
                auto& reg = ctx.world.registry();
                const auto e = reg.create();
                reg.emplace<lux::ecs::NameComponent>(e, lux::ecs::NameComponent{"Point Light"});
                place3d(reg, e, ctx);
                reg.emplace<lux::ecs::ResolvedTransform3DComponent>(e);
                reg.emplace<lux::ecs::PointLightComponent>(e);
                return e;
            }});
    }

} // namespace lux::editor
