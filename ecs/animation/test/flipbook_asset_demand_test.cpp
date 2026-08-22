#include <lux/engine/ecs/animation/systems/FlipbookAssetResolver.hpp>

#include <lux/engine/ecs/World.hpp>
#include <lux/engine/ecs/animation/components/FlipbookAnimationComponent.hpp>
#include <lux/engine/resource/asset/AssetManager.hpp>
#include <lux/engine/resource/asset/texture/TextureAtlasAssets.hpp>

#include <cstdlib>
#include <iostream>
#include <memory>

namespace
{
    [[noreturn]] void fail(const char* message)
    {
        std::cerr << "flipbook_asset_demand_test: " << message << '\n';
        std::exit(1);
    }

    void expect(bool condition, const char* message)
    {
        if (!condition)
            fail(message);
    }

    std::unique_ptr<lux::asset::AssetInfo> info(
        lux::asset::asset_id_t id,
        lux::asset::EAssetType type)
    {
        auto result = std::make_unique<lux::asset::AssetInfo>();
        result->id = id;
        result->type = type;
        return result;
    }

    std::unique_ptr<lux::asset::FlipbookClipAsset> clipAsset(
        lux::asset::asset_id_t id,
        lux::asset::asset_id_t atlas_id)
    {
        auto clip = std::make_unique<lux::rdesc::FlipbookClip>();
        clip->atlas_uuid = lux::asset::opaqueFromAssetId(atlas_id);
        clip->frames.push_back({0u, 0.1f});
        return std::make_unique<lux::asset::FlipbookClipAsset>(
            info(id, lux::asset::EAssetType::FLIPBOOK_CLIP),
            std::move(clip)
        );
    }
}

int main()
{
    auto catalog = lux::asset::runtimeAssetCodecCatalog();
    lux::asset::AssetManager manager{catalog};
    const auto atlas_id = manager.generateUUID();
    const auto clip_id = manager.generateUUID();
    const auto replacement_id = manager.generateUUID();

    auto atlas = std::make_unique<lux::rdesc::TextureAtlas>();
    expect(manager.registerAsset(
        std::make_unique<lux::asset::TextureAtlasAsset>(
            info(atlas_id, lux::asset::EAssetType::TEXTURE_ATLAS),
            std::move(atlas))),
        "atlas registration failed");
    expect(manager.registerAsset(clipAsset(clip_id, atlas_id)),
        "clip registration failed");
    expect(manager.registerAsset(clipAsset(replacement_id, atlas_id)),
        "replacement clip registration failed");

    lux::ecs::World world;
    const auto entity = world.createEntity();
    lux::ecs::FlipbookAnimationComponent animation;
    animation.clip = clip_id;
    world.emplace<lux::ecs::FlipbookAnimationComponent>(
        entity, std::move(animation));

    lux::ecs::FlipbookAssetResolver resolver{
        manager, lux::asset_runtime::AssetClient{}};
    resolver.update({world.registry(), 0.016f});
    auto& cache = world.get<lux::ecs::FlipbookAnimCacheComponent>(entity);
    expect(cache.clip != nullptr && cache.atlas != nullptr,
        "ready clip and atlas were not resolved");
    expect(cache.clip_ref.id() == clip_id &&
            cache.atlas_ref.id() == atlas_id &&
            manager.isReferenced(clip_id) && manager.isReferenced(atlas_id),
        "resolver did not retain both dependencies");

    expect(manager.unloadData(clip_id), "clip eviction failed");
    resolver.update({world.registry(), 0.016f});
    expect(cache.clip == nullptr && cache.atlas == nullptr &&
            cache.clip_ref.id() == clip_id,
        "evicted clip was not observed without dropping demand");

    auto reloaded = manager.installLoadedAsset(
        clip_id, clipAsset(clip_id, atlas_id));
    expect(reloaded.has_value(), "clip reload installation failed");
    resolver.update({world.registry(), 0.016f});
    expect(cache.clip != nullptr && cache.atlas != nullptr,
        "reloaded clip was not observed on the next update");

    world.get<lux::ecs::FlipbookAnimationComponent>(entity).clip =
        replacement_id;
    resolver.update({world.registry(), 0.016f});
    expect(cache.clip_id == replacement_id &&
            cache.clip_ref.id() == replacement_id &&
            !manager.isReferenced(clip_id),
        "runtime clip replacement did not transfer its residency ticket");

    const auto before = resolver.runsBefore();
    expect(before.size() == 1u && lux::ecs::sameSystemType(
            before.front(),
            lux::ecs::systemType<lux::ecs::FlipbookAnimationSystem>()),
        "resolver ordering no longer precedes the pure sampler");
    return 0;
}
