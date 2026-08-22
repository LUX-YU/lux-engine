#include <lux/engine/ecs/animation/systems/SkeletalAnimationAssetResolver.hpp>

#include <lux/engine/ecs/render/components/3d/AnimatorCacheComponent.hpp>
#include <lux/engine/ecs/render/components/3d/AnimatorComponent.hpp>
#include <lux/engine/ecs/render/components/3d/SkeletalMeshComponent.hpp>
#include <lux/engine/resource/asset/AssetManager.hpp>
#include <lux/engine/resource/asset/animation/AnimationClipAsset.hpp>
#include <lux/engine/resource/asset/animation/SkeletonAsset.hpp>

#include <utility>

namespace lux::ecs
{
    SkeletalAnimationAssetResolver::SkeletalAnimationAssetResolver(
        lux::asset::AssetManager& manager,
        lux::asset_runtime::AssetClient client) noexcept
        : manager_(&manager)
        , client_(std::move(client))
    {}

    void SkeletalAnimationAssetResolver::update(
        const lux::ecs::SystemUpdateContext& context)
    {
        auto& registry = context.registry();
        auto view = registry.view<AnimatorComponent, SkeletalMeshComponent>();
        view.each([this, &registry](
            entt::entity entity,
            AnimatorComponent& animation,
            SkeletalMeshComponent& mesh)
        {
            auto& cache = registry.get_or_emplace<
                AnimatorCacheComponent>(entity);
            if (cache.skeleton_id != mesh.skeleton_asset_id)
            {
                cache.skeleton_id = mesh.skeleton_asset_id;
                cache.have_pose = false;
                cache.bind_dirty = true;
                cache.skeleton_ref = manager_->acquire(cache.skeleton_id);
            }
            cache.skeleton = nullptr;
            if (!cache.skeleton_id.is_nil())
            {
                const auto* skeleton = manager_->fetchAssetAs<
                    lux::asset::SkeletonAsset>(cache.skeleton_id);
                if (skeleton && skeleton->data())
                    cache.skeleton = skeleton->data();
                else
                    static_cast<void>(client_.request(cache.skeleton_id));
            }

            if (cache.clip_id != animation.clip_asset_id)
            {
                cache.clip_id = animation.clip_asset_id;
                cache.have_pose = false;
                cache.clip_ref = manager_->acquire(cache.clip_id);
            }
            cache.clip = nullptr;
            if (!cache.clip_id.is_nil())
            {
                const auto* clip = manager_->fetchAssetAs<
                    lux::asset::AnimationClipAsset>(cache.clip_id);
                if (clip && clip->data())
                    cache.clip = clip->data();
                else
                    static_cast<void>(client_.request(cache.clip_id));
            }
        });
    }
}
