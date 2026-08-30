#include <lux/engine/description/Texture.hpp>
#include <lux/engine/resource/asset/texture/TextureAsset.hpp>

#include <type_traits>

int main()
{
    static_assert(std::is_same_v<
        lux::asset::TextureAsset::data_type,
        lux::rdesc::Texture
    >);
    static_assert(lux::asset::TextureAsset::primary_magic == 0x01309143U);
    static_assert(lux::asset::TextureAsset::asset_type ==
        lux::asset::AssetTypeId::fromName("lux.texture"));
    return 0;
}
