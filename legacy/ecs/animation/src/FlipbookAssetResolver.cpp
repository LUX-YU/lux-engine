#include <lux/engine/ecs/animation/systems/FlipbookAssetResolver.hpp>

#include <lux/engine/ecs/animation/components/FlipbookAnimationComponent.hpp>
#include <lux/engine/resource/asset/AssetManager.hpp>
#include <lux/engine/resource/asset/texture/TextureAtlasAssets.hpp>

#include <utility>

namespace lux::ecs
{
    FlipbookAssetResolver::FlipbookAssetResolver(
        lux::asset::AssetManager& manager,
        lux::asset_runtime::AssetClient client) noexcept
        : manager_(&manager)
        , client_(std::move(client))
    {}

    void FlipbookAssetResolver::update(
        const lux::ecs::SystemUpdateContext& context)
    {
        auto& registry = context.registry();
        auto view = registry.view<FlipbookAnimationComponent>();
        view.each([this, &registry](
            entt::entity entity,
            FlipbookAnimationComponent& animation)
        {
            auto& cache = registry.get_or_emplace<
                FlipbookAnimCacheComponent>(entity);
            if (cache.clip_id != animation.clip)
            {
                cache.clip_id = animation.clip;
                cache.applied_step = 0xFFFFFFFFu;
                cache.clip_ref = manager_->acquire(cache.clip_id);
                cache.atlas_ref = {};
            }

            cache.clip = nullptr;
            cache.atlas = nullptr;
            if (cache.clip_id.is_nil())
                return;

            const auto* clip = manager_->fetchAssetAs<
                lux::asset::FlipbookClipAsset>(cache.clip_id);
            if (clip == nullptr || clip->data() == nullptr)
            {
                static_cast<void>(client_.request(cache.clip_id));
                return;
            }
            cache.clip = clip->data();

            if (lux::rdesc::isNullAssetRef(cache.clip->atlas_uuid))
                return;
            const auto atlas_id = lux::asset::assetIdFromOpaque(
                cache.clip->atlas_uuid
            );
            if (cache.atlas_ref.id() != atlas_id)
                cache.atlas_ref = manager_->acquire(atlas_id);
            const auto* atlas = manager_->fetchAssetAs<
                lux::asset::TextureAtlasAsset>(atlas_id);
            if (atlas == nullptr || atlas->data() == nullptr)
            {
                static_cast<void>(client_.request(atlas_id));
                return;
            }
            cache.atlas = atlas->data();
        });
    }
}
