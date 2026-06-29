#include <lux/engine/asset/AnimationClipAsset.hpp>

namespace lux::asset
{
    AnimationClipAsset::AnimationClipAsset(std::unique_ptr<AssetInfo>     info,
                                           std::unique_ptr<AnimationClip> clip)
        : TAsset<AnimationClip>(std::move(info), std::move(clip))
    {
    }
}
