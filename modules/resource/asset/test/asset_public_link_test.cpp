#include <lux/engine/resource/asset/AssetCodecCatalog.hpp>
#include <lux/engine/resource/asset/storage/AssetProvider.hpp>
#include <lux/engine/resource/asset/storage/AssetVfs.hpp>
#include <lux/engine/resource/asset/storage/VirtualPath.hpp>
#include <lux/engine/resource/asset/storage/pak/PakArchive.hpp>
#include <lux/engine/resource/asset/storage/pak/PakAssetProvider.hpp>

int main()
{
    const auto catalog = lux::asset::runtimeAssetCodecCatalog();
    const auto path = lux::asset::VirtualPath::parse("/Game/Public/Link");
    return catalog && path ? 0 : 1;
}
