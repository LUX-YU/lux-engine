#include "DeviceRenderFixture.hpp"

#include <lux/engine/function/render/client/genops/MaterialOperation.ops.hpp>
#include <lux/engine/function/render/client/genops/MeshStackOperation.ops.hpp>
#include <lux/engine/function/render/client/resources/material/GraphMaterialData.hpp>
#include <lux/engine/resource/asset/material/MaterialAssets.hpp>
#include <lux/engine/resource/asset/mesh/MeshAsset.hpp>
#include <lux/engine/resource/asset/model/ModelAsset.hpp>
#include <lux/engine/resource/asset/storage/AssetVfs.hpp>
#include <lux/engine/resource/asset/storage/pak/PakArchive.hpp>
#include <lux/engine/resource/asset/storage/pak/PakAssetProvider.hpp>

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <span>
#include <vector>

namespace
{
    inline constexpr int kSkip = 77;

    template <class Asset>
    [[nodiscard]] std::shared_ptr<const Asset> decode(lux::asset::AssetVfsView vfs, lux::asset::AssetId id)
    {
        const auto blob = vfs.open(id);
        if (!blob)
        {
            return {};
        }
        const auto asset = lux::asset::TAssetSerDeser<Asset>::decode(
            id,
            blob->bytes,
            lux::asset::AssetDecodeLimits{blob->bytes.size(), 128U * 1024U * 1024U, 16U}
        );
        return asset ? *asset : std::shared_ptr<const Asset>{};
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
    const auto view = vfs->view();
    const auto model = decode<ModelAsset>(view, model_id);
    if (!model || model->data().primitives.empty())
    {
        return 4;
    }
    const auto& primitive = model->data().primitives.front();
    const auto mesh_asset = decode<MeshAsset>(view, primitive.mesh);
    const auto material_asset = decode<MaterialAsset>(view, primitive.material);
    if (!mesh_asset || !material_asset)
    {
        return 5;
    }

    std::atomic_int validation_errors{};
    rendertest::DeviceRenderFixture fixture(
        64U,
        64U,
        "model_asset_vulkan_qualification",
        {.enable_validation = true, .validation_errors = &validation_errors}
    );
    if (!fixture.ok())
    {
        std::puts("SKIP: Vulkan device or validation layer unavailable");
        return kSkip;
    }
    const auto scene = fixture.makeSceneWithView("ModelUploadScene", "ModelUploadView");

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

    const auto material_registration = fixture.awaitControl(
        fixture.control().registerFeatureType(kMaterialFeatureFactory)
    );
    fixture.awaitControl(fixture.control().addFeature(
        scene.scene_id,
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
        scene.scene_id,
        mesh_registration.feature_type_id,
        MeshStackCommTag{}
    ));
    const auto mesh_ops = MeshStackOperationIds::fromOps(mesh_registration.ops, mesh_registration.op_count);
    if (!material_ops.valid() || !mesh_ops.valid())
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
    auto mesh_request = uploadMesh(
        MeshStackUploadClient{fixture.uploadClientForTest(), mesh_ops},
        mesh_asset->data()
    );
    if (!mesh_request)
    {
        return 9;
    }
    const auto mesh = fixture.awaitUpload(std::move(*mesh_request));
    const bool uploads_valid = material.status == 0U && !material.handle.isNull() && mesh.status == 0U &&
        !mesh.handle.isNull();
    std::printf(
        "model=%u,mesh_handle=%u:%u,material_handle=%u:%u,validation_errors=%d\n",
        model ? 1U : 0U,
        mesh.handle.index,
        mesh.handle.gen,
        material.handle.index,
        material.handle.gen,
        validation_errors.load()
    );
    return uploads_valid && validation_errors.load() == 0 ? 0 : 10;
}
