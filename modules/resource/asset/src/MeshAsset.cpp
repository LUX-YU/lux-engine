#include <lux/engine/asset/MeshAsset.hpp>

namespace lux::asset
{
    /**
     * @brief Constructs a MeshAsset with the given asset information.
     * @param info Unique pointer to AssetInfo containing mesh metadata
     */
    MeshAsset::MeshAsset(std::unique_ptr<AssetInfo> info)
        : TAsset(std::move(info))
    {

    }
} // namespace lux::asset
