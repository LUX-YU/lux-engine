#include <lux/engine/scene/SceneAssetCodec.hpp>

#include <type_traits>

int main()
{
    static_assert(std::is_same_v<
        lux::scene::SceneAsset::data_type,
        lux::scene::SceneDescription
    >);
    static_assert(lux::scene::SceneAsset::asset_type ==
        lux::asset::AssetTypeId::fromName("lux.scene.description"));
    return lux::scene::SceneAsset::primary_magic == lux::scene::SceneAssetPrimaryMagic ? 0 : 1;
}
