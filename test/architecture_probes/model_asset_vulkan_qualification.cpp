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

#include <Eigen/Core>

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <string_view>
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
    bool gpu_available{};
    bool uploads_ready{};
    {
        lux::rendertest::DeviceRenderFixture fixture(
            128U,
            128U,
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
            material_asset->id(),
            graph_material,
            gbuffer_shader.shader,
            forward_shader.shader,
            static_cast<std::uint32_t>(material_asset->data().alpha_mode),
            material_asset->data().double_sided
        );
        if (!material_request) return 11;
        const auto material = fixture.awaitUpload(std::move(*material_request));
        if (material.status != 0U || material.handle.isNull()) return 12;

        auto mesh_request = uploadMesh(
            MeshStackUploadClient{fixture.uploadClientForTest(), mesh_ops},
            mesh_asset->id(),
            mesh_asset->data()
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
        const auto instance = fixture.await(addTransientMeshInstance(
            MeshStackProxy{fixture.session(), mesh_ops},
            scene.scene_id,
            mesh.handle,
            material.handle,
            model_transform.data(),
            kInstanceFlagCastShadow | kInstanceFlagReceiveShadow | kInstanceFlagVisible | (1U << 31U)
        ));
        if (instance.status != MeshInstanceCreateStatus::Ok || !instance.object) return 15;

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
        fixture.flush(8);
        const auto pixels = fixture.readback(scene);
        if (fixture.lastReadback().status != 0U) return 16;
        for (std::size_t offset = 0U; offset + 3U < pixels.size(); offset += 4U)
            if ((pixels[offset] | pixels[offset + 1U] | pixels[offset + 2U]) != 0U) ++lit_pixels;

        MeshStackProxy{fixture.session(), mesh_ops}.removeMeshInstance({scene.scene_id, instance.object});
        MeshStackControlClient{fixture.control(), mesh_ops}.destroyMesh({mesh.handle});
        MaterialControlClient{fixture.control(), material_ops}.destroyMaterial({material.handle});
        for (const auto handle : texture_handles) fixture.control().destroyTexture(handle);
        fixture.control().destroyShader(gbuffer_shader.shader);
        fixture.control().destroyShader(forward_shader.shader);
        fixture.flush(4);
    }

    std::printf(
        "gpu=%u model_primitives=%zu mesh_vertices=%zu material_textures=%zu lit_pixels=%u validation_errors=%d\n",
        gpu_available ? 1U : 0U,
        model->data().primitives.size(),
        mesh_asset->data().vertices.size(),
        std::count_if(
            material_asset->data().texture_slot_ids.begin(),
            material_asset->data().texture_slot_ids.end(),
            [](AssetId id) noexcept { return !id.isNull(); }
        ),
        lit_pixels,
        validation_errors.load()
    );
    const bool success = gpu_available && uploads_ready && lit_pixels >= 32U && validation_errors.load() == 0;
    if (!success && !require_gpu) return kSkip;
    return success ? 0 : 17;
}
