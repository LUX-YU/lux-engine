#include <lux/engine/ecs/integration/presentation2d/InstallPresentation2DSystems.hpp>

#include <lux/engine/ecs/PersistentEntityIndex.hpp>
#include <lux/engine/ecs/ScheduleBuilder.hpp>
#include <lux/engine/ecs/animation/systems/FlipbookAnimationSystem.hpp>
#include <lux/engine/ecs/integration/physics2d_pixel/PixelCollisionProbe.hpp>
#include <lux/engine/ecs/physics/CollisionProbe2D.hpp>
#include <lux/engine/ecs/physics/systems/Simulation2DSystem.hpp>
#include <lux/engine/ecs/pixel/systems/PixelFieldRuntime.hpp>
#include <lux/engine/ecs/pixel/systems/PixelFieldSystem.hpp>
#include <lux/engine/ecs/render/RenderSystemStages.hpp>
#include <lux/engine/ecs/render/subsystems/2d/Camera2DUploadSubsystem.hpp>
#include <lux/engine/ecs/render/subsystems/2d/Grid2DSubsystem.hpp>
#include <lux/engine/ecs/render/subsystems/2d/Image2DSubsystem.hpp>
#include <lux/engine/ecs/render/subsystems/2d/PixelField2DSubsystem.hpp>
#include <lux/engine/ecs/render/subsystems/2d/Tilemap2DSubsystem.hpp>
#include <lux/engine/ecs/render/subsystems/ResidencySubsystem.hpp>
#include <lux/engine/ecs/render/systems/2d/Camera2DSystem.hpp>
#include <lux/engine/ecs/tilemap/components/TilemapComponent.hpp>
#include <lux/engine/ecs/tilemap/systems/TilemapRuntime.hpp>
#include <lux/engine/ecs/transform/systems/Transform2DSystem.hpp>
#include <lux/engine/resource/asset/AssetServices.hpp>
#include <lux/engine/ecs/animation/systems/FlipbookAssetResolver.hpp>

#include <memory>
#include <string_view>

namespace lux::ecs
{
    bool installPresentation2DSystems(
        ScheduleBuilder& builder,
        const ComponentTypeCatalog& components)
    {
        constexpr std::string_view required_components[]{
            "lux.ecs.flipbookanimationcomponent",
            "lux.ecs.camera2dcomponent",
            "lux.ecs.pixelfield2dcomponent",
            "lux.ecs.image2dcomponent",
            "lux.ecs.ysort2dcomponent",
            "lux.ecs.parallax2dcomponent",
            "lux.ecs.tilemapcomponent",
            "lux.ecs.grid2dcomponent"};
        if (auto validated = validateComponentSchemas(
                components, required_components); !validated)
        {
            return false;
        }

        const auto checkpoint = builder.checkpoint();
        auto* const assets = builder.services().borrow<
            lux::asset::AssetServices>();
        if (!assets ||
            !builder.add(
                std::make_unique<lux::ecs::FlipbookAssetResolver>(
                    assets->manager,
                    assets->loads),
                kPhasePreTransform) ||
            !builder.add(std::make_unique<FlipbookAnimationSystem>()))
        {
            (void)builder.rollbackTo(checkpoint);
            return false;
        }
        auto camera = std::make_unique<Camera2DSystem>();
        auto* const camera_owner = camera.get();
        if (!builder.add(std::move(camera)) ||
            !builder.services().adopt(*camera_owner))
        {
            (void)builder.rollbackTo(checkpoint);
            return false;
        }

        auto* const pixels = builder.services().borrow<PixelFieldRuntime>();
        auto* const persistent = builder.services().borrow<
            PersistentEntityIndex>();
        auto* const simulation = builder.services().borrow<Simulation2DSystem>();
        auto* const probes = builder.services().borrow<CollisionProbes2D>();
        if (pixels)
        {
            if (!persistent || !simulation || !probes ||
                !builder.add(std::make_unique<PixelFieldSystem>(
                    *pixels,
                    *persistent)))
            {
                (void)builder.rollbackTo(checkpoint);
                return false;
            }
            simulation->setPhase(
                Simulation2DSystem::Phase::ApplyFieldCommands,
                [pixels](lux::ecs::Registry&, float)
                {
                    pixels->applyCommands();
                });
            simulation->setPhase(
                Simulation2DSystem::Phase::SimulateFields,
                [pixels](lux::ecs::Registry&, float)
                {
                    pixels->step();
                });
            probes->add(makePixelCollisionProbe(*pixels));
        }

        auto* const render = builder.services().borrow<RenderSystemStages>();
        if (!render)
            return true;
        auto* const residency = builder.services().borrow<ResidencySubsystem>();
        const auto node = [render](auto system)
        {
            return render->add(std::move(system)).has_value();
        };
        if (!residency ||
            !node(std::make_unique<Camera2DUploadSubsystem>()) ||
            !node(std::make_unique<Grid2DSubsystem>()))
        {
            (void)builder.rollbackTo(checkpoint);
            return false;
        }
        residency->resolveTextureOf<
            Image2DComponent,
            &Image2DComponent::texture>();
        if (!node(std::make_unique<Image2DSubsystem>()) ||
            (pixels && !node(std::make_unique<PixelField2DSubsystem>(pixels))))
        {
            (void)builder.rollbackTo(checkpoint);
            return false;
        }
        if (auto* tilemaps = builder.services().borrow<TilemapRuntime>())
        {
            residency->resolveTextureOf<
                TilemapComponent,
                &TilemapComponent::tileset_texture>();
            if (!node(std::make_unique<Tilemap2DSubsystem>(tilemaps)))
            {
                (void)builder.rollbackTo(checkpoint);
                return false;
            }
        }
        return true;
    }
}
