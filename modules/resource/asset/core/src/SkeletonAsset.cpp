#include <lux/engine/resource/asset/SkeletonAsset.hpp>

namespace lux::asset
{
    SkeletonAsset::SkeletonAsset(std::unique_ptr<AssetInfo> info,
                                 std::unique_ptr<lux::rdesc::Skeleton> skeleton)
        : TAsset<lux::rdesc::Skeleton>(std::move(info), std::move(skeleton))
    {
    }
}
