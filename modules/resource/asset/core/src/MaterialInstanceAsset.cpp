#include <lux/engine/resource/asset/MaterialInstanceAsset.hpp>

namespace lux::asset
{
    template class LUX_RESOURCE_PUBLIC TAsset<MaterialInstanceData>;

    MaterialInstanceAsset::MaterialInstanceAsset(
        std::unique_ptr<AssetInfo> info,
        std::unique_ptr<MaterialInstanceData> data
    )
        : TAsset<MaterialInstanceData>(std::move(info), std::move(data))
    {
    }

    MaterialInstanceAsset::~MaterialInstanceAsset() = default;
} // namespace lux::asset
