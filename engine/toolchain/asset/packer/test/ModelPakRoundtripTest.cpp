#include <lux/engine/resource/asset/material/MaterialAssets.hpp>
#include <lux/engine/resource/asset/mesh/MeshAsset.hpp>
#include <lux/engine/resource/asset/model/ModelAsset.hpp>
#include <lux/engine/resource/asset/storage/AssetVfs.hpp>
#include <lux/engine/resource/asset/storage/pak/PakArchive.hpp>
#include <lux/engine/resource/asset/storage/pak/PakAssetProvider.hpp>
#include <lux/engine/resource/asset/texture/TextureAsset.hpp>

#include <cassert>
#include <filesystem>
#include <limits>

namespace
{
    template <class Asset>
    std::shared_ptr<const Asset> decode(
        lux::asset::AssetVfsView vfs,
        lux::asset::AssetId id
    )
    {
        const auto blob = vfs.open(id);
        assert(blob);
        const auto asset = lux::asset::TAssetSerDeser<Asset>::decode(
            id,
            blob->bytes,
            lux::asset::AssetDecodeLimits{
                blob->bytes.size(),
                64U * 1024U * 1024U,
                16U
            }
        );
        assert(asset);
        return *asset;
    }
}

int main()
{
    const auto pak_info = lux::asset::inspectPak(LUX_MODEL_TEST_PAK);
    assert(pak_info);
    auto provider = lux::asset::PakAssetProvider::loadFromFile(LUX_MODEL_TEST_PAK);
    assert(provider);
    lux::asset::AssetVfs vfs;
    assert(vfs.mount({"/Game", *provider, 0}) != lux::asset::kInvalidMountId);
    const auto view = vfs.view();

    lux::asset::AssetId model_id;
    for (const auto& entry : pak_info->entries)
        if (entry.magic_number == lux::asset::ModelAsset::primary_magic) model_id = entry.id;
    assert(!model_id.isNull());
    const auto model = decode<lux::asset::ModelAsset>(view, model_id);
    assert(model->data().primitives.size() == 3U);

    for (const auto& primitive : model->data().primitives)
    {
        const auto mesh = decode<lux::asset::MeshAsset>(view, primitive.mesh);
        const auto material = decode<lux::asset::MaterialAsset>(view, primitive.material);
        assert(!mesh->data().vertices.empty());
        for (const auto texture_id : material->data().texture_slot_ids)
        {
            if (texture_id.isNull()) continue;
            const auto texture = decode<lux::asset::TextureAsset>(view, texture_id);
            assert(texture->data().mipCount() != 0U);
        }
    }
    assert(model->data().primitives[0].material == model->data().primitives[1].material);
    return 0;
}
