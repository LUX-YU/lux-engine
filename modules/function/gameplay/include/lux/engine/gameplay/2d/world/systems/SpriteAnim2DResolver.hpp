#pragma once
// ============================================================================
//  SpriteAnim2DResolver.hpp — app-level asset resolution for 2D frame
//  animation (A2-01). The d2 sibling of d3::SkeletalAnimationResolver.
//
//  Each frame it walks every SpriteAnimationComponent entity, re-queries the
//  clip asset (and, through the clip's atlas_uuid, the atlas asset) from the
//  AssetManager, writes the resolved const pointers into the entity's
//  SpriteAnimCacheComponent, and fires the injected async-load hook for
//  anything absent. Re-querying every frame means an evicted asset resolves to
//  null next frame and the pure SpriteAnimationSystem bails gracefully.
//
//  Wired EXPLICITLY at the app level (keeps World free of AssetManager), BEFORE
//  World::tick():
//
//      sprite_resolver.update(world.registry(), dt);
//      world.tick(dt);      // ... SpriteAnimationSystem reads fresh pointers
// ============================================================================

#include <lux/engine/gameplay/world/systems/ISystem.hpp>
#include <lux/engine/gameplay/world/systems/AssetLoadFn.hpp>
#include <lux/engine/function/visibility.h>

namespace lux::asset { class AssetManager; }

namespace lux::gameplay::d2
{
    using lux::gameplay::AssetLoadFn;

    class LUX_FUNCTION_PUBLIC SpriteAnim2DResolver final : public lux::gameplay::ISystem
    {
    public:
        /// @param mgr          asset source (non-owning; must outlive this).
        /// @param request_load async-load hook for absent clip/atlas assets;
        ///        null installs a synchronous ensureAsset fallback.
        explicit SpriteAnim2DResolver(lux::asset::AssetManager& mgr,
                                      AssetLoadFn               request_load = {}) noexcept;
        ~SpriteAnim2DResolver() override;

        SpriteAnim2DResolver(const SpriteAnim2DResolver&)            = delete;
        SpriteAnim2DResolver& operator=(const SpriteAnim2DResolver&) = delete;

        void update(lux::meta::EntityRegistry& registry, float dt) override;

    private:
        lux::asset::AssetManager* mgr_;
        AssetLoadFn               request_load_;
    };

} // namespace lux::gameplay::d2
