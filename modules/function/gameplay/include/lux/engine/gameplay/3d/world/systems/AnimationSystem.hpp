#pragma once
/**
 * @file AnimationSystem.hpp
 * @brief Pure per-frame sampler for (AnimatorComponent + SkeletalMeshComponent)
 *        entities.
 *
 * Reads the skeleton + clip ALREADY RESOLVED into each entity's
 * AnimatorCacheComponent (by SkeletalAnimationResolver, which runs earlier in
 * the frame and owns all AssetManager contact), advances the playback cursor by
 * dt*speed, samples a new pose, and writes the skinning matrices back into
 * AnimatorComponent so the renderer can upload them next frame.
 *
 * This system is PURE: it holds no AssetManager, performs no asset lookups, and
 * never touches disk. If an entity's cache has no resolved skeleton yet (absent
 * or still streaming in), the system simply skips it — the renderer falls back
 * to the mesh's bind pose. This is why it is a plain World built-in (default
 * ctor) while the resolver is wired explicitly at the app level.
 *
 * Tick order (the resolver must precede World::tick):
 *   …app… SkeletalAnimationResolver   ← resolves cache pointers
 *   World 1. TransformSystem
 *   World 2. CameraSystem
 *   World 3. AnimationSystem          ← here (reads the resolved cache)
 */

#include <lux/engine/gameplay/world/systems/ISystem.hpp>
#include <lux/engine/function/visibility.h>

namespace lux::gameplay::d3
{
    class LUX_FUNCTION_PUBLIC AnimationSystem final : public lux::gameplay::ISystem
    {
    public:
        AnimationSystem() noexcept;
        ~AnimationSystem() override;

        AnimationSystem(const AnimationSystem&)            = delete;
        AnimationSystem& operator=(const AnimationSystem&) = delete;

        void update(lux::meta::EntityRegistry& registry, float dt) override;
    };

} // namespace lux::gameplay::d3
