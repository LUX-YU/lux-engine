#pragma once
/**
 * @file AnimatorCacheComponent.hpp
 * @brief Runtime-only resolution cache shared by SkeletalAnimationResolver
 *        (writer) and AnimationSystem (reader).
 *
 * Deliberately NOT a reflected component (no LUX_CLASS / not in the module's
 * meta TARGET_FILES): it holds engine-internal resolved pointers that must
 * never be serialized or exposed to Lua.
 *
 * Responsibility split (async-asset redesign):
 *   - SkeletalAnimationResolver (app-level system, owns AssetManager + the
 *     async-load hook) runs FIRST each frame. It re-queries the skeleton/clip
 *     assets by id, writes the resolved `const` data pointers below, and fires
 *     the async loader for anything still absent. It owns ALL AssetManager
 *     contact.
 *   - AnimationSystem (a pure World built-in) runs AFTER and only READS these
 *     pointers — no AssetManager, no per-frame disk IO. A null `skeleton`
 *     simply means "not resolved yet" and the system bails for that entity
 *     (renderer falls back to the mesh's bind pose).
 *
 * The pointers are REFRESHED every frame by the resolver, so they never dangle
 * across an eviction: an evicted asset re-queries to null next frame and the
 * system bails — exactly what AssetHandle's onWillUnload used to guarantee, now
 * achieved by the resolver's per-frame re-query (no lifecycle subscription).
 */

#include <vector>

#include <lux/engine/asset/Asset.hpp>               // asset_id_t
#include <lux/engine/animation/PoseSampling.hpp>    // BindLocalTRS

namespace lux::rdesc { struct Skeleton; struct AnimationClip; }

namespace lux::gameplay::d3
{
    struct AnimatorCacheComponent
    {
        // ── Resolved each frame by SkeletalAnimationResolver; READ-ONLY to
        //    AnimationSystem. nullptr = not resolved (absent / still loading). ──
        const lux::rdesc::Skeleton*      skeleton = nullptr;
        const lux::rdesc::AnimationClip* clip     = nullptr;

        // The ids the resolved pointers correspond to. The resolver compares the
        // component's current skeleton/clip ids against these to detect a runtime
        // swap (re-resolve + force a resample). nil clip_id == "no clip".
        lux::asset::asset_id_t skeleton_id{};
        lux::asset::asset_id_t clip_id{};

        // Pose-dirty skip: when the sample time is unchanged since the last
        // sampled frame (paused / speed 0 / settled), AnimationSystem keeps
        // last frame's cur_pose + skinning_matrices and skips resampling.
        // Reset by the resolver on skeleton / clip swap so a swapped asset
        // always re-samples.
        float last_sampled_time = 0.0f;
        bool  have_pose         = false;

        // Precomputed bind-local T/R/S for the resolved skeleton. The bind pose
        // is static, so AnimationSystem decomposes it once here (rebuilt on
        // skeleton swap via bind_dirty, set by the resolver) instead of
        // re-decomposing inside samplePose every frame.
        std::vector<lux::animation::BindLocalTRS> bind_trs;
        bool                                      bind_dirty = true;
    };

} // namespace lux::gameplay::d3
