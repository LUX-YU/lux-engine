#include <lux/engine/ecs/integration/presentation2d/InstallPresentation2DSystems.hpp>

#include <lux/engine/ecs/PersistentEntityIndex.hpp>
#include <lux/engine/ecs/ScheduleBuilder.hpp>
#include <lux/engine/ecs/animation/systems/FlipbookAnimationSystem.hpp>
#include <lux/engine/ecs/integration/physics2d_pixel/PixelCollisionProbe.hpp>
#include <lux/engine/ecs/physics/CollisionProbe2D.hpp>
#include <lux/engine/ecs/physics/systems/Simulation2DSystem.hpp>
#include <lux/engine/ecs/pixel/systems/PixelFieldRuntime.hpp>
#include <lux/engine/ecs/pixel/systems/PixelFieldSystem.hpp>
#include <lux/engine/ecs/render/RenderStage.hpp>
#include <lux/engine/ecs/render/SceneRenderBinding.hpp>
#include <lux/engine/ecs/render/subsystems/2d/Camera2DUploadSubsystem.hpp>
#include <lux/engine/ecs/render/subsystems/2d/Grid2DSubsystem.hpp>
#include <lux/engine/ecs/render/subsystems/2d/Image2DSubsystem.hpp>
#include <lux/engine/ecs/render/systems/2d/PixelField2DSystem.hpp>
#include <lux/engine/ecs/render/systems/2d/Tilemap2DSystem.hpp>
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

        return true;
    }

    bool installPresentation2DRendering(
        ScheduleBuilder& builder,
        const ComponentTypeCatalog& components,
        std::vector<std::unique_ptr<RenderStage>>& stages,
        std::vector<std::string_view>& feature_roots,
        ResidencySubsystem& residency)
    {
        constexpr std::string_view required_components[]{
            "lux.ecs.camera2dcomponent",
            "lux.ecs.pixelfield2dcomponent",
            "lux.ecs.image2dcomponent",
            "lux.ecs.tilemapcomponent",
            "lux.ecs.grid2dcomponent"};
        if (auto validated = validateComponentSchemas(
                components, required_components); !validated)
        {
            return false;
        }

        auto* const render = builder.services().borrow<SceneRenderBinding>();
        auto* const active_view = builder.services().borrow<ActiveRenderView>();
        if (!render || !active_view)
            return false;

        stages.push_back(std::make_unique<Camera2DUploadSubsystem>());
        stages.push_back(std::make_unique<Grid2DSubsystem>());
        residency.resolveTextureOf<
            Image2DComponent,
            &Image2DComponent::texture>();
        const auto image_requirements =
            Image2DSubsystem::requiredRenderFeatures();
        feature_roots.insert(
            feature_roots.end(),
            image_requirements.begin(),
            image_requirements.end());
        if (!builder.add(
                std::make_unique<Image2DSubsystem>(*render, *active_view),
                kPhaseRender))
        {
            return false;
        }
        auto* const pixels = builder.services().borrow<PixelFieldRuntime>();
        if (pixels)
        {
            const auto requirements =
                PixelField2DSystem::requiredRenderFeatures();
            feature_roots.insert(
                feature_roots.end(),
                requirements.begin(),
                requirements.end());
            if (!builder.add(
                    std::make_unique<PixelField2DSystem>(*render, pixels),
                    kPhaseRender))
            {
                return false;
            }
        }
        if (auto* tilemaps = builder.services().borrow<TilemapRuntime>())
        {
            residency.resolveTextureOf<
                TilemapComponent,
                &TilemapComponent::tileset_texture>();
            const auto requirements =
                Tilemap2DSystem::requiredRenderFeatures();
            feature_roots.insert(
                feature_roots.end(),
                requirements.begin(),
                requirements.end());
            if (!builder.add(
                    std::make_unique<Tilemap2DSystem>(*render, tilemaps),
                    kPhaseRender))
            {
                return false;
            }
        }
        return true;
    }
}
