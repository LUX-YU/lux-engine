#pragma once
#include <lux/engine/description/Mesh.hpp>
#include "Asset.hpp"

namespace lux::asset
{
    /**
     * @brief Configuration structure for mesh loading operations.
     */
    struct MeshLoadConfig{};

    struct MeshAssetInfo
    {
    };
    
    /**
     * @brief Asset class for mesh resources.
     */
    class LUX_ASSET_PUBLIC MeshAsset : public TAsset<lux::rdesc::Mesh>
    {
        friend class MeshSerDeser;
    public:
        static constexpr EAssetType asset_type{ EAssetType::MESH };

        explicit MeshAsset(std::unique_ptr<AssetInfo> info);
    };
}
