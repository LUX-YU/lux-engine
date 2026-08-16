/**
 * @file Flipbook2DResolver.cpp
 * @brief Resolves image-anim clip + atlas asset refs into
 *        FlipbookAnimCacheComponent for the pure FlipbookAnimationSystem (A2-01).
 *        Owns all AssetManager contact for 2D frame animation.
 */

#include <lux/engine/ecs/animation/systems/Flipbook2DResolver.hpp>
#include <lux/engine/ecs/animation/components/FlipbookAnimationComponent.hpp>

#include <lux/engine/resource/asset/AssetManager.hpp>
#include <lux/engine/resource/asset/TextureAtlasAssets.hpp>

#include <cassert>
#include <utility>

namespace lux::ecs
{
    Flipbook2DResolver::Flipbook2DResolver(
        lux::asset::AssetManager& mgr, AssetLoadFn request_load) noexcept
        : mgr_(&mgr)
        , request_load_(std::move(request_load))
    {
        // 必填(2026-08-04 收口,CPU 懒加载钩子消费者同款)。
        assert(request_load_ && "request_load is required; tests use "
                                "lux::ecs::syncTestLoader(mgr)");
    }

    Flipbook2DResolver::~Flipbook2DResolver() = default;

    void Flipbook2DResolver::update(const lux::ecs::SystemUpdateContext& ctx)
    {
        auto& registry = ctx.registry();
        auto view = registry.view<FlipbookAnimationComponent>();
        view.each([this, &registry](entt::entity e, FlipbookAnimationComponent& anim)
        {
            // Runtime-only cache, lazily attached (non-reflected pool — safe to
            // emplace mid-iteration; it is not part of this view).
            auto& cache = registry.get_or_emplace<FlipbookAnimCacheComponent>(e);

            // Clip swap → restart the applied-step cursor so the first frame of
            // the new clip is applied (and its events fire) on next sample.
            if (cache.clip_id != anim.clip)
            {
                cache.clip_id      = anim.clip;
                cache.applied_step = 0xFFFFFFFFu;
                // 驻留票在换装分支换发。atlas 的旧票也在这里还 —— 只随 clip
                // 换装松手,不随 clip 被驱逐松手(防交替驱逐-重载抖动)。
                cache.clip_ref  = mgr_->acquire(cache.clip_id);
                cache.atlas_ref = {};
            }

            // Re-query EVERY frame — an evicted asset resolves to null and the
            // pure system bails; no cached pointer can dangle.
            cache.clip  = nullptr;
            cache.atlas = nullptr;
            if (cache.clip_id.is_nil())
                return;

            const auto* ca = mgr_->fetchAssetAs<lux::asset::FlipbookClipAsset>(cache.clip_id);
            if (ca == nullptr || ca->data() == nullptr)
            {
                request_load_(cache.clip_id);
                return;
            }
            cache.clip = ca->data();

            // The atlas rides the clip's opaque ref — resolved here so the pure
            // system never sees an id, only pointers.
            if (lux::rdesc::isNullAssetRef(cache.clip->atlas_uuid))
                return;   // clip without an atlas: system skips (atlas == null)
            const auto atlas_id = lux::asset::assetIdFromOpaque(cache.clip->atlas_uuid);
            // atlas 是二跳引用(id 藏在 clip 数据里),票在第一次解析出 id 时换发。
            if (cache.atlas_ref.id() != atlas_id)
                cache.atlas_ref = mgr_->acquire(atlas_id);
            const auto* aa = mgr_->fetchAssetAs<lux::asset::TextureAtlasAsset>(atlas_id);
            if (aa == nullptr || aa->data() == nullptr)
            {
                request_load_(atlas_id);
                return;
            }
            cache.atlas = aa->data();
        });
    }

} // namespace lux::ecs
