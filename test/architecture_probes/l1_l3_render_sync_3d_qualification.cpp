#include "DeviceRenderFixture.hpp"

#include <lux/engine/function/render/client/genops/ForwardMeshOperation.ops.hpp>
#include <lux/engine/function/render/client/genops/LightOperation.ops.hpp>
#include <lux/engine/function/render/client/genops/MaterialOperation.ops.hpp>
#include <lux/engine/function/render/client/genops/MeshShadowOperation.ops.hpp>
#include <lux/engine/function/render/client/genops/MeshStackOperation.ops.hpp>
#include <lux/engine/function/render/client/genops/RenderClusterOperation.ops.hpp>
#include <lux/engine/function/render/client/genops/ShadowMapOperation.ops.hpp>
#include <lux/engine/function/render/client/genops/ViewCameraOperation.ops.hpp>
#include <lux/engine/function/render/client/resources/material/GraphMaterialData.hpp>
#include <lux/engine/resource/asset/material/MaterialAssets.hpp>
#include <lux/engine/resource/asset/mesh/MeshAsset.hpp>
#include <lux/engine/resource/asset/model/ModelAsset.hpp>
#include <lux/engine/resource/asset/storage/AssetVfs.hpp>
#include <lux/engine/resource/asset/storage/pak/PakArchive.hpp>
#include <lux/engine/resource/asset/storage/pak/PakAssetProvider.hpp>
#include <lux/engine/scene/Builtin3DRenderStages.hpp>
#include <lux/engine/scene/RenderSystem.hpp>
#include <lux/engine/scene/ResolvedMeshResources.hpp>
#include <lux/engine/simulation/ecs/Transform.hpp>
#include <lux/engine/simulation/ecs/Visual.hpp>

#include <Eigen/Geometry>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <span>
#include <thread>
#include <vector>

namespace
{
    inline constexpr int kSkip = 77;

    template <class Asset>
    [[nodiscard]] std::shared_ptr<const Asset> decode(lux::asset::AssetVfs& vfs, lux::asset::AssetId id)
    {
        const auto blob = vfs.open(id);
        if (!blob)
        {
            return {};
        }
        const auto result = lux::asset::TAssetSerDeser<Asset>::decode(
            id,
            blob->bytes,
            lux::asset::AssetDecodeLimits{blob->bytes.size(), 128U * 1024U * 1024U, 16U}
        );
        return result ? *result : std::shared_ptr<const Asset>{};
    }
}

int main()
{
    using namespace lux;
    using namespace lux::asset;
    using namespace lux::render;

    const auto pak = inspectPak(LUX_MODEL_QUALIFICATION_PAK);
    auto provider = PakAssetProvider::loadFromFile(LUX_MODEL_QUALIFICATION_PAK);
    if (!pak || !provider)
    {
        return 2;
    }
    auto vfs = std::make_shared<AssetVfs>();
    if (vfs->mount({"/Game", *provider, 0}) == kInvalidMountId)
    {
        return 3;
    }
    AssetId model_id;
    for (const auto& entry : pak->entries)
    {
        if (entry.magic_number == ModelAsset::primary_magic)
        {
            model_id = entry.id;
        }
    }
    const auto model = decode<ModelAsset>(*vfs, model_id);
    if (!model || model->data().primitives.empty())
    {
        return 4;
    }
    const auto& primitive = model->data().primitives.front();
    const auto mesh_asset = decode<MeshAsset>(*vfs, primitive.mesh);
    const auto material_asset = decode<MaterialAsset>(*vfs, primitive.material);
    if (!mesh_asset || !material_asset)
    {
        return 5;
    }

    std::atomic_int validation_errors{};
    rendertest::DeviceRenderFixture fixture(
        256U,
        256U,
        "l1_l3_render_sync_3d_qualification",
        {.enable_validation = true, .validation_errors = &validation_errors}
    );
    if (!fixture.ok())
    {
        std::puts("SKIP: Vulkan device or validation layer unavailable");
        return kSkip;
    }
    const auto render_scene = fixture.makeSceneWithView("L1L3Scene", "L1L3View");

    const auto compile_shader = [&](const std::vector<std::uint32_t>& spirv, const rdesc::ShaderInfo& info) {
        const auto info_bytes = rdesc::ShaderInfo::serialize(info);
        return fixture.awaitControl(fixture.control().compileShader(
            std::span<const std::byte>{
                reinterpret_cast<const std::byte*>(spirv.data()), spirv.size() * sizeof(std::uint32_t)
            },
            info_bytes
        ));
    };
    const auto gbuffer_shader = compile_shader(
        material_asset->data().gbuffer_spirv,
        material_asset->data().gbuffer_info
    );
    const auto forward_shader = compile_shader(
        material_asset->data().forward_spirv,
        material_asset->data().forward_info
    );
    if (gbuffer_shader.status != 0U || forward_shader.status != 0U)
    {
        return 6;
    }

    const auto camera_registration = fixture.awaitControl(
        fixture.control().registerFeatureType(kViewCameraFeatureFactory)
    );
    fixture.awaitControl(fixture.control().addFeature(
        render_scene.scene_id,
        camera_registration.feature_type_id,
        ViewCameraCommTag{}
    ));
    const auto camera_ops = ViewCameraOperationIds::fromOps(camera_registration.ops, camera_registration.op_count);
    const auto material_registration = fixture.awaitControl(
        fixture.control().registerFeatureType(kMaterialFeatureFactory)
    );
    fixture.awaitControl(fixture.control().addFeature(
        render_scene.scene_id,
        material_registration.feature_type_id,
        MaterialCommTag{}
    ));
    const auto material_ops = MaterialOperationIds::fromOps(
        material_registration.ops,
        material_registration.op_count
    );
    const auto mesh_registration = fixture.awaitControl(
        fixture.control().registerFeatureType(kMeshStackFeatureFactory)
    );
    fixture.awaitControl(fixture.control().addFeature(
        render_scene.scene_id,
        mesh_registration.feature_type_id,
        MeshStackCommTag{}
    ));
    const auto mesh_ops = MeshStackOperationIds::fromOps(mesh_registration.ops, mesh_registration.op_count);
    const auto cluster_registration = fixture.awaitControl(
        fixture.control().registerFeatureType(kRenderClusterFeatureFactory)
    );
    fixture.awaitControl(fixture.control().addFeature(
        render_scene.scene_id,
        cluster_registration.feature_type_id,
        RenderClusterCommTag{}
    ));
    const auto light_registration = fixture.awaitControl(
        fixture.control().registerFeatureType(kLightFeatureFactory)
    );
    fixture.awaitControl(fixture.control().addFeature(
        render_scene.scene_id,
        light_registration.feature_type_id,
        LightCommTag{}
    ));
    const auto light_ops = LightOperationIds::fromOps(light_registration.ops, light_registration.op_count);
    const auto shadow_registration = fixture.awaitControl(
        fixture.control().registerFeatureType(kShadowMapFeatureFactory)
    );
    ShadowMapCommConfig shadow_config{};
    shadow_config.atlas_page_resolution = 256U;
    shadow_config.atlas_page_count = 1U;
    fixture.awaitControl(fixture.control().addFeature(
        render_scene.scene_id,
        shadow_registration.feature_type_id,
        shadow_config
    ));
    const auto mesh_shadow_registration = fixture.awaitControl(
        fixture.control().registerFeatureType(kMeshShadowFeatureFactory)
    );
    fixture.awaitControl(fixture.control().addFeature(
        render_scene.scene_id,
        mesh_shadow_registration.feature_type_id,
        MeshShadowCommConfig{}
    ));
    const auto forward_registration = fixture.awaitControl(
        fixture.control().registerFeatureType(kForwardMeshFeatureFactory)
    );
    ForwardMeshCommConfig forward_config{};
    forward_config.graph_fragment = forward_shader.shader;
    fixture.awaitControl(fixture.control().addFeature(
        render_scene.scene_id,
        forward_registration.feature_type_id,
        forward_config
    ));
    if (!camera_ops.valid() || !material_ops.valid() || !mesh_ops.valid() || !light_ops.valid())
    {
        return 7;
    }

    GraphMaterialData graph_material{};
    graph_material.param_count = material_asset->data().parameter_count;
    for (std::uint32_t parameter = 0U; parameter < graph_material.param_count; ++parameter)
    {
        std::copy_n(
            material_asset->data().parameter_defaults[parameter].begin(),
            4U,
            graph_material.params[parameter]
        );
    }
    auto material_request = uploadGraphMaterial(
        MaterialUploadClient{fixture.uploadClientForTest(), material_ops},
        graph_material,
        gbuffer_shader.shader,
        forward_shader.shader,
        static_cast<std::uint32_t>(material_asset->data().alpha_mode),
        material_asset->data().double_sided
    );
    if (!material_request)
    {
        return 8;
    }
    const auto material = fixture.awaitUpload(std::move(*material_request));
    if (material.status != 0U || material.handle.isNull())
    {
        return 9;
    }
    auto mesh_request = uploadMesh(
        MeshStackUploadClient{fixture.uploadClientForTest(), mesh_ops},
        mesh_asset->data()
    );
    if (!mesh_request)
    {
        return 8;
    }
    const auto mesh = fixture.awaitUpload(std::move(*mesh_request));
    if (mesh.status != 0U || mesh.handle.isNull())
    {
        return 9;
    }

    simulation::ecs::Registry registry;
    const auto mesh_entity = registry.create();
    registry.emplace<simulation::ecs::Mesh3D>(
        mesh_entity,
        simulation::ecs::Mesh3D{rdesc::MeshVisualDescription{mesh_asset->id(), material_asset->id()}}
    );
    registry.emplace<scene::ResolvedMeshResources>(
        mesh_entity,
        scene::ResolvedMeshResources{mesh_asset->id(), material_asset->id(), mesh.handle, material.handle}
    );
    simulation::ecs::WorldTransform3D mesh_transform{};
    mesh_transform.value.translation().z() = -2.0;
    registry.emplace<simulation::ecs::WorldTransform3D>(mesh_entity, mesh_transform);

    const auto light_entity = registry.create();
    simulation::ecs::Light3D light{};
    light.value.type = rdesc::ELightType::DIRECTIONAL;
    light.value.intensity = 3.0F;
    registry.emplace<simulation::ecs::Light3D>(light_entity, light);
    simulation::ecs::WorldTransform3D light_transform{};
    light_transform.value.linear() = Eigen::AngleAxisd{-0.55, Eigen::Vector3d::UnitX()}.toRotationMatrix();
    registry.emplace<simulation::ecs::WorldTransform3D>(light_entity, light_transform);

    auto mesh_stage = scene::createMesh3DRenderStage(scene::Mesh3DRenderStageConfig{
        .registry = &registry,
        .scene = render_scene.scene_id,
        .operations = mesh_ops
    });
    auto light_stage = scene::createLight3DRenderStage(scene::Light3DRenderStageConfig{
        .registry = &registry,
        .scene = render_scene.scene_id,
        .operations = light_ops
    });
    if (!mesh_stage || !light_stage)
    {
        return 10;
    }
    scene::RenderSystem::StageList stages;
    stages.push_back(std::move(*mesh_stage));
    stages.push_back(std::move(*light_stage));
    auto render_system = scene::RenderSystem::create(std::move(stages));
    if (!render_system || (*render_system)->tryPublish() != scene::ERenderPublishResult::FULL_SYNC_PUBLISHED)
    {
        return 11;
    }
    if (!fixture.session().trySubmitFrame() || !fixture.session().waitAndPumpReplies())
    {
        return 12;
    }
    if ((*render_system)->tryForwardUpdate(fixture.session()) != scene::ERenderForwardResult::FORWARDED ||
        !fixture.session().waitAndPumpReplies() || !fixture.session().beginFrame())
    {
        return 13;
    }

    const float view[16] = {
        1.0F, 0.0F, 0.0F, 0.0F,
        0.0F, 1.0F, 0.0F, 0.0F,
        0.0F, 0.0F, 1.0F, 0.0F,
        0.0F, 0.0F, 0.0F, 1.0F
    };
    const float projection[16] = {
        1.0F, 0.0F, 0.0F, 0.0F,
        0.0F, -1.0F, 0.0F, 0.0F,
        0.0F, 0.0F, -1.001001F, -1.0F,
        0.0F, 0.0F, -0.1001001F, 0.0F
    };
    const float eye[3] = {0.0F, 0.0F, 0.0F};
    viewCameraUpdateTransient(
        ViewCameraProxy{fixture.session(), camera_ops},
        render_scene.scene_id,
        render_scene.view,
        view,
        projection,
        eye
    );
    if (!fixture.session().trySubmitFrame() || !fixture.session().waitAndPumpReplies())
    {
        return 14;
    }
    for (std::uint32_t step = 0U; step < 8U; ++step)
    {
        registry.patch<simulation::ecs::WorldTransform3D>(mesh_entity, [step](auto& transform) {
            transform.value.linear() = Eigen::AngleAxisd{
                static_cast<double>(step + 1U) * 0.05,
                Eigen::Vector3d::UnitY()
            }.toRotationMatrix();
            transform.value.translation().z() = -2.0;
        });
        if ((*render_system)->tryPublish() != scene::ERenderPublishResult::PUBLISHED ||
            (*render_system)->tryForwardUpdate(fixture.session()) != scene::ERenderForwardResult::FORWARDED ||
            !fixture.session().waitAndPumpReplies() || !fixture.session().beginFrame() ||
            !fixture.session().trySubmitFrame() || !fixture.session().waitAndPumpReplies())
        {
            return 15;
        }
    }
    if (!fixture.session().beginFrame())
    {
        return 16;
    }
    fixture.flush(8U);
    const auto pixels = fixture.readback(render_scene);
    if (fixture.lastReadback().status != 0U)
    {
        return 16;
    }
    std::size_t lit_pixels{};
    for (std::size_t offset = 0U; offset + 3U < pixels.size(); offset += 4U)
    {
        if ((pixels[offset] | pixels[offset + 1U] | pixels[offset + 2U]) != 0U)
        {
            ++lit_pixels;
        }
    }
    std::printf(
        "lit_pixels=%zu,mesh_handle=%u:%u,material_handle=%u:%u,validation_errors=%d\n",
        lit_pixels,
        mesh.handle.index,
        mesh.handle.gen,
        material.handle.index,
        material.handle.gen,
        validation_errors.load()
    );
    return lit_pixels >= 64U && validation_errors.load() == 0 ? 0 : 17;
}
