#include <lux/engine/resource/asset/animation/AnimationClipAsset.hpp>

namespace lux::asset
{
    AnimationClipAsset::AnimationClipAsset(std::unique_ptr<AssetInfo>     info,
                                           std::unique_ptr<lux::rdesc::AnimationClip> clip)
        : TAsset<lux::rdesc::AnimationClip>(std::move(info), std::move(clip))
    {
    }
}
