/**
 * @file SpriteAnim2DResolver.cpp
 * @brief Resolves sprite-anim clip + atlas asset refs into
 *        SpriteAnimCacheComponent for the pure SpriteAnimationSystem (A2-01).
 *        Owns all AssetManager contact for 2D frame animation.
 */

#include <lux/pack/d2/world/systems/SpriteAnim2DResolver.hpp>
#include <lux/pack/d2/world/components/SpriteAnimationComponent.hpp>

#include <lux/engine/asset/AssetManager.hpp>
#include <lux/engine/asset/Sprite2DAssets.hpp>

#include <utility>
#include <lux/engine/ecs/systems/AssetLoadFn.hpp>

namespace lux::pack
{
    using lux::ecs::AssetLoadFn;

    SpriteAnim2DResolver::SpriteAnim2DResolver(
        lux::asset::AssetManager& mgr, AssetLoadFn request_load) noexcept
        : mgr_(&mgr)
        , request_load_(std::move(request_load))
    {
        if (!request_load_)
            request_load_ = [this](const lux::asset::asset_id_t& id)
            {
                (void)mgr_->ensureAsset(id);
            };
    }

    SpriteAnim2DResolver::~SpriteAnim2DResolver() = default;

    void SpriteAnim2DResolver::update(lux::meta::EntityRegistry& registry, float /*dt*/)
    {
        auto view = registry.view<SpriteAnimationComponent>();
        view.each([this, &registry](entt::entity e, SpriteAnimationComponent& anim)
        {
            // Runtime-only cache, lazily attached (non-reflected pool — safe to
            // emplace mid-iteration; it is not part of this view).
            auto& cache = registry.get_or_emplace<SpriteAnimCacheComponent>(e);

            // Clip swap → restart the applied-step cursor so the first frame of
            // the new clip is applied (and its events fire) on next sample.
            if (cache.clip_id != anim.clip)
            {
                cache.clip_id      = anim.clip;
                cache.applied_step = 0xFFFFFFFFu;
            }

            // Re-query EVERY frame — an evicted asset resolves to null and the
            // pure system bails; no cached pointer can dangle.
            cache.clip  = nullptr;
            cache.atlas = nullptr;
            if (cache.clip_id.is_nil())
                return;

            const auto* ca = mgr_->fetchAssetAs<lux::asset::SpriteAnimClipAsset>(cache.clip_id);
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
            const auto* aa = mgr_->fetchAssetAs<lux::asset::SpriteAtlasAsset>(atlas_id);
            if (aa == nullptr || aa->data() == nullptr)
            {
                request_load_(atlas_id);
                return;
            }
            cache.atlas = aa->data();
        });
    }

} // namespace lux::pack
