#include <lux/engine/ecs/animation/systems/SkeletalAnimationAssetResolver.hpp>

#include <lux/engine/ecs/World.hpp>
#include <lux/engine/ecs/render/components/3d/AnimatorCacheComponent.hpp>
#include <lux/engine/ecs/render/components/3d/AnimatorComponent.hpp>
#include <lux/engine/ecs/render/components/3d/SkeletalMeshComponent.hpp>
#include <lux/engine/resource/asset/AssetManager.hpp>
#include <lux/engine/resource/asset/animation/AnimationClipAsset.hpp>
#include <lux/engine/resource/asset/animation/SkeletonAsset.hpp>

#include <cstdlib>
#include <iostream>
#include <memory>

namespace
{
    [[noreturn]] void fail(const char* message)
    {
        std::cerr << "skeletal_asset_demand_test: " << message << '\n';
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

    std::unique_ptr<lux::asset::SkeletonAsset> skeletonAsset(
        lux::asset::asset_id_t id)
    {
        auto skeleton = std::make_unique<lux::rdesc::Skeleton>();
        lux::rdesc::Bone_t root{};
        root.name = "root";
        root.parent_index = -1;
        root.bind_local = Eigen::Affine3f::Identity();
        root.inv_bind_world = Eigen::Affine3f::Identity();
        skeleton->bones.push_back(std::move(root));
        return std::make_unique<lux::asset::SkeletonAsset>(
            info(id, lux::asset::EAssetType::SKELETON),
            std::move(skeleton)
        );
    }

    std::unique_ptr<lux::asset::AnimationClipAsset> clipAsset(
        lux::asset::asset_id_t id)
    {
        auto clip = std::make_unique<lux::rdesc::AnimationClip>();
        clip->name = "idle";
        clip->duration = 1.0f;
        return std::make_unique<lux::asset::AnimationClipAsset>(
            info(id, lux::asset::EAssetType::ANIMATION_CLIP),
            std::move(clip)
        );
    }
}

int main()
{
    lux::asset::AssetManager manager{
        lux::asset::runtimeAssetCodecCatalog()};
    const auto skeleton_id = manager.generateUUID();
    const auto clip_id = manager.generateUUID();
    const auto replacement_id = manager.generateUUID();
    expect(manager.registerAsset(skeletonAsset(skeleton_id)),
        "skeleton registration failed");
    expect(manager.registerAsset(clipAsset(clip_id)),
        "clip registration failed");
    expect(manager.registerAsset(clipAsset(replacement_id)),
        "replacement clip registration failed");

    lux::ecs::World world;
    const auto entity = world.createEntity();
    lux::ecs::AnimatorComponent animator;
    animator.clip_asset_id = clip_id;
    lux::ecs::SkeletalMeshComponent mesh;
    mesh.skeleton_asset_id = skeleton_id;
    world.emplace<lux::ecs::AnimatorComponent>(entity, std::move(animator));
    world.emplace<lux::ecs::SkeletalMeshComponent>(entity, std::move(mesh));

    lux::ecs::SkeletalAnimationAssetResolver resolver{
        manager, lux::asset_runtime::AssetClient{}};
    resolver.update({world.registry(), 0.016f});
    auto& cache = world.get<lux::ecs::AnimatorCacheComponent>(entity);
    expect(cache.skeleton != nullptr && cache.clip != nullptr,
        "ready skeleton and clip were not resolved");
    expect(cache.skeleton_ref.id() == skeleton_id &&
            cache.clip_ref.id() == clip_id &&
            manager.isReferenced(skeleton_id) && manager.isReferenced(clip_id),
        "resolver did not retain both animation dependencies");

    expect(manager.unloadData(clip_id), "clip eviction failed");
    resolver.update({world.registry(), 0.016f});
    expect(cache.clip == nullptr && cache.clip_ref.id() == clip_id,
        "evicted clip was not observed without dropping demand");
    auto reloaded = manager.installLoadedAsset(clip_id, clipAsset(clip_id));
    expect(reloaded.has_value(), "clip reload installation failed");
    resolver.update({world.registry(), 0.016f});
    expect(cache.clip != nullptr,
        "reloaded animation clip was not observed on the next update");

    world.get<lux::ecs::AnimatorComponent>(entity).clip_asset_id =
        replacement_id;
    resolver.update({world.registry(), 0.016f});
    expect(cache.clip_id == replacement_id &&
            cache.clip_ref.id() == replacement_id &&
            !manager.isReferenced(clip_id),
        "clip replacement did not transfer its residency ticket");

    const auto before = resolver.runsBefore();
    expect(before.size() == 1u && lux::ecs::sameSystemType(
            before.front(),
            lux::ecs::systemType<lux::ecs::AnimationSystem>()),
        "resolver ordering no longer precedes the pure sampler");
    return 0;
}
