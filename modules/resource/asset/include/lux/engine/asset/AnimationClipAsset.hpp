#pragma once
/**
 * @file AnimationClipAsset.hpp
 * @brief Asset wrapper for an animation clip resource (`.luxasset`
 *        containing keyframe tracks targeting a Skeleton).
 *
 * `AnimationClipAsset` is the RAII handle for an `rdesc::AnimationClip`.
 * A clip is skeleton-bound by name: each `BoneTrack` carries a
 * `bone_index` resolved at import time against a specific Skeleton.
 * Re-targeting a clip onto a different rig is a separate concern, out of
 * scope here.
 *
 * The on-disk `.luxasset` layout (`AssetFileHeader` + binary description
 * blob, no payload section since keyframes are metadata) is the concern
 * of @ref AnimationClipSerDeser.
 */

#include <lux/engine/description/AnimationClip.hpp>
#include "Asset.hpp"

namespace lux::asset
{
    /// Re-exported for convenience under the lux::asset namespace.
    using lux::rdesc::BoneTrack;
    using lux::rdesc::AnimationClip;

    /**
     * @brief Configuration for AnimationClipSerDeser. Empty today;
     *        reserved for future options (e.g. resampling rate when
     *        importing high-density rotation keys).
     */
    struct AnimationClipLoadConfig{};

    /**
     * @brief Asset class for animation clip resources.
     */
    class LUX_RESOURCE_PUBLIC AnimationClipAsset : public TAsset<AnimationClip>
    {
        friend class AnimationClipSerDeser;
    public:
        static constexpr EAssetType asset_type{ EAssetType::ANIMATION_CLIP };

        explicit AnimationClipAsset(std::unique_ptr<AssetInfo>         info,
                                    std::unique_ptr<AnimationClip>     clip = nullptr);
    };
}
