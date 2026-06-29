#pragma once
#include <Eigen/Geometry>
#include <string>
#include <vector>
#include <cstdint>

namespace lux::rdesc
{
    /**
     * @brief Keyframe track for one bone in an AnimationClip.
     *
     * Translation, rotation and scale each have their own time arrays
     * (translations[i] takes effect at times_t[i] seconds). The three
     * channels are stored independently because DCC tools (Assimp, FBX
     * SDK, glTF) routinely export them at different sample rates —
     * rotation keys are usually denser than translation, and scale is
     * often a single keyframe per bone. Forcing a unified time array
     * would create redundant keys.
     *
     * Times are in seconds and are monotonically increasing. The first
     * key may be at t > 0 (sampler clamps t <= first key to that key's
     * value); the last key may be before clip duration (sampler clamps
     * t >= last key to that key's value).
     *
     * Rotations are stored as unit quaternions, sampler uses slerp.
     * Translations and scales are stored as Vector3, sampler uses lerp.
     */
    struct BoneTrack
    {
        int32_t                          bone_index;    ///< Index into target Skeleton::bones
        std::vector<float>               times_t;       ///< Translation key times (seconds)
        std::vector<Eigen::Vector3f>     translations;  ///< Translation key values
        std::vector<float>               times_r;       ///< Rotation key times (seconds)
        std::vector<Eigen::Quaternionf>  rotations;     ///< Rotation key values (unit quaternions)
        std::vector<float>               times_s;       ///< Scale key times (seconds)
        std::vector<Eigen::Vector3f>     scales;        ///< Scale key values
    };

    /**
     * @brief A complete animation clip targeting a Skeleton.
     *
     * Duration is in seconds (Assimp's ticks_per_sec divisor is already
     * applied at import time). When `loop = true`, the AnimationSystem
     * wraps `time` with `fmod(time, duration)`; otherwise it clamps.
     *
     * `tracks` only contains entries for bones the clip actually animates;
     * bones absent from `tracks` fall back to `Skeleton::bones[i].bind_local`
     * (rest pose) at runtime.
     */
    struct AnimationClip
    {
        std::string             name;      ///< Clip name (e.g. "walk", "jump_loop")
        float                   duration;  ///< Clip length in seconds
        bool                    loop;      ///< Loop vs. clamp at end
        std::vector<BoneTrack>  tracks;    ///< Per-animated-bone keyframe tracks
    };

    /**
     * @brief Runtime pose — per-bone local-space transform for one frame.
     *
     * Lives in the description layer (rather than function::animation) so
     * that the FlowForge AnimGraph data pin `Pose` can carry it through
     * the graph without dragging a runtime dependency into the data layer.
     * It is NOT serialized as part of any asset — it is the in-memory
     * output of PoseSampling / AnimGraphRuntime.
     *
     * `bone_local.size()` is expected to match the bound Skeleton's bone
     * count. `bone_local[i]` is the local-space transform of bone i for
     * this frame (rest = `Skeleton::bones[i].bind_local`, animated =
     * sampled from a clip's BoneTrack).
     */
    struct Pose
    {
        std::vector<Eigen::Affine3f> bone_local; ///< Per-bone local transform
    };
} // namespace lux::rdesc
