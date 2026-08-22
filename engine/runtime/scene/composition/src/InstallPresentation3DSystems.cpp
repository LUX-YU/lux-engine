#include <lux/engine/runtime/scene/composition/InstallPresentation3DSystems.hpp>

#include <lux/engine/ecs/ScheduleBuilder.hpp>
#include <lux/engine/ecs/render/RenderStage.hpp>
#include <lux/engine/ecs/render/subsystems/ResidencySubsystem.hpp>
#include <lux/engine/ecs/render/subsystems/3d/Camera3DUploadSubsystem.hpp>
#include <lux/engine/ecs/render/subsystems/3d/HeightFogSubsystem.hpp>
#include <lux/engine/ecs/render/subsystems/3d/Grid3DSubsystem.hpp>
#include <lux/engine/ecs/render/subsystems/3d/LightSubsystems.hpp>
#include <lux/engine/ecs/render/subsystems/3d/MeshSubsystems.hpp>
#include <lux/engine/ecs/render/subsystems/3d/SkyboxSubsystem.hpp>
#include <lux/engine/ecs/render/subsystems/3d/SpatialCullSubsystem.hpp>
#include <lux/engine/ecs/render/subsystems/3d/WaterSurfaceSubsystem.hpp>
#include <lux/engine/ecs/render/systems/3d/Camera3DSystem.hpp>
#include <lux/engine/resource/asset/AssetServices.hpp>
#include <lux/engine/runtime/entity_scene/SectionBlobStore.hpp>
#include <lux/engine/ecs/render/systems/3d/ClassicMeshBatchRenderSystem.hpp>
#include <lux/engine/ecs/render/systems/3d/TerrainTileRenderSystem.hpp>
#include <lux/engine/ecs/render/SceneRenderBinding.hpp>
#include <lux/engine/ecs/SystemPhase.hpp>

#include <memory>
#include <string_view>
#include <type_traits>

namespace lux::runtime
{
    bool installPresentation3DSystems(
        lux::ecs::ScheduleBuilder& builder,
        const lux::ecs::ComponentTypeCatalog& components)
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
            return false;
        }

        const auto checkpoint = builder.checkpoint();
        if (!builder.add(std::make_unique<Camera3DSystem>()))
        {
            (void)builder.rollbackTo(checkpoint);
            return false;
        }
        return true;
    }

    bool installPresentation3DRendering(
        lux::ecs::ScheduleBuilder& builder,
        const lux::ecs::ComponentTypeCatalog& components,
        lux::ecs::ClassicMeshPreparePort classic_mesh_preparation,
        lux::ecs::TerrainPreparePort terrain_preparation,
        std::vector<std::unique_ptr<lux::ecs::RenderStage>>& stages,
        std::vector<std::string_view>& feature_roots,
        lux::ecs::ResidencySubsystem& residency)
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
            return false;
        }

        auto* const assets = builder.services().borrow<
            lux::asset::AssetServices>();
        auto* const blobs = builder.services().borrow<
            lux::ecs::entity_scene::ContentBlobClient>();
        auto* const residency_callbacks = builder.services().borrow<
            ResidencyCallbacks>();
        auto* const render = builder.services().borrow<SceneRenderBinding>();
        auto* const active_view = builder.services().borrow<ActiveRenderView>();
        if (!assets || !blobs || !residency_callbacks || !render ||
            !active_view || !classic_mesh_preparation || !terrain_preparation)
        {
            return false;
        }

        const auto node = [&stages](auto stage)
        {
            stages.push_back(std::move(stage));
            return true;
        };
        if (!node(std::make_unique<SpatialCullSubsystem>()) ||
            !node(std::make_unique<Grid3DSubsystem>()) ||
            !node(std::make_unique<Camera3DUploadSubsystem>()))
        {
            return false;
        }

        const auto render_system =
            [&builder, &feature_roots](auto system)
            {
                using System = typename decltype(system)::element_type;
                const auto requirements = System::requiredRenderFeatures();
                feature_roots.insert(
                    feature_roots.end(),
                    requirements.begin(),
                    requirements.end());
                return builder.add(std::move(system), kPhaseRender);
            };
        if (!render_system(std::make_unique<DirectionalLightSubsystem>(
                *render, *active_view)) ||
            !render_system(std::make_unique<PointLightSubsystem>(
                *render, *active_view)) ||
            !render_system(std::make_unique<SpotLightSubsystem>(
                *render, *active_view)))
        {
            return false;
        }

        residency.resolveTextureOf<
            SkyboxComponent,
            &SkyboxComponent::equirect_texture_id>();
        residency.resolveTextureOf<
            WaterSurfaceComponent,
            &WaterSurfaceComponent::normal_texture>();
        residency.resolveMeshOf<
            MeshComponent,
            &MeshComponent::mesh_asset_id,
            &MeshComponent::material_asset_id>();
        residency.resolveMeshOf<
            SkeletalMeshComponent,
            &SkeletalMeshComponent::mesh_asset_id,
            &SkeletalMeshComponent::material_asset_id>();
        if (!node(std::make_unique<SkyboxSubsystem>()) ||
            !node(std::make_unique<HeightFogSubsystem>()))
        {
            return false;
        }
        if (!render_system(std::make_unique<WaterSurfaceSubsystem>(
                *render, *active_view)) ||
            !render_system(std::make_unique<MeshSubsystem>(
                *render, *active_view)) ||
            !render_system(std::make_unique<SkeletalMeshSubsystem>(
                *render, *active_view)))
        {
            return false;
        }

        const auto classic_roots =
            ClassicMeshBatchRenderSystem::requiredRenderFeatures();
        feature_roots.insert(
            feature_roots.end(), classic_roots.begin(), classic_roots.end());
        if (!builder.add(
                std::make_unique<ClassicMeshBatchRenderSystem>(
                *render,
                *blobs,
                *residency_callbacks,
                assets->manager,
                classic_mesh_preparation),
                kPhaseRender))
        {
            return false;
        }

        const auto terrain_roots =
            TerrainTileRenderSystem::requiredRenderFeatures();
        feature_roots.insert(
            feature_roots.end(), terrain_roots.begin(), terrain_roots.end());
        if (!builder.add(
                std::make_unique<TerrainTileRenderSystem>(
                *render,
                *blobs,
                terrain_preparation),
                kPhaseRender))
        {
            return false;
        }
        return true;
    }
}
