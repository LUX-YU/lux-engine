#pragma once
/**
 * @file PreWorldTickSystem.hpp
 * @brief Generic adapter: drive any gameplay `ISystem` in the onPreWorldTick phase.
 *
 * Several editor scene subsystems are plain gameplay `ISystem`s that must run
 * BEFORE World::tick (e.g. SkeletalAnimationResolver resolves asset pointers the
 * pure AnimationSystem built-in then reads). Rather than hand-write a near-empty
 * EditorSceneSystem subclass per such system, this template turns the wrapping
 * into a single declaration: it owns the wrapped system by value (constructed
 * in-place via a forwarding constructor — no copy/move of the non-copyable
 * system) and forwards onPreWorldTick → `update(registry, dt)`.
 *
 *     registerSceneSystem(
 *         std::make_unique<PreWorldTickSystem<SkeletalAnimationResolver>>(
 *             *assets_, request_load_));
 *
 * The wrapped type only needs a `void update(EntityRegistry&, float)` method (the
 * ISystem contract); it does NOT have to derive from ISystem. Use system() if the
 * host ever needs to reach the wrapped instance.
 */

#include <lux/engine/editor/scene/EditorSceneSystem.hpp>
#include <lux/engine/gameplay/world/World.hpp>   // ctx.world.registry()

#include <utility>

namespace lux::editor
{
    template <typename SystemT>
    class PreWorldTickSystem final : public EditorSceneSystem
    {
    public:
        /// Constructs the wrapped system in place — forwards to SystemT's ctor, so
        /// a non-copyable/non-movable ISystem (e.g. SkeletalAnimationResolver) is
        /// fine: it is never copied or moved, only emplaced.
        template <typename... Args>
        explicit PreWorldTickSystem(Args&&... args)
            : system_(std::forward<Args>(args)...)
        {
        }

        void onPreWorldTick(const SceneTickContext& ctx) override
        {
            system_.update(ctx.world.registry(), ctx.dt);
        }

        [[nodiscard]] SystemT&       system()       noexcept { return system_; }
        [[nodiscard]] const SystemT& system() const noexcept { return system_; }

    private:
        SystemT system_;
    };

} // namespace lux::editor
