#pragma once
#include <lux/engine/description/Mesh.hpp>
#include "Asset.hpp"

namespace lux::asset
{
    // ===== Backward compatibility: re-export description types =====
    using lux::rdesc::max_bone_influence;
    using lux::rdesc::Bone;
    using lux::rdesc::Vertex;
    using lux::rdesc::Mesh;

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
    class LUX_RESOURCE_PUBLIC MeshAsset : public TAsset<Mesh>
    {
        friend class MeshSerDeser;
        friend class ModelSerDeser;
    public:
        static constexpr EAssetType asset_type{ EAssetType::MESH };

        explicit MeshAsset(std::unique_ptr<AssetInfo> info);
    };
}