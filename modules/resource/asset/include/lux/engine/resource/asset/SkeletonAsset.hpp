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
    /**
     * @brief Configuration for SkeletonSerDeser. Empty today; reserved
     *        for future options (e.g. clamping bone count for
     *        memory-budget reasons during streaming).
     */
    struct SkeletonLoadConfig{};

    /**
     * @brief Asset class for skeleton resources.
     */
    class LUX_RESOURCE_PUBLIC SkeletonAsset
        : public TAsset<lux::rdesc::Skeleton>
    {
        friend class SkeletonSerDeser;
    public:
        static constexpr EAssetType asset_type{ EAssetType::SKELETON };

        explicit SkeletonAsset(std::unique_ptr<AssetInfo>      info,
                               std::unique_ptr<lux::rdesc::Skeleton> skeleton = nullptr);
    };
}
