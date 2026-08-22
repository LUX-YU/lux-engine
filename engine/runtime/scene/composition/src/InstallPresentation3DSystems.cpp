#include <lux/engine/runtime/scene/composition/InstallPresentation3DSystems.hpp>

#include <lux/engine/ecs/ScheduleBuilder.hpp>
#include <lux/engine/ecs/render/RenderSystemStages.hpp>
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
#include <lux/engine/runtime/render/scene/ClassicMeshBatchRenderSubsystem.hpp>
#include <lux/engine/runtime/render/scene/TerrainTileRenderSubsystem.hpp>
#include <lux/engine/runtime/scene/SceneAsyncContext.hpp>

#include <memory>
#include <string_view>

namespace lux::runtime
{
    bool installPresentation3DSystems(
        lux::ecs::ScheduleBuilder& builder,
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
            return false;
        }

        const auto checkpoint = builder.checkpoint();
        auto* const assets = builder.services().borrow<
            lux::asset::AssetServices>();
        auto* const blobs = builder.services().borrow<
            lux::ecs::entity_scene::ContentBlobClient>();
        auto* const async = builder.services().borrow<SceneAsyncContext>();
        auto* const render = builder.services().borrow<RenderSystemStages>();
        auto* const residency = builder.services().borrow<ResidencySubsystem>();
        auto* const residency_callbacks = builder.services().borrow<
            ResidencyCallbacks>();
        if (!assets || !blobs || !async || !render || !residency ||
            !residency_callbacks || !classic_mesh_preparation ||
            !terrain_preparation ||
            !builder.add(std::make_unique<Camera3DSystem>()))
        {
            (void)builder.rollbackTo(checkpoint);
            return false;
        }

        const auto node = [render](auto system)
        {
            return render->add(std::move(system)).has_value();
        };
        if (!node(std::make_unique<SpatialCullSubsystem>()) ||
            !node(std::make_unique<Grid3DSubsystem>()) ||
            !node(std::make_unique<DirectionalLightSubsystem>()) ||
            !node(std::make_unique<PointLightSubsystem>()) ||
            !node(std::make_unique<SpotLightSubsystem>()) ||
            !node(std::make_unique<Camera3DUploadSubsystem>()))
        {
            (void)builder.rollbackTo(checkpoint);
            return false;
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
            (void)builder.rollbackTo(checkpoint);
            return false;
        }
        return true;
    }
}
