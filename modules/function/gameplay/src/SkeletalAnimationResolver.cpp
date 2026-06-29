/**
 * @file SkeletalAnimationResolver.cpp
 * @brief Resolves skeleton/clip asset refs into AnimatorCacheComponent for the
 *        pure AnimationSystem. Owns all AssetManager contact for animation.
 */

#include "lux/engine/gameplay/3d/world/systems/SkeletalAnimationResolver.hpp"
#include "lux/engine/gameplay/3d/world/components/AnimatorComponent.hpp"
#include "lux/engine/gameplay/3d/world/components/AnimatorCacheComponent.hpp"
#include "lux/engine/gameplay/3d/world/components/SkeletalMeshComponent.hpp"

#include <lux/engine/asset/AssetManager.hpp>
#include <lux/engine/asset/SkeletonAsset.hpp>
#include <lux/engine/asset/AnimationClipAsset.hpp>

#include <utility>

namespace lux::gameplay::d3
{
    SkeletalAnimationResolver::SkeletalAnimationResolver(
        lux::asset::AssetManager& mgr, AssetLoadFn request_load) noexcept
        : mgr_(&mgr)
        , request_load_(std::move(request_load))
    {
        // No async loader wired → synchronous fallback (mirrors the bridge).
        if (!request_load_)
            request_load_ = [this](const lux::asset::asset_id_t& id)
            {
                (void)mgr_->ensureAsset(id);
            };
    }

    SkeletalAnimationResolver::~SkeletalAnimationResolver() = default;

    void SkeletalAnimationResolver::update(lux::meta::EntityRegistry& registry, float /*dt*/)
    {
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
                cache.skeleton_id = mesh.skeleton_asset_id;
                cache.have_pose   = false;
                cache.bind_dirty  = true;
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

} // namespace lux::gameplay::d3
