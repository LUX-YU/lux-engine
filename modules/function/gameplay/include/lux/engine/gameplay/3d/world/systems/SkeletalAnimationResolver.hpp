#pragma once
/**
 * @file SkeletalAnimationResolver.hpp
 * @brief App-level system that resolves skeletal-animation asset references for
 *        the pure AnimationSystem.
 *
 * AnimationSystem was redesigned to be a PURE World built-in (no AssetManager,
 * no per-frame asset lookups, no disk IO). The asset-facing half lives here:
 * each frame this resolver walks every (AnimatorComponent + SkeletalMeshComponent)
 * entity, re-queries its skeleton + clip from the AssetManager, writes the
 * resolved `const` data pointers into the entity's AnimatorCacheComponent, and
 * fires the injected async-load hook for anything still absent.
 *
 * It is wired EXPLICITLY at the app level (like RenderableSystem), NOT a World
 * built-in — that is what keeps World free of any AssetManager dependency. Drive
 * it once per frame BEFORE World::tick() so AnimationSystem reads fresh pointers:
 *
 *     resolver.update(world.registry(), dt);   // resolve cache
 *     world.tick(dt);                           // Transform/Camera/Animation
 *
 * Re-querying every frame (rather than caching a pointer) means an evicted asset
 * resolves to null next frame and the system gracefully bails — the same safety
 * AssetHandle's onWillUnload used to provide, without any lifecycle subscription.
 */

#include <lux/engine/gameplay/world/systems/ISystem.hpp>
#include <lux/engine/gameplay/world/systems/AssetLoadFn.hpp>
#include <lux/engine/function/visibility.h>

namespace lux::asset { class AssetManager; }

namespace lux::gameplay::d3
{
    using lux::gameplay::AssetLoadFn;   // async-load hook — stays in the gameplay core

    class LUX_FUNCTION_PUBLIC SkeletalAnimationResolver final : public lux::gameplay::ISystem
    {
    public:
        /// @param mgr          asset source (non-owning; must outlive this).
        /// @param request_load async-load hook for absent skeleton/clip data
        ///        (app wires EngineExecutor::requestLoad). Null installs a
        ///        synchronous mgr.ensureAsset fallback so un-wired processes work.
        explicit SkeletalAnimationResolver(lux::asset::AssetManager& mgr,
                                           AssetLoadFn               request_load = {}) noexcept;
        ~SkeletalAnimationResolver() override;

        SkeletalAnimationResolver(const SkeletalAnimationResolver&)            = delete;
        SkeletalAnimationResolver& operator=(const SkeletalAnimationResolver&) = delete;

        void update(lux::meta::EntityRegistry& registry, float dt) override;

    private:
        lux::asset::AssetManager* mgr_;
        AssetLoadFn               request_load_;
    };

} // namespace lux::gameplay::d3
