#pragma once
// ============================================================================
//  FlipbookAnimationSystem.hpp — pure per-frame sampler for 2D frame animation
//  (A2-01). The d2 sibling of d3::AnimationSystem.
//
//  Reads the clip + atlas ALREADY RESOLVED into FlipbookAnimCacheComponent (by
//  Flipbook2DResolver, app level), advances `time` by dt*speed (frame dt —
//  frame animation is presentation, not simulation, so it does NOT ride the
//  fixed step), finds the current clip step, and on a step change copies that
//  frame's uv/pivot into the Image2DComponent (applyAtlasFrame) + fires the
//  entered steps' events into `events_this_frame`.
//
//  PURE: no AssetManager, no IO. Unresolved cache (clip/atlas still loading or
//  evicted) → the entity is skipped this frame, the image keeps its last uv.
//
//  Contributed by the built-in 2D scene descriptor before the fixed-step and
//  compiled scene-system plan includes flipbook animation.
// ============================================================================

#include <lux/engine/ecs/systems/ISystem.hpp>
#include <lux/engine/function/visibility.h>

namespace lux::ecs
{
    struct FlipbookAnimationComponent;
    struct FlipbookAnimCacheComponent;
    struct Image2DComponent;

    class LUX_FUNCTION_PUBLIC FlipbookAnimationSystem final : public lux::ecs::ISystem
    {
    public:
        FlipbookAnimationSystem() noexcept;
        ~FlipbookAnimationSystem() override;

        FlipbookAnimationSystem(const FlipbookAnimationSystem&)            = delete;
        FlipbookAnimationSystem& operator=(const FlipbookAnimationSystem&) = delete;

        void update(const lux::ecs::SystemUpdateContext& ctx) override;

        [[nodiscard]] AccessDeclaration accessDeclaration() const noexcept override
        {
            static constexpr ResourceAccess kAccess[] = {
                writes<FlipbookAnimationComponent>(),
                writes<FlipbookAnimCacheComponent>(),
                writes<Image2DComponent>(),
            };
            return {
                .resources = kAccess,
                .complete = true,
                .structural = false,
            };
        }
    };
} // namespace lux::ecs
