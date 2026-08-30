#include <lux/engine/resource/asset/Asset.hpp>
#include <lux/engine/resource/asset/AssetSerDeser.hpp>
#include <lux/engine/resource/asset/CookedAssetImage.hpp>

#include <type_traits>

int main()
{
    static_assert(!std::is_copy_constructible_v<lux::asset::Asset>);
    const lux::asset::AssetDecodeLimits limits{1024U, 1024U, 4U};
    return limits.max_image_bytes == 1024U && lux::asset::kCookedAssetVersionV2 == 20260606U ? 0 : 1;
}
