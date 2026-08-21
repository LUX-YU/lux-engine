/**
 * @file SkeletalAnimationResolver.cpp
 * @brief Resolves skeleton/clip asset refs into AnimatorCacheComponent for the
 *        pure AnimationSystem. Owns all AssetManager contact for animation.
 */

#include "lux/engine/ecs/animation/systems/SkeletalAnimationResolver.hpp"
#include "lux/engine/ecs/render/components/3d/AnimatorComponent.hpp"
#include "lux/engine/ecs/render/components/3d/AnimatorCacheComponent.hpp"
#include "lux/engine/ecs/render/components/3d/SkeletalMeshComponent.hpp"

#include <lux/engine/resource/asset/AssetManager.hpp>
#include <lux/engine/resource/asset/animation/SkeletonAsset.hpp>
#include <lux/engine/resource/asset/animation/AnimationClipAsset.hpp>

#include <cassert>
#include <utility>

namespace lux::ecs
{
    SkeletalAnimationResolver::SkeletalAnimationResolver(
        lux::asset::AssetManager& mgr, AssetLoadFn request_load) noexcept
        : mgr_(&mgr)
        , request_load_(std::move(request_load))
    {
        // 必填(2026-08-04 收口,CPU 懒加载钩子消费者同款,见
        // 成因注释)。本回落此前**零可达者**(唯一构造点 SceneRuntime 的
        // loader 只可能来自宿主,没有宿主传空)—— 纯死代码,删。
        assert(request_load_ && "request_load is required; tests use "
                                "lux::ecs::syncTestLoader(mgr)");
    }

    SkeletalAnimationResolver::~SkeletalAnimationResolver() = default;

    void SkeletalAnimationResolver::update(const lux::ecs::SystemUpdateContext& ctx)
    {
        auto& registry = ctx.registry();
        auto view = registry.view<AnimatorComponent, SkeletalMeshComponent>();

        view.each([this, &registry](entt::entity e, AnimatorComponent& anim, SkeletalMeshComponent& mesh)
        {
            // Runtime-only resolution cache. get_or_emplace lazily attaches one
            // per animator; it lives in a non-reflected pool, so emplacing it
            // mid-iteration is safe (it is not part of this view).
            auto& cache = registry.get_or_emplace<AnimatorCacheComponent>(e);

            // ── Skeleton ──────────────────────────────────────────────────
            // Detect a runtime swap (component id changed since last resolve);
            // force AnimationSystem to resample + rebuild the bind decomposition.
            if (cache.skeleton_id != mesh.skeleton_asset_id)
            {
                cache.skeleton_id  = mesh.skeleton_asset_id;
                cache.have_pose    = false;
                cache.bind_dirty   = true;
                // 驻留票在换装分支换发(不是每帧):流送驱逐的闸门从此看得见动画。
                cache.skeleton_ref = mgr_->acquire(cache.skeleton_id);
            }
            // Re-query EVERY frame so an evicted asset resolves to null (the
            // system then bails) — no cached pointer to dangle.
            cache.skeleton = nullptr;
            if (!cache.skeleton_id.is_nil())
            {
                const auto* sa =
                    mgr_->fetchAssetAs<lux::asset::SkeletonAsset>(cache.skeleton_id);
                if (sa && sa->data())
                    cache.skeleton = sa->data();
                else
                    request_load_(cache.skeleton_id);   // absent / data-less shell → load
            }

            // ── Clip (optional; a nil id is a valid "stand in bind pose") ──
            if (cache.clip_id != anim.clip_asset_id)
            {
                cache.clip_id   = anim.clip_asset_id;
                cache.have_pose = false;
                cache.clip_ref  = mgr_->acquire(cache.clip_id);   // nil → 空票
            }
            cache.clip = nullptr;
            if (!cache.clip_id.is_nil())
            {
                const auto* ca =
                    mgr_->fetchAssetAs<lux::asset::AnimationClipAsset>(cache.clip_id);
                if (ca && ca->data())
                    cache.clip = ca->data();
                else
                    request_load_(cache.clip_id);
            }
        });
    }

} // namespace lux::ecs
