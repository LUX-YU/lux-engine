#include <lux/engine/asset/SkeletonAsset.hpp>

namespace lux::asset
{
    SkeletonAsset::SkeletonAsset(std::unique_ptr<AssetInfo> info,
                                 std::unique_ptr<Skeleton>  skeleton)
        : TAsset<Skeleton>(std::move(info), std::move(skeleton))
    {
    }
}
