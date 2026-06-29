/**
 * @file AnimationSystem.cpp
 * @brief Per-entity animation update — PURE (reads resolved cache only).
 *
 * See AnimationSystem.hpp: this system holds no AssetManager. The skeleton /
 * clip pointers are resolved earlier in the frame by SkeletalAnimationResolver
 * into each entity's AnimatorCacheComponent; here we only sample + skin.
 */

#include "lux/engine/gameplay/3d/world/systems/AnimationSystem.hpp"
#include "lux/engine/gameplay/3d/world/components/AnimatorComponent.hpp"
#include "lux/engine/gameplay/3d/world/components/AnimatorCacheComponent.hpp"
#include "lux/engine/gameplay/3d/world/components/SkeletalMeshComponent.hpp"

#include <lux/engine/animation/PoseSampling.hpp>   // samplePose / precomputeBindLocals
                                                   // (+ rdesc::Skeleton / AnimationClip)

#include <cmath>   // std::fmod (A3 loop-time wrap)

namespace lux::gameplay::d3
{
    AnimationSystem::AnimationSystem() noexcept = default;
    AnimationSystem::~AnimationSystem() = default;

    void AnimationSystem::update(lux::meta::EntityRegistry& registry, float dt)
    {
        // Iterate (Animator, SkeletalMesh) — the two components together define
        // an animated character; missing either means we have no work to do.
        auto view = registry.view<AnimatorComponent, SkeletalMeshComponent>();

        view.each([&registry, dt](entt::entity e, AnimatorComponent& anim,
                                  SkeletalMeshComponent& /*mesh*/)
        {
            // The resolution cache is populated by SkeletalAnimationResolver
            // (runs earlier this frame). No cache → the resolver isn't wired
            // (headless / tool) → nothing to sample; bail leaving the renderer
            // on the mesh's bind pose.
            auto* cache = registry.try_get<AnimatorCacheComponent>(e);
            if (!cache) return;

            // ---- 1. Advance playback cursor ----
            if (!anim.paused)
                anim.time_seconds += dt * anim.speed;

            // ---- 2. Resolved skeleton (resolver refreshes it each frame) ----
            // null = absent / still streaming in → bail (bind pose).
            const lux::rdesc::Skeleton* skel = cache->skeleton;
            if (!skel) return;

            // Decompose the (static) bind pose once per skeleton resolution so
            // samplePose doesn't re-decompose it every frame. bind_dirty is set
            // by the resolver on skeleton swap; the size guard also catches the
            // first-resolve / mismatched-cache case.
            if (cache->bind_dirty || cache->bind_trs.size() != skel->bones.size())
            {
                lux::animation::precomputeBindLocals(*skel, cache->bind_trs);
                cache->bind_dirty = false;
            }

            // ---- 3. Resolved clip (optional — nil clip id leaves this null) ----
            const lux::rdesc::AnimationClip* clip = cache->clip;

            // ---- 3b. Bound the cursor for looping clips (A3) so float precision
            //          doesn't drift over long sessions / large time values. ----
            // Loop is a per-instance PLAYBACK property (AnimatorComponent::loop),
            // not shared clip data — a one-shot action must not be forced to loop.
            if (clip && anim.loop && clip->duration > 0.0f)
            {
                anim.time_seconds = std::fmod(anim.time_seconds, clip->duration);
                if (anim.time_seconds < 0.0f) anim.time_seconds += clip->duration;
            }

            // ---- 3c. Pose-dirty skip ----
            // If the sample time is unchanged since the last sampled frame (paused,
            // speed 0, or settled) and the cached outputs are still sized to this
            // skeleton, last frame's cur_pose / skinning_matrices remain valid —
            // skip the resample + recompose. have_pose is reset by the resolver on
            // skeleton/clip re-resolve, so a swapped asset always re-samples.
            const float sample_time = clip ? anim.time_seconds : 0.0f;
            if (cache->have_pose
                && cache->last_sampled_time == sample_time
                && anim.cur_pose.bone_local.size() == skel->bones.size()
                && anim.skinning_matrices.size() == skel->bones.size())
            {
                return;  // pose unchanged → keep last frame's GPU-ready outputs
            }

            if (clip)
            {
                lux::animation::samplePose(*clip, *skel, anim.time_seconds, anim.loop,
                                           anim.cur_pose, cache->bind_trs);
            }
            else
            {
                lux::rdesc::AnimationClip empty;
                empty.duration = 0.0f;
                empty.loop     = false;
                lux::animation::samplePose(empty, *skel, 0.0f, /*loop=*/false,
                                           anim.cur_pose, cache->bind_trs);
            }

            // ---- 4. Compose skinning matrices for the GPU ----
            lux::animation::buildSkinningMatrices(anim.cur_pose, *skel, anim.skinning_matrices);
            cache->last_sampled_time = sample_time;
            cache->have_pose         = true;
        });
    }

} // namespace lux::gameplay::d3
