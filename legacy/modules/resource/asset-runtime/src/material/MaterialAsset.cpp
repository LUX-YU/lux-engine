#include <lux/engine/resource/asset/material/MaterialAsset.hpp>

namespace lux::asset
{
    template class LUX_ASSET_PUBLIC TAsset<MaterialData>;

    MaterialAsset::MaterialAsset(
        std::unique_ptr<AssetInfo> info,
        std::unique_ptr<MaterialData> data
    )
        : TAsset<MaterialData>(std::move(info), std::move(data))
    {
    }

    MaterialAsset::~MaterialAsset() = default;
} // namespace lux::asset
