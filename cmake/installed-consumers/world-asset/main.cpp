#include <lux/engine/world/WorldAssetCodec.hpp>

#include <type_traits>

int
main()
{
    static_assert(std::is_same_v<
        lux::world::WorldAsset::data_type,
        lux::world::WorldDescription
    >);
    static_assert(lux::world::WorldAsset::asset_type ==
        lux::asset::AssetTypeId::fromName("lux.world.description"));
    return lux::world::WorldAsset::primary_magic == lux::world::WorldAssetPrimaryMagic ? 0 : 1;
}
