#pragma once
/**
 * @file AnimatorComponent.hpp
 * @brief Single-clip animation playback state + per-frame outputs.
 *
 * Owns the playback cursor (time / speed / paused) plus the two outputs that
 * AnimationSystem writes each frame:
 *
 *   * cur_pose          — bone-local Affine3f per bone (size == skeleton.bones)
 *   * skinning_matrices — world-space mat4 per bone, ready to upload to GPU
 *                          bone-palette SSBO.
 *
 * Lives in gameplay so the editor's reflection-driven Inspector (when added)
 * can scrub the cursor. The actual sampler functions are in
 * `lux::animation` (function::animation component) — AnimationSystem links
 * against it; the component itself does not (avoids dragging an animation
 * dep into every gameplay user).
 *
 * Phase 3 will replace `clip_asset_id` with an AnimGraph reference, but the
 * Pose / skinning_matrices output shape stays the same.
 */

#include <vector>

#include <Eigen/Geometry>

#include <lux/engine/asset/Asset.hpp>
#include <lux/engine/description/AnimationClip.hpp>   // lux::rdesc::Pose
#include <lux/engine/meta/MetaAnnotations.hpp>
#include <lux/engine/ecs/World.hpp>

namespace lux::pack
{
    using lux::ecs::World;

    struct LUX_COMPONENT() AnimatorComponent
    {
        // ---- input (driven by editor / gameplay code) ----

        /// UUID of the AnimationClipAsset to play. Zero = no-op.
        lux::asset::asset_id_t clip_asset_id;

        LUX_MEMBER(display_name=Time, tooltip=Current playback time in seconds)
        float time_seconds = 0.0f;

        LUX_MEMBER(display_name=Speed, tooltip=Playback speed multiplier; negative plays backwards)
        float speed = 1.0f;

        LUX_MEMBER(display_name=Paused)
        bool paused = false;

        /// Per-instance playback loop: wrap time at clip end (true) or clamp /
        /// hold the last frame for one-shot actions (false). NOT read from
        /// clip data — looping is a playback decision, not a clip property.
        LUX_MEMBER(display_name=Loop, tooltip=Wrap time at clip end; one-shot when false)
        bool loop = true;

        // ---- output (overwritten by AnimationSystem each tick) ----

        /// Bone-local pose sampled from clip_asset at time_seconds.
        /// Sized to associated skeleton.bones.size() by AnimationSystem.
        lux::rdesc::Pose cur_pose;

        /// World-space skinning matrices. Sized to skeleton.bones.size().
        /// SkinningFeature uploads this into the bone-palette SSBO.
        std::vector<Eigen::Matrix4f> skinning_matrices;
    };

} // namespace lux::pack
