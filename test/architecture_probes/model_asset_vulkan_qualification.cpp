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
#include <lux/engine/resource/asset/texture/TextureAsset.hpp>
#include <lux/engine/scene/RenderSystem.hpp>
#include <lux/engine/scene/Builtin3DRenderStages.hpp>
#include <lux/engine/scene/ResolvedMeshResources.hpp>
#include <lux/engine/simulation/ecs/Transform.hpp>
#include <lux/engine/simulation/ecs/Visual.hpp>

#include <Eigen/Core>
#include <Eigen/Geometry>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <memory>
#include <numeric>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace
{
    inline constexpr int kSkip = 77;

    template <class Asset>
    [[nodiscard]] std::shared_ptr<const Asset> decode(
        lux::asset::AssetVfs& vfs,
        lux::asset::AssetId id
    )
    {
        const auto blob = vfs.open(id);
        if (!blob) return {};
        const auto asset = lux::asset::TAssetSerDeser<Asset>::decode(
            id,
            blob->bytes,
            lux::asset::AssetDecodeLimits{
                blob->bytes.size(),
                128U * 1024U * 1024U,
                16U
            }
        );
        return asset ? *asset : std::shared_ptr<const Asset>{};
    }
}

int main(int argc, char** argv)
{
    using namespace lux;
    using namespace lux::asset;
    using namespace lux::render;
    const bool require_gpu = argc == 2 && std::string_view{argv[1]} == "--require-gpu";

    const auto pak = inspectPak(LUX_MODEL_QUALIFICATION_PAK);
    auto provider = PakAssetProvider::loadFromFile(LUX_MODEL_QUALIFICATION_PAK);
    if (!pak || !provider) return 2;
    auto vfs = std::make_shared<AssetVfs>();
    if (vfs->mount({"/Game", *provider, 0}) == kInvalidMountId) return 3;
    AssetId model_id;
    for (const auto& entry : pak->entries)
        if (entry.magic_number == ModelAsset::primary_magic) model_id = entry.id;
    if (model_id.isNull()) return 4;
    const auto model = decode<ModelAsset>(*vfs, model_id);
    if (!model || model->data().primitives.empty()) return 5;
    const auto& primitive = model->data().primitives.front();
    const auto mesh_asset = decode<MeshAsset>(*vfs, primitive.mesh);
    const auto material_asset = decode<MaterialAsset>(*vfs, primitive.material);
    if (!mesh_asset || !material_asset) return 6;

    std::atomic_int validation_errors{};
    std::uint32_t lit_pixels{};
    double luminance_variance{};
    double full_sync_ms{};
    double initial_state_apply_ms{};
    double state_update_mean_ms{};
    double frame_mean_ms{};
    double frame_p95_ms{};
    double gpu_frame_mean_ms{};
    double gpu_frame_p95_ms{};
    std::size_t gpu_timing_samples{};
    std::uint32_t server_alive_instances{};
    std::uint32_t server_alive_lights{};
    std::size_t rendered_mesh_vertices{mesh_asset->data().vertices.size()};
    std::string gpu_timing_json;
    bool gpu_available{};
    bool uploads_ready{};
    {
#if defined(LUX_LARGE_3D_SCENE_PERFORMANCE)
        constexpr std::uint32_t kTargetWidth = 1280U;
        constexpr std::uint32_t kTargetHeight = 720U;
#else
        constexpr std::uint32_t kTargetWidth = 128U;
        constexpr std::uint32_t kTargetHeight = 128U;
#endif
        lux::rendertest::DeviceRenderFixture fixture(
            kTargetWidth,
            kTargetHeight,
            "model_asset_vulkan_qualification",
            {.enable_validation = true, .validation_errors = &validation_errors}
        );
        if (!fixture.ok())
        {
            std::fprintf(stderr, "no Vulkan device; Model qualification skipped\n");
            return kSkip;
        }
        gpu_available = true;

        const auto compile_shader = [&](const std::vector<std::uint32_t>& spirv, const rdesc::ShaderInfo& info) {
            const auto info_bytes = rdesc::ShaderInfo::serialize(info);
            return fixture.awaitControl(fixture.control().compileShader(
                std::span<const std::byte>{
                    reinterpret_cast<const std::byte*>(spirv.data()),
                    spirv.size() * sizeof(std::uint32_t)
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
        if (gbuffer_shader.status != 0U || forward_shader.status != 0U) return 7;

        const auto scene = fixture.makeSceneWithView("TypedModel", "TypedModelView");
        const auto camera_registration =
            fixture.awaitControl(fixture.control().registerFeatureType(kViewCameraFeatureFactory));
        fixture.awaitControl(fixture.control().addFeature(
            scene.scene_id,
            camera_registration.feature_type_id,
            ViewCameraCommTag{}
        ));
        const auto camera_ops = ViewCameraOperationIds::fromOps(
            camera_registration.ops,
            camera_registration.op_count
        );

        const auto material_registration =
            fixture.awaitControl(fixture.control().registerFeatureType(kMaterialFeatureFactory));
        fixture.awaitControl(fixture.control().addFeature(
            scene.scene_id,
            material_registration.feature_type_id,
            MaterialCommTag{}
        ));
        const auto material_ops = MaterialOperationIds::fromOps(
            material_registration.ops,
            material_registration.op_count
        );

        const auto mesh_registration =
            fixture.awaitControl(fixture.control().registerFeatureType(kMeshStackFeatureFactory));
        fixture.awaitControl(fixture.control().addFeature(
            scene.scene_id,
            mesh_registration.feature_type_id,
            MeshStackCommTag{}
        ));
        const auto mesh_ops = MeshStackOperationIds::fromOps(
            mesh_registration.ops,
            mesh_registration.op_count
        );
        if (!camera_ops.valid() || !material_ops.valid() || !mesh_ops.valid()) return 8;

        const auto cluster_registration =
            fixture.awaitControl(fixture.control().registerFeatureType(kRenderClusterFeatureFactory));
        fixture.awaitControl(fixture.control().addFeature(
            scene.scene_id,
            cluster_registration.feature_type_id,
            RenderClusterCommTag{}
        ));
        const auto light_registration =
            fixture.awaitControl(fixture.control().registerFeatureType(kLightFeatureFactory));
        fixture.awaitControl(fixture.control().addFeature(
            scene.scene_id,
            light_registration.feature_type_id,
            LightCommTag{}
        ));
        const auto light_ops = LightOperationIds::fromOps(light_registration.ops, light_registration.op_count);
        const auto shadow_registration =
            fixture.awaitControl(fixture.control().registerFeatureType(kShadowMapFeatureFactory));
        ShadowMapCommConfig shadow_config{};
        shadow_config.atlas_page_resolution = 256U;
        shadow_config.atlas_page_count = 1U;
        fixture.awaitControl(fixture.control().addFeature(
            scene.scene_id,
            shadow_registration.feature_type_id,
            shadow_config
        ));
        const auto mesh_shadow_registration =
            fixture.awaitControl(fixture.control().registerFeatureType(kMeshShadowFeatureFactory));
        fixture.awaitControl(fixture.control().addFeature(
            scene.scene_id,
            mesh_shadow_registration.feature_type_id,
            MeshShadowCommConfig{}
        ));
        const auto forward_registration =
            fixture.awaitControl(fixture.control().registerFeatureType(kForwardMeshFeatureFactory));
        ForwardMeshCommConfig forward_config{};
        forward_config.graph_fragment = forward_shader.shader;
        fixture.awaitControl(fixture.control().addFeature(
            scene.scene_id,
            forward_registration.feature_type_id,
            forward_config
        ));

        GraphMaterialData graph_material{};
        graph_material.param_count = material_asset->data().parameter_count;
        for (std::uint32_t parameter = 0U; parameter < graph_material.param_count; ++parameter)
            std::copy_n(material_asset->data().parameter_defaults[parameter].begin(), 4U, graph_material.params[parameter]);

        std::vector<RTextureHandle> texture_handles;
        for (std::uint32_t slot = 0U; slot < rdesc::MaterialDescription::kMaxTextures; ++slot)
        {
            const auto texture_id = material_asset->data().texture_slot_ids[slot];
            if (texture_id.isNull()) continue;
            const auto texture_asset = decode<TextureAsset>(*vfs, texture_id);
            if (!texture_asset) return 9;
            const auto& texture = texture_asset->data();
            std::vector<OwnedTextureMipLevel> mip_levels;
            for (std::uint32_t level = 0U; level < texture.mipCount(); ++level)
            {
                const auto& range = texture.mipRange(level);
                mip_levels.push_back({
                    texture.pixels().subspan(
                        static_cast<std::size_t>(range.offset),
                        static_cast<std::size_t>(range.size)
                    ),
                    range.width,
                    range.height
                });
            }
            auto request = fixture.uploadClientForTest().tryCreateTexture2DMips(
                std::move(mip_levels),
                texture.channel(),
                texture.pixelFormat(),
                false
            );
            if (!request)
            {
                std::fprintf(stderr, "compressed texture unsupported; Model qualification skipped\n");
                return kSkip;
            }
            const auto uploaded = fixture.awaitUpload(std::move(*request));
            if (uploaded.status != 0U || uploaded.handle.isNull()) return 10;
            texture_handles.push_back(uploaded.handle);
            graph_material.tex_bindless[slot] = uploaded.handle.index;
            graph_material.tex_mask |= 1U << slot;
        }

        auto material_request = uploadGraphMaterial(
            MaterialUploadClient{fixture.uploadClientForTest(), material_ops},
            graph_material,
            gbuffer_shader.shader,
            forward_shader.shader,
            static_cast<std::uint32_t>(material_asset->data().alpha_mode),
            material_asset->data().double_sided
        );
        if (!material_request) return 11;
        const auto material = fixture.awaitUpload(std::move(*material_request));
        if (material.status != 0U || material.handle.isNull()) return 12;

        const lux::rdesc::Mesh* mesh_to_upload = std::addressof(mesh_asset->data());
#if defined(LUX_LARGE_3D_SCENE_PERFORMANCE)
        lux::rdesc::Mesh performance_mesh{};
        constexpr std::uint32_t kTriangleCopies = 64U;
        performance_mesh.vertices.reserve(mesh_asset->data().vertices.size() * kTriangleCopies);
        performance_mesh.indices.reserve(mesh_asset->data().indices.size() * kTriangleCopies);
        for (std::uint32_t copy = 0U; copy < kTriangleCopies; ++copy)
        {
            const float angle = static_cast<float>(copy) * 0.61803398875F;
            const Eigen::Matrix3f rotation = Eigen::AngleAxisf{angle, Eigen::Vector3f::UnitY()}.toRotationMatrix();
            const Eigen::Vector3f offset{
                std::cos(angle) * (0.18F + static_cast<float>(copy % 4U) * 0.07F),
                (static_cast<float>(copy / 8U) - 3.5F) * 0.08F,
                std::sin(angle) * (0.18F + static_cast<float>(copy % 4U) * 0.07F)
            };
            const auto vertex_base = static_cast<std::uint32_t>(performance_mesh.vertices.size());
            for (auto vertex : mesh_asset->data().vertices)
            {
                vertex.position = rotation * vertex.position * 0.35F + offset;
                vertex.normal = (rotation * vertex.normal).normalized();
                vertex.tangent = (rotation * vertex.tangent).normalized();
                vertex.bitangent = (rotation * vertex.bitangent).normalized();
                performance_mesh.vertices.push_back(vertex);
            }
            for (const auto index : mesh_asset->data().indices)
            {
                performance_mesh.indices.push_back(vertex_base + index);
            }
        }
        mesh_to_upload = std::addressof(performance_mesh);
        rendered_mesh_vertices = performance_mesh.vertices.size();
#endif
        auto mesh_request = uploadMesh(
            MeshStackUploadClient{fixture.uploadClientForTest(), mesh_ops},
            *mesh_to_upload
        );
        if (!mesh_request) return 13;
        const auto mesh = fixture.awaitUpload(std::move(*mesh_request));
        if (mesh.status != 0U || mesh.handle.isNull()) return 14;
        uploads_ready = true;

        Eigen::Matrix4f model_transform = Eigen::Matrix4f::Identity();
        for (const auto& node : model->data().nodes)
        {
            if (std::find(node.primitives.begin(), node.primitives.end(), 0U) != node.primitives.end())
            {
                model_transform = node.local_transform.matrix();
                break;
            }
        }
        model_transform(2, 3) = -2.0F;
        simulation::ecs::Registry simulation_registry;
        std::vector<simulation::ecs::Entity> mesh_entities;
#if defined(LUX_LARGE_3D_SCENE_PERFORMANCE)
        constexpr std::uint32_t kGridWidth = 100U;
        constexpr std::uint32_t kGridDepth = 100U;
        mesh_entities.reserve(kGridWidth * kGridDepth);
        for (std::uint32_t depth = 0U; depth < kGridDepth; ++depth)
        {
            for (std::uint32_t column = 0U; column < kGridWidth; ++column)
            {
                const auto grid_entity = simulation_registry.create();
                mesh_entities.push_back(grid_entity);
                simulation_registry.emplace<simulation::ecs::Mesh3D>(
                    grid_entity,
                    simulation::ecs::Mesh3D{
                        rdesc::MeshVisualDescription{mesh_asset->id(), material_asset->id()}
                    }
                );
                simulation_registry.emplace<scene::ResolvedMeshResources>(
                    grid_entity,
                    scene::ResolvedMeshResources{
                        mesh_asset->id(), material_asset->id(), mesh.handle, material.handle
                    }
                );
                simulation::ecs::WorldTransform3D grid_transform{};
                grid_transform.value.linear() = model_transform.block<3, 3>(0, 0).cast<double>() * 0.55;
                grid_transform.value.translation() = Eigen::Vector3d{
                    (static_cast<double>(column) - 49.5) * 1.5,
                    (static_cast<double>((column * 17U + depth * 13U) % 9U) - 4.0) * 0.18,
                    -5.0 - static_cast<double>(depth) * 1.5
                };
                simulation_registry.emplace<simulation::ecs::WorldTransform3D>(grid_entity, grid_transform);
            }
        }
#else
        mesh_entities.push_back(simulation_registry.create());
        simulation_registry.emplace<simulation::ecs::Mesh3D>(
            mesh_entities.front(),
            simulation::ecs::Mesh3D{rdesc::MeshVisualDescription{mesh_asset->id(), material_asset->id()}}
        );
        simulation_registry.emplace<scene::ResolvedMeshResources>(
            mesh_entities.front(),
            scene::ResolvedMeshResources{mesh_asset->id(), material_asset->id(), mesh.handle, material.handle}
        );
        simulation::ecs::WorldTransform3D world_transform{};
        world_transform.value = model_transform.cast<double>();
        simulation_registry.emplace<simulation::ecs::WorldTransform3D>(mesh_entities.front(), world_transform);
#endif
        const auto entity = mesh_entities.front();
        const auto directional_light_entity = simulation_registry.create();
        simulation::ecs::WorldTransform3D directional_transform{};
        directional_transform.value.linear() =
            Eigen::AngleAxisd{-0.65, Eigen::Vector3d::UnitX()}.toRotationMatrix();
        simulation_registry.emplace<simulation::ecs::WorldTransform3D>(
            directional_light_entity,
            directional_transform
        );
        simulation::ecs::Light3D light{};
        light.value.type = rdesc::ELightType::DIRECTIONAL;
        light.value.intensity = 2.0F;
        simulation_registry.emplace<simulation::ecs::Light3D>(directional_light_entity, light);
#if defined(LUX_LARGE_3D_SCENE_PERFORMANCE)
        for (std::uint32_t light_index = 0U; light_index < 32U; ++light_index)
        {
            const auto point_entity = simulation_registry.create();
            simulation::ecs::WorldTransform3D point_transform{};
            point_transform.value.translation() = Eigen::Vector3d{
                (static_cast<double>(light_index % 8U) - 3.5) * 12.0,
                4.0,
                -12.0 - static_cast<double>(light_index / 8U) * 28.0
            };
            simulation_registry.emplace<simulation::ecs::WorldTransform3D>(point_entity, point_transform);
            simulation::ecs::Light3D point{};
            point.value.type = rdesc::ELightType::POINT;
            point.value.color = std::array{
                0.35F + static_cast<float>(light_index % 3U) * 0.3F,
                0.4F + static_cast<float>((light_index + 1U) % 3U) * 0.25F,
                0.45F + static_cast<float>((light_index + 2U) % 3U) * 0.25F
            };
            point.value.intensity = 18.0F;
            point.value.range = 24.0F;
            simulation_registry.emplace<simulation::ecs::Light3D>(point_entity, point);
        }
#endif
        auto mesh_stage = scene::createMesh3DRenderStage(scene::Mesh3DRenderStageConfig{
            .registry = &simulation_registry,
            .scene = scene.scene_id,
            .operations = mesh_ops
        });
        auto light_stage = scene::createLight3DRenderStage(scene::Light3DRenderStageConfig{
            .registry = &simulation_registry,
            .scene = scene.scene_id,
            .operations = light_ops
        });
        if (!mesh_stage || !light_stage) return 15;
        scene::RenderSystem::StageList render_stages;
        render_stages.push_back(std::move(*mesh_stage));
        render_stages.push_back(std::move(*light_stage));
        auto render_system = scene::RenderSystem::create(std::move(render_stages));
        const auto full_sync_start = std::chrono::steady_clock::now();
        if (!render_system || (*render_system)->tryPublish() != scene::ERenderPublishResult::FULL_SYNC_PUBLISHED) return 15;
        full_sync_ms = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - full_sync_start
        ).count();

        if (!fixture.session().trySubmitFrame()) return 15;
        if (!fixture.session().waitAndPumpReplies()) return 15;
        const auto initial_apply_start = std::chrono::steady_clock::now();
        if ((*render_system)->tryForwardUpdate(fixture.session()) != scene::ERenderForwardResult::FORWARDED) return 15;
        if (!fixture.session().waitAndPumpReplies()) return 15;
        initial_state_apply_ms = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - initial_apply_start
        ).count();
#if defined(LUX_LARGE_3D_SCENE_PERFORMANCE)
        const auto mesh_stats = fixture.awaitControl(
            MeshStackControlClient{fixture.control(), mesh_ops}.stats({scene.scene_id})
        );
        const auto light_stats = fixture.awaitControl(
            LightControlClient{fixture.control(), light_ops}.stats({scene.scene_id})
        );
        server_alive_instances = mesh_stats.alive_instances;
        server_alive_lights = light_stats.directional_lights + light_stats.point_lights +
            light_stats.spot_lights + light_stats.area_lights;
        if (server_alive_instances != mesh_entities.size() || server_alive_lights != 33U) return 15;
#endif
        if (!fixture.session().beginFrame()) return 15;

#if defined(LUX_LARGE_3D_SCENE_PERFORMANCE)
        const Eigen::Vector3f camera_eye{0.0F, 4.0F, 3.0F};
        const Eigen::Vector3f camera_target{0.0F, 0.0F, -18.0F};
        const Eigen::Vector3f forward = (camera_target - camera_eye).normalized();
        const Eigen::Vector3f side = forward.cross(Eigen::Vector3f::UnitY()).normalized();
        const Eigen::Vector3f camera_up = side.cross(forward);
        Eigen::Matrix4f view_matrix = Eigen::Matrix4f::Identity();
        view_matrix.block<1, 3>(0, 0) = side.transpose();
        view_matrix.block<1, 3>(1, 0) = camera_up.transpose();
        view_matrix.block<1, 3>(2, 0) = -forward.transpose();
        view_matrix(0, 3) = -side.dot(camera_eye);
        view_matrix(1, 3) = -camera_up.dot(camera_eye);
        view_matrix(2, 3) = forward.dot(camera_eye);
        Eigen::Matrix4f projection_matrix = Eigen::Matrix4f::Zero();
        projection_matrix(0, 0) = 0.974279F;
        projection_matrix(1, 1) = -1.732051F;
        projection_matrix(2, 2) = -1.001001F;
        projection_matrix(2, 3) = -0.1001001F;
        projection_matrix(3, 2) = -1.0F;
        const float eye[3] = {camera_eye.x(), camera_eye.y(), camera_eye.z()};
        viewCameraUpdateTransient(
            ViewCameraProxy{fixture.session(), camera_ops},
            scene.scene_id,
            scene.view,
            view_matrix.data(),
            projection_matrix.data(),
            eye
        );
#else
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
            scene.scene_id,
            scene.view,
            view,
            projection,
            eye
        );
#endif
#if defined(LUX_LARGE_3D_SCENE_PERFORMANCE)
        if (!fixture.session().trySubmitFrame() || !fixture.session().waitAndPumpReplies()) return 16;
        std::vector<double> state_update_times;
        state_update_times.reserve(20U);
        std::vector<double> frame_times;
        frame_times.reserve(80U);
        for (std::uint32_t update_step = 0U; update_step < 20U; ++update_step)
        {
            const auto update_start = std::chrono::steady_clock::now();
            for (std::size_t index = update_step % 10U; index < mesh_entities.size(); index += 10U)
            {
                simulation_registry.patch<simulation::ecs::WorldTransform3D>(
                    mesh_entities[index],
                    [update_step, index](auto& transform) {
                        transform.value.translation().y() =
                            std::sin(static_cast<double>(update_step) * 0.2 + static_cast<double>(index) * 0.013) * 0.35;
                    }
                );
            }
            if ((*render_system)->tryPublish() != scene::ERenderPublishResult::PUBLISHED) return 16;
            if ((*render_system)->tryForwardUpdate(fixture.session()) != scene::ERenderForwardResult::FORWARDED)
                return 16;
            if (!fixture.session().waitAndPumpReplies()) return 16;
            state_update_times.push_back(std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - update_start
            ).count());

            const auto frame_start = std::chrono::steady_clock::now();
            if (!fixture.session().beginFrame() || !fixture.session().trySubmitFrame()) return 16;
            if (!fixture.session().waitAndPumpReplies()) return 16;
            frame_times.push_back(std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - frame_start
            ).count());
        }
        for (std::uint32_t frame = 0U; frame < 60U; ++frame)
        {
            const auto frame_start = std::chrono::steady_clock::now();
            if (!fixture.session().beginFrame() || !fixture.session().trySubmitFrame()) return 16;
            if (!fixture.session().waitAndPumpReplies()) return 16;
            frame_times.push_back(std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - frame_start
            ).count());
        }
        state_update_mean_ms = std::accumulate(state_update_times.begin(), state_update_times.end(), 0.0) /
            static_cast<double>(state_update_times.size());
        frame_mean_ms = std::accumulate(frame_times.begin(), frame_times.end(), 0.0) /
            static_cast<double>(frame_times.size());
        auto ordered_frame_times = frame_times;
        std::sort(ordered_frame_times.begin(), ordered_frame_times.end());
        frame_p95_ms = ordered_frame_times[static_cast<std::size_t>(ordered_frame_times.size() * 0.95)];

        std::string timing_storage(128U * 1024U, '\0');
        const auto timing_reply = fixture.awaitControl(
            fixture.control().queryGpuTiming(scene.scene_id, timing_storage.data(), timing_storage.size())
        );
        if (timing_reply.status == 0U)
        {
            gpu_timing_json.assign(
                timing_storage.data(),
                std::min<std::size_t>(timing_reply.written, timing_storage.size())
            );
            std::vector<double> gpu_frame_times;
            std::size_t cursor{};
            constexpr std::string_view marker{"\"total_ms\":"};
            while ((cursor = gpu_timing_json.find(marker, cursor)) != std::string::npos)
            {
                cursor += marker.size();
                char* end{};
                const double value = std::strtod(gpu_timing_json.c_str() + cursor, &end);
                if (end == gpu_timing_json.c_str() + cursor) break;
                gpu_frame_times.push_back(value);
                cursor = static_cast<std::size_t>(end - gpu_timing_json.c_str());
            }
            if (!gpu_frame_times.empty())
            {
                gpu_timing_samples = gpu_frame_times.size();
                gpu_frame_mean_ms = std::accumulate(gpu_frame_times.begin(), gpu_frame_times.end(), 0.0) /
                    static_cast<double>(gpu_frame_times.size());
                std::sort(gpu_frame_times.begin(), gpu_frame_times.end());
                gpu_frame_p95_ms = gpu_frame_times[static_cast<std::size_t>(gpu_frame_times.size() * 0.95)];
            }
        }
        if (!fixture.session().beginFrame()) return 16;
#elif defined(LUX_L1_L3_RENDER_SYNC_QUALIFICATION)
        if (!fixture.session().trySubmitFrame() || !fixture.session().waitAndPumpReplies()) return 16;
        for (std::uint32_t step = 0U; step < 40U; ++step)
        {
            simulation_registry.patch<simulation::ecs::WorldTransform3D>(entity, [step](auto& transform) {
                const double angle = static_cast<double>(step + 1U) * 0.05;
                transform.value.linear() = Eigen::AngleAxisd{angle, Eigen::Vector3d::UnitY()}.toRotationMatrix();
                transform.value.translation().z() = -2.0;
            });
            const auto published = (*render_system)->tryPublish();
            if (published != scene::ERenderPublishResult::PUBLISHED) return 16;
            if ((*render_system)->tryForwardUpdate(fixture.session()) != scene::ERenderForwardResult::FORWARDED)
                return 16;
            if (!fixture.session().waitAndPumpReplies()) return 16;
            if (!fixture.session().beginFrame() || !fixture.session().trySubmitFrame()) return 16;
            if (!fixture.session().waitAndPumpReplies()) return 16;
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        if (!fixture.session().beginFrame()) return 16;
#endif
        fixture.flush(8);
        const auto pixels = fixture.readback(scene);
        if (fixture.lastReadback().status != 0U) return 16;
        double luminance_sum{};
        double luminance_square_sum{};
        for (std::size_t offset = 0U; offset + 3U < pixels.size(); offset += 4U)
        {
            if ((pixels[offset] | pixels[offset + 1U] | pixels[offset + 2U]) != 0U) ++lit_pixels;
            const double blue = static_cast<double>(pixels[offset]) / 255.0;
            const double green = static_cast<double>(pixels[offset + 1U]) / 255.0;
            const double red = static_cast<double>(pixels[offset + 2U]) / 255.0;
            const double luminance = red * 0.2126 + green * 0.7152 + blue * 0.0722;
            luminance_sum += luminance;
            luminance_square_sum += luminance * luminance;
        }
        const double pixel_count = static_cast<double>(pixels.size() / 4U);
        const double mean_luminance = luminance_sum / pixel_count;
        luminance_variance = luminance_square_sum / pixel_count - mean_luminance * mean_luminance;
#if defined(LUX_LARGE_3D_SCENE_PERFORMANCE)
        std::ofstream screenshot{LUX_LARGE_3D_SCENE_SCREENSHOT, std::ios::binary | std::ios::trunc};
        if (!screenshot) return 16;
        screenshot << "P6\n" << kTargetWidth << ' ' << kTargetHeight << "\n255\n";
        for (std::size_t offset = 0U; offset + 3U < pixels.size(); offset += 4U)
        {
            const char rgb[3]{
                static_cast<char>(pixels[offset + 2U]),
                static_cast<char>(pixels[offset + 1U]),
                static_cast<char>(pixels[offset])
            };
            screenshot.write(rgb, 3);
        }
        screenshot.close();
#endif

        simulation_registry.remove<simulation::ecs::Mesh3D>(entity);
        simulation_registry.remove<simulation::ecs::Light3D>(entity);
        MeshStackControlClient{fixture.control(), mesh_ops}.destroyMesh({mesh.handle});
        MaterialControlClient{fixture.control(), material_ops}.destroyMaterial({material.handle});
        for (const auto handle : texture_handles) fixture.control().destroyTexture(handle);
        fixture.control().destroyShader(gbuffer_shader.shader);
        fixture.control().destroyShader(forward_shader.shader);
        fixture.flush(4);
    }

    std::printf(
        "gpu=%u model_primitives=%zu mesh_vertices=%zu material_textures=%zu server_instances=%u "
        "server_lights=%u lit_pixels=%u "
        "luminance_variance=%.8f full_sync_ms=%.3f initial_state_apply_ms=%.3f "
        "state_update_mean_ms=%.3f frame_mean_ms=%.3f "
        "frame_p95_ms=%.3f gpu_frame_mean_ms=%.6f gpu_frame_p95_ms=%.6f gpu_samples=%zu "
        "validation_errors=%d\n",
        gpu_available ? 1U : 0U,
        model->data().primitives.size(),
        rendered_mesh_vertices,
        std::count_if(
            material_asset->data().texture_slot_ids.begin(),
            material_asset->data().texture_slot_ids.end(),
            [](AssetId id) noexcept { return !id.isNull(); }
        ),
        server_alive_instances,
        server_alive_lights,
        lit_pixels,
        luminance_variance,
        full_sync_ms,
        initial_state_apply_ms,
        state_update_mean_ms,
        frame_mean_ms,
        frame_p95_ms,
        gpu_frame_mean_ms,
        gpu_frame_p95_ms,
        gpu_timing_samples,
        validation_errors.load()
    );
#if defined(LUX_LARGE_3D_SCENE_PERFORMANCE)
    const bool image_is_nontrivial = lit_pixels > (1280U * 720U) / 50U && luminance_variance > 1.0e-4;
    const bool success = gpu_available && uploads_ready && image_is_nontrivial && validation_errors.load() == 0;
#else
    const bool success = gpu_available && uploads_ready && lit_pixels >= 32U && validation_errors.load() == 0;
#endif
    if (!success && !require_gpu) return kSkip;
    return success ? 0 : 17;
}
