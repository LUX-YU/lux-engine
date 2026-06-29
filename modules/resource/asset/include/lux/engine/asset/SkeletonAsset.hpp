#pragma once
/**
 * @file SkeletonAsset.hpp
 * @brief Asset wrapper for a skeleton resource (`.luxasset` containing
 *        a bone hierarchy + bind pose).
 *
 * `SkeletonAsset` is the RAII handle for an `rdesc::Skeleton`. It is
 * mesh-independent: a single skeleton can back multiple skeletal meshes
 * (e.g. detachable equipment that shares a character's bone hierarchy).
 *
 * The on-disk `.luxasset` layout (`AssetFileHeader` + binary description
 * blob, no payload section since a skeleton has no separate runtime
 * payload) is the concern of @ref SkeletonSerDeser.
 */

#include <lux/engine/description/Skeleton.hpp>
#include "Asset.hpp"

namespace lux::asset
{
    /// Re-exported for convenience so consumers can refer to the same
    /// types under the lux::asset namespace (mirrors MeshAsset's
    /// re-export of rdesc::Mesh).
    using lux::rdesc::Bone_t;
    using lux::rdesc::Skeleton;

    /**
     * @brief Configuration for SkeletonSerDeser. Empty today; reserved
     *        for future options (e.g. clamping bone count for
     *        memory-budget reasons during streaming).
     */
    struct SkeletonLoadConfig{};

    /**
     * @brief Asset class for skeleton resources.
     */
    class LUX_RESOURCE_PUBLIC SkeletonAsset : public TAsset<Skeleton>
    {
        friend class SkeletonSerDeser;
    public:
        static constexpr EAssetType asset_type{ EAssetType::SKELETON };

        explicit SkeletonAsset(std::unique_ptr<AssetInfo>      info,
                               std::unique_ptr<Skeleton>       skeleton = nullptr);
    };
}
