#include <lux/engine/runtime/packs/spatial2d/Presentation2DContribution.hpp>

#include <lux/engine/ecs/PersistentEntityIndex.hpp>
#include <lux/engine/ecs/ScheduleBuilder.hpp>
#include <lux/engine/ecs/animation/systems/FlipbookAnimationSystem.hpp>
#include <lux/engine/ecs/integration/physics2d_pixel/PixelCollisionProbe.hpp>
#include <lux/engine/ecs/physics/CollisionProbe2D.hpp>
#include <lux/engine/ecs/physics/systems/Simulation2DSystem.hpp>
#include <lux/engine/ecs/pixel/systems/PixelFieldRuntime.hpp>
#include <lux/engine/ecs/pixel/systems/PixelFieldSystem.hpp>
#include <lux/engine/ecs/render/RenderSystemBuilder.hpp>
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
#include <lux/engine/runtime/assets/SceneAssetServices.hpp>
#include <lux/engine/runtime/packs/spatial2d/Simulation2DContribution.hpp>
#include <lux/engine/runtime/packs/spatial2d/Transform2DContribution.hpp>
#include <lux/engine/runtime/packs/scene2d/Flipbook2DResolver.hpp>

#include <memory>
#include <span>
#include <string>
#include <string_view>

namespace lux::runtime
{
    lux::cxx::expected<
        SceneContributionDescriptor,
        lux::ecs::ComponentCatalogFailure>
    makePresentation2DContribution(
        const lux::ecs::ComponentTypeCatalog& components)
    {
        using namespace lux::ecs;
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
            return lux::cxx::unexpected(validated.error());
        }

        SceneContributionDescriptor descriptor;
        descriptor.id = lux::scene::SceneFeatureId{
            std::string{kPresentation2DContributionName}};
        descriptor.display_name = "2D presentation";
        descriptor.required_contributions = {
            lux::scene::SceneFeatureId{
                std::string{kSimulation2DContributionName}},
            lux::scene::SceneFeatureId{
                std::string{kSpatial2DTransformContributionName}}};
        descriptor.required_services = {
            lux::cxx::typeToken<Simulation2DSystem>(),
            lux::cxx::typeToken<Transform2DSystem>(),
            lux::cxx::typeToken<lux::asset_runtime::SceneAssetServices>()};
        descriptor.provided_services = {lux::cxx::typeToken<Camera2DSystem>()};
        descriptor.provider = lux::extensions::ExtensionId{
            "org.lux.builtin"};
        descriptor.build = [](
            SceneContributionBatchBuilder& builder,
            const SceneContributionBuildContext& context,
            ContributionConfig) -> lux::cxx::expected<
                void,
                SceneContributionBuildFailure>
        {
            auto* const assets = builder.findService<
                lux::asset_runtime::SceneAssetServices>(context);
            if (!assets)
            {
                return lux::cxx::unexpected(
                    SceneContributionBuildFailure{
                        ESceneContributionBuildError::MISSING_SERVICE,
                        lux::cxx::typeToken<lux::asset_runtime::
                            SceneAssetServices>()});
            }
            if (auto added = builder.add(
                    std::make_unique<Flipbook2DResolver>(
                        assets->manager,
                        assets->loads),
                    kPhasePreTransform); !added)
            {
                return added;
            }
            if (auto added = builder.add(
                    std::make_unique<FlipbookAnimationSystem>()); !added)
            {
                return added;
            }
            if (auto added = builder.addServiceSystem(
                    std::make_unique<Camera2DSystem>()); !added)
            {
                return added;
            }

            auto* const pixels = builder.findService<PixelFieldRuntime>(
                context);
            auto* const persistent = builder.findService<
                PersistentEntityIndex>(context);
            auto* const simulation = builder.findService<Simulation2DSystem>(
                context);
            auto* const probes = builder.findService<CollisionProbes2D>(
                context);
            if (pixels)
            {
                if (!persistent || !simulation || !probes)
                {
                    return lux::cxx::unexpected(
                        SceneContributionBuildFailure{
                            ESceneContributionBuildError::MISSING_SERVICE});
                }
                if (auto added = builder.add(
                        std::make_unique<PixelFieldSystem>(
                            *pixels,
                            *persistent)); !added)
                {
                    return added;
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
                auto probe = makePixelCollisionProbe(*pixels);
                probes->add(std::move(probe));
            }

            auto* const render = builder.findService<RenderSystemBuilder>(
                context);
            if (!render)
                return {};
            const auto node = [render](auto system)
            {
                return render->add(std::move(system)).has_value();
            };
            if (!node(std::make_unique<Camera2DUploadSubsystem>()) ||
                !node(std::make_unique<Grid2DSubsystem>()))
            {
                return lux::cxx::unexpected(
                    SceneContributionBuildFailure{
                        ESceneContributionBuildError::BUILD_REJECTED});
            }
            auto* const residency = builder.findService<ResidencySubsystem>(
                context);
            if (!residency)
            {
                return lux::cxx::unexpected(
                    SceneContributionBuildFailure{
                        ESceneContributionBuildError::MISSING_SERVICE,
                        lux::cxx::typeToken<ResidencySubsystem>()});
            }
            residency->resolveTextureOf<
                Image2DComponent,
                &Image2DComponent::texture>();
            if (!node(std::make_unique<Image2DSubsystem>()))
            {
                return lux::cxx::unexpected(
                    SceneContributionBuildFailure{
                        ESceneContributionBuildError::BUILD_REJECTED});
            }
            if (pixels && !node(
                    std::make_unique<PixelField2DSubsystem>(pixels)))
            {
                return lux::cxx::unexpected(
                    SceneContributionBuildFailure{
                        ESceneContributionBuildError::BUILD_REJECTED});
            }
            if (auto* tilemaps = builder.findService<TilemapRuntime>(context))
            {
                residency->resolveTextureOf<
                    TilemapComponent,
                    &TilemapComponent::tileset_texture>();
                if (!node(std::make_unique<Tilemap2DSubsystem>(tilemaps)))
                {
                    return lux::cxx::unexpected(
                        SceneContributionBuildFailure{
                            ESceneContributionBuildError::BUILD_REJECTED});
                }
            }
            return {};
        };
        return descriptor;
    }
}
