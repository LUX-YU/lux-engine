#include <lux/engine/runtime/packs/spatial3d/Presentation3DContribution.hpp>

#include <lux/engine/ecs/ScheduleBuilder.hpp>
#include <lux/engine/ecs/render/RenderSystemBuilder.hpp>
#include <lux/engine/ecs/render/subsystems/ResidencySubsystem.hpp>
#include <lux/engine/ecs/render/subsystems/3d/Camera3DUploadSubsystem.hpp>
#include <lux/engine/ecs/render/subsystems/3d/HeightFogSubsystem.hpp>
#include <lux/engine/ecs/render/subsystems/3d/LightSubsystems.hpp>
#include <lux/engine/ecs/render/subsystems/3d/MeshSubsystems.hpp>
#include <lux/engine/ecs/render/subsystems/3d/SkyboxSubsystem.hpp>
#include <lux/engine/ecs/render/subsystems/3d/SpatialCullSubsystem.hpp>
#include <lux/engine/ecs/render/subsystems/3d/WaterSurfaceSubsystem.hpp>
#include <lux/engine/ecs/render/systems/3d/Camera3DSystem.hpp>
#include <lux/engine/runtime/assets/SceneAssetServices.hpp>
#include <lux/engine/runtime/entity_scene/SectionBlobStore.hpp>
#include <lux/engine/runtime/render/scene/ClassicMeshBatchRenderSubsystem.hpp>
#include <lux/engine/runtime/render/scene/TerrainTileRenderSubsystem.hpp>
#include <lux/engine/runtime/scene/SceneAsyncContext.hpp>
#include <lux/engine/runtime/spatial3d/transform/Spatial3DTransformContribution.hpp>

#include <memory>
#include <span>
#include <string>
#include <string_view>

namespace lux::runtime
{
    lux::cxx::expected<
        SceneContributionDescriptor,
        lux::ecs::ComponentCatalogFailure>
    makePresentation3DContribution(
        const lux::ecs::ComponentTypeCatalog& components,
        ClassicMeshPrepareClient classic_mesh_preparation,
        TerrainPrepareClient terrain_preparation)
    {
        using namespace lux::ecs;
        constexpr std::string_view required_components[]{
            "lux.ecs.camera3dcomponent",
            "lux.ecs.grid3dcomponent",
            "lux.ecs.directionallightcomponent",
            "lux.ecs.pointlightcomponent",
            "lux.ecs.spotlightcomponent",
            "lux.ecs.scenesettingscomponent",
            "lux.ecs.heightfogcomponent",
            "lux.ecs.skyboxcomponent",
            "lux.ecs.watersurfacecomponent",
            "lux.ecs.meshcomponent",
            "lux.ecs.skeletalmeshcomponent",
            "lux.ecs.classicmeshbatchcomponent",
            "lux.ecs.visuallodnodecomponent",
            "lux.ecs.visuallodparentcomponent",
            "lux.ecs.terraintilecomponent",
            "lux.ecs.terrainlodnodecomponent",
            "lux.ecs.terrainlodparentcomponent"};
        if (auto validated = validateComponentSchemas(
                components, required_components); !validated)
        {
            return lux::cxx::unexpected(validated.error());
        }

        SceneContributionDescriptor descriptor;
        descriptor.id = lux::scene::SceneFeatureId{
            std::string{kPresentation3DContributionName}};
        descriptor.display_name = "3D presentation";
        descriptor.required_contributions.emplace_back(
            std::string{kSpatial3DTransformContributionName});
        descriptor.required_services = {
            lux::cxx::typeToken<lux::asset_runtime::SceneAssetServices>(),
            lux::cxx::typeToken<SceneAsyncContext>(),
            lux::cxx::typeToken<entity_scene::ContentBlobClient>(),
            lux::cxx::typeToken<RenderSystemBuilder>(),
            lux::cxx::typeToken<ResidencySubsystem>()};
        descriptor.provider = lux::extensions::ExtensionId{
            "org.lux.builtin"};
        descriptor.build = [classic_mesh_preparation,
                            terrain_preparation](
            SceneContributionBatchBuilder& builder,
            const SceneContributionBuildContext& context,
            ContributionConfig) -> lux::cxx::expected<
                void,
                SceneContributionBuildFailure>
        {
            auto* const assets = builder.findService<
                lux::asset_runtime::SceneAssetServices>(context);
            auto* const blobs = builder.findService<
                entity_scene::ContentBlobClient>(context);
            auto* const async = builder.findService<SceneAsyncContext>(
                context);
            auto* const render = builder.findService<RenderSystemBuilder>(
                context);
            auto* const residency = builder.findService<ResidencySubsystem>(
                context);
            auto* const residency_callbacks = builder.findService<
                ResidencyCallbacks>(context);
            if (!assets || !blobs || !async || !render || !residency ||
                !residency_callbacks || !classic_mesh_preparation ||
                !terrain_preparation)
            {
                return lux::cxx::unexpected(
                    SceneContributionBuildFailure{
                        ESceneContributionBuildError::MISSING_SERVICE});
            }

            if (auto added = builder.add(
                    std::make_unique<Camera3DSystem>()); !added)
            {
                return added;
            }
            const auto node = [render](auto system)
            {
                return render->add(std::move(system)).has_value();
            };
            if (!node(std::make_unique<SpatialCullSubsystem>()) ||
                !node(std::make_unique<DirectionalLightSubsystem>()) ||
                !node(std::make_unique<PointLightSubsystem>()) ||
                !node(std::make_unique<SpotLightSubsystem>()) ||
                !node(std::make_unique<Camera3DUploadSubsystem>()))
            {
                return lux::cxx::unexpected(
                    SceneContributionBuildFailure{
                        ESceneContributionBuildError::BUILD_REJECTED});
            }

            residency->resolveTextureOf<
                SkyboxComponent,
                &SkyboxComponent::equirect_texture_id>();
            residency->resolveTextureOf<
                WaterSurfaceComponent,
                &WaterSurfaceComponent::normal_texture>();
            residency->resolveMeshOf<
                MeshComponent,
                &MeshComponent::mesh_asset_id,
                &MeshComponent::material_asset_id>();
            residency->resolveMeshOf<
                SkeletalMeshComponent,
                &SkeletalMeshComponent::mesh_asset_id,
                &SkeletalMeshComponent::material_asset_id>();
            if (!node(std::make_unique<SkyboxSubsystem>()) ||
                !node(std::make_unique<HeightFogSubsystem>()) ||
                !node(std::make_unique<WaterSurfaceSubsystem>()) ||
                !node(std::make_unique<MeshSubsystem>()) ||
                !node(std::make_unique<SkeletalMeshSubsystem>()) ||
                !node(std::make_unique<ClassicMeshBatchRenderSubsystem>(
                    *blobs,
                    *residency_callbacks,
                    assets->manager,
                    classic_mesh_preparation,
                    *async)) ||
                !node(std::make_unique<TerrainTileRenderSubsystem>(
                    *blobs,
                    terrain_preparation,
                    *async)))
            {
                return lux::cxx::unexpected(
                    SceneContributionBuildFailure{
                        ESceneContributionBuildError::BUILD_REJECTED});
            }
            return {};
        };
        return descriptor;
    }
}
