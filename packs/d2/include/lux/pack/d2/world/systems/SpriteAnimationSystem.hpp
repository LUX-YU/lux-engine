#pragma once
// ============================================================================
//  SpriteAnimationSystem.hpp — pure per-frame sampler for 2D frame animation
//  (A2-01). The d2 sibling of d3::AnimationSystem.
//
//  Reads the clip + atlas ALREADY RESOLVED into SpriteAnimCacheComponent (by
//  SpriteAnim2DResolver, app level), advances `time` by dt*speed (frame dt —
//  frame animation is presentation, not simulation, so it does NOT ride the
//  fixed step), finds the current clip step, and on a step change copies that
//  frame's uv/pivot into the SpriteComponent (applyAtlasFrame) + fires the
//  entered steps' events into `events_this_frame`.
//
//  PURE: no AssetManager, no IO. Unresolved cache (clip/atlas still loading or
//  evicted) → the entity is skipped this frame, the sprite keeps its last uv.
//
//  Installed by d2::install() BEFORE Simulation2DSystem/Transform2D (slot 1 of
//  the canonical order) when the plan enables SpriteAnimation.
// ============================================================================

#include <lux/engine/ecs/systems/ISystem.hpp>
#include <lux/engine/function/visibility.h>

namespace lux::pack
{
    class LUX_FUNCTION_PUBLIC SpriteAnimationSystem final : public lux::ecs::ISystem
    {
    public:
        SpriteAnimationSystem() noexcept;
        ~SpriteAnimationSystem() override;

        SpriteAnimationSystem(const SpriteAnimationSystem&)            = delete;
        SpriteAnimationSystem& operator=(const SpriteAnimationSystem&) = delete;

        void update(lux::meta::EntityRegistry& registry, float dt) override;
    };

} // namespace lux::pack
