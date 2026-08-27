#pragma once
/**
 * @file PoseSampling.hpp
 * @brief Animation pose sampling and skinning matrix composition.
 *
 * Stateless free functions. Inputs are immutable description-tier POD
 * (`lux::rdesc::AnimationClip` / `lux::rdesc::Skeleton`); outputs are written
 * into caller-owned `lux::rdesc::Pose` / matrix arrays so the caller controls
 * allocation. AnimationSystem stores those outputs in AnimatorComponent (which
 * persists across frames) and buildSkinningMatrices reuses thread_local scratch,
 * so steady-state update does not heap-allocate.
 */

#include <span>
#include <vector>

#include <Eigen/Geometry>

#include <lux/engine/description/AnimationClip.hpp>
#include <lux/engine/description/Skeleton.hpp>
#include <lux/engine/function/visibility.h>

namespace lux::animation
{
    /// Per-bone bind-local transform decomposed into T/R/S. samplePose needs
    /// these so a track missing one channel (e.g. no scale keys) can fall
    /// back to the bind value per channel rather than all-or-nothing. The
    /// decomposition is of static skeleton data, so callers should precompute
    /// it ONCE per skeleton (precomputeBindLocals) and hand it to samplePose
    /// instead of paying the decompose cost every frame.
    struct BindLocalTRS
    {
        Eigen::Vector3f translation{Eigen::Vector3f::Zero()};
        Eigen::Quaternionf rotation{Eigen::Quaternionf::Identity()};
        Eigen::Vector3f scale{Eigen::Vector3f::Ones()};
    };

    /// Decompose every bone's `bind_local` into T/R/S once. `out` is resized to
    /// `skeleton.bones.size()`. Call when the skeleton is (re)resolved, then
    /// pass the result to samplePose. Cheap to recompute, but the whole point
    /// is to NOT recompute it per frame.
    LUX_FUNCTION_PUBLIC void precomputeBindLocals(const lux::rdesc::Skeleton& skeleton, std::vector<BindLocalTRS>& out);

    /// Sample @p clip at @p time and write per-bone local transforms into
    /// @p out_pose. Bones not animated by the clip are filled with
    /// @p skeleton.bones[i].bind_local (the rest pose).
    ///
    /// `time` is wrapped into `[0, clip.duration)` when @p loop, else
    /// clamped to `[0, clip.duration]`. `out_pose.bone_local` is resized to
    /// `skeleton.bones.size()` if needed.
    ///
    /// @p loop is a PLAYBACK decision supplied by the caller (e.g.
    /// `AnimatorComponent::loop`), NOT read from `clip.loop` — looping is a
    /// per-instance property, not shared clip data.
    ///
    /// Per channel (T / R / S):
    ///   * empty track     → bind value for that channel
    ///   * single keyframe → constant
    ///   * 2+ keyframes    → linear interp (T, S) / slerp (R) between the
    ///                       two adjacent keys; binary search since Assimp
    ///                       guarantees monotonic times.
    /// @p bind_trs (optional) is the precomputed bind-local T/R/S from
    /// precomputeBindLocals. When it is sized to `skeleton.bones.size()` it is
    /// used for per-channel fallback; otherwise samplePose decomposes the bind
    /// pose on demand (correct, but recomputes static data every call). Callers
    /// in a per-frame loop should always supply it.
    LUX_FUNCTION_PUBLIC void samplePose(
        const lux::rdesc::AnimationClip& clip,
        const lux::rdesc::Skeleton& skeleton,
        float time,
        bool loop,
        lux::rdesc::Pose& out_pose,
        std::span<const BindLocalTRS> bind_trs = {}
    );

    /// Compose world-space skinning matrices from a local-space pose.
    ///
    /// Precondition: for every bone `i`, `parent_index[i] < i` (topological
    /// order). Phase 1's `SkeletonAsset` import path enforces this; the
    /// function asserts on debug builds and silently produces wrong results
    /// on release if the precondition is violated.
    ///
    ///   world_local[i] = (parent >= 0 ? world_local[parent] : I) * pose[i]
    ///   out_matrices[i] = world_local[i] * skeleton.bones[i].inv_bind_world
    ///
    /// `out_matrices` is resized to `skeleton.bones.size()` if needed.
    LUX_FUNCTION_PUBLIC void buildSkinningMatrices(
        const lux::rdesc::Pose& pose,
        const lux::rdesc::Skeleton& skeleton,
        std::vector<Eigen::Matrix4f>& out_matrices
    );
}
