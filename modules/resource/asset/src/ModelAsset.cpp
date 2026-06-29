#include <lux/engine/asset/ModelAsset.hpp>

namespace lux::asset
{
    /**
     * @brief Constructs a ModelAsset with the given asset information.
     * @param info Unique pointer to AssetInfo containing model metadata
     */
    ModelAsset::ModelAsset(std::unique_ptr<AssetInfo> info)
        : TAsset<ModelNode>(std::move(info))
    {

    }

    /**
     * @brief Virtual destructor for ModelAsset.
     */
    ModelAsset::~ModelAsset(){}
}
