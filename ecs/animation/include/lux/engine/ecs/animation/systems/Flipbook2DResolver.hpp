#pragma once
// ============================================================================
//  Flipbook2DResolver.hpp — app-level asset resolution for 2D frame
//  animation (A2-01). The d2 sibling of d3::SkeletalAnimationResolver.
//
//  Each frame it walks every FlipbookAnimationComponent entity, re-queries the
//  clip asset (and, through the clip's atlas_uuid, the atlas asset) from the
//  AssetManager, writes the resolved const pointers into the entity's
//  FlipbookAnimCacheComponent, and fires the injected async-load hook for
//  anything absent. Re-querying every frame means an evicted asset resolves to
//  null next frame and the pure FlipbookAnimationSystem bails gracefully.
//
//  Wired EXPLICITLY at the app level (keeps World free of AssetManager), BEFORE
//  Schedule::tick():
//
//      flipbook_resolver.update(world.registry(), dt);
//      schedule.tick(dt);   // ... FlipbookAnimationSystem reads fresh pointers
// ============================================================================

#include <lux/engine/ecs/systems/ISystem.hpp>
#include <lux/engine/ecs/systems/AssetLoadFn.hpp>
#include <lux/engine/ecs/animation/systems/FlipbookAnimationSystem.hpp>
#include <lux/engine/function/visibility.h>

namespace lux::asset { class AssetManager; }

namespace lux::ecs
{
    class LUX_FUNCTION_PUBLIC Flipbook2DResolver final : public lux::ecs::ISystem
    {
    public:
        /// @param mgr          asset source (non-owning; must outlive this).
        /// @param request_load async-load hook for absent clip/atlas assets;
        ///        null installs a synchronous ensureAsset fallback.
        explicit Flipbook2DResolver(lux::asset::AssetManager& mgr,
                                      AssetLoadFn               request_load = {}) noexcept;
        ~Flipbook2DResolver() override;

        Flipbook2DResolver(const Flipbook2DResolver&)            = delete;
        Flipbook2DResolver& operator=(const Flipbook2DResolver&) = delete;

        void update(const lux::ecs::SystemUpdateContext& ctx) override;

        [[nodiscard]] std::span<const SystemType> runsBefore()
            const noexcept override
        {
            static constexpr SystemType kBefore[] = {
                systemType<FlipbookAnimationSystem>()};
            return kBefore;
        }

    private:
        lux::asset::AssetManager* mgr_;
        AssetLoadFn               request_load_;
    };

} // namespace lux::ecs
