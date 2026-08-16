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
 * It is wired EXPLICITLY at the app level (like RenderSystem), NOT a World
 * built-in — that is what keeps World free of any AssetManager dependency. Drive
 * it before AnimationSystem in Schedule so animation reads fresh pointers:
 *
 *     resolver.update(world.registry(), dt);   // resolve cache
 *     schedule.tick(dt);                        // Transform/Camera/Animation
 *
 * Re-querying every frame (rather than caching a pointer) means an evicted asset
 * resolves to null next frame and the system gracefully bails — the same safety
 * AssetHandle's onWillUnload used to provide, without any lifecycle subscription.
 */

#include <lux/engine/ecs/systems/ISystem.hpp>
#include <lux/engine/ecs/systems/AssetLoadFn.hpp>
#include <lux/engine/ecs/animation/systems/AnimationSystem.hpp>
#include <lux/engine/function/visibility.h>

namespace lux::asset { class AssetManager; }

namespace lux::ecs
{
    class LUX_FUNCTION_PUBLIC SkeletalAnimationResolver final : public lux::ecs::ISystem
    {
    public:
        /// @param mgr          asset source (non-owning; must outlive this).
        /// @param request_load async-load hook for absent skeleton/clip data
        ///        (app wires AssetClient::request). Null installs a
        ///        synchronous mgr.ensureAsset fallback so un-wired processes work.
        explicit SkeletalAnimationResolver(lux::asset::AssetManager& mgr,
                                           AssetLoadFn               request_load = {}) noexcept;
        ~SkeletalAnimationResolver() override;

        SkeletalAnimationResolver(const SkeletalAnimationResolver&)            = delete;
        SkeletalAnimationResolver& operator=(const SkeletalAnimationResolver&) = delete;

        void update(const lux::ecs::SystemUpdateContext& ctx) override;

        /// 必须先于 `AnimationSystem` —— 它读本解析器写进 `AnimatorCacheComponent`
        /// 的 skeleton/clip 指针。此前这条只由「相位 PreTransform < Simulation」隐含
        /// 保证:两者恰好在不同相位,而那是巧合,不是声明。
        [[nodiscard]] std::span<const SystemType> runsBefore() const noexcept override
        {
            static constexpr SystemType kBefore[] = {
                systemType<AnimationSystem>()};
            return kBefore;
        }

    private:
        lux::asset::AssetManager* mgr_;
        AssetLoadFn               request_load_;
    };

} // namespace lux::ecs
